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

#include "config.h"
#include <wtf/FileSystem.h>

#include <array>
#include <cstdlib>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <sys/syslimits.h>
#include <unistd.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/FileHandle.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringView.h>

namespace WTF::FileSystemImpl {

static constexpr int errorInterrupted = static_cast<int>(0x80010004); // SCE_ERROR_ERRNO_EINTR
static constexpr int errorExists = static_cast<int>(0x80010011); // SCE_ERROR_ERRNO_EEXIST

static CString nativePath(const String& path)
{
    if (path.isEmpty() || path.contains(char16_t(0)))
        return { };
    if (path.contains(':'))
        return fileSystemRepresentation(path);

    // Share the selected runtime's working directory, without sharing its FDs.
    std::array<char, PATH_MAX> buffer;
    if (!getcwd(buffer.data(), buffer.size()))
        return { };
    auto directory = String::fromUTF8(buffer.data());
    if (path.startsWith('/')) {
        auto colon = directory.find(':');
        if (colon == notFound)
            return { };
        return fileSystemRepresentation(makeString(directory.left(colon + 1), path));
    }
    return fileSystemRepresentation(pathByAppendingComponent(directory, path));
}

static int nativeOpen(const CString& path, int flags, int permissions)
{
    int result;
    do {
        result = sceIoOpen(path.data(), flags, permissions);
    } while (result == errorInterrupted);
    return result;
}

FileHandle openFile(const String& path, FileOpenMode mode, FileAccessPermission permission, OptionSet<FileLockMode> lockMode, bool failIfFileExists)
{
    if (lockMode)
        return { };
    auto representation = nativePath(path);
    if (representation.isNull())
        return { };
    int flags;
    switch (mode) {
    case FileOpenMode::Read:
        flags = SCE_O_RDONLY;
        break;
    case FileOpenMode::Truncate:
        flags = SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC;
        break;
    case FileOpenMode::ReadWrite:
        flags = SCE_O_RDWR | SCE_O_CREAT;
        break;
    default:
        return { };
    }
    if (failIfFileExists)
        flags |= SCE_O_CREAT | SCE_O_EXCL;
    int permissions = permission == FileAccessPermission::User ? 0600 : 0666;
    return FileHandle::adopt(PlatformFileHandle(nativeOpen(representation, flags, permissions)));
}

static bool isFileNameComponent(StringView value)
{
    return !value.contains(char16_t(0)) && !value.contains('/') && !value.contains('\\') && !value.contains(':');
}

std::pair<String, FileHandle> openTemporaryFile(StringView prefix, StringView suffix, const String& temporaryDirectory)
{
    if (!isFileNameComponent(prefix) || !isFileNameComponent(suffix))
        return { };
    String directory = temporaryDirectory;
    if (directory.isEmpty()) {
        auto* configured = getenv("TMPDIR");
        if (!configured || !*configured)
            return { };
        directory = String::fromUTF8(configured);
    }
    if (directory.isEmpty() || directory.contains(char16_t(0)))
        return { };

    // Atomic native exclusive creation resolves both random-name collisions
    // and races. Only EEXIST permits retry; other I/O failures stop here.
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        std::array<uint8_t, 16> entropy;
        cryptographicallyRandomValues(entropy);
        std::array<char, 32> hex;
        constexpr auto digits = "0123456789abcdef";
        for (size_t index = 0; index < entropy.size(); ++index) {
            hex[2 * index] = digits[entropy[index] >> 4];
            hex[2 * index + 1] = digits[entropy[index] & 15];
        }
        auto name = makeString(prefix, '-', String::fromUTF8(std::span(hex)), suffix);
        auto path = pathByAppendingComponent(directory, name);
        auto representation = nativePath(path);
        if (representation.isNull())
            return { };
        int uid = nativeOpen(representation, SCE_O_RDWR | SCE_O_CREAT | SCE_O_EXCL, 0600);
        if (uid >= 0)
            return { path, FileHandle::adopt(PlatformFileHandle(uid)) };
        if (uid != errorExists)
            return { };
    }
    return { };
}

// The runtime's std::filesystem cannot create or read directories: these use
// the native calls.

static std::optional<SceIoStat> nativeStat(const CString& path)
{
    SceIoStat stat;
    if (sceIoGetstat(path.data(), &stat) < 0)
        return std::nullopt;
    return stat;
}

bool makeAllDirectories(const String& path)
{
    auto representation = nativePath(path);
    if (representation.isNull())
        return false;
    auto full = representation.span();
    // Every prefix that ends a component after the device name ("ux0:").
    size_t device = std::string_view(full.data(), full.size()).find(':');
    size_t start = device == std::string_view::npos ? 0 : device + 1;
    for (size_t end = start; end <= full.size(); ++end) {
        if (end < full.size() && full[end] != '/')
            continue;
        if (end == start || full[end - 1] == '/')
            continue;
        CString prefix(full.first(end));
        if (sceIoMkdir(prefix.data(), 0777) >= 0)
            continue;
        auto stat = nativeStat(prefix);
        if (!stat || !SCE_S_ISDIR(stat->st_mode))
            return false;
    }
    return true;
}

Vector<String> listDirectory(const String& path)
{
    Vector<String> names;
    auto representation = nativePath(path);
    if (representation.isNull())
        return names;
    SceUID directory = sceIoDopen(representation.data());
    if (directory < 0)
        return names;
    SceIoDirent entry;
    while (true) {
        zeroBytes(entry);
        if (sceIoDread(directory, &entry) <= 0)
            break;
        auto name = String::fromUTF8(entry.d_name);
        if (!name.isEmpty() && name != "."_s && name != ".."_s)
            names.append(WTF::move(name));
    }
    sceIoDclose(directory);
    return names;
}

bool deleteFile(const String& path)
{
    auto representation = nativePath(path);
    if (representation.isNull())
        return false;
    auto stat = nativeStat(representation);
    if (!stat || SCE_S_ISDIR(stat->st_mode))
        return false;
    return sceIoRemove(representation.data()) >= 0;
}

bool deleteEmptyDirectory(const String& path)
{
    auto representation = nativePath(path);
    return !representation.isNull() && sceIoRmdir(representation.data()) >= 0;
}

bool deleteNonEmptyDirectory(const String& path)
{
    auto representation = nativePath(path);
    if (representation.isNull())
        return false;
    for (auto& name : listDirectory(path)) {
        auto child = pathByAppendingComponent(path, name);
        auto childRepresentation = nativePath(child);
        auto stat = nativeStat(childRepresentation);
        if (stat && SCE_S_ISDIR(stat->st_mode))
            deleteNonEmptyDirectory(child);
        else
            sceIoRemove(childRepresentation.data());
    }
    return sceIoRmdir(representation.data()) >= 0;
}

bool moveFile(const String& oldPath, const String& newPath)
{
    auto from = nativePath(oldPath);
    auto to = nativePath(newPath);
    if (from.isNull() || to.isNull())
        return false;
    if (sceIoRename(from.data(), to.data()) >= 0)
        return true;
    // Unlike POSIX rename(), the native call does not replace an existing file.
    auto stat = nativeStat(to);
    if (!stat || SCE_S_ISDIR(stat->st_mode) || sceIoRemove(to.data()) < 0)
        return false;
    return sceIoRename(from.data(), to.data()) >= 0;
}

std::optional<uint64_t> directorySize(const String& path)
{
    auto representation = nativePath(path);
    auto stat = representation.isNull() ? std::nullopt : nativeStat(representation);
    if (!stat || !SCE_S_ISDIR(stat->st_mode))
        return std::nullopt;
    CheckedUint64 size = 0;
    for (auto& name : listDirectory(path)) {
        auto child = pathByAppendingComponent(path, name);
        auto childStat = nativeStat(nativePath(child));
        if (!childStat)
            return std::nullopt;
        if (SCE_S_ISDIR(childStat->st_mode)) {
            auto childSize = directorySize(child);
            if (!childSize)
                return std::nullopt;
            size += *childSize;
        } else
            size += static_cast<uint64_t>(childStat->st_size);
        if (size.hasOverflowed())
            return std::nullopt;
    }
    return size.value();
}

} // namespace WTF::FileSystemImpl
