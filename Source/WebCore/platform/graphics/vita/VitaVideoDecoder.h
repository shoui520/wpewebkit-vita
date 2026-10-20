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

#include <memory>
#include <optional>
#include <span>
#include <wtf/Function.h>
#include <wtf/Seconds.h>
#include <wtf/text/WTFString.h>

struct AVCodecParameters;

namespace WebCore {

// A decoded picture in 8-bit 4:2:0: NV12 (U and V interleaved) from the
// hardware, I420 (U and V planes) from FFmpeg.
struct VitaVideoFrame {
    const uint8_t* luma { nullptr };
    // With interleaved chroma, V follows each U.
    const uint8_t* u { nullptr };
    const uint8_t* v { nullptr };
    unsigned lumaPitch { 0 };
    unsigned chromaPitch { 0 };
    bool interleavedChroma { false };
    unsigned width { 0 };
    unsigned height { 0 };
    Seconds time;
    // Gives the picture's memory back to the decoder; any thread, once.
    Function<void()> release;

    VitaVideoFrame() = default;
    VitaVideoFrame(VitaVideoFrame&&) = default;
    VitaVideoFrame& operator=(VitaVideoFrame&& other)
    {
        if (this != &other) {
            // The picture this one held goes back first.
            if (auto previous = std::exchange(release, nullptr))
                previous();
            luma = other.luma;
            u = other.u;
            v = other.v;
            lumaPitch = other.lumaPitch;
            chromaPitch = other.chromaPitch;
            interleavedChroma = other.interleavedChroma;
            width = other.width;
            height = other.height;
            time = other.time;
            release = std::exchange(other.release, nullptr);
        }
        return *this;
    }
    ~VitaVideoFrame()
    {
        if (release)
            release();
    }
};

// Decodes one video stream; used from one thread. Pictures come out in display
// order and stay valid until released.
class VitaVideoDecoder {
public:
    using Output = Function<void(VitaVideoFrame&&)>;

    // The hardware H.264 decoder when it takes the stream, else FFmpeg's. On
    // failure, sets FAILURE to why.
    static std::unique_ptr<VitaVideoDecoder> create(const AVCodecParameters&, String& failure);
    virtual ~VitaVideoDecoder() = default;

    // "hardware H.264", "software vp9"...
    virtual String name() const = 0;
    // How many pictures may be out (not yet released) while decode() is called.
    virtual unsigned maximumHeldPictures() const = 0;
    // False on an error the stream cannot go on from.
    virtual bool decode(std::span<const uint8_t>, std::optional<Seconds> presentationTime, std::optional<Seconds> decodeTime, const Output&) = 0;
    // The pictures still inside, at the end of the stream, as many as there is
    // room for: true when some are left for another call.
    virtual bool drain(const Output&) = 0;
    // Forgets what it holds, before decoding from another place.
    virtual void flush() = 0;
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && PLATFORM(VITA)
