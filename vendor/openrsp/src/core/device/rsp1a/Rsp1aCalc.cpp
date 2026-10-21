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

#include "core/device/rsp1a/Rsp1aCalc.h"

#include "core/device/rsp1b/Rsp1bTables.h"
#include "core/dsp/DspCommon.h"

namespace sdrplay {
namespace device {
namespace rsp1a {

// ------------------------------------------------------------------ band program
std::uint8_t feBandCode(const BandState &st)
{
    // band code written by programBand: bands 1/4 -> 0x04, 2/5 -> 0x14;
    // bands 0 and 3 depend on the front-end filter band:
    //   band 0: filter 2 -> 0x02, 3 -> 0x01, 4 -> 0x03, else 0x00
    //   band 3: filter 8 -> 0x08, 9 -> 0x18, else 0x10
    const int filter = st.rsp1aSub;
    switch (st.band) {
    case 0: return filter == 2 ? 0x02 : filter == 3 ? 0x01 : filter == 4 ? 0x03 : 0x00;
    case 1:
    case 4: return 0x04;
    case 2:
    case 5: return 0x14;
    case 3: return filter == 8 ? 0x08 : filter == 9 ? 0x18 : 0x10;
    default: return 0x00;
    }
}

std::uint32_t bridgeReg8(Model m, const BandState &st)
{
    // writeBridgeGpio: RSP1 0xF280 with the band's sub code in bits 11:8; RSP1A 0xEE80
    if (m == Model::Rsp1) return static_cast<std::uint32_t>(st.sub) << 8 | 0xF280u;
    return 0xEE80u;
}

// ------------------------------------------------------------------ gain
int numLnaStates(Model m) { return m == Model::Rsp1 ? kLnaStatesRsp1 : kLnaStatesRsp1a; }

int lnaRow(Model m, const BandState &st)
{
    // LNA table row per band (model-independent band state):
    // RSP1A 0 1 1 1 2 3 (RSP1B only: 4 in band 0 from 50 MHz), RSP1 0 0 0 0 1 2.
    static const int kRowA[6] = {0, 1, 1, 1, 2, 3};
    static const int kRow1[6] = {0, 0, 0, 0, 1, 2};
    const int b = (st.band >= 0 && st.band < 6) ? st.band : 0;
    return m == Model::Rsp1 ? kRow1[b] : kRowA[b];
}

int clampLna(Model m, int lnaState)
{
    const int top = numLnaStates(m) - 1;
    return lnaState > top ? top : lnaState;
}

const LnaEntry &lnaEntry(Model m, const BandState &st, int lnaState)
{
    if (m == Model::Rsp1) return kLnaRsp1[lnaRow(m, st)][lnaState];
    return kLnaRsp1a[lnaRow(m, st)][lnaState];
}

bool setGrValid(Model m, const BandState &st, int gRdB, int minGr, int lnaState)
{
    if (!(minGr <= gRdB && gRdB < 60)) return false;
    if (m == Model::Rsp1) return lnaState < 4;
    if (lnaState >= 10) return false;
    if (lnaState >= 7 && st.band == 0) return false;
    if (lnaState == 9 && st.band == 5) return false;
    return true;
}


bool agcLnaValid(Model m, const BandState &st, int lnaState)
{
    if (m == Model::Rsp1) return lnaState <= 3;
    if (lnaState > 9) return false;
    if (lnaState > 6 && st.band == 0) return false;
    if (lnaState == 9 && st.band > 4) return false;
    return true;
}


std::uint32_t gainBits(Model m, const BandState &st, int gApplied, int lnaState)
{
    // programGain: r1 payload = gApplied | (mix != 0) << 8 | (bands 0/3: lna ? 0xC0 : 0; else (lna != 0) << 9)
    const LnaEntry &e = lnaEntry(m, st, lnaState);
    std::uint32_t g = static_cast<std::uint32_t>(gApplied) | (e.mixGR ? 0x100u : 0u);
    if (st.band == 0 || st.band == 3) {
        if (e.lnaGR) g |= 0xC0;
    } else if (e.lnaGR) {
        g |= 0x200;
    }
    return g;
}

int totalGr(Model m, const BandState &st, int gApplied, int lnaState)
{
    // total GR = attDb + gApplied, + 19 if the mixer GR is on, + the LNA GR if it is on
    // (24 on the AM path, else the band's LNA dB)
    const LnaEntry &e = lnaEntry(m, st, lnaState);
    int t = e.attDb + gApplied + (e.mixGR ? 19 : 0);
    if (e.lnaGR) {
        static const int kBandLna[6] = {24, 24, 24, 24, 7, 5}; // AM path 24; else the band's LNA dB
        t += kBandLna[(st.band >= 0 && st.band < 6) ? st.band : 0];
    }
    return t;
}

std::uint8_t attBitsA(int attDb)
{
    switch (attDb) {
    case 0:
    case 20: return 0xE0;
    case 6:
    case 26: return 0xC0;
    case 12:
    case 32: return 0xA0;
    case 18:
    case 38: return 0x60;
    default: return 0x00;
    }
}

std::uint8_t padBitB(int attDb)
{
    switch (attDb) {
    case 20:
    case 26:
    case 32:
    case 38: return 0x02;
    default: return 0x00;
    }
}

// ------------------------------------------------------------------ gainVals
int gvGroup(Model m, float f)
{
    // Group g holds the frequencies below bound g (MHz); the last group has no upper bound.
    // A NaN frequency stays in group 0 (its gain values are NaN in every group).
    static const float kBoundsRsp1a[] = {2.0f, 12.0f, 30.0f, 60.0f, 120.0f, 250.0f, 300.0f, 380.0f, 420.0f, 1000.0f};
    static const float kBoundsRsp1[] = {12.0f, 30.0f, 60.0f, 120.0f, 250.0f, 420.0f, 1000.0f};
    const bool r1 = m == Model::Rsp1;
    const float *bounds = r1 ? kBoundsRsp1 : kBoundsRsp1a;
    const int numBounds = r1 ? kGvGroupsRsp1 - 1 : kGvGroupsRsp1a - 1;
    int g = 0;
    while (g < numBounds && f >= bounds[g]) ++g;
    return g;
}

double dabNotchCorrection(float f) { return rsp1b::dabNotchCorrection(f, rsp1b::kDabNotchTable); }

GainVals gainValues(Model m, float f, int bwType, int ifType, bool downConv, int minGr, int gApplied, int lnaState,
                    bool dabNotchOn)
{
    const bool r1 = m == Model::Rsp1;
    const float *rows = r1 ? &kGvRsp1[0][0] : &kGvRsp1a[0][0];
    const int numRows = r1 ? kGvRowsRsp1 : kGvRowsRsp1a;
    const int width = r1 ? kGvWidthRsp1 : kGvWidthRsp1a;
    const std::uint8_t *count = r1 ? kGvRsp1Count : kGvRsp1aCount;
    const int lastCol = width - 1; // LNA 3 / LNA 9 column

    const int grp = gvGroup(m, f);
    int first = 0;
    for (int g = 0; g < grp; ++g) first += count[g];
    const int n = count[grp];
    // A column outside the row (an LNA state above the model's range, reachable with the
    // AGC on) reads 0.
    auto at = [&](int row, int col) {
        return (row >= 0 && row < numRows && col >= 0 && col < width) ? rows[row * width + col] : 0.0f;
    };
    // row pick: first i with f < rows[i+1].freq, else the last two rows (extrapolation)
    int loRow = first + n - 2, hiRow = first + n - 1;
    for (int i = 0; i < n - 1; ++i) {
        if (f < at(first + i + 1, 0)) {
            loRow = first + i;
            hiRow = first + i + 1;
            break;
        }
    }
    // corr: low IF (450/1620/2048) vs zero IF per bwType
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
    auto G = [&](int col) {
        const float lo = at(loRow, col);
        return lo + corr + (at(hiRow, col) - lo) * frac;
    };
    GainVals v;
    v.curr = G(lnaState + 1) - static_cast<float>(gApplied);
    v.max = G(1) - static_cast<float>(minGr);
    v.min = G(lastCol) - 59.0f;
    if (!r1 && dabNotchOn) {
        const float c = static_cast<float>(dabNotchCorrection(f));
        v.min = v.min - c;
        v.max = v.max - c;
        v.curr = v.curr - c;
    }
    return v;
}

} // namespace rsp1a
} // namespace device
} // namespace sdrplay
