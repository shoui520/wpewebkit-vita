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

#pragma once

#include "DNSResolveQueue.h"
#include <wtf/CompletionHandler.h>
#include <wtf/HashMap.h>

namespace WebCore {

class DNSResolveQueueVita final : public DNSResolveQueue {
    WTF_MAKE_TZONE_ALLOCATED(DNSResolveQueueVita);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(DNSResolveQueueVita);
public:
    DNSResolveQueueVita() = default;

    void resolve(const String& hostname, uint64_t identifier, DNSCompletionHandler&&) final;
    void stopResolve(uint64_t identifier) final;

private:
    void updateIsUsingProxy() final;
    void platformResolve(const String&) final;

    HashMap<uint64_t, DNSCompletionHandler> m_requests;
};

using DNSResolveQueuePlatform = DNSResolveQueueVita;

} // namespace WebCore
