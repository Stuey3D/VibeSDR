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

// Device packet parsing and sample unpacking (RSP1B / MSi2500-compatible bridge).
//
// The stream is a sequence of 1024-byte packets (iso and bulk alike): a 16-byte
// header + a 1008-byte payload. Header parsing, one unpacker per payload format (252, 336,
// 384, 504 IQ, 504 real) and the unpacker selection by samples per packet.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sdrplay {
namespace dsp {

// Payload formats (bridge reg 7 picks the device side; see Rsp1bCalc fsState).
enum class PacketFormat : std::uint8_t {
    Iq14x252 = 0,  // reg7 0x14/0x94: 252 x {int16 I, int16 Q}, value = (int16)(raw << 2)
    Iq12x336 = 1,  // reg7 0x05/0x85: 672 x 12-bit LE bitstream, (int16)(v << 4)
    Iq10x384 = 2,  // reg7 0xA5: 6 x 164-byte blocks, 10-bit values + 2-bit exponent codes
    Iq8x504 = 3,   // reg7 0xC94: 504 x {int8 I, int8 Q}, (int16)(v << 8)
    Real14x504 = 4 // reg7 0x414/0x494: 504 x int16 real, (int16)(raw << 2), xq untouched
};
constexpr int kPacketFormatCount = 5;

int samplesPerPacket(PacketFormat f);

// Unpacker choice: by samples per packet; for 504 the IQ8 unpacker iff
// (RSPduo not in single-tuner mode) || ifType == 0 (the CURRENT channel ifType), else
// real. Returns false for an spp that is not recognised (the caller then leaves the
// buffers untouched).
bool formatForSpp(int spp, bool iqFor504, PacketFormat &out);
// RSP1B convenience: iqFor504 = (ifType == 0).
inline bool formatForSppRsp1b(int spp, int ifType, PacketFormat &out) { return formatForSpp(spp, ifType == 0, out); }

struct PacketHeader {
    std::uint32_t sampleNum = 0; // ADC sample count of the first sample (wraps at 2^32)
    std::uint32_t gset = 0;      // u16 bits 0..9 (gain readback)
    std::uint8_t grToggle = 0;   // bit 13
    std::uint8_t rfToggle = 0;   // bit 14
    std::uint8_t fsToggle = 0;   // bit 15
};

PacketHeader parsePacketHeader(const std::uint8_t *pkt);

// Unpack one packet's payload into xi[0..spp) and xq[0..spp) (Real14x504 writes xi only).
// Scalar reference; Kernels.h routes through the dispatch table.
void unpackScalar(PacketFormat f, const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq);

// Bulk transfer alignment (first bulk completion after streamInit): the byte offset at
// which packets start in the buffer, 0 or half a packet (512). Decided from the sample
// counters of packets 40..42: equal differences at offset 0 -> 0, else equal differences at
// offset 512 -> 512, else 0. A buffer too short to hold the probed counters gives 0.
std::uint32_t bulkAlignmentOffset(const std::uint8_t *buf, std::size_t len);

} // namespace dsp
} // namespace sdrplay
