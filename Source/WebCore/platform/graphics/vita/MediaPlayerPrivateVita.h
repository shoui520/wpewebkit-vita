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

#include "MediaPlayerPrivate.h"
#include "PlatformTimeRanges.h"
#include "VideoPlaybackQualityMetrics.h"
#include "VitaMediaPipeline.h"
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeWeakPtr.h>

typedef struct _cairo_surface cairo_surface_t;

namespace WebCore {

class AudioTrackPrivate;
class MediaByteStream;
class NativeImage;
class VideoTrackPrivate;

// Plays video files (MP4, WebM, Ogg...) with a VitaMediaPipeline reading the
// resource through WebCore's media loader in byte ranges: H.264 decodes on the
// hardware, other video with FFmpeg. Decoded pictures are converted to a Cairo
// image to paint, and audio goes to a system audio port.
class MediaPlayerPrivateVita final
    : public MediaPlayerPrivateInterface
    , public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<MediaPlayerPrivateVita, WTF::DestructionThread::Main> {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerPrivateVita);
public:
    explicit MediaPlayerPrivateVita(MediaPlayer&);
    ~MediaPlayerPrivateVita();

    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

    static void registerMediaEngine(MediaEngineRegistrar);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);
    static void getSupportedTypes(HashSet<String>&);

    void pipelineEvent(VitaMediaPipeline::Event, unsigned generation);

private:
    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::Vita; }

    void load(const String&) final;
    void cancelLoad() final;
    void prepareToPlay() final;
    void setPreload(MediaPlayer::Preload) final;

    void play() final;
    void pause() final;
    bool paused() const final { return m_paused; }
    bool ended() const final { return m_ended; }

    FloatSize naturalSize() const final { return m_naturalSize; }
    bool hasVideo() const final { return m_hasVideo; }
    bool hasAudio() const final { return m_hasAudio; }
    // The element goes fullscreen; the frames paint as usual.
    bool supportsFullscreen() const final { return true; }
    void setPageIsVisible(bool) final { }

    MediaTime duration() const final;
    MediaTime currentTime() const final;
    void seekToTarget(const SeekTarget&) final;
    bool seeking() const final { return m_seeking; }
    double rate() const final { return m_paused ? 0 : 1; }

    void setVolume(float) final;
    void setMuted(bool) final;

    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    String errorMessage() const final { return m_errorMessage; }
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }
    MediaTime maxTimeSeekable() const final { return m_duration.isValid() ? m_duration : MediaTime::zeroTime(); }
    const PlatformTimeRanges& buffered() const final;
    unsigned long long totalBytes() const final;
    bool didLoadingProgress() const final;
    std::optional<VideoPlaybackQualityMetrics> videoPlaybackQualityMetrics() final;

    void paint(GraphicsContext&, const FloatRect&) final;
    RefPtr<NativeImage> nativeImageForCurrentTime() final;
    DestinationColorSpace colorSpace() final;

    void loadIfDeferred();
    void createPlayer();
    void destroyPlayer();
    bool wantsToPlay() const { return !m_paused || m_playRequested; }
    bool acquireLibrary();
    bool canGiveUpLibraryFor(const MediaPlayerPrivateVita&) const;
    void giveUpLibrary();
    void scheduleLibraryYield();
    void yieldLibrary();
    static void grantLibraryToNextWaiting();
    void playerReady();
    void startPlayback();
    void showFirstFrame();
    void reachedEnd();
    void convertFrame(const VitaVideoFrame&);
    void updateTracks();
    void removeTracks();
    void fail(MediaPlayer::NetworkState, const String& message = { });
    void setNetworkState(MediaPlayer::NetworkState);
    void setReadyState(MediaPlayer::ReadyState);
    void updateFrame();
    void finishSeek();
    void applyVolume();

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    URL m_url;
    String m_errorMessage;
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };

    // The pipeline; its events carry the generation they belong to.
    RefPtr<VitaMediaPipeline> m_pipeline;
    unsigned m_generation { 0 };
    RefPtr<MediaByteStream> m_stream;
    // Frames shown, and those dropped by pipelines before this one.
    uint32_t m_presentedFrames { 0 };
    uint32_t m_droppedFrames { 0 };
    bool m_waitingForPreload { false };
    // Gave the library up to another player once loaded: a new player resumes at m_startTime.
    bool m_suspended { false };
    // Asked to play before it had data (prepareToPlay()).
    bool m_playRequested { false };
    bool m_ready { false };
    bool m_paused { true };
    bool m_ended { false };
    bool m_seeking { false };
    // Paused, waiting for the first frame to show.
    bool m_showingFirstFrame { false };
    MonotonicTime m_firstFrameStarted;
    // When playback last started or resumed.
    MonotonicTime m_playbackStarted;
    // Where playback resumes when a new player has to be made (after the end, or
    // after giving the library up).
    MediaTime m_startTime { MediaTime::zeroTime() };
    MediaTime m_seekTarget;
    MediaTime m_duration { MediaTime::invalidTime() };
    bool m_hasVideo { false };
    bool m_hasAudio { false };
    FloatSize m_naturalSize;
    // The media's tracks: the controls fade while a video with a video track plays.
    RefPtr<VideoTrackPrivate> m_videoTrack;
    RefPtr<AudioTrackPrivate> m_audioTrack;
    float m_volume { 1 };
    bool m_muted { false };
    mutable unsigned long long m_lastReportedBytes { 0 };
    mutable PlatformTimeRanges m_buffered;

    // The latest frame, converted (at half size while shown that small).
    RefPtr<cairo_surface_t> m_frame;
    RefPtr<NativeImage> m_frameImage;
    // The size of the last paint, in device pixels, and when a canvas last
    // asked for the frame.
    FloatSize m_paintedSize;
    MonotonicTime m_lastImageRequest;
    RunLoop::Timer m_frameTimer;
    RunLoop::Timer m_readyTimeout;
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
