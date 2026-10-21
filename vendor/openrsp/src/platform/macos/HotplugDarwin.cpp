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

// macOS hotplug monitor on libusb hotplug (see HotplugDarwin.h).
#include "HotplugDarwin.h"

#include <libusb.h>

#include <atomic>
#include <thread>

namespace sdrplay {
namespace platform {
namespace darwin {

bool libusbHotplugAvailable() { return libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) != 0; }

namespace {
class LibusbHotplugMonitor final : public HotplugMonitor {
public:
    LibusbHotplugMonitor(libusb_context *ctx, UsbEnumerator::HotplugFn fn) : ctx_(ctx), fn_(std::move(fn)) {}
    ~LibusbHotplugMonitor() override
    {
        stop_ = true;
        if (registered_) libusb_hotplug_deregister_callback(ctx_, handle_); // wakes the event thread
        if (thread_.joinable()) thread_.join();
        libusb_exit(ctx_);
    }
    LibusbHotplugMonitor(const LibusbHotplugMonitor &) = delete;
    LibusbHotplugMonitor &operator=(const LibusbHotplugMonitor &) = delete;

    bool start(std::uint16_t vid)
    {
        const int rc = libusb_hotplug_register_callback(
            ctx_, static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
            LIBUSB_HOTPLUG_ENUMERATE, vid, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, &onEvent, this,
            &handle_);
        if (rc != LIBUSB_SUCCESS) return false;
        registered_ = true;
        thread_ = std::thread([this] {
            while (!stop_) {
                timeval tv{0, 100000};
                libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
            }
        });
        return true;
    }

private:
    static int LIBUSB_CALL onEvent(libusb_context *, libusb_device *d, libusb_hotplug_event ev, void *user)
    {
        auto *self = static_cast<LibusbHotplugMonitor *>(user);
        UsbDeviceInfo info;
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(d, &desc) == 0) {
            info.vid = desc.idVendor;
            info.pid = desc.idProduct;
            info.bcdDevice = desc.bcdDevice;
            info.iSerialNumber = desc.iSerialNumber;
        }
        info.busNumber = libusb_get_bus_number(d);
        info.portNumber = libusb_get_port_number(d);
        info.deviceAddress = libusb_get_device_address(d);
        info.opaqueId = (static_cast<std::uint64_t>(info.busNumber) << 8) | info.deviceAddress;
        if (self->fn_ && !self->stop_) self->fn_(ev == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED, info);
        return 0; // stay registered
    }

    libusb_context *ctx_;
    UsbEnumerator::HotplugFn fn_;
    libusb_hotplug_callback_handle handle_ = 0;
    bool registered_ = false;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
} // namespace

std::unique_ptr<HotplugMonitor> startLibusbHotplug(std::uint16_t vid, UsbEnumerator::HotplugFn fn)
{
    if (!fn || !libusbHotplugAvailable()) return nullptr;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) return nullptr;
    auto m = std::make_unique<LibusbHotplugMonitor>(ctx, std::move(fn));
    if (!m->start(vid)) return nullptr; // destructor exits the context
    return m;
}

} // namespace darwin
} // namespace platform
} // namespace sdrplay
