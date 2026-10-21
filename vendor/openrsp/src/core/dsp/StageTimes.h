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

// Wall time spent in each stage of StreamProcessor::processBlock, summed over every stream
// in the process, for live displays (the Android driver CPU card). Off by default: each
// read() arms the timing of the next kArmBlocks blocks, so nothing is timed once readers stop.
#pragma once

#include <cstdint>

namespace sdrplay {
namespace dsp {
namespace stages {

enum Stage : unsigned { Unpack, DcOffset, PowerAgc, DownConvert, Decimate, IqBalance, kCount };

constexpr std::uint32_t kArmBlocks = 40000; // several seconds at any sample rate

// "unpack", "dc", "agc", "downconvert", "decimate", "iq"
const char *key(Stage s);

struct Snapshot {
    std::uint64_t ns[kCount];
    std::uint64_t blocks; // timed blocks
};

// Running totals since process start; also arms the timing.
Snapshot read();

// One block: true if it is to be timed (consumes one armed block).
bool takeBlock();
void add(const std::uint64_t (&ns)[kCount]);

} // namespace stages
} // namespace dsp
} // namespace sdrplay
