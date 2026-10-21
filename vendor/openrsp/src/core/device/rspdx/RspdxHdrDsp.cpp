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

#include "core/device/rspdx/RspdxHdrDsp.h"

#include "core/device/rspdx/RspdxTables.h"
#include "core/dsp/DspCommon.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sdrplay {
namespace device {
namespace rspdx {

namespace {
inline std::int32_t wrapMul(std::int32_t a, std::int32_t b)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
inline std::int32_t wrapAdd(std::int32_t a, std::int32_t b)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
// Q15 rounding: (v + 2^14) / 2^15 rounded toward minus infinity (arithmetic shift), the add wrapping
inline std::int32_t rnd15(std::int32_t v) { return dsp::asr(wrapAdd(v, 0x4000), 15); }
inline std::int32_t mix(std::int32_t coef, std::int16_t x) { return rnd15(wrapMul(coef, x)); }

// 23-tap symmetric (h[11] centre) over p[0..22]
std::int32_t fir23(const std::int32_t *p, const std::int32_t *h)
{
    std::int32_t acc = wrapMul(p[11], h[11]);
    for (int i = 0; i < 11; ++i) acc = wrapAdd(acc, wrapMul(wrapAdd(p[22 - i], p[i]), h[i]));
    return acc;
}
// 94-tap symmetric (47 pairs i / 93-i) over p[0..93]
std::int32_t fir94(const std::int32_t *p, const std::int32_t *h)
{
    std::int32_t acc = 0;
    for (int i = 0; i < 47; ++i) acc = wrapAdd(acc, wrapMul(wrapAdd(p[93 - i], p[i]), h[i]));
    return acc;
}
// 140-tap symmetric (70 pairs i / 139-i) over p[0..139]
std::int32_t fir140(const std::int32_t *p, const std::int32_t *h)
{
    std::int32_t acc = 0;
    for (int i = 0; i < 70; ++i) acc = wrapAdd(acc, wrapMul(wrapAdd(p[139 - i], p[i]), h[i]));
    return acc;
}
int bwIndex(int hdrBw)
{
    // 0, 1, 2 select their own coefficient set; anything else -> 3
    return (hdrBw == 0 || hdrBw == 1 || hdrBw == 2) ? hdrBw : 3;
}
} // namespace

void hdrSinCos(double x, double *s, double *c)
{
#if defined(__GLIBC__)
    ::sincos(x, s, c);
#else
    // Off glibc the platform libm may differ from glibc in the last ulp,
    // which can move a 32767-scaled value across a rounding boundary.
    *s = std::sin(x);
    *c = std::cos(x);
#endif
}

std::int16_t hdrDecim4NcoRound(double v)
{
    double r;
    if (v < 0.0) r = std::ceil(v - 0.5);
    else r = std::floor(v + 0.5);
    return static_cast<std::int16_t>(static_cast<std::int32_t>(r));
}

// ------------------------------------------------------------------ HdrDecim4
void HdrDecim4::reset(std::int32_t frf)
{
    std::fill(xi_.begin(), xi_.end(), 0);
    std::fill(xq_.begin(), xq_.end(), 0);
    std::fill(yi_.begin(), yi_.end(), 0);
    std::fill(yq_.begin(), yq_.end(), 0);
    phi_ = 0.0;
    dphi_ = (static_cast<double>(frf) * kHdrDecim4TwoPi) / kHdrDecim4NcoFs;
}

std::uint32_t HdrDecim4::process(const std::int16_t *x, std::uint32_t n, int hdrBw, std::int16_t *outI,
                             std::int16_t *outQ)
{
    const std::int32_t *h2 = kHdrDecim4Stage2[bwIndex(hdrBw)];
    xi_.resize(22 + std::size_t(n));
    xq_.resize(22 + std::size_t(n));
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::int16_t v = x[k];
        double s = 0.0, c = 0.0;
        hdrSinCos(phi_, &s, &c);
        const std::int16_t ci = hdrDecim4NcoRound(c * kHdrDecim4NcoScale);
        const std::int16_t si = hdrDecim4NcoRound(-s * kHdrDecim4NcoScale);
        xi_[22 + k] = mix(ci, v);
        xq_[22 + k] = mix(si, v);
        phi_ = phi_ + dphi_;
        if (phi_ >= kHdrDecim4TwoPi) phi_ = phi_ - kHdrDecim4TwoPi;
    }
    const std::uint32_t n2 = n >> 1;
    yi_.resize(93 + std::size_t(n2));
    yq_.resize(93 + std::size_t(n2));
    for (std::uint32_t j = 0; j < n2; ++j) {
        yi_[93 + j] = rnd15(fir23(&xi_[2 * j], kHdrDecim4Stage1));
        yq_[93 + j] = rnd15(fir23(&xq_[2 * j], kHdrDecim4Stage1));
    }
    std::memmove(xi_.data(), xi_.data() + n, 22 * sizeof(std::int32_t));
    std::memmove(xq_.data(), xq_.data() + n, 22 * sizeof(std::int32_t));
    const std::uint32_t n4 = n >> 2;
    for (std::uint32_t j = 0; j < n4; ++j) {
        outI[j] = static_cast<std::int16_t>(rnd15(fir94(&yi_[2 * j], h2)));
        outQ[j] = static_cast<std::int16_t>(rnd15(fir94(&yq_[2 * j], h2)));
    }
    std::memmove(yi_.data(), yi_.data() + n2, 93 * sizeof(std::int32_t));
    std::memmove(yq_.data(), yq_.data() + n2, 93 * sizeof(std::int32_t));
    return n4;
}

// ------------------------------------------------------------------ HdrDecim3
void HdrDecim3::reset(std::int32_t frf)
{
    std::fill(xi_.begin(), xi_.end(), 0);
    std::fill(xq_.begin(), xq_.end(), 0);
    table_ = kHdrDecim3NcoCount - 1;
    for (int i = 0; i < kHdrDecim3NcoCount - 1; ++i)
        if (kHdrDecim3Nco[i].frfHz == frf) table_ = i;
    iI_ = 0;
    iQ_ = kHdrDecim3Nco[table_].qStart;
}

std::uint32_t HdrDecim3::process(const std::int16_t *x, std::uint32_t n, int hdrBw, std::int16_t *outI,
                             std::int16_t *outQ)
{
    const HdrDecim3Nco &t = kHdrDecim3Nco[table_];
    const std::int32_t *h = kHdrDecim3Fir[bwIndex(hdrBw)];
    xi_.resize(139 + std::size_t(n));
    xq_.resize(139 + std::size_t(n));
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::int16_t v = x[k];
        xi_[139 + k] = mix(t.table[iI_++], v);
        xq_[139 + k] = mix(t.table[iQ_++], v);
        if (iI_ >= t.size) iI_ = 0;
        if (iQ_ >= t.size) iQ_ = 0;
    }
    // one output for every third input, starting with the first: ceil(n / 3) outputs
    std::uint32_t out = 0;
    for (std::uint32_t m = 0; m < n; m += 3, ++out) {
        outI[out] = static_cast<std::int16_t>(rnd15(fir140(&xi_[3 * std::size_t(out)], h)));
        outQ[out] = static_cast<std::int16_t>(rnd15(fir140(&xq_[3 * std::size_t(out)], h)));
    }
    std::memmove(xi_.data(), xi_.data() + n, 139 * sizeof(std::int32_t));
    std::memmove(xq_.data(), xq_.data() + n, 139 * sizeof(std::int32_t));
    return out;
}

// ------------------------------------------------------------------ tracker
bool HdrDspTracker::update(int bw, bool active, std::int32_t f, bool streamReset)
{
    const bool changed = !valid || bw != hdrBw || active != hdrActive || f != frf;
    valid = true;
    hdrBw = bw;
    hdrActive = active;
    frf = f;
    return changed || streamReset;
}

} // namespace rspdx
} // namespace device
} // namespace sdrplay
