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

// Portable clocks on std::chrono (all OSes).
#include "platform/Clock.h"

#include <chrono>

namespace sdrplay {
namespace platform {

std::uint64_t realtimeMicros()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

std::uint64_t monotonicNanos()
{
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace platform
} // namespace sdrplay
