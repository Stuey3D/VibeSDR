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

// Windows clocks (replaces common/ClockStd.cpp on Windows).
//
// realtimeMicros(): microseconds since boot from QueryPerformanceCounter (the Windows API
//   stamps TimedErrorInfo.time_us and sdrplay_api_GetTimeInMicroSecs this way, not with
//   wall-clock time as on Linux).
// monotonicNanos(): nanoseconds from QueryPerformanceCounter.
// Both split the counter into whole seconds and a remainder so the scaling cannot overflow.
#include "platform/Clock.h"
#include "platform/windows/WinCommon.h"

namespace sdrplay {
namespace platform {

namespace {
std::int64_t qpf()
{
    static const std::int64_t f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart > 0 ? v.QuadPart : 1;
    }();
    return f;
}

// Counter ticks scaled to `unitsPerSecond` without overflow.
std::uint64_t scaledCounter(std::uint64_t unitsPerSecond)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const std::uint64_t f = static_cast<std::uint64_t>(qpf());
    const std::uint64_t t = static_cast<std::uint64_t>(c.QuadPart);
    return (t / f) * unitsPerSecond + (t % f) * unitsPerSecond / f;
}
} // namespace

std::uint64_t realtimeMicros()
{
    return scaledCounter(1000000u);
}

std::uint64_t monotonicNanos()
{
    return scaledCounter(1000000000u);
}

} // namespace platform
} // namespace sdrplay
