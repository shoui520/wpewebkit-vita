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

#if PLATFORM(VITA) && (ENABLE(VIDEO) || ENABLE(WEB_AUDIO))

#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace WebCore {

// The first audio track of an MP4 (M4A, MOV) file in memory: its decoder
// configuration and where its samples are.
struct MP4AudioTrack {
    // From the MPEG-4 decoder configuration: 0x40 AAC, 0x66 to 0x68 MPEG-2
    // AAC, 0x69 and 0x6B MP3.
    uint8_t objectType { 0 };
    // AudioSpecificConfig for AAC.
    Vector<uint8_t> decoderConfig;
    unsigned channels { 0 };
    unsigned sampleRate { 0 };
    // The media's time scale, and the part of it the edit list plays.
    uint32_t timescale { 0 };
    uint64_t skip { 0 };
    std::optional<uint64_t> playedDuration;
    struct Sample {
        uint64_t offset;
        uint32_t size;
    };
    Vector<Sample> samples;
};

std::optional<MP4AudioTrack> parseMP4AudioTrack(std::span<const uint8_t> file);

} // namespace WebCore

#endif // PLATFORM(VITA) && (ENABLE(VIDEO) || ENABLE(WEB_AUDIO))
