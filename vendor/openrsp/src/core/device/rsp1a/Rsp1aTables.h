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

// RSP1A (hwVer 255) and RSP1 (hwVer 1) calibration and mapping tables.
// Pure data, no OS headers; the values are in Rsp1aTables.cpp.
//
//   LNA table RSP1A  {attDb, lnaGR, mixGR} x 10 states x 4 rows   (= RSP1B rows 0..3)
//   LNA table RSP1   {attDb, lnaGR, mixGR} x 4 states x 3 rows
//   gainVals RSP1A   49 x {freqMHz, G[LNA0..9]} float32, group counts
//   gainVals RSP1    58 x {freqMHz, G[LNA0..3]} float32, group counts
//   DAB notch        rsp1b::kDabNotchTable
#pragma once

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rsp1a {

struct LnaEntry {
    std::uint8_t attDb; // front-end step attenuator (+20 dB pad), GPIO
    std::uint8_t lnaGR; // tuner LNA gain reduction flag (r1 bits 7:6 or 9)
    std::uint8_t mixGR; // tuner mixer gain reduction flag (r1 bit 8)
};

constexpr int kLnaRowsRsp1a = 4;
constexpr int kLnaStatesRsp1a = 10;
// row 0: band 0 (f < 60 MHz); 1: bands 1-3; 2: band 4; 3: band 5
extern const LnaEntry kLnaRsp1a[kLnaRowsRsp1a][kLnaStatesRsp1a];

constexpr int kLnaRowsRsp1 = 3;
constexpr int kLnaStatesRsp1 = 4;
// row 0: bands 0-3; 1: band 4; 2: band 5
extern const LnaEntry kLnaRsp1[kLnaRowsRsp1][kLnaStatesRsp1];

// gainVals rows {freqMHz, G[LNA0..N]}, N = 9 (RSP1A) or 3 (RSP1).
// Gain-value table: total gain (dB) per row frequency and LNA state, derived from measurements.
// Every value the API can report is easily derived again from the gain values it returns: with
// the AGC off, at a row's own frequency gainVals.curr = G[LNAstate] + corr - gRdB exactly
// (corr = -3.5 dB at zero IF and the default bandwidth), so stepping LNAstate there reads the
// row. Between rows the values are straight lines, so sweeping rfHz shows the row frequencies
// (where the slope changes) and the group edges (where the value jumps).
// tools/derive_gv does this on an RSPduo, where it reproduces every reported value; the same
// sweep applies to this table.
constexpr int kGvRowsRsp1a = 49;
constexpr int kGvWidthRsp1a = 11;
constexpr int kGvGroupsRsp1a = 11;
extern const float kGvRsp1a[kGvRowsRsp1a][kGvWidthRsp1a];
extern const std::uint8_t kGvRsp1aCount[kGvGroupsRsp1a]; // 4,3,3,3,4,3,4,4,4,8,9

constexpr int kGvRowsRsp1 = 58;
constexpr int kGvWidthRsp1 = 5;
constexpr int kGvGroupsRsp1 = 8;
extern const float kGvRsp1[kGvRowsRsp1][kGvWidthRsp1];
extern const std::uint8_t kGvRsp1Count[kGvGroupsRsp1]; // 5,5,6,6,6,8,11,11

} // namespace rsp1a
} // namespace device
} // namespace sdrplay
