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

// PowerEstimation. See the header for the arithmetic contract.
#include "core/dsp/agc/PowerEstimation.h"

#include "core/dsp/Kernels.h"

#include <sdrplay_api.h>

#include <cmath>

namespace sdrplay {
namespace dsp {
namespace agc {

namespace {
constexpr double kTwoPowMinus30 = 0x1p-30;
} // namespace

double sumSquares(const std::int16_t *xi, const std::int16_t *xq, std::uint32_t n, bool iOnly)
{
    // Reference: double acc += a*a (+ b*b) in sample order. Every term and partial sum is an
    // integer < 2^53 while n <= 2^22, so the exact integer sum (any order, any kernel
    // table) converts to the identical double. Larger n: the scalar reference's rounding.
    const Kernels &k = (n <= (1u << 22)) ? kernels() : scalarKernels();
    return static_cast<double>(k.sumSquares(xi, xq, n, iOnly));
}

double powerFromSum(double sum, std::uint32_t n, bool *valid)
{
    if (valid) *valid = false;
    const double p = (sum * kTwoPowMinus30) / static_cast<double>(n);
    if (!(p > 0.0)) return kNoPowerDb;   // also catches 0/0 (n == 0)
    if (valid) *valid = true;
    const double l = 10.0 * std::log10(p);
    const double h = (l >= 0.0) ? 0.5 : -0.5;
    return h + l;
}

void OverloadDetector::reset()
{
    ov_ = 0;
    ack_ = false;
    detectedSent_ = false;
    correctedSent_ = false;
    cnt_ = 0;
    resendN_ = kOverloadResendBlocks;
}

OverloadMsg OverloadDetector::update(double powDb, const OverloadContext &ctx)
{
    // Latched message: wait for the ack, re-send every resendN_ blocks.
    if ((detectedSent_ || correctedSent_) && !ack_) {
        if (cnt_++ < resendN_) return OverloadMsg::None;
        cnt_ = 0;
        return detectedSent_ ? OverloadMsg::Detected : OverloadMsg::Corrected;
    }
    cnt_ = 0;
    ack_ = false;
    detectedSent_ = false;
    correctedSent_ = false;

    if (powDb > kOverloadDetectDb) {
        if (ctx.hdr || ctx.agcEnable == 0 || ctx.lastAgcGr == MAX_BB_GR) {
            if (++ov_ != 6) return OverloadMsg::None;   // detected on the 6th block in a row
            detectedSent_ = true;
            return OverloadMsg::Detected;
        }
        // AGC on with range left: not an overload; end any run (below)
    } else if (!(powDb < kOverloadCorrectDb)) {
        // -16 <= pow <= -14 (hysteresis): keep the run unless HDR or an AGC with range left
        if (!ctx.hdr && (ctx.agcEnable == 0 || ctx.lastAgcGr == MAX_BB_GR)) return OverloadMsg::None;
    }
    // end of a run: Corrected only after a run of at least 5 blocks
    if (ov_ < 5) {
        ov_ = 0;
        return OverloadMsg::None;
    }
    correctedSent_ = true;
    ov_ = 0;
    return OverloadMsg::Corrected;
}

double PowerEstimator::estimate(const std::int16_t *xi, const std::int16_t *xq, std::uint32_t n, bool iOnly,
                                const OverloadContext &ctx, OverloadMsg *msg)
{
    return estimateFromSum(sumSquares(xi, xq, n, iOnly), n, ctx, msg);
}

double PowerEstimator::estimateFromSum(double sum, std::uint32_t n, const OverloadContext &ctx, OverloadMsg *msg)
{
    bool valid = false;
    const double pw = powerFromSum(sum, n, &valid);
    const OverloadMsg m = valid ? det_.update(pw, ctx) : OverloadMsg::None;  // P <= 0: plain return
    if (msg) *msg = m;
    return pw;
}

} // namespace agc
} // namespace dsp
} // namespace sdrplay
