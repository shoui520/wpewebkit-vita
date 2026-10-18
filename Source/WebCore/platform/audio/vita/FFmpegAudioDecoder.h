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

#if PLATFORM(VITA) && (ENABLE(VIDEO) || ENABLE(WEB_AUDIO))

#include <memory>
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVIOContext;
struct AVPacket;
struct SwrContext;

namespace WebCore {

// Decodes the audio of a file (MP3, Ogg Vorbis or Opus, WebM audio, FLAC,
// WAV, ADTS AAC) with FFmpeg into planar floats at a chosen rate.
class FFmpegAudioDecoder {
    WTF_MAKE_NONCOPYABLE(FFmpegAudioDecoder);
    WTF_MAKE_TZONE_ALLOCATED(FFmpegAudioDecoder);
public:
    // Where the file's bytes come from; called on the decoding thread.
    class Input {
    public:
        virtual ~Input() = default;
        // The bytes read, 0 at the end, or negative on an error.
        virtual int read(std::span<uint8_t>) = 0;
        virtual bool seek(uint64_t position) = 0;
        virtual std::optional<uint64_t> size() = 0;
    };

    // On failure, sets FAILURE (when given) to why.
    static std::unique_ptr<FFmpegAudioDecoder> open(Input&, String* failure = nullptr);
    // An MP4 (M4A) file in memory, which this FFmpeg cannot demux; it
    // stays in use while decoding. No seeking.
    static std::unique_ptr<FFmpegAudioDecoder> openMP4(std::span<const uint8_t>);
    ~FFmpegAudioDecoder();

    unsigned channelCount() const { return m_channelCount; }
    int sampleRate() const { return m_sampleRate; }
    std::optional<Seconds> duration() const { return m_duration; }

    // The output; channels other than the file's are mixed.
    void setOutput(unsigned channels, int sampleRate);

    enum class Result : uint8_t { Decoded, End, Error };
    // Appends the samples of the next packet to each channel's vector.
    Result decode(Vector<Vector<float>>& channels);

    // Restarts at or before the time; the next samples decoded start at the
    // returned time.
    std::optional<Seconds> seek(Seconds);

private:
    FFmpegAudioDecoder();

    struct MP4Input;
    Result decodeMP4(Vector<Vector<float>>&);
    bool convert(const AVFrame*, Vector<Vector<float>>&);
    bool receiveFrames(Vector<Vector<float>>&);

    AVIOContext* m_io { nullptr };
    AVFormatContext* m_format { nullptr };
    AVCodecContext* m_codec { nullptr };
    SwrContext* m_resampler { nullptr };
    AVPacket* m_packet { nullptr };
    AVFrame* m_frame { nullptr };
    int m_stream { -1 };
    unsigned m_channelCount { 0 };
    int m_sampleRate { 0 };
    std::optional<Seconds> m_duration;
    unsigned m_outputChannels { 0 };
    int m_outputRate { 0 };
    // The format the resampler converts from.
    int m_resamplerFormat { -1 };
    int m_resamplerRate { 0 };
    int m_resamplerChannels { 0 };
    bool m_draining { false };
    std::unique_ptr<MP4Input> m_mp4;
};

// Input from bytes in memory.
class MemoryAudioInput final : public FFmpegAudioDecoder::Input {
public:
    explicit MemoryAudioInput(std::span<const uint8_t> data)
        : m_data(data)
    {
    }

    int read(std::span<uint8_t>) final;
    bool seek(uint64_t position) final;
    std::optional<uint64_t> size() final { return m_data.size(); }

private:
    std::span<const uint8_t> m_data;
    size_t m_position { 0 };
};

} // namespace WebCore

#endif // PLATFORM(VITA) && (ENABLE(VIDEO) || ENABLE(WEB_AUDIO))
