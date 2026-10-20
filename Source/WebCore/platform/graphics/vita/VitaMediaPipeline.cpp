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
#include "VitaMediaPipeline.h"

#if ENABLE(VIDEO) && PLATFORM(VITA)

#include "FFmpegDemuxer.h"
#include "MediaByteStream.h"
#include <psp2/audioout.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

namespace WebCore {

// Packets read ahead of the decoders: enough for a few seconds of either kind
// even where the file interleaves them in long runs.
static constexpr size_t maximumPacketBytes = 6 * MB;
static constexpr size_t wantedVideoPackets = 30;
static constexpr size_t wantedAudioPackets = 100;
// Audio goes out at 48 kHz stereo in blocks of this many samples (21 ms).
static constexpr unsigned audioRate = 48000;
static constexpr unsigned audioBlock = 1024;
// A picture shows when the clock is this close to its time.
static constexpr Seconds presentationTolerance = 5_ms;
// After a seek, pictures more than this before the target are not shown.
static constexpr Seconds seekSlack = 50_ms;

class StreamInput final : public FFmpegAudioDecoder::Input {
public:
    explicit StreamInput(MediaByteStream& stream)
        : m_stream(stream)
    {
    }

    int read(std::span<uint8_t> buffer) final
    {
        int result = m_stream->read(buffer.data(), m_position, buffer.size());
        if (result > 0)
            m_position += result;
        return result;
    }

    bool seek(uint64_t position) final
    {
        m_position = position;
        return true;
    }

    std::optional<uint64_t> size() final
    {
        uint64_t size = m_stream->size();
        if (!size)
            return std::nullopt;
        return size;
    }

private:
    const Ref<MediaByteStream> m_stream;
    uint64_t m_position { 0 };
};

Ref<VitaMediaPipeline> VitaMediaPipeline::create(Ref<MediaByteStream>&& stream, EventHandler&& handler)
{
    return adoptRef(*new VitaMediaPipeline(WTF::move(stream), WTF::move(handler)));
}

VitaMediaPipeline::VitaMediaPipeline(Ref<MediaByteStream>&& stream, EventHandler&& handler)
    : m_stream(WTF::move(stream))
    , m_handler(WTF::move(handler))
    , m_volume(SCE_AUDIO_VOLUME_0DB)
{
}

VitaMediaPipeline::~VitaMediaPipeline()
{
    stop();
}

void VitaMediaPipeline::start(Seconds start)
{
    ASSERT(isMainThread());
    {
        Locker locker { m_lock };
        // Like a seek: the clock waits at the start for the first picture or sound.
        m_seekTarget = start;
        m_seekPending = true;
        m_seekDonePosted = false;
        m_clockTime = start;
    }
    m_reader = Thread::create("Media reader"_s, [protectedThis = Ref { *this }] {
        protectedThis->readerThread();
    });
}

void VitaMediaPipeline::stop()
{
    m_stopped = true;
    {
        Locker locker { m_lock };
        if (m_closing)
            return;
        m_closing = true;
        m_condition.notifyAll();
    }
    // Reads waiting for data give up.
    m_stream->close();
    if (RefPtr reader = std::exchange(m_reader, nullptr))
        reader->waitForCompletion();
    // The reader made these.
    if (RefPtr video = std::exchange(m_video, nullptr))
        video->waitForCompletion();
    if (RefPtr audio = std::exchange(m_audio, nullptr))
        audio->waitForCompletion();
    {
        Locker locker { m_lock };
        // Pictures go back to the decoder before it goes.
        m_frames.clear();
        m_videoPackets.clear();
        m_audioPackets.clear();
    }
    m_videoDecoder = nullptr;
    avcodec_free_context(&m_audioCodec);
    m_demuxer = nullptr;
    m_input = nullptr;
}

String VitaMediaPipeline::failure() const
{
    Locker locker { m_lock };
    return m_failure;
}

void VitaMediaPipeline::post(Event event)
{
    callOnMainThread([protectedThis = Ref { *this }, event] {
        if (!protectedThis->m_stopped && protectedThis->m_handler)
            protectedThis->m_handler(event);
    });
}

void VitaMediaPipeline::fail(String&& message)
{
    {
        Locker locker { m_lock };
        if (!m_failure.isNull())
            return;
        m_failure = WTF::move(message);
    }
    post(Event::Failed);
}

bool VitaMediaPipeline::open()
{
    m_input = std::make_unique<StreamInput>(m_stream.get());
    String failure;
    m_demuxer = FFmpegDemuxer::open(*m_input, &failure);
    if (!m_demuxer) {
        // A failed load explains more than what the demuxer made of the bytes.
        auto loadFailure = m_stream->failure();
        fail(loadFailure.isNull() ? WTF::move(failure) : WTF::move(loadFailure));
        return false;
    }
    if (int stream = m_demuxer->videoStream(); stream >= 0) {
        auto& parameters = *m_demuxer->packetParameters(stream);
        m_videoDecoder = VitaVideoDecoder::create(parameters, failure);
        if (!m_videoDecoder) {
            fail(WTF::move(failure));
            return false;
        }
        m_info.hasVideo = true;
        m_info.width = parameters.width;
        m_info.height = parameters.height;
        if (parameters.sample_aspect_ratio.num > 0 && parameters.sample_aspect_ratio.den > 0)
            m_info.pixelAspectRatio = static_cast<float>(parameters.sample_aspect_ratio.num) / parameters.sample_aspect_ratio.den;
        m_info.videoDecoder = m_videoDecoder->name();
    }
    if (int stream = m_demuxer->audioStream(); stream >= 0) {
        auto& parameters = *m_demuxer->parameters(stream);
        if (const AVCodec* codec = avcodec_find_decoder(parameters.codec_id)) {
            m_audioCodec = avcodec_alloc_context3(codec);
            if (m_audioCodec && avcodec_parameters_to_context(m_audioCodec, &parameters) >= 0) {
                m_audioCodec->pkt_timebase = { 1, 1000000 };
                if (avcodec_open2(m_audioCodec, codec, nullptr) >= 0)
                    m_info.hasAudio = true;
            }
            if (!m_info.hasAudio)
                avcodec_free_context(&m_audioCodec);
        }
        if (!m_info.hasAudio && !m_info.hasVideo) {
            fail(makeString("No decoder for "_s, m_demuxer->codecName(stream), " audio"_s));
            return false;
        }
    }
    m_info.duration = m_demuxer->duration();
    return true;
}

bool VitaMediaPipeline::packetsWanted() const
{
    if (m_endOfPackets || m_packetBytes >= maximumPacketBytes)
        return false;
    return (m_info.hasVideo && m_videoPackets.size() < wantedVideoPackets) || (m_info.hasAudio && m_audioPackets.size() < wantedAudioPackets);
}

void VitaMediaPipeline::readerThread()
{
    if (!open())
        return;
    if (m_info.hasVideo) {
        m_video = Thread::create("Media video"_s, [protectedThis = Ref { *this }] {
            protectedThis->videoThread();
        });
    }
    if (m_info.hasAudio) {
        m_audio = Thread::create("Media audio"_s, [protectedThis = Ref { *this }] {
            protectedThis->audioThread();
        }, ThreadType::Audio, Thread::QOS::UserInteractive);
    }
    post(Event::Ready);

    // The generation the demuxer is placed for; a new one moves it.
    std::optional<unsigned> placed;
    FFmpegDemuxer::Packet packet;
    while (true) {
        unsigned generation;
        Seconds target;
        {
            Locker locker { m_lock };
            while (!m_closing && placed == m_generation && !packetsWanted())
                m_condition.wait(m_lock);
            if (m_closing)
                return;
            generation = m_generation;
            target = m_seekTarget;
        }
        if (placed != generation) {
            if (placed || target > 0_s)
                m_demuxer->seek(target);
            placed = generation;
            Locker locker { m_lock };
            if (m_generation == generation)
                m_endOfPackets = false;
            continue;
        }

        auto result = m_demuxer->read(packet);
        Locker locker { m_lock };
        if (m_generation != generation)
            continue;
        if (result != FFmpegDemuxer::Result::Packet) {
            m_endOfPackets = true;
            m_condition.notifyAll();
            continue;
        }
        bool video = packet.stream == m_demuxer->videoStream();
        if (!video && packet.stream != m_demuxer->audioStream())
            continue;
        Packet queued;
        // FFmpeg's decoders read past the end of a packet.
        queued.data.reserveInitialCapacity(packet.data.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        queued.data.append(packet.data);
        queued.data.grow(packet.data.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        queued.data.shrink(packet.data.size());
        queued.presentationTime = packet.pts;
        queued.decodeTime = packet.dts;
        queued.generation = generation;
        m_packetBytes += packet.data.size();
        (video ? m_videoPackets : m_audioPackets).append(WTF::move(queued));
        m_condition.notifyAll();
    }
}

void VitaMediaPipeline::videoThread()
{
    auto& decoder = *m_videoDecoder;
    unsigned generation;
    {
        Locker locker { m_lock };
        generation = m_generation;
    }
    VitaVideoDecoder::Output output = [&](VitaVideoFrame&& frame) {
        Locker locker { m_lock };
        if (m_generation != generation || frame.time < m_seekTarget - seekSlack)
            return;
        // In display order already; a stream with odd timestamps still shows in time order.
        size_t index = m_frames.size();
        while (index && m_frames[index - 1].time > frame.time)
            --index;
        if (index == m_frames.size())
            m_frames.append(WTF::move(frame));
        else
            m_frames.insert(index, WTF::move(frame));
        if (m_seekPending) {
            m_seekPending = false;
            m_clockStarted = MonotonicTime::now();
            post(Event::SeekDone);
        }
        m_condition.notifyAll();
    };

    unsigned maximumHeld = decoder.maximumHeldPictures();
    while (true) {
        Packet packet;
        bool drain = false;
        {
            Locker locker { m_lock };
            auto canDecode = [&] {
                if (m_closing || m_generation != generation)
                    return true;
                if (m_frames.size() + m_presenting > maximumHeld)
                    return false;
                return !m_videoPackets.isEmpty() || (m_endOfPackets && !m_videoEnded);
            };
            while (!canDecode())
                m_condition.wait(m_lock);
            if (m_closing)
                return;
            if (m_generation != generation) {
                generation = m_generation;
                DropLockForScope unlocker { locker };
                decoder.flush();
                continue;
            }
            if (!m_videoPackets.isEmpty()) {
                packet = m_videoPackets.takeFirst();
                m_packetBytes -= packet.data.size();
                m_condition.notifyAll();
                if (packet.generation != generation)
                    continue;
            } else
                drain = true;
        }
        if (drain) {
            // Goes on once pictures are given back.
            if (decoder.drain(output))
                continue;
            Locker locker { m_lock };
            if (m_generation != generation)
                continue;
            m_videoEnded = true;
            if (m_seekPending) {
                // A seek past the last picture.
                m_seekPending = false;
                m_clockStarted = MonotonicTime::now();
                post(Event::SeekDone);
            }
            checkEnded();
            continue;
        }
        if (!decoder.decode(packet.data.span(), packet.presentationTime, packet.decodeTime, output)) {
            fail(makeString("The video could not be decoded ("_s, decoder.name(), ')'));
            return;
        }
    }
}

void VitaMediaPipeline::audioThread()
{
    AVCodecContext* codec = m_audioCodec;
    SwrContext* resampler = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, audioBlock, audioRate, SCE_AUDIO_OUT_MODE_STEREO);
    int portVolume = -1;

    // Decoded samples waiting to be played, interleaved, and the time of the first.
    Vector<int16_t> pending;
    Seconds pendingTime;
    unsigned generation;
    Seconds target;
    bool trimming = true;
    {
        Locker locker { m_lock };
        generation = m_generation;
        target = m_seekTarget;
    }

    auto append = [&](AVFrame* decoded) {
        if (!resampler) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, audioRate, &decoded->ch_layout,
                static_cast<AVSampleFormat>(decoded->format), decoded->sample_rate, 0, nullptr) < 0 || swr_init(resampler) < 0) {
                swr_free(&resampler);
                return;
            }
        }
        int room = swr_get_out_samples(resampler, decoded->nb_samples);
        if (room <= 0)
            return;
        size_t old = pending.size();
        pending.grow(old + room * 2);
        uint8_t* out = reinterpret_cast<uint8_t*>(pending.mutableSpan().data() + old);
        int converted = swr_convert(resampler, &out, room, const_cast<const uint8_t**>(decoded->extended_data), decoded->nb_samples);
        pending.shrink(old + std::max(converted, 0) * 2);
        if (!old) {
            int64_t time = decoded->best_effort_timestamp != AV_NOPTS_VALUE ? decoded->best_effort_timestamp : decoded->pts;
            if (time != AV_NOPTS_VALUE)
                pendingTime = Seconds { time / 1000000.0 };
        }
        if (trimming && !pending.isEmpty()) {
            // After a seek, the samples before the target are not played.
            if (pendingTime < target) {
                size_t skip = std::min<size_t>(pending.size() / 2, static_cast<size_t>((target - pendingTime).seconds() * audioRate));
                pending.removeAt(0, skip * 2);
                pendingTime += Seconds { static_cast<double>(skip) / audioRate };
            }
            if (!pending.isEmpty())
                trimming = false;
        }
    };

    auto decodePacket = [&](Packet& queued) {
        av_packet_unref(packet);
        if (av_new_packet(packet, queued.data.size()) < 0)
            return;
        memcpy(packet->data, queued.data.span().data(), queued.data.size());
        packet->pts = queued.presentationTime ? static_cast<int64_t>(queued.presentationTime->seconds() * 1000000) : AV_NOPTS_VALUE;
        packet->dts = queued.decodeTime ? static_cast<int64_t>(queued.decodeTime->seconds() * 1000000) : AV_NOPTS_VALUE;
        if (avcodec_send_packet(codec, packet) < 0)
            return;
        while (avcodec_receive_frame(codec, frame) >= 0) {
            append(frame);
            av_frame_unref(frame);
        }
    };

    while (true) {
        Locker locker { m_lock };
        if (m_closing)
            break;
        if (m_generation != generation) {
            generation = m_generation;
            target = m_seekTarget;
            trimming = true;
            pending.clear();
            avcodec_flush_buffers(codec);
            if (resampler)
                swr_init(resampler);
            continue;
        }
        size_t samples = pending.size() / 2;
        if (!m_info.hasVideo && m_seekPending && !trimming && samples >= audioBlock) {
            m_seekPending = false;
            post(Event::SeekDone);
        }
        if (m_playing && samples >= audioBlock && port >= 0) {
            {
                DropLockForScope unlocker { locker };
                if (int volume = m_volume; volume != portVolume) {
                    int volumes[2] = { volume, volume };
                    sceAudioOutSetVolume(port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volumes);
                    portVolume = volume;
                }
                // Blocks until the port takes the block: this paces the thread.
                sceAudioOutOutput(port, pending.span().data());
            }
            if (m_generation != generation)
                continue;
            Seconds blockEnd = pendingTime + Seconds { static_cast<double>(audioBlock) / audioRate };
            int rest = std::max(sceAudioOutGetRestSample(port), 0);
            m_audioClock = blockEnd - Seconds { static_cast<double>(rest) / audioRate };
            m_audioWrittenEnd = blockEnd;
            m_audioClockStamp = MonotonicTime::now();
            m_audioClockValid = m_playing;
            pending.removeAt(0, audioBlock * 2);
            pendingTime = blockEnd;
            continue;
        }
        if (samples < 4 * audioBlock && !m_audioPackets.isEmpty()) {
            auto queued = m_audioPackets.takeFirst();
            m_packetBytes -= queued.data.size();
            m_condition.notifyAll();
            if (queued.generation != generation)
                continue;
            DropLockForScope unlocker { locker };
            decodePacket(queued);
            continue;
        }
        if (samples < audioBlock && m_endOfPackets && m_audioPackets.isEmpty() && !m_audioEnded) {
            // The end: the clock goes on without the audio, from where it ended.
            m_clockTime = m_audioClockValid ? m_audioWrittenEnd : currentTimeLocked();
            m_clockStarted = MonotonicTime::now();
            m_audioClockValid = false;
            m_audioEnded = true;
            if (m_seekPending && !m_info.hasVideo) {
                m_seekPending = false;
                post(Event::SeekDone);
            }
            checkEnded();
            m_condition.notifyAll();
            continue;
        }
        m_condition.wait(m_lock);
    }

    if (port >= 0) {
        sceAudioOutOutput(port, nullptr);
        sceAudioOutReleasePort(port);
    }
    swr_free(&resampler);
    av_frame_free(&frame);
    av_packet_free(&packet);
}

void VitaMediaPipeline::checkEnded()
{
    if (m_endedPosted || m_seekPending || !m_endOfPackets)
        return;
    bool videoDone = !m_info.hasVideo || (m_videoEnded && m_frames.isEmpty() && !m_presenting);
    bool audioDone = !m_info.hasAudio || m_audioEnded;
    if (!videoDone || !audioDone)
        return;
    m_endedPosted = true;
    post(Event::Ended);
}

Seconds VitaMediaPipeline::currentTimeLocked() const
{
    if (m_seekPending)
        return m_seekTarget;
    auto now = MonotonicTime::now();
    if (m_info.hasAudio && !m_audioEnded) {
        if (!m_audioClockValid)
            return m_clockTime;
        Seconds elapsed = m_playing ? now - m_audioClockStamp : 0_s;
        return m_audioClock + std::min(elapsed, m_audioWrittenEnd - m_audioClock);
    }
    return m_playing ? m_clockTime + (now - m_clockStarted) : m_clockTime;
}

Seconds VitaMediaPipeline::currentTime() const
{
    Locker locker { m_lock };
    return currentTimeLocked();
}

void VitaMediaPipeline::play()
{
    Locker locker { m_lock };
    if (m_playing)
        return;
    m_clockTime = currentTimeLocked();
    m_clockStarted = MonotonicTime::now();
    m_playing = true;
    m_condition.notifyAll();
}

void VitaMediaPipeline::pause()
{
    Locker locker { m_lock };
    if (!m_playing)
        return;
    m_clockTime = currentTimeLocked();
    m_audioClockValid = false;
    m_playing = false;
    m_condition.notifyAll();
}

void VitaMediaPipeline::seek(Seconds time)
{
    Locker locker { m_lock };
    ++m_generation;
    m_seekTarget = time;
    m_seekPending = true;
    m_frames.clear();
    m_videoPackets.clear();
    m_audioPackets.clear();
    m_packetBytes = 0;
    m_endOfPackets = false;
    m_shownSinceSeek = false;
    m_videoEnded = false;
    m_audioEnded = false;
    m_endedPosted = false;
    m_clockTime = time;
    m_clockStarted = MonotonicTime::now();
    m_audioClockValid = false;
    m_condition.notifyAll();
}

void VitaMediaPipeline::setVolume(float volume)
{
    m_volume = std::clamp(static_cast<int>(volume * SCE_AUDIO_VOLUME_0DB), 0, SCE_AUDIO_VOLUME_0DB);
}

bool VitaMediaPipeline::presentFrame(const Function<void(const VitaVideoFrame&)>& paint)
{
    Locker locker { m_lock };
    if (m_frames.isEmpty()) {
        checkEnded();
        return false;
    }
    if (m_shownSinceSeek) {
        Seconds now = currentTimeLocked();
        // Pictures whose successor is due already are late: skipped.
        while (m_frames.size() >= 2 && m_frames[1].time <= now + presentationTolerance) {
            m_frames.removeAt(0);
            ++m_droppedFrames;
        }
        if (m_frames.first().time > now + presentationTolerance) {
            m_condition.notifyAll();
            return false;
        }
    }
    auto frame = WTF::move(m_frames[0]);
    m_frames.removeAt(0);
    m_shownSinceSeek = true;
    m_presenting = true;
    {
        DropLockForScope unlocker { locker };
        paint(frame);
        frame = { };
    }
    m_presenting = false;
    m_condition.notifyAll();
    checkEnded();
    return true;
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
