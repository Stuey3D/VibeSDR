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

// Platform interface: clocks.
#pragma once

#include <cstdint>

namespace sdrplay {
namespace platform {

// Wall-clock microseconds since the Unix epoch (gettimeofday on POSIX; the clock
// behind TimedErrorInfo.time_us).
std::uint64_t realtimeMicros();
// Monotonic nanoseconds (arbitrary epoch) for timeouts and cadence measurement.
std::uint64_t monotonicNanos();

} // namespace platform
} // namespace sdrplay
