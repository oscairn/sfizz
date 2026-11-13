// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#pragma once

/* These are the NEON versions of the SIMDHelpers. Each computes every element exactly as the
   scalar version does (the same operations in the same order, the multiply-adds fused as the
   compilers contract the scalar ones), so their output is the scalar output, in place too. multiplyMul has none: under fast math
   a compiler may reorder the scalar version's two products. */
void gainNEON(const float* gain, const float* input, float* output, unsigned size) noexcept;
void gain1NEON(float gain, const float* input, float* output, unsigned size) noexcept;
void multiplyAddNEON(const float* gain, const float* input, float* output, unsigned size) noexcept;
void multiplyAdd1NEON(float gain, const float* input, float* output, unsigned size) noexcept;
void addNEON(const float* input, float* output, unsigned size) noexcept;
void add1NEON(float value, float* output, unsigned size) noexcept;
void subtractNEON(const float* input, float* output, unsigned size) noexcept;
void subtract1NEON(float value, float* output, unsigned size) noexcept;
