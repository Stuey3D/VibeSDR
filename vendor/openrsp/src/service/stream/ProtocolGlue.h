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

// Glue between the RSP1B protocol engine's Io sink (core/device/rsp1b,
// Rsp1bProtocol.h) and the stream transport, for the service's Io
// implementation:
//
//   Io::control(t):        if isSetInterface(t) -> transport.setInterface(t.wValue)
//                          (libusb_set_interface_alt_setting, so the kernel
//                          switches the endpoint configuration; a raw
//                          SET_INTERFACE control transfer would not), else a
//                          normal control transfer.
//   Io::stream(op):        streamOp(transport, op)
//   Io::readFirstBlock(t): readFirstBlock(transport, t)
#pragma once

#include "core/device/rsp1b/Rsp1bProtocol.h"
#include "service/stream/StreamTransport.h"

namespace sdrplay {
namespace service {
namespace stream {

// Standard SET_INTERFACE (bmRequestType 0x01, bRequest 0x0B) on interface 0.
inline bool isSetInterface(const device::rsp1b::ControlTransfer &t)
{
    return t.bmRequestType == 0x01 && t.bRequest == device::rsp1b::kReqSetInterface && t.wIndex == 0;
}

// Stream init / uninit / flush. ResetDevice (a device reset the protocol asks
// for on some USB function addresses, see UsbStreamAdapter::resetDevice) is a
// no-op returning 0.
int streamOp(StreamTransport &tr, device::rsp1b::StreamOp op);

// The first stream read of Protocol::resyncToggles: kReadChunkBytes (384 packets), toggle
// bits 13/14/15 of the u16 at offset 4 of the last packet. Returns the
// StreamTransport::read result (ReadResult).
int readFirstBlock(StreamTransport &tr, device::rsp1b::ToggleBits &t);

// Header toggles of one 1024-byte packet.
device::rsp1b::ToggleBits packetToggles(const std::uint8_t *pkt);

} // namespace stream
} // namespace service
} // namespace sdrplay
