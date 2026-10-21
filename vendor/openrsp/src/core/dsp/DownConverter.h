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

// Low-IF down-converter (non-RSPduo): path selection from fs/bandwidth/IF, then the
// half-band paths (with the ADS-B variants) or the 1620 kHz decimate-by-3 path.
//
// Exactness: the mixer and
// every intermediate filter stage keep int32 values;
// only the final output is narrowed to int16 by wrapping. Filter sums wrap at
// 32 bits. Mixer phases persist across blocks and are cleared on `reset`.
#pragma once

#include <cstdint>
#include <vector>

namespace sdrplay {
namespace dsp {

// Down-converter paths. The active paths mix the real low-IF input to zero IF and decimate.
enum class DownConvPath : std::uint8_t {
    kNone,          // no down-conversion
    kHalfBand2,     // IF 450 kHz, bw 600 kHz: one 23-tap half-band, decimate by 2
    kLif1620Decim3, // IF 1620 kHz at fs 6 MHz: 300-step mixer, 58-tap low-pass, decimate by 3
    kHalfBand4,     // 23-tap then 51-tap half-band, decimate by 4
};

struct DownConverterConfig {
    bool lif = false;          // ifType != sdrplay_api_IF_Zero
    bool enable = false;
    DownConvPath path = DownConvPath::kNone;
    // Sample count and first sample number after the down-converter: (n >> countShift) / countDivisor.
    std::uint32_t countShift = 0;
    std::uint32_t countDivisor = 1;
    bool adsbCfg = false;      // fs 8 MHz / IF 2048 / bw 1536|5000
    bool ionly = false;        // lif && (!enable || path == kNone)
    int ifType = 0;            // mixer selection (2048: fs/4, 450: COS40/SIN40)
    double mixHz = 0.0;        // NCO frequency the tuner programming uses (info only)
};

// Down-converter selection for a non-RSPduo device. fsMHz is the device fs in MHz as the
// service stores it; it is compared as (int)(1e6*fsMHz + 0.5).
DownConverterConfig selectDownConverter(double fsMHz, int bwType, int ifType);

// N (packets per block) for the channel (decimation fields from the live channel params).
std::uint32_t numActivePackets(const DownConverterConfig &dc, bool decEnable, std::uint8_t decFactor);

class DownConverter {
public:
    DownConverter();
    void init();  // channel init: all histories and phases zeroed (Q phase of the 1620 kHz mixer = 75)
    // Runs the down-converter in place on xi (real input; xq is output only).
    // adsbMode is the live channel value. Returns the number of output samples
    // (n / M for the decimating paths, n for ADS-B modes 1-3, n without down-conversion).
    std::uint32_t run(const DownConverterConfig &cfg, std::int16_t *xi, std::int16_t *xq, std::uint32_t n,
                      bool reset, int adsbMode);

private:
    void resetState();
    void normalPath(const DownConverterConfig &cfg, std::int16_t *xi, std::int16_t *xq, std::uint32_t n);
    void lif1620Path(std::int16_t *xi, std::int16_t *xq, std::uint32_t n);
    void adsbLowPass(std::int16_t *xi, std::int16_t *xq, std::uint32_t n);
    void adsbBandPass(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool bandPass2MHz);

    struct Fir {
        std::vector<std::int32_t> buf; // history (L-1) followed by the new block
        std::uint32_t hist = 0;
        void setup(std::uint32_t L, std::uint32_t maxIn);
        void clear();
        std::int32_t *input() { return buf.data() + hist; }
        // filter the `n` samples already written at input(); y gets ceil(n/M) values
        std::uint32_t run(std::uint32_t n, std::uint32_t M, const std::int32_t *h, std::uint32_t L, std::int32_t *y);
    };
    Fir hb1I_, hb1Q_, hb2I_, hb2Q_; // 23 taps, 51 taps
    Fir h58I_, h58Q_;               // 1620 kHz path
    Fir lpfI_, lpfQ_;               // ADS-B mode 1
    Fir bpf_, hbfI_, hbfQ_;         // ADS-B modes 2/3
    std::vector<std::int32_t> tmpI_, tmpQ_;
    int phase40_ = 0;
    int phaseI300_ = 0;
    int phaseQ300_ = 75;
};

} // namespace dsp
} // namespace sdrplay
