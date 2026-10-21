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

// rsp_info: list attached SDRplay RSPs.
//
// This header is the portable, USB-free half of the tool: the device record a
// scan produces, the per-device analysis (model, ROM-loader state, firmware
// image) and the text / JSON formatting. The libusb scan lives in UsbScan.cpp;
// tests drive everything here with fake device lists.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/device/Models.h"

namespace rspinfo {

enum class InUse { No, Yes, Unknown };

// What the USB scan learnt about one device. Everything the formatter shows
// comes from here plus analyse(); tests construct these by hand.
struct RawDevice {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::uint16_t bcdDevice = 0;
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    std::uint8_t port = 0;   // libusb_get_port_number
    std::string portPath;    // "1-1", "3-2.4" (Linux sysfs name)
    std::uint8_t iSerialIndex = 0;

    bool serialPresent = false; // false: iSerial 0 or unreadable
    std::string serial;
    std::string serialSource;   // "sysfs", "descriptor"
    std::string serialError;

    bool opened = false;
    std::string openError;

    InUse inUse = InUse::Unknown;
    std::string inUseDetail; // e.g. "interface 1-1:1.0 bound to usbfs"
};

// Models (VID, PID filter, hwVer, names) come from core/device/Models.h and the
// loader-state firmware image from core/firmware/FirmwareCatalog.h.

// --------------------------------------------------------------- analysis
struct Analysis {
    const sdrplay::device::ModelInfo *model = nullptr; // nullptr: PID not in the service's filter
    bool romLoader = false;
    std::string firmwareImage; // image the service would upload (loader state)
};

Analysis analyse(const RawDevice &d);

const char *inUseName(InUse u);

// --------------------------------------------------------------- output
std::string formatText(const std::vector<RawDevice> &devs, bool verbose);
std::string formatJson(const std::vector<RawDevice> &devs, bool verbose);
std::string jsonEscape(const std::string &s);

} // namespace rspinfo
