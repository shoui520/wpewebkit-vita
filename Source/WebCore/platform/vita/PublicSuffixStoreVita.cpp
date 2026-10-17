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
#include "PublicSuffixStore.h"

#include <wtf/URL.h>

namespace WebCore {

// Vita has no public suffix database. Every top-level label is treated as a
// public suffix, as are generic second-level labels under country codes
// ("co.uk", "com.au", "ne.jp") and a few shared hosting domains.

static constexpr ASCIILiteral genericSecondLevelLabels[] = {
    "ac"_s, "co"_s, "com"_s, "edu"_s, "go"_s, "gob"_s, "gov"_s, "gv"_s, "ltd"_s, "mil"_s,
    "ne"_s, "net"_s, "nic"_s, "nom"_s, "or"_s, "org"_s, "plc"_s, "sch"_s,
};

static constexpr ASCIILiteral sharedHostingSuffixes[] = {
    "appspot.com"_s, "blogspot.com"_s, "cloudfront.net"_s, "github.io"_s, "gitlab.io"_s,
    "herokuapp.com"_s, "netlify.app"_s, "pages.dev"_s, "vercel.app"_s, "workers.dev"_s,
};

static bool isCountryCodeLabel(StringView label)
{
    return label.length() == 2 && isASCIIAlpha(label[0]) && isASCIIAlpha(label[1]);
}

bool PublicSuffixStore::platformIsPublicSuffix(StringView domain) const
{
    if (domain.isEmpty() || URL::hostIsIPAddress(domain))
        return false;

    auto firstDot = domain.find('.');
    if (firstDot == notFound)
        return true;

    for (auto suffix : sharedHostingSuffixes) {
        if (equalIgnoringASCIICase(domain, suffix))
            return true;
    }

    auto topLevelLabel = domain.substring(firstDot + 1);
    if (topLevelLabel.contains('.') || !isCountryCodeLabel(topLevelLabel))
        return false;

    auto secondLevelLabel = domain.left(firstDot);
    for (auto label : genericSecondLevelLabels) {
        if (equalIgnoringASCIICase(secondLevelLabel, label))
            return true;
    }
    return false;
}

String PublicSuffixStore::platformTopPrivatelyControlledDomain(StringView domain) const
{
    // This function is expected to work with the format used by cookies, so skip any leading dots.
    unsigned position = 0;
    while (position < domain.length() && domain[position] == '.')
        position++;

    auto host = domain.substring(position);
    if (host.isEmpty() || host.endsWith('.'))
        return { };
    if (URL::hostIsIPAddress(host))
        return host.toString();
    if (platformIsPublicSuffix(host))
        return { };

    // The last label is always a public suffix, so this finds the longest
    // suffix and returns it with one more label.
    size_t separatorPosition;
    for (unsigned labelStart = 0; (separatorPosition = host.find('.', labelStart)) != notFound; labelStart = separatorPosition + 1) {
        if (platformIsPublicSuffix(host.substring(separatorPosition + 1)))
            return host.substring(labelStart).toString();
    }
    return { };
}

} // namespace WebCore
