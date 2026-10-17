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
#include "DNSResolveQueueVita.h"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WorkQueue.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(DNSResolveQueueVita);

static WorkQueue& resolverQueue()
{
    static NeverDestroyed<Ref<WorkQueue>> queue(WorkQueue::create("org.webkit.DNSResolver"_s));
    return queue.get();
}

void DNSResolveQueueVita::updateIsUsingProxy()
{
    m_isUsingProxy = false;
}

// The network stack keeps its own DNS cache, which a system lookup would not
// warm, so prefetches are dropped.
void DNSResolveQueueVita::platformResolve(const String&)
{
    decrementRequestCount();
}

void DNSResolveQueueVita::resolve(const String& hostname, uint64_t identifier, DNSCompletionHandler&& completionHandler)
{
    ASSERT(isMainThread());
    m_requests.add(identifier, WTF::move(completionHandler));
    resolverQueue().dispatch([this, hostname = hostname.isolatedCopy(), identifier] {
        struct addrinfo hints { };
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* result = nullptr;
        Vector<IPAddress> addresses;
        if (!getaddrinfo(hostname.utf8().data(), nullptr, &hints, &result)) {
            for (auto* entry = result; entry; entry = entry->ai_next) {
                if (entry->ai_family == AF_INET)
                    addresses.append(IPAddress { reinterpret_cast<struct sockaddr_in*>(entry->ai_addr)->sin_addr });
            }
            freeaddrinfo(result);
        }
        callOnMainThread([this, identifier, addresses = WTF::move(addresses)]() mutable {
            auto completionHandler = m_requests.take(identifier);
            if (!completionHandler)
                return;
            if (addresses.isEmpty())
                completionHandler(makeUnexpected(DNSError::CannotResolve));
            else
                completionHandler(WTF::move(addresses));
        });
    });
}

void DNSResolveQueueVita::stopResolve(uint64_t identifier)
{
    if (auto completionHandler = m_requests.take(identifier))
        completionHandler(makeUnexpected(DNSError::Cancelled));
}

} // namespace WebCore
