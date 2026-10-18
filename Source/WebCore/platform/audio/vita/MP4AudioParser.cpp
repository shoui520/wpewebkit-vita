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
#include "MP4AudioParser.h"

#if PLATFORM(VITA) && (ENABLE(VIDEO) || ENABLE(WEB_AUDIO))

namespace WebCore {

namespace {

constexpr uint32_t fourCC(const char (&name)[5])
{
    return static_cast<uint32_t>(name[0]) << 24 | static_cast<uint32_t>(name[1]) << 16 | static_cast<uint32_t>(name[2]) << 8 | static_cast<uint32_t>(name[3]);
}

// Reads big-endian fields; past the end, every read fails and returns 0.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> data)
        : m_data(data)
    {
    }

    bool failed() const { return m_failed; }
    size_t remaining() const { return m_data.size() - m_position; }

    uint64_t read(unsigned bytes)
    {
        if (remaining() < bytes) {
            m_failed = true;
            m_position = m_data.size();
            return 0;
        }
        uint64_t value = 0;
        for (unsigned i = 0; i < bytes; ++i)
            value = value << 8 | m_data[m_position++];
        return value;
    }

    void skip(size_t bytes)
    {
        if (remaining() < bytes) {
            m_failed = true;
            m_position = m_data.size();
            return;
        }
        m_position += bytes;
    }

    std::span<const uint8_t> take(size_t bytes)
    {
        if (remaining() < bytes) {
            m_failed = true;
            m_position = m_data.size();
            return { };
        }
        auto result = m_data.subspan(m_position, bytes);
        m_position += bytes;
        return result;
    }

    std::span<const uint8_t> rest() { return take(remaining()); }

private:
    std::span<const uint8_t> m_data;
    size_t m_position { 0 };
    bool m_failed { false };
};

struct Box {
    uint32_t type;
    std::span<const uint8_t> body;
};

// Calls the function for each box in the data until it returns false.
template<typename Function> bool forEachBox(std::span<const uint8_t> data, const Function& function)
{
    Reader reader { data };
    while (reader.remaining() >= 8) {
        uint64_t size = reader.read(4);
        uint32_t type = reader.read(4);
        size_t header = 8;
        if (size == 1) {
            size = reader.read(8);
            header = 16;
        } else if (!size)
            size = reader.remaining() + header;
        if (reader.failed() || size < header || size - header > reader.remaining())
            return false;
        if (!function(Box { type, reader.take(size - header) }))
            return true;
    }
    return true;
}

std::optional<Box> findBox(std::span<const uint8_t> data, uint32_t type)
{
    std::optional<Box> found;
    forEachBox(data, [&](const Box& box) {
        if (box.type != type)
            return true;
        found = box;
        return false;
    });
    return found;
}

// An MPEG-4 descriptor's length: up to four bytes of seven bits.
uint32_t descriptorLength(Reader& reader)
{
    uint32_t length = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t byte = reader.read(1);
        length = length << 7 | (byte & 0x7f);
        if (!(byte & 0x80))
            break;
    }
    return length;
}

bool parseESDS(std::span<const uint8_t> body, MP4AudioTrack& track)
{
    Reader reader { body };
    reader.skip(4);
    if (reader.read(1) != 0x03)
        return false;
    descriptorLength(reader);
    reader.skip(2);
    uint8_t flags = reader.read(1);
    if (flags & 0x80)
        reader.skip(2);
    if (flags & 0x40)
        reader.skip(reader.read(1));
    if (flags & 0x20)
        reader.skip(2);
    if (reader.read(1) != 0x04)
        return false;
    descriptorLength(reader);
    track.objectType = reader.read(1);
    reader.skip(12);
    if (!reader.failed() && reader.remaining() && reader.read(1) == 0x05) {
        uint32_t length = descriptorLength(reader);
        auto config = reader.take(length);
        track.decoderConfig.append(config);
    }
    return !reader.failed() || track.objectType;
}

bool parseSampleEntry(std::span<const uint8_t> stsd, MP4AudioTrack& track)
{
    Reader reader { stsd };
    reader.skip(8);
    bool found = false;
    forEachBox(reader.rest(), [&](const Box& entry) {
        if (entry.type != fourCC("mp4a"))
            return true;
        Reader fields { entry.body };
        fields.skip(8);
        unsigned version = fields.read(2);
        fields.skip(6);
        track.channels = fields.read(2);
        fields.skip(6);
        track.sampleRate = fields.read(4) >> 16;
        if (version == 1)
            fields.skip(16);
        else if (version == 2) {
            fields.skip(4);
            uint64_t bits = fields.read(8);
            double rate;
            static_assert(sizeof(rate) == sizeof(bits));
            memcpy(&rate, &bits, sizeof(rate));
            track.sampleRate = static_cast<unsigned>(rate);
            track.channels = fields.read(4);
            fields.skip(20);
        }
        if (fields.failed())
            return false;
        auto children = fields.rest();
        // QuickTime files put the configuration in a 'wave' box.
        if (auto wave = findBox(children, fourCC("wave")))
            children = wave->body;
        if (auto esds = findBox(children, fourCC("esds")))
            found = parseESDS(esds->body, track);
        return false;
    });
    return found;
}

bool parseSampleTable(std::span<const uint8_t> stbl, MP4AudioTrack& track)
{
    auto stsd = findBox(stbl, fourCC("stsd"));
    auto stsz = findBox(stbl, fourCC("stsz"));
    auto stsc = findBox(stbl, fourCC("stsc"));
    auto stco = findBox(stbl, fourCC("stco"));
    auto co64 = findBox(stbl, fourCC("co64"));
    if (!stsd || !stsz || !stsc || (!stco && !co64))
        return false;
    if (!parseSampleEntry(stsd->body, track))
        return false;

    Reader sizes { stsz->body };
    sizes.skip(4);
    uint32_t fixedSize = sizes.read(4);
    uint32_t sampleCount = sizes.read(4);
    if (sizes.failed() || sampleCount > 50'000'000 || (!fixedSize && sizes.remaining() < 4ull * sampleCount))
        return false;

    Reader chunks { stco ? stco->body : co64->body };
    unsigned offsetBytes = stco ? 4 : 8;
    chunks.skip(4);
    uint32_t chunkCount = chunks.read(4);
    if (chunks.failed() || chunks.remaining() < static_cast<uint64_t>(offsetBytes) * chunkCount)
        return false;

    Reader runs { stsc->body };
    runs.skip(4);
    uint32_t runCount = runs.read(4);
    if (runs.failed() || runs.remaining() < 12ull * runCount || !runCount)
        return false;
    struct Run {
        uint32_t firstChunk;
        uint32_t samplesPerChunk;
    };
    Vector<Run> chunkRuns;
    for (uint32_t i = 0; i < runCount; ++i) {
        uint32_t firstChunk = runs.read(4);
        uint32_t samplesPerChunk = runs.read(4);
        runs.skip(4);
        chunkRuns.append({ firstChunk, samplesPerChunk });
    }

    track.samples.reserveInitialCapacity(sampleCount);
    size_t run = 0;
    uint32_t sample = 0;
    for (uint32_t chunk = 1; chunk <= chunkCount && sample < sampleCount; ++chunk) {
        while (run + 1 < chunkRuns.size() && chunkRuns[run + 1].firstChunk <= chunk)
            ++run;
        uint64_t offset = chunks.read(offsetBytes);
        for (uint32_t i = 0; i < chunkRuns[run].samplesPerChunk && sample < sampleCount; ++i, ++sample) {
            uint32_t size = fixedSize ? fixedSize : static_cast<uint32_t>(sizes.read(4));
            track.samples.append({ offset, size });
            offset += size;
        }
    }
    return !track.samples.isEmpty();
}

bool parseTrack(std::span<const uint8_t> trak, uint32_t movieTimescale, MP4AudioTrack& track)
{
    auto mdia = findBox(trak, fourCC("mdia"));
    if (!mdia)
        return false;
    auto hdlr = findBox(mdia->body, fourCC("hdlr"));
    if (!hdlr)
        return false;
    Reader handler { hdlr->body };
    handler.skip(8);
    if (handler.read(4) != fourCC("soun"))
        return false;

    auto mdhd = findBox(mdia->body, fourCC("mdhd"));
    if (!mdhd)
        return false;
    Reader header { mdhd->body };
    unsigned version = header.read(1);
    header.skip(3 + (version == 1 ? 16 : 8));
    track.timescale = header.read(4);
    if (header.failed() || !track.timescale)
        return false;

    // The first edit that plays media: the encoder's delay comes before it.
    if (auto edts = findBox(trak, fourCC("edts"))) {
        if (auto elst = findBox(edts->body, fourCC("elst"))) {
            Reader edits { elst->body };
            unsigned editVersion = edits.read(1);
            edits.skip(3);
            uint32_t count = edits.read(4);
            for (uint32_t i = 0; i < count && !edits.failed(); ++i) {
                uint64_t duration = edits.read(editVersion == 1 ? 8 : 4);
                int64_t mediaTime = editVersion == 1 ? static_cast<int64_t>(edits.read(8)) : static_cast<int32_t>(edits.read(4));
                edits.skip(4);
                if (mediaTime < 0)
                    continue;
                track.skip = mediaTime;
                if (duration && movieTimescale)
                    track.playedDuration = duration * track.timescale / movieTimescale;
                break;
            }
        }
    }

    auto minf = findBox(mdia->body, fourCC("minf"));
    auto stbl = minf ? findBox(minf->body, fourCC("stbl")) : std::nullopt;
    return stbl && parseSampleTable(stbl->body, track);
}

} // namespace

std::optional<MP4AudioTrack> parseMP4AudioTrack(std::span<const uint8_t> file)
{
    auto ftyp = findBox(file, fourCC("ftyp"));
    auto moov = findBox(file, fourCC("moov"));
    if (!ftyp || !moov)
        return std::nullopt;
    uint32_t movieTimescale = 0;
    if (auto mvhd = findBox(moov->body, fourCC("mvhd"))) {
        Reader header { mvhd->body };
        unsigned version = header.read(1);
        header.skip(3 + (version == 1 ? 16 : 8));
        movieTimescale = header.read(4);
    }
    std::optional<MP4AudioTrack> result;
    forEachBox(moov->body, [&](const Box& box) {
        if (box.type != fourCC("trak"))
            return true;
        MP4AudioTrack track;
        if (!parseTrack(box.body, movieTimescale, track))
            return true;
        result = WTF::move(track);
        return false;
    });
    if (!result)
        return std::nullopt;
    for (auto& sample : result->samples) {
        if (sample.offset > file.size() || sample.size > file.size() - sample.offset)
            return std::nullopt;
    }
    return result;
}

} // namespace WebCore

#endif // PLATFORM(VITA) && (ENABLE(VIDEO) || ENABLE(WEB_AUDIO))
