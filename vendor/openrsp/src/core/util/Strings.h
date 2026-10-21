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

// Small portable string helpers (no POSIX strcasecmp on MSVC).
#pragma once

#include <cstddef>

namespace sdrplay {
namespace util {

inline int asciiLower(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

// strcasecmp in the "C" locale (serial number comparison).
inline int strCaseCmp(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        const int ca = asciiLower(static_cast<unsigned char>(*a));
        const int cb = asciiLower(static_cast<unsigned char>(*b));
        if (ca != cb || ca == 0) return ca - cb;
    }
}

// strnlen (not in C++17 <cstring>).
inline std::size_t strLenBounded(const char *s, std::size_t max)
{
    std::size_t n = 0;
    while (n < max && s[n]) ++n;
    return n;
}

} // namespace util
} // namespace sdrplay
