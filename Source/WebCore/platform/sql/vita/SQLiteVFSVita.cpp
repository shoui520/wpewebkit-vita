/*
 * Copyright (C) 2026 Vita port contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// Native SQLite operating-system layer for the Vita port.
//
// SQLite is built with SQLITE_OS_OTHER, so the platform supplies
// sqlite3_os_init()/sqlite3_os_end(). Files use native SceUID I/O with 64-bit
// offsets; the POSIX runtime's FIOS descriptors are never mixed in.
//
// Locking scope: the Vita runs a single application process, and databases
// live in application-owned storage. Locks therefore coordinate every
// connection and thread in this process with SQLite's full SHARED, RESERVED,
// PENDING and EXCLUSIVE protocol. They do not exclude other processes; native
// open flags were measured not to provide inter-handle exclusion. No WAL or
// memory mapping is advertised: there is no xShmMap, so SQLite keeps rollback
// journals.

#include "config.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <sqlite3.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WallTime.h>
#include <wtf/text/CString.h>

namespace WebCore {

static constexpr int errorNoEntry = static_cast<int>(0x80010002); // SCE_ERROR_ERRNO_ENOENT
static constexpr int errorInterrupted = static_cast<int>(0x80010004); // SCE_ERROR_ERRNO_EINTR
static constexpr int errorExists = static_cast<int>(0x80010011); // SCE_ERROR_ERRNO_EEXIST
static constexpr int errorNoSpace = static_cast<int>(0x8001001C); // SCE_ERROR_ERRNO_ENOSPC
static constexpr int maximumPathLength = 1024;

// Most recent native failure on this thread, reported through xGetLastError
// and sqlite3_system_errno().
static thread_local int lastNativeError;

static int recordFailure(int nativeResult)
{
    if (nativeResult < 0)
        lastNativeError = nativeResult;
    return nativeResult;
}


struct VitaFile;

// Process-wide lock state for one canonical database path.
struct VitaLockRecord {
    WTF_MAKE_TZONE_ALLOCATED(VitaLockRecord);
public:
    unsigned references { 0 };
    unsigned sharedHolders { 0 };
    VitaFile* reserved { nullptr };
    VitaFile* pending { nullptr };
    VitaFile* exclusive { nullptr };
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(VitaLockRecord);

struct VitaFile {
    sqlite3_file base;
    SceUID uid;
    int lockLevel;
    VitaLockRecord* lockRecord;
    CString* path;
    bool deleteOnClose;
};

static Lock lockTableLock;

static HashMap<CString, std::unique_ptr<VitaLockRecord>>& lockTable() WTF_REQUIRES_LOCK(lockTableLock)
{
    static NeverDestroyed<HashMap<CString, std::unique_ptr<VitaLockRecord>>> table;
    return table;
}

static VitaLockRecord* acquireLockRecord(const CString& path)
{
    Locker locker { lockTableLock };
    auto& record = lockTable().ensure(path, [] {
        return makeUnique<VitaLockRecord>();
    }).iterator->value;
    ++record->references;
    return record.get();
}

static void releaseLockRecord(const CString& path, VitaLockRecord* record)
{
    Locker locker { lockTableLock };
    ASSERT(record->references);
    if (!--record->references)
        lockTable().remove(path);
}

static int nativeOpen(const char* path, int flags, int permissions)
{
    int result;
    do {
        result = recordFailure(sceIoOpen(path, flags, permissions));
    } while (result == errorInterrupted);
    return result;
}

static std::optional<int64_t> nativeSize(SceUID uid)
{
    SceIoStat stat { };
    if (recordFailure(sceIoGetstatByFd(uid, &stat)) < 0 || stat.st_size < 0)
        return std::nullopt;
    return stat.st_size;
}

// Returns SQLITE_OK, SQLITE_FULL when the native volume is out of space, or
// the supplied I/O error code.
static int writeAll(SceUID uid, const uint8_t* data, int64_t size, int64_t offset, int errorCode)
{
    while (size > 0) {
        auto count = static_cast<SceSize>(std::min<int64_t>(size, std::numeric_limits<int32_t>::max()));
        int result;
        do {
            result = recordFailure(sceIoPwrite(uid, data, count, offset));
        } while (result == errorInterrupted);
        if (result == errorNoSpace)
            return SQLITE_FULL;
        if (result <= 0)
            return errorCode;
        data += result;
        size -= result;
        offset += result;
    }
    return SQLITE_OK;
}

// Native size changes do not establish zero-filled extensions, so gaps are
// written explicitly.
static int zeroFill(SceUID uid, int64_t from, int64_t to, int errorCode)
{
    static constexpr std::array<uint8_t, 4096> zeros { };
    while (from < to) {
        auto count = std::min<int64_t>(to - from, zeros.size());
        if (int result = writeAll(uid, zeros.data(), count, from, errorCode); result != SQLITE_OK)
            return result;
        from += count;
    }
    return SQLITE_OK;
}

static int vitaUnlock(sqlite3_file*, int);

static int vitaClose(sqlite3_file* file)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    // SQLite unlocks before closing; never leave a stale process lock behind.
    vitaUnlock(file, SQLITE_LOCK_NONE);
    int result = SQLITE_OK;
    if (recordFailure(sceIoClose(vitaFile->uid)) < 0)
        result = SQLITE_IOERR_CLOSE;
    if (vitaFile->deleteOnClose && recordFailure(sceIoRemove(vitaFile->path->data())) < 0)
        result = SQLITE_IOERR_DELETE;
    releaseLockRecord(*vitaFile->path, vitaFile->lockRecord);
    delete vitaFile->path;
    return result;
}

static int vitaRead(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    auto* bytes = static_cast<uint8_t*>(buffer);
    int done = 0;
    while (done < amount) {
        int result;
        do {
            result = recordFailure(sceIoPread(vitaFile->uid, bytes + done, amount - done, offset + done));
        } while (result == errorInterrupted);
        if (result < 0)
            return SQLITE_IOERR_READ;
        if (!result) {
            // SQLite requires the unread remainder to be zeroed.
            std::memset(bytes + done, 0, amount - done);
            return SQLITE_IOERR_SHORT_READ;
        }
        done += result;
    }
    return SQLITE_OK;
}

static int vitaWrite(sqlite3_file* file, const void* buffer, int amount, sqlite3_int64 offset)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    auto size = nativeSize(vitaFile->uid);
    if (!size)
        return SQLITE_IOERR_FSTAT;
    if (offset > *size) {
        if (int result = zeroFill(vitaFile->uid, *size, offset, SQLITE_IOERR_WRITE); result != SQLITE_OK)
            return result;
    }
    return writeAll(vitaFile->uid, static_cast<const uint8_t*>(buffer), amount, offset, SQLITE_IOERR_WRITE);
}

static int vitaTruncate(sqlite3_file* file, sqlite3_int64 newSize)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    if (newSize < 0)
        return SQLITE_IOERR_TRUNCATE;
    auto size = nativeSize(vitaFile->uid);
    if (!size)
        return SQLITE_IOERR_FSTAT;
    if (newSize > *size)
        return zeroFill(vitaFile->uid, *size, newSize, SQLITE_IOERR_TRUNCATE);
    SceIoStat stat { };
    stat.st_size = newSize;
    return recordFailure(sceIoChstatByFd(vitaFile->uid, &stat, SCE_CST_SIZE)) < 0 ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}

static int vitaSync(sqlite3_file* file, int)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    return recordFailure(sceIoSyncByFd(vitaFile->uid, 0)) < 0 ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}

static int vitaFileSize(sqlite3_file* file, sqlite3_int64* size)
{
    auto nativeFileSize = nativeSize(reinterpret_cast<VitaFile*>(file)->uid);
    if (!nativeFileSize)
        return SQLITE_IOERR_FSTAT;
    *size = *nativeFileSize;
    return SQLITE_OK;
}

static int vitaLock(sqlite3_file* file, int level)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    if (vitaFile->lockLevel >= level)
        return SQLITE_OK;

    Locker locker { lockTableLock };
    auto& record = *vitaFile->lockRecord;
    switch (level) {
    case SQLITE_LOCK_SHARED:
        ASSERT(vitaFile->lockLevel == SQLITE_LOCK_NONE);
        if (record.pending || record.exclusive)
            return SQLITE_BUSY;
        ++record.sharedHolders;
        vitaFile->lockLevel = SQLITE_LOCK_SHARED;
        return SQLITE_OK;
    case SQLITE_LOCK_RESERVED:
        ASSERT(vitaFile->lockLevel == SQLITE_LOCK_SHARED);
        if (record.reserved || record.pending || record.exclusive)
            return SQLITE_BUSY;
        record.reserved = vitaFile;
        vitaFile->lockLevel = SQLITE_LOCK_RESERVED;
        return SQLITE_OK;
    case SQLITE_LOCK_EXCLUSIVE:
        ASSERT(vitaFile->lockLevel >= SQLITE_LOCK_SHARED);
        // PENDING stops new readers while existing readers drain.
        if (vitaFile->lockLevel < SQLITE_LOCK_PENDING) {
            if ((record.pending && record.pending != vitaFile) || record.exclusive)
                return SQLITE_BUSY;
            record.pending = vitaFile;
            vitaFile->lockLevel = SQLITE_LOCK_PENDING;
        }
        if (record.sharedHolders > 1)
            return SQLITE_BUSY;
        record.exclusive = vitaFile;
        vitaFile->lockLevel = SQLITE_LOCK_EXCLUSIVE;
        return SQLITE_OK;
    default:
        // SQLite never requests PENDING directly.
        return SQLITE_MISUSE;
    }
}

static int vitaUnlock(sqlite3_file* file, int level)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    ASSERT(level <= SQLITE_LOCK_SHARED);
    if (vitaFile->lockLevel <= level)
        return SQLITE_OK;

    Locker locker { lockTableLock };
    auto& record = *vitaFile->lockRecord;
    if (vitaFile->lockLevel > SQLITE_LOCK_SHARED) {
        if (record.reserved == vitaFile)
            record.reserved = nullptr;
        if (record.pending == vitaFile)
            record.pending = nullptr;
        if (record.exclusive == vitaFile)
            record.exclusive = nullptr;
        vitaFile->lockLevel = SQLITE_LOCK_SHARED;
    }
    if (level == SQLITE_LOCK_NONE) {
        ASSERT(record.sharedHolders);
        --record.sharedHolders;
        vitaFile->lockLevel = SQLITE_LOCK_NONE;
    }
    return SQLITE_OK;
}

static int vitaCheckReservedLock(sqlite3_file* file, int* result)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    Locker locker { lockTableLock };
    auto& record = *vitaFile->lockRecord;
    *result = record.reserved || record.pending || record.exclusive;
    return SQLITE_OK;
}

static int vitaFileControl(sqlite3_file*, int, void*)
{
    return SQLITE_NOTFOUND;
}

static int vitaSectorSize(sqlite3_file*)
{
    return 4096;
}

static int vitaDeviceCharacteristics(sqlite3_file*)
{
    // No atomic-write, safe-append or power-safe overwrite guarantee is known.
    return 0;
}

static const sqlite3_io_methods vitaIOMethods = {
    1,
    vitaClose,
    vitaRead,
    vitaWrite,
    vitaTruncate,
    vitaSync,
    vitaFileSize,
    vitaLock,
    vitaUnlock,
    vitaCheckReservedLock,
    vitaFileControl,
    vitaSectorSize,
    vitaDeviceCharacteristics,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

static const char* temporaryDirectory()
{
    if (sqlite3_temp_directory && *sqlite3_temp_directory)
        return sqlite3_temp_directory;
    auto* configured = getenv("TMPDIR");
    return configured && *configured ? configured : nullptr;
}

static int vitaOpen(sqlite3_vfs*, const char* name, sqlite3_file* file, int flags, int* outFlags)
{
    auto* vitaFile = reinterpret_cast<VitaFile*>(file);
    std::memset(vitaFile, 0, sizeof(*vitaFile));
    vitaFile->uid = -1;

    int nativeFlags = (flags & SQLITE_OPEN_READWRITE) ? SCE_O_RDWR : SCE_O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE)
        nativeFlags |= SCE_O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE)
        nativeFlags |= SCE_O_EXCL;

    CString path;
    SceUID uid = -1;
    if (name) {
        path = CString(name);
        uid = nativeOpen(name, nativeFlags, 0600);
    } else {
        // Anonymous temporary files: atomic exclusive creation with a random
        // name, deleted on close. Only EEXIST permits another attempt.
        auto* directory = temporaryDirectory();
        if (!directory)
            return SQLITE_CANTOPEN;
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            std::array<uint8_t, 16> entropy;
            cryptographicallyRandomValues(entropy);
            std::array<char, maximumPathLength> buffer;
            int length = snprintf(buffer.data(), buffer.size(), "%s/etilqs_", directory);
            for (auto byte : entropy) {
                if (length + 2 >= static_cast<int>(buffer.size()))
                    return SQLITE_CANTOPEN;
                length += snprintf(buffer.data() + length, buffer.size() - length, "%02x", byte);
            }
            uid = nativeOpen(buffer.data(), SCE_O_RDWR | SCE_O_CREAT | SCE_O_EXCL, 0600);
            if (uid != errorExists) {
                path = CString(buffer.data());
                break;
            }
        }
        flags |= SQLITE_OPEN_DELETEONCLOSE;
    }
    if (uid < 0)
        return SQLITE_CANTOPEN;

    vitaFile->uid = uid;
    vitaFile->lockLevel = SQLITE_LOCK_NONE;
    vitaFile->path = new CString(path);
    vitaFile->lockRecord = acquireLockRecord(path);
    vitaFile->deleteOnClose = flags & SQLITE_OPEN_DELETEONCLOSE;
    vitaFile->base.pMethods = &vitaIOMethods;
    if (outFlags)
        *outFlags = flags;
    return SQLITE_OK;
}

static int vitaDelete(sqlite3_vfs*, const char* name, int)
{
    // Native directory synchronization is not exposed; syncDir is not honored.
    int result = recordFailure(sceIoRemove(name));
    if (result == errorNoEntry)
        return SQLITE_IOERR_DELETE_NOENT;
    return result < 0 ? SQLITE_IOERR_DELETE : SQLITE_OK;
}

static int vitaAccess(sqlite3_vfs*, const char* name, int flags, int* result)
{
    SceIoStat stat { };
    if (sceIoGetstat(name, &stat) < 0) {
        *result = 0;
        return SQLITE_OK;
    }
    switch (flags) {
    case SQLITE_ACCESS_EXISTS:
        *result = 1;
        break;
    case SQLITE_ACCESS_READWRITE:
        *result = !!(stat.st_mode & SCE_S_IWUSR);
        break;
    default:
        *result = !!(stat.st_mode & SCE_S_IRUSR);
        break;
    }
    return SQLITE_OK;
}

// Produce a canonical native path: a lowercase mount prefix, single
// separators and resolved "." and ".." components. Relative paths have no
// native meaning without an explicit mount and are rejected.
static int vitaFullPathname(sqlite3_vfs*, const char* name, int outputSize, char* output)
{
    auto* colon = strchr(name, ':');
    if (!colon || colon == name)
        return SQLITE_CANTOPEN;
    size_t mountLength = colon - name + 1;
    if (static_cast<int>(mountLength) + 1 >= outputSize)
        return SQLITE_CANTOPEN;
    for (size_t index = 0; index < mountLength; ++index) {
        auto character = name[index];
        if (character == '/' || character == '\\')
            return SQLITE_CANTOPEN;
        output[index] = (character >= 'A' && character <= 'Z') ? character - 'A' + 'a' : character;
    }
    int length = mountLength;
    const char* cursor = colon + 1;
    while (*cursor) {
        while (*cursor == '/')
            ++cursor;
        if (!*cursor)
            break;
        auto* end = cursor;
        while (*end && *end != '/')
            ++end;
        size_t componentLength = end - cursor;
        if (componentLength == 1 && cursor[0] == '.') {
            cursor = end;
            continue;
        }
        if (componentLength == 2 && cursor[0] == '.' && cursor[1] == '.') {
            while (length > static_cast<int>(mountLength) && output[length - 1] != '/')
                --length;
            if (length > static_cast<int>(mountLength))
                --length;
            cursor = end;
            continue;
        }
        if (length + 1 + static_cast<int>(componentLength) + 1 > outputSize)
            return SQLITE_CANTOPEN;
        output[length++] = '/';
        std::memcpy(output + length, cursor, componentLength);
        length += componentLength;
        cursor = end;
    }
    if (length == static_cast<int>(mountLength)) {
        if (length + 2 > outputSize)
            return SQLITE_CANTOPEN;
        output[length++] = '/';
    }
    output[length] = '\0';
    return SQLITE_OK;
}

static int vitaRandomness(sqlite3_vfs*, int size, char* output)
{
    cryptographicallyRandomValues(std::span(reinterpret_cast<uint8_t*>(output), static_cast<size_t>(size)));
    return size;
}

static int vitaSleep(sqlite3_vfs*, int microseconds)
{
    sceKernelDelayThread(microseconds);
    return microseconds;
}

static int vitaCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64* output)
{
    // Julian day number of the Unix epoch, in milliseconds.
    static constexpr sqlite3_int64 unixEpochJulianMilliseconds = 210866760000000;
    *output = unixEpochJulianMilliseconds + static_cast<sqlite3_int64>(WallTime::now().secondsSinceEpoch().milliseconds());
    return SQLITE_OK;
}

static int vitaCurrentTime(sqlite3_vfs* vfs, double* output)
{
    sqlite3_int64 milliseconds;
    vitaCurrentTimeInt64(vfs, &milliseconds);
    *output = milliseconds / 86400000.0;
    return SQLITE_OK;
}

static int vitaGetLastError(sqlite3_vfs*, int, char*)
{
    return lastNativeError;
}

static sqlite3_vfs vitaVFS = {
    2,
    sizeof(VitaFile),
    maximumPathLength,
    nullptr,
    "vita",
    nullptr,
    vitaOpen,
    vitaDelete,
    vitaAccess,
    vitaFullPathname,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    vitaRandomness,
    vitaSleep,
    vitaCurrentTime,
    vitaGetLastError,
    vitaCurrentTimeInt64,
    nullptr,
    nullptr,
    nullptr,
};

} // namespace WebCore

extern "C" int sqlite3_os_init()
{
    return sqlite3_vfs_register(&WebCore::vitaVFS, 1);
}

extern "C" int sqlite3_os_end()
{
    return SQLITE_OK;
}
