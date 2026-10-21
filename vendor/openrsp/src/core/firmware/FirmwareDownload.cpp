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

#include "core/firmware/FirmwareDownload.h"

#include "platform/Usb.h"

namespace sdrplay {
namespace firmware {

int download(platform::UsbDevice &usb, const std::uint8_t *image, std::size_t len)
{
    std::size_t off = 0;
    while (off < len) {
        std::size_t n = len - off;
        if (n > kFwChunk) n = kFwChunk;
        int r = usb.controlTransfer(kFwRequestType, kFwRequest, static_cast<std::uint16_t>(off), 0,
                                   const_cast<std::uint8_t *>(image + off), static_cast<std::uint16_t>(n),
                                   kFwTimeoutMs);
        if (r != static_cast<int>(n)) return 1; // the service logs the failure (Firmware.cpp)
        off += n;
    }
    return 0;
}

} // namespace firmware
} // namespace sdrplay
