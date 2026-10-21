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

// Pure RSPdx / RSPdx-R2 arithmetic (no state, no I/O) for what differs from the
// RSP1B.
//
//   computeBand       selectBand (+ DX LNA table row, hdrActive)
//   hdrActive         HDR mode test (enabled and below 2 MHz)
//   setGrValid        programGain DX validation
//   agcLnaValid       Update DX LNA check (AGC on)
//   attBits           programGain DX attenuator tables
//   hdrAttBits        programGain in HDR
//   feBandA/B         programBand front-end bits
//   gainValues        gain values
//   hdrDownConverter  configureDownConverter in HDR
//
// The model-independent tuner / bridge arithmetic (fsState, adcPll, reg0Word,
// tunerPll, trims, mixFreq, the low-IF down-converter) is the RSP1B one and is
// reused from core/device/rsp1b/Rsp1bCalc.h unchanged.
#pragma once

#include "core/device/rsp1b/Rsp1bCalc.h"

#include <cstdint>

namespace sdrplay {
namespace device {
namespace rspdx {

// ---------------------------------------------------------------- band state
struct BandState {
    rsp1b::BandState b;   // the model-independent part (band, sub-bands incl. dxSub 1..13, firstLO, lif1, loDiv)
    int row = 0;          // DX LNA table row 0..7
    int lnaBandGR = 0;    // 24 (bands 0-3), 7 (band 4), 5 (band 5)
    bool hdrActive = false;
};

// D.hdrEnable == 1 && rfHz / 1e6 < 2.0
bool hdrActive(std::uint8_t hdrEnable, double rfHz);
// selectBand. Returns sdrplay_api_Success, or sdrplay_api_OutOfRange (rf out of range; `out` untouched).
int computeBand(double rfHz, int loMode, int firstLO, std::uint8_t hdrEnable, BandState &out);
// Update(Tuner_Frf) "same band" test: the RSP1B tuple (band, sub, firstLO, every sub-band) and hdrActive.
bool sameBand(const BandState &a, const BandState &b);
int lnaRow(int band, int dxSub);
int lnaBandGR(int band);

// ---------------------------------------------------------------- gain validation
// Largest LNAstate accepted in this sub-band.
int maxLnaState(int band, int dxSub);
// The LNA region rule shared by programGain (non-HDR) and Update (AGC on): LNA < 28 and
// the per-band limits. The gRdB range is checked separately by setGrValid.
bool lnaValid(int band, int dxSub, int lna);
bool setGrValid(int gRdB, int minGr, int band, int dxSub, int lna);
constexpr int kHdrMaxLna = 21; // HDR: LNAstate > 21 -> sdrplay_api_OutOfRange (the message reports 22 states)

// ---------------------------------------------------------------- front end (MCP23017-like expander)
// enc(x): XB[5:2] step-attenuator code (active low: 3/6/12/15 dB on bits 2..5). -1 if x is not a step.
int attCode(int x);
struct FeAtt {
    std::uint8_t pad = 0;  // XA bit 5 (24 dB pad; 15 dB on the 50-60 MHz path)
    std::uint8_t code = 0; // XB[5:2]
};
// Non-HDR programGain: band != 0 / band 0 below 50 MHz / band 0 at 50-60 MHz (dxSub == 6).
FeAtt attBits(int band, int dxSub, int attDb);
struct HdrAtt {
    std::uint8_t a0 = 0;   // XA bit 0 (HDR extra attenuator off for attDb <= 24)
    std::uint8_t code = 0; // XB[5:2]
};
HdrAtt hdrAttBits(int attDb);
// Band program: XA bit 7, and XB[7:6] | XB[1:0].
std::uint8_t feBandA(const BandState &st);
std::uint8_t feBandB(const BandState &st);

struct LnaEntryView {
    int attDb = 0, lnaGR = 0, mixGR = 0;
};
// r1 payload (before dcCal/speedUp): gApplied | mixGR<<8 | lnaGR bits (0xC0 for bands 0/3, bit 9 otherwise).
std::uint32_t gainBits(int band, int gApplied, const LnaEntryView &e);
LnaEntryView lnaEntry(int row, int lnaState);     // kLnaTable[row][min(lna,27)]
LnaEntryView hdrLnaEntry(int lnaState);           // kHdrLnaTable[min(lna,21)]
// programGain total: lnaBandGR*(lnaGR!=0, bands 1,2,4,5) + mix + att + gApplied + 24*(lnaGR!=0, bands 0/3)
int totalGr(const BandState &st, int gApplied, const LnaEntryView &e);

// ---------------------------------------------------------------- HDR
// programRf in HDR: Frf = rfHz truncated toward zero (dsp::truncToI32), 875000 if > 2 MHz. Init sets 875000.
std::int32_t hdrFrf(double rfHz);
constexpr std::int32_t kHdrDefaultFrf = 875000;
// configureDownConverter in HDR: enable 1, LIF 1, the decimate-by-3 path (count / 3) at
// fs 6.0 MHz, else the decimate-by-4 path with a count shift of 4 (n/16).
rsp1b::DownConv hdrDownConverter(double fsMHz);

// ---------------------------------------------------------------- gainVals
enum class Variant { Dx, DxR2 };
// group 0..12 for the non-HDR frequency (float compares on the group edges); 13 = HDR
int gainGroup(float freqMHz);
// Correction for bwType / ifType, the RSP1B rule.
float bwIfCorr(int bwType, int ifType);
struct GainVals {
    float curr = 0.0f, max = 0.0f, min = 0.0f;
};
// Non-HDR: freqMHz = rsp1b::gainFreqMHz(fSynth, mix, lif1).
// dabNotchOn: XA bit 6 == 0. antennaC: D.antennaSel == 2.
GainVals gainValues(Variant v, float freqMHz, int bwType, int ifType, bool downConv, int minGr, int gApplied,
                    int lnaState, bool dabNotchOn, bool antennaC);
// HDR (group 13): freqMHz = (float)Frf / 1e6f, corr = -3.5 (- 3 if downConv), nothing subtracted.
GainVals hdrGainVals(Variant v, std::int32_t frf, bool downConv, int lnaState);
// DAB-notch correction c(f) in double (DX table kDabNotchDx up to 330 MHz).
double dabCorrection(float freqMHz);

} // namespace rspdx
} // namespace device
} // namespace sdrplay
