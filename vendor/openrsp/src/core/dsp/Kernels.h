// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the OpenRSP authors
//
// This file is part of OpenRSP.
//
// OpenRSP is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// OpenRSP is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with OpenRSP. If
// not, see <https://www.gnu.org/licenses/>.

// Kernel dispatch table for the hot loops of the stream path.
//
// The scalar table is the reference arithmetic. SIMD tables (src/core/dsp/simd/: SSE2, AVX2 on x86-64; NEON on
// aarch64 and armv7) fill the same table and must produce IDENTICAL bits on every
// input: every entry is either integer arithmetic with defined
// wrap-around (see DspCommon.h; Z/2^32 is a ring, so sums and products may be reordered
// freely), or double arithmetic whose per-lane operation sequence is kept exactly
// (dcCorrect: the 8-sample blocks of r_dcChannel in simd/ScalarRef.inc). The unit tests check every table against the scalar one on adversarial
// inputs and on recorded streams.
//
// Selection: the best table this CPU supports (runtime CPU-feature dispatch, one binary
// runs everywhere), unless SDRPLAY_FORCE_SIMD=scalar|sse2|avx2|avx512|neon names one; a
// forced table this build/CPU cannot run falls back to the best available one (with
// avx512 -> avx2 -> sse2), see selectKernels().
#pragma once

#include <cstdint>
#include <vector>

#include "core/dsp/Packet.h"

namespace sdrplay {
namespace dsp {

struct Kernels {
    const char *name;
    // One 1024-byte packet -> xi/xq (spp samples). Real14x504 writes xi only.
    void (*unpack[kPacketFormatCount])(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq);
    // IQ imbalance correction, one pass per sample (r_iqSample in simd/ScalarRef.inc):
    //   stage 0: mode[0] 1 scales I, 2 scales Q: x = (int16)((x * coef[0]) >> 15)
    //            sums[0] += |I|, sums[1] += |Q|   (|-32768| = 32768, 32-bit wrapping sums)
    //   rotate:  I' = (int16)(((I - Q) * 23170) >> 15), Q' = (int16)(((I + Q) * 23170) >> 15)
    //   stage 1: as stage 0 with mode[1], coef[1], sums[2], sums[3]
    // sums are written, not accumulated. coef in [-32768, 32767].
    void (*iqBalance)(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, const int mode[2], const int coef[2],
                      std::uint32_t sums[4]);
    // Decimating FIR over an int32 sequence u whose L-1 history samples precede u[0]:
    //   y[j] = (wrap32(sum_{i<L} h[i] * u[M*j - i] + 0x4000)) >> shift,  j < nOut
    // Reads u[-(L-1) .. M*(nOut-1)] only.
    void (*firDecimate)(const std::int32_t *u, std::uint32_t nOut, std::uint32_t M, const std::int32_t *h,
                        std::uint32_t L, unsigned shift, std::int32_t *y);
    // Exact sum over k<n of xi[k]^2 + xq[k]^2 (xi[k]^2 only when iOnly). Callers use it
    // for n <= 2^22 (sum < 2^53, so a double accumulator is exact
    // and equals (double) of this value; PowerEstimation.cpp).
    std::uint64_t (*sumSquares)(const std::int16_t *xi, const std::int16_t *xq, std::uint32_t n, bool iOnly);
    // DC offset correction on one or two channels (xq == nullptr: xi only):
    //   d += ((double)x - d) * c ; x = sat16(x - (int32)d)   (double, no FMA, evaluated in
    //   8-sample blocks: r_dcChannel in simd/ScalarRef.inc)
    // The I and Q recursions are independent; *dcI / *dcQ hold the tracker state.
    void (*dcCorrect)(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, double *dcI, double *dcQ, double c);
    // fs/4 mixer: I = {x, 0, -x, 0}, Q = {0, x, 0, -x} by k & 3 (block-relative).
    void (*mixFs4)(const std::int16_t *x, std::uint32_t n, std::int32_t *mi, std::int32_t *mq);
    // Table mixer: out[k] = (wrap32(tab[(phase + k) % period] * x[k] + 0x4000)) >> 15.
    // Returns the phase after n samples ((phase + n) % period). phase < period.
    std::uint32_t (*mixTable)(const std::int16_t *x, std::uint32_t n, const std::int32_t *tab, std::uint32_t period,
                              std::uint32_t phase, std::int32_t *out);
    // dst[k] = (int16) src[k] (wrapping)
    void (*narrow16)(const std::int32_t *src, std::uint32_t n, std::int16_t *dst);
};

const Kernels &scalarKernels();
// The active table: the test override if set, else the one chosen at first use from the
// CPU features and SDRPLAY_FORCE_SIMD.
const Kernels &kernels();
void setKernelsForTest(const Kernels *k); // nullptr restores the default (auto-selected) table
// Every table this build can run on this CPU, scalar first.
const std::vector<const Kernels *> &availableKernels();
// The table auto-selection picks: SDRPLAY_FORCE_SIMD (value `force`, may be null) or the
// best supported one. *note (optional) gets a one-line explanation (e.g. a fallback).
const Kernels &selectKernels(const char *force, const char **note = nullptr);
// Lookup by name ("scalar", "sse2", "avx2", "neon"); nullptr if not runnable here.
const Kernels *findKernels(const char *name);

namespace simd {
// Each returns nullptr when the variant is not compiled into this build or the CPU
// lacks the instructions (src/core/dsp/simd/*.cpp).
const Kernels *sse2Kernels();
const Kernels *avx2Kernels();
const Kernels *avx512Kernels();
const Kernels *neonKernels();
} // namespace simd

} // namespace dsp
} // namespace sdrplay
