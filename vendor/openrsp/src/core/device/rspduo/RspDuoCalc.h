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

// Pure RSPduo register arithmetic (no state, no I/O). The bridge (MSi2500-like)
// and the two MSi001-like tuners use the same formulas as the RSP1B, so the
// RSP1B calc (core/device/rsp1b/Rsp1bCalc.h) is reused wherever the arithmetic
// is model independent; this file holds only what differs on the RSPduo. Each
// function is listed with the protocol step that uses it.
//
//   fsState        rate state              zero-IF table when mode != Single
//   mixFreq/lif    configureDownConverter  master/slave LIF forced
//   bandCode       programBand             T1Band/T2Band expander codes
//   reg0Word       programBand             HiZ base 0x40610
//   tunerPll       programRf               INT/FRAC errors told apart
//   gainWord       programGain             RSPduo rows / HiZ bits / total GR
//   gainValues     refreshGainVals         tuner A/B tables, HiZ groups
#pragma once

#include "core/api/ApiTypes.h"
#include "core/device/rsp1b/Rsp1bCalc.h"

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rspduo {

using rsp1b::BandState;
using rsp1b::DownConv;
using rsp1b::FsState;
using rsp1b::GainVals;

// Every `mode` below is an sdrplay_api_RspDuoModeT value.

// Master and slave force the low-IF down-converter (configureDownConverter).
constexpr bool lifForced(int mode)
{
    return mode == sdrplay_api_RspDuoMode_Master || mode == sdrplay_api_RspDuoMode_Slave;
}

// Rate state: the zero-IF spp/alt table is used when mode != Single
// (whatever the ifType), otherwise as the RSP1B (low-IF -> 504 real). The spp-504
// register-7 rule of the rate hardware update (0xC94 when mode != Single) is the
// zero-IF table's own value, so rsp1b::fsState gives every field.
int fsState(double fsHz, int ifType, int mode, bool bulk, FsState &out);

// Down-converter mix frequency (used by programRf) and the tuner IF
// the r0 IF bits come from. Master/slave: 8.192 MHz mode {2.048, 2.048} MHz,
// fs 6 MHz {1.58, 1.62} MHz, otherwise {2.0, 2.048} MHz; Single/Dual: RSP1B rules.
double mixFreq(double fsHz, int bwType, int ifType, int mode, bool fs8192);
// ifType value whose r0 IF bits are written:
// the channel ifType, except master/slave where the forced LIF decides.
int r0IfType(double fsHz, int ifType, int mode, bool fs8192);
DownConv downConverter(double fsHz, int bwType, int ifType, int mode, bool fs8192);

// LNA table row: band 0 -> 0, 1-3 -> 1, 4 -> 2, 5 -> 3.
int lnaRowOfBand(int band);
// programGain row: the HiZ row 4 when band 0, amPortSel == 1 and tuner == 1.
int lnaRow(const BandState &st, int tuner, int amPortSel);
// programGain HiZ r1 encoding: band 0 or 3, tuner == 1 and amPortSel == 1.
bool hizGainPath(const BandState &st, int tuner, int amPortSel);
// gain values HiZ: same condition (bands 0/3, T == 1, amPortSel == 1).
inline bool hizGainVals(const BandState &st, int tuner, int amPortSel) { return hizGainPath(st, tuner, amPortSel); }

struct GainWord {
    std::uint32_t g = 0;   // r1 bits 9:0 before dcCal/speedUp
    int lnaAcc = 0;        // HiZ lnaGR, or 24 for an active band-0/3 LNA
    int bandLnaOn = 0;     // bands 1, 2, 4, 5
    int mixOn = 0;         // mixer gain reduction on
    int attDb = 0;         // table attDb
    int total = 0;         // total GR
};
GainWord gainWord(const BandState &st, int tuner, int amPortSel, int gApplied, int lnaState);

// Expander 3-bit attenuator code for an attDb: 0/20 -> 7, 6/26 -> 6,
// 12/32 -> 5, 18/38 -> 3; any other value -> 0 (bits left clear).
std::uint8_t attStep(int attDb);
// Pad (expander pad bit): attDb in {20, 26, 32, 38}.
bool attPad(int attDb);
// HiZ r1 LNA bits for a HiZ-row lnaGR: 6 -> 0x40, 12 -> 0x80, 18 -> 0xC0, any other value -> 0.
std::uint32_t hizLnaBits(int lnaGR);

// programGain validation (API call). true = ok.
bool grValid(int gRdB, int minGr, int lnaState, int band, int tuner, int amPortSel);
// Update Tuner_Gr with AGC on: no T == 3 HiZ rule.
bool agcLnaValid(int lnaState, int band, int tuner, int amPortSel);

// Expander band code (5 bits): D1A 4:0 for tuner 1, D1B 5:1 (<< 1) for tuner 2.
std::uint8_t bandCode(const BandState &st);
// r0: base 0x40610 (HiZ) when hiz, else the RSP1B base; IF and BW bits as the RSP1B.
std::uint32_t reg0Word(const BandState &st, int bwType, int ifTypeForR0, bool loCodePath, bool hiz);

using TunerPll = rsp1b::TunerPll; // r2/r3 and INT/FRAC/AFC (fSynthHz and fail unused)
enum PllResult : int { kPllOk = rsp1b::kPllOk, kPllIntOutOfRange = rsp1b::kPllIntOutOfRange,
                       kPllFracOutOfRange = rsp1b::kPllFracOutOfRange };
// programRf PLL. fSynthHz = k*rf + mix + lif1 is always computed (stored
// before the INT/FRAC checks). Returns a PllResult.
int tunerPll(double rfHz, double ppm, double mixHz, const BandState &st, double &fSynthHz, TunerPll &out);
// programRf range checks: HiZ 0 <= rf <= 60e6; otherwise the band table.
bool rfInHizRange(double rfHz);
bool rfInBandRange(double rfHz, int band);

// Gain values (sdrplay_api_GainValuesT). tuner 2 uses the B set, 1 and 3 the A set.
// Returns 0, or 1 when hiz and (tuner != 1 or f >= 30 MHz): values untouched.
int gainValues(float freqMHz, int tuner, bool hiz, int bwType, int ifType, bool downConv, int minGr, int gApplied,
               int lnaState, bool dabNotchOn, GainVals &out);
// gain-values group for f (non-HiZ); HiZ: 11 below 2 MHz, else 12.
int gainGroup(float freqMHz);

} // namespace rspduo
} // namespace device
} // namespace sdrplay
