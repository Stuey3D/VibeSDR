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

// macOS clocks: mach_continuous_time for the monotonic clock (keeps counting
// across sleep, like Linux CLOCK_BOOTTIME; mach_absolute_time would stop), and
// gettimeofday for the wall clock (the TimedErrorInfo.time_us source).
#include "platform/Clock.h"

#include <mach/mach_time.h>
#include <sys/time.h>

namespace sdrplay {
namespace platform {

std::uint64_t realtimeMicros()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<std::uint64_t>(tv.tv_sec) * 1000000u + static_cast<std::uint64_t>(tv.tv_usec);
}

std::uint64_t monotonicNanos()
{
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t{};
        mach_timebase_info(&t);
        if (t.denom == 0) t.numer = t.denom = 1;
        return t;
    }();
    const std::uint64_t ticks = mach_continuous_time();
    if (tb.numer == tb.denom) return ticks; // x86_64: ticks are nanoseconds
    // arm64 (24 MHz: 125/3): split to avoid overflow of ticks * numer.
    const std::uint64_t whole = ticks / tb.denom, rem = ticks % tb.denom;
    return whole * tb.numer + rem * tb.numer / tb.denom;
}

} // namespace platform
} // namespace sdrplay
