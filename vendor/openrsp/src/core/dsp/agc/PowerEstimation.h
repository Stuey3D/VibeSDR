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

// Block power estimate and ADC-overload state machine (pure, portable).
//
// The arithmetic has a fixed operation order (build with -ffp-contract=off):
//   sum = Σ ((double)xi² + (double)xq²)        (I-only: Σ (double)xi²), accumulated in order
//   P   = sum * 2^-30 / (double)n
//   pow = P > 0 ? 10*log10(P) + (10*log10(P) >= 0 ? 0.5 : -0.5) : -200.0     (not truncated)
// Every partial sum is an exact integer (< 2^53), so the sum does not depend on the order;
// log10 is the C library's.
//
// Overload messages (PowerOverloadChange, tuner = the channel's tuner): 6 consecutive blocks
// above -14 dBFS -> Overload_Detected, unless the AGC is on and still has range (last AGC gRdB
// below MAX_BB_GR). Below -16 dBFS (or AGC regaining range) after a detection -> Overload_Corrected.
// After either message no new decision is taken until Update(Ctrl_OverloadMsgAck); meanwhile
// the same message is re-sent every 1000 blocks.
#pragma once

#include <cstdint>

namespace sdrplay {
namespace dsp {
namespace agc {

constexpr double kNoPowerDb = -200.0;
constexpr double kOverloadDetectDb = -14.0;
constexpr double kOverloadCorrectDb = -16.0;
constexpr std::uint32_t kOverloadResendBlocks = 1000;  // initial re-send period

// Exact Σ(xi²+xq²) (or Σ xi² when iOnly), equal to the in-order double accumulation.
double sumSquares(const std::int16_t *xi, const std::int16_t *xq, std::uint32_t n, bool iOnly);

// pow_dB from an exact sum of squares; kNoPowerDb when P <= 0 or n == 0 (0/0 -> NaN), in which
// case *valid (if given) is false and the overload state machine is skipped.
double powerFromSum(double sum, std::uint32_t n, bool *valid = nullptr);

// sdrplay_api_PowerOverloadCbEventIdT
enum class OverloadMsg : int { None = -1, Detected = 0, Corrected = 1 };

// What the overload decision reads from the channel besides the power.
struct OverloadContext {
    bool hdr = false;        // RSPdx HDR mode; always false on RSP1B
    int agcEnable = 0;       // ctrlParams.agc.enable as seen by the stream thread
    int lastAgcGr = -1;      // last gRdB applied by the AGC (-1 after channel init)
};

class OverloadDetector {
public:
    // Channel init: counters and latches cleared, re-send period 1000.
    void reset();

    // One decision for a block whose power is powDb. Not called when powerFromSum
    // returned kNoPowerDb (no decision is taken for such a block).
    OverloadMsg update(double powDb, const OverloadContext &ctx);

    // Update(Ctrl_OverloadMsgAck): sets the ack latch.
    void ack() { ack_ = true; }

    int overCount() const { return ov_; }
    bool detectedSent() const { return detectedSent_; }
    bool correctedSent() const { return correctedSent_; }
    bool acked() const { return ack_; }
    std::uint32_t resendCount() const { return cnt_; }

private:
    int ov_ = 0;
    bool ack_ = false;
    bool detectedSent_ = false;
    bool correctedSent_ = false;
    std::uint32_t cnt_ = 0;
    std::uint32_t resendN_ = kOverloadResendBlocks;
};

// PowerEstimation as a whole: power of the block plus the overload decision.
class PowerEstimator {
public:
    void reset() { det_.reset(); }
    // Returns pow_dB; *msg receives the message to send (None if nothing).
    double estimate(const std::int16_t *xi, const std::int16_t *xq, std::uint32_t n, bool iOnly,
                    const OverloadContext &ctx, OverloadMsg *msg);
    // Same, from a precomputed exact sum of squares (replay vectors).
    double estimateFromSum(double sum, std::uint32_t n, const OverloadContext &ctx, OverloadMsg *msg);

    OverloadDetector &detector() { return det_; }
    const OverloadDetector &detector() const { return det_; }

private:
    OverloadDetector det_;
};

} // namespace agc
} // namespace dsp
} // namespace sdrplay
