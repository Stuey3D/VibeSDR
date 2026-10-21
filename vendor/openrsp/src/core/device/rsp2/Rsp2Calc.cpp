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

#include "core/device/rsp2/Rsp2Calc.h"

#include "core/device/rsp2/Rsp2Tables.h"

namespace sdrplay {
namespace device {
namespace rsp2 {

int lnaRow(const BandState &st)
{
    // LNA table row per band
    static const int kRow[6] = {0, 0, 0, 1, 2, 3};
    return (st.band >= 0 && st.band < 6) ? kRow[st.band] : 0;
}

int subBand(const BandState &st) { return st.rsp2Sub; }

std::uint32_t reg0Word(const BandState &st, int bwType, int ifType, bool loCodePath, int amPortSel)
{
    // AM port selected in band 0: base 0x40610 instead of 0x40E10, i.e. bit 11 cleared
    // (no other field of the word uses bit 11).
    std::uint32_t w = rsp1b::reg0Word(st, bwType, ifType, loCodePath);
    if (st.band == 0 && amPortSel == 1) w &= ~0x800u;
    return w;
}

std::uint8_t feBandA(const BandState &st)
{
    // band 0 and band 3 by sub-band; bands 1/4 and 2/5 fixed
    const int sub = subBand(st);
    switch (st.band) {
    case 0: return sub == 2 ? 0x20 : (sub == 3 ? 0x10 : 0x00);
    case 1:
    case 4: return 0x18;
    case 2:
    case 5: return 0x38;
    case 3: return sub == 7 ? 0x08 : (sub == 8 ? 0x28 : 0x30);
    default: return 0x00;
    }
}

std::uint8_t feAttPreset(const BandState &st) { return st.band == 3 ? 0x38 : 0x24; }

std::uint8_t attBits(int attDb)
{
    // step-attenuator bits for the LNA-table attDb values
    switch (attDb) {
    case 0: return 0x38;
    case 10: return 0x34;
    case 15: return 0x24;
    case 21: return 0x14;
    default: return 0x00;
    }
}

std::uint32_t amLnaBits(int lnaGR)
{
    switch (lnaGR) {
    case 6: return 0x40;
    case 12: return 0x80;
    case 18: return 0xC0;
    default: return 0;
    }
}

bool lnaStateValid(int lnaState, int band, int amPortSel)
{
    if (lnaState > 8 || lnaState < 0) return false;
    if (lnaState >= 5 && band == 0) return amPortSel != 1;
    if (lnaState > 5 && band > 3) return false;
    return true;
}

GainCalc gainCalc(const BandState &st, int amPortSel, int gApplied, int lnaState)
{
    GainCalc c;
    int row = lnaRow(st);
    if (st.band == 0 && amPortSel == 1 && row == 0) row = 4;
    c.row = row;
    const LnaEntry &e = kLnaTable[row][lnaState];
    c.attDb = e.attDb;
    c.lnaGR = e.lnaGR;
    c.mixF = e.mixGR != 0;
    const std::uint32_t g0 = (c.mixF ? 0x100u : 0u) | static_cast<std::uint32_t>(gApplied);
    if (st.band == 0 || st.band == 3) {
        c.lnaFlag = false;
        if (amPortSel == 1) {
            // AM-port LNA bits in band 0 and band 3 (band 3's lnaGR 24 maps to no bits)
            c.amGR = e.lnaGR;
            c.g = g0 | amLnaBits(e.lnaGR);
        } else {
            c.amGR = e.lnaGR ? 24 : 0;
            c.g = g0 | (e.lnaGR ? 0xC0u : 0u);
        }
    } else {
        c.amGR = 0;
        c.lnaFlag = e.lnaGR != 0;
        c.g = g0 | (c.lnaFlag ? 0x200u : 0u);
    }
    int att = e.attDb;
    // L-band (row 3): table steps 15 / 21 / 10 dB are reported, and counted in the total, as 11 / 10 / 14 dB
    if (row == 3) {
        if (att == 15) att = 11;
        else if (att == 21) att = 10;
        else if (att == 10) att = 14;
    }
    c.attReported = att;
    static const int kBandLna[6] = {24, 24, 24, 24, 7, 5}; // LNA gain reduction per band
    const int bandLna = (st.band >= 0 && st.band < 6) ? kBandLna[st.band] : 0;
    c.total = (c.lnaFlag ? bandLna : 0) + (c.mixF ? 19 : 0) + att + gApplied + c.amGR;
    return c;
}

int gainGroup(float f, bool amPortGroup)
{
    if (amPortGroup) return 10;
    // Group g holds the frequencies below bound g (MHz); group 9 has no upper bound.
    // A NaN frequency stays in group 0 (its gain values are NaN in every group).
    static const float kBounds[] = {12.0f, 30.0f, 60.0f, 120.0f, 250.0f, 300.0f, 380.0f, 420.0f, 1000.0f};
    int g = 0;
    while (g < 9 && f >= kBounds[g]) ++g;
    return g;
}

GainVals gainValues(float f, int bwType, int ifType, bool downConv, int minGr, int gApplied, int lnaState,
                    bool amPortGroup)
{
    // value = (hi - lo) * frac + (lo + corr), then minus gApplied / minGr / 59.
    const int grp = gainGroup(f, amPortGroup);
    int first = 0;
    for (int g = 0; g < grp; ++g) first += kGvGroupCount[g];
    const int n = kGvGroupCount[grp];
    // A column outside the row (an LNA state above 8, reachable with the AGC on) reads 0.
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
    auto interp = [&](int col) {
        const float lo = at(loRow, col);
        return lo + corr + (at(hiRow, col) - lo) * frac;
    };
    GainVals v;
    v.curr = interp(lnaState + 1) - static_cast<float>(gApplied);
    v.max = interp(1) - static_cast<float>(minGr);
    v.min = interp(kGvCols - 1) - 59.0f;
    return v;
}

} // namespace rsp2
} // namespace device
} // namespace sdrplay
