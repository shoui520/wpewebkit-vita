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
#include <psp2/io/fcntl.h>
#include <sys/syslimits.h>
#include <unistd.h>
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

} // namespace WTF::FileSystemImpl
