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

// Pure RSP1B register arithmetic (no state, no I/O). Each function computes one
// step of the hardware programming.
//
//   fsState        sample-rate dependent state
//   adcPll         ADC PLL registers
//   computeBand    RF band selection
//   mixFreq        down-converter mix frequency
//   reg0Word       tuner register 0
//   tunerPll       tuner synthesiser registers (synthFreqHz, synthWords)
//   readbackTrims / bypassTrim  tuner trim readback and bypass
//   gainValues     sdrplay_api_GainValuesT
#pragma once

#include <cstdint>

#include "core/dsp/DownConverter.h"

namespace sdrplay {
namespace device {
namespace rsp1b {

// ---------------------------------------------------------------- bridge / ADC
// Bridge reg 7 (packet format) fields. Bits 5:0 select the sample coding, bit 7 marks the
// alt-1 formats, whose packets the bridge hands over in 3-packet microframe bursts, and
// bits 11:10 select the 504-sample formats. The bridge firmware looks only at bits 5:4 (a value
// of 2 makes it fill byte 1023 of every packet buffer with 0xFA); the bridge interprets the rest.
constexpr std::uint32_t kPktFmtCodingMask = 0x03F;
constexpr std::uint32_t kPktFmtWord16 = 0x014;      // 14-bit samples in 16-bit words (252 I/Q, 504 real)
constexpr std::uint32_t kPktFmtPacked12 = 0x005;    // 12-bit samples as a packed bit stream (336 I/Q)
constexpr std::uint32_t kPktFmtPacked10Exp = 0x025; // 10-bit samples plus 2-bit exponent codes (384 I/Q)
constexpr std::uint32_t kPktFmtBurst3 = 0x080;      // 3 packets per microframe (set exactly on alt 1)
constexpr std::uint32_t kPktFmtSpp504 = 0x400;      // 504 samples per packet (16-bit real without kPktFmtIq8)
constexpr std::uint32_t kPktFmtIq8 = 0x800;         // with kPktFmtSpp504: 504 x {int8 I, int8 Q}

struct FsState {
    double fsMHz = 0.0;       // fsHz / 1e6
    std::uint8_t alt = 0;     // USB alt setting (3 = bulk)
    std::uint32_t reg7 = 0;   // bridge reg 7 packet format
    int spp = 0;              // samples per packet
    int D = 0;                // ADC divider
    int V = 0;                // VCO band
    std::uint32_t wrapThr = 0;// sync-update wrap threshold (2^32 - samples in 30 ms)
};
// The functions that return an int return sdrplay_api_Success or sdrplay_api_OutOfRange.

// OutOfRange: fsMHz above every packet format. lowIf = (ifType != 0); bulk = devParams.mode == BULK.
int fsState(double fsHz, bool lowIf, bool bulk, FsState &out);

struct AdcPll {
    std::uint32_t reg4 = 0;   // 20-bit FRAC
    std::uint32_t reg3 = 0;   // V<<14 | reCal<<12 | INT<<7 | ((D-2)/2)<<2 | 3
};
// OutOfRange: fsHz outside [2e6, 10e6] or INT outside 1..31.
int adcPll(double fsHz, double ppm, int D, int V, std::uint8_t reCal, AdcPll &out);

// ---------------------------------------------------------------- tuner
struct BandState {
    int band = 0;          // 0..5
    int sub = 0;           // selectBand "sub" (part of the same-band key)
    // Sub-band indices of the per-model front ends (all computed for every model).
    int rsp1aSub = 0;      // RSP1A front-end filter band
    int rsp2Sub = 0;       // RSP2 sub-band
    int duoSub = 0;        // RSPduo front-end band
    int dxSub = 0;         // RSPdx sub-band 1..13
    int rsp1bSub = 0;      // RSP1B LNA sub-band
    int firstLO = 0;       // Hz
    double lif1Hz = 0.0;   // +firstLO (band 0), -firstLO (band 3), else 0
    int loDiv = 0;
    int lnaRow = 0;        // LNA table row
    int feSub = 0;         // front-end sub-band s (1..5 band 0, 8..10 band 3, else 0)
    bool loFlag = false;   // band in {0,3} and firstLO != 120 MHz
};
// Upper end of band 5 and of the tuner range, MHz: 2 GHz inclusive (1 Hz margin).
constexpr double kRfMaxMHz = 2000.000001;

// loMode: 1 Auto (others keep firstLO as set by applyLoMode). OutOfRange: rf outside every band.
int computeBand(double rfHz, int loMode, int firstLO, BandState &out);
bool sameBand(const BandState &a, const BandState &b);

// applyLoMode: firstLO for the forced modes; OutOfRange for an invalid mode.
int loModeFirstLo(int loMode, int &firstLO);

double mixFreq(double fsHz, int bwType, int ifType);

std::uint32_t reg0Word(const BandState &st, int bwType, int ifType, bool loCodePath);

// TunerPll::fail
enum : int { kPllOk = 0, kPllIntOutOfRange = 1, kPllFracOutOfRange = 2 };

struct TunerPll {
    std::uint32_t r3 = 0, r2 = 0;
    int INT = 0, FRAC = 0, AFC = 0;
    double fSynthHz = 0.0; // k*rf + mix + lif1 (used for gainVals);
                           // set even when the INT/FRAC checks fail
    int fail = kPllOk;     // kPllOk, kPllIntOutOfRange or kPllFracOutOfRange
};
// Synthesiser scale k = 1 - ppm * 1e-6 of tunerPll; ppm 0 uses a scale of 1 - 1e-9
// (kept for synthesiser-word compatibility).
constexpr double kScaleAtZeroPpm = 0.999999999;
// Synthesiser frequency k*rf + mix + lif1 (k as above).
double synthFreqHz(double rfHz, double ppm, double mixHz, const BandState &st);
// INT/FRAC/AFC and the r2/r3 words for a VCO frequency; returns kPllOk, kPllIntOutOfRange or
// kPllFracOutOfRange and leaves out unchanged on a failure.
int synthWords(double vcoHz, TunerPll &out);
// OutOfRange: INT or FRAC out of range (nothing must be written).
int tunerPll(double rfHz, double ppm, double mixHz, const BandState &st, TunerPll &out);

struct Trims {
    int t[6] = {0, 0, 0, 0, 0, 0};
};
Trims readbackTrims(std::uint32_t v);
void bypassTrim(const Trims &t, int en0, std::uint32_t &r13, std::uint32_t &r14);

// ---------------------------------------------------------------- gain / front end
// programGain word payload g (r1 bits 9:0 before dcCal/speedUp): gApplied | mix<<8 | LNA bits.
std::uint32_t gainBits(const BandState &st, int gApplied, int lnaState);
// lnaGRdB reported in GainChange (total GR - IF GR) for a valid LNA state.
int totalGr(const BandState &st, int gApplied, int lnaState);
// ATT_B bits (B 4:2) for attDb mod 20
std::uint8_t attBits(int attDb);
// band-program GPIO bits: A bit 7 / B 7:6|1:0
std::uint8_t feBandA(const BandState &st);
std::uint8_t feBandB(const BandState &st);

// Low-IF down-converter configuration (configureDownConverter, non-RSPduo).
// Output sample counts are (n >> countShift) / countDivisor; device sample counts are the
// output counts scaled back the same way.
struct DownConv {
    bool enabled = false;
    dsp::DownConvPath path = dsp::DownConvPath::kNone;
    int countShift = 0;
    int countDivisor = 1;
    bool adsbCfg = false;
};
DownConv downConverter(double fsHz, int bwType, int ifType);

struct GainVals {
    float curr = 0.0f, max = 0.0f, min = 0.0f;
};
// freqMHz = (float)(((fSynth - mixHz) - lif1Hz) / 1e6), i.e. the ppm-corrected RF the
// synthesiser was last programmed for (0 before the first programRf).
// downConv: low-IF down-converter active (-3 dB). dabNotchOn = GPIO A bit 6 == 0.
// lnaState is the raw state; a state above 9 reads 0 for G(LNAstate).
GainVals gainValues(float freqMHz, int bwType, int ifType, bool downConv, int minGr, int gApplied, int lnaState,
                    bool dabNotchOn);
float gainFreqMHz(double fSynthHz, double mixHz, double lif1Hz);
// DAB-notch correction (dB) at f MHz: the 5 MHz-step table (67 points, kDabNotchTable or a
// model's own) up to 330 MHz, 0 below it; then linear from 1.7 dB at 330 MHz to 0.4 dB at
// 600 MHz and on to 0 at 2000 MHz.
double dabNotchCorrection(float f, const double *table);

} // namespace rsp1b
} // namespace device
} // namespace sdrplay
