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

// RSP1B calibration and mapping tables. Pure data, no OS headers.
//
//   LNA table      {attDb, lnaGR, mixGR} x 10 states x 5 rows
//   gain values    57 x {freqMHz, G[LNA0..9]} float32, with per-group row counts
//   DAB notch      67 doubles, one per 5 MHz from 0 to 330 MHz
#pragma once

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rsp1b {

struct LnaEntry {
    std::uint8_t attDb;  // front-end attenuator (GPIO)
    std::uint8_t lnaGR;  // tuner LNA gain reduction (r1 bits 7:6 or 9)
    std::uint8_t mixGR;  // tuner mixer gain reduction (r1 bit 8)
};

constexpr int kLnaRows = 5;
constexpr int kLnaStates = 10;
// row 0: band 0 f < 50 MHz; 1: bands 1-3; 2: band 4; 3: band 5; 4: band 0 50-60 MHz
extern const LnaEntry kLnaTable[kLnaRows][kLnaStates];

// Gain-value table: total gain (dB) per row frequency and LNA state, derived from measurements.
// Every value the API can report is easily derived again from the gain values it returns: with
// the AGC off, at a row's own frequency gainVals.curr = G[LNAstate] + corr - gRdB exactly
// (corr = -3.5 dB at zero IF and the default bandwidth), so stepping LNAstate there reads the
// row. Between rows the values are straight lines, so sweeping rfHz shows the row frequencies
// (where the slope changes) and the group edges (where the value jumps).
// tools/derive_gv does this on an RSPduo, where it reproduces every reported value; the same
// sweep applies to this table.
// The 1825 MHz row's G8 and G9 are G7 - 19.2 (see the row).
constexpr int kGvRows = 57;
constexpr int kGvCols = 11; // {freqMHz, G0..G9}
constexpr int kGvGroups = 12;
// Rows grouped by frequency band, in frequency order; each group lists its rows by
// ascending freqMHz.
extern const float kGvTable[kGvRows][kGvCols];
extern const std::uint8_t kGvGroupCount[kGvGroups]; // 8,4,3,3,3,4,3,4,4,4,8,9

// DAB-notch correction at 0, 5, ..., 330 MHz, derived from measurements of the notch filter's loss.
constexpr int kDabTableSize = 67;
extern const double kDabNotchTable[kDabTableSize];

} // namespace rsp1b
} // namespace device
} // namespace sdrplay
