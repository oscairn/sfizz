// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#pragma once
#include "WindowedSinc.h"
#include <cstdint>
#if SIMDE_NATURAL_VECTOR_SIZE_GE(128)
#include <simde/x86/sse2.h>
#endif

namespace sfz {

template <class T>
inline void AbstractWindowedSinc<T>::fillTable() noexcept
{
    float* table = const_cast<float*>(static_cast<T*>(this)->getTablePointer());
    size_t points = static_cast<T*>(this)->getNumPoints();
    size_t tableSize = static_cast<T*>(this)->getTableSize();

    WindowedSincDetail::calculateTable(
        absl::MakeSpan(table, tableSize), points, beta_, TableExtra);
}

template <class T>
inline float AbstractWindowedSinc<T>::getUnchecked(float x) const noexcept
{
    const float* table = static_cast<const T*>(this)->getTablePointer();
    size_t points = static_cast<const T*>(this)->getNumPoints();
    size_t tableSize = static_cast<const T*>(this)->getTableSize();

    float ix = (x + points / 2.0f) * ((tableSize - 1) / points);
    intptr_t i0 = static_cast<intptr_t>(ix);
    float mu = ix - i0;
    float y0 = table[i0];
    float dy = table[i0 + 1] - y0;
    return y0 + mu * dy;
}

#if SIMDE_NATURAL_VECTOR_SIZE_GE(128)
template <class T>
inline simde__m128 AbstractWindowedSinc<T>::getUncheckedX4(simde__m128 x) const noexcept
{
    const float* table = static_cast<const T*>(this)->getTablePointer();
    size_t points = static_cast<const T*>(this)->getNumPoints();
    size_t tableSize = static_cast<const T*>(this)->getTableSize();

    simde__m128 ix = simde_mm_mul_ps(
        simde_mm_add_ps(x, simde_mm_set1_ps(points / 2.0f)),
        simde_mm_set1_ps((tableSize - 1) / points));
    alignas(simde__m128i) int j0[4];
    simde__m128i i0 = simde_mm_cvttps_epi32(ix);
    simde_mm_store_si128((simde__m128i*)j0, i0);
    simde__m128 mu = simde_mm_sub_ps(ix, simde_mm_cvtepi32_ps(i0));

    // Scalar loads: the table pairs are not 16-byte aligned, and simde's
    // loadl_epi64 reads them through an aligned vector pointer
    simde__m128 y0 = simde_mm_setr_ps(table[j0[0]], table[j0[1]], table[j0[2]], table[j0[3]]);
    simde__m128 y1 = simde_mm_setr_ps(table[j0[0] + 1], table[j0[1] + 1], table[j0[2] + 1], table[j0[3] + 1]);

    simde__m128 dy = simde_mm_sub_ps(y1, y0);
    return simde_mm_add_ps(y0, simde_mm_mul_ps(mu, dy));
}
#endif

template <class T>
inline double AbstractWindowedSinc<T>::getExact(double x) const noexcept
{
    size_t points = static_cast<const T*>(this)->getNumPoints();
    return WindowedSincDetail::calculateExact(x, points, beta_);
}

} // namespace sfz
