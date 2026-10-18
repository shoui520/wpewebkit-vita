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

#if ENABLE(VIDEO) && PLATFORM(VITA)

#include "PlatformMediaResourceLoader.h"
#include <optional>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/URL.h>
#include <wtf/Vector.h>

namespace WebCore {

class ResourceResponse;

// A media resource as a player reads it: ranges loaded through the media
// loader into a window of memory that follows the reads. Reads come from the
// player's threads and wait for the main thread's loads.
class MediaByteStream final : public ThreadSafeRefCounted<MediaByteStream, WTF::DestructionThread::Main> {
public:
    static Ref<MediaByteStream> create(Ref<PlatformMediaResourceLoader>&& loader, const URL& url)
    {
        return adoptRef(*new MediaByteStream(WTF::move(loader), url));
    }

    // The player's threads.
    int read(uint8_t* buffer, uint64_t position, uint32_t length);
    // Waits for the size to be known.
    uint64_t size();
    std::optional<uint64_t> knownSize() const;
    // Why the load failed, or null.
    String failure() const;

    // The main thread.
    void start();
    void close();
    uint64_t loadedBytes() const;
    // The part of the resource loaded, as a fraction from its start, when known.
    std::optional<double> loadedFraction() const;

private:
    class Client;

    MediaByteStream(Ref<PlatformMediaResourceLoader>&& loader, const URL& url)
        : m_loader(WTF::move(loader))
        , m_url(url.isolatedCopy())
    {
    }

    // Keep up to this much loaded ahead of the reads; load more when less
    // than the resume distance is left; keep some behind for reads that step
    // back between interleaved tracks.
    static constexpr uint64_t maximumAhead = 8 * MB;
    static constexpr uint64_t resumeAhead = 2 * MB;
    static constexpr uint64_t keepBehind = 4 * MB;
    // A read past the loaded data by more than this starts a new range rather
    // than waiting for the load to get there.
    static constexpr uint64_t jumpDistance = 512 * KB;

    uint64_t dataEnd() const WTF_REQUIRES_LOCK(m_lock) { return m_dataStart + m_data.size(); }
    void requestFrom(uint64_t offset) WTF_REQUIRES_LOCK(m_lock);
    void stopRequest() WTF_REQUIRES_LOCK(m_lock);
    void dropConsumedData() WTF_REQUIRES_LOCK(m_lock);

    void startRequest(uint64_t offset, unsigned request);
    void didReceiveResponse(unsigned request, uint64_t offset, const ResourceResponse&);
    void didReceiveData(unsigned request, std::span<const uint8_t>);
    // A null failure is a load that completed.
    void didFinish(unsigned request, const String& failure);
    void fail(String&&) WTF_REQUIRES_LOCK(m_lock);

    const Ref<PlatformMediaResourceLoader> m_loader;
    const URL m_url;
    // The main thread.
    RefPtr<PlatformMediaResource> m_resource;

    mutable Lock m_lock;
    Condition m_condition;
    Vector<uint8_t> m_data WTF_GUARDED_BY_LOCK(m_lock);
    uint64_t m_dataStart WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    uint64_t m_readEnd WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    std::optional<uint64_t> m_size WTF_GUARDED_BY_LOCK(m_lock);
    // Loads are numbered; data from one that was replaced or stopped is dropped.
    unsigned m_request WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_loading WTF_GUARDED_BY_LOCK(m_lock) { false };
    // The load ended at the end of the resource.
    bool m_atEnd WTF_GUARDED_BY_LOCK(m_lock) { false };
    // A server that ignored the range sends the bytes before it too.
    uint64_t m_skip WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_failed WTF_GUARDED_BY_LOCK(m_lock) { false };
    String m_failure WTF_GUARDED_BY_LOCK(m_lock);
    bool m_closed WTF_GUARDED_BY_LOCK(m_lock) { false };
    uint64_t m_loadedBytes WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
