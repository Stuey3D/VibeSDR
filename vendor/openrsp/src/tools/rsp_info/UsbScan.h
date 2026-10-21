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

// libusb-1.0 scan for rsp_info. Strictly read-only: cached descriptors, and
// libusb_open only when the serial string has to be fetched with a
// GET_DESCRIPTOR request (no set_configuration, claim, detach, reset,
// alt-setting or vendor request).
#pragma once

#include "RspInfo.h"

#include <string>
#include <vector>

namespace rspinfo {

// Fills devs with every VID 0x1df7 device whose PID the service accepts.
// false (and *error) only if libusb itself cannot be used.
bool scanDevices(std::vector<RawDevice> &devs, std::string *error);

} // namespace rspinfo
