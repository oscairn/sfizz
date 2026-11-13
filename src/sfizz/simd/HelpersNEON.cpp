// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "HelpersNEON.h"
#include "HelpersScalar.h"
#include "Common.h"
#include "../SIMDConfig.h"

#if SFIZZ_HAVE_NEON
#include <arm_neon.h>

// Eight elements at a time, then four, then the rest through the scalar version. Each group's
// loads come before its stores, so input and output may be the same array (the scalar versions'
// vectorized forms fall back to one element at a time then).
#define SFIZZ_NEON_LOOP(load, compute, scalarCall) \
    const float* const end8 = output + (size & ~7u); \
    const float* const end4 = output + (size & ~3u); \
    while (output < end8) { \
        load(0) load(4) compute(0) compute(4) output += 8; ADVANCE(8) \
    } \
    if (output < end4) { \
        load(0) compute(0) output += 4; ADVANCE(4) \
    } \
    scalarCall;

void gainNEON(const float* gain, const float* input, float* output, unsigned size) noexcept
{
#define ADVANCE(n) gain += n; input += n;
#define LOAD(k) const float32x4_t g##k = vld1q_f32(gain + k), x##k = vld1q_f32(input + k);
#define COMPUTE(k) vst1q_f32(output + k, vmulq_f32(g##k, x##k));
    SFIZZ_NEON_LOOP(LOAD, COMPUTE, gainScalar(gain, input, output, size & 3u))
#undef ADVANCE
#undef LOAD
#undef COMPUTE
}

void gain1NEON(float gain, const float* input, float* output, unsigned size) noexcept
{
    const float32x4_t g = vdupq_n_f32(gain);
#define ADVANCE(n) input += n;
#define LOAD(k) const float32x4_t x##k = vld1q_f32(input + k);
#define COMPUTE(k) vst1q_f32(output + k, vmulq_f32(g, x##k));
    SFIZZ_NEON_LOOP(LOAD, COMPUTE, gain1Scalar(gain, input, output, size & 3u))
#undef ADVANCE
#undef LOAD
#undef COMPUTE
}

void multiplyAddNEON(const float* gain, const float* input, float* output, unsigned size) noexcept
{
#define ADVANCE(n) gain += n; input += n;
#define LOAD(k) const float32x4_t g##k = vld1q_f32(gain + k), x##k = vld1q_f32(input + k), o##k = vld1q_f32(output + k);
#define COMPUTE(k) vst1q_f32(output + k, vfmaq_f32(o##k, g##k, x##k));
    SFIZZ_NEON_LOOP(LOAD, COMPUTE, multiplyAddScalar(gain, input, output, size & 3u))
#undef ADVANCE
#undef LOAD
#undef COMPUTE
}

void multiplyAdd1NEON(float gain, const float* input, float* output, unsigned size) noexcept
{
    const float32x4_t g = vdupq_n_f32(gain);
#define ADVANCE(n) input += n;
#define LOAD(k) const float32x4_t x##k = vld1q_f32(input + k), o##k = vld1q_f32(output + k);
#define COMPUTE(k) vst1q_f32(output + k, vfmaq_f32(o##k, g, x##k));
    SFIZZ_NEON_LOOP(LOAD, COMPUTE, multiplyAdd1Scalar(gain, input, output, size & 3u))
#undef ADVANCE
#undef LOAD
#undef COMPUTE
}

void addNEON(const float* input, float* output, unsigned size) noexcept
{
#define ADVANCE(n) input += n;
#define LOAD(k) const float32x4_t x##k = vld1q_f32(input + k), o##k = vld1q_f32(output + k);
#define COMPUTE(k) vst1q_f32(output + k, vaddq_f32(o##k, x##k));
    SFIZZ_NEON_LOOP(LOAD, COMPUTE, addScalar(input, output, size & 3u))
#undef ADVANCE
#undef LOAD
#undef COMPUTE
}

void add1NEON(float value, float* output, unsigned size) noexcept
{
    const float32x4_t v = vdupq_n_f32(value);
#define ADVANCE(n)
#define LOAD(k) const float32x4_t o##k = vld1q_f32(output + k);
#define COMPUTE(k) vst1q_f32(output + k, vaddq_f32(o##k, v));
    SFIZZ_NEON_LOOP(LOAD, COMPUTE, add1Scalar(value, output, size & 3u))
#undef ADVANCE
#undef LOAD
#undef COMPUTE
}

void subtractNEON(const float* input, float* output, unsigned size) noexcept
{
#define ADVANCE(n) input += n;
#define LOAD(k) const float32x4_t x##k = vld1q_f32(input + k), o##k = vld1q_f32(output + k);
#define COMPUTE(k) vst1q_f32(output + k, vsubq_f32(o##k, x##k));
    SFIZZ_NEON_LOOP(LOAD, COMPUTE, subtractScalar(input, output, size & 3u))
#undef ADVANCE
#undef LOAD
#undef COMPUTE
}

void subtract1NEON(float value, float* output, unsigned size) noexcept
{
    const float32x4_t v = vdupq_n_f32(value);
#define ADVANCE(n)
#define LOAD(k) const float32x4_t o##k = vld1q_f32(output + k);
#define COMPUTE(k) vst1q_f32(output + k, vsubq_f32(o##k, v));
    SFIZZ_NEON_LOOP(LOAD, COMPUTE, subtract1Scalar(value, output, size & 3u))
#undef ADVANCE
#undef LOAD
#undef COMPUTE
}

#undef SFIZZ_NEON_LOOP

#endif
