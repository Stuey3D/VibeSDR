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

// RSP2 / RSP2pro (hwVer 2) calibration and mapping tables.
// Pure data, no OS headers.
//
//   LNA table      {attDb, lnaGR, mixGR} x 9 states x 5 rows
//   gainVals rows  63 x {freqMHz, G[LNA0..8]} float32, with per-group row counts
#pragma once

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rsp2 {

struct LnaEntry {
    std::uint8_t attDb;  // front-end step attenuator (GPIO B 5:2)
    std::uint8_t lnaGR;  // tuner LNA gain reduction (r1 bits 7:6 or 9)
    std::uint8_t mixGR;  // tuner mixer gain reduction (r1 bit 8)
};

constexpr int kLnaRows = 5;
constexpr int kLnaStates = 9;
// row 0: bands 0-2; 1: band 3; 2: band 4; 3: band 5; 4: band 0 with amPortSel 1 (Hi-Z)
extern const LnaEntry kLnaTable[kLnaRows][kLnaStates];

// Gain-value table: total gain (dB) per row frequency and LNA state, derived from measurements.
// Every value the API can report is easily derived again from the gain values it returns: with
// the AGC off, at a row's own frequency gainVals.curr = G[LNAstate] + corr - gRdB exactly
// (corr = -3.5 dB at zero IF and the default bandwidth), so stepping LNAstate there reads the
// row. Between rows the values are straight lines, so sweeping rfHz shows the row frequencies
// (where the slope changes) and the group edges (where the value jumps).
// tools/derive_gv does this on an RSPduo, where it reproduces every reported value; the same
// sweep applies to this table.
constexpr int kGvRows = 63;
constexpr int kGvCols = 10; // {freqMHz, G0..G8}
constexpr int kGvGroups = 11;
// Rows grouped by frequency band (group 10: AM port); each group lists its rows by
// ascending freqMHz.
extern const float kGvTable[kGvRows][kGvCols];
extern const std::uint8_t kGvGroupCount[kGvGroups]; // 5,5,6,6,6,3,3,3,11,11,4

} // namespace rsp2
} // namespace device
} // namespace sdrplay
