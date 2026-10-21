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

// Shared constants and exact-arithmetic helpers for the RSP stream path
// (src/core/dsp). Pure, portable C++17: no OS headers.
//
// Numeric conventions: integers are 32-bit two's
// complement, `>>` is arithmetic, int16 narrowing WRAPS, except where a stage
// saturates explicitly (DC correction). FIR sums are 32-bit, so a
// sum that overflows wraps modulo 2^32 (part of the sample-stream contract);
// we get that by summing exactly in
// int64 and truncating (Z/2^32 is a ring, so the result does not depend on the
// order of the additions). Doubles/floats must be evaluated without contraction
// (-ffp-contract=off, set for sdrplay_core in CMakeLists.txt).
//
// Implementation-defined behaviour relied on (true for GCC, Clang and MSVC on every
// target we build): right shift of a negative int is arithmetic, and converting an
// out-of-range integer to a signed type wraps modulo 2^N. Both are guaranteed by C++20.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sdrplay {
namespace dsp {

constexpr std::uint32_t kPacketBytes = 1024;        // one device packet
constexpr std::uint32_t kPacketHeaderBytes = 16;    // header in front of the 1008-byte payload
constexpr std::uint32_t kMaxBlockSamples = 16128;   // 504 x 32: the xi/xq capacity of one shm slot (32256 bytes)
constexpr std::uint32_t kMaxPacketsPerBlock = 32;
constexpr std::uint32_t kStreamReadBytes = 384 * kPacketBytes; // one USB stream read (384 packets)

inline std::int16_t wrap16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(v))); }
inline std::int32_t wrap32(std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(v))); }
inline std::int16_t sat16(std::int32_t v) { return v > 32767 ? std::int16_t(32767) : v < -32768 ? std::int16_t(-32768) : static_cast<std::int16_t>(v); }
inline std::int32_t asr(std::int32_t v, unsigned s) { return v >> s; } // s < 32

// "Q15 round": (acc + 0x4000) >> shift, acc wrapped to 32 bits first.
inline std::int32_t q15Round(std::int64_t acc, unsigned shift = 15) { return asr(wrap32(acc + 0x4000), shift); }

// Floating-point to integer conversions with fully defined results.
//
// A plain C++ cast of a NaN or out-of-range value is undefined. These helpers truncate
// toward zero and give a fixed result for NaN and out-of-range inputs, so values computed
// from user-controlled parameters are the same on every architecture.
//
// truncToI32: in-range values truncated; NaN or a result outside int32 gives INT32_MIN.
// A float argument converts to double exactly, so this also serves float inputs.
inline std::int32_t truncToI32(double v)
{
    if (!(v > -2147483649.0 && v < 2147483648.0)) return INT32_MIN;
    return static_cast<std::int32_t>(v);
}

// truncToI64: as truncToI32 for int64; NaN or out of range gives INT64_MIN.
inline std::int64_t truncToI64(double v)
{
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return INT64_MIN;
    return static_cast<std::int64_t>(v);
}

// truncToU32: the low 32 bits of truncToI64 (an unsigned 32-bit result that wraps instead
// of saturating; NaN or out of range gives 0).
inline std::uint32_t truncToU32(double d) { return static_cast<std::uint32_t>(truncToI64(d)); }

// truncToWrap16: v truncated toward zero and wrapped to int16 (modulo 2^16); NaN or a value
// outside int64 gives 0.
inline std::int16_t truncToWrap16(double v)
{
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(truncToI64(v)));
}

} // namespace dsp
} // namespace sdrplay
