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
#include "MediaByteStream.h"

#if ENABLE(VIDEO) && PLATFORM(VITA)

#include "HTTPHeaderNames.h"
#include "ParsedContentRange.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

class MediaByteStream::Client final : public PlatformMediaResourceClient {
public:
    Client(MediaByteStream& stream, unsigned request, uint64_t offset)
        : m_stream(stream)
        , m_request(request)
        , m_offset(offset)
    {
    }

private:
    void responseReceived(PlatformMediaResource&, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completionHandler) final
    {
        m_stream->didReceiveResponse(m_request, m_offset, response);
        completionHandler(ShouldContinuePolicyCheck::Yes);
    }
    bool shouldCacheResponse(PlatformMediaResource&, const ResourceResponse&) final { return false; }
    void dataReceived(PlatformMediaResource&, const SharedBuffer& buffer) final { m_stream->didReceiveData(m_request, buffer.span()); }
    void accessControlCheckFailed(PlatformMediaResource&, const ResourceError&) final { m_stream->didFinish(m_request, "The server does not allow this page to load the media"_s); }
    void loadFailed(PlatformMediaResource&, const ResourceError& error) final
    {
        if (error.isCancellation())
            m_stream->didFinish(m_request, { });
        else
            m_stream->didFinish(m_request, error.localizedDescription().isEmpty() ? "The media failed to load"_s : error.localizedDescription());
    }
    void loadFinished(PlatformMediaResource&, const NetworkLoadMetrics&) final { m_stream->didFinish(m_request, { }); }

    const Ref<MediaByteStream> m_stream;
    const unsigned m_request;
    const uint64_t m_offset;
};

int MediaByteStream::read(uint8_t* buffer, uint64_t position, uint32_t length)
{
    Locker locker { m_lock };
    auto deadline = MonotonicTime::now() + 30_s;
    while (true) {
        if (m_closed || m_failed)
            return -1;
        if (m_size && position >= *m_size)
            return 0;
        uint64_t end = position + length;
        if (m_size)
            end = std::min(end, *m_size);
        uint64_t loadedEnd = dataEnd();
        bool startsInData = position >= m_dataStart && position <= loadedEnd;
        // A load that ended short of the size: what there is.
        if (startsInData && !m_loading && m_atEnd)
            end = std::min(end, loadedEnd);
        if (startsInData && end <= loadedEnd) {
            memcpySpan(std::span { buffer, static_cast<size_t>(end - position) }, m_data.span().subspan(position - m_dataStart, end - position));
            m_readEnd = end;
            if (!m_loading && !m_atEnd && loadedEnd - end < resumeAhead)
                requestFrom(loadedEnd);
            dropConsumedData();
            return end - position;
        }
        if (position < m_dataStart || position > loadedEnd + jumpDistance)
            requestFrom(position);
        else if (!m_loading)
            requestFrom(loadedEnd);
        if (MonotonicTime::now() > deadline) {
            fail("Loading the media timed out"_s);
            return -1;
        }
        m_condition.waitFor(m_lock, 100_ms);
    }
}

uint64_t MediaByteStream::size()
{
    Locker locker { m_lock };
    auto deadline = MonotonicTime::now() + 30_s;
    while (!m_size && !m_failed && !m_closed && !m_atEnd && MonotonicTime::now() < deadline)
        m_condition.waitFor(m_lock, 100_ms);
    return m_size.value_or(0);
}

std::optional<uint64_t> MediaByteStream::knownSize() const
{
    Locker locker { m_lock };
    return m_size;
}

String MediaByteStream::failure() const
{
    Locker locker { m_lock };
    return m_failure.isolatedCopy();
}

void MediaByteStream::fail(String&& failure)
{
    m_failed = true;
    if (m_failure.isNull())
        m_failure = failure.isolatedCopy();
    m_condition.notifyAll();
}

void MediaByteStream::start()
{
    Locker locker { m_lock };
    requestFrom(0);
}

void MediaByteStream::close()
{
    {
        Locker locker { m_lock };
        m_closed = true;
        ++m_request;
        m_condition.notifyAll();
    }
    if (RefPtr resource = std::exchange(m_resource, nullptr)) {
        resource->setClient(nullptr);
        resource->shutdown();
    }
}

uint64_t MediaByteStream::loadedBytes() const
{
    Locker locker { m_lock };
    return m_loadedBytes;
}

std::optional<double> MediaByteStream::loadedFraction() const
{
    Locker locker { m_lock };
    if (!m_size || !*m_size || m_dataStart)
        return std::nullopt;
    return std::min(1.0, static_cast<double>(dataEnd()) / *m_size);
}

void MediaByteStream::requestFrom(uint64_t offset)
{
    // Continuing the loaded data keeps it; elsewhere starts a new window.
    if (offset != dataEnd()) {
        m_data.clear();
        m_dataStart = offset;
        m_readEnd = offset;
    }
    m_loading = true;
    m_atEnd = false;
    unsigned request = ++m_request;
    callOnMainThread([protectedThis = Ref { *this }, offset, request] {
        protectedThis->startRequest(offset, request);
    });
}

void MediaByteStream::stopRequest()
{
    m_loading = false;
    ++m_request;
    callOnMainThread([protectedThis = Ref { *this }] {
        if (RefPtr resource = std::exchange(protectedThis->m_resource, nullptr)) {
            resource->setClient(nullptr);
            resource->shutdown();
        }
    });
}

void MediaByteStream::dropConsumedData()
{
    // Drop in large steps: each one moves the data that stays.
    if (m_readEnd < m_dataStart + keepBehind + 2 * MB)
        return;
    size_t drop = m_readEnd - keepBehind - m_dataStart;
    m_data.removeAt(0, drop);
    m_dataStart += drop;
}

void MediaByteStream::startRequest(uint64_t offset, unsigned request)
{
    {
        Locker locker { m_lock };
        if (m_closed || request != m_request)
            return;
    }
    if (RefPtr resource = std::exchange(m_resource, nullptr)) {
        resource->setClient(nullptr);
        resource->shutdown();
    }
    ResourceRequest resourceRequest { URL { m_url } };
    resourceRequest.setHTTPHeaderField(HTTPHeaderName::Range, makeString("bytes="_s, offset, '-'));
    resourceRequest.setHTTPHeaderField(HTTPHeaderName::AcceptEncoding, "identity"_s);
    m_resource = m_loader->requestResource(WTF::move(resourceRequest), PlatformMediaResourceLoader::LoadOption::DisallowCaching);
    if (!m_resource) {
        Locker locker { m_lock };
        fail("The media could not be requested"_s);
        return;
    }
    m_resource->setClient(adoptRef(*new Client(*this, request, offset)));
}

void MediaByteStream::didReceiveResponse(unsigned request, uint64_t offset, const ResourceResponse& response)
{
    Locker locker { m_lock };
    if (request != m_request)
        return;
    int status = response.httpStatusCode();
    m_skip = 0;
    if (status == 206) {
        ParsedContentRange range { response.httpHeaderField(HTTPHeaderName::ContentRange) };
        if (range.isValid()) {
            if (range.instanceLength() != ParsedContentRange::unknownLength)
                m_size = range.instanceLength();
            if (static_cast<uint64_t>(range.firstBytePosition()) <= offset)
                m_skip = offset - range.firstBytePosition();
            else
                fail("The server sent another part of the media than the one asked for"_s);
        }
    } else if (status == 416 && m_size && offset >= *m_size) {
        m_loading = false;
        m_atEnd = true;
    } else if (status && (status < 200 || status >= 300))
        fail(makeString("The server answered "_s, status));
    else {
        // The whole resource: a server without ranges, or not HTTP.
        m_skip = offset;
        if (response.expectedContentLength() > 0)
            m_size = response.expectedContentLength();
    }
    m_condition.notifyAll();
}

void MediaByteStream::didReceiveData(unsigned request, std::span<const uint8_t> data)
{
    Locker locker { m_lock };
    if (request != m_request)
        return;
    size_t skipped = std::min<uint64_t>(m_skip, data.size());
    m_skip -= skipped;
    data = data.subspan(skipped);
    m_data.append(data);
    m_loadedBytes += data.size();
    if (dataEnd() - m_readEnd > maximumAhead)
        stopRequest();
    m_condition.notifyAll();
}

void MediaByteStream::didFinish(unsigned request, const String& failure)
{
    Locker locker { m_lock };
    if (request != m_request)
        return;
    m_loading = false;
    if (!failure.isNull())
        fail(String { failure });
    else {
        if (!m_size && !m_dataStart)
            m_size = dataEnd();
        m_atEnd = !m_size || dataEnd() >= *m_size;
    }
    m_condition.notifyAll();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
