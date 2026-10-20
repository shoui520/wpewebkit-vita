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
#include "FFmpegDemuxer.h"

#if ENABLE(VIDEO) && PLATFORM(VITA)

#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
}

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(FFmpegDemuxer);

static constexpr int ioBufferSize = 64 * 1024;

static int readInput(void* opaque, uint8_t* buffer, int size)
{
    int result = static_cast<FFmpegDemuxer::Input*>(opaque)->read({ buffer, static_cast<size_t>(size) });
    if (!result)
        return AVERROR_EOF;
    return result < 0 ? AVERROR(EIO) : result;
}

static int64_t seekInput(void* opaque, int64_t offset, int whence)
{
    auto& input = *static_cast<FFmpegDemuxer::Input*>(opaque);
    auto size = input.size();
    if (whence & AVSEEK_SIZE)
        return size ? static_cast<int64_t>(*size) : -1;
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

FFmpegDemuxer::FFmpegDemuxer() = default;

FFmpegDemuxer::~FFmpegDemuxer()
{
    av_packet_free(&m_packet);
    av_bsf_free(&m_annexB);
    avformat_close_input(&m_format);
    if (m_io) {
        av_freep(&m_io->buffer);
        avio_context_free(&m_io);
    }
}

std::unique_ptr<FFmpegDemuxer> FFmpegDemuxer::open(Input& input, String* failure)
{
    static bool quiet = (av_log_set_level(AV_LOG_QUIET), true);
    UNUSED_VARIABLE(quiet);

    auto fail = [&](ASCIILiteral reason, int error = 0) -> std::unique_ptr<FFmpegDemuxer> {
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

    std::unique_ptr<FFmpegDemuxer> demuxer { new FFmpegDemuxer };
    auto* buffer = static_cast<uint8_t*>(av_malloc(ioBufferSize));
    if (!buffer)
        return fail("Out of memory"_s);
    demuxer->m_io = avio_alloc_context(buffer, ioBufferSize, 0, &input, readInput, nullptr, seekInput);
    if (!demuxer->m_io) {
        av_free(buffer);
        return fail("Out of memory"_s);
    }
    demuxer->m_format = avformat_alloc_context();
    if (!demuxer->m_format)
        return fail("Out of memory"_s);
    demuxer->m_format->pb = demuxer->m_io;
    demuxer->m_format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (int error = avformat_open_input(&demuxer->m_format, nullptr, nullptr, nullptr); error < 0)
        return fail("The media format was not recognized"_s, error);
    if (int error = avformat_find_stream_info(demuxer->m_format, nullptr); error < 0)
        return fail("The media could not be read"_s, error);

    demuxer->m_videoStream = av_find_best_stream(demuxer->m_format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    demuxer->m_audioStream = av_find_best_stream(demuxer->m_format, AVMEDIA_TYPE_AUDIO, -1, demuxer->m_videoStream, nullptr, 0);
    if (demuxer->m_videoStream < 0)
        demuxer->m_videoStream = -1;
    if (demuxer->m_audioStream < 0)
        demuxer->m_audioStream = -1;
    if (demuxer->m_videoStream < 0 && demuxer->m_audioStream < 0)
        return fail("The media has neither video nor audio"_s);
    for (unsigned i = 0; i < demuxer->m_format->nb_streams; ++i) {
        if (static_cast<int>(i) != demuxer->m_videoStream && static_cast<int>(i) != demuxer->m_audioStream)
            demuxer->m_format->streams[i]->discard = AVDISCARD_ALL;
    }

    if (demuxer->m_videoStream >= 0) {
        auto* stream = demuxer->m_format->streams[demuxer->m_videoStream];
        if (stream->codecpar->codec_id == AV_CODEC_ID_H264) {
            const AVBitStreamFilter* filter = av_bsf_get_by_name("h264_mp4toannexb");
            if (!filter || av_bsf_alloc(filter, &demuxer->m_annexB) < 0)
                return fail("Out of memory"_s);
            avcodec_parameters_copy(demuxer->m_annexB->par_in, stream->codecpar);
            demuxer->m_annexB->time_base_in = stream->time_base;
            if (int error = av_bsf_init(demuxer->m_annexB); error < 0)
                return fail("The H.264 stream could not be read"_s, error);
        }
    }

    if (demuxer->m_format->duration > 0)
        demuxer->m_duration = Seconds { static_cast<double>(demuxer->m_format->duration) / AV_TIME_BASE };
    demuxer->m_packet = av_packet_alloc();
    if (!demuxer->m_packet)
        return fail("Out of memory"_s);
    return demuxer;
}

const AVCodecParameters* FFmpegDemuxer::parameters(int stream) const
{
    return stream >= 0 ? m_format->streams[stream]->codecpar : nullptr;
}

const AVCodecParameters* FFmpegDemuxer::packetParameters(int stream) const
{
    if (stream == m_videoStream && m_annexB)
        return m_annexB->par_out;
    return parameters(stream);
}

String FFmpegDemuxer::codecName(int stream) const
{
    if (stream < 0)
        return { };
    return String::fromLatin1(avcodec_get_name(m_format->streams[stream]->codecpar->codec_id));
}

std::optional<Seconds> FFmpegDemuxer::toSeconds(int stream, int64_t value) const
{
    if (value == AV_NOPTS_VALUE)
        return std::nullopt;
    auto base = m_format->streams[stream]->time_base;
    return Seconds { static_cast<double>(value) * base.num / base.den };
}

auto FFmpegDemuxer::read(Packet& packet) -> Result
{
    av_packet_unref(m_packet);
    while (true) {
        // Converted H.264 waiting in the filter goes first.
        if (m_annexB) {
            int result = av_bsf_receive_packet(m_annexB, m_packet);
            if (!result) {
                m_packet->stream_index = m_videoStream;
                break;
            }
            if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
                return Result::Error;
            if (result == AVERROR_EOF)
                return Result::End;
        }
        if (m_draining)
            return Result::End;
        int error = av_read_frame(m_format, m_packet);
        if (error == AVERROR_EOF) {
            if (!m_annexB)
                return Result::End;
            m_draining = true;
            av_bsf_send_packet(m_annexB, nullptr);
            continue;
        }
        if (error < 0)
            return Result::Error;
        if (m_annexB && m_packet->stream_index == m_videoStream) {
            if (av_bsf_send_packet(m_annexB, m_packet) < 0)
                return Result::Error;
            continue;
        }
        break;
    }
    int stream = m_packet->stream_index;
    packet.stream = stream;
    packet.data = { m_packet->data, static_cast<size_t>(m_packet->size) };
    packet.pts = toSeconds(stream, m_packet->pts);
    packet.dts = toSeconds(stream, m_packet->dts);
    packet.duration = toSeconds(stream, m_packet->duration).value_or(0_s);
    packet.keyframe = m_packet->flags & AV_PKT_FLAG_KEY;
    return Result::Packet;
}

bool FFmpegDemuxer::seek(Seconds time)
{
    av_packet_unref(m_packet);
    int stream = m_videoStream >= 0 ? m_videoStream : m_audioStream;
    auto base = m_format->streams[stream]->time_base;
    int64_t target = static_cast<int64_t>(time.seconds() * base.den / base.num);
    if (av_seek_frame(m_format, stream, target, AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    if (m_annexB)
        av_bsf_flush(m_annexB);
    m_draining = false;
    return true;
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
