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

#include "core/dsp/DownConverter.h"

#include <algorithm>
#include <initializer_list>

#include <sdrplay_api.h>

#include "core/dsp/DspCommon.h"
#include "core/dsp/DspTables.h"
#include "core/dsp/Kernels.h"

namespace sdrplay {
namespace dsp {

DownConverterConfig selectDownConverter(double fsMHz, int bwType, int ifType)
{
    DownConverterConfig c;
    c.ifType = ifType;
    c.lif = ifType != sdrplay_api_IF_Zero;
    if (ifType == sdrplay_api_IF_2_048) c.mixHz = 2048000.0;
    else if (ifType == sdrplay_api_IF_1_620) c.mixHz = 1620000.0;
    else if (ifType == sdrplay_api_IF_0_450) c.mixHz = 450000.0;
    const double scaled = 1000000.0 * fsMHz;
    const int fsi = static_cast<int>(scaled + 0.5);
    const auto set = [&c](DownConvPath path, std::uint32_t countShift, std::uint32_t countDivisor) {
        c.enable = path != DownConvPath::kNone;
        c.path = path;
        c.countShift = countShift;
        c.countDivisor = countDivisor;
    };
    bool matched = false;
    if (fsi == 8192000 || fsi == 8000000) {
        if (bwType == sdrplay_api_BW_1_536 && ifType == sdrplay_api_IF_2_048) {
            set(DownConvPath::kHalfBand4, 2, 1);
            if (fsi == 8000000) {
                c.mixHz = 2000000.0;
                c.adsbCfg = true;
            }
            matched = true;
        } else if (fsi == 8000000 && bwType == sdrplay_api_BW_5_000 && ifType == sdrplay_api_IF_2_048) {
            set(DownConvPath::kHalfBand4, 2, 1);
            c.mixHz = 2000000.0;
            c.adsbCfg = true;
            matched = true;
        }
    } else if (fsi == 2000000) {
        if (ifType == sdrplay_api_IF_0_450 && bwType <= sdrplay_api_BW_0_300) {
            set(DownConvPath::kHalfBand4, 2, 1);
            matched = true;
        } else if (bwType == sdrplay_api_BW_0_600 && ifType == sdrplay_api_IF_0_450) {
            set(DownConvPath::kHalfBand2, 1, 1);
            matched = true;
        }
    } else if (fsi == 6000000 && bwType <= sdrplay_api_BW_1_536 && ifType == sdrplay_api_IF_1_620) {
        set(DownConvPath::kLif1620Decim3, 0, 3);
        c.mixHz = 1580000.0;
        matched = true;
    }
    if (!matched) set(DownConvPath::kNone, 0, 1);
    c.ionly = c.lif && (!c.enable || c.path == DownConvPath::kNone);
    return c;
}

std::uint32_t numActivePackets(const DownConverterConfig &dc, bool decEnable, std::uint8_t decFactor)
{
    // Packets per down-converted output unit: 3 for the decimate-by-3 path, 4 for the other
    // down-converter paths, 1 without down-conversion.
    const bool downConverting = dc.enable && dc.path != DownConvPath::kNone;
    const std::uint32_t unit = !downConverting ? 1u : dc.path == DownConvPath::kLif1620Decim3 ? 3u : 4u;
    // Block size when user decimation does not scale it: the unit, at least 4 packets.
    const std::uint32_t plain = downConverting ? unit : 4u;
    if (!decEnable) return plain;
    switch (decFactor) {
    case 2:
    case 4:
    case 8:
    case 16: return unit * 4;
    case 32: return unit * 8;
    default: return plain;
    }
}

// ------------------------------------------------------------------ Fir helper

void DownConverter::Fir::setup(std::uint32_t L, std::uint32_t maxIn)
{
    hist = L - 1;
    buf.assign(hist + maxIn, 0);
}

void DownConverter::Fir::clear() { std::fill(buf.begin(), buf.begin() + hist, 0); }

std::uint32_t DownConverter::Fir::run(std::uint32_t n, std::uint32_t M, const std::int32_t *h, std::uint32_t L,
                                      std::int32_t *y)
{
    const std::uint32_t nOut = (n + M - 1) / M;
    kernels().firDecimate(input(), nOut, M, h, L, 15, y);
    // keep the last L-1 samples (history + block) as the next history
    std::copy(buf.begin() + n, buf.begin() + n + hist, buf.begin());
    return nOut;
}

// ------------------------------------------------------------------ DownConverter

DownConverter::DownConverter()
{
    const std::uint32_t maxIn = kMaxBlockSamples;
    hb1I_.setup(23, maxIn);
    hb1Q_.setup(23, maxIn);
    hb2I_.setup(51, maxIn / 2);
    hb2Q_.setup(51, maxIn / 2);
    h58I_.setup(58, maxIn);
    h58Q_.setup(58, maxIn);
    lpfI_.setup(51, maxIn);
    lpfQ_.setup(51, maxIn);
    bpf_.setup(51, maxIn);
    hbfI_.setup(23, maxIn);
    hbfQ_.setup(23, maxIn);
    tmpI_.assign(maxIn, 0);
    tmpQ_.assign(maxIn, 0);
    init();
}

void DownConverter::resetState()
{
    for (Fir *f : {&hb1I_, &hb1Q_, &hb2I_, &hb2Q_, &h58I_, &h58Q_, &lpfI_, &lpfQ_, &bpf_, &hbfI_, &hbfQ_}) f->clear();
    phase40_ = 0;
    phaseI300_ = 0;
    phaseQ300_ = 75;
}

void DownConverter::init() { resetState(); }

void DownConverter::normalPath(const DownConverterConfig &cfg, std::int16_t *xi, std::int16_t *xq, std::uint32_t n)
{
    const Kernels &K = kernels();
    std::int32_t *mi = hb1I_.input();
    std::int32_t *mq = hb1Q_.input();
    if (cfg.ifType == sdrplay_api_IF_2_048) {
        // fs/4: I = {x, 0, -x, 0}, Q = {0, x, 0, -x}, groups of 4 from the block start
        K.mixFs4(xi, n, mi, mq);
    } else {
        // COS40/SIN40, processed 4 samples at a time; phase wraps when it passes 39
        // (phase40_ is a multiple of 4 and 40 % 4 == 0, so whole groups are a plain
        // per-sample phase mod 40; a partial last group still advances the phase by 4)
        const std::uint32_t whole = n & ~3u;
        K.mixTable(xi, whole, tables::kCos40, 40, std::uint32_t(phase40_), mi);
        K.mixTable(xi, whole, tables::kSin40, 40, std::uint32_t(phase40_), mq);
        phase40_ = int((std::uint32_t(phase40_) + whole) % 40u);
        for (std::uint32_t k = whole; k < n; k += 4) {
            for (std::uint32_t u = 0; u < 4 && k + u < n; ++u) {
                const std::int32_t v = xi[k + u];
                mi[k + u] = q15Round(std::int64_t(tables::kCos40[phase40_ + int(u)]) * v);
                mq[k + u] = q15Round(std::int64_t(v) * tables::kSin40[phase40_ + int(u)]);
            }
            phase40_ += 4;
            if (phase40_ >= 40) phase40_ = 0;
        }
    }
    if (cfg.path == DownConvPath::kHalfBand2) {
        const std::uint32_t m = hb1I_.run(n, 2, tables::kHb1, 23, tmpI_.data());
        hb1Q_.run(n, 2, tables::kHb1, 23, tmpQ_.data());
        K.narrow16(tmpI_.data(), m, xi);
        K.narrow16(tmpQ_.data(), m, xq);
        return;
    }
    const std::uint32_t m = hb1I_.run(n, 2, tables::kHb1, 23, hb2I_.input());
    hb1Q_.run(n, 2, tables::kHb1, 23, hb2Q_.input());
    const std::uint32_t m2 = hb2I_.run(m, 2, tables::kHb2, 51, tmpI_.data());
    hb2Q_.run(m, 2, tables::kHb2, 51, tmpQ_.data());
    K.narrow16(tmpI_.data(), m2, xi);
    K.narrow16(tmpQ_.data(), m2, xq);
}

void DownConverter::lif1620Path(std::int16_t *xi, std::int16_t *xq, std::uint32_t n)
{
    const Kernels &K = kernels();
    std::int32_t *mi = h58I_.input();
    std::int32_t *mq = h58Q_.input();
    phaseI300_ = int(K.mixTable(xi, n, tables::kT300, 300, std::uint32_t(phaseI300_), mi));
    phaseQ300_ = int(K.mixTable(xi, n, tables::kT300, 300, std::uint32_t(phaseQ300_), mq));
    const std::uint32_t m = h58I_.run(n, 3, tables::kH58, 58, tmpI_.data());
    h58Q_.run(n, 3, tables::kH58, 58, tmpQ_.data());
    K.narrow16(tmpI_.data(), m, xi);
    K.narrow16(tmpQ_.data(), m, xq);
}

void DownConverter::adsbLowPass(std::int16_t *xi, std::int16_t *xq, std::uint32_t n)
{
    const Kernels &K = kernels();
    K.mixFs4(xi, n, lpfI_.input(), lpfQ_.input());
    lpfI_.run(n, 1, tables::kAdsbLpf51, 51, tmpI_.data());
    lpfQ_.run(n, 1, tables::kAdsbLpf51, 51, tmpQ_.data());
    K.narrow16(tmpI_.data(), n, xi);
    K.narrow16(tmpQ_.data(), n, xq);
}

void DownConverter::adsbBandPass(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool bandPass2MHz)
{
    std::int32_t *in = bpf_.input();
    for (std::uint32_t k = 0; k < n; ++k) in[k] = xi[k];
    std::int32_t *u = tmpI_.data(); // band-pass output: |u| <= 2^16 after the >> 15
    bpf_.run(n, 1, bandPass2MHz ? tables::kAdsbBpf2 : tables::kAdsbBpf3, 51, u);
    std::int32_t *mi = hbfI_.input();
    std::int32_t *mq = hbfQ_.input();
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::int32_t v = u[k];
        switch (k & 3u) {
        case 0: mi[k] = v; mq[k] = 0; break;
        case 1: mi[k] = 0; mq[k] = v; break;
        case 2: mi[k] = -v; mq[k] = 0; break;
        default: mi[k] = 0; mq[k] = -v; break;
        }
    }
    hbfI_.run(n, 1, tables::kHb1, 23, tmpI_.data());
    hbfQ_.run(n, 1, tables::kHb1, 23, tmpQ_.data());
    const Kernels &K = kernels();
    K.narrow16(tmpI_.data(), n, xi);
    K.narrow16(tmpQ_.data(), n, xq);
}

std::uint32_t DownConverter::run(const DownConverterConfig &cfg, std::int16_t *xi, std::int16_t *xq, std::uint32_t n,
                                 bool reset, int adsbMode)
{
    if (!cfg.enable) return n;
    if (cfg.path == DownConvPath::kNone) return n;
    if (cfg.path == DownConvPath::kLif1620Decim3) {
        if (reset) {
            h58I_.clear();
            h58Q_.clear();
            phaseI300_ = 0;
            phaseQ300_ = 75;
        }
        lif1620Path(xi, xq, n);
        return (n + 2) / 3;
    }
    if (reset) {
        // half-band paths: clear all their buffers (incl. the ADS-B ones) and the 40-phase
        for (Fir *f : {&hb1I_, &hb1Q_, &hb2I_, &hb2Q_, &lpfI_, &lpfQ_, &bpf_, &hbfI_, &hbfQ_}) f->clear();
        phase40_ = 0;
    }
    if (cfg.adsbCfg) {
        if (adsbMode == sdrplay_api_ADSB_NO_DECIMATION_LOWPASS) {
            adsbLowPass(xi, xq, n);
            return n;
        }
        if (adsbMode >= sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_2MHZ) {
            // band-pass modes: 2 MHz, else (3 MHz or any larger value) 3 MHz
            adsbBandPass(xi, xq, n, adsbMode == sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_2MHZ);
            return n;
        }
    }
    normalPath(cfg, xi, xq, n);
    return cfg.path == DownConvPath::kHalfBand2 ? n / 2 : n / 4;
}

} // namespace dsp
} // namespace sdrplay
