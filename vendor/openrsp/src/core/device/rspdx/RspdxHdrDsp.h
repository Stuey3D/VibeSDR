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

// RSPdx HDR digital down-converters: the direct-sampled real HF
// signal in I (Q ignored) is mixed to I/Q and decimated. Pure integer DSP
// apart from the decimate-by-4 converter's NCO.
//
//   HdrDecim4  (fs != 6 MHz): libm sincos NCO at Frf*2pi/8e6, 23-tap /2 FIR
//              (fir23), 94-tap /2 FIR by hdrBw (fir94): n real in -> n/4 I/Q out.
//   HdrDecim3  (fs == 6 MHz): table NCO chosen from the exact Frf at reset
//              (875 kHz table otherwise), 140-tap /3 FIR by hdrBw (fir140):
//              n in -> ceil(n/3) out.
//
// Both keep their NCO phase and FIR histories across blocks; reset() starts
// over (stream reset, or hdrBw / hdrActive / Frf changed since the last
// block - HdrDspTracker).
//
// Integer arithmetic wraps as int32 (computed in uint32).
// Output buffers may alias the input (the conversion is in place: every
// input sample is consumed before the first output is written).
#pragma once

#include <cstdint>
#include <vector>

namespace sdrplay {
namespace device {
namespace rspdx {

class HdrDecim4 {
public:
    void reset(std::int32_t frf);
    // Returns the number of I/Q samples written (n / 4).
    std::uint32_t process(const std::int16_t *x, std::uint32_t n, int hdrBw, std::int16_t *outI, std::int16_t *outQ);

    double phase() const { return phi_; }
    double phaseStep() const { return dphi_; }

private:
    double phi_ = 0.0;
    double dphi_ = 0.0;
    std::vector<std::int32_t> xi_ = std::vector<std::int32_t>(22, 0), xq_ = std::vector<std::int32_t>(22, 0); // stage-1 input (22 history)
    std::vector<std::int32_t> yi_ = std::vector<std::int32_t>(93, 0), yq_ = std::vector<std::int32_t>(93, 0); // stage-2 input (93 history)
};

class HdrDecim3 {
public:
    void reset(std::int32_t frf);
    // Returns the number of I/Q samples written (ceil(n / 3)).
    std::uint32_t process(const std::int16_t *x, std::uint32_t n, int hdrBw, std::int16_t *outI, std::int16_t *outQ);

    int tableIndex() const { return table_; }
    int iIndex() const { return iI_; }
    int qIndex() const { return iQ_; }

private:
    int table_ = 9; // kHdrDecim3Nco entry (9 = 875 kHz default)
    int iI_ = 0, iQ_ = 0;
    std::vector<std::int32_t> xi_ = std::vector<std::int32_t>(139, 0), xq_ = std::vector<std::int32_t>(139, 0);
};

// HDR NCO cos/sin as glibc sincos evaluates them.
void hdrSinCos(double x, double *s, double *c);
// v rounded half away from zero, then (short) (decimate-by-4 NCO samples, v = 32767 cos / -32767 sin).
std::int16_t hdrDecim4NcoRound(double v);

// Stream thread: the HDR down-converter is reset when hdrBw, hdrActive or Frf
// changed since the last block, or on a stream reset.
struct HdrDspTracker {
    bool valid = false;
    int hdrBw = -1;
    bool hdrActive = false;
    std::int32_t frf = 0;
    // Returns true when the converter must be reset for this block.
    bool update(int bw, bool active, std::int32_t f, bool streamReset);
};

} // namespace rspdx
} // namespace device
} // namespace sdrplay
