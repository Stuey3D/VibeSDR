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

// RSPdx (hwVer 4) / RSPdx-R2 (hwVer 7) calibration and mapping tables.
// Pure data, no OS headers; the definitions are in RspdxTables.cpp.
//
//   DX LNA table    8 rows x 28 states {attDb, lnaGR, mixGR}
//   HDR LNA table   22 states
//   gainVals DX     73 rows x 29 float32, counts per group
//   gainVals R2     76 rows x 29 float32, counts per group
//   bw/IF corr      {bwType, lowIF, zeroIF} x 5 (same as RSP1B)
//   DAB notch (DX)  67 doubles, 0-330 MHz in 5 MHz steps
//   antenna C       {slope, offset}
//   HDR DSP         decimate-by-4 FIRs and NCO constants, decimate-by-3 FIRs and NCO tables
#pragma once

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rspdx {

struct LnaEntry {
    std::uint8_t attDb; // front-end step attenuator + pad (expander XB[5:2], XA bit 5)
    std::uint8_t lnaGR; // tuner LNA gain reduction (r1 bits 7:6 or 9); 24 / 7 / 5
    std::uint8_t mixGR; // always 0 on the RSPdx
};

constexpr int kLnaRows = 8;
constexpr int kLnaStates = 28;
// row (band state): 0: f<12 MHz, 1: 12-50, 2: 50-60, 3: bands 1/2, 4: 250-300,
// 5: 300-420, 6: band 4, 7: band 5
extern const LnaEntry kLnaTable[kLnaRows][kLnaStates];

constexpr int kHdrLnaStates = 22;
extern const LnaEntry kHdrLnaTable[kHdrLnaStates];

// Gain-value table: total gain (dB) per row frequency and LNA state, derived from measurements.
// Every value the API can report is easily derived again from the gain values it returns: with
// the AGC off, at a row's own frequency gainVals.curr = G[LNAstate] + corr - gRdB exactly
// (corr = -3.5 dB at zero IF and the default bandwidth), so stepping LNAstate there reads the
// row. Between rows the values are straight lines, so sweeping rfHz shows the row frequencies
// (where the slope changes) and the group edges (where the value jumps).
// tools/derive_gv does this on an RSPduo, where it reproduces every reported value; the same
// sweep applies to this table.
constexpr int kGvGroups = 14;   // 13 non-HDR groups + 1 HDR group (13)
constexpr int kGvRowFloats = 29; // {freqMHz, G[LNA 0..27]}
constexpr int kGvDxRows = 73;
constexpr int kGvR2Rows = 76;
extern const float kGvDxTable[kGvDxRows * kGvRowFloats];
extern const std::uint8_t kGvDxCounts[kGvGroups]; // 6,7,7,3,3,2,4,4,4,5,4,8,9,7
extern const float kGvR2Table[kGvR2Rows * kGvRowFloats];
extern const std::uint8_t kGvR2Counts[kGvGroups]; // 6,7,7,3,3,2,4,4,4,5,4,8,9,10

struct BwCorr {
    int bwType;   // 0: every bwType without its own row
    float lowIf;  // ifType 450 / 1620 / 2048
    float zeroIf; // any other ifType
};
constexpr int kBwCorrRows = 5;
extern const BwCorr kBwCorr[kBwCorrRows]; // the last row is the bwType 0 default

constexpr int kDabTableSize = 67;
extern const double kDabNotchDx[kDabTableSize];
extern const double kAntennaC[2]; // antenna C correction (dB): f * kAntennaC[0] + kAntennaC[1]

// HDR digital down-converters (RspdxHdrDsp): symmetric Q15 low-pass FIRs, half of each stored
extern const std::int32_t kHdrDecim4Stage1[12];    // 23 taps, 8 MHz -> 4 MHz
extern const std::int32_t kHdrDecim4Stage2[4][47]; // 94 taps by hdrBw, 4 MHz -> 2 MHz
extern const std::int32_t kHdrDecim3Fir[4][70];    // 140 taps by hdrBw, 6 MHz -> 2 MHz

struct HdrDecim3Nco {
    std::int32_t frfHz;   // exact Frf selecting the table; -1 = default (875 kHz)
    std::int32_t qStart;  // initial Q index
    std::int32_t size;    // table length (both indices wrap at it)
    const std::int32_t *table;
};
constexpr int kHdrDecim3NcoCount = 10;
extern const HdrDecim3Nco kHdrDecim3Nco[kHdrDecim3NcoCount]; // the last entry is the default

// Decimate-by-4 NCO: phase step 2 pi Frf / fs, cos/sin scaled to Q15
constexpr double kHdrDecim4TwoPi = 2.0 * 3.141592653589793;
constexpr double kHdrDecim4NcoScale = 32767.0;
constexpr double kHdrDecim4NcoFs = 8e6; // Hz

} // namespace rspdx
} // namespace device
} // namespace sdrplay
