/*
 * Copyright (C) 2003-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2006 Samuel Weinig <sam.weinig@gmail.com>
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (C) 2009 Gustavo Noronha Silva
 * Copyright (C) 2009 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"

#include "HTTPParsers.h"
#include <unicode/uchar.h>
#include <unicode/utf16.h>
#include <wtf/text/StringBuilder.h>

namespace WebCore {

ResourceRequest ResourceRequest::fromResourceRequestData(ResourceRequestData&& data)
{
    return WTF::switchOn(WTF::move(data), [](RequestData&& request) {
        return ResourceRequest(WTF::move(request));
    });
}

ResourceRequestData ResourceRequest::getRequestDataToSerialize() const
{
    updateResourceRequest();
    return m_requestData;
}

static String sanitizeFilename(StringView filename)
{
    auto trimmed = filename.trim([](char16_t character) {
        return deprecatedIsSpaceOrNewline(character) || character == '/' || character == '\\' || character == '.';
    });
    StringBuilder result;
    for (auto character : trimmed.codePoints()) {
        auto category = u_charType(character);
        if (category == U_CONTROL_CHAR || category == U_FORMAT_CHAR || U_IS_SURROGATE(character)
            || (character <= 0x7F && StringView("\"~*/:<>?\\|"_s).contains(static_cast<char16_t>(character)))) {
            result.append('_');
            continue;
        }
        if (U_IS_BMP(character))
            result.append(static_cast<char16_t>(character));
        else
            result.append(static_cast<char16_t>(U16_LEAD(character)), static_cast<char16_t>(U16_TRAIL(character)));
    }
    return result.toString();
}

String ResourceResponse::platformSuggestedFilename() const
{
    auto disposition = httpHeaderField(HTTPHeaderName::ContentDisposition);
    // The common parser currently supports the legacy filename parameter.
    // RFC 6266 extended/quoted parameter handling belongs with the transport.
    auto filename = filenameFromHTTPContentDisposition(disposition).toString();
    if (filename.is8Bit())
        filename = String::fromUTF8WithLatin1Fallback(filename.span8());
    return sanitizeFilename(filename);
}

} // namespace WebCore
