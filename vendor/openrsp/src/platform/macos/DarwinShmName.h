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

// POSIX shm object names on macOS.
//
// Darwin limits shm names to PSHMNAMLEN = 31 characters (32 -> ENAMETOOLONG).
// The IPC names never reach it: the longest is 26 characters
// (Glbl\sdrSrvDv00_mHrtbtRSem), so they are used UNCHANGED. Only names longer
// than 31 characters - which exist only in our unit tests (per-process test
// prefixes, "sdrplay_unittest_<pid>_<n>_x") - are mapped deterministically to
// 31 characters: the first 18 characters, '~', and 12 hex digits of the
// FNV-1a-64 hash of the full name. Both our service and our client go through
// the same mapping, so they still meet.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sdrplay {
namespace platform {
namespace darwin_shm {

inline std::string kernelShmName(const std::string &name)
{
    constexpr std::size_t kMax = 31; // PSHMNAMLEN
    if (name.size() <= kMax) return name;
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : name) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    static const char hex[] = "0123456789abcdef";
    std::string out = name.substr(0, 18);
    out += '~';
    for (int i = 11; i >= 0; --i) out += hex[(h >> (4 * i)) & 0xf];
    return out; // 18 + 1 + 12 = 31
}

} // namespace darwin_shm
} // namespace platform
} // namespace sdrplay
