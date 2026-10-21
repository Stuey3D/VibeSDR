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

// Pure RSP2 / RSP2pro (hwVer 2) register arithmetic (no state, no I/O).
//
// The bridge (ADC PLL, packet format), the band plan, the tuner PLL, the LO
// trims and the low-IF down-converter are model-independent: they are
// reused from rsp1b::Calc unchanged. Only the RSP2 specifics live here:
//
//   lnaRow / subBand     LNA table row and RSP2 sub-band of the RF band
//   reg0Word             tuner r0 word (AM port mode byte 0x61)
//   feBandA / feAttPreset  band program (RSP2 GPIO block)
//   gainCalc / attBits / lnaStateValid  programGain
//   gainValues           current gain values
//
// The *meaning* of the expander bits (antenna A/B, Hi-Z port, notch/bias-T/ext-ref
// polarity, filter bank, attenuator dB) has not yet been confirmed
// on a real unit.
#pragma once

#include "core/device/rsp1b/Rsp1bCalc.h"

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rsp2 {

using rsp1b::BandState;

// RSP2 LNA table row: bands 0-2 -> 0, band 3 -> 1, band 4 -> 2, band 5 -> 3.
int lnaRow(const BandState &st);
// RSP2 sub-band: 1..3 (band 0: <12, 12-30, 30-60), 4, 5, 6..8 (band 3), 9, 10.
int subBand(const BandState &st);

// Tuner r0 word: rsp1b::reg0Word with the band-0 base 0x40E10 replaced by 0x40610
// when amPortSel == 1 (r0 mode byte 0xE1 -> 0x61; band 3 keeps 0x40E10).
std::uint32_t reg0Word(const BandState &st, int bwType, int ifType, bool loCodePath, int amPortSel);

// Band program (RSP2 front-end block): GPIO A bits 5:3 (filter bank)
// and the step-attenuator preset in B bits 5:2.
std::uint8_t feBandA(const BandState &st);
std::uint8_t feAttPreset(const BandState &st);

// GPIO B 5:2 step-attenuator bits for an LNA-table attDb (0 for values outside the map).
std::uint8_t attBits(int attDb);

// r1 AM-port LNA bits for an LNA-table lnaGR: 6 -> 0x40, 12 -> 0x80, 18 -> 0xC0, else 0.
std::uint32_t amLnaBits(int lnaGR);

// programGain / Update(AGC) LNA-state rule (gRdB and minGr are checked separately):
// LNAstate <= 8; not >= 5 in band 0 with the AM port; not > 5 in bands 4/5.
bool lnaStateValid(int lnaState, int band, int amPortSel);

struct GainCalc {
    int row = 0;            // LNA table row actually used (4 = AM port)
    int attDb = 0;          // table attDb
    int attReported = 0;    // attDb as counted in lnaGRdB (L-band remap)
    int lnaGR = 0;          // table lnaGR
    bool mixF = false;      // mixer gain reduction on (r1 bit 8)
    bool lnaFlag = false;   // LNA gain reduction on (bands 1/2/4/5)
    int amGR = 0;           // AM-port LNA gain reduction (bands 0/3)
    std::uint32_t g = 0;    // r1 payload bits 9:0
    int total = 0;          // total gain reduction (lnaGRdB = total - gApplied)
};
// lnaState must already be clamped to 0..8.
GainCalc gainCalc(const BandState &st, int amPortSel, int gApplied, int lnaState);

// Gain-values group (0..10): group 10 when (band 0 or 3) and amPortSel == 1.
int gainGroup(float freqMHz, bool amPortGroup);

using rsp1b::GainVals;
// f: rsp1b::gainFreqMHz(fSynth, mix, lif1). downConv: low-IF down-converter
// active (-3 dB). No DAB-notch correction on the RSP2. lnaState is the raw state;
// a state above 8 reads 0 for G(LNAstate).
GainVals gainValues(float freqMHz, int bwType, int ifType, bool downConv, int minGr, int gApplied, int lnaState,
                    bool amPortGroup);

} // namespace rsp2
} // namespace device
} // namespace sdrplay
