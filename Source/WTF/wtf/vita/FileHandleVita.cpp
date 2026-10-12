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
#include <wtf/FileHandle.h>

#include <array>
#include <limits>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <wtf/FileSystem.h>
#include <wtf/MappedFileData.h>

namespace WTF::FileSystemImpl {

static constexpr int errorInterrupted = static_cast<int>(0x80010004); // SCE_ERROR_ERRNO_EINTR

std::optional<uint64_t> FileHandle::read(std::span<uint8_t> data)
{
    if (!m_handle)
        return { };
    auto size = std::min(data.size(), static_cast<size_t>(std::numeric_limits<SceSSize>::max()));
    int result;
    do {
        result = sceIoRead(m_handle->uid, data.data(), size);
    } while (result == errorInterrupted);
    if (result < 0)
        return { };
    return static_cast<uint64_t>(result);
}

std::optional<uint64_t> FileHandle::write(std::span<const uint8_t> data)
{
    if (!m_handle)
        return { };
    auto size = std::min(data.size(), static_cast<size_t>(std::numeric_limits<SceSSize>::max()));
    int result;
    do {
        result = sceIoWrite(m_handle->uid, data.data(), size);
    } while (result == errorInterrupted);
    if (result < 0)
        return { };
    return static_cast<uint64_t>(result);
}

bool FileHandle::truncate(int64_t offset)
{
    if (!m_handle || offset < 0)
        return false;
    auto oldSize = size();
    if (!oldSize)
        return false;

    // Native size changes do not zero an extended range. Write that range
    // explicitly, without moving the handle's current seek position.
    if (static_cast<uint64_t>(offset) > *oldSize) {
        std::array<uint8_t, 4096> zeros { };
        uint64_t position = *oldSize;
        while (position < static_cast<uint64_t>(offset)) {
            auto count = std::min(static_cast<uint64_t>(offset) - position, static_cast<uint64_t>(zeros.size()));
            int result;
            do {
                result = sceIoPwrite(m_handle->uid, zeros.data(), count, position);
            } while (result == errorInterrupted);
            if (result <= 0) {
                if (position != *oldSize) {
                    SceIoStat original { };
                    original.st_size = *oldSize;
                    if (sceIoChstatByFd(m_handle->uid, &original, SCE_CST_SIZE) < 0)
                        LOG_ERROR("Unable to restore file size after extension failure");
                }
                return false;
            }
            position += static_cast<unsigned>(result);
        }
        return true;
    }
    SceIoStat stat { };
    stat.st_size = offset;
    return !sceIoChstatByFd(m_handle->uid, &stat, SCE_CST_SIZE);
}

bool FileHandle::flush()
{
    return m_handle && !sceIoSyncByFd(m_handle->uid, 0);
}

std::optional<uint64_t> FileHandle::seek(int64_t offset, FileSeekOrigin origin)
{
    if (!m_handle)
        return { };
    int whence;
    switch (origin) {
    case FileSeekOrigin::Beginning:
        whence = SCE_SEEK_SET;
        break;
    case FileSeekOrigin::Current:
        whence = SCE_SEEK_CUR;
        break;
    case FileSeekOrigin::End:
        whence = SCE_SEEK_END;
        break;
    default:
        return { };
    }
    auto result = sceIoLseek(m_handle->uid, offset, whence);
    if (result < 0)
        return { };
    return static_cast<uint64_t>(result);
}

std::optional<PlatformFileID> FileHandle::id()
{
    // SceIoStat exposes no portable, stable inode identity.
    return { };
}

void FileHandle::close()
{
    if (auto handle = std::exchange(m_handle, std::nullopt))
        sceIoClose(handle->uid);
}

std::optional<uint64_t> FileHandle::size()
{
    if (!m_handle)
        return { };
    SceIoStat stat { };
    if (sceIoGetstatByFd(m_handle->uid, &stat) < 0 || stat.st_size < 0)
        return { };
    return static_cast<uint64_t>(stat.st_size);
}

std::optional<MappedFileData> FileHandle::map(MappedFileMode, FileOpenMode)
{
    // Do not send native I/O UIDs to the POSIX runtime's mmap backend.
    // Empty files need no mapping; nonempty file mappings are unsupported.
    if (auto bytes = size(); bytes && !*bytes)
        return MappedFileData::emptyFile();
    return { };
}

} // namespace WTF::FileSystemImpl
