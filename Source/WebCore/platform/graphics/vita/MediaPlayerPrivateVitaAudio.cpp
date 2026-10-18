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
#include "MediaPlayerPrivateVitaAudio.h"

#if ENABLE(VIDEO) && PLATFORM(VITA)

#include "DestinationColorSpace.h"
#include "MediaByteStream.h"
#include "MediaPlayer.h"
#include "audio/vita/FFmpegAudioDecoder.h"
#include <psp2/audioout.h>
#include <wtf/Condition.h>
#include <wtf/CrossThreadCopier.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerPrivateVitaAudio);

// The system mixes main ports at this rate.
static constexpr int outputRate = 48000;
// Frames per port write, about 21 ms.
static constexpr unsigned framesPerWrite = 1024;

// The decoder reads the resource through the stream, from its own position.
class StreamAudioInput final : public FFmpegAudioDecoder::Input {
public:
    explicit StreamAudioInput(Ref<MediaByteStream>&& stream)
        : m_stream(WTF::move(stream))
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
        if (auto size = m_stream->knownSize(); size && position > *size)
            return false;
        m_position = position;
        return true;
    }

    // FFmpeg also plays streams without a size.
    std::optional<uint64_t> size() final { return m_stream->knownSize(); }

    String failure() const { return m_stream->failure(); }

private:
    const Ref<MediaByteStream> m_stream;
    uint64_t m_position { 0 };
};

// Decodes on a thread of its own and writes the samples to a system audio
// port while playing; the blocking writes pace it.
class MediaPlayerPrivateVitaAudio::Engine final : public ThreadSafeRefCounted<Engine> {
public:
    static Ref<Engine> create(Ref<MediaByteStream>&& stream, MediaPlayerPrivateVitaAudio& owner, unsigned generation)
    {
        return adoptRef(*new Engine(WTF::move(stream), owner, generation));
    }

    void start()
    {
        m_thread = Thread::create("Media audio"_s, [protectedThis = Ref { *this }] {
            protectedThis->run();
        }, ThreadType::Audio, Thread::QOS::UserInteractive);
    }

    // The stream must be closed first, so reads waiting for data give up.
    void stop()
    {
        {
            Locker locker { m_lock };
            m_stopping = true;
            m_condition.notifyAll();
        }
        if (RefPtr thread = std::exchange(m_thread, nullptr))
            thread->waitForCompletion();
    }

    void setPlaying(bool playing)
    {
        Locker locker { m_lock };
        m_playing = playing;
        m_condition.notifyAll();
    }

    void seek(Seconds time)
    {
        Locker locker { m_lock };
        m_seekTarget = time;
        m_position = time.seconds();
        m_condition.notifyAll();
    }

    // 0 to 1.
    void setVolume(float volume) { m_volume = std::clamp(static_cast<int>(volume * SCE_AUDIO_VOLUME_0DB), 0, SCE_AUDIO_VOLUME_0DB); }
    Seconds position() const { return Seconds { m_position.load() }; }

private:
    Engine(Ref<MediaByteStream>&& stream, MediaPlayerPrivateVitaAudio& owner, unsigned generation)
        : m_input(WTF::move(stream))
        , m_owner(owner)
        , m_generation(generation)
    {
    }

    template<typename Function> void notify(Function&& function)
    {
        callOnMainThread([owner = m_owner, generation = m_generation, function = std::forward<Function>(function)] {
            if (RefPtr player = owner.get())
                function(*player, generation);
        });
    }

    void run();

    StreamAudioInput m_input;
    const ThreadSafeWeakPtr<MediaPlayerPrivateVitaAudio> m_owner;
    const unsigned m_generation;
    RefPtr<Thread> m_thread;

    Lock m_lock;
    Condition m_condition;
    bool m_stopping WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_playing WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::optional<Seconds> m_seekTarget WTF_GUARDED_BY_LOCK(m_lock);
    std::atomic<double> m_position { 0 };
    std::atomic<int> m_volume { SCE_AUDIO_VOLUME_0DB };
};

void MediaPlayerPrivateVitaAudio::Engine::run()
{
    String failure;
    auto decoder = FFmpegAudioDecoder::open(m_input, &failure);
    if (!decoder) {
        bool stopping;
        {
            Locker locker { m_lock };
            stopping = m_stopping;
        }
        if (!stopping) {
            // A load that failed explains more than the format it left unread.
            auto loadFailure = m_input.failure();
            bool network = !loadFailure.isNull();
            notify([network, message = crossThreadCopy(network ? loadFailure : failure)](auto& player, unsigned generation) {
                player.engineFailed(generation, network ? MediaPlayer::NetworkState::NetworkError : MediaPlayer::NetworkState::FormatError, message);
            });
        }
        return;
    }
    decoder->setOutput(2, outputRate);
    notify([duration = decoder->duration()](auto& player, unsigned generation) {
        player.engineOpened(generation, duration);
    });

    Vector<Vector<float>> pending(2);
    size_t consumed = 0;
    size_t skip = 0;
    bool atEnd = false;
    Vector<int16_t> samples(framesPerWrite * 2);
    int port = -1;
    int portVolume = -1;
    auto releasePort = [&] {
        if (port < 0)
            return;
        sceAudioOutOutput(port, nullptr);
        sceAudioOutReleasePort(port);
        port = -1;
    };

    while (true) {
        std::optional<Seconds> seekTarget;
        bool playing;
        {
            Locker locker { m_lock };
            playing = m_playing;
        }
        // Paused: the port is free for others meanwhile.
        if (!playing)
            releasePort();
        {
            Locker locker { m_lock };
            while (!m_stopping && !m_seekTarget && !m_playing)
                m_condition.wait(m_lock);
            if (m_stopping)
                break;
            seekTarget = std::exchange(m_seekTarget, std::nullopt);
        }

        if (seekTarget) {
            for (auto& channel : pending)
                channel.clear();
            consumed = 0;
            skip = 0;
            atEnd = false;
            if (auto start = decoder->seek(*seekTarget)) {
                // Samples from before the target are dropped as they decode.
                if (*seekTarget > *start)
                    skip = static_cast<size_t>((*seekTarget - *start).seconds() * outputRate);
            } else
                atEnd = true;
            m_position = seekTarget->seconds();
            notify([](auto& player, unsigned generation) {
                player.engineSeeked(generation);
            });
            continue;
        }

        bool failed = false;
        while (pending[0].size() - consumed < framesPerWrite && !atEnd) {
            auto result = decoder->decode(pending);
            if (result == FFmpegAudioDecoder::Result::End)
                atEnd = true;
            else if (result == FFmpegAudioDecoder::Result::Error) {
                atEnd = true;
                failed = true;
            }
            if (skip) {
                size_t dropped = std::min(skip, pending[0].size() - consumed);
                consumed += dropped;
                skip -= dropped;
            }
        }
        size_t available = pending[0].size() - consumed;
        if (!available && atEnd) {
            {
                Locker locker { m_lock };
                if (m_stopping)
                    break;
                // A seek made meanwhile goes on from there.
                if (m_seekTarget)
                    continue;
                m_playing = false;
            }
            if (failed) {
                notify([message = crossThreadCopy(m_input.failure())](auto& player, unsigned generation) {
                    player.engineFailed(generation, MediaPlayer::NetworkState::DecodeError, message.isNull() ? String { "The audio could not be decoded"_s } : message);
                });
            } else {
                notify([](auto& player, unsigned generation) {
                    player.engineEnded(generation);
                });
            }
            continue;
        }

        if (port < 0) {
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, framesPerWrite, outputRate, SCE_AUDIO_OUT_MODE_STEREO);
            portVolume = -1;
            if (port < 0) {
                sleep(100_ms);
                continue;
            }
        }
        if (int volume = m_volume; volume != portVolume) {
            int volumes[2] = { volume, volume };
            sceAudioOutSetVolume(port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volumes);
            portVolume = volume;
        }
        size_t frames = std::min<size_t>(available, framesPerWrite);
        auto left = pending[0].span().subspan(consumed, frames);
        auto right = pending[1].span().subspan(consumed, frames);
        auto toSample = [](float value) {
            return static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767);
        };
        for (size_t i = 0; i < framesPerWrite; ++i) {
            samples[2 * i] = i < frames ? toSample(left[i]) : 0;
            samples[2 * i + 1] = i < frames ? toSample(right[i]) : 0;
        }
        sceAudioOutOutput(port, samples.span().data());
        consumed += frames;
        m_position = m_position + static_cast<double>(frames) / outputRate;
        // Drop what was played once there is a fair amount.
        if (consumed >= 16 * framesPerWrite) {
            for (auto& channel : pending)
                channel.removeAt(0, consumed);
            consumed = 0;
        }
    }
    releasePort();
}

static HashSet<String>& mimeTypeCache()
{
    static NeverDestroyed<HashSet<String>> cache = [] {
        HashSet<String> types;
        for (auto type : { "audio/mpeg"_s, "audio/mp3"_s, "audio/x-mpeg"_s, "audio/mpeg3"_s, "audio/x-mp3"_s,
            "audio/ogg"_s, "application/ogg"_s, "audio/opus"_s, "audio/webm"_s,
            "audio/flac"_s, "audio/x-flac"_s, "audio/wav"_s, "audio/wave"_s, "audio/x-wav"_s, "audio/vnd.wave"_s,
            "audio/aac"_s, "audio/x-aac"_s })
            types.add(type);
        return types;
    }();
    return cache;
}

void MediaPlayerPrivateVitaAudio::getSupportedTypes(HashSet<String>& types)
{
    types = mimeTypeCache();
}

MediaPlayer::SupportsType MediaPlayerPrivateVitaAudio::supportsType(const MediaEngineSupportParameters& parameters)
{
    if (parameters.isMediaSource || parameters.isMediaStream)
        return MediaPlayer::SupportsType::IsNotSupported;
    auto containerType = parameters.type.containerType();
    if (containerType.isEmpty() || !mimeTypeCache().contains(containerType))
        return MediaPlayer::SupportsType::IsNotSupported;
    auto codecs = parameters.type.codecs();
    if (codecs.isEmpty())
        return MediaPlayer::SupportsType::MayBeSupported;
    for (auto& codec : codecs) {
        bool known = codec == "vorbis"_s || codec == "opus"_s || codec == "flac"_s || codec == "mp3"_s
            || codec == "1"_s || codec.startsWith("mp4a.40"_s) || codec == "mp4a.69"_s || codec == "mp4a.6B"_s;
        if (!known)
            return MediaPlayer::SupportsType::IsNotSupported;
    }
    return MediaPlayer::SupportsType::IsSupported;
}

class MediaPlayerFactoryVitaAudio final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(MediaPlayerFactoryVitaAudio);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(MediaPlayerFactoryVitaAudio);
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::Vita; }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(*new MediaPlayerPrivateVitaAudio(player));
    }

    void getSupportedTypes(HashSet<String>& types) const final
    {
        MediaPlayerPrivateVitaAudio::getSupportedTypes(types);
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        return MediaPlayerPrivateVitaAudio::supportsType(parameters);
    }
};

void MediaPlayerPrivateVitaAudio::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactoryVitaAudio>());
}

MediaPlayerPrivateVitaAudio::MediaPlayerPrivateVitaAudio(MediaPlayer& player)
    : m_player(player)
{
}

MediaPlayerPrivateVitaAudio::~MediaPlayerPrivateVitaAudio()
{
    destroyEngine();
}

void MediaPlayerPrivateVitaAudio::load(const String& url)
{
    destroyEngine();
    m_url = URL { url };
    m_errorMessage = { };
    m_opened = false;
    m_ended = false;
    m_duration = std::nullopt;
    setNetworkState(MediaPlayer::NetworkState::Loading);
    setReadyState(MediaPlayer::ReadyState::HaveNothing);
    RefPtr player = m_player.get();
    // Nothing loads for preload="none" until playback is wanted.
    m_waitingForPreload = player && player->preload() == MediaPlayer::Preload::None;
    if (m_waitingForPreload)
        setNetworkState(MediaPlayer::NetworkState::Idle);
    else
        createEngine();
}

void MediaPlayerPrivateVitaAudio::prepareToPlay()
{
    if (!std::exchange(m_waitingForPreload, false))
        return;
    setNetworkState(MediaPlayer::NetworkState::Loading);
    createEngine();
}

void MediaPlayerPrivateVitaAudio::setPreload(MediaPlayer::Preload preload)
{
    if (preload != MediaPlayer::Preload::None)
        prepareToPlay();
}

void MediaPlayerPrivateVitaAudio::cancelLoad()
{
    destroyEngine();
    m_waitingForPreload = false;
    setNetworkState(MediaPlayer::NetworkState::Idle);
}

void MediaPlayerPrivateVitaAudio::createEngine()
{
    RefPtr player = m_player.get();
    if (!player || m_engine)
        return;
    Ref stream = MediaByteStream::create(player->mediaResourceLoader(), m_url);
    m_stream = stream.ptr();
    m_engine = Engine::create(WTF::move(stream), *this, ++m_generation);
    applyVolume();
    if (!m_paused)
        m_engine->setPlaying(true);
    m_stream->start();
    m_engine->start();
}

void MediaPlayerPrivateVitaAudio::destroyEngine()
{
    // Reads waiting for data give up, so the thread can finish.
    if (RefPtr stream = std::exchange(m_stream, nullptr))
        stream->close();
    if (RefPtr engine = std::exchange(m_engine, nullptr))
        engine->stop();
    ++m_generation;
}

void MediaPlayerPrivateVitaAudio::engineOpened(unsigned generation, std::optional<Seconds> duration)
{
    if (generation != m_generation)
        return;
    m_opened = true;
    m_duration = duration ? MediaTime::createWithDouble(duration->seconds()) : MediaTime::positiveInfiniteTime();
    RefPtr player = m_player.get();
    if (!player)
        return;
    player->durationChanged();
    player->characteristicChanged();
    // The decoder reads as it plays; it has what it needs to start.
    setReadyState(MediaPlayer::ReadyState::HaveEnoughData);
    setNetworkState(MediaPlayer::NetworkState::Idle);
}

void MediaPlayerPrivateVitaAudio::engineFailed(unsigned generation, MediaPlayer::NetworkState state, const String& message)
{
    if (generation != m_generation)
        return;
    destroyEngine();
    m_errorMessage = message;
    setNetworkState(state);
}

void MediaPlayerPrivateVitaAudio::engineSeeked(unsigned generation)
{
    if (generation != m_generation || !m_seeking)
        return;
    m_seeking = false;
    if (RefPtr player = m_player.get()) {
        player->seeked(m_seekTarget);
        player->timeChanged();
    }
}

void MediaPlayerPrivateVitaAudio::engineEnded(unsigned generation)
{
    if (generation != m_generation || m_ended)
        return;
    m_ended = true;
    m_paused = true;
    if (RefPtr player = m_player.get()) {
        player->timeChanged();
        player->playbackStateChanged();
    }
}

void MediaPlayerPrivateVitaAudio::play()
{
    if (!m_paused)
        return;
    m_paused = false;
    if (m_waitingForPreload)
        prepareToPlay();
    else if (RefPtr engine = m_engine) {
        if (std::exchange(m_ended, false))
            engine->seek(0_s);
        engine->setPlaying(true);
    }
    if (RefPtr player = m_player.get())
        player->playbackStateChanged();
}

void MediaPlayerPrivateVitaAudio::pause()
{
    if (m_paused)
        return;
    m_paused = true;
    if (RefPtr engine = m_engine)
        engine->setPlaying(false);
    if (RefPtr player = m_player.get())
        player->playbackStateChanged();
}

MediaTime MediaPlayerPrivateVitaAudio::duration() const
{
    return m_duration.value_or(MediaTime::zeroTime());
}

MediaTime MediaPlayerPrivateVitaAudio::currentTime() const
{
    if (m_ended)
        return duration();
    if (m_seeking)
        return m_seekTarget;
    RefPtr engine = m_engine;
    if (!engine)
        return MediaTime::zeroTime();
    auto time = MediaTime::createWithDouble(engine->position().seconds());
    return m_duration && m_duration->isFinite() ? std::min(time, *m_duration) : time;
}

void MediaPlayerPrivateVitaAudio::seekToTarget(const SeekTarget& target)
{
    m_seekTarget = std::max(target.time, MediaTime::zeroTime());
    if (m_duration && m_duration->isFinite())
        m_seekTarget = std::min(m_seekTarget, *m_duration);
    m_seeking = true;
    m_ended = false;
    if (RefPtr engine = m_engine) {
        engine->seek(Seconds { m_seekTarget.toDouble() });
        return;
    }
    callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }, generation = m_generation] {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->engineSeeked(generation);
    });
}

MediaTime MediaPlayerPrivateVitaAudio::maxTimeSeekable() const
{
    return m_duration && m_duration->isFinite() ? *m_duration : MediaTime::zeroTime();
}

void MediaPlayerPrivateVitaAudio::setVolume(float volume)
{
    m_volume = volume;
    applyVolume();
}

void MediaPlayerPrivateVitaAudio::setMuted(bool muted)
{
    m_muted = muted;
    applyVolume();
}

void MediaPlayerPrivateVitaAudio::applyVolume()
{
    if (RefPtr engine = m_engine)
        engine->setVolume(m_muted ? 0 : m_volume);
}

void MediaPlayerPrivateVitaAudio::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateVitaAudio::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    m_readyState = state;
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

const PlatformTimeRanges& MediaPlayerPrivateVitaAudio::buffered() const
{
    m_buffered.clear();
    RefPtr stream = m_stream;
    auto fraction = stream ? stream->loadedFraction() : std::nullopt;
    if (fraction && *fraction > 0 && m_duration && m_duration->isFinite())
        m_buffered.add(MediaTime::zeroTime(), MediaTime::createWithDouble(m_duration->toDouble() * *fraction));
    return m_buffered;
}

unsigned long long MediaPlayerPrivateVitaAudio::totalBytes() const
{
    RefPtr stream = m_stream;
    return stream ? stream->loadedBytes() : 0;
}

bool MediaPlayerPrivateVitaAudio::didLoadingProgress() const
{
    auto loaded = totalBytes();
    return std::exchange(m_lastReportedBytes, loaded) != loaded;
}

DestinationColorSpace MediaPlayerPrivateVitaAudio::colorSpace()
{
    return DestinationColorSpace::SRGB();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
