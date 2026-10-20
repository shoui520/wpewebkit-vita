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
#include "VitaVideoDecoder.h"

#if ENABLE(VIDEO) && PLATFORM(VITA)

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>
#include <wtf/HexNumber.h>
#include <wtf/Lock.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>

// In the SDK's stubs, though not in its header. The non-game entry points take
// pictures up to 1920x1088; the game ones stop at 1280x720 pixels.
int sceVideodecInitLibraryNongameapp(SceVideodecType, const SceVideodecQueryInitInfoHwAvcdec*);
int sceAvcdecQueryDecoderMemSizeNongameapp(SceVideodecType, const SceAvcdecQueryDecoderInfo*, SceAvcdecDecoderInfo*);
int sceAvcdecCreateDecoderNongameapp(SceVideodecType, SceAvcdecCtrl*, const SceAvcdecQueryDecoderInfo*);
int sceAvcdecDecodeStop(const SceAvcdecCtrl*, SceAvcdecArrayPicture*);
int sceAvcdecDecodeFlush(const SceAvcdecCtrl*);
}

namespace WebCore {

static String errorCode(int code)
{
    return makeString("0x"_s, hex(static_cast<uint32_t>(code), 8));
}

// A block of memory for the decoder: video memory, or physically contiguous
// main memory when that runs out; access units can be in ordinary memory.
class DecoderMemory {
    WTF_MAKE_NONCOPYABLE(DecoderMemory);
public:
    DecoderMemory() = default;
    ~DecoderMemory()
    {
        if (m_block >= 0)
            sceKernelFreeMemBlock(m_block);
    }

    // Video memory comes in 256 KiB units; physically contiguous memory in 1 MiB
    // units, aligned to them, and refuses an alignment option.
    int allocate(const char* name, size_t size, size_t alignment)
    {
        int result = allocate(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, roundUp(size, std::max<size_t>(alignment, 256 * KB)), alignment);
        if (result < 0)
            result = allocate(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, roundUp(size, 1 * MB), 0);
        return result;
    }

    int allocateUnits(const char* name, size_t size)
    {
        return allocate(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, roundUp(size, 4 * KB), 0);
    }

    uint8_t* base() const { return static_cast<uint8_t*>(m_base); }
    size_t size() const { return m_size; }

private:
    static size_t roundUp(size_t size, size_t unit) { return (size + unit - 1) & ~(unit - 1); }

    int allocate(const char* name, SceKernelMemBlockType type, size_t size, size_t alignment)
    {
        SceKernelAllocMemBlockOpt options { };
        options.size = 0x14;
        options.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
        options.alignment = alignment;
        int result = sceKernelAllocMemBlock(name, type, size, alignment ? &options : nullptr);
        if (result < 0)
            return result;
        m_block = result;
        m_size = size;
        sceKernelGetMemBlockBase(m_block, &m_base);
        return 0;
    }

    SceUID m_block { -1 };
    void* m_base { nullptr };
    size_t m_size { 0 };
};

// The hardware decoder. A call taking an access unit hands back at most one
// picture, and the decoder drops pictures that are finished but not taken
// before the next ones: after each unit, calls with an empty unit take the
// rest, waiting a moment for one still being decoded. Each call waits on the
// decoder for a few milliseconds without using the CPU.
class HardwareAvcDecoder final : public VitaVideoDecoder {
public:
    static std::unique_ptr<HardwareAvcDecoder> create(unsigned width, unsigned height, String& failure);
    ~HardwareAvcDecoder();

    String name() const final { return makeString("hardware H.264 "_s, m_width, 'x', m_height); }
    unsigned maximumHeldPictures() const final { return m_buffers.size() - 2; }
    bool decode(std::span<const uint8_t>, std::optional<Seconds>, std::optional<Seconds>, const Output&) final;
    bool drain(const Output&) final;
    void flush() final;

private:
    HardwareAvcDecoder() = default;

    enum class Call : uint8_t { Unit, Empty, Stop };
    // Makes a call with a free picture buffer; the picture it returns goes to OUTPUT.
    // Returns the call's result, or 1 when no buffer was free.
    int call(Call, const SceAvcdecAu*, const Output*, bool& gotPicture);
    void releaseBuffer(unsigned index);

    // Timestamps go to the decoder in 90 kHz units, offset so none is negative.
    // The decoder gave no pictures at all for timestamps past 32 bits.
    static constexpr double timeOffset = 10;
    static SceVideodecTimeStamp toTimeStamp(std::optional<Seconds>);

    unsigned m_width { 0 };
    unsigned m_height { 0 };
    SceAvcdecCtrl m_decoder { };
    bool m_created { false };
    DecoderMemory m_frameMemory;
    DecoderMemory m_unitMemory;
    DecoderMemory m_pictureMemory;
    size_t m_pictureSize { 0 };
    Vector<uint8_t*> m_buffers;
    Lock m_lock;
    Vector<unsigned> m_freeBuffers WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_errors { 0 };
    // Shared with released frames, which can outlive the decoder.
    std::shared_ptr<HardwareAvcDecoder*> m_self;
};

static int initializeHardwareLibrary()
{
    static int result = [] {
        sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
        SceVideodecQueryInitInfoHwAvcdec init { };
        init.size = sizeof(init);
        init.horizontal = 1920;
        init.vertical = 1088;
        init.numOfRefFrames = 3;
        init.numOfStreams = 1;
        return sceVideodecInitLibraryNongameapp(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    }();
    return result;
}

std::unique_ptr<HardwareAvcDecoder> HardwareAvcDecoder::create(unsigned width, unsigned height, String& failure)
{
    if (int result = initializeHardwareLibrary(); result < 0) {
        failure = makeString("The hardware video decoder did not start ("_s, errorCode(result), ')');
        return nullptr;
    }
    std::unique_ptr<HardwareAvcDecoder> decoder { new HardwareAvcDecoder };
    decoder->m_width = (width + 15) & ~15u;
    decoder->m_height = (height + 15) & ~15u;
    // The decoder takes three reference frames at 1920x1088 (frame memory for
    // 3 * frames * pixels, up to 24 MiB).
    SceAvcdecQueryDecoderInfo query { decoder->m_width, decoder->m_height, 3 };
    SceAvcdecDecoderInfo info { };
    if (int result = sceAvcdecQueryDecoderMemSizeNongameapp(SCE_VIDEODEC_TYPE_HW_AVCDEC, &query, &info); result < 0) {
        failure = makeString("The hardware video decoder does not take "_s, width, 'x', height, " ("_s, errorCode(result), ')');
        return nullptr;
    }
    if (int result = decoder->m_frameMemory.allocate("video decoder", info.frameMemSize, 1 * MB); result < 0) {
        failure = makeString("No memory for the hardware video decoder's frames ("_s, errorCode(result), ')');
        return nullptr;
    }
    decoder->m_decoder.frameBuf.pBuf = decoder->m_frameMemory.base();
    decoder->m_decoder.frameBuf.size = info.frameMemSize;
    if (int result = sceAvcdecCreateDecoderNongameapp(SCE_VIDEODEC_TYPE_HW_AVCDEC, &decoder->m_decoder, &query); result < 0) {
        failure = makeString("The hardware video decoder did not open ("_s, errorCode(result), ')');
        return nullptr;
    }
    decoder->m_created = true;

    // An access unit is at most 3/4 of a byte per pixel.
    if (int result = decoder->m_unitMemory.allocateUnits("video units", decoder->m_width * decoder->m_height * 3 / 4); result < 0) {
        failure = makeString("No memory for video units ("_s, errorCode(result), ')');
        return nullptr;
    }
    // NV12 pictures: two waiting to be shown at 1080p, three below, one being
    // shown, and one for each call.
    unsigned buffers = decoder->m_width * decoder->m_height > 1280 * 720 ? 4 : 5;
    decoder->m_pictureSize = (decoder->m_width * decoder->m_height * 3 / 2 + 255) & ~size_t(255);
    if (int result = decoder->m_pictureMemory.allocate("video pictures", decoder->m_pictureSize * buffers, 256 * KB); result < 0) {
        failure = makeString("No memory for video pictures ("_s, errorCode(result), ')');
        return nullptr;
    }
    Locker locker { decoder->m_lock };
    for (unsigned i = 0; i < buffers; ++i) {
        decoder->m_buffers.append(decoder->m_pictureMemory.base() + i * decoder->m_pictureSize);
        decoder->m_freeBuffers.append(i);
    }
    decoder->m_self = std::make_shared<HardwareAvcDecoder*>(decoder.get());
    return decoder;
}

HardwareAvcDecoder::~HardwareAvcDecoder()
{
    {
        Locker locker { m_lock };
        if (m_self)
            *m_self = nullptr;
    }
    if (m_created)
        sceAvcdecDeleteDecoder(&m_decoder);
}

SceVideodecTimeStamp HardwareAvcDecoder::toTimeStamp(std::optional<Seconds> time)
{
    if (!time)
        return { 0xffffffff, 0xffffffff };
    auto units = static_cast<uint64_t>(std::max(0.0, (time->seconds() + timeOffset) * 90000 + 0.5));
    return { static_cast<uint32_t>(units >> 32), static_cast<uint32_t>(units) };
}

void HardwareAvcDecoder::releaseBuffer(unsigned index)
{
    Locker locker { m_lock };
    m_freeBuffers.append(index);
}

int HardwareAvcDecoder::call(Call kind, const SceAvcdecAu* unit, const Output* output, bool& gotPicture)
{
    gotPicture = false;
    std::optional<unsigned> buffer;
    {
        Locker locker { m_lock };
        if (!m_freeBuffers.isEmpty())
            buffer = m_freeBuffers.takeLast();
    }
    if (!buffer)
        return 1;

    SceAvcdecPicture picture { };
    picture.size = sizeof(picture);
    picture.frame.pixelType = SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER;
    picture.frame.framePitch = m_width;
    picture.frame.frameWidth = m_width;
    picture.frame.frameHeight = m_height;
    picture.frame.pPicture[0] = m_buffers[*buffer];
    SceAvcdecPicture* pictures[1] = { &picture };
    SceAvcdecArrayPicture array { 0, 1, pictures };
    int result;
    if (kind == Call::Stop)
        result = sceAvcdecDecodeStop(&m_decoder, &array);
    else
        result = sceAvcdecDecode(&m_decoder, unit, &array);
    if (result < 0 || !array.numOfOutput || !output) {
        releaseBuffer(*buffer);
        gotPicture = result >= 0 && array.numOfOutput;
        return result;
    }
    gotPicture = true;

    auto& frame = picture.frame;
    unsigned cropLeft = frame.frameCropLeftOffset & ~1u;
    unsigned cropTop = frame.frameCropTopOffset & ~1u;
    VitaVideoFrame decoded;
    decoded.width = frame.horizontalSize - std::min(frame.horizontalSize, frame.frameCropLeftOffset + frame.frameCropRightOffset);
    decoded.height = frame.verticalSize - std::min(frame.verticalSize, frame.frameCropTopOffset + frame.frameCropBottomOffset);
    decoded.lumaPitch = frame.framePitch;
    decoded.chromaPitch = frame.framePitch;
    decoded.interleavedChroma = true;
    auto* base = static_cast<uint8_t*>(frame.pPicture[0]);
    decoded.luma = base + cropTop * frame.framePitch + cropLeft;
    decoded.u = base + frame.framePitch * m_height + (cropTop / 2) * frame.framePitch + cropLeft;
    decoded.v = decoded.u + 1;
    uint64_t units = static_cast<uint64_t>(picture.info.pts.upper) << 32 | picture.info.pts.lower;
    decoded.time = Seconds { units / 90000.0 - timeOffset };
    decoded.release = [self = m_self, index = *buffer] {
        if (auto* decoder = *self)
            decoder->releaseBuffer(index);
    };
    (*output)(WTF::move(decoded));
    return result;
}

bool HardwareAvcDecoder::decode(std::span<const uint8_t> data, std::optional<Seconds> presentationTime, std::optional<Seconds> decodeTime, const Output& output)
{
    if (data.size() > m_unitMemory.size())
        return ++m_errors < 30;
    memcpy(m_unitMemory.base(), data.data(), data.size());
    SceAvcdecAu unit { };
    unit.pts = toTimeStamp(presentationTime);
    unit.dts = toTimeStamp(decodeTime);
    unit.es = { m_unitMemory.base(), static_cast<uint32_t>(data.size()) };
    bool gotPicture;
    int result = call(Call::Unit, &unit, &output, gotPicture);
    if (result == 1) {
        // No buffer: the caller holds too many pictures.
        return false;
    }
    if (result < 0)
        return ++m_errors < 30;

    // The pictures finished beyond the one returned; up to three empty calls,
    // a millisecond apart once one comes back empty.
    SceAvcdecAu empty { };
    empty.pts = toTimeStamp(std::nullopt);
    empty.dts = toTimeStamp(std::nullopt);
    unsigned emptyCalls = 0;
    while (emptyCalls < 3) {
        if (emptyCalls)
            sceKernelDelayThread(1000);
        if (call(Call::Empty, &empty, &output, gotPicture))
            break;
        if (!gotPicture)
            ++emptyCalls;
    }
    return true;
}

bool HardwareAvcDecoder::drain(const Output& output)
{
    for (unsigned i = 0; i < 32; ++i) {
        bool gotPicture;
        int result = call(Call::Stop, nullptr, &output, gotPicture);
        if (result == 1)
            return true;
        if (result || !gotPicture)
            break;
    }
    return false;
}

void HardwareAvcDecoder::flush()
{
    for (unsigned i = 0; i < 32; ++i) {
        bool gotPicture;
        if (call(Call::Stop, nullptr, nullptr, gotPicture) || !gotPicture)
            break;
    }
    sceAvcdecDecodeFlush(&m_decoder);
}

// FFmpeg's decoders, on threads of their own.
class SoftwareVideoDecoder final : public VitaVideoDecoder {
public:
    static std::unique_ptr<SoftwareVideoDecoder> create(const AVCodecParameters&, String& failure);
    ~SoftwareVideoDecoder();

    String name() const final { return makeString("software "_s, String::fromLatin1(m_codec->codec->name)); }
    unsigned maximumHeldPictures() const final { return 4; }
    bool decode(std::span<const uint8_t>, std::optional<Seconds>, std::optional<Seconds>, const Output&) final;
    bool drain(const Output&) final;
    void flush() final { avcodec_flush_buffers(m_codec); }

private:
    SoftwareVideoDecoder() = default;
    bool receive(const Output&);

    AVCodecContext* m_codec { nullptr };
    AVPacket* m_packet { nullptr };
    unsigned m_errors { 0 };
};

static constexpr AVRational microseconds { 1, 1000000 };

std::unique_ptr<SoftwareVideoDecoder> SoftwareVideoDecoder::create(const AVCodecParameters& parameters, String& failure)
{
    const AVCodec* codec = avcodec_find_decoder(parameters.codec_id);
    if (!codec) {
        failure = makeString("No decoder for "_s, String::fromLatin1(avcodec_get_name(parameters.codec_id)), " video"_s);
        return nullptr;
    }
    std::unique_ptr<SoftwareVideoDecoder> decoder { new SoftwareVideoDecoder };
    decoder->m_codec = avcodec_alloc_context3(codec);
    decoder->m_packet = av_packet_alloc();
    if (!decoder->m_codec || !decoder->m_packet || avcodec_parameters_to_context(decoder->m_codec, &parameters) < 0) {
        failure = "Out of memory"_s;
        return nullptr;
    }
    decoder->m_codec->pkt_timebase = microseconds;
    // The main thread has core 1: three threads for the others.
    decoder->m_codec->thread_count = 3;
    decoder->m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (int error = avcodec_open2(decoder->m_codec, codec, nullptr); error < 0) {
        failure = makeString("The "_s, String::fromLatin1(codec->name), " decoder did not open"_s);
        return nullptr;
    }
    return decoder;
}

SoftwareVideoDecoder::~SoftwareVideoDecoder()
{
    av_packet_free(&m_packet);
    avcodec_free_context(&m_codec);
}

bool SoftwareVideoDecoder::receive(const Output& output)
{
    while (true) {
        AVFrame* frame = av_frame_alloc();
        if (!frame)
            return false;
        int result = avcodec_receive_frame(m_codec, frame);
        if (result < 0) {
            av_frame_free(&frame);
            return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
        }
        if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUVJ420P) {
            av_frame_free(&frame);
            return false;
        }
        VitaVideoFrame decoded;
        decoded.width = frame->width;
        decoded.height = frame->height;
        decoded.luma = frame->data[0];
        decoded.u = frame->data[1];
        decoded.v = frame->data[2];
        decoded.lumaPitch = frame->linesize[0];
        decoded.chromaPitch = frame->linesize[1];
        int64_t time = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        decoded.time = Seconds { time == AV_NOPTS_VALUE ? 0 : time / 1000000.0 };
        decoded.release = [frame]() mutable {
            av_frame_free(&frame);
        };
        output(WTF::move(decoded));
    }
}

bool SoftwareVideoDecoder::decode(std::span<const uint8_t> data, std::optional<Seconds> presentationTime, std::optional<Seconds> decodeTime, const Output& output)
{
    av_packet_unref(m_packet);
    if (av_new_packet(m_packet, data.size()) < 0)
        return false;
    memcpy(m_packet->data, data.data(), data.size());
    m_packet->pts = presentationTime ? static_cast<int64_t>(presentationTime->seconds() * 1000000) : AV_NOPTS_VALUE;
    m_packet->dts = decodeTime ? static_cast<int64_t>(decodeTime->seconds() * 1000000) : AV_NOPTS_VALUE;
    m_packet->time_base = microseconds;
    int result;
    while ((result = avcodec_send_packet(m_codec, m_packet)) == AVERROR(EAGAIN)) {
        if (!receive(output))
            return false;
    }
    if (result < 0)
        return ++m_errors < 30;
    if (!receive(output))
        return false;
    return true;
}

bool SoftwareVideoDecoder::drain(const Output& output)
{
    avcodec_send_packet(m_codec, nullptr);
    receive(output);
    return false;
}

std::unique_ptr<VitaVideoDecoder> VitaVideoDecoder::create(const AVCodecParameters& parameters, String& failure)
{
    // The hardware decodes 8-bit 4:2:0 Baseline, Main and High profile H.264 up to
    // 1920x1088; FFmpeg decodes the rest, more slowly.
    bool hardwareProfile = parameters.profile == AV_PROFILE_UNKNOWN || parameters.profile == AV_PROFILE_H264_BASELINE
        || parameters.profile == AV_PROFILE_H264_CONSTRAINED_BASELINE || parameters.profile == AV_PROFILE_H264_MAIN
        || parameters.profile == AV_PROFILE_H264_HIGH;
    bool hardwareFormat = parameters.format == AV_PIX_FMT_NONE || parameters.format == AV_PIX_FMT_YUV420P || parameters.format == AV_PIX_FMT_YUVJ420P;
    unsigned width = (parameters.width + 15) & ~15;
    unsigned height = (parameters.height + 15) & ~15;
    if (parameters.codec_id == AV_CODEC_ID_H264 && hardwareProfile && hardwareFormat && width && height && width <= 1920 && height <= 1088) {
        String hardwareFailure;
        if (auto decoder = HardwareAvcDecoder::create(parameters.width, parameters.height, hardwareFailure))
            return decoder;
        failure = hardwareFailure;
    }
    if (auto decoder = SoftwareVideoDecoder::create(parameters, failure))
        return decoder;
    return nullptr;
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
