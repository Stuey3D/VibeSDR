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

// USB stub for builds without libusb-1.0 (and for the stub compile check).
// The service then simply sees no devices. TODO(per OS): build libusb for
// Windows (WinUSB), macOS (IOKit) and Android (libusb_wrap_sys_device).
#include "platform/Usb.h"

namespace sdrplay {
namespace platform {

std::unique_ptr<UsbEnumerator> createUsbEnumerator() { return nullptr; }

} // namespace platform
} // namespace sdrplay
