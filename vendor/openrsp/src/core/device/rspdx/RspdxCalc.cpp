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

#include "core/device/rspdx/RspdxCalc.h"

#include "core/device/rspdx/RspdxTables.h"
#include "core/dsp/DspCommon.h"

namespace sdrplay {
namespace device {
namespace rspdx {

using dsp::truncToI32;

// ------------------------------------------------------------------ band state
bool hdrActive(std::uint8_t hdrEnable, double rfHz)
{
    // HDR mode applies when it is enabled and the tuned frequency is below 2 MHz.
    return hdrEnable == 1 && rfHz < 2000000.0;
}

int lnaRow(int band, int dxSub)
{
    switch (band) {
    case 0:
        if (dxSub <= 3) return 0; // below 12 MHz
        if (dxSub <= 5) return 1; // 12-50 MHz
        return 2;                 // 50-60 MHz
    case 1:
    case 2: return 3;
    case 3: return dxSub == 9 ? 4 : 5; // 250-300 MHz / 300-420 MHz
    case 4: return 6;
    default: return 7;
    }
}

int lnaBandGR(int band)
{
    return band == 4 ? 7 : band == 5 ? 5 : 24;
}

int computeBand(double rfHz, int loMode, int firstLO, std::uint8_t hdrEnable, BandState &out)
{
    BandState s;
    const int r = rsp1b::computeBand(rfHz, loMode, firstLO, s.b);
    if (r != 0) return r;
    s.row = lnaRow(s.b.band, s.b.dxSub);
    s.lnaBandGR = lnaBandGR(s.b.band);
    s.hdrActive = hdrActive(hdrEnable, rfHz);
    out = s;
    return 0;
}

bool sameBand(const BandState &a, const BandState &b)
{
    return rsp1b::sameBand(a.b, b.b) && a.hdrActive == b.hdrActive;
}

// ------------------------------------------------------------------ gain validation
bool lnaValid(int band, int dxSub, int lna)
{
    // RSPdx LNA state limits: programGain (non-HDR) and Update (AGC on)
    if (lna >= 28) return false;
    if (lna >= 19 && dxSub <= 3) return false;
    if (lna >= 20 && dxSub <= 5) return false;
    if (lna >= 25) return dxSub > 6 && !(lna == 27 && (band == 1 || band == 2)) && band != 4 && band != 5;
    if (lna >= 21) return band != 4 && band != 5;
    if (lna >= 19) return band != 5;
    return true;
}

bool setGrValid(int gRdB, int minGr, int band, int dxSub, int lna)
{
    return minGr <= gRdB && gRdB < 60 && lnaValid(band, dxSub, lna);
}

int maxLnaState(int band, int dxSub)
{
    int m = 0;
    while (m < 27 && lnaValid(band, dxSub, m + 1)) ++m;
    return m;
}

// ------------------------------------------------------------------ front end
int attCode(int x)
{
    switch (x) {
    case 0: return 0xF;
    case 3: return 0xE;
    case 6: return 0xD;
    case 9: return 0xC;
    case 12: return 0xB;
    case 15: return 0x7;
    case 18: return 0x6;
    case 21: return 0x5;
    case 24: return 0x4;
    case 27: return 0x3;
    case 30: return 0x2;
    case 33: return 0x1;
    case 36: return 0x0;
    default: return -1;
    }
}

FeAtt attBits(int band, int dxSub, int att)
{
    // attenuator code tables: every
    // value outside a table leaves pad 0 and code 0.
    FeAtt f;
    if (band != 0) {
        if (att <= 21 && attCode(att) >= 0) {
            f.code = static_cast<std::uint8_t>(attCode(att));
        } else if (att >= 24 && att <= 60 && attCode(att - 24) >= 0) {
            f.pad = 1;
            f.code = static_cast<std::uint8_t>(attCode(att - 24));
        }
    } else if (dxSub != 6) {
        if (attCode(att) >= 0) {
            f.pad = 1;
            f.code = static_cast<std::uint8_t>(attCode(att));
        }
    } else {
        if (att <= 12 && attCode(att) >= 0) {
            f.code = static_cast<std::uint8_t>(attCode(att));
        } else if (att >= 15 && att <= 51 && attCode(att - 15) >= 0) {
            f.pad = 1;
            f.code = static_cast<std::uint8_t>(attCode(att - 15));
        }
    }
    return f;
}

HdrAtt hdrAttBits(int att)
{
    HdrAtt h;
    if (att <= 24 && attCode(att) >= 0) {
        h.a0 = 1;
        h.code = static_cast<std::uint8_t>(attCode(att));
    } else if (att == 25) {
        h.code = 0xF;
    } else if (att >= 27 && att <= 57 && attCode(att - 24) >= 0) {
        h.code = static_cast<std::uint8_t>(attCode(att - 24));
    }
    return h;
}

std::uint8_t feBandA(const BandState &st)
{
    // XA bit 7: band 0 dxSub 1..4 (f < 30 MHz) and bands 1, 2, 4, 5
    switch (st.b.band) {
    case 0: return st.b.dxSub == 5 || st.b.dxSub == 6 ? 0x00 : 0x80;
    case 3: return 0x00;
    default: return 0x80;
    }
}

std::uint8_t feBandB(const BandState &st)
{
    switch (st.b.band) {
    case 0: {
        std::uint8_t lo = 3; // dxSub 1 (and any other value)
        if (st.b.dxSub == 2) lo = 1;
        else if (st.b.dxSub == 3) lo = 0;
        else if (st.b.dxSub == 4) lo = 2;
        else if (st.b.dxSub == 5 || st.b.dxSub == 6) lo = 1;
        return lo;
    }
    case 3:
        switch (st.b.dxSub) {
        case 10: return 0;
        case 11: return 2;
        default: return 3;
        }
    case 1:
    case 4: return 0x80 | 3;
    default: return 0x40 | 3;
    }
}

LnaEntryView lnaEntry(int row, int lnaState)
{
    const int l = lnaState > 27 ? 27 : (lnaState < 0 ? 0 : lnaState);
    const int r = row < 0 ? 0 : (row >= kLnaRows ? kLnaRows - 1 : row);
    const LnaEntry &e = kLnaTable[r][l];
    return LnaEntryView{e.attDb, e.lnaGR, e.mixGR};
}

LnaEntryView hdrLnaEntry(int lnaState)
{
    const int l = lnaState > kHdrMaxLna ? kHdrMaxLna : (lnaState < 0 ? 0 : lnaState);
    const LnaEntry &e = kHdrLnaTable[l];
    return LnaEntryView{e.attDb, e.lnaGR, e.mixGR};
}

std::uint32_t gainBits(int band, int gApplied, const LnaEntryView &e)
{
    std::uint32_t g = static_cast<std::uint32_t>(gApplied) | (e.mixGR != 0 ? 0x100u : 0u);
    if (band == 0 || band == 3) {
        if (e.lnaGR != 0) g |= 0xC0;
    } else if (e.lnaGR != 0) {
        g |= 0x200;
    }
    return g;
}

int totalGr(const BandState &st, int gApplied, const LnaEntryView &e)
{
    int t = e.attDb + gApplied + (e.mixGR != 0 ? 19 : 0); // mixGR is 0 on every DX row
    if (e.lnaGR != 0) t += (st.b.band == 0 || st.b.band == 3) ? 24 : st.lnaBandGR;
    return t;
}

// ------------------------------------------------------------------ HDR
std::int32_t hdrFrf(double rfHz)
{
    const std::int32_t f = truncToI32(rfHz);
    return f > 2000000 ? kHdrDefaultFrf : f;
}

rsp1b::DownConv hdrDownConverter(double fsMHz)
{
    rsp1b::DownConv d;
    d.enabled = true;
    if (fsMHz == 6.0) {
        d.path = dsp::DownConvPath::kLif1620Decim3;
        d.countDivisor = 3;
    } else {
        d.path = dsp::DownConvPath::kHalfBand4;
        d.countShift = 4; // count / sync-update scaling n/16 (programSyncUpdate)
    }
    return d;
}

// ------------------------------------------------------------------ gainVals
int gainGroup(float f)
{
    // group g covers [kEdges[g - 1], kEdges[g]) MHz; group 0 is f < 0.5 MHz, so an unordered f (NaN)
    // falls in group 1
    static const float kEdges[] = {0.5f,   2.0f,   12.0f,  30.0f,  50.0f,  60.0f,
                                   120.0f, 250.0f, 300.0f, 380.0f, 420.0f, 1000.0f};
    if (f < kEdges[0]) return 0;
    int g = 1;
    while (g < 12 && f >= kEdges[g]) ++g;
    return g;
}

float bwIfCorr(int bwType, int ifType)
{
    const BwCorr *e = &kBwCorr[kBwCorrRows - 1]; // default row
    for (int i = 0; i < kBwCorrRows - 1; ++i)
        if (kBwCorr[i].bwType == bwType) e = &kBwCorr[i];
    const bool lowIf = ifType == 450 || ifType == 1620 || ifType == 2048;
    return lowIf ? e->lowIf : e->zeroIf;
}

namespace {
struct Table {
    const float *rows;
    int nRows;
    const std::uint8_t *counts;
};
Table tableOf(Variant v)
{
    if (v == Variant::DxR2) return Table{kGvR2Table, kGvR2Rows, kGvR2Counts};
    return Table{kGvDxTable, kGvDxRows, kGvDxCounts};
}

float at(const Table &t, int row, int col)
{
    // 0 outside the table or the row (a column past G[LNA 27] is reachable with LNAstate > 27
    // when the AGC is on)
    if (row < 0 || row >= t.nRows || col < 0 || col >= kGvRowFloats) return 0.0f;
    return t.rows[row * kGvRowFloats + col];
}

GainVals interp(Variant v, int grp, float f, float corr, int lna, float subCurr, float subMax, float subMin)
{
    const Table t = tableOf(v);
    int first = 0;
    for (int g = 0; g < grp; ++g) first += t.counts[g];
    const int n = t.counts[grp];
    int lo = first + n - 2, hi = first + n - 1;
    if (n < 1) {
        lo = first;
        hi = first + 1;
    } else {
        for (int i = 0; i < n - 1; ++i) {
            if (f < at(t, first + i + 1, 0)) {
                lo = first + i;
                hi = first + i + 1;
                break;
            }
        }
    }
    const float lo0 = at(t, lo, 0);
    const float frac = (f - lo0) / (at(t, hi, 0) - lo0);
    // float32, linear between the two rows
    auto one = [&](int col, float sub) {
        const float l = at(t, lo, col);
        return l + corr + (at(t, hi, col) - l) * frac - sub;
    };
    GainVals g;
    g.curr = one(lna + 1, subCurr);
    g.max = one(1, subMax);
    g.min = one(28, subMin);
    return g;
}
} // namespace

double dabCorrection(float f) { return rsp1b::dabNotchCorrection(f, kDabNotchDx); }

GainVals gainValues(Variant v, float f, int bwType, int ifType, bool downConv, int minGr, int gApplied, int lnaState,
                    bool dabNotchOn, bool antennaC)
{
    float corr = bwIfCorr(bwType, ifType);
    if (downConv) corr = corr - 3.0f;
    GainVals g = interp(v, gainGroup(f), f, corr, lnaState, static_cast<float>(gApplied), static_cast<float>(minGr),
                        59.0f);
    if (dabNotchOn) {
        const float c = static_cast<float>(dabCorrection(f));
        g.min = g.min - c;
        g.max = g.max - c;
        g.curr = g.curr - c;
    }
    if (antennaC && f > 30.0f) {
        const double d = static_cast<double>(f);
        const float c = static_cast<float>(d * kAntennaC[0] + kAntennaC[1]);
        g.min = g.min + c;
        g.max = g.max + c;
        g.curr = g.curr + c;
    }
    return g;
}

GainVals hdrGainVals(Variant v, std::int32_t frf, bool downConv, int lnaState)
{
    const float f = static_cast<float>(frf) / 1000000.0f;
    float corr = kBwCorr[kBwCorrRows - 1].zeroIf; // -3.5
    if (downConv) corr = corr - 3.0f;
    return interp(v, 13, f, corr, lnaState, 0.0f, 0.0f, 0.0f);
}

} // namespace rspdx
} // namespace device
} // namespace sdrplay
