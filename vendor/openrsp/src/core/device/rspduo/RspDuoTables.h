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

// RSPduo (hwVer 3) calibration and mapping tables.
// Pure data.
//
//   LNA table        {attDb, lnaGR, mixGR} x 10 states x 5 rows
//                    rows 0-3 identical to the RSP1B's; row 4 = tuner-1 HiZ port
//   gainVals set A   64 x {freqMHz, G[LNA0..9]} float32, 13 groups
//   gainVals set B   55 x {freqMHz, G[LNA0..9]} float32, 11 groups
//   band RF ranges   6 x {loMHz, hiMHz} double (programRf range check)
//   DAB notch        identical to the RSP1B table: rsp1b::kDabNotchTable
#pragma once

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rspduo {

struct LnaEntry {
    std::uint8_t attDb;  // front-end attenuator (expander gain bits / pad)
    std::uint8_t lnaGR;  // tuner LNA gain reduction
    std::uint8_t mixGR;  // tuner mixer gain reduction (r1 bit 8)
};

constexpr int kLnaRows = 5;
constexpr int kLnaStates = 10;
// row 0: < 60 MHz (band 0, no 50-60 MHz override on the RSPduo); 1: bands 1-3;
// 2: band 4; 3: band 5; 4: tuner-1 HiZ (AM port 1) in band 0
extern const LnaEntry kLnaTable[kLnaRows][kLnaStates];
constexpr int kLnaRowHiZ = 4;

// gainVals: flat row-major tables, 11 floats per row {freqMHz, G0..G9}.
// Gain-value table: total gain (dB) per row frequency and LNA state, derived from measurements.
// Every value the API can report is easily derived again from the gain values it returns: with
// the AGC off, at a row's own frequency gainVals.curr = G[LNAstate] + corr - gRdB exactly
// (corr = -3.5 dB at zero IF and the default bandwidth), so stepping LNAstate there reads the
// row. Between rows the values are straight lines, so sweeping rfHz shows the row frequencies
// (where the slope changes) and the group edges (where the value jumps).
// tools/derive_gv (ctest derive_gv_check) does this and reproduces all 1082 values the API
// reports for these tables (the other cells are LNA states it refuses at those frequencies).
constexpr int kGvARows = 64;
constexpr int kGvAGroups = 13; // 11 band groups + HiZ <2 MHz + HiZ 2-30 MHz
constexpr int kGvBRows = 55;
constexpr int kGvBGroups = 11;
extern const float kGvATable[kGvARows * 11];
extern const std::uint8_t kGvAGroupCount[kGvAGroups]; // 7 5 3 3 4 4 4 4 4 8 9 3 6
extern const float kGvBTable[kGvBRows * 11];
extern const std::uint8_t kGvBGroupCount[kGvBGroups]; // 7 5 3 3 4 4 4 4 4 8 9

// programRf band range check: band b allows loMHz*1e6 <= rfHz <= hiMHz*1e6.
struct BandRange {
    double loMHz;
    double hiMHz;
};
extern const BandRange kBandRange[6];

// Band LNA gain reduction term (selectBand) per band.
extern const int kBandLnaGr[6];

} // namespace rspduo
} // namespace device
} // namespace sdrplay
