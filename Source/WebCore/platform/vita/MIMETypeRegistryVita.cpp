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
#include "MIMETypeRegistry.h"

namespace WebCore {

// There is no system MIME database on Vita. The first entry for a type is its
// preferred extension.
static constexpr auto platformMediaTypes = std::to_array<TypeExtensionPair>({
    { "application/javascript"_s, "js"_s },
    { "application/javascript"_s, "mjs"_s },
    { "application/json"_s, "json"_s },
    { "application/pdf"_s, "pdf"_s },
    { "application/rss+xml"_s, "rss"_s },
    { "application/atom+xml"_s, "atom"_s },
    { "application/wasm"_s, "wasm"_s },
    { "application/xhtml+xml"_s, "xhtml"_s },
    { "application/xhtml+xml"_s, "xht"_s },
    { "application/xml"_s, "xml"_s },
    { "application/zip"_s, "zip"_s },
    { "application/gzip"_s, "gz"_s },
    { "application/octet-stream"_s, "bin"_s },
    { "font/otf"_s, "otf"_s },
    { "font/ttf"_s, "ttf"_s },
    { "font/woff"_s, "woff"_s },
    { "font/woff2"_s, "woff2"_s },
    { "image/avif"_s, "avif"_s },
    { "image/bmp"_s, "bmp"_s },
    { "image/gif"_s, "gif"_s },
    { "image/jpeg"_s, "jpg"_s },
    { "image/jpeg"_s, "jpeg"_s },
    { "image/jpeg"_s, "jpe"_s },
    { "image/png"_s, "png"_s },
    { "image/svg+xml"_s, "svg"_s },
    { "image/svg+xml"_s, "svgz"_s },
    { "image/webp"_s, "webp"_s },
    { "image/x-icon"_s, "ico"_s },
    { "image/x-xbitmap"_s, "xbm"_s },
    { "text/css"_s, "css"_s },
    { "text/csv"_s, "csv"_s },
    { "text/html"_s, "html"_s },
    { "text/html"_s, "htm"_s },
    { "text/html"_s, "shtml"_s },
    { "text/markdown"_s, "md"_s },
    { "text/plain"_s, "txt"_s },
    { "text/plain"_s, "text"_s },
    { "text/plain"_s, "log"_s },
    { "text/vtt"_s, "vtt"_s },
    { "text/xml"_s, "xml"_s },
    { "text/xsl"_s, "xsl"_s },
    { "text/xsl"_s, "xslt"_s },
    { "audio/aac"_s, "aac"_s },
    { "audio/flac"_s, "flac"_s },
    { "audio/mp4"_s, "m4a"_s },
    { "audio/mpeg"_s, "mp3"_s },
    { "audio/ogg"_s, "ogg"_s },
    { "audio/ogg"_s, "oga"_s },
    { "audio/opus"_s, "opus"_s },
    { "audio/wav"_s, "wav"_s },
    { "video/mp4"_s, "mp4"_s },
    { "video/mp4"_s, "m4v"_s },
    { "video/ogg"_s, "ogv"_s },
    { "video/quicktime"_s, "mov"_s },
    { "video/webm"_s, "webm"_s },
});

String MIMETypeRegistry::mimeTypeForExtension(StringView extension)
{
    for (auto& entry : platformMediaTypes) {
        if (equalIgnoringASCIICase(extension, entry.extension))
            return entry.type;
    }
    return emptyString();
}

bool MIMETypeRegistry::isApplicationPluginMIMEType(const String&)
{
    return false;
}

String MIMETypeRegistry::preferredExtensionForMIMEType(const String& mimeType)
{
    for (auto& entry : platformMediaTypes) {
        if (equalIgnoringASCIICase(mimeType, entry.type))
            return entry.extension;
    }
    return emptyString();
}

Vector<String> MIMETypeRegistry::extensionsForMIMEType(const String& mimeType)
{
    Vector<String> extensions;
    for (auto& entry : platformMediaTypes) {
        if (equalIgnoringASCIICase(mimeType, entry.type))
            extensions.append(entry.extension);
    }
    return extensions;
}

} // namespace WebCore
