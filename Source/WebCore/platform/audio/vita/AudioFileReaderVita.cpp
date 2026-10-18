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
#include "AudioFileReader.h"

#if ENABLE(WEB_AUDIO) && PLATFORM(VITA)

#include "AudioBus.h"
#include "FFmpegAudioDecoder.h"
#include <wtf/FileSystem.h>
#include <wtf/text/MakeString.h>

namespace WebCore {

// As many channels as an audio context takes.
static constexpr unsigned maximumChannels = 32;

// Decodes a whole file into planar floats at the requested rate.
RefPtr<AudioBus> createBusFromInMemoryAudioFile(std::span<const uint8_t> data, bool mixToMono, float sampleRate)
{
    MemoryAudioInput input { data };
    // MP4 (M4A) files start with an 'ftyp' box.
    bool isMP4 = data.size() > 8 && equalSpans(data.subspan(4, 4), "ftyp"_span8);
    auto decoder = isMP4 ? FFmpegAudioDecoder::openMP4(data) : FFmpegAudioDecoder::open(input);
    if (!decoder)
        return nullptr;

    unsigned channels = mixToMono ? 1 : std::min(decoder->channelCount(), maximumChannels);
    int outputRate = sampleRate > 0 ? static_cast<int>(std::lround(sampleRate)) : decoder->sampleRate();
    if (!channels || outputRate <= 0)
        return nullptr;
    decoder->setOutput(channels, outputRate);

    Vector<Vector<float>> output(channels);
    if (auto duration = decoder->duration()) {
        size_t estimate = static_cast<size_t>(duration->seconds() * outputRate) + outputRate / 4;
        for (auto& channel : output)
            channel.reserveInitialCapacity(estimate);
    }
    while (true) {
        auto result = decoder->decode(output);
        if (result == FFmpegAudioDecoder::Result::Error)
            return nullptr;
        if (result == FFmpegAudioDecoder::Result::End)
            break;
    }
    if (output[0].isEmpty())
        return nullptr;

    auto bus = AudioBus::create(channels, output[0].size());
    bus->setSampleRate(outputRate);
    for (unsigned i = 0; i < channels; ++i) {
        memcpySpan(bus->channel(i)->mutableSpan(), output[i].span());
        // Each channel goes once it is copied: whole songs are large.
        output[i] = { };
    }
    return bus;
}

// The HRTF impulse responses, from the package.
RefPtr<AudioBus> AudioBus::loadPlatformResource(const char* name, float sampleRate)
{
    auto data = FileSystem::readEntireFile(makeString("app0:audio/"_s, unsafeSpan(name), ".wav"_s));
    if (!data)
        return nullptr;
    return createBusFromInMemoryAudioFile(data->span(), false, sampleRate);
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO) && PLATFORM(VITA)
