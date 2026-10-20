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
#include "MediaPlayerPrivateVita.h"

#if ENABLE(VIDEO) && PLATFORM(VITA)

#include "AudioTrackPrivate.h"
#include "GraphicsContext.h"
#include "MediaByteStream.h"
#include "MediaPlayer.h"
#include "NativeImage.h"
#include "VideoTrackPrivate.h"
#include <arm_neon.h>
#include <cairo.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerPrivateVita);

// The first video and audio streams, the ones played.
class VitaVideoTrack final : public VideoTrackPrivate {
public:
    static Ref<VitaVideoTrack> create() { return adoptRef(*new VitaVideoTrack); }
    TrackID id() const final { return 1; }
    Kind kind() const final { return Kind::Main; }
};

class VitaAudioTrack final : public AudioTrackPrivate {
public:
    static Ref<VitaAudioTrack> create() { return adoptRef(*new VitaAudioTrack); }
    TrackID id() const final { return 2; }
    Kind kind() const final { return Kind::Main; }
};

// Pictures are 8-bit 4:2:0: NV12 from the hardware decoder (U and V
// interleaved), I420 from FFmpeg (U and V planes). They become Cairo's
// premultiplied BGRA with the limited range BT.601 or BT.709 matrix in 6-bit
// fixed point.
struct YUVMatrix {
    int16_t y, rv, gu, gv, bu;
};

static YUVMatrix matrixForHeight(unsigned height)
{
    // HD video uses BT.709.
    return height >= 720 ? YUVMatrix { 75, 115, 14, 34, 135 } : YUVMatrix { 75, 102, 25, 52, 129 };
}

static void storePixel(const YUVMatrix& matrix, int luma, int u, int v, uint8_t* out)
{
    int y = (std::max(luma, 16) - 16) * matrix.y;
    out[0] = std::clamp((y + matrix.bu * u + 32) >> 6, 0, 255);
    out[1] = std::clamp((y - matrix.gu * u - matrix.gv * v + 32) >> 6, 0, 255);
    out[2] = std::clamp((y + matrix.rv * v + 32) >> 6, 0, 255);
    out[3] = 255;
}

// Eight pixels from their luma and the chroma terms of each color.
static inline void storePixels(const YUVMatrix& matrix, uint8x8_t luma, int16x8_t red, int16x8_t green, int16x8_t blue, uint8_t* out)
{
    int16x8_t scaled = vreinterpretq_s16_u16(vmull_u8(vqsub_u8(luma, vdup_n_u8(16)), vdup_n_u8(matrix.y)));
    uint8x8x4_t bgra;
    bgra.val[0] = vqrshrun_n_s16(vqaddq_s16(scaled, blue), 6);
    bgra.val[1] = vqrshrun_n_s16(vqaddq_s16(scaled, green), 6);
    bgra.val[2] = vqrshrun_n_s16(vqaddq_s16(scaled, red), 6);
    bgra.val[3] = vdup_n_u8(255);
    vst4_u8(out, bgra);
}

template<bool interleaved>
static void convertFrame(const VitaVideoFrame& frame, cairo_surface_t* surface)
{
    auto matrix = matrixForHeight(frame.height);
    unsigned width = frame.width;
    unsigned height = frame.height;
    // Bytes from one chroma sample to the next.
    constexpr unsigned step = interleaved ? 2 : 1;
    uint8_t* output = cairo_image_surface_get_data(surface);
    unsigned outputStride = cairo_image_surface_get_stride(surface);

    const uint8x8_t offset128 = vdup_n_u8(128);
    unsigned vectorWidth = width & ~15u;
    for (unsigned row = 0; row < height; row += 2) {
        const uint8_t* uRow = frame.u + (row / 2) * frame.chromaPitch;
        const uint8_t* vRow = frame.v + (row / 2) * frame.chromaPitch;
        for (unsigned line = row; line < std::min(row + 2, height); ++line) {
            const uint8_t* luma = frame.luma + line * frame.lumaPitch;
            uint8_t* out = output + line * outputStride;
            for (unsigned x = 0; x < vectorWidth; x += 16) {
                uint8x8_t uBytes, vBytes;
                if constexpr (interleaved) {
                    uint8x8x2_t pairs = vld2_u8(uRow + x);
                    uBytes = pairs.val[0];
                    vBytes = pairs.val[1];
                } else {
                    uBytes = vld1_u8(uRow + x / 2);
                    vBytes = vld1_u8(vRow + x / 2);
                }
                int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(uBytes, offset128));
                int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(vBytes, offset128));
                // Each chroma sample covers two pixels.
                int16x8x2_t red = vzipq_s16(vmulq_n_s16(v, matrix.rv), vmulq_n_s16(v, matrix.rv));
                int16x8_t greenTerm = vmlsq_n_s16(vmulq_n_s16(u, -matrix.gu), v, matrix.gv);
                int16x8x2_t green = vzipq_s16(greenTerm, greenTerm);
                int16x8x2_t blue = vzipq_s16(vmulq_n_s16(u, matrix.bu), vmulq_n_s16(u, matrix.bu));
                uint8x16_t y = vld1q_u8(luma + x);
                storePixels(matrix, vget_low_u8(y), red.val[0], green.val[0], blue.val[0], out + x * 4);
                storePixels(matrix, vget_high_u8(y), red.val[1], green.val[1], blue.val[1], out + (x + 8) * 4);
            }
            for (unsigned x = vectorWidth; x < width; ++x)
                storePixel(matrix, luma[x], uRow[(x / 2) * step] - 128, vRow[(x / 2) * step] - 128, out + x * 4);
        }
    }
    cairo_surface_mark_dirty(surface);
}

// The same at half the width and height, for a video shown at half its size
// or less: each pixel averages four luma samples and takes the one chroma
// sample covering them.
template<bool interleaved>
static void convertFrameAtHalfSize(const VitaVideoFrame& frame, cairo_surface_t* surface)
{
    auto matrix = matrixForHeight(frame.height);
    constexpr unsigned step = interleaved ? 2 : 1;
    uint8_t* output = cairo_image_surface_get_data(surface);
    unsigned outputStride = cairo_image_surface_get_stride(surface);

    const uint8x8_t offset128 = vdup_n_u8(128);
    unsigned outputWidth = frame.width / 2;
    unsigned vectorWidth = outputWidth & ~15u;
    for (unsigned row = 0; row < frame.height / 2; ++row) {
        const uint8_t* top = frame.luma + 2 * row * frame.lumaPitch;
        const uint8_t* bottom = top + frame.lumaPitch;
        const uint8_t* uRow = frame.u + row * frame.chromaPitch;
        const uint8_t* vRow = frame.v + row * frame.chromaPitch;
        uint8_t* out = output + row * outputStride;
        for (unsigned x = 0; x < vectorWidth; x += 16) {
            uint8x16x2_t topPairs = vld2q_u8(top + 2 * x);
            uint8x16x2_t bottomPairs = vld2q_u8(bottom + 2 * x);
            uint8x16_t y = vrhaddq_u8(vhaddq_u8(topPairs.val[0], topPairs.val[1]), vhaddq_u8(bottomPairs.val[0], bottomPairs.val[1]));
            uint8x16_t uBytes, vBytes;
            if constexpr (interleaved) {
                uint8x16x2_t pairs = vld2q_u8(uRow + 2 * x);
                uBytes = pairs.val[0];
                vBytes = pairs.val[1];
            } else {
                uBytes = vld1q_u8(uRow + x);
                vBytes = vld1q_u8(vRow + x);
            }
            for (int half = 0; half < 2; ++half) {
                int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(half ? vget_high_u8(uBytes) : vget_low_u8(uBytes), offset128));
                int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(half ? vget_high_u8(vBytes) : vget_low_u8(vBytes), offset128));
                int16x8_t green = vmlsq_n_s16(vmulq_n_s16(u, -matrix.gu), v, matrix.gv);
                storePixels(matrix, half ? vget_high_u8(y) : vget_low_u8(y), vmulq_n_s16(v, matrix.rv), green, vmulq_n_s16(u, matrix.bu), out + (x + half * 8) * 4);
            }
        }
        for (unsigned x = vectorWidth; x < outputWidth; ++x) {
            // Rounded as the vector loop does.
            int luma = (((top[2 * x] + top[2 * x + 1]) >> 1) + ((bottom[2 * x] + bottom[2 * x + 1]) >> 1) + 1) >> 1;
            storePixel(matrix, luma, uRow[x * step] - 128, vRow[x * step] - 128, out + x * 4);
        }
    }
    cairo_surface_mark_dirty(surface);
}

static HashSet<String>& mimeTypeCache()
{
    static NeverDestroyed<HashSet<String>> cache = [] {
        HashSet<String> types;
        for (auto type : { "video/mp4"_s, "audio/mp4"_s, "video/x-m4v"_s, "audio/x-m4a"_s, "video/quicktime"_s,
            "video/webm"_s, "video/x-matroska"_s, "video/ogg"_s, "video/mp2t"_s })
            types.add(type);
        return types;
    }();
    return cache;
}

void MediaPlayerPrivateVita::getSupportedTypes(HashSet<String>& types)
{
    types = mimeTypeCache();
}

// H.264 in 8-bit 4:2:0 (the hardware's, or FFmpeg's past 1920x1088), VP8, VP9
// profile 0 and Theora; AAC, MP3, Opus, Vorbis and FLAC. Not HEVC or AV1.
static bool isSupportedCodec(StringView codec)
{
    if (codec.startsWith("avc1"_s) || codec.startsWith("avc3"_s)) {
        // avc1.PPCCLL: High 10, 4:2:2 and 4:4:4 profiles decode to other formats.
        auto profile = codec.substring(5, 2);
        return !equalLettersIgnoringASCIICase(profile, "6e"_s) && !equalLettersIgnoringASCIICase(profile, "7a"_s)
            && !equalLettersIgnoringASCIICase(profile, "f4"_s) && profile != "2C"_s && profile != "2c"_s;
    }
    if (codec.startsWith("vp09."_s))
        return codec.startsWith("vp09.00"_s);
    for (auto supported : { "vp8"_s, "vp9"_s, "vp8.0"_s, "vp9.0"_s, "theora"_s, "opus"_s, "vorbis"_s, "flac"_s, "mp3"_s, "mp4a.69"_s, "mp4a.6b"_s, "mp4a.6B"_s }) {
        if (codec == supported)
            return true;
    }
    return codec.startsWith("mp4a.40"_s);
}

MediaPlayer::SupportsType MediaPlayerPrivateVita::supportsType(const MediaEngineSupportParameters& parameters)
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
        if (!isSupportedCodec(codec))
            return MediaPlayer::SupportsType::IsNotSupported;
    }
    return MediaPlayer::SupportsType::IsSupported;
}

class MediaPlayerFactoryVita final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(MediaPlayerFactoryVita);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(MediaPlayerFactoryVita);
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::Vita; }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(*new MediaPlayerPrivateVita(player));
    }

    void getSupportedTypes(HashSet<String>& types) const final
    {
        MediaPlayerPrivateVita::getSupportedTypes(types);
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        return MediaPlayerPrivateVita::supportsType(parameters);
    }
};

void MediaPlayerPrivateVita::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactoryVita>());
}

// One player at a time has a pipeline: the hardware decoder takes one stream,
// and each pipeline holds several megabytes (discord.com has several
// autoplaying videos). Playing takes the pipeline's place from the player
// holding it, and loading takes it from one that is paused with its first frame
// shown, or ended; otherwise players wait for it in turn. Main thread only.
static MediaPlayerPrivateVita* libraryHolder;
static constexpr Seconds playbackBeforeYield = 2_s;

static Vector<MediaPlayerPrivateVita*>& playersWaitingForLibrary()
{
    static NeverDestroyed<Vector<MediaPlayerPrivateVita*>> players;
    return players;
}

MediaPlayerPrivateVita::MediaPlayerPrivateVita(MediaPlayer& player)
    : m_player(player)
    , m_frameTimer(RunLoop::mainSingleton(), "MediaPlayerPrivateVita::FrameTimer"_s, this, &MediaPlayerPrivateVita::updateFrame)
    , m_readyTimeout(RunLoop::mainSingleton(), "MediaPlayerPrivateVita::ReadyTimeout"_s, [this] { fail(MediaPlayer::NetworkState::FormatError, "The media did not open in time"_s); })
{
}

MediaPlayerPrivateVita::~MediaPlayerPrivateVita()
{
    destroyPlayer();
    removeTracks();
}

void MediaPlayerPrivateVita::updateTracks()
{
    RefPtr player = m_player.get();
    if (!player)
        return;
    if (m_hasVideo && !m_videoTrack) {
        Ref track = VitaVideoTrack::create();
        track->setConfiguration({ { }, static_cast<uint32_t>(m_naturalSize.width()), static_cast<uint32_t>(m_naturalSize.height()) });
        track->setSelected(true);
        m_videoTrack = track.copyRef();
        player->addVideoTrack(track);
    }
    if (m_hasAudio && !m_audioTrack) {
        Ref track = VitaAudioTrack::create();
        track->setEnabled(true);
        m_audioTrack = track.copyRef();
        player->addAudioTrack(track);
    }
}

void MediaPlayerPrivateVita::removeTracks()
{
    RefPtr player = m_player.get();
    if (RefPtr track = std::exchange(m_videoTrack, nullptr); track && player)
        player->removeVideoTrack(*track);
    if (RefPtr track = std::exchange(m_audioTrack, nullptr); track && player)
        player->removeAudioTrack(*track);
}

void MediaPlayerPrivateVita::load(const String& url)
{
    destroyPlayer();
    removeTracks();
    m_url = URL { url };
    m_errorMessage = { };
    m_suspended = false;
    m_playRequested = false;
    m_ready = false;
    m_ended = false;
    m_startTime = MediaTime::zeroTime();
    setNetworkState(MediaPlayer::NetworkState::Loading);
    setReadyState(MediaPlayer::ReadyState::HaveNothing);
    RefPtr player = m_player.get();
    // Nothing loads for preload="none" until playback is wanted.
    m_waitingForPreload = player && player->preload() == MediaPlayer::Preload::None;
    if (m_waitingForPreload)
        setNetworkState(MediaPlayer::NetworkState::Idle);
    else
        createPlayer();
}

void MediaPlayerPrivateVita::prepareToPlay()
{
    // Until it has data to play, the element asks for this rather than play().
    m_playRequested = true;
    if (playersWaitingForLibrary().contains(this))
        createPlayer();
    else
        loadIfDeferred();
}

void MediaPlayerPrivateVita::loadIfDeferred()
{
    if (!std::exchange(m_waitingForPreload, false))
        return;
    setNetworkState(MediaPlayer::NetworkState::Loading);
    createPlayer();
}

void MediaPlayerPrivateVita::setPreload(MediaPlayer::Preload preload)
{
    if (preload != MediaPlayer::Preload::None)
        loadIfDeferred();
}

void MediaPlayerPrivateVita::cancelLoad()
{
    destroyPlayer();
    m_waitingForPreload = false;
    m_suspended = false;
    m_playRequested = false;
    setNetworkState(MediaPlayer::NetworkState::Idle);
}

bool MediaPlayerPrivateVita::acquireLibrary()
{
    auto& waiting = playersWaitingForLibrary();
    if (libraryHolder && libraryHolder != this) {
        if (!libraryHolder->canGiveUpLibraryFor(*this)) {
            if (!waiting.contains(this))
                waiting.append(this);
            if (wantsToPlay()) {
                // Asks again once the holder has played a while.
                RunLoop::mainSingleton().dispatchAfter(libraryHolder->m_playbackStarted + playbackBeforeYield - MonotonicTime::now(), [weakThis = ThreadSafeWeakPtr { *this }] {
                    RefPtr protectedThis = weakThis.get();
                    if (protectedThis && protectedThis->wantsToPlay() && playersWaitingForLibrary().contains(protectedThis.get()))
                        protectedThis->createPlayer();
                });
            }
            return false;
        }
        RefPtr { libraryHolder }->giveUpLibrary();
    }
    waiting.removeFirst(this);
    libraryHolder = this;
    return true;
}

bool MediaPlayerPrivateVita::canGiveUpLibraryFor(const MediaPlayerPrivateVita& other) const
{
    bool settled = m_paused && (m_ended || (m_ready && !m_showingFirstFrame && !m_seeking));
    if (!other.wantsToPlay())
        return settled;
    // Not to a player that was playing a moment ago either: pages that play a video
    // again when it pauses would pass the library back and forth.
    return settled || MonotonicTime::now() - m_playbackStarted >= playbackBeforeYield;
}

void MediaPlayerPrivateVita::giveUpLibrary()
{
    bool loaded = m_ready && !m_ended;
    bool playing = !m_paused && !m_ended;
    if (loaded)
        m_startTime = currentTime();
    // The player taking over holds it next.
    libraryHolder = nullptr;
    destroyPlayer();
    if (!loaded) {
        // Loads again in turn.
        if (!m_ended && !m_waitingForPreload && m_networkState == MediaPlayer::NetworkState::Loading)
            playersWaitingForLibrary().append(this);
        return;
    }
    // Keeps its media's state and last frame; playing again makes a new player.
    m_suspended = true;
    if (playing) {
        m_paused = true;
        if (RefPtr player = m_player.get())
            player->playbackStateChanged();
    }
}

// Once paused with a frame to show, or ended, the holder lets a waiting player load.
void MediaPlayerPrivateVita::scheduleLibraryYield()
{
    if (libraryHolder != this || playersWaitingForLibrary().isEmpty())
        return;
    callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->yieldLibrary();
    });
}

void MediaPlayerPrivateVita::yieldLibrary()
{
    auto& waiting = playersWaitingForLibrary();
    if (libraryHolder != this || waiting.isEmpty() || !canGiveUpLibraryFor(*waiting.first()))
        return;
    giveUpLibrary();
    grantLibraryToNextWaiting();
}

void MediaPlayerPrivateVita::grantLibraryToNextWaiting()
{
    auto& waiting = playersWaitingForLibrary();
    while (!libraryHolder && !waiting.isEmpty()) {
        RefPtr next = waiting[0];
        waiting.removeAt(0);
        next->createPlayer();
    }
}

void MediaPlayerPrivateVita::createPlayer()
{
    RefPtr player = m_player.get();
    if (!player || m_pipeline || !acquireLibrary())
        return;
    m_suspended = false;
    Ref stream = MediaByteStream::create(player->mediaResourceLoader(), m_url);
    m_stream = stream.copyRef();
    stream->start();
    m_pipeline = VitaMediaPipeline::create(WTF::move(stream), [weakThis = ThreadSafeWeakPtr { *this }, generation = ++m_generation](VitaMediaPipeline::Event event) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->pipelineEvent(event, generation);
    });
    applyVolume();
    m_pipeline->start(Seconds { m_startTime.toDouble() });
    m_readyTimeout.startOneShot(60_s);
}

void MediaPlayerPrivateVita::destroyPlayer()
{
    m_frameTimer.stop();
    m_readyTimeout.stop();
    if (RefPtr pipeline = std::exchange(m_pipeline, nullptr)) {
        m_droppedFrames += pipeline->droppedFrames();
        pipeline->stop();
    } else if (RefPtr stream = m_stream)
        stream->close();
    playersWaitingForLibrary().removeFirst(this);
    if (libraryHolder == this) {
        libraryHolder = nullptr;
        callOnMainThread(grantLibraryToNextWaiting);
    }
    m_stream = nullptr;
    m_showingFirstFrame = false;
    m_ready = false;
    ++m_generation;
}

void MediaPlayerPrivateVita::pipelineEvent(VitaMediaPipeline::Event event, unsigned generation)
{
    RefPtr pipeline = m_pipeline;
    if (generation != m_generation || !pipeline)
        return;
    switch (event) {
    case VitaMediaPipeline::Event::Ready:
        playerReady();
        break;
    case VitaMediaPipeline::Event::Failed:
        fail(m_ready ? MediaPlayer::NetworkState::DecodeError : MediaPlayer::NetworkState::FormatError, pipeline->failure());
        break;
    case VitaMediaPipeline::Event::SeekDone:
        finishSeek();
        break;
    case VitaMediaPipeline::Event::Ended:
        reachedEnd();
        break;
    }
}

void MediaPlayerPrivateVita::playerReady()
{
    if (m_ready)
        return;
    m_ready = true;
    m_readyTimeout.stop();
    bool hadVideo = m_hasVideo;
    bool hadAudio = m_hasAudio;
    auto oldSize = m_naturalSize;
    auto& info = m_pipeline->info();
    m_hasVideo = info.hasVideo;
    m_hasAudio = info.hasAudio;
    if (m_hasVideo)
        m_naturalSize = { std::round(info.width * info.pixelAspectRatio), static_cast<float>(info.height) };
    auto duration = info.duration ? MediaTime::createWithDouble(info.duration->seconds()) : MediaTime::zeroTime();
    bool durationChanged = duration != m_duration;
    m_duration = duration;

    RefPtr player = m_player.get();
    if (!player)
        return;
    if (durationChanged)
        player->durationChanged();
    if (oldSize != m_naturalSize)
        player->sizeChanged();
    updateTracks();
    if (hadVideo != m_hasVideo || hadAudio != m_hasAudio)
        player->characteristicChanged();
    // The pipeline buffers as it plays; it has what it needs to start.
    setReadyState(MediaPlayer::ReadyState::HaveEnoughData);
    setNetworkState(MediaPlayer::NetworkState::Idle);
    if (!m_paused)
        startPlayback();
    else if (m_hasVideo)
        showFirstFrame();
    else
        scheduleLibraryYield();
}

// Paused, the pipeline decodes up to the first picture and the timer shows it.
void MediaPlayerPrivateVita::showFirstFrame()
{
    m_showingFirstFrame = true;
    m_firstFrameStarted = MonotonicTime::now();
    if (!m_frameTimer.isActive())
        m_frameTimer.startRepeating(16_ms);
}

void MediaPlayerPrivateVita::startPlayback()
{
    RefPtr pipeline = m_pipeline;
    if (!m_ready || !pipeline)
        return;
    m_showingFirstFrame = false;
    m_playbackStarted = MonotonicTime::now();
    pipeline->play();
    if (m_hasVideo && !m_frameTimer.isActive())
        m_frameTimer.startRepeating(16_ms);
}

void MediaPlayerPrivateVita::play()
{
    if (!m_paused)
        return;
    m_paused = false;
    if (m_ended) {
        m_ended = false;
        m_startTime = MediaTime::zeroTime();
        if (RefPtr pipeline = m_pipeline) {
            pipeline->seek(0_s);
            startPlayback();
        } else
            createPlayer();
    } else if (m_waitingForPreload)
        loadIfDeferred();
    else if (m_suspended || playersWaitingForLibrary().contains(this))
        createPlayer();
    else
        startPlayback();
    if (RefPtr player = m_player.get())
        player->playbackStateChanged();
}

void MediaPlayerPrivateVita::pause()
{
    if (m_paused)
        return;
    m_paused = true;
    m_playRequested = false;
    if (RefPtr pipeline = m_pipeline)
        pipeline->pause();
    // The timer stops once a seek in progress shows its frame.
    if (RefPtr player = m_player.get())
        player->playbackStateChanged();
    scheduleLibraryYield();
}

MediaTime MediaPlayerPrivateVita::duration() const
{
    return m_duration.isValid() ? m_duration : MediaTime::zeroTime();
}

MediaTime MediaPlayerPrivateVita::currentTime() const
{
    if (m_ended)
        return duration();
    if (m_seeking)
        return m_seekTarget;
    RefPtr pipeline = m_pipeline;
    if (!pipeline || !m_ready)
        return m_startTime;
    auto time = MediaTime::createWithDouble(pipeline->currentTime().seconds());
    return m_duration.isValid() && m_duration > MediaTime::zeroTime() ? std::min(time, m_duration) : time;
}

std::optional<VideoPlaybackQualityMetrics> MediaPlayerPrivateVita::videoPlaybackQualityMetrics()
{
    if (!m_hasVideo)
        return std::nullopt;
    uint32_t dropped = m_droppedFrames;
    if (RefPtr pipeline = m_pipeline)
        dropped += pipeline->droppedFrames();
    VideoPlaybackQualityMetrics metrics;
    metrics.totalVideoFrames = m_presentedFrames + dropped;
    metrics.droppedVideoFrames = dropped;
    metrics.displayCompositedVideoFrames = m_presentedFrames;
    return metrics;
}

void MediaPlayerPrivateVita::seekToTarget(const SeekTarget& target)
{
    m_seekTarget = std::max(target.time, MediaTime::zeroTime());
    if (m_duration.isValid() && m_duration > MediaTime::zeroTime())
        m_seekTarget = std::min(m_seekTarget, m_duration);
    m_seeking = true;
    m_ended = false;
    if (RefPtr pipeline = m_pipeline) {
        // Before Ready too: the pipeline opens the media there.
        pipeline->seek(Seconds { m_seekTarget.toDouble() });
        if (m_hasVideo) {
            if (m_paused)
                showFirstFrame();
            else if (!m_frameTimer.isActive())
                m_frameTimer.startRepeating(16_ms);
        }
        return;
    }
    // Starts there.
    m_startTime = m_seekTarget;
    callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }] {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->finishSeek();
    });
}

void MediaPlayerPrivateVita::finishSeek()
{
    if (!m_seeking)
        return;
    m_seeking = false;
    if (RefPtr player = m_player.get()) {
        player->seeked(m_seekTarget);
        player->timeChanged();
    }
}

void MediaPlayerPrivateVita::convertFrame(const VitaVideoFrame& frame)
{
    unsigned width = frame.width;
    unsigned height = frame.height;
    // Shown at half its size or less, and not read back by a canvas: a quarter
    // of the pixels to convert and to scale.
    bool halfSize = !m_paintedSize.isEmpty() && m_paintedSize.width() * 2 <= width && m_paintedSize.height() * 2 <= height
        && MonotonicTime::now() - m_lastImageRequest > 1_s;
    unsigned frameWidth = halfSize ? width / 2 : width;
    unsigned frameHeight = halfSize ? height / 2 : height;
    if (!m_frame || static_cast<unsigned>(cairo_image_surface_get_width(m_frame.get())) != frameWidth || static_cast<unsigned>(cairo_image_surface_get_height(m_frame.get())) != frameHeight) {
        m_frame = adoptRef(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, frameWidth, frameHeight));
        m_frameImage = NativeImage::create(RefPtr { m_frame });
    }
    if (halfSize)
        frame.interleavedChroma ? convertFrameAtHalfSize<true>(frame, m_frame.get()) : convertFrameAtHalfSize<false>(frame, m_frame.get());
    else
        frame.interleavedChroma ? WebCore::convertFrame<true>(frame, m_frame.get()) : WebCore::convertFrame<false>(frame, m_frame.get());
}

void MediaPlayerPrivateVita::updateFrame()
{
    RefPtr pipeline = m_pipeline;
    if (!pipeline)
        return;
    bool newFrame = m_hasVideo && pipeline->presentFrame([&](const VitaVideoFrame& frame) {
        convertFrame(frame);
    });
    if (newFrame) {
        ++m_presentedFrames;
        if (RefPtr player = m_player.get())
            player->repaint();
    }
    if (m_showingFirstFrame && (newFrame || MonotonicTime::now() - m_firstFrameStarted > 3_s)) {
        m_showingFirstFrame = false;
        scheduleLibraryYield();
    }
    if (m_paused && !m_seeking && !m_showingFirstFrame)
        m_frameTimer.stop();
}

void MediaPlayerPrivateVita::reachedEnd()
{
    if (m_ended)
        return;
    m_ended = true;
    RefPtr player = m_player.get();
    // A looping element seeks back to the start and plays on; reported paused,
    // the element would pause first.
    if (!player || !player->isLooping()) {
        m_paused = true;
        if (RefPtr pipeline = m_pipeline)
            pipeline->pause();
    }
    m_frameTimer.stop();
    if (player) {
        player->timeChanged();
        player->playbackStateChanged();
    }
    scheduleLibraryYield();
}

void MediaPlayerPrivateVita::fail(MediaPlayer::NetworkState state, const String& message)
{
    // A failed load explains more than what the player made of the bytes it got.
    auto loadFailure = m_stream ? m_stream->failure() : String { };
    m_errorMessage = loadFailure.isNull() ? message : loadFailure;
    destroyPlayer();
    setNetworkState(state);
}

void MediaPlayerPrivateVita::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateVita::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    m_readyState = state;
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

void MediaPlayerPrivateVita::setVolume(float volume)
{
    m_volume = volume;
    applyVolume();
}

void MediaPlayerPrivateVita::setMuted(bool muted)
{
    m_muted = muted;
    applyVolume();
}

void MediaPlayerPrivateVita::applyVolume()
{
    if (RefPtr pipeline = m_pipeline)
        pipeline->setVolume(m_muted ? 0 : m_volume);
}

const PlatformTimeRanges& MediaPlayerPrivateVita::buffered() const
{
    m_buffered.clear();
    RefPtr stream = m_stream;
    auto fraction = stream ? stream->loadedFraction() : std::nullopt;
    if (fraction && m_duration.isValid() && *fraction > 0)
        m_buffered.add(MediaTime::zeroTime(), MediaTime::createWithDouble(m_duration.toDouble() * *fraction));
    return m_buffered;
}

unsigned long long MediaPlayerPrivateVita::totalBytes() const
{
    RefPtr stream = m_stream;
    return stream ? stream->loadedBytes() : 0;
}

bool MediaPlayerPrivateVita::didLoadingProgress() const
{
    auto loaded = totalBytes();
    return std::exchange(m_lastReportedBytes, loaded) != loaded;
}

void MediaPlayerPrivateVita::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled() || !m_frameImage)
        return;
    m_paintedSize = context.getCTM().mapRect(rect).size();
    FloatRect source { 0, 0, static_cast<float>(cairo_image_surface_get_width(m_frame.get())), static_cast<float>(cairo_image_surface_get_height(m_frame.get())) };
    context.drawNativeImage(*m_frameImage, rect, source, { InterpolationQuality::Low });
}

RefPtr<NativeImage> MediaPlayerPrivateVita::nativeImageForCurrentTime()
{
    m_lastImageRequest = MonotonicTime::now();
    return m_frameImage;
}

DestinationColorSpace MediaPlayerPrivateVita::colorSpace()
{
    return DestinationColorSpace::SRGB();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
