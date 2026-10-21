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

#include "core/dsp/Kernels.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "core/dsp/DspCommon.h"

namespace sdrplay {
namespace dsp {

namespace {

#include "simd/ScalarRef.inc"

template <PacketFormat F>
void unpackT(const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq) { unpackScalar(F, pkt, xi, xq); }

void iqBalanceScalar(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, const int mode[2], const int coef[2],
                     std::uint32_t sums[4])
{
    sums[0] = sums[1] = sums[2] = sums[3] = 0;
    r_iqBalance(xi, xq, n, mode, coef, sums);
}

void firDecimateScalar(const std::int32_t *u, std::uint32_t nOut, std::uint32_t M, const std::int32_t *h,
                       std::uint32_t L, unsigned shift, std::int32_t *y)
{
    for (std::uint32_t j = 0; j < nOut; ++j) {
        const std::int32_t *p = u + std::size_t(M) * j;
        // int64 products summed mod 2^64 (no UB for any taps); q15Round keeps only the low
        // 32 bits, which equal those of the exact sum
        std::uint64_t acc = 0;
        for (std::uint32_t i = 0; i < L; ++i)
            acc += static_cast<std::uint64_t>(std::int64_t(h[i]) * p[-static_cast<std::ptrdiff_t>(i)]);
        y[j] = q15Round(static_cast<std::uint32_t>(acc), shift);
    }
}

// The power estimate's accumulation order, in double (PowerEstimation.cpp), converted
// back: every partial sum is an integer < 2^53 for the n the callers use, so exact.
std::uint64_t sumSquaresScalar(const std::int16_t *xi, const std::int16_t *xq, std::uint32_t n, bool iOnly)
{
    double acc = 0.0;
    if (iOnly) {
        for (std::uint32_t k = 0; k < n; ++k) {
            const double a = static_cast<double>(xi[k]);
            acc += a * a;
        }
    } else {
        for (std::uint32_t k = 0; k < n; ++k) {
            const double a = static_cast<double>(xi[k]);
            const double b = static_cast<double>(xq[k]);
            const double s = a * a + b * b; // two multiplies, then the add (no contraction)
            acc += s;
        }
    }
    return static_cast<std::uint64_t>(acc);
}

void dcCorrectScalar(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, double *dcI, double *dcQ, double c)
{
    r_dcChannel(xi, n, *dcI, c);
    if (xq) r_dcChannel(xq, n, *dcQ, c);
}

void mixFs4Scalar(const std::int16_t *x, std::uint32_t n, std::int32_t *mi, std::int32_t *mq)
{
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::int32_t v = x[k];
        switch (k & 3u) {
        case 0: mi[k] = v; mq[k] = 0; break;
        case 1: mi[k] = 0; mq[k] = v; break;
        case 2: mi[k] = -v; mq[k] = 0; break;
        default: mi[k] = 0; mq[k] = -v; break;
        }
    }
}

std::uint32_t mixTableScalar(const std::int16_t *x, std::uint32_t n, const std::int32_t *tab, std::uint32_t period,
                             std::uint32_t phase, std::int32_t *out)
{
    for (std::uint32_t k = 0; k < n; ++k) {
        out[k] = q15Round(std::int64_t(tab[phase]) * x[k]);
        if (++phase >= period) phase = 0;
    }
    return phase;
}

void narrow16Scalar(const std::int32_t *src, std::uint32_t n, std::int16_t *dst)
{
    for (std::uint32_t k = 0; k < n; ++k) dst[k] = wrap16(src[k]);
}

const Kernels kScalar = {
    "scalar",
    {unpackT<PacketFormat::Iq14x252>, unpackT<PacketFormat::Iq12x336>, unpackT<PacketFormat::Iq10x384>,
     unpackT<PacketFormat::Iq8x504>, unpackT<PacketFormat::Real14x504>},
    iqBalanceScalar,
    firDecimateScalar,
    sumSquaresScalar,
    dcCorrectScalar,
    mixFs4Scalar,
    mixTableScalar,
    narrow16Scalar,
};

std::atomic<const Kernels *> g_test{nullptr};
std::atomic<const Kernels *> g_auto{nullptr};

const Kernels &autoKernels()
{
    const Kernels *k = g_auto.load(std::memory_order_acquire);
    if (!k) {
        k = &selectKernels(std::getenv("SDRPLAY_FORCE_SIMD"));
        g_auto.store(k, std::memory_order_release); // idempotent: every thread computes the same table
    }
    return *k;
}

} // namespace

const Kernels &scalarKernels() { return kScalar; }

const std::vector<const Kernels *> &availableKernels()
{
    static const std::vector<const Kernels *> list = [] {
        std::vector<const Kernels *> v{&kScalar};
        for (const Kernels *k : {simd::sse2Kernels(), simd::avx2Kernels(), simd::avx512Kernels(), simd::neonKernels()})
            if (k) v.push_back(k);
        return v;
    }();
    return list;
}

const Kernels *findKernels(const char *name)
{
    if (!name) return nullptr;
    for (const Kernels *k : availableKernels())
        if (std::strcmp(k->name, name) == 0) return k;
    return nullptr;
}

const Kernels &selectKernels(const char *force, const char **note)
{
    if (note) *note = nullptr;
    const std::vector<const Kernels *> &all = availableKernels();
    const Kernels *best = all.back(); // ordered scalar < sse2 < avx2 < avx512 (x86) / neon (arm)
    if (!force || !*force) return *best;
    if (const Kernels *k = findKernels(force)) return *k;
    // Not runnable here: the next lower x86 level, else the best available one.
    if (std::strcmp(force, "avx512") == 0) {
        if (const Kernels *k = findKernels("avx2")) {
            if (note) *note = "SDRPLAY_FORCE_SIMD=avx512 not available, using avx2";
            return *k;
        }
    }
    if (std::strcmp(force, "avx512") == 0 || std::strcmp(force, "avx2") == 0) {
        if (const Kernels *k = findKernels("sse2")) {
            if (note) *note = "SDRPLAY_FORCE_SIMD variant not available, using sse2";
            return *k;
        }
    }
    if (note) *note = "SDRPLAY_FORCE_SIMD variant not available, using the best supported table";
    return *best;
}

const Kernels &kernels()
{
    const Kernels *k = g_test.load(std::memory_order_acquire);
    return k ? *k : autoKernels();
}

void setKernelsForTest(const Kernels *k) { g_test.store(k, std::memory_order_release); }

} // namespace dsp
} // namespace sdrplay
