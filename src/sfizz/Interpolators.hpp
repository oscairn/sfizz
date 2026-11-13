// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#include "Interpolators.h"
#include "WindowedSinc.h"
#include "MathHelpers.h"
#include "SIMDConfig.h"
#include <simde/simde-features.h>
#if SIMDE_NATURAL_VECTOR_SIZE_GE(128)
#include <simde/x86/sse.h>
#include <simde/arm/neon/addv.h>

#if defined(SIMDE_ARM_NEON_A32V7_NATIVE)
    #define simde__m128_to_simde_float32x4(a) simde__m128_to_neon_f32((a))
#elif defined(SIMDE_X86_SSE_NATIVE)
    #define simde__m128_to_simde_float32x4(a) simde_float32x4_from_m128(simde__m128_to_private((a)).n)
#elif defined(SIMDE_STATEMENT_EXPR_)
    #define simde__m128_to_simde_float32x4(a) SIMDE_STATEMENT_EXPR_(({ \
        simde__m128_private a_ = simde__m128_to_private((a)); \
        simde_float32x4_private r_; \
        r_.values = a_.f32; \
        simde_float32x4_from_private(r_); \
    }))
#endif

#endif /* SIMDE_NATURAL_VECTOR_SIZE_GE(128) */


namespace sfz {

template <InterpolatorModel M, class R>
class Interpolator;

template <InterpolatorModel M, class R>
inline R interpolate(const R* values, R coeff)
{
    return Interpolator<M, R>::process(values, coeff);
}

//------------------------------------------------------------------------------
// Nearest

template <class R>
class Interpolator<kInterpolatorNearest, R>
{
public:
    static inline R process(const R* values, R coeff)
    {
        return values[coeff > static_cast<R>(0.5)];
    }
};

//------------------------------------------------------------------------------
// Linear

template <class R>
class Interpolator<kInterpolatorLinear, R>
{
public:
    static inline R process(const R* values, R coeff)
    {
        return values[0] * (static_cast<R>(1.0) - coeff) + values[1] * coeff;
    }
};

//------------------------------------------------------------------------------
// Hermite 3rd order, SSE specialization

#if SIMDE_NATURAL_VECTOR_SIZE_GE(128)
template <>
class Interpolator<kInterpolatorHermite3, float>
{
public:
    static inline float process(const float* values, float coeff)
    {
        simde__m128 x = simde_mm_sub_ps(simde_mm_setr_ps(-1, 0, 1, 2), simde_mm_set1_ps(coeff));
        simde__m128 h = hermite3x4(x);
        simde__m128 y = simde_mm_mul_ps(h, simde_mm_loadu_ps(values - 1));
        return simde_vaddvq_f32(simde__m128_to_simde_float32x4(y));
    }
};
#endif

#if SIMDE_NATURAL_VECTOR_SIZE_GE(128) && defined(SIMDE_ARM_NEON_A64V8_NATIVE) && defined(__GNUC__) && !defined(__clang__)
#define SFIZZ_HERMITE3_FOUR 1
/**
 * @brief Four outputs of the Hermite interpolator at once, for one or two channels read at the
 *        same positions: each is interpolate<kInterpolatorHermite3>(&sources[ch][indices[j]],
 *        coeffs[j]) to the bit (the same weights, each tap's product rounded on its own and the
 *        products summed in the same order), with the weights computed once for four outputs
 *        and both channels. gcc only: clang orders the weights' arithmetic differently
 *        under -ffast-math depending on the context, so it keeps the per-output loop
 */
inline void interpolateHermite3Four(const float* const* sources, int channels, const int* indices,
    const float* coeffs, float* const* outputs) noexcept
{
    // The weights: hermite3x4's pieces without its selection, since the taps' distances are
    // known: 0 and 1 are within 1 (the inner piece), -1 and 2 between 1 and 2 (the outer); a
    // distance of exactly 1 or 2, where the selection could go the other way, gives 0 in both
    const simde__m128 c = simde_mm_loadu_ps(coeffs);
    auto inner = [](simde__m128 x) {
        x = simde_x_mm_abs_ps(x);
        const simde__m128 x2 = simde_mm_mul_ps(x, x);
        const simde__m128 x3 = simde_mm_mul_ps(x2, x);
        const simde__m128 q = simde_mm_mul_ps(simde_mm_set1_ps(5./2.), x2);
        return simde__m128_to_neon_f32(simde_mm_add_ps(simde_mm_sub_ps(simde_mm_set1_ps(1), q), simde_mm_mul_ps(simde_mm_set1_ps(3./2.), x3)));
    };
    auto outer = [](simde__m128 x) {
        x = simde_x_mm_abs_ps(x);
        const simde__m128 x2 = simde_mm_mul_ps(x, x);
        const simde__m128 x3 = simde_mm_mul_ps(x2, x);
        const simde__m128 q = simde_mm_mul_ps(simde_mm_set1_ps(5./2.), x2);
        return simde__m128_to_neon_f32(simde_mm_sub_ps(simde_mm_add_ps(simde_mm_sub_ps(simde_mm_set1_ps(2), simde_mm_mul_ps(simde_mm_set1_ps(4), x)), q), simde_mm_mul_ps(simde_mm_set1_ps(1./2.), x3)));
    };
    const float32x4_t h[4] = {
        outer(simde_mm_sub_ps(simde_mm_set1_ps(-1.0f), c)),
        inner(simde_mm_sub_ps(simde_mm_set1_ps(0.0f), c)),
        inner(simde_mm_sub_ps(simde_mm_set1_ps(1.0f), c)),
        outer(simde_mm_sub_ps(simde_mm_set1_ps(2.0f), c)),
    };
    for (int ch = 0; ch < channels; ++ch) {
        const float* v = sources[ch];
        // rows: the four taps of each output; transposed to the k-th tap of each output
        const float32x4_t r0 = vld1q_f32(v + indices[0] - 1), r1 = vld1q_f32(v + indices[1] - 1);
        const float32x4_t r2 = vld1q_f32(v + indices[2] - 1), r3 = vld1q_f32(v + indices[3] - 1);
        const float64x2_t t0 = vreinterpretq_f64_f32(vtrn1q_f32(r0, r1)), t1 = vreinterpretq_f64_f32(vtrn2q_f32(r0, r1));
        const float64x2_t t2 = vreinterpretq_f64_f32(vtrn1q_f32(r2, r3)), t3 = vreinterpretq_f64_f32(vtrn2q_f32(r2, r3));
        float32x4_t p[4] = {
            vmulq_f32(h[0], vreinterpretq_f32_f64(vtrn1q_f64(t0, t2))),
            vmulq_f32(h[1], vreinterpretq_f32_f64(vtrn1q_f64(t1, t3))),
            vmulq_f32(h[2], vreinterpretq_f32_f64(vtrn2q_f64(t0, t2))),
            vmulq_f32(h[3], vreinterpretq_f32_f64(vtrn2q_f64(t1, t3))),
        };
        // the per-output sum's order, (p0 + p1) + (p2 + p3), each product and pair rounded on
        // its own: the barriers keep -ffast-math from fusing or reordering them
        for (auto& x : p)
            __asm__("" : "+w"(x));
        float32x4_t low = vaddq_f32(p[0], p[1]), high = vaddq_f32(p[2], p[3]);
        __asm__("" : "+w"(low), "+w"(high));
        vst1q_f32(outputs[ch], vaddq_f32(low, high));
    }
}
#endif

//------------------------------------------------------------------------------
// Hermite 3rd order, generic

template <class R>
class Interpolator<kInterpolatorHermite3, R>
{
public:
    static inline R process(const R* values, R coeff)
    {
        R y = 0;
        for (int i = -1; i < 3; ++i) {
            R h = hermite3<R>(i - coeff);
            y += h * values[i];
        }
        return y;
    }
};

//------------------------------------------------------------------------------
// B-spline 3rd order, SSE specialization

#if SIMDE_NATURAL_VECTOR_SIZE_GE(128)
template <>
class Interpolator<kInterpolatorBspline3, float>
{
public:
    static inline float process(const float* values, float coeff)
    {
        simde__m128 x = simde_mm_sub_ps(simde_mm_setr_ps(-1, 0, 1, 2), simde_mm_set1_ps(coeff));
        simde__m128 h = bspline3x4(x);
        simde__m128 y = simde_mm_mul_ps(h, simde_mm_loadu_ps(values - 1));
        return simde_vaddvq_f32(simde__m128_to_simde_float32x4(y));
    }
};
#endif

//------------------------------------------------------------------------------
// B-spline 3rd order, generic

template <class R>
class Interpolator<kInterpolatorBspline3, R>
{
public:
    static inline R process(const R* values, R coeff)
    {
        R y = 0;
        for (int i = -1; i < 3; ++i) {
            R h = bspline3<R>(i - coeff);
            y += h * values[i];
        }
        return y;
    }
};

//------------------------------------------------------------------------------
// Windowed sinc

namespace SincInterpolatorDetail {
    // See sfizz wiki page "Resampling".
    constexpr size_t PointsMin = 8;
    constexpr size_t PointsMax = 72;

    // Adjust Kaiser window Beta as necessary.
    constexpr double BetaMin = 6.0;
    constexpr double BetaMax = 10.0;

    constexpr double getBetaForNumPoints(size_t points)
    {
        return BetaMin + (BetaMax - BetaMin) *
            (double(points - PointsMin) / double(PointsMax - PointsMin));
    }

    constexpr size_t getTableSizeForNumPoints(size_t /*points*/)
    {
        return 1u << 16;
    }
}

///
template <size_t Points>
struct SincInterpolatorTraits {
    static_assert(Points == 8  || Points == 12 || Points == 16 ||
                  Points == 24 || Points == 36 || Points == 48 ||
                  Points == 60 || Points == 72,
                  "Windowed sinc size is not acceptable");

    enum {
        TableSize = SincInterpolatorDetail::getTableSizeForNumPoints(Points)
    };

    static void initialize()
    {
        static const FixedWindowedSinc<Points, TableSize> globalInstance(
            SincInterpolatorDetail::getBetaForNumPoints(Points));
        windowedSinc = &globalInstance;
    }

    static const FixedWindowedSinc<Points, TableSize>* windowedSinc;
};

template <size_t Points>
const FixedWindowedSinc<Points, SincInterpolatorTraits<Points>::TableSize>*
SincInterpolatorTraits<Points>::windowedSinc = nullptr;

///
template <class R, size_t Points>
class SincInterpolator;

//------------------------------------------------------------------------------
// Windowed sinc any order, SSE specialization
#if SIMDE_NATURAL_VECTOR_SIZE_GE(128)
template <size_t Points>
class SincInterpolator<float, Points>
{
public:
    static_assert(Points % 4 == 0, "Windowed sinc must be multiple of 4");

    static inline float process(const float* values, float coeff)
    {
        const auto &ws = *SincInterpolatorTraits<Points>::windowedSinc;

        constexpr int j0 = 1 - int(Points) / 2;
        float x0 = j0 - coeff;

        simde__m128 y = simde_mm_set1_ps(0.0f);
        simde__m128 x = simde_mm_add_ps(simde_mm_set1_ps(x0), simde_mm_setr_ps(0, 1, 2, 3));
        size_t i = 0;
        do {
            simde__m128 h = ws.getUncheckedX4(x);
            y = simde_mm_add_ps(y, simde_mm_mul_ps(h, simde_mm_loadu_ps(&values[j0 + static_cast<int>(i)])));
            x = simde_mm_add_ps(x, simde_mm_set1_ps(4.0f));
            i += 4;
        } while (i < Points);

        return simde_vaddvq_f32(simde__m128_to_simde_float32x4(y));
    }
};
#endif

//------------------------------------------------------------------------------
// Windowed sinc any order, generic
template <class R, size_t Points>
class SincInterpolator
{
public:
    static inline R process(const R* values, R coeff)
    {
        const auto &ws = *SincInterpolatorTraits<Points>::windowedSinc;

        int j0 = 1 - int(Points) / 2;

        R h[Points];
        for (int i = 0; i < int(Points); ++i)
            h[i] = R(ws.getUnchecked(j0 - coeff + i));

        R y = h[0] * values[j0];
        for (int i = 1; i < int(Points); ++i)
            y += h[i] * values[j0 + i];

        return y;
    }
};

template <class R>
class Interpolator<kInterpolatorSinc8, R> : public SincInterpolator<R, 8> {};
template <class R>
class Interpolator<kInterpolatorSinc12, R> : public SincInterpolator<R, 12> {};
template <class R>
class Interpolator<kInterpolatorSinc16, R> : public SincInterpolator<R, 16> {};
template <class R>
class Interpolator<kInterpolatorSinc24, R> : public SincInterpolator<R, 24> {};
template <class R>
class Interpolator<kInterpolatorSinc36, R> : public SincInterpolator<R, 36> {};
template <class R>
class Interpolator<kInterpolatorSinc48, R> : public SincInterpolator<R, 48> {};
template <class R>
class Interpolator<kInterpolatorSinc60, R> : public SincInterpolator<R, 60> {};
template <class R>
class Interpolator<kInterpolatorSinc72, R> : public SincInterpolator<R, 72> {};

} // namespace sfz
