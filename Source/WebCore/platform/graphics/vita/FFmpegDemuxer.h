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

#include "audio/vita/FFmpegAudioDecoder.h"
#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/WTFString.h>

struct AVBSFContext;
struct AVCodecParameters;
struct AVFormatContext;
struct AVIOContext;
struct AVPacket;

namespace WebCore {

// Splits a media file (MP4 and MOV, fragmented or not, WebM and Matroska, Ogg,
// MPEG-TS) into the packets of its first video and first audio track. H.264
// comes out in Annex B with its parameter sets, as the hardware decoder takes it.
class FFmpegDemuxer {
    WTF_MAKE_NONCOPYABLE(FFmpegDemuxer);
    WTF_MAKE_TZONE_ALLOCATED(FFmpegDemuxer);
public:
    using Input = FFmpegAudioDecoder::Input;

    // On failure, sets FAILURE (when given) to why.
    static std::unique_ptr<FFmpegDemuxer> open(Input&, String* failure = nullptr);
    ~FFmpegDemuxer();

    // Stream indexes, or -1.
    int videoStream() const { return m_videoStream; }
    int audioStream() const { return m_audioStream; }
    const AVCodecParameters* parameters(int stream) const;
    // The parameters of the packets read() returns: for H.264, with the Annex B
    // parameter sets.
    const AVCodecParameters* packetParameters(int stream) const;
    // FFmpeg's name for a stream's codec ("h264", "vp9", "hevc"...).
    String codecName(int stream) const;
    std::optional<Seconds> duration() const { return m_duration; }

    struct Packet {
        int stream { -1 };
        std::span<const uint8_t> data;
        std::optional<Seconds> pts;
        std::optional<Seconds> dts;
        Seconds duration;
        bool keyframe { false };
    };
    enum class Result : uint8_t { Packet, End, Error };
    // The packet's data stays valid until the next read or seek.
    Result read(Packet&);
    // Goes to the keyframe at or before the time.
    bool seek(Seconds);

private:
    FFmpegDemuxer();
    std::optional<Seconds> toSeconds(int stream, int64_t) const;

    AVIOContext* m_io { nullptr };
    AVFormatContext* m_format { nullptr };
    AVBSFContext* m_annexB { nullptr };
    AVPacket* m_packet { nullptr };
    int m_videoStream { -1 };
    int m_audioStream { -1 };
    std::optional<Seconds> m_duration;
    bool m_draining { false };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
