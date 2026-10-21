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

// User decimation (Ctrl_Decimation): updateDecimation (staging), the dispatcher (run), and
// the kernels: boxcar (IQ / I-only), HB1 + FIR108 (wideBandSignal = 1), HB3 + FIR108b
// (wideBandSignal >= 2).
//
// Exactness notes:
//  * Valid factors {2,4,8,16,32} get shift s = log2(M) and rounds +-2^(s-1). Any other
//    factor in [2,32] with enable=1 keeps the kernel running with shift 0 and rounds 0:
//    boxcar sums M inputs per output for n outputs; wideBand 1 runs FIR108 over n outputs.
//    Such a kernel asks for up to M*n (boxcar) or 2n (FIR108) inputs; inputs beyond the n
//    samples of the block are 0. The output keeps the block length but is not a meaningful
//    signal.
//  * wideBand >= 2 with an invalid factor leaves the block undecimated.
//  * Every stage narrows to int16 by wrapping; sums wrap at 32 bits.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sdrplay {
namespace dsp {

// Sample buffers of one block. The kernels write xi[0..n) / xq[0..n) in place.
// xiReadable/xqReadable: how many int16 may be read starting at xi/xq. The decimator reads
// at most min(readable, n) of them; inputs beyond that are 0.
struct BlockBuffers {
    std::int16_t *xi = nullptr;
    std::int16_t *xq = nullptr;
    std::size_t xiReadable = 0;
    std::size_t xqReadable = 0;
};

// updateDecimation result for one channel (staged shift and rounds).
struct DecimationSetting {
    bool enable = false;      // ctrlParams.decimation.enable (copied at the next block start)
    std::uint8_t factor = 1;  // decimationFactor
    std::uint8_t wide = 0;    // wideBandSignal
    std::uint32_t shift = 0;  // 0 for factor 1 / invalid / disabled
    std::int32_t round[2] = {0, 0}; // [0] for sums >= 0, [1] for sums < 0
};
DecimationSetting updateDecimation(bool enable, std::uint8_t factor, std::uint8_t wide);
bool isValidDecimationFactor(std::uint8_t factor); // {2,4,8,16,32}

class UserDecimator {
public:
    // Channel init: rings zeroed, active setting {0,1,0}.
    void init();
    // Applied at a block start after updateDecimation (a staged decimation change).
    void setActive(const DecimationSetting &s) { active_ = s; }
    const DecimationSetting &active() const { return active_; }
    bool enabled() const { return active_.enable; }
    // Runs the kernel for the active setting on n samples; the caller then
    // shifts n and firstSampleNum by active().shift. ionly: low-IF I-only variants.
    void run(const BlockBuffers &b, std::uint32_t n, bool reset, bool ionly);

private:
    // Filter history: the newest input is at w - 1, older ones below it (mod ring size).
    struct Ring {
        std::int32_t i[128];
        std::int32_t q[128];
        std::uint32_t w; // next write position
    };
    void boxcar(const BlockBuffers &b, std::uint32_t nOut, std::uint32_t len, std::int32_t rPos, std::int32_t rNeg,
                unsigned shift, bool ionly);
    void ringStage(const BlockBuffers &b, Ring &ring, std::uint32_t mask, const std::int32_t *h, std::uint32_t L,
                   unsigned shift, std::uint32_t nOut, bool ionly);
    // Same result as ringStage via a linear history + Kernels::firDecimate (SIMD tables).
    void ringStageLinear(const BlockBuffers &b, Ring &ring, std::uint32_t mask, const std::int32_t *h,
                         std::uint32_t L, unsigned shift, std::uint32_t nOut, bool ionly);
    void wide1(const BlockBuffers &b, std::uint32_t n, bool reset, bool ionly);
    void wide2(const BlockBuffers &b, std::uint32_t n, bool reset, bool ionly);

    DecimationSetting active_{};
    Ring hb1_{};      // 32 entries
    Ring fir108_{};   // 128 entries; FIR108 and FIR108b
    Ring hb3_[4]{};
    std::vector<std::int32_t> lin_, linOut_; // ringStageLinear scratch (not state)
};

} // namespace dsp
} // namespace sdrplay
