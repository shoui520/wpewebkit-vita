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
#include "FFmpegAudioDecoder.h"

#if PLATFORM(VITA) && (ENABLE(VIDEO) || ENABLE(WEB_AUDIO))

#include "MP4AudioParser.h"
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FFmpegAudioDecoder);

struct FFmpegAudioDecoder::MP4Input {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(MP4Input);
    std::span<const uint8_t> file;
    MP4AudioTrack track;
    size_t nextSample { 0 };
    bool started { false };
    // In output frames: the encoder's delay, and what the edit list plays after it.
    uint64_t skip { 0 };
    std::optional<uint64_t> remaining;
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(FFmpegAudioDecoder::MP4Input);

FFmpegAudioDecoder::FFmpegAudioDecoder() = default;

static constexpr int ioBufferSize = 32 * 1024;

static int readInput(void* opaque, uint8_t* buffer, int size)
{
    int result = static_cast<FFmpegAudioDecoder::Input*>(opaque)->read({ buffer, static_cast<size_t>(size) });
    if (!result)
        return AVERROR_EOF;
    return result < 0 ? AVERROR(EIO) : result;
}

static int64_t seekInput(void* opaque, int64_t offset, int whence)
{
    auto& input = *static_cast<FFmpegAudioDecoder::Input*>(opaque);
    auto size = input.size();
    if (whence & AVSEEK_SIZE)
        return size ? static_cast<int64_t>(*size) : -1;
    // FFmpeg asks for absolute positions or ones from the end.
    whence &= ~AVSEEK_FORCE;
    int64_t target = offset;
    if (whence == SEEK_END) {
        if (!size)
            return -1;
        target += *size;
    } else if (whence != SEEK_SET)
        return -1;
    if (target < 0 || !input.seek(target))
        return -1;
    return target;
}

std::unique_ptr<FFmpegAudioDecoder> FFmpegAudioDecoder::open(Input& input, String* failure)
{
    static bool quiet = (av_log_set_level(AV_LOG_QUIET), true);
    UNUSED_VARIABLE(quiet);

    auto fail = [&](ASCIILiteral reason, int error = 0) -> std::unique_ptr<FFmpegAudioDecoder> {
        if (failure) {
            if (error) {
                std::array<char, AV_ERROR_MAX_STRING_SIZE> text { };
                av_strerror(error, text.data(), text.size());
                *failure = makeString(reason, " ("_s, String::fromLatin1(text.data()), ')');
            } else
                *failure = reason;
        }
        return nullptr;
    };

    std::unique_ptr<FFmpegAudioDecoder> decoder { new FFmpegAudioDecoder };
    auto* buffer = static_cast<uint8_t*>(av_malloc(ioBufferSize));
    if (!buffer)
        return fail("Out of memory"_s);
    decoder->m_io = avio_alloc_context(buffer, ioBufferSize, 0, &input, readInput, nullptr, seekInput);
    if (!decoder->m_io) {
        av_free(buffer);
        return fail("Out of memory"_s);
    }
    decoder->m_format = avformat_alloc_context();
    if (!decoder->m_format)
        return fail("Out of memory"_s);
    decoder->m_format->pb = decoder->m_io;
    decoder->m_format->flags |= AVFMT_FLAG_CUSTOM_IO;
    // On failure the context is freed and the pointer cleared.
    if (int error = avformat_open_input(&decoder->m_format, nullptr, nullptr, nullptr); error < 0)
        return fail("The audio format was not recognized"_s, error);
    if (int error = avformat_find_stream_info(decoder->m_format, nullptr); error < 0)
        return fail("The audio could not be read"_s, error);

    const AVCodec* codec = nullptr;
    decoder->m_stream = av_find_best_stream(decoder->m_format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (decoder->m_stream < 0 || !codec)
        return fail("There is no audio this browser can decode"_s);
    auto* stream = decoder->m_format->streams[decoder->m_stream];
    decoder->m_codec = avcodec_alloc_context3(codec);
    if (!decoder->m_codec || avcodec_parameters_to_context(decoder->m_codec, stream->codecpar) < 0)
        return fail("Out of memory"_s);
    decoder->m_codec->pkt_timebase = stream->time_base;
    if (int error = avcodec_open2(decoder->m_codec, codec, nullptr); error < 0)
        return fail("The audio decoder could not start"_s, error);
    // Only the chosen stream is read.
    for (unsigned i = 0; i < decoder->m_format->nb_streams; ++i) {
        if (static_cast<int>(i) != decoder->m_stream)
            decoder->m_format->streams[i]->discard = AVDISCARD_ALL;
    }

    decoder->m_channelCount = decoder->m_codec->ch_layout.nb_channels;
    decoder->m_sampleRate = decoder->m_codec->sample_rate;
    if (!decoder->m_channelCount || decoder->m_sampleRate <= 0)
        return fail("The audio has no channels or sample rate"_s);
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
        decoder->m_duration = Seconds { stream->duration * av_q2d(stream->time_base) };
    else if (decoder->m_format->duration != AV_NOPTS_VALUE && decoder->m_format->duration > 0)
        decoder->m_duration = Seconds { static_cast<double>(decoder->m_format->duration) / AV_TIME_BASE };

    decoder->m_packet = av_packet_alloc();
    decoder->m_frame = av_frame_alloc();
    if (!decoder->m_packet || !decoder->m_frame)
        return fail("Out of memory"_s);
    decoder->setOutput(decoder->m_channelCount, decoder->m_sampleRate);
    return decoder;
}

std::unique_ptr<FFmpegAudioDecoder> FFmpegAudioDecoder::openMP4(std::span<const uint8_t> file)
{
    auto track = parseMP4AudioTrack(file);
    if (!track)
        return nullptr;
    AVCodecID codecID;
    switch (track->objectType) {
    case 0x40:
    case 0x66:
    case 0x67:
    case 0x68:
        codecID = AV_CODEC_ID_AAC;
        break;
    case 0x69:
    case 0x6B:
        codecID = AV_CODEC_ID_MP3;
        break;
    default:
        return nullptr;
    }
    const AVCodec* codec = avcodec_find_decoder(codecID);
    if (!codec)
        return nullptr;

    std::unique_ptr<FFmpegAudioDecoder> decoder { new FFmpegAudioDecoder };
    decoder->m_codec = avcodec_alloc_context3(codec);
    if (!decoder->m_codec)
        return nullptr;
    decoder->m_codec->sample_rate = track->sampleRate;
    av_channel_layout_default(&decoder->m_codec->ch_layout, track->channels ? track->channels : 2);
    if (!track->decoderConfig.isEmpty()) {
        size_t size = track->decoderConfig.size();
        auto* extradata = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!extradata)
            return nullptr;
        memcpySpan(std::span { extradata, size }, track->decoderConfig.span());
        decoder->m_codec->extradata = extradata;
        decoder->m_codec->extradata_size = size;
    }
    if (avcodec_open2(decoder->m_codec, codec, nullptr) < 0)
        return nullptr;
    decoder->m_channelCount = decoder->m_codec->ch_layout.nb_channels;
    decoder->m_sampleRate = decoder->m_codec->sample_rate;
    if (!decoder->m_channelCount || decoder->m_sampleRate <= 0 || !track->timescale)
        return nullptr;
    if (track->playedDuration)
        decoder->m_duration = Seconds { static_cast<double>(*track->playedDuration) / track->timescale };

    decoder->m_packet = av_packet_alloc();
    decoder->m_frame = av_frame_alloc();
    if (!decoder->m_packet || !decoder->m_frame)
        return nullptr;
    decoder->m_mp4 = makeUnique<MP4Input>();
    decoder->m_mp4->file = file;
    decoder->m_mp4->track = WTF::move(*track);
    decoder->setOutput(decoder->m_channelCount, decoder->m_sampleRate);
    return decoder;
}

FFmpegAudioDecoder::~FFmpegAudioDecoder()
{
    av_frame_free(&m_frame);
    av_packet_free(&m_packet);
    swr_free(&m_resampler);
    avcodec_free_context(&m_codec);
    avformat_close_input(&m_format);
    if (m_io) {
        av_freep(&m_io->buffer);
        avio_context_free(&m_io);
    }
}

void FFmpegAudioDecoder::setOutput(unsigned channels, int sampleRate)
{
    if (channels == m_outputChannels && sampleRate == m_outputRate)
        return;
    m_outputChannels = channels;
    m_outputRate = sampleRate;
    swr_free(&m_resampler);
    m_resamplerFormat = -1;
}

bool FFmpegAudioDecoder::convert(const AVFrame* frame, Vector<Vector<float>>& channels)
{
    if (channels.size() != m_outputChannels)
        channels.resize(m_outputChannels);
    if (frame) {
        int frameChannels = frame->ch_layout.nb_channels;
        if (!m_resampler || frame->format != m_resamplerFormat || frame->sample_rate != m_resamplerRate || frameChannels != m_resamplerChannels) {
            swr_free(&m_resampler);
            AVChannelLayout input { };
            if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_default(&input, frameChannels);
            else if (av_channel_layout_copy(&input, &frame->ch_layout) < 0)
                return false;
            AVChannelLayout output { };
            av_channel_layout_default(&output, m_outputChannels);
            int result = swr_alloc_set_opts2(&m_resampler, &output, AV_SAMPLE_FMT_FLTP, m_outputRate,
                &input, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&input);
            av_channel_layout_uninit(&output);
            if (result < 0 || swr_init(m_resampler) < 0) {
                swr_free(&m_resampler);
                return false;
            }
            m_resamplerFormat = frame->format;
            m_resamplerRate = frame->sample_rate;
            m_resamplerChannels = frameChannels;
        }
    } else if (!m_resampler)
        return true;

    int room = swr_get_out_samples(m_resampler, frame ? frame->nb_samples : 0);
    if (room <= 0)
        return true;
    size_t start = channels[0].size();
    Vector<uint8_t*, 8> planes;
    for (auto& channel : channels) {
        channel.grow(start + room);
        planes.append(reinterpret_cast<uint8_t*>(channel.mutableSpan().subspan(start).data()));
    }
    int converted = swr_convert(m_resampler, planes.span().data(), room,
        frame ? frame->extended_data : nullptr, frame ? frame->nb_samples : 0);
    for (auto& channel : channels)
        channel.shrink(start + std::max(converted, 0));
    return converted >= 0;
}

bool FFmpegAudioDecoder::receiveFrames(Vector<Vector<float>>& channels)
{
    while (true) {
        int result = avcodec_receive_frame(m_codec, m_frame);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return true;
        if (result < 0)
            return false;
        bool converted = convert(m_frame, channels);
        av_frame_unref(m_frame);
        if (!converted)
            return false;
    }
}

FFmpegAudioDecoder::Result FFmpegAudioDecoder::decodeMP4(Vector<Vector<float>>& channels)
{
    auto& input = *m_mp4;
    if (!std::exchange(input.started, true)) {
        auto toOutput = [&](uint64_t time) {
            return time * m_outputRate / input.track.timescale;
        };
        input.skip = toOutput(input.track.skip);
        if (input.track.playedDuration)
            input.remaining = toOutput(*input.track.playedDuration);
    }
    if (channels.size() != m_outputChannels)
        channels.resize(m_outputChannels);
    size_t before = channels[0].size();
    while (!m_draining) {
        if (input.nextSample < input.track.samples.size()) {
            auto& sample = input.track.samples[input.nextSample++];
            if (av_new_packet(m_packet, sample.size) < 0)
                return Result::Error;
            memcpySpan(std::span { m_packet->data, sample.size }, input.file.subspan(sample.offset, sample.size));
            int result;
            while ((result = avcodec_send_packet(m_codec, m_packet)) == AVERROR(EAGAIN)) {
                if (!receiveFrames(channels)) {
                    av_packet_unref(m_packet);
                    return Result::Error;
                }
            }
            av_packet_unref(m_packet);
            // A damaged packet is skipped.
            if (result < 0 && result != AVERROR_INVALIDDATA)
                return Result::Error;
            if (!receiveFrames(channels))
                return Result::Error;
        } else {
            m_draining = true;
            avcodec_send_packet(m_codec, nullptr);
            if (!receiveFrames(channels) || !convert(nullptr, channels))
                return Result::Error;
        }

        // The encoder's delay goes, and what plays after the edit.
        size_t produced = channels[0].size() - before;
        if (size_t drop = std::min<uint64_t>(input.skip, produced)) {
            for (auto& channel : channels)
                channel.removeAt(before, drop);
            input.skip -= drop;
            produced -= drop;
        }
        if (input.remaining) {
            size_t keep = std::min<uint64_t>(produced, *input.remaining);
            for (auto& channel : channels)
                channel.shrink(before + keep);
            *input.remaining -= keep;
            produced = keep;
            if (!*input.remaining)
                m_draining = true;
        }
        if (produced)
            return Result::Decoded;
    }
    return Result::End;
}

FFmpegAudioDecoder::Result FFmpegAudioDecoder::decode(Vector<Vector<float>>& channels)
{
    if (m_mp4)
        return decodeMP4(channels);
    if (m_draining)
        return Result::End;
    auto decodedSize = [&] -> size_t {
        return channels.isEmpty() ? 0 : channels[0].size();
    };
    size_t before = decodedSize();
    // Frames left from a packet sent before.
    if (!receiveFrames(channels))
        return Result::Error;
    if (decodedSize() > before)
        return Result::Decoded;
    while (true) {
        int result = av_read_frame(m_format, m_packet);
        if (result < 0) {
            if (result != AVERROR_EOF && !avio_feof(m_format->pb))
                return Result::Error;
            // The decoder's and resampler's last samples.
            m_draining = true;
            avcodec_send_packet(m_codec, nullptr);
            if (!receiveFrames(channels) || !convert(nullptr, channels))
                return Result::Error;
            return decodedSize() > before ? Result::Decoded : Result::End;
        }
        if (m_packet->stream_index != m_stream) {
            av_packet_unref(m_packet);
            continue;
        }
        while ((result = avcodec_send_packet(m_codec, m_packet)) == AVERROR(EAGAIN)) {
            if (!receiveFrames(channels)) {
                av_packet_unref(m_packet);
                return Result::Error;
            }
        }
        av_packet_unref(m_packet);
        // A damaged packet is skipped.
        if (result < 0 && result != AVERROR_INVALIDDATA)
            return Result::Error;
        if (!receiveFrames(channels))
            return Result::Error;
        if (decodedSize() > before)
            return Result::Decoded;
    }
}

std::optional<Seconds> FFmpegAudioDecoder::seek(Seconds time)
{
    if (m_mp4)
        return std::nullopt;
    auto* stream = m_format->streams[m_stream];
    int64_t timestamp = static_cast<int64_t>(time.seconds() / av_q2d(stream->time_base));
    if (stream->start_time != AV_NOPTS_VALUE)
        timestamp += stream->start_time;
    if (av_seek_frame(m_format, m_stream, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        // Without an index, from the start.
        if (av_seek_frame(m_format, m_stream, stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_BYTE) < 0)
            return std::nullopt;
        timestamp = 0;
    }
    avcodec_flush_buffers(m_codec);
    if (m_resampler)
        swr_init(m_resampler);
    m_draining = false;
    // Where the next packet starts, when the demuxer knows.
    int result = av_read_frame(m_format, m_packet);
    if (result < 0)
        return time;
    Seconds start = time;
    if (m_packet->pts != AV_NOPTS_VALUE) {
        int64_t pts = m_packet->pts - (stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0);
        start = Seconds { std::max<int64_t>(pts, 0) * av_q2d(stream->time_base) };
    }
    if (m_packet->stream_index == m_stream)
        avcodec_send_packet(m_codec, m_packet);
    av_packet_unref(m_packet);
    return start;
}

int MemoryAudioInput::read(std::span<uint8_t> buffer)
{
    size_t count = std::min(buffer.size(), m_data.size() - m_position);
    memcpySpan(buffer.first(count), m_data.subspan(m_position, count));
    m_position += count;
    return count;
}

bool MemoryAudioInput::seek(uint64_t position)
{
    if (position > m_data.size())
        return false;
    m_position = position;
    return true;
}

} // namespace WebCore

#endif // PLATFORM(VITA) && (ENABLE(VIDEO) || ENABLE(WEB_AUDIO))
