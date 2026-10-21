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

// Host-side firmware upload procedure (see uploadFirmware in src/service/Firmware.cpp):
// vendor OUT control transfers bmRequestType 0x40, bRequest 0x44,
// wValue = byte offset, wIndex = 0, chunks of at most 4096 bytes, 1000 ms
// timeout. Followed by resetBridgeCpu (register 8 = 0x80, then request 0x40)
// and a <= 15 s wait for re-enumeration.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sdrplay {
namespace platform {
class UsbDevice;
}
namespace firmware {

constexpr std::uint8_t kFwRequestType = 0x40;
constexpr std::uint8_t kFwRequest = 0x44;
constexpr std::size_t kFwChunk = 0x1000;
constexpr std::uint32_t kFwTimeoutMs = 1000;
constexpr std::uint32_t kReenumerateTimeoutMs = 15000;

// Returns 0 on success, 1 on the first failed transfer.
int download(platform::UsbDevice &usb, const std::uint8_t *image, std::size_t len);

} // namespace firmware
} // namespace sdrplay
