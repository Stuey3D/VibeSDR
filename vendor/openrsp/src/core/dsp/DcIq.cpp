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

#include "core/dsp/DcIq.h"

#include <algorithm>
#include <cstdlib>

#include "core/dsp/DspCommon.h"
#include "core/dsp/Kernels.h"

namespace sdrplay {
namespace dsp {

void DcOffsetCorrector::resetTrackers()
{
    c = c0;
    tracking = false;
    cnt = 0;
    dcI = 0.0;
    dcQ = 0.0;
}

void DcOffsetCorrector::setSampleRate(double fsHz)
{
    c0 = 3000.0 / fsHz;
    trackPkts = static_cast<int>((64.0 * fsHz) / 2000000.0);
}

bool DcOffsetCorrector::run(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool reset, bool dcEnabled,
                            std::uint32_t packetsPerBlock)
{
    if (reset) {
        c = c0;
        tracking = false;
        cnt = 0;
    }
    if (!dcEnabled) return false;
    if (n != 0) {
        // DC correction per channel: dc += ((double)x - dc) * c ; y = sat16(x - (int)dc), evaluated
        // in 8-sample blocks (r_dcChannel, simd/ScalarRef.inc)
        kernels().dcCorrect(xi, xq, n, &dcI, &dcQ, c);
    }
    if (statsEnabled && !tracking) {
        if (cnt == 0) last = dcI;
        if (cnt < trackPkts) {
            cnt += static_cast<int>(packetsPerBlock);
        } else {
            cnt = 0;
            // DC drift over the tracking window, as a 16-bit signed value.
            const int drift = truncToWrap16(dcI - last);
            if (std::abs(drift) < threshold) {
                c = c / 20.0;
                tracking = true;
            }
        }
    }
    return true;
}

void IqImbalanceCorrector::reset()
{
    counter = 0;
    mode[0] = mode[1] = 0;
    sumA[0] = sumA[1] = sumB[0] = sumB[1] = 0.f;
    coef[0] = coef[1] = 32767;
    acc[0] = acc[1] = kIqBalanced;
}

void IqImbalanceCorrector::update(int p)
{
    // Step the accumulator towards balance, limited to [0, 2 * kIqBalanced]. With step >= 0
    // and the accumulator starting at kIqBalanced it never leaves that range, and coef stays
    // in [0, 32767].
    int a;
    if (truncToI32(static_cast<double>(sumA[p] - sumB[p])) < 0) {
        a = std::max(acc[p] - step, 0);
    } else {
        a = std::min(acc[p] + step, 2 * kIqBalanced);
    }
    acc[p] = a;

    // Inside the dead band no scaling is applied (coef is kept); below it the second input
    // is scaled by acc/32768, above it the first input by (65536 - acc)/32768, capped at 32767.
    if (std::abs(a - kIqBalanced) < deadBand) {
        mode[p] = 0;
    } else if (a < kIqBalanced) {
        mode[p] = 2;
        coef[p] = a;
    } else {
        mode[p] = 1;
        coef[p] = std::min(2 * kIqBalanced - a, 32767);
    }
    sumA[p] = 0.f;
    sumB[p] = 0.f;
}

bool IqImbalanceCorrector::run(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool resetFlag, bool enabled)
{
    if (resetFlag) reset();
    if (!enabled) return false;
    const bool due = period <= counter;
    // Stage 0 scaling and sums, the 45-degree rotation, stage 1 scaling and sums: one pass.
    std::uint32_t s[4];
    kernels().iqBalance(xi, xq, n, mode, coef, s);
    if (n != 0) { // otherwise every sum += 0.0f: unchanged
        for (int p = 0; p < 2; ++p) {
            sumA[p] += static_cast<float>(static_cast<std::int32_t>(s[2 * p]));
            sumB[p] += static_cast<float>(static_cast<std::int32_t>(s[2 * p + 1]));
        }
    }
    if (due) {
        update(0);
        update(1);
        counter = 0;
    } else {
        counter += n;
    }
    return true;
}

} // namespace dsp
} // namespace sdrplay
