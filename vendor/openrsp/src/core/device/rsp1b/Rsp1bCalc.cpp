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

#include "core/device/rsp1b/Rsp1bCalc.h"

#include "core/api/ApiTypes.h"
#include "core/device/rsp1b/Rsp1bTables.h"
#include "core/dsp/DspCommon.h"

#include <iterator>
#include <limits>

namespace sdrplay {
namespace device {
namespace rsp1b {

namespace {
// Every double -> integer conversion that can see a user-controlled value uses the
// defined-result helpers of DspCommon.h (truncation toward zero; NaN or out of range
// gives INT32_MIN / INT64_MIN).
using dsp::truncToI32;
using dsp::truncToU32;

// double -> float with IEEE overflow to +-inf (a C++ cast of a finite value outside
// the float range is UB). Values at or beyond FLT_MAX + half an ulp (2^103) round to inf.
float toFloatIeee(double v)
{
    constexpr double kInfEdge = 340282356779733661637539395458142568448.0; // 2^128 - 2^103
    if (v >= kInfEdge) return std::numeric_limits<float>::infinity();
    if (v <= -kInfEdge) return -std::numeric_limits<float>::infinity();
    return static_cast<float>(v);
}
// The sample rate rounded to whole Hz.
std::int32_t roundedFsHz(double fsHz) { return truncToI32(fsHz + 0.5); }

// ADC PLL: the VCO runs at 12 * D * fs from the 24 MHz reference; FRAC has 20 bits.
constexpr double kAdcRefHz = 24000000.0;
// Tuner synthesiser: INT counts 96 MHz steps of the VCO, FRAC 1/3000 of a step (32 kHz) and
// AFC 1/4096 of a FRAC step (7.8125 Hz).
constexpr double kPllRefHz = 96000000.0;
constexpr double kPllFracStepHz = kPllRefHz / 3000.0;
constexpr double kPllAfcStepHz = kPllFracStepHz / 4096.0;

// Highest sample rate (MHz) of a packet format: samples per packet times the packet rate of
// its USB alt setting (23976 packets/s on alt 1, 7992 on alt 2).
constexpr double kPacketRateAlt1 = 23976.0;
constexpr double kPacketRateAlt2 = 7992.0;
constexpr double maxFsMHz(int spp, double packetRate) { return spp * packetRate / 1000000.0; }

// One packet format: samples per packet, iso alt setting, reg 7 coding and size bits (the
// kPktFmtBurst3 bit follows from the alt setting), and whether fsMHz may equal the limit.
struct FsFormat {
    int spp;
    std::uint8_t alt;
    std::uint32_t fields;
    bool limitIncluded;
};
bool fits(double fMHz, const FsFormat &p)
{
    const double limit = maxFsMHz(p.spp, p.alt == 1 ? kPacketRateAlt1 : kPacketRateAlt2);
    return p.limitIncluded ? fMHz <= limit : fMHz < limit;
}
// Formats in order of increasing limit; fsState takes the first one that fits.
constexpr FsFormat kZeroIfFormats[] = {
    {252, 2, kPktFmtWord16, true},
    {336, 2, kPktFmtPacked12, true},
    {252, 1, kPktFmtWord16, true},
    {336, 1, kPktFmtPacked12, true},
    {384, 1, kPktFmtPacked10Exp, false},
    {504, 1, kPktFmtIq8 | kPktFmtSpp504 | kPktFmtWord16, true},
};
constexpr FsFormat kLowIfFormats[] = {
    {504, 2, kPktFmtSpp504 | kPktFmtWord16, true},
    {504, 1, kPktFmtSpp504 | kPktFmtWord16, true},
};
} // namespace

// ------------------------------------------------------------------ bridge
int fsState(double fsHz, bool lowIf, bool bulk, FsState &out)
{
    // Sample-rate dependent packet format and ADC dividers.
    const double f = fsHz / 1000000.0;
    const FsFormat *first = lowIf ? std::begin(kLowIfFormats) : std::begin(kZeroIfFormats);
    const FsFormat *last = lowIf ? std::end(kLowIfFormats) - 1 : std::end(kZeroIfFormats) - 1;
    if (!fits(f, *last)) return sdrplay_api_OutOfRange; // the caller reports the fsMHz value as out of range
    const FsFormat *p = first;
    while (!fits(f, *p)) ++p;
    FsState s;
    s.fsMHz = f;
    s.alt = p->alt;
    s.spp = p->spp;
    s.reg7 = p->fields | (p->alt == 1 ? kPktFmtBurst3 : 0u);
    if (bulk) s.alt = 3;
    // ADC divider D and VCO band V. These thresholds, and every packet-format limit above, are the
    // switch points measured on hardware in tools/derive_fsstate/measured.txt (derive_fsstate check).
    if (f <= 2.485) s.D = 16;
    else if (f <= 2.865) s.D = 14;
    else if (f <= 3.388) s.D = 12;
    else if (f <= 4.145) s.D = 10;
    else if (f <= 5.347) s.D = 8;
    else if (f <= 7.569) s.D = 6;
    else s.D = 4;
    if (f <= 5.8) s.V = 4;
    else if (f <= 6.9) s.V = 5;
    else if (f <= 8.1) s.V = 6;
    else s.V = 7;
    // Sync-update wrap threshold: 2^32 minus the samples of 30 ms (rounded half away from
    // zero), in 32-bit modular arithmetic.
    const double w = fsHz * 0.03;
    const int r = w > 0.0 ? truncToI32(w + 0.5) : truncToI32(w - 0.5);
    s.wrapThr = 0u - static_cast<std::uint32_t>(r);
    out = s;
    return sdrplay_api_Success;
}

int adcPll(double fsHz, double ppm, int D, int V, std::uint8_t reCal, AdcPll &out)
{
    // ADC PLL INT/FRAC for fsHz
    if (!(2000000.0 <= fsHz && fsHz <= 10000000.0)) return sdrplay_api_OutOfRange; // the caller reports the fsHz value as out of range
    const double n = 12.0 * D * fsHz / kAdcRefHz * (1.0 - ppm * 1e-6);
    const int i = truncToI32(n);
    int frac = truncToI32((n - static_cast<double>(i)) * 1048576.0 + 0.5);
    if (frac < 0) frac = 0;
    if (frac > 0xFFFFF) frac = 0xFFFFF;
    if (i < 1 || i > 31) return sdrplay_api_OutOfRange; // the caller logs an invalid-IntVal warning
    out.reg4 = static_cast<std::uint32_t>(frac);
    out.reg3 = static_cast<std::uint32_t>(V) << 14 | static_cast<std::uint32_t>(reCal) << 12 |
               static_cast<std::uint32_t>(i) << 7 | static_cast<std::uint32_t>((D - 2) / 2) << 2 | 3u;
    return sdrplay_api_Success;
}

// ------------------------------------------------------------------ tuner
int computeBand(double rfHz, int loMode, int firstLO, BandState &out)
{
    // Band selection (hwVer 6).
    const double f = rfHz / 1000000.0;
    BandState s;
    s.firstLO = firstLO;
    auto setSubBands = [&s](int rsp1a, int rsp2, int duo, int dx, int rsp1b) {
        s.rsp1aSub = rsp1a; s.rsp2Sub = rsp2; s.duoSub = duo; s.dxSub = dx; s.rsp1bSub = rsp1b;
    };
    if (0.0 <= f && f < 60.0) {
        s.band = 0;
        // band-0 sub code (part of the same-band key): 5 up to 11.999 MHz, 13 up to 29.999 MHz
        s.sub = f <= 11.999 ? 5 : (f <= 29.999 ? 13 : 0);
        if (f < 0.5) setSubBands(1, 1, 1, 1, 1);
        else if (f < 2.0) setSubBands(1, 1, 1, 2, 1);
        else if (f < 12.0) setSubBands(2, 1, 2, 3, 2);
        else if (f < 30.0) setSubBands(3, 2, 3, 4, 3);
        else if (f < 50.0) setSubBands(4, 3, 4, 5, 4);
        else setSubBands(4, 3, 4, 6, 5);
        if (loMode == 1) s.firstLO = 120000000;
        s.lif1Hz = static_cast<double>(s.firstLO);
        s.lnaRow = s.rsp1bSub > 4 ? 4 : 0;
        s.feSub = f < 2.0 ? 1 : f < 12.0 ? 2 : f < 30.0 ? 3 : f < 50.0 ? 4 : 5;
    } else if (60.0 <= f && f < 120.0) {
        s.band = 1; s.sub = 1; s.lnaRow = 1; setSubBands(5, 4, 5, 7, 6);
    } else if (120.0 <= f && f < 250.0) {
        s.band = 2; s.sub = 8; s.lnaRow = 1; setSubBands(6, 5, 6, 8, 7);
    } else if (250.0 <= f && f < 420.0) {
        s.band = 3; s.sub = 4; s.lnaRow = 1;
        if (f < 300.0) setSubBands(7, 6, 7, 9, 8);
        else if (f >= 380.0) setSubBands(9, 8, 9, 11, 10);
        else setSubBands(8, 7, 8, 10, 9);
        if (loMode == 1) s.firstLO = f < 375.0 ? 120000000 : (f < 395.0 ? 144000000 : 168000000);
        s.lif1Hz = static_cast<double>(-s.firstLO);
        s.feSub = f < 300.0 ? 8 : f < 380.0 ? 9 : 10;
    } else if (420.0 <= f && f < 1000.0) {
        s.band = 4; s.sub = 1; s.lnaRow = 2; setSubBands(10, 9, 10, 12, 11);
    } else if (1000.0 <= f && f < kRfMaxMHz) {
        s.band = 5; s.sub = 8; s.lnaRow = 3; setSubBands(11, 10, 11, 13, 12);
    } else {
        return sdrplay_api_OutOfRange; // the caller reports the tuner's rfMHz as out of range
    }
    static const int kLoDiv[6] = {16, 32, 16, 16, 4, 2};
    s.loDiv = kLoDiv[s.band];
    s.loFlag = (s.band == 0 || s.band == 3) && s.firstLO != 120000000;
    out = s;
    return sdrplay_api_Success;
}

bool sameBand(const BandState &a, const BandState &b)
{
    return a.band == b.band && a.sub == b.sub && a.firstLO == b.firstLO && a.rsp1aSub == b.rsp1aSub &&
           a.rsp2Sub == b.rsp2Sub && a.duoSub == b.duoSub && a.dxSub == b.dxSub && a.rsp1bSub == b.rsp1bSub;
}

int loModeFirstLo(int loMode, int &firstLO)
{
    // applyLoMode: first LO for the forced modes
    switch (loMode) {
    case 1: return sdrplay_api_Success;
    case 2: firstLO = 120000000; return sdrplay_api_Success;
    case 3: firstLO = 144000000; return sdrplay_api_Success;
    case 4: firstLO = 168000000; return sdrplay_api_Success;
    default: return sdrplay_api_OutOfRange; // the caller reports the tuner's loMode as invalid
    }
}

double mixFreq(double fsHz, int bwType, int ifType)
{
    // configureDownConverter (non-RSPduo): the mix frequency used by programRf.
    if (ifType == 0) return 0.0;
    double mix = 0.0;
    if (ifType == 2048) mix = 2048000.0;
    else if (ifType == 1620) mix = 1620000.0;
    else if (ifType == 450) mix = 450000.0;
    const int fsi = roundedFsHz(fsHz);
    if (fsi == 8192000 || fsi == 8000000) {
        if (bwType == 1536 && ifType == 2048) {
            if (fsi == 8000000) mix = 2000000.0;
        } else if (fsi == 8000000 && bwType == 5000 && ifType == 2048) {
            mix = 2000000.0;
        }
    } else if (fsi == 6000000 && bwType <= 1536 && ifType == 1620) {
        mix = 1580000.0;
    }
    return mix;
}

std::uint32_t reg0Word(const BandState &st, int bwType, int ifType, bool loCodePath)
{
    static const std::uint32_t kBase[6] = {0x40e10, 0x40420, 0x40440, 0x40e10, 0x40480, 0x40500};
    std::uint32_t w = kBase[st.band];
    if (ifType == 0) w |= 0x3000;
    else if (ifType == 1620) w |= 0x1000;
    else if (ifType == 450) w |= 0x2000;
    switch (bwType) {
    case 200: break;
    case 300: w |= 0x4000; break;
    case 600: w |= 0x8000; break;
    case 1536: w |= 0xC000; break;
    case 5000: w |= 0x10000; break;
    case 6000: w |= 0x14000; break;
    case 7000: w |= 0x18000; break;
    case 8000: w |= 0x1C000; break;
    default: w |= 0xC000; break;
    }
    if (loCodePath) {
        w &= 0xFFF1FFFFu;
        if (st.firstLO == 144000000) w |= 0x20000;
    }
    return w;
}

double synthFreqHz(double rfHz, double ppm, double mixHz, const BandState &st)
{
    const double k = (ppm == 0.0) ? kScaleAtZeroPpm : (1.0 - ppm * 1e-6);
    return k * rfHz + mixHz + st.lif1Hz;
}

int synthWords(double vcoHz, TunerPll &out)
{
    // INT, FRAC and AFC are truncated and taken modulo 2^32 (truncToU32); the range checks
    // apply to those 32-bit values.
    const int n = static_cast<std::int32_t>(truncToU32(vcoHz / kPllRefHz));
    if (n < 1 || n > 63) return kPllIntOutOfRange;
    const double rem = vcoHz - n * kPllRefHz;
    const unsigned frac = truncToU32(rem / kPllFracStepHz);
    if (frac >= 4096) return kPllFracOutOfRange;
    const unsigned afc = truncToU32((rem - frac * kPllFracStepHz) / kPllAfcStepHz) & 0xFFFu;
    out.INT = n;
    out.FRAC = static_cast<int>(frac);
    out.AFC = static_cast<int>(afc);
    out.r3 = afc << 4 | 3u;
    out.r2 = (static_cast<std::uint32_t>(n) << 12 | frac) << 4 | 2u;
    return kPllOk;
}

int tunerPll(double rfHz, double ppm, double mixHz, const BandState &st, TunerPll &out)
{
    out.fSynthHz = synthFreqHz(rfHz, ppm, mixHz, st); // kept when the range checks fail
    out.fail = synthWords(out.fSynthHz * st.loDiv, out);
    return out.fail == kPllOk ? sdrplay_api_Success : sdrplay_api_OutOfRange;
}

Trims readbackTrims(std::uint32_t w)
{
    Trims t;
    t.t[0] = static_cast<int>(w & 0x1F);
    t.t[1] = static_cast<int>((w >> 5) & 0xF);
    t.t[2] = static_cast<int>((w >> 9) & 0x1F);
    t.t[3] = static_cast<int>((w >> 14) & 0xF);
    t.t[4] = static_cast<int>((w >> 18) & 0x1F);
    int t5 = static_cast<int>((w >> 23) & 7);
    if (t5 == 4) t5 = 3; // a read-back trim 5 of 4 is programmed back (r14) as 3
    t.t[5] = t5;
    return t;
}

void bypassTrim(const Trims &t, int en0, std::uint32_t &r13, std::uint32_t &r14)
{
    r13 = static_cast<std::uint32_t>(t.t[1] << 11 | t.t[0] << 5 | en0 << 4 | 0xD);
    r14 = static_cast<std::uint32_t>(t.t[5] << 21 | t.t[4] << 16 | t.t[3] << 11 | t.t[2] << 5 | 0xE);
}

// ------------------------------------------------------------------ gain
std::uint32_t gainBits(const BandState &st, int gApplied, int lnaState)
{
    const LnaEntry &e = kLnaTable[st.lnaRow][lnaState];
    std::uint32_t g = static_cast<std::uint32_t>(gApplied) | (e.mixGR ? 0x100u : 0u);
    if (st.band == 0 || st.band == 3) {
        if (e.lnaGR) g |= 0xC0;
    } else if (e.lnaGR) {
        g |= 0x200;
    }
    return g;
}

int totalGr(const BandState &st, int gApplied, int lnaState)
{
    const LnaEntry &e = kLnaTable[st.lnaRow][lnaState];
    int t = e.attDb + gApplied + (e.mixGR ? 19 : 0);
    if (e.lnaGR) {
        static const int kBandLna[6] = {24, 24, 24, 24, 7, 5};
        t += kBandLna[st.band];
    }
    return t;
}

std::uint8_t attBits(int attDb)
{
    switch (attDb % 20) {
    case 0: return 0x1C;
    case 6: return 0x18;
    case 12: return 0x14;
    default: return 0x0C; // 18
    }
}

std::uint8_t feBandA(const BandState &st)
{
    const bool on = st.band == 1 || st.band == 2 || st.band == 4 || st.band == 5 || (st.band == 0 && st.feSub <= 3);
    return on ? 0x80 : 0x00;
}

std::uint8_t feBandB(const BandState &st)
{
    std::uint8_t hi = 0;
    if (st.band == 1 || st.band == 4) hi = 0x80;
    else if (st.band == 2 || st.band == 5) hi = 0x40;
    std::uint8_t lo = 1;
    if (st.band == 0) {
        static const std::uint8_t kSub0[6] = {1, 1, 0, 2, 1, 1};
        lo = kSub0[st.feSub];
    } else if (st.band == 3) {
        lo = st.feSub == 8 ? 3 : st.feSub == 9 ? 0 : 2;
    }
    return static_cast<std::uint8_t>(hi | lo);
}

DownConv downConverter(double fsHz, int bwType, int ifType)
{
    DownConv d;
    const auto set = [&d](dsp::DownConvPath path, int countShift, int countDivisor) {
        d.enabled = true;
        d.path = path;
        d.countShift = countShift;
        d.countDivisor = countDivisor;
    };
    const int fs = roundedFsHz(fsHz);
    if (ifType == sdrplay_api_IF_0_450 && fs == 2000000 &&
        (bwType == sdrplay_api_BW_0_200 || bwType == sdrplay_api_BW_0_300)) {
        set(dsp::DownConvPath::kHalfBand4, 2, 1);
    } else if (ifType == sdrplay_api_IF_0_450 && fs == 2000000 && bwType == sdrplay_api_BW_0_600) {
        set(dsp::DownConvPath::kHalfBand2, 1, 1);
    } else if (ifType == sdrplay_api_IF_1_620 && fs == 6000000 && bwType <= sdrplay_api_BW_1_536) {
        set(dsp::DownConvPath::kLif1620Decim3, 0, 3);
    } else if (ifType == sdrplay_api_IF_2_048 && fs == 8192000 && bwType == sdrplay_api_BW_1_536) {
        set(dsp::DownConvPath::kHalfBand4, 2, 1);
    } else if (ifType == sdrplay_api_IF_2_048 && fs == 8000000 &&
               (bwType == sdrplay_api_BW_1_536 || bwType == sdrplay_api_BW_5_000)) {
        set(dsp::DownConvPath::kHalfBand4, 2, 1);
        d.adsbCfg = true;
    }
    return d;
}

float gainFreqMHz(double fSynthHz, double mixHz, double lif1Hz)
{
    // fSynth is kept when programRf's INT/FRAC checks fail, so it can be anything a
    // user ppm produces (e.g. ppm 1e300): narrow without UB
    return toFloatIeee(((fSynthHz - mixHz) - lif1Hz) / 1000000.0);
}

double dabNotchCorrection(float f, const double *table)
{
    const double d = static_cast<double>(f);
    if (f > 600.0f) return 0.4 - 0.4 * (d - 600.0) / 1400.0;
    if (f <= 330.0f) {
        const int i = truncToI32(d / 5.0);
        if (i < 0 || i >= kDabTableSize) return 0.0;
        if (i == kDabTableSize - 1) return table[i];
        return table[i] + (table[i + 1] - table[i]) * (d - i * 5.0) / 5.0;
    }
    return 1.7 - 1.3 * (d - 330.0) / 270.0;
}

GainVals gainValues(float f, int bwType, int ifType, bool downConv, int minGr, int gApplied, int lnaState,
                    bool dabNotchOn)
{
    // Gain values: float32 throughout, DAB term in double.
    // Group g holds the frequencies below kBounds[g] (MHz); the last group has no upper
    // bound. A NaN frequency stays in group 0 (its result is NaN in every group).
    static const float kBounds[] = {2.0f, 12.0f, 30.0f, 50.0f, 60.0f, 120.0f, 250.0f, 300.0f, 380.0f, 420.0f, 1000.0f};
    int grp = 0;
    while (grp < kGvGroups - 1 && f >= kBounds[grp]) ++grp;
    int first = 0;
    for (int g = 0; g < grp; ++g) first += kGvGroupCount[g];
    const int n = kGvGroupCount[grp];
    // A column outside the row (an LNA state above 9, reachable with the AGC on) reads 0.
    auto at = [](int row, int col) { return (col >= 0 && col < kGvCols) ? kGvTable[row][col] : 0.0f; };
    // row pick: first i with f < rows[i+1].freq, else the last two rows (extrapolation)
    int loRow = first + n - 2, hiRow = first + n - 1;
    for (int i = 0; i < n - 1; ++i) {
        if (f < at(first + i + 1, 0)) {
            loRow = first + i;
            hiRow = first + i + 1;
            break;
        }
    }
    float lowIfCorr = -6.5f, zeroIfCorr = -3.5f;
    switch (bwType) {
    case 5000: lowIfCorr = -14.0f; zeroIfCorr = -11.0f; break;
    case 6000: lowIfCorr = -15.5f; zeroIfCorr = -12.5f; break;
    case 7000: lowIfCorr = -16.5f; zeroIfCorr = -13.5f; break;
    case 8000: lowIfCorr = -18.0f; zeroIfCorr = -15.0f; break;
    default: break;
    }
    float corr = (ifType == 450 || ifType == 1620 || ifType == 2048) ? lowIfCorr : zeroIfCorr;
    if (downConv) corr = corr - 3.0f;
    const float loF = at(loRow, 0);
    const float frac = (f - loF) / (at(hiRow, 0) - loF);
    // float32, linear between the two rows
    auto interp = [&](int col) {
        const float lo = at(loRow, col);
        return lo + corr + (at(hiRow, col) - lo) * frac;
    };
    GainVals v;
    v.curr = interp(lnaState + 1) - static_cast<float>(gApplied);
    v.max = interp(1) - static_cast<float>(minGr);
    v.min = interp(kGvCols - 1) - 59.0f;
    if (dabNotchOn) {
        const float cf = static_cast<float>(dabNotchCorrection(f, kDabNotchTable));
        v.curr = v.curr - cf;
        v.max = v.max - cf;
        v.min = v.min - cf;
    }
    return v;
}

} // namespace rsp1b
} // namespace device
} // namespace sdrplay
