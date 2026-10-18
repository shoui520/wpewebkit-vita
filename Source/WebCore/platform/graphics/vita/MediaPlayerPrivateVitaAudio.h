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
#include <wtf/ThreadSafeWeakPtr.h>

namespace WebCore {

class MediaByteStream;

// Plays audio files that the system video player library does not (MP3,
// Ogg Vorbis and Opus, WebM audio, FLAC, WAV, ADTS AAC): FFmpeg decodes the
// resource as it loads, and a thread writes the samples to a system audio port.
class MediaPlayerPrivateVitaAudio final
    : public MediaPlayerPrivateInterface
    , public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<MediaPlayerPrivateVitaAudio, WTF::DestructionThread::Main> {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerPrivateVitaAudio);
public:
    explicit MediaPlayerPrivateVitaAudio(MediaPlayer&);
    ~MediaPlayerPrivateVitaAudio();

    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

    static void registerMediaEngine(MediaEngineRegistrar);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);
    static void getSupportedTypes(HashSet<String>&);

    class Engine;
    // From the engine's thread, delivered on the main thread.
    void engineOpened(unsigned generation, std::optional<Seconds> duration);
    void engineFailed(unsigned generation, MediaPlayer::NetworkState, const String& message);
    void engineSeeked(unsigned generation);
    void engineEnded(unsigned generation);

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

    FloatSize naturalSize() const final { return { }; }
    bool hasVideo() const final { return false; }
    bool hasAudio() const final { return m_opened; }
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
    MediaTime maxTimeSeekable() const final;
    const PlatformTimeRanges& buffered() const final;
    unsigned long long totalBytes() const final;
    bool didLoadingProgress() const final;

    void paint(GraphicsContext&, const FloatRect&) final { }
    DestinationColorSpace colorSpace() final;

    void createEngine();
    void destroyEngine();
    void setNetworkState(MediaPlayer::NetworkState);
    void setReadyState(MediaPlayer::ReadyState);
    void applyVolume();

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    URL m_url;
    String m_errorMessage;
    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };

    RefPtr<Engine> m_engine;
    RefPtr<MediaByteStream> m_stream;
    // Engine notifications carry the generation they belong to.
    unsigned m_generation { 0 };
    bool m_waitingForPreload { false };
    bool m_opened { false };
    bool m_paused { true };
    bool m_ended { false };
    bool m_seeking { false };
    MediaTime m_seekTarget;
    std::optional<MediaTime> m_duration;
    float m_volume { 1 };
    bool m_muted { false };
    mutable unsigned long long m_lastReportedBytes { 0 };
    mutable PlatformTimeRanges m_buffered;
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
