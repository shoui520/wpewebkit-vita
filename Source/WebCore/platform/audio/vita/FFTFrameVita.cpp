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
#include "FFTFrame.h"

#if ENABLE(WEB_AUDIO) && PLATFORM(VITA)

#include <arm_neon.h>
#include <array>
#include <atomic>
#include <cmath>
#include <numbers>
#include <wtf/Lock.h>
#include <wtf/StdLibExtras.h>

namespace WebCore {

namespace {

constexpr unsigned minFFTPow2Size = 2;
constexpr unsigned maxFFTPow2Size = 24;

// The tables for a real FFT of size N, done as a complex FFT of size M = N / 2.
struct FFTTables {
    explicit FFTTables(unsigned log2Size)
        : complexSize(1u << (log2Size - 1))
        , bitReversed(complexSize)
        , stageCosine(complexSize)
        , stageSine(complexSize)
        , realCosine(complexSize / 2 + 1)
        , realSine(complexSize / 2 + 1)
    {
        unsigned bits = log2Size - 1;
        for (unsigned i = 0; i < complexSize; ++i) {
            unsigned reversed = 0;
            for (unsigned bit = 0; bit < bits; ++bit)
                reversed |= ((i >> bit) & 1) << (bits - 1 - bit);
            bitReversed[i] = reversed;
        }
        for (unsigned half = 1; half < complexSize; half *= 2) {
            for (unsigned j = 0; j < half; ++j) {
                double angle = std::numbers::pi * j / half;
                stageCosine[half + j] = std::cos(angle);
                stageSine[half + j] = std::sin(angle);
            }
        }
        for (unsigned i = 0; i <= complexSize / 2; ++i) {
            double realAngle = std::numbers::pi * i / complexSize;
            realCosine[i] = std::cos(realAngle);
            realSine[i] = std::sin(realAngle);
        }
    }

    unsigned complexSize;
    Vector<uint32_t> bitReversed;
    // exp(2 pi i j / span) for the butterflies of each pass, j < span / 2, at
    // [span / 2, span): contiguous, and aligned for the vector loop.
    Vector<float> stageCosine;
    Vector<float> stageSine;
    // exp(pi i k / M) to split the real transform.
    Vector<float> realCosine;
    Vector<float> realSine;
};

const FFTTables& tablesForSize(unsigned log2Size)
{
    static std::array<std::atomic<const FFTTables*>, maxFFTPow2Size + 1> tables;
    static Lock lock;
    RELEASE_ASSERT(log2Size >= minFFTPow2Size && log2Size <= maxFFTPow2Size);
    if (auto* existing = tables[log2Size].load(std::memory_order_acquire))
        return *existing;
    Locker locker { lock };
    if (auto* existing = tables[log2Size].load(std::memory_order_acquire))
        return *existing;
    // Kept for the life of the process, like the platform FFT setups.
    auto* created = new FFTTables(log2Size);
    tables[log2Size].store(created, std::memory_order_release);
    return *created;
}

// The passes over spans of 8 and more, four butterflies at a time.
template<bool inverse>
void complexFFTPasses(const FFTTables& tables, float* real, float* imag)
{
    unsigned size = tables.complexSize;
    for (unsigned half = 4; half < size; half *= 2) {
        const float* cosine = tables.stageCosine.span().data() + half;
        const float* sine = tables.stageSine.span().data() + half;
        // Block by block: the passes over large sizes stay in the cache.
        for (unsigned start = 0; start < size; start += 2 * half) {
            float* lowReal = real + start;
            float* lowImag = imag + start;
            float* highReal = lowReal + half;
            float* highImag = lowImag + half;
            for (unsigned j = 0; j < half; j += 4) {
                float32x4_t c = vld1q_f32(cosine + j);
                float32x4_t s = vld1q_f32(sine + j);
                float32x4_t r = vld1q_f32(highReal + j);
                float32x4_t i = vld1q_f32(highImag + j);
                // (r + i i) times exp(-2 pi i j / span), or exp(+...) inverse.
                float32x4_t productReal = inverse ? vmlsq_f32(vmulq_f32(r, c), i, s) : vmlaq_f32(vmulq_f32(r, c), i, s);
                float32x4_t productImag = inverse ? vmlaq_f32(vmulq_f32(i, c), r, s) : vmlsq_f32(vmulq_f32(i, c), r, s);
                float32x4_t baseReal = vld1q_f32(lowReal + j);
                float32x4_t baseImag = vld1q_f32(lowImag + j);
                vst1q_f32(highReal + j, vsubq_f32(baseReal, productReal));
                vst1q_f32(highImag + j, vsubq_f32(baseImag, productImag));
                vst1q_f32(lowReal + j, vaddq_f32(baseReal, productReal));
                vst1q_f32(lowImag + j, vaddq_f32(baseImag, productImag));
            }
        }
    }
}

// In place on bit-reversed input; inverse uses exp(+2 pi i k / M).
void complexFFT(const FFTTables& tables, float* real, float* imag, bool inverse)
{
    unsigned size = tables.complexSize;
    if (size == 2) {
        float sumReal = real[0] + real[1];
        float sumImag = imag[0] + imag[1];
        real[1] = real[0] - real[1];
        imag[1] = imag[0] - imag[1];
        real[0] = sumReal;
        imag[0] = sumImag;
        return;
    }
    // The spans of 2 and 4 together: their twiddles are 1 and -i (i inverse).
    for (unsigned start = 0; start < size; start += 4) {
        float* r = real + start;
        float* i = imag + start;
        float real0 = r[0] + r[1], imag0 = i[0] + i[1];
        float real1 = r[0] - r[1], imag1 = i[0] - i[1];
        float real2 = r[2] + r[3], imag2 = i[2] + i[3];
        float real3 = r[2] - r[3], imag3 = i[2] - i[3];
        float productReal = inverse ? -imag3 : imag3;
        float productImag = inverse ? real3 : -real3;
        r[0] = real0 + real2;
        i[0] = imag0 + imag2;
        r[2] = real0 - real2;
        i[2] = imag0 - imag2;
        r[1] = real1 + productReal;
        i[1] = imag1 + productImag;
        r[3] = real1 - productReal;
        i[3] = imag1 - productImag;
    }
    if (inverse)
        complexFFTPasses<true>(tables, real, imag);
    else
        complexFFTPasses<false>(tables, real, imag);
}

// The forward transform of the N real samples: X[k] for 0 < k < N / 2 in
// real/imag[k], with the real X[0] in real[0] and X[N / 2] in imag[0].
void realFFT(const FFTTables& tables, const float* input, float* real, float* imag)
{
    unsigned size = tables.complexSize;
    const uint32_t* bitReversed = tables.bitReversed.span().data();
    // z[n] = x[2n] + i x[2n + 1], stored bit-reversed for the complex FFT.
    for (unsigned n = 0; n < size; ++n) {
        real[bitReversed[n]] = input[2 * n];
        imag[bitReversed[n]] = input[2 * n + 1];
    }
    complexFFT(tables, real, imag, false);

    // Even and odd sample spectra: E = (Z[k] + conj Z[M - k]) / 2,
    // O = -i (Z[k] - conj Z[M - k]) / 2; then X[k] = E + W^k O and
    // X[M - k] = conj(E - W^k O), W = exp(-pi i / M).
    float dc = real[0] + imag[0];
    float nyquist = real[0] - imag[0];
    real[0] = dc;
    imag[0] = nyquist;
    const float* cosine = tables.realCosine.span().data();
    const float* sine = tables.realSine.span().data();
    for (unsigned k = 1; k <= size / 2; ++k) {
        unsigned mirror = size - k;
        float evenReal = (real[k] + real[mirror]) / 2;
        float evenImag = (imag[k] - imag[mirror]) / 2;
        float oddReal = (imag[k] + imag[mirror]) / 2;
        float oddImag = (real[mirror] - real[k]) / 2;
        // W^k O, with W^k = cos - i sin.
        float rotatedReal = oddReal * cosine[k] + oddImag * sine[k];
        float rotatedImag = oddImag * cosine[k] - oddReal * sine[k];
        real[k] = evenReal + rotatedReal;
        imag[k] = evenImag + rotatedImag;
        if (mirror != k) {
            real[mirror] = evenReal - rotatedReal;
            imag[mirror] = rotatedImag - evenImag;
        }
    }
}

// The inverse of realFFT, scaled so that it returns the original samples.
// Overwrites real and imag.
void inverseRealFFT(const FFTTables& tables, float* real, float* imag, float* output)
{
    unsigned size = tables.complexSize;
    // Z[k] = E + i O with E = (X[k] + conj X[M - k]) / 2 and
    // O = W^-k (X[k] - conj X[M - k]) / 2; Z[M - k] = conj(E - i O).
    float dc = real[0];
    float nyquist = imag[0];
    real[0] = (dc + nyquist) / 2;
    imag[0] = (dc - nyquist) / 2;
    const float* cosine = tables.realCosine.span().data();
    const float* sine = tables.realSine.span().data();
    for (unsigned k = 1; k <= size / 2; ++k) {
        unsigned mirror = size - k;
        float evenReal = (real[k] + real[mirror]) / 2;
        float evenImag = (imag[k] - imag[mirror]) / 2;
        float differenceReal = (real[k] - real[mirror]) / 2;
        float differenceImag = (imag[k] + imag[mirror]) / 2;
        // W^-k = cos + i sin.
        float oddReal = differenceReal * cosine[k] - differenceImag * sine[k];
        float oddImag = differenceReal * sine[k] + differenceImag * cosine[k];
        real[k] = evenReal - oddImag;
        imag[k] = evenImag + oddReal;
        if (mirror != k) {
            real[mirror] = evenReal + oddImag;
            imag[mirror] = oddReal - evenImag;
        }
    }

    const uint32_t* bitReversed = tables.bitReversed.span().data();
    for (unsigned n = 0; n < size; ++n) {
        unsigned reversed = bitReversed[n];
        if (n < reversed) {
            std::swap(real[n], real[reversed]);
            std::swap(imag[n], imag[reversed]);
        }
    }
    complexFFT(tables, real, imag, true);
    float scale = 1.0f / size;
    unsigned vectorSize = size & ~3u;
    for (unsigned n = 0; n < vectorSize; n += 4) {
        float32x4x2_t samples { { vmulq_n_f32(vld1q_f32(real + n), scale), vmulq_n_f32(vld1q_f32(imag + n), scale) } };
        vst2q_f32(output + 2 * n, samples);
    }
    for (unsigned n = vectorSize; n < size; ++n) {
        output[2 * n] = real[n] * scale;
        output[2 * n + 1] = imag[n] * scale;
    }
}

} // namespace

FFTFrame::FFTFrame(unsigned fftSize)
    : m_FFTSize(fftSize)
    , m_log2FFTSize(static_cast<unsigned>(std::log2(fftSize)))
    , m_realData(fftSize / 2)
    , m_imagData(fftSize / 2)
{
    ASSERT(1u << m_log2FFTSize == m_FFTSize);
}

FFTFrame::FFTFrame()
    : m_FFTSize(0)
    , m_log2FFTSize(0)
{
}

FFTFrame::FFTFrame(const FFTFrame& frame)
    : m_FFTSize(frame.m_FFTSize)
    , m_log2FFTSize(frame.m_log2FFTSize)
    , m_realData(frame.m_FFTSize / 2)
    , m_imagData(frame.m_FFTSize / 2)
{
    memcpySpan(m_realData.span(), frame.m_realData.span());
    memcpySpan(m_imagData.span(), frame.m_imagData.span());
}

FFTFrame::~FFTFrame() = default;

void FFTFrame::initialize()
{
}

void FFTFrame::doFFT(std::span<const float> data)
{
    RELEASE_ASSERT(data.size() >= m_FFTSize);
    realFFT(tablesForSize(m_log2FFTSize), data.data(), m_realData.data(), m_imagData.data());
}

void FFTFrame::doInverseFFT(std::span<float> data)
{
    RELEASE_ASSERT(data.size() >= m_FFTSize);
    inverseRealFFT(tablesForSize(m_log2FFTSize), m_realData.data(), m_imagData.data(), data.data());
}

int FFTFrame::minFFTSize()
{
    return 1 << minFFTPow2Size;
}

int FFTFrame::maxFFTSize()
{
    return 1 << maxFFTPow2Size;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO) && PLATFORM(VITA)
