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

#include "core/dsp/Packet.h"

#include "core/dsp/DspCommon.h"

namespace sdrplay {
namespace dsp {

namespace {

inline std::uint32_t rd32(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
inline std::uint16_t rd16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

// LE bitstream value i of width w (w <= 16) starting at byte p.
inline std::uint32_t bits(const std::uint8_t *p, unsigned i, unsigned w)
{
    const unsigned bit = i * w;
    const std::uint8_t *q = p + (bit >> 3);
    std::uint32_t v = std::uint32_t(q[0]) | std::uint32_t(q[1]) << 8;
    if ((bit & 7u) + w > 16) v |= std::uint32_t(q[2]) << 16; // never touch a byte past the value
    return (v >> (bit & 7u)) & ((1u << w) - 1u);
}

} // namespace

int samplesPerPacket(PacketFormat f)
{
    switch (f) {
    case PacketFormat::Iq14x252: return 252;
    case PacketFormat::Iq12x336: return 336;
    case PacketFormat::Iq10x384: return 384;
    case PacketFormat::Iq8x504:
    case PacketFormat::Real14x504: return 504;
    }
    return 0;
}

bool formatForSpp(int spp, bool iqFor504, PacketFormat &out)
{
    switch (spp) {
    case 252: out = PacketFormat::Iq14x252; return true;
    case 336: out = PacketFormat::Iq12x336; return true;
    case 384: out = PacketFormat::Iq10x384; return true;
    case 504: out = iqFor504 ? PacketFormat::Iq8x504 : PacketFormat::Real14x504; return true;
    default: return false;
    }
}

PacketHeader parsePacketHeader(const std::uint8_t *pkt)
{
    PacketHeader h;
    h.sampleNum = rd32(pkt);
    const std::uint32_t w = rd16(pkt + 4);
    h.gset = w & 0x3FFu;
    h.grToggle = static_cast<std::uint8_t>(w >> 13 & 1u);
    h.rfToggle = static_cast<std::uint8_t>(w >> 14 & 1u);
    h.fsToggle = static_cast<std::uint8_t>(w >> 15 & 1u);
    return h;
}

void unpackScalar(PacketFormat f, const std::uint8_t *pkt, std::int16_t *xi, std::int16_t *xq)
{
    const std::uint8_t *p = pkt + kPacketHeaderBytes;
    switch (f) {
    case PacketFormat::Iq14x252:
        for (unsigned k = 0; k < 252; ++k) {
            xi[k] = wrap16(std::int32_t(rd16(p + 4 * k)) << 2);
            xq[k] = wrap16(std::int32_t(rd16(p + 4 * k + 2)) << 2);
        }
        break;
    case PacketFormat::Iq12x336:
        for (unsigned k = 0; k < 336; ++k) {
            xi[k] = wrap16(std::int32_t(bits(p, 2 * k, 12)) << 4);
            xq[k] = wrap16(std::int32_t(bits(p, 2 * k + 1, 12)) << 4);
        }
        break;
    case PacketFormat::Iq10x384:
        // per 164-byte block, 128 10-bit values then a u32 of
        // 16 2-bit exponent codes (LSB first), one per 4 IQ pairs; shift {0:2,1:1,2:0,3:0}.
        for (unsigned b = 0; b < 6; ++b) {
            const std::uint8_t *blk = p + 164 * b;
            std::uint32_t codes = rd32(blk + 160);
            for (unsigned g = 0; g < 16; ++g, codes >>= 2) {
                const unsigned c = codes & 3u;
                const unsigned sh = c == 3 ? 0u : 2u - c;
                for (unsigned k = 0; k < 4; ++k) {
                    const unsigned j = 4 * g + k;
                    const std::int16_t vi = wrap16(std::int32_t(bits(blk, 2 * j, 10)) << 6);
                    const std::int16_t vq = wrap16(std::int32_t(bits(blk, 2 * j + 1, 10)) << 6);
                    xi[64 * b + j] = static_cast<std::int16_t>(asr(vi, sh));
                    xq[64 * b + j] = static_cast<std::int16_t>(asr(vq, sh));
                }
            }
        }
        break;
    case PacketFormat::Iq8x504:
        for (unsigned k = 0; k < 504; ++k) {
            xi[k] = wrap16(std::int32_t(static_cast<std::int8_t>(p[2 * k])) * 256);
            xq[k] = wrap16(std::int32_t(static_cast<std::int8_t>(p[2 * k + 1])) * 256);
        }
        break;
    case PacketFormat::Real14x504:
        for (unsigned k = 0; k < 504; ++k)
            xi[k] = wrap16(std::int32_t(rd16(p + 2 * k)) << 2);
        break;
    }
}

std::uint32_t bulkAlignmentOffset(const std::uint8_t *buf, std::size_t len)
{
    // Probe the sample counters (first header word) of packets 40, 41 and 42, assuming the
    // packets start at `offset`: equal differences mean the guess is right.
    constexpr std::size_t kProbe = 40 * kPacketBytes;
    constexpr std::uint32_t kHalfPacket = kPacketBytes / 2;
    if (!buf || len < kProbe + kHalfPacket + 2 * kPacketBytes + 4) return 0;
    const auto equalDiffs = [buf](std::size_t offset) {
        const std::uint8_t *p = buf + kProbe + offset;
        const std::uint32_t c0 = rd32(p), c1 = rd32(p + kPacketBytes), c2 = rd32(p + 2 * kPacketBytes);
        return c1 - c0 == c2 - c1;
    };
    if (equalDiffs(0)) return 0;
    if (equalDiffs(kHalfPacket)) return kHalfPacket;
    return 0;
}

} // namespace dsp
} // namespace sdrplay
