/*
 * Copyright (C) 2011 University of Szeged
 * Copyright (C) 2011 Zoltan Herczeg
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
 * THIS SOFTWARE IS PROVIDED BY UNIVERSITY OF SZEGED ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL UNIVERSITY OF SZEGED OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef FEGaussianBlurNEON_h
#define FEGaussianBlurNEON_h

#if HAVE(ARM_NEON_INTRINSICS)

#include "FEGaussianBlur.h"
#include "NEONHelpers.h"

namespace WebCore {

// A box blur of premultiplied RGBA8 pixels in integers: each output is the sum of the
// kernelSize inputs from left before it to right - 1 after it (zero outside), times a 16.16
// reciprocal. Both directions read and write rows in order: a column at a time, each pixel
// was a cache miss once the buffer outgrew the cache.

inline uint32_t boxBlurReciprocalNEON(unsigned kernelSize)
{
    return ((1u << 16) + kernelSize / 2) / kernelSize;
}

inline uint16x4_t loadPixelNEON(const uint32_t* pixel)
{
    return vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vld1_dup_u32(pixel))));
}

inline void storePixelNEON(uint32_t* pixel, uint32x4_t sum, uint32_t reciprocal)
{
    uint16x4_t value = vrshrn_n_u32(vmulq_n_u32(sum, reciprocal), 16);
    vst1_lane_u32(pixel, vreinterpret_u32_u8(vqmovn_u16(vcombine_u16(value, value))), 0);
}

inline void boxBlurRowsNEON(const PixelBuffer& sourceBuffer, PixelBuffer& destinationBuffer, unsigned kernelSize, int left, int right, int width, int height)
{
    auto* source = reinterpret_cast<const uint32_t*>(sourceBuffer.bytes().data());
    auto* destination = reinterpret_cast<uint32_t*>(destinationBuffer.bytes().data());
    uint32_t reciprocal = boxBlurReciprocalNEON(kernelSize);
    for (int y = 0; y < height; ++y) {
        auto* sourceRow = source + y * width;
        auto* destinationRow = destination + y * width;
        uint32x4_t sum = vdupq_n_u32(0);
        for (int x = 0; x < std::min(right, width); ++x)
            sum = vaddw_u16(sum, loadPixelNEON(sourceRow + x));
        for (int x = 0; x < width; ++x) {
            storePixelNEON(destinationRow + x, sum, reciprocal);
            if (x >= left)
                sum = vsubw_u16(sum, loadPixelNEON(sourceRow + x - left));
            if (x + right < width)
                sum = vaddw_u16(sum, loadPixelNEON(sourceRow + x + right));
        }
    }
}

// Down each column, with the column sums for a row kept across the row.
inline void boxBlurColumnsNEON(const PixelBuffer& sourceBuffer, PixelBuffer& destinationBuffer, unsigned kernelSize, int top, int bottom, int width, int height)
{
    auto* source = sourceBuffer.bytes().data();
    auto* destination = destinationBuffer.bytes().data();
    uint32_t reciprocal = boxBlurReciprocalNEON(kernelSize);
    size_t rowBytes = 4 * width;
    int wholeBlocks = width / 4;
    Vector<uint32x4_t> sumsVector(width, vdupq_n_u32(0));
    auto* sums = sumsVector.mutableSpan().data();
    auto update = [&](const uint8_t* row, bool add) {
        for (int block = 0; block < wholeBlocks; ++block) {
            uint8x16_t pixels = vld1q_u8(row + 16 * block);
            uint16x8_t low = vmovl_u8(vget_low_u8(pixels));
            uint16x8_t high = vmovl_u8(vget_high_u8(pixels));
            auto* blockSums = sums + 4 * block;
            if (add) {
                blockSums[0] = vaddw_u16(blockSums[0], vget_low_u16(low));
                blockSums[1] = vaddw_u16(blockSums[1], vget_high_u16(low));
                blockSums[2] = vaddw_u16(blockSums[2], vget_low_u16(high));
                blockSums[3] = vaddw_u16(blockSums[3], vget_high_u16(high));
            } else {
                blockSums[0] = vsubw_u16(blockSums[0], vget_low_u16(low));
                blockSums[1] = vsubw_u16(blockSums[1], vget_high_u16(low));
                blockSums[2] = vsubw_u16(blockSums[2], vget_low_u16(high));
                blockSums[3] = vsubw_u16(blockSums[3], vget_high_u16(high));
            }
        }
        for (int x = 4 * wholeBlocks; x < width; ++x) {
            auto pixel = loadPixelNEON(reinterpret_cast<const uint32_t*>(row) + x);
            sums[x] = add ? vaddw_u16(sums[x], pixel) : vsubw_u16(sums[x], pixel);
        }
    };
    for (int y = 0; y < std::min(bottom, height); ++y)
        update(source + y * rowBytes, true);
    for (int y = 0; y < height; ++y) {
        auto* destinationRow = destination + y * rowBytes;
        for (int block = 0; block < wholeBlocks; ++block) {
            auto* blockSums = sums + 4 * block;
            uint16x8_t low = vcombine_u16(vrshrn_n_u32(vmulq_n_u32(blockSums[0], reciprocal), 16), vrshrn_n_u32(vmulq_n_u32(blockSums[1], reciprocal), 16));
            uint16x8_t high = vcombine_u16(vrshrn_n_u32(vmulq_n_u32(blockSums[2], reciprocal), 16), vrshrn_n_u32(vmulq_n_u32(blockSums[3], reciprocal), 16));
            vst1q_u8(destinationRow + 16 * block, vcombine_u8(vqmovn_u16(low), vqmovn_u16(high)));
        }
        for (int x = 4 * wholeBlocks; x < width; ++x)
            storePixelNEON(reinterpret_cast<uint32_t*>(destinationRow) + x, sums[x], reciprocal);
        if (y >= top)
            update(source + (y - top) * rowBytes, false);
        if (y + bottom < height)
            update(source + (y + bottom) * rowBytes, true);
    }
}

} // namespace WebCore

#endif // HAVE(ARM_NEON_INTRINSICS)

#endif // FEGaussianBlurNEON_h
