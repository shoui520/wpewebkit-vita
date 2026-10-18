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

#include "GraphicsContext.h"
#include "MediaByteStream.h"
#include "MediaPlayer.h"
#include "NativeImage.h"
#include <arm_neon.h>
#include <cairo.h>
#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>

// In the library, though not in the SDK's header.
extern "C" {
int sceAvPlayerStreamCount(SceAvPlayerHandle);
int sceAvPlayerEnableStream(SceAvPlayerHandle, uint32_t stream);
}

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerPrivateVita);

// The video player library's state events.
enum : int32_t {
    PlayerStateStop = 1,
    PlayerStateReady = 2,
    PlayerStatePlay = 3,
    PlayerStatePause = 4,
    PlayerStateBuffering = 5,
};

// Where the library's events go: the player of one generation, on the main thread.
struct PlayerEventTarget {
    WTF_MAKE_STRUCT_TZONE_ALLOCATED(PlayerEventTarget);
    ThreadSafeWeakPtr<MediaPlayerPrivateVita> player;
    unsigned generation;
};

WTF_MAKE_STRUCT_TZONE_ALLOCATED_IMPL(PlayerEventTarget);

// Plays the player's audio on a system port from a thread of its own.
class MediaAudioOutput final : public ThreadSafeRefCounted<MediaAudioOutput> {
public:
    static Ref<MediaAudioOutput> create(int handle) { return adoptRef(*new MediaAudioOutput(handle)); }

    void start();
    void stop();
    // 0 to 1.
    void setVolume(float volume) { m_volume = std::clamp(static_cast<int>(volume * SCE_AUDIO_VOLUME_0DB), 0, SCE_AUDIO_VOLUME_0DB); }

private:
    explicit MediaAudioOutput(int handle)
        : m_handle(handle)
    {
    }

    void run();

    const int m_handle;
    std::atomic<bool> m_running { false };
    std::atomic<int> m_volume { SCE_AUDIO_VOLUME_0DB };
    RefPtr<Thread> m_thread;
};

void MediaAudioOutput::start()
{
    if (m_thread)
        return;
    m_running = true;
    m_thread = Thread::create("Media audio"_s, [protectedThis = Ref { *this }] {
        protectedThis->run();
    }, ThreadType::Audio, Thread::QOS::UserInteractive);
}

void MediaAudioOutput::stop()
{
    m_running = false;
    if (RefPtr thread = std::exchange(m_thread, nullptr))
        thread->waitForCompletion();
}

void MediaAudioOutput::run()
{
    int port = -1;
    unsigned portSamples = 0;
    unsigned portRate = 0;
    unsigned portChannels = 0;
    int portVolume = -1;
    while (m_running) {
        SceAvPlayerFrameInfo frame { };
        if (!sceAvPlayerGetAudioData(m_handle, &frame) || !frame.pData) {
            sleep(5_ms);
            continue;
        }
        unsigned channels = frame.details.audio.channelCount == 1 ? 1 : 2;
        unsigned samples = frame.details.audio.size / (2 * channels);
        unsigned rate = frame.details.audio.sampleRate;
        if (!samples || !rate)
            continue;
        auto mode = channels == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO;
        if (port < 0) {
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, samples, rate, mode);
            if (port < 0) {
                sleep(100_ms);
                continue;
            }
        } else if (samples != portSamples || rate != portRate || channels != portChannels)
            sceAudioOutSetConfig(port, samples, rate, mode);
        portSamples = samples;
        portRate = rate;
        portChannels = channels;
        if (int volume = m_volume; volume != portVolume) {
            int volumes[2] = { volume, volume };
            sceAudioOutSetVolume(port, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volumes);
            portVolume = volume;
        }
        // Blocks until the port takes the samples: this paces the thread.
        sceAudioOutOutput(port, frame.pData);
    }
    if (port >= 0) {
        sceAudioOutOutput(port, nullptr);
        sceAudioOutReleasePort(port);
    }
}

// The player's memory: frames go in physically contiguous memory that the
// decoder can write.
static void* allocate(void*, uint32_t alignment, uint32_t size)
{
    return tryFastAlignedMalloc(std::max<uint32_t>(alignment, sizeof(void*)), size);
}

static void deallocate(void*, void* pointer)
{
    fastAlignedFree(pointer);
}

static void* allocateFrameMemory(void*, uint32_t alignment, uint32_t size)
{
    constexpr uint32_t blockAlignment = 1 * MB;
    alignment = std::max(alignment, blockAlignment);
    size = (size + alignment - 1) & ~(alignment - 1);
    SceUID block = sceKernelAllocMemBlock("media frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, size, nullptr);
    if (block < 0)
        return nullptr;
    void* base = nullptr;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static void freeFrameMemory(void*, void* pointer)
{
    SceUID block = sceKernelFindMemBlockByAddr(pointer, 0);
    if (block >= 0)
        sceKernelFreeMemBlock(block);
}

static int openStream(void*, const char*)
{
    return 0;
}

static int closeStream(void*)
{
    return 0;
}

static int readStream(void* stream, uint8_t* buffer, uint64_t position, uint32_t length)
{
    return static_cast<MediaByteStream*>(stream)->read(buffer, position, length);
}

static uint64_t streamSize(void* stream)
{
    return static_cast<MediaByteStream*>(stream)->size();
}

static void playerEventCallback(void* target, int32_t event, int32_t, void*)
{
    auto& eventTarget = *static_cast<PlayerEventTarget*>(target);
    callOnMainThread([player = eventTarget.player, event, generation = eventTarget.generation] {
        if (RefPtr protectedPlayer = player.get())
            protectedPlayer->playerEvent(event, generation);
    });
}

// Frames from the decoder are NV21: a luma plane, then interleaved V and U at
// half resolution. They become Cairo's premultiplied BGRA with the limited
// range BT.601 or BT.709 matrix in 6-bit fixed point.
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

static void convertFrame(const uint8_t* frame, unsigned width, unsigned height, cairo_surface_t* surface)
{
    auto matrix = matrixForHeight(height);
    unsigned stride = (width + 7) & ~7u;
    const uint8_t* chroma = frame + stride * height;
    uint8_t* output = cairo_image_surface_get_data(surface);
    unsigned outputStride = cairo_image_surface_get_stride(surface);

    const uint8x8_t offset128 = vdup_n_u8(128);
    unsigned vectorWidth = width & ~15u;
    for (unsigned row = 0; row < height; row += 2) {
        const uint8_t* vu = chroma + (row / 2) * stride;
        for (unsigned line = row; line < std::min(row + 2, height); ++line) {
            const uint8_t* luma = frame + line * stride;
            uint8_t* out = output + line * outputStride;
            for (unsigned x = 0; x < vectorWidth; x += 16) {
                uint8x8x2_t pairs = vld2_u8(vu + x);
                int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(pairs.val[0], offset128));
                int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(pairs.val[1], offset128));
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
                storePixel(matrix, luma[x], vu[(x & ~1u) + 1] - 128, vu[x & ~1u] - 128, out + x * 4);
        }
    }
    cairo_surface_mark_dirty(surface);
}

// The same at half the width and height, for a video shown at half its size
// or less: each pixel averages four luma samples and takes the one chroma
// sample covering them.
static void convertFrameAtHalfSize(const uint8_t* frame, unsigned width, unsigned height, cairo_surface_t* surface)
{
    auto matrix = matrixForHeight(height);
    unsigned stride = (width + 7) & ~7u;
    const uint8_t* chroma = frame + stride * height;
    uint8_t* output = cairo_image_surface_get_data(surface);
    unsigned outputStride = cairo_image_surface_get_stride(surface);

    const uint8x8_t offset128 = vdup_n_u8(128);
    unsigned outputWidth = width / 2;
    unsigned vectorWidth = outputWidth & ~15u;
    for (unsigned row = 0; row < height / 2; ++row) {
        const uint8_t* top = frame + 2 * row * stride;
        const uint8_t* bottom = top + stride;
        const uint8_t* vu = chroma + row * stride;
        uint8_t* out = output + row * outputStride;
        for (unsigned x = 0; x < vectorWidth; x += 16) {
            uint8x16x2_t topPairs = vld2q_u8(top + 2 * x);
            uint8x16x2_t bottomPairs = vld2q_u8(bottom + 2 * x);
            uint8x16_t y = vrhaddq_u8(vhaddq_u8(topPairs.val[0], topPairs.val[1]), vhaddq_u8(bottomPairs.val[0], bottomPairs.val[1]));
            uint8x16x2_t pairs = vld2q_u8(vu + 2 * x);
            for (int half = 0; half < 2; ++half) {
                int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(half ? vget_high_u8(pairs.val[0]) : vget_low_u8(pairs.val[0]), offset128));
                int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(half ? vget_high_u8(pairs.val[1]) : vget_low_u8(pairs.val[1]), offset128));
                int16x8_t green = vmlsq_n_s16(vmulq_n_s16(u, -matrix.gu), v, matrix.gv);
                storePixels(matrix, half ? vget_high_u8(y) : vget_low_u8(y), vmulq_n_s16(v, matrix.rv), green, vmulq_n_s16(u, matrix.bu), out + (x + half * 8) * 4);
            }
        }
        for (unsigned x = vectorWidth; x < outputWidth; ++x) {
            // Rounded as the vector loop does.
            int luma = (((top[2 * x] + top[2 * x + 1]) >> 1) + ((bottom[2 * x] + bottom[2 * x + 1]) >> 1) + 1) >> 1;
            storePixel(matrix, luma, vu[2 * x + 1] - 128, vu[2 * x] - 128, out + x * 4);
        }
    }
    cairo_surface_mark_dirty(surface);
}

static HashSet<String>& mimeTypeCache()
{
    static NeverDestroyed<HashSet<String>> cache = [] {
        HashSet<String> types;
        for (auto type : { "video/mp4"_s, "audio/mp4"_s, "video/x-m4v"_s, "audio/x-m4a"_s, "video/quicktime"_s })
            types.add(type);
        return types;
    }();
    return cache;
}

void MediaPlayerPrivateVita::getSupportedTypes(HashSet<String>& types)
{
    types = mimeTypeCache();
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
    // H.264 and AAC.
    for (auto& codec : codecs) {
        if (!codec.startsWith("avc1"_s) && !codec.startsWith("avc3"_s) && !codec.startsWith("mp4a.40"_s))
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

MediaPlayerPrivateVita::MediaPlayerPrivateVita(MediaPlayer& player)
    : m_player(player)
    , m_frameTimer(RunLoop::mainSingleton(), "MediaPlayerPrivateVita::FrameTimer"_s, this, &MediaPlayerPrivateVita::updateFrame)
    , m_readyTimeout(RunLoop::mainSingleton(), "MediaPlayerPrivateVita::ReadyTimeout"_s, [this] { fail(MediaPlayer::NetworkState::FormatError, "The system video player did not open the media in time"_s); })
{
}

MediaPlayerPrivateVita::~MediaPlayerPrivateVita()
{
    destroyPlayer();
}

void MediaPlayerPrivateVita::load(const String& url)
{
    destroyPlayer();
    m_url = URL { url };
    m_errorMessage = { };
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
    if (!std::exchange(m_waitingForPreload, false))
        return;
    setNetworkState(MediaPlayer::NetworkState::Loading);
    createPlayer();
}

void MediaPlayerPrivateVita::setPreload(MediaPlayer::Preload preload)
{
    if (preload != MediaPlayer::Preload::None)
        prepareToPlay();
}

void MediaPlayerPrivateVita::cancelLoad()
{
    destroyPlayer();
    m_waitingForPreload = false;
    setNetworkState(MediaPlayer::NetworkState::Idle);
}

void MediaPlayerPrivateVita::createPlayer()
{
    RefPtr player = m_player.get();
    if (!player || m_handle)
        return;
    static int moduleResult = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
    if (moduleResult < 0) {
        fail(MediaPlayer::NetworkState::DecodeError, makeString("The system video player could not be loaded (0x"_s, hex(static_cast<uint32_t>(moduleResult), 8), ')'));
        return;
    }

    m_stream = MediaByteStream::create(player->mediaResourceLoader(), m_url);
    m_eventTarget = makeUnique<PlayerEventTarget>(PlayerEventTarget { *this, ++m_generation });
    SceAvPlayerInitData init { };
    init.memoryReplacement.allocate = allocate;
    init.memoryReplacement.deallocate = deallocate;
    init.memoryReplacement.allocateTexture = allocateFrameMemory;
    init.memoryReplacement.deallocateTexture = freeFrameMemory;
    init.fileReplacement.objectPointer = m_stream.get();
    init.fileReplacement.open = openStream;
    init.fileReplacement.close = closeStream;
    init.fileReplacement.readOffset = readStream;
    init.fileReplacement.size = streamSize;
    init.eventReplacement.objectPointer = m_eventTarget.get();
    init.eventReplacement.eventCallback = playerEventCallback;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 3;
    init.autoStart = false;
    // The handle is an address, so negative as an int; null on failure.
    m_handle = sceAvPlayerInit(&init);
    if (!m_handle) {
        m_stream->close();
        m_stream = nullptr;
        fail(MediaPlayer::NetworkState::DecodeError, "The system video player could not start"_s);
        return;
    }
    m_stream->start();
    // The name goes to the file replacement; the library may judge the container by it.
    if (int result = sceAvPlayerAddSource(m_handle, "media.mp4"); result < 0) {
        fail(MediaPlayer::NetworkState::FormatError, makeString("The system video player did not accept the media (0x"_s, hex(static_cast<uint32_t>(result), 8), ')'));
        return;
    }
    m_readyTimeout.startOneShot(60_s);
}

void MediaPlayerPrivateVita::destroyPlayer()
{
    m_frameTimer.stop();
    m_readyTimeout.stop();
    // Reads waiting for data give up, so the player's threads can finish.
    if (RefPtr stream = m_stream)
        stream->close();
    if (RefPtr audio = std::exchange(m_audio, nullptr))
        audio->stop();
    if (m_handle) {
        sceAvPlayerStop(m_handle);
        sceAvPlayerClose(m_handle);
        m_handle = 0;
    }
    m_eventTarget = nullptr;
    m_stream = nullptr;
    m_started = false;
    m_showingFirstFrame = false;
    m_ready = false;
    ++m_generation;
}

void MediaPlayerPrivateVita::playerEvent(int32_t event, unsigned generation)
{
    if (generation != m_generation || !m_handle)
        return;
    switch (event) {
    case PlayerStateReady:
        playerReady();
        break;
    case PlayerStateStop:
        if (!m_ready)
            fail(MediaPlayer::NetworkState::FormatError, "The system video player could not play the media"_s);
        else if (m_started)
            reachedEnd();
        break;
    default:
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
    MediaTime duration = MediaTime::zeroTime();
    int streams = sceAvPlayerStreamCount(m_handle);
    for (int i = 0; i < streams; ++i) {
        SceAvPlayerStreamInfo info { };
        if (sceAvPlayerGetStreamInfo(m_handle, i, &info) < 0)
            continue;
        if (info.type == SCE_AVPLAYER_VIDEO) {
            m_hasVideo = true;
            float width = info.details.video.width;
            float height = info.details.video.height;
            // The shape of a pixel (1 for square ones), not of the picture.
            if (info.details.video.aspectRatio > 0)
                width = std::round(width * info.details.video.aspectRatio);
            m_naturalSize = { width, height };
        } else if (info.type == SCE_AVPLAYER_AUDIO)
            m_hasAudio = true;
        else
            continue;
        sceAvPlayerEnableStream(m_handle, i);
        duration = std::max(duration, MediaTime(static_cast<int64_t>(info.duration), 1000));
    }
    if (!m_hasVideo && !m_hasAudio) {
        fail(MediaPlayer::NetworkState::FormatError, "The media has no stream the system video player can play"_s);
        return;
    }
    bool durationChanged = duration != m_duration;
    m_duration = duration;

    RefPtr player = m_player.get();
    if (!player)
        return;
    if (durationChanged)
        player->durationChanged();
    if (oldSize != m_naturalSize)
        player->sizeChanged();
    if (hadVideo != m_hasVideo || hadAudio != m_hasAudio)
        player->characteristicChanged();
    // The library buffers as it plays; it has what it needs to start.
    setReadyState(MediaPlayer::ReadyState::HaveEnoughData);
    setNetworkState(MediaPlayer::NetworkState::Idle);
    if (!m_paused)
        startPlayback();
    else if (m_hasVideo)
        showFirstFrame();
}

void MediaPlayerPrivateVita::showFirstFrame()
{
    if (m_started || !m_handle)
        return;
    m_started = true;
    m_showingFirstFrame = true;
    m_firstFrameStarted = MonotonicTime::now();
    sceAvPlayerStart(m_handle);
    if (m_startTime > MediaTime::zeroTime())
        sceAvPlayerJumpToTime(m_handle, static_cast<uint64_t>(m_startTime.toDouble() * 1000));
    m_frameTimer.startRepeating(16_ms);
}

void MediaPlayerPrivateVita::startPlayback()
{
    if (!m_ready || !m_handle)
        return;
    m_showingFirstFrame = false;
    if (!m_started) {
        m_started = true;
        sceAvPlayerStart(m_handle);
        if (m_startTime > MediaTime::zeroTime())
            sceAvPlayerJumpToTime(m_handle, static_cast<uint64_t>(m_startTime.toDouble() * 1000));
    } else
        sceAvPlayerResume(m_handle);
    if (m_hasAudio && !m_audio) {
        m_audio = MediaAudioOutput::create(m_handle);
        applyVolume();
        m_audio->start();
    }
    m_frameTimer.startRepeating(16_ms);
}

void MediaPlayerPrivateVita::play()
{
    if (!m_paused)
        return;
    m_paused = false;
    if (m_ended) {
        // The library stops at the end: playing again takes a new player.
        m_ended = false;
        m_startTime = MediaTime::zeroTime();
        destroyPlayer();
        createPlayer();
    } else if (m_waitingForPreload)
        prepareToPlay();
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
    if (m_started && m_handle)
        sceAvPlayerPause(m_handle);
    m_frameTimer.stop();
    if (RefPtr player = m_player.get())
        player->playbackStateChanged();
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
    if (!m_started || m_showingFirstFrame || !m_handle)
        return m_startTime;
    return MediaTime(sceAvPlayerCurrentTime(m_handle), 1000);
}

std::optional<VideoPlaybackQualityMetrics> MediaPlayerPrivateVita::videoPlaybackQualityMetrics()
{
    if (!m_hasVideo)
        return std::nullopt;
    VideoPlaybackQualityMetrics metrics;
    metrics.totalVideoFrames = m_presentedFrames + m_droppedFrames;
    metrics.droppedVideoFrames = m_droppedFrames;
    metrics.displayCompositedVideoFrames = m_presentedFrames;
    return metrics;
}

void MediaPlayerPrivateVita::seekToTarget(const SeekTarget& target)
{
    // Frames after the jump do not follow those before it.
    m_lastFrameTime = std::nullopt;
    m_seekTarget = std::clamp(target.time, MediaTime::zeroTime(), duration());
    if (m_ended || !m_started || !m_handle) {
        // Starts there.
        m_startTime = m_seekTarget;
        if (m_ended) {
            m_ended = false;
            destroyPlayer();
            if (!m_waitingForPreload)
                createPlayer();
        }
        m_seeking = true;
        callOnMainThread([weakThis = ThreadSafeWeakPtr { *this }] {
            if (RefPtr protectedThis = weakThis.get())
                protectedThis->finishSeek();
        });
        return;
    }
    m_seeking = true;
    m_seekStarted = MonotonicTime::now();
    sceAvPlayerJumpToTime(m_handle, static_cast<uint64_t>(m_seekTarget.toDouble() * 1000));
    if (!m_frameTimer.isActive())
        m_frameTimer.startRepeating(16_ms);
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

void MediaPlayerPrivateVita::updateFrame()
{
    if (!m_handle)
        return;
    SceAvPlayerFrameInfo frame { };
    bool newFrame = m_hasVideo && sceAvPlayerGetVideoData(m_handle, &frame) && frame.pData;
    if (newFrame) {
        if (m_lastFrameTime && frame.timeStamp > *m_lastFrameTime) {
            uint64_t step = frame.timeStamp - *m_lastFrameTime;
            if (!m_frameInterval || step < m_frameInterval)
                m_frameInterval = step;
            m_droppedFrames += (step + m_frameInterval / 2) / m_frameInterval - 1;
        }
        m_lastFrameTime = frame.timeStamp;
        ++m_presentedFrames;
        unsigned width = frame.details.video.width;
        unsigned height = frame.details.video.height;
        // Shown at half its size or less, and not read back by a canvas: a
        // quarter of the pixels to convert and to scale.
        bool halfSize = !m_paintedSize.isEmpty() && m_paintedSize.width() * 2 <= width && m_paintedSize.height() * 2 <= height
            && MonotonicTime::now() - m_lastImageRequest > 1_s;
        unsigned frameWidth = halfSize ? width / 2 : width;
        unsigned frameHeight = halfSize ? height / 2 : height;
        if (!m_frame || static_cast<unsigned>(cairo_image_surface_get_width(m_frame.get())) != frameWidth || static_cast<unsigned>(cairo_image_surface_get_height(m_frame.get())) != frameHeight) {
            m_frame = adoptRef(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, frameWidth, frameHeight));
            m_frameImage = NativeImage::create(RefPtr { m_frame });
        }
        if (halfSize)
            convertFrameAtHalfSize(frame.pData, width, height, m_frame.get());
        else
            convertFrame(frame.pData, width, height, m_frame.get());
        if (RefPtr player = m_player.get())
            player->repaint();
    }
    if (m_showingFirstFrame && (newFrame || MonotonicTime::now() - m_firstFrameStarted > 3_s)) {
        m_showingFirstFrame = false;
        sceAvPlayerPause(m_handle);
    }
    // The library goes inactive at the end of the media.
    if (m_started && !m_paused && !m_seeking && !sceAvPlayerIsActive(m_handle)) {
        reachedEnd();
        return;
    }
    if (m_seeking) {
        // The jump is done once a frame of the new position shows, or the clock moved.
        double now = sceAvPlayerCurrentTime(m_handle) / 1000.0;
        if (newFrame || std::abs(now - m_seekTarget.toDouble()) < 0.5 || MonotonicTime::now() - m_seekStarted > 2_s)
            finishSeek();
    }
    if (m_paused && !m_seeking && !m_showingFirstFrame)
        m_frameTimer.stop();
}

void MediaPlayerPrivateVita::reachedEnd()
{
    if (m_ended)
        return;
    m_ended = true;
    m_paused = true;
    m_frameTimer.stop();
    if (RefPtr audio = std::exchange(m_audio, nullptr))
        audio->stop();
    if (RefPtr player = m_player.get()) {
        player->timeChanged();
        player->playbackStateChanged();
    }
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
    if (RefPtr audio = m_audio)
        audio->setVolume(m_muted ? 0 : m_volume);
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
