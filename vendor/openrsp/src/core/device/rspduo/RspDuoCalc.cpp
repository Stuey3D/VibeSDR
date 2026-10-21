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

#include "core/device/rspduo/RspDuoCalc.h"

#include "core/device/rsp1b/Rsp1bTables.h"
#include "core/device/rspduo/RspDuoTables.h"

namespace sdrplay {
namespace device {
namespace rspduo {

namespace {
// Highest LNA state in band 0: 4 on the tuner-1 HiZ port (amPortSel 1), 6 otherwise.
int band0MaxLna(int tuner, int amPortSel) { return tuner == 1 && amPortSel == 1 ? 4 : 6; }
} // namespace

int fsState(double fsHz, int ifType, int mode, bool bulk, FsState &out)
{
    const bool zeroIfTable = mode != sdrplay_api_RspDuoMode_Single_Tuner || ifType == 0;
    return rsp1b::fsState(fsHz, !zeroIfTable, bulk, out);
}

double mixFreq(double fsHz, int bwType, int ifType, int mode, bool fs8192)
{
    if (lifForced(mode)) {
        if (fs8192) return 2048000.0;
        if (fsHz == 6000000.0) return 1580000.0;
        return 2000000.0;
    }
    return rsp1b::mixFreq(fsHz, bwType, ifType);
}

int r0IfType(double fsHz, int ifType, int mode, bool fs8192)
{
    if (lifForced(mode)) {
        if (fs8192) return 2048;
        if (fsHz == 6000000.0) return 1620;
        return 2048;
    }
    return ifType;
}

DownConv downConverter(double fsHz, int bwType, int ifType, int mode, bool fs8192)
{
    if (!lifForced(mode)) return rsp1b::downConverter(fsHz, bwType, ifType);
    DownConv d;
    d.enabled = true;
    if (fs8192) {
        d.path = dsp::DownConvPath::kHalfBand4; d.countShift = 2;
    } else if (fsHz == 6000000.0) {
        d.path = dsp::DownConvPath::kLif1620Decim3; d.countDivisor = 3;
    } else {
        d.path = dsp::DownConvPath::kHalfBand4; d.countShift = 2;
        d.adsbCfg = bwType == 1536 || bwType == 5000;
    }
    return d;
}

int lnaRowOfBand(int band)
{
    static const int kRow[6] = {0, 1, 1, 1, 2, 3};
    return band >= 0 && band < 6 ? kRow[band] : 0;
}

int lnaRow(const BandState &st, int tuner, int amPortSel)
{
    const int row = lnaRowOfBand(st.band);
    if (st.band == 0 && amPortSel == 1 && tuner == 1) return kLnaRowHiZ;
    return row;
}

bool hizGainPath(const BandState &st, int tuner, int amPortSel)
{
    return (st.band == 0 || st.band == 3) && tuner == 1 && amPortSel == 1;
}

GainWord gainWord(const BandState &st, int tuner, int amPortSel, int gApplied, int lnaState)
{
    // callers clamp lnaState to 0..9; anything else reads as the all-zero entry
    static const LnaEntry kNone = {0, 0, 0};
    const LnaEntry &e =
        lnaState >= 0 && lnaState < kLnaStates ? kLnaTable[lnaRow(st, tuner, amPortSel)][lnaState] : kNone;
    GainWord w;
    w.attDb = e.attDb;
    w.mixOn = e.mixGR != 0 ? 1 : 0;
    const std::uint32_t mix = static_cast<std::uint32_t>(w.mixOn) << 8;
    if (st.band == 0 || st.band == 3) {
        w.bandLnaOn = 0;
        if (hizGainPath(st, tuner, amPortSel)) {
            w.lnaAcc = e.lnaGR;
            w.g = mix | static_cast<std::uint32_t>(gApplied) | hizLnaBits(e.lnaGR);
        } else {
            w.g = mix | static_cast<std::uint32_t>(gApplied);
            w.lnaAcc = 0;
            if (e.lnaGR != 0) {
                w.g |= 0xC0;
                w.lnaAcc = 24;
            }
        }
    } else {
        w.lnaAcc = 0;
        w.bandLnaOn = e.lnaGR != 0 ? 1 : 0;
        w.g = static_cast<std::uint32_t>(w.bandLnaOn) << 9 | mix | static_cast<std::uint32_t>(gApplied);
    }
    const int band = st.band >= 0 && st.band < 6 ? st.band : 0;
    w.total = kBandLnaGr[band] * w.bandLnaOn + 19 * w.mixOn + w.attDb + gApplied + w.lnaAcc;
    return w;
}

std::uint8_t attStep(int attDb)
{
    switch (attDb) {
    case 0: case 20: return 7;
    case 6: case 26: return 6;
    case 12: case 32: return 5;
    case 18: case 38: return 3;
    default: return 0;
    }
}

bool attPad(int attDb)
{
    return attDb == 20 || attDb == 26 || attDb == 32 || attDb == 38;
}

std::uint32_t hizLnaBits(int lnaGR)
{
    switch (lnaGR) {
    case 6: return 0x40;
    case 12: return 0x80;
    case 18: return 0xC0;
    default: return 0;
    }
}

bool grValid(int gRdB, int minGr, int lnaState, int band, int tuner, int amPortSel)
{
    if (gRdB < minGr || gRdB > 59 || lnaState > 9) return false;
    if (band == 0) {
        if (tuner == 3 && amPortSel == 1) return false; // the HiZ port is not available to both tuners
        return lnaState <= band0MaxLna(tuner, amPortSel);
    }
    if (band == 5) return lnaState != 9;
    return true;
}

bool agcLnaValid(int lnaState, int band, int tuner, int amPortSel)
{
    if (lnaState > 9) return false;
    if (band == 0) return lnaState <= band0MaxLna(tuner, amPortSel);
    if (band > 4) return lnaState != 9;
    return true;
}

std::uint8_t bandCode(const BandState &st)
{
    switch (st.band) {
    case 0:
        switch (st.duoSub) {
        case 2: return 0x02;
        case 3: return 0x01;
        case 4: return 0x03;
        default: return 0x00;
        }
    case 3:
        if (st.duoSub == 8) return 0x08;
        if (st.duoSub == 9) return 0x18;
        return 0x10;
    case 1:
    case 4: return 0x04;
    default: return 0x14; // bands 2 and 5
    }
}

std::uint32_t reg0Word(const BandState &st, int bwType, int ifTypeForR0, bool loCodePath, bool hiz)
{
    std::uint32_t w = rsp1b::reg0Word(st, bwType, ifTypeForR0, loCodePath);
    if (hiz && st.band == 0) w &= ~0x800u; // HiZ base 0x40610 in place of the band-0 base 0x40e10
    return w;
}

int tunerPll(double rfHz, double ppm, double mixHz, const BandState &st, double &fSynthHz, TunerPll &out)
{
    // programRf PLL (the RSP1B synthesiser)
    fSynthHz = rsp1b::synthFreqHz(rfHz, ppm, mixHz, st);
    return rsp1b::synthWords(fSynthHz * st.loDiv, out);
}

bool rfInHizRange(double rfHz) { return 0.0 <= rfHz && rfHz <= 60000000.0; }

bool rfInBandRange(double rfHz, int band)
{
    if (band < 0 || band > 5) return false;
    return kBandRange[band].loMHz * 1000000.0 <= rfHz && rfHz <= kBandRange[band].hiMHz * 1000000.0;
}

int gainGroup(float f)
{
    // group g covers [kEdges[g - 1], kEdges[g]) MHz
    static const float kEdges[] = {2.0f, 12.0f, 30.0f, 60.0f, 120.0f, 250.0f, 300.0f, 380.0f, 420.0f, 1000.0f};
    int g = 0;
    while (g < 10 && f >= kEdges[g]) ++g;
    return g;
}

int gainValues(float f, int tuner, bool hiz, int bwType, int ifType, bool downConv, int minGr, int gApplied,
               int lnaState, bool dabNotchOn, GainVals &v)
{
    // Same float32 grouping as the RSP1B code.
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

    int grp;
    if (hiz) {
        if (tuner != 1 || f >= 30.0f) return 1; // the HiZ port is on tuner 1 only, below 30 MHz
        grp = f >= 2.0f ? 12 : 11;
    } else {
        grp = gainGroup(f);
    }
    const bool setB = tuner == 2;
    const float *tbl = setB ? kGvBTable : kGvATable;
    const std::uint8_t *cnt = setB ? kGvBGroupCount : kGvAGroupCount;
    const int rows = setB ? kGvBRows : kGvARows;
    int first = 0;
    for (int g = 0; g < grp; ++g) first += cnt[g];
    const int n = cnt[grp];
    int loRow = first + n - 2, hiRow = first + n - 1;
    for (int i = 0; i < n - 1; ++i) {
        if (f < tbl[(first + i + 1) * 11]) {
            loRow = first + i;
            hiRow = first + i + 1;
            break;
        }
    }
    // 0 outside the table or the row (a column past G9 is reachable with LNAstate > 9 and AGC on)
    auto at = [tbl, rows](int row, int col) {
        if (row < 0 || row >= rows || col < 0 || col >= 11) return 0.0f;
        return tbl[row * 11 + col];
    };
    const float loF = at(loRow, 0);
    const float frac = (f - loF) / (at(hiRow, 0) - loF);
    // float32, linear between the two rows
    auto interp = [&](int col) {
        const float lo = at(loRow, col);
        return lo + corr + (at(hiRow, col) - lo) * frac;
    };
    GainVals out;
    out.curr = interp(lnaState + 1) - static_cast<float>(gApplied);
    out.max = interp(1) - static_cast<float>(minGr);
    out.min = interp(10) - 59.0f;
    if (dabNotchOn) {
        const float cf = static_cast<float>(rsp1b::dabNotchCorrection(f, rsp1b::kDabNotchTable));
        out.curr = out.curr - cf;
        out.max = out.max - cf;
        out.min = out.min - cf;
    }
    v = out;
    return 0;
}

} // namespace rspduo
} // namespace device
} // namespace sdrplay
