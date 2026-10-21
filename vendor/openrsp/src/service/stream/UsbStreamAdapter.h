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

// device::StreamTransport (the model's seam, core/device/Device.h) implemented
// with the service's libusb stream transport. The model issues SET_INTERFACE
// itself through UsbDevice::setAltSetting, so start(alt) only selects the
// transfer geometry before stream init.
//
// Wiring (service): one UsbStreamAdapter per opened device, built
// on the device's platform::UsbDevice and handed to DeviceContext::stream;
// a single UsbEventThread on the service's UsbEnumerator must be running
// (created by the first device scan) for completions to happen.
// Hooks::deviceRemoved fires on the USB event thread when the device is gone.
#pragma once

#include "core/device/Device.h"
#include "service/stream/StreamTransport.h"

namespace sdrplay {
namespace service {
namespace stream {

class UsbStreamAdapter final : public device::StreamTransport {
public:
    UsbStreamAdapter(platform::UsbDevice *usb, stream::StreamTransport::Hooks hooks = {})
        : tr_(usb, std::move(hooks))
    {
    }

    int start(std::uint8_t alt) override
    {
        tr_.selectAlt(alt);
        return tr_.streamInit();
    }
    void stop() override
    {
        tr_.wakeReaders();
        tr_.streamUninit();
    }
    void flush() override { tr_.flush(); }
    // The protocol asks for a device reset at init on some USB function addresses. There
    // is no portable USB reset (and on iPadOS a reset re-enumerates the device under a new
    // service), so the reset is skipped and Init carries on.
    int resetDevice() override { return 0; }
    int read(std::uint8_t *dst, std::size_t maxPackets, std::uint32_t timeoutMs) override
    {
        return tr_.readPackets(dst, maxPackets, timeoutMs);
    }

    long availablePackets() override { return static_cast<long>(tr_.available() / stream::kPacketBytes); }

    stream::StreamTransport &transport() { return tr_; }

private:
    stream::StreamTransport tr_;
};

} // namespace stream
} // namespace service
} // namespace sdrplay
