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

// DC offset correction and IQ imbalance correction.
#pragma once

#include <cstdint>

namespace sdrplay {
namespace dsp {

// DC drift (sample units, over one tracking window) below which the corrector switches to
// the slow tracking coefficient c / 20.
constexpr int kDcTrackThreshold = 150;
// IQ balance accumulator: kIqBalanced means balanced (gain ratio 1 in Q15).
constexpr int kIqBalanced = 32768;
// Accumulator step per IQ balance update.
constexpr int kIqAccStep = 38;
// Dead band around kIqBalanced in which no scaling is applied: 3 steps.
constexpr int kIqDeadBand = 3 * kIqAccStep;

// The coefficient c, its reload value c0, trackPkts and the threshold are
// device-wide; the trackers are per channel. The RSP1B has one channel, so one object holds both.
class DcOffsetCorrector {
public:
    // Channel init and Update(Ctrl_DCoffsetIQimbalance) with DC enabled:
    // trackers to 0, c = c0, tracking/cnt cleared.
    void resetTrackers();
    // Stream thread on an fs change (confirmed fs, Hz): c0 = 3000/fs, trackPkts = (int)(64*fs/2e6).
    void setSampleRate(double fsHz);
    // DC correction of one block (xq = nullptr for low IF). packetsPerBlock is the
    // device-wide N added to the tracking counter per block.
    // Returns false when DC is not enabled (the caller then logs the error
    // "DCoffset not enabled" for every block); the reset part still happens.
    bool run(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool reset, bool dcEnabled,
             std::uint32_t packetsPerBlock);

    // State (exposed for tests and the AGC/diagnostics).
    double c = 0.0015;
    double c0 = 0.0015;
    int trackPkts = 64;
    int threshold = kDcTrackThreshold;
    double dcI = 0.0;      // never reset by stream resets
    double dcQ = 0.0;
    double last = 0.0;
    int cnt = 0;
    bool statsEnabled = true;
    bool tracking = false;
};

class IqImbalanceCorrector {
public:
    void reset(); // mode 0, sums 0, acc kIqBalanced, coef 32767, counter 0
    // IQ imbalance correction of one block. Runs only when enabled (IQenable && !LIF); the
    // reset happens regardless. Returns false when not enabled.
    bool run(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool reset, bool enabled);

    int mode[2] = {0, 0};          // 0: no scaling, 1: scale the first input by coef (Q15), 2: the second
    int coef[2] = {32767, 32767};
    int acc[2] = {kIqBalanced, kIqBalanced};
    float sumA[2] = {0.f, 0.f};
    float sumB[2] = {0.f, 0.f};
    std::uint32_t period = 0;      // 0: update every block
    std::uint32_t counter = 0;
    int step = kIqAccStep;         // accumulator step per update, >= 0
    int deadBand = kIqDeadBand;

private:
    void update(int p);
};

} // namespace dsp
} // namespace sdrplay
