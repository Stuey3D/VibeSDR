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

#include "service/stream/ProtocolGlue.h"


namespace sdrplay {
namespace service {
namespace stream {

using device::rsp1b::StreamOp;
using device::rsp1b::ToggleBits;

int streamOp(StreamTransport &tr, StreamOp op)
{
    switch (op) {
    case StreamOp::Init:
        return tr.streamInit();
    case StreamOp::Uninit:
        return tr.streamUninit();
    case StreamOp::Flush:
        return tr.flush();
    case StreamOp::ResetDevice:
        return 0;
    }
    return 1;
}

ToggleBits packetToggles(const std::uint8_t *pkt)
{
    const unsigned w = static_cast<unsigned>(pkt[4]) | static_cast<unsigned>(pkt[5]) << 8;
    ToggleBits t;
    t.gr = (w >> 13) & 1;
    t.rf = (w >> 14) & 1;
    t.fs = (w >> 15) & 1;
    return t;
}

int readFirstBlock(StreamTransport &tr, ToggleBits &t)
{
    PacketRing::Spans s;
    const int r = tr.acquire(kReadChunkBytes, s);
    if (r != kReadOk) return r;
    // The last packet of the chunk is contiguous unless a half-packet bulk
    // realignment shifted the ring phase, so gather its header.
    std::uint8_t hdr[16];
    const std::size_t at = kReadChunkBytes - kPacketBytes;
    for (std::size_t i = 0; i < sizeof hdr; ++i) {
        const std::size_t k = at + i;
        hdr[i] = k < s.n[0] ? s.p[0][k] : s.p[1][k - s.n[0]];
    }
    t = packetToggles(hdr);
    tr.release(kReadChunkBytes);
    return 0;
}

} // namespace stream
} // namespace service
} // namespace sdrplay
