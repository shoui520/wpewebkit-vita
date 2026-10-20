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

#include "VitaVideoDecoder.h"
#include "audio/vita/FFmpegAudioDecoder.h"
#include <atomic>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

struct AVCodecContext;

namespace WebCore {

class FFmpegDemuxer;
class MediaByteStream;

// Plays a media file read from a MediaByteStream: one thread demuxes it with
// FFmpeg, one decodes its video (VitaVideoDecoder) into a few pictures waiting
// to be shown, one decodes its audio and plays it on a system port. The audio
// is the clock while there is some; the system clock otherwise.
class VitaMediaPipeline final : public ThreadSafeRefCounted<VitaMediaPipeline, WTF::DestructionThread::Main> {
public:
    enum class Event : uint8_t { Ready, Failed, SeekDone, Ended };
    using EventHandler = Function<void(Event)>;

    struct Info {
        bool hasVideo { false };
        bool hasAudio { false };
        unsigned width { 0 };
        unsigned height { 0 };
        // The shape of a pixel: 1 for square ones.
        float pixelAspectRatio { 1 };
        std::optional<Seconds> duration;
        String videoDecoder;
    };

    // Events are delivered on the main thread until stop().
    static Ref<VitaMediaPipeline> create(Ref<MediaByteStream>&&, EventHandler&&);
    ~VitaMediaPipeline();

    // The main thread from here on.
    // Opens the media and decodes from START; Ready or Failed follows.
    void start(Seconds start);
    // Ends the threads; no events come after.
    void stop();
    // Valid after Ready.
    const Info& info() const { return m_info; }
    String failure() const;

    void play();
    void pause();
    // SeekDone follows once the time can be shown.
    void seek(Seconds);
    void setVolume(float);
    Seconds currentTime() const;

    // Shows the picture due now through PAINT when it is not the one shown last; the
    // first picture after a start or a seek shows whatever the time.
    bool presentFrame(const Function<void(const VitaVideoFrame&)>& paint);
    unsigned droppedFrames() const { return m_droppedFrames; }

private:
    VitaMediaPipeline(Ref<MediaByteStream>&&, EventHandler&&);

    struct Packet {
        Vector<uint8_t> data;
        std::optional<Seconds> presentationTime;
        std::optional<Seconds> decodeTime;
        unsigned generation { 0 };
    };

    void post(Event);
    void fail(String&&);
    void readerThread();
    bool open();
    void videoThread();
    void audioThread();
    Seconds currentTimeLocked() const WTF_REQUIRES_LOCK(m_lock);
    void checkEnded() WTF_REQUIRES_LOCK(m_lock);
    bool packetsWanted() const WTF_REQUIRES_LOCK(m_lock);

    const Ref<MediaByteStream> m_stream;
    // The main thread.
    EventHandler m_handler;
    bool m_stopped { false };

    // Set by the reader thread before Ready.
    Info m_info;
    std::unique_ptr<FFmpegAudioDecoder::Input> m_input;
    std::unique_ptr<FFmpegDemuxer> m_demuxer;
    AVCodecContext* m_audioCodec { nullptr };
    std::unique_ptr<VitaVideoDecoder> m_videoDecoder;
    RefPtr<Thread> m_reader;
    RefPtr<Thread> m_video;
    RefPtr<Thread> m_audio;

    mutable Lock m_lock;
    Condition m_condition;
    bool m_closing WTF_GUARDED_BY_LOCK(m_lock) { false };
    String m_failure WTF_GUARDED_BY_LOCK(m_lock);
    bool m_playing WTF_GUARDED_BY_LOCK(m_lock) { false };
    // Seeks number the packets and pictures after them.
    unsigned m_generation WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    Seconds m_seekTarget WTF_GUARDED_BY_LOCK(m_lock);
    bool m_seekPending WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_seekDonePosted WTF_GUARDED_BY_LOCK(m_lock) { true };
    Deque<Packet> m_videoPackets WTF_GUARDED_BY_LOCK(m_lock);
    Deque<Packet> m_audioPackets WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_packetBytes WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_endOfPackets WTF_GUARDED_BY_LOCK(m_lock) { false };
    Vector<VitaVideoFrame> m_frames WTF_GUARDED_BY_LOCK(m_lock);
    // A picture the main thread is painting.
    bool m_presenting WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_shownSinceSeek WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_videoEnded WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_audioEnded WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_endedPosted WTF_GUARDED_BY_LOCK(m_lock) { false };

    // The clock: the time at m_clockStarted, running while playing without audio.
    Seconds m_clockTime WTF_GUARDED_BY_LOCK(m_lock);
    MonotonicTime m_clockStarted WTF_GUARDED_BY_LOCK(m_lock);
    // With audio: the time heard at m_audioClockStamp and the end of what was written.
    bool m_audioClockValid WTF_GUARDED_BY_LOCK(m_lock) { false };
    Seconds m_audioClock WTF_GUARDED_BY_LOCK(m_lock);
    Seconds m_audioWrittenEnd WTF_GUARDED_BY_LOCK(m_lock);
    MonotonicTime m_audioClockStamp WTF_GUARDED_BY_LOCK(m_lock);

    std::atomic<int> m_volume;
    std::atomic<unsigned> m_droppedFrames { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
