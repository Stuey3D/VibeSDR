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

#include "core/device/common/CommonRspProtocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sdrplay {
namespace device {
namespace common {

namespace {
// Every limit value is passed to "%d" as a 32-bit int: an unsigned field of
// 0xFFFFFFFF prints as -1.
int d32(long long v) { return static_cast<int>(static_cast<std::uint32_t>(v)); }
} // namespace

std::string limitMsg(const char *what, long long lo, long long hi, long long v)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s out of range (%d:%d) = %d", what, static_cast<int>(lo), static_cast<int>(hi),
                  d32(v));
    return buf;
}

std::string tunerLimitMsg(int tuner, const char *what, long long lo, long long hi, long long v)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, "Tuner%d: %s out of range (%d:%d) = %d", tuner, what, static_cast<int>(lo),
                  static_cast<int>(hi), d32(v));
    return buf;
}

} // namespace common
} // namespace device
} // namespace sdrplay
