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

#include "core/dsp/UserDecimator.h"

#include <cstring>

#include "core/dsp/DspCommon.h"
#include "core/dsp/DspTables.h"
#include "core/dsp/Kernels.h"

namespace sdrplay {
namespace dsp {

namespace {

// Input sample idx; 0 beyond the readable part of the block.
inline std::int32_t rd(const std::int16_t *p, std::size_t readable, std::size_t idx)
{
    return idx < readable ? std::int32_t(p[idx]) : 0;
}

// log2 of a valid decimation factor; 0 for any other factor.
std::uint32_t decimationShift(std::uint8_t factor)
{
    switch (factor) {
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    case 16: return 4;
    case 32: return 5;
    default: return 0;
    }
}

} // namespace

bool isValidDecimationFactor(std::uint8_t factor) { return decimationShift(factor) != 0; }

DecimationSetting updateDecimation(bool enable, std::uint8_t factor, std::uint8_t wide)
{
    DecimationSetting s;
    s.enable = enable;
    s.factor = factor;
    s.wide = wide;
    const std::uint32_t sh = decimationShift(factor);
    if (enable && sh != 0) {
        s.shift = sh;
        const std::int32_t r = std::int32_t(1) << (sh - 1);
        s.round[0] = r;
        s.round[1] = -r;
    }
    return s;
}

void UserDecimator::init()
{
    active_ = DecimationSetting{};
    std::memset(&hb1_, 0, sizeof hb1_);
    std::memset(&fir108_, 0, sizeof fir108_);
    std::memset(hb3_, 0, sizeof hb3_);
}

void UserDecimator::boxcar(const BlockBuffers &b, std::uint32_t nOut, std::uint32_t len, std::int32_t rPos,
                           std::int32_t rNeg, unsigned shift, bool ionly)
{
    for (std::uint32_t j = 0; j < nOut; ++j) {
        const std::size_t base = std::size_t(j) * len;
        std::uint32_t si = 0, sq = 0;
        for (std::uint32_t k = 0; k < len; ++k) {
            si += static_cast<std::uint32_t>(rd(b.xi, b.xiReadable, base + k));
            if (!ionly) sq += static_cast<std::uint32_t>(rd(b.xq, b.xqReadable, base + k));
        }
        const std::int32_t vi = static_cast<std::int32_t>(si);
        b.xi[j] = wrap16(asr(wrap32(std::int64_t(vi) + (vi < 0 ? rNeg : rPos)), shift));
        if (!ionly) {
            const std::int32_t vq = static_cast<std::int32_t>(sq);
            b.xq[j] = wrap16(asr(wrap32(std::int64_t(vq) + (vq < 0 ? rNeg : rPos)), shift));
        }
    }
}

// One ring FIR stage, decimate by 2, in place: per output j the pair x[2j], x[2j+1] is
// written at w, w+1 and the taps read x[2j] (at w) and the older inputs below it.
void UserDecimator::ringStage(const BlockBuffers &b, Ring &ring, std::uint32_t mask, const std::int32_t *h,
                              std::uint32_t L, unsigned shift, std::uint32_t nOut, bool ionly)
{
    if (&kernels() != &scalarKernels()) { // the scalar table keeps the reference code below
        ringStageLinear(b, ring, mask, h, L, shift, nOut, ionly);
        return;
    }
    std::uint32_t w = ring.w;
    for (std::uint32_t j = 0; j < nOut; ++j) {
        const std::size_t s0 = std::size_t(j) * 2;
        ring.i[w] = rd(b.xi, b.xiReadable, s0);
        ring.i[w + 1] = rd(b.xi, b.xiReadable, s0 + 1);
        if (!ionly) {
            ring.q[w] = rd(b.xq, b.xqReadable, s0);
            ring.q[w + 1] = rd(b.xq, b.xqReadable, s0 + 1);
        }
        std::int64_t ai = 0, aq = 0;
        for (std::uint32_t t = 0; t < L; ++t) {
            const std::uint32_t idx = (w - t) & mask;
            ai += std::int64_t(h[t]) * ring.i[idx];
            if (!ionly) aq += std::int64_t(h[t]) * ring.q[idx];
        }
        w = (w + 2) & mask;
        b.xi[j] = wrap16(q15Round(ai, shift));
        if (!ionly) b.xq[j] = wrap16(q15Round(aq, shift));
    }
    ring.w = w;
}

// ringStage as firDecimate (M = 2) over [history | x]: output j reads x[2j - t] for t < L,
// where x[-t] (t >= 1) is the history entry ring[(w - t) & mask] (L <= mask + 1). Afterwards
// the ring holds the newest inputs, as ringStage leaves it. All inputs are copied before any
// output is written, which is safe in place because output j only overwrites x[j], j <= 2j.
void UserDecimator::ringStageLinear(const BlockBuffers &b, Ring &ring, std::uint32_t mask, const std::int32_t *h,
                                    std::uint32_t L, unsigned shift, std::uint32_t nOut, bool ionly)
{
    const std::uint32_t hist = L - 1;
    const std::uint32_t nIn = 2 * nOut;
    if (lin_.size() < hist + nIn) lin_.resize(hist + nIn);
    if (linOut_.size() < nOut) linOut_.resize(nOut);
    const Kernels &K = kernels();
    const std::uint32_t w = ring.w;
    for (int ch = 0; ch < (ionly ? 1 : 2); ++ch) {
        std::int32_t *rbuf = ch == 0 ? ring.i : ring.q;
        const std::int16_t *x = ch == 0 ? b.xi : b.xq;
        const std::size_t readable = ch == 0 ? b.xiReadable : b.xqReadable;
        std::int32_t *u = lin_.data() + hist;
        for (std::uint32_t t = 1; t <= hist; ++t) u[-std::ptrdiff_t(t)] = rbuf[(w - t) & mask];
        const std::uint32_t direct = static_cast<std::uint32_t>(readable < nIn ? readable : nIn);
        for (std::uint32_t k = 0; k < direct; ++k) u[k] = x[k];
        for (std::uint32_t k = direct; k < nIn; ++k) u[k] = 0; // as rd(): beyond the readable part
        K.firDecimate(u, nOut, 2, h, L, shift, linOut_.data());
        K.narrow16(linOut_.data(), nOut, ch == 0 ? b.xi : b.xq);
        const std::uint32_t keep = nIn < mask + 1 ? nIn : mask + 1;
        for (std::uint32_t k = nIn - keep; k < nIn; ++k) rbuf[(w + k) & mask] = u[k];
    }
    ring.w = (w + nIn) & mask;
}

void UserDecimator::wide1(const BlockBuffers &b, std::uint32_t n, bool reset, bool ionly)
{
    if (reset) { // HB1 + FIR108 rings and their write positions
        std::memset(&hb1_, 0, sizeof hb1_);
        std::memset(&fir108_, 0, sizeof fir108_);
    }
    // Factor M = 2^s: a boxcar of M/2 (M = 32: a boxcar of 8, then HB1), then FIR108
    // decimating by 2. An invalid factor (s = 0) runs FIR108 alone over n outputs. The shift
    // was staged with the factor of the last Update(Ctrl_Decimation) while M is the live
    // decimationFactor, so they can disagree; a stage whose shift would be negative produces
    // no output.
    const std::uint32_t s = active_.shift;
    const std::uint32_t M = active_.factor;
    if (M == 32) {
        if (s >= 2) {
            const unsigned bs = s - 2;
            const std::uint32_t cnt = n >> bs;
            if (cnt != 0) boxcar(b, cnt, 8, asr(active_.round[0], 2), asr(active_.round[1], 2), bs, ionly);
        }
        if (s >= 1) {
            const std::uint32_t c2 = n >> (s - 1);
            if (c2 != 0) ringStage(b, hb1_, 31, tables::kHb1, 23, 15, c2, ionly);
        }
    } else if (M > 2 && s != 0) {
        const unsigned bs = s - 1;
        const std::uint32_t cnt = n >> bs;
        if (cnt != 0) boxcar(b, cnt, M >> 1, asr(active_.round[0], 1), asr(active_.round[1], 1), bs, ionly);
    }
    const std::uint32_t cf = n >> s;
    if (cf != 0) ringStage(b, fir108_, 127, tables::kFir108, 108, 15, cf, ionly);
}

void UserDecimator::wide2(const BlockBuffers &b, std::uint32_t n, bool reset, bool ionly)
{
    if (reset) { // HB3 rings + FIR108 rings and their write positions
        std::memset(hb3_, 0, sizeof hb3_);
        std::memset(&fir108_, 0, sizeof fir108_);
    }
    const std::uint32_t s = active_.shift;
    if (s == 0) return; // invalid factor: block left undecimated; see header
    for (std::uint32_t k = 1; k < s; ++k) {
        const std::uint32_t cnt = n >> k;
        if (cnt != 0) ringStage(b, hb3_[k - 1], 31, tables::kHb3, 31, 15, cnt, ionly);
    }
    const std::uint32_t cf = n >> s;
    if (cf != 0) ringStage(b, fir108_, 127, tables::kFir108b, 108, 16, cf, ionly);
}

void UserDecimator::run(const BlockBuffers &buffers, std::uint32_t n, bool reset, bool ionly)
{
    if (!active_.enable) return;
    if (active_.factor < 2 || active_.factor > 32) return;
    // The kernels read the n input samples of this block and nothing beyond it.
    BlockBuffers b = buffers;
    if (b.xiReadable > n) b.xiReadable = n;
    if (b.xqReadable > n) b.xqReadable = n;
    if (active_.wide == 0) {
        // boxcar of `factor` inputs, n >> shift outputs
        const std::uint32_t cnt = n >> active_.shift;
        if (cnt != 0) boxcar(b, cnt, active_.factor, active_.round[0], active_.round[1], active_.shift, ionly);
    } else if (active_.wide == 1) {
        wide1(b, n, reset, ionly);
    } else {
        wide2(b, n, reset, ionly);
    }
}

} // namespace dsp
} // namespace sdrplay
