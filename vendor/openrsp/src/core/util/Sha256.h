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

// Portable SHA-256 (FIPS 180-4). Used to verify firmware images at load time.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sdrplay {
namespace util {

class Sha256 {
public:
    Sha256();
    void update(const void *data, std::size_t len);
    std::array<std::uint8_t, 32> finish();

    static std::array<std::uint8_t, 32> digest(const void *data, std::size_t len);
    static std::string hexDigest(const void *data, std::size_t len); // lowercase hex

private:
    void block(const std::uint8_t *p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t bufLen_ = 0;
    std::uint64_t totalLen_ = 0;
};

std::string toHex(const std::uint8_t *p, std::size_t n);

} // namespace util
} // namespace sdrplay
