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

// Pure RSP1A (hwVer 255) / RSP1 (hwVer 1) arithmetic (no state, no I/O).
//
// The bridge, ADC PLL, band state, r0 word, tuner PLL, trims and down-converter
// are model-independent, so they are the RSP1B functions (core/device/rsp1b/Rsp1bCalc.h),
// used read-only. This file holds only what differs:
//
//   lnaRow / lnaEntry / clampLna    programGain
//   setGrValid / agcLnaValid        programGain / Update (AGC on)
//   gainBits / totalGr              programGain (r1 payload, GainChange lnaGRdB)
//   attBitsA / padBitB              programGain RSP1A GPIO
//   feBandCode                      programBand
//   bridgeReg8                      writeBridgeGpio
//   gvGroup / gainValues            GainValuesT (curr/max/min)
#pragma once

#include "core/device/rsp1a/Rsp1aTables.h"
#include "core/device/rsp1b/Rsp1bCalc.h"

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rsp1a {

// Model-independent pieces shared with the RSP1B.
using rsp1b::AdcPll;
using rsp1b::BandState;
using rsp1b::DownConv;
using rsp1b::FsState;
using rsp1b::GainVals;
using rsp1b::Trims;
using rsp1b::TunerPll;

enum class Model : std::uint8_t {
    Rsp1 = 1,    // PID 0x2500, original Mirics design: no expander, band filters on bridge reg 8
    Rsp1A = 255, // PID 0x3000: MCP23017-style expander (FE12 = GPIOA, FE13 = GPIOB)
};

inline Model modelOf(std::uint8_t hwVer) { return hwVer == 1 ? Model::Rsp1 : Model::Rsp1A; }

// ---------------------------------------------------------------- band program
// GPIOA bits 4:0 band/filter code (RSP1A), from the band and the front-end filter band.
// A zeroed band state (loMode 0 at the first Init) gives 0x00.
std::uint8_t feBandCode(const BandState &st);
// Bridge reg 8 value written by writeBridgeGpio: RSP1 0xF280 | sub << 8,
// RSP1A 0xEE80.
std::uint32_t bridgeReg8(Model m, const BandState &st);

// ---------------------------------------------------------------- gain
int numLnaStates(Model m);                // 10 / 4
int lnaRow(Model m, const BandState &st); // RSP1A: 0 1 1 1 2 3; RSP1: 0 0 0 0 1 2
// programGain clamp before the table lookup (all callers): min(LNAstate, 9) / min(LNAstate, 3).
int clampLna(Model m, int lnaState);
// lnaState must be clamped.
const LnaEntry &lnaEntry(Model m, const BandState &st, int lnaState);
// programGain validation (Init/Update callers only): minGr <= gRdB <= 59 and the LNA rule.
//   RSP1A: LNAstate <= 9, not (>= 7 and band 0), not (9 and band 5)
//   RSP1:  LNAstate <= 3
bool setGrValid(Model m, const BandState &st, int gRdB, int minGr, int lnaState);
// Update with the AGC on: "LNAstate out of range" check.
//   RSP1A: LNAstate > 9 || (> 6 && band 0) || (== 9 && band > 4)
//   RSP1:  LNAstate > 3
bool agcLnaValid(Model m, const BandState &st, int lnaState);

// r1 payload bits 9:0 (gApplied | mix<<8 | LNA bits); lnaState clamped.
std::uint32_t gainBits(Model m, const BandState &st, int gApplied, int lnaState);
// Total GR: attDb + gApplied + 19*mix + LNA dB (24 on the AM path bands 0/3, else the band's LNA dB).
int totalGr(Model m, const BandState &st, int gApplied, int lnaState);

// RSP1A programGain GPIO: attBitsA goes to A 7:5, padBitB to B bit 1.
// Switch on attDb: 0/6/12/18 -> e0/c0/a0/60; 20/26/32/38 -> the same + pad;
// any other value (unreachable from the table) -> 0 and no pad.
std::uint8_t attBitsA(int attDb);
std::uint8_t padBitB(int attDb);

// ---------------------------------------------------------------- gainVals
// Group index for freqMHz (float compares against float32 constants).
//   RSP1A: <2:0 <12:1 <30:2 <60:3 <120:4 <250:5 <300:6 <380:7 <420:8 <1000:9 else 10
//   RSP1:  <12:0 <30:1 <60:2 <120:3 <250:4 <420:5 <1000:6 else 7
int gvGroup(Model m, float freqMHz);
// Gain values (sdrplay_api_GainValuesT) for these models. freqMHz = rsp1b::gainFreqMHz(...). corr as the
// RSP1B (bw/IF table, -3 dB for the down-converter); G = lo + corr + (hi-lo)*frac in float32,
// linear between the two table rows around freqMHz;
// curr = G(LNAstate) - gApplied, max = G(0) - minGr, min = G(last) - 59.
// dabNotchOn (RSP1A only: GPIOB bit 0 == 0) subtracts the RSP1B DAB-notch correction.
// lnaState is the raw state; a state beyond the model's columns reads 0 for G(LNAstate).
GainVals gainValues(Model m, float freqMHz, int bwType, int ifType, bool downConv, int minGr, int gApplied,
                    int lnaState, bool dabNotchOn);
// The DAB-notch correction c (double; the caller subtracts (float)c).
double dabNotchCorrection(float freqMHz);

} // namespace rsp1a
} // namespace device
} // namespace sdrplay
