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

// macOS USB hotplug through libusb's own hotplug API (Darwin/IOKit backend has
// LIBUSB_CAP_HAS_HOTPLUG; it is IOServiceAddMatchingNotification underneath).
//
// The macOS service does NOT use hotplug: it re-enumerates on every GetDevices
// and probes selected devices with libusb_open once per second. This monitor
// does not replace that; it is the building block behind
// UsbEnumerator::setHotplugCallback on macOS (not yet wired into
// common/UsbLibusb.cpp).
#pragma once

#include "platform/Usb.h"

#include <cstdint>
#include <memory>

namespace sdrplay {
namespace platform {
namespace darwin {

class HotplugMonitor {
public:
    virtual ~HotplugMonitor() = default; // deregisters and joins the event thread
};

// True if the libusb in use reports LIBUSB_CAP_HAS_HOTPLUG.
bool libusbHotplugAvailable();

// Starts a monitor on a private libusb context and its own event thread;
// callbacks run on that thread. Devices already attached are reported as
// arrivals first (LIBUSB_HOTPLUG_ENUMERATE). nullptr if unsupported.
// UsbDeviceInfo.serial stays empty (no device I/O inside libusb callbacks);
// opaqueId = bus << 8 | address, the key used by the libusb enumerator.
std::unique_ptr<HotplugMonitor> startLibusbHotplug(std::uint16_t vid, UsbEnumerator::HotplugFn fn);

} // namespace darwin
} // namespace platform
} // namespace sdrplay
