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
#include "AudioDestinationResampler.h"

#if ENABLE(WEB_AUDIO) && PLATFORM(VITA)

#include "AudioBus.h"
#include <psp2/audioout.h>
#include <wtf/Threading.h>
#include <wtf/ThreadSafeWeakPtr.h>

namespace WebCore {

// The system mixes main ports at this rate.
constexpr float outputSampleRate = 48000;
// Frames per port write: a multiple of 64, about 5 ms.
constexpr unsigned framesPerWrite = 256;

// Renders the graph on a thread of its own that writes to a system audio
// port; the blocking writes pace it.
class AudioDestinationVita final : public AudioDestinationResampler, public ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<AudioDestinationVita, WTF::DestructionThread::Main> {
public:
    explicit AudioDestinationVita(const CreationOptions& options)
        : AudioDestinationResampler(options, outputSampleRate)
        , m_channels(numberOfOutputChannels())
    {
        for (auto& channel : m_channels)
            channel.resize(framesPerWrite);
    }

    ~AudioDestinationVita()
    {
        stopThread();
    }

    void ref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::ref(); }
    void deref() const final { ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr::deref(); }

private:
    void startRendering(CompletionHandler<void(bool)>&& completionHandler) final
    {
        if (!m_thread) {
            m_running = true;
            m_thread = Thread::create("Web Audio"_s, [this] {
                run();
            }, ThreadType::Audio, Thread::QOS::UserInteractive);
        }
        setIsPlaying(true);
        completionHandler(true);
    }

    void stopRendering(CompletionHandler<void(bool)>&& completionHandler) final
    {
        stopThread();
        setIsPlaying(false);
        completionHandler(true);
    }

    void stopThread()
    {
        m_running = false;
        if (RefPtr thread = std::exchange(m_thread, nullptr))
            thread->waitForCompletion();
    }

    void run()
    {
        int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, framesPerWrite, static_cast<int>(outputSampleRate), SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0)
            return;
        Vector<int16_t> samples(framesPerWrite * 2);
        uint64_t frame = 0;
        while (m_running) {
            for (size_t i = 0; i < m_channels.size(); ++i)
                m_outputBus->setChannelMemory(i, m_channels[i].mutableSpan());
            size_t framesToRender = pullRendered(framesPerWrite);
            render(frame / outputSampleRate, MonotonicTime::now(), framesToRender);

            auto left = m_channels[0].span();
            auto right = m_channels[m_channels.size() > 1 ? 1 : 0].span();
            auto toSample = [](float value) {
                return static_cast<int16_t>(std::clamp(value, -1.0f, 1.0f) * 32767);
            };
            for (unsigned i = 0; i < framesPerWrite; ++i) {
                samples[2 * i] = toSample(left[i]);
                samples[2 * i + 1] = toSample(right[i]);
            }
            sceAudioOutOutput(port, samples.span().data());
            frame += framesPerWrite;
        }
        sceAudioOutOutput(port, nullptr);
        sceAudioOutReleasePort(port);
    }

    Vector<Vector<float>> m_channels;
    std::atomic<bool> m_running { false };
    RefPtr<Thread> m_thread;
};

Ref<AudioDestination> AudioDestination::create(const CreationOptions& options)
{
    return adoptRef(*new AudioDestinationVita(options));
}

float AudioDestination::hardwareSampleRate()
{
    return outputSampleRate;
}

unsigned long AudioDestination::maxChannelCount()
{
    return 2;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO) && PLATFORM(VITA)
