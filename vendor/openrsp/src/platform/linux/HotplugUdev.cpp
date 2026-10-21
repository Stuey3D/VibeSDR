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

// Linux hotplug via libudev: a monitor on "udev" netlink events, subsystem
// usb / devtype usb_device, filtered on ID_VENDOR_ID. Runs its own thread and
// delivers add/remove to the UsbEnumerator::HotplugFn.
//
// The service does NOT react to hotplug events for its observable behaviour:
// it re-enumerates on every GetDevices and its watchdog probes each selected
// device with libusb_open once per second. This monitor does not replace that;
// it is the platform building block for hosts without that probe (Android:
// devices are injected) and for tools.
#include "platform/Usb.h"

#include <libudev.h>
#include <poll.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace sdrplay {
namespace platform {

bool udevHotplugAvailable()
{
    struct udev *u = udev_new();
    if (!u) return false;
    udev_unref(u);
    return true;
}

struct HotplugMonitor;
struct HotplugMonitorDeleter {
    void operator()(HotplugMonitor *m) const;
};

struct HotplugMonitor {
    struct udev *udev = nullptr;
    struct udev_monitor *mon = nullptr;
    std::uint16_t vid = 0;
    UsbEnumerator::HotplugFn fn;
    std::atomic<bool> stop{false};
    std::thread thread;

    ~HotplugMonitor()
    {
        stop = true;
        if (thread.joinable()) thread.join();
        if (mon) udev_monitor_unref(mon);
        if (udev) udev_unref(udev);
    }

    static unsigned hexAttr(struct udev_device *d, const char *key)
    {
        const char *v = udev_device_get_property_value(d, key);
        return v ? static_cast<unsigned>(std::strtoul(v, nullptr, 16)) : 0u;
    }
    static unsigned decAttr(struct udev_device *d, const char *key)
    {
        const char *v = udev_device_get_property_value(d, key);
        return v ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0u;
    }

    void run()
    {
        const int fd = udev_monitor_get_fd(mon);
        while (!stop) {
            struct pollfd p {};
            p.fd = fd;
            p.events = POLLIN;
            if (poll(&p, 1, 100) <= 0) continue;
            struct udev_device *d = udev_monitor_receive_device(mon);
            if (!d) continue;
            const char *action = udev_device_get_action(d);
            if (action && hexAttr(d, "ID_VENDOR_ID") == vid) {
                const bool add = std::strcmp(action, "add") == 0;
                const bool remove = std::strcmp(action, "remove") == 0;
                if (add || remove) {
                    UsbDeviceInfo info;
                    info.vid = static_cast<std::uint16_t>(vid);
                    info.pid = static_cast<std::uint16_t>(hexAttr(d, "ID_MODEL_ID"));
                    info.busNumber = static_cast<std::uint8_t>(decAttr(d, "BUSNUM"));
                    info.deviceAddress = static_cast<std::uint8_t>(decAttr(d, "DEVNUM"));
                    info.opaqueId = (static_cast<std::uint64_t>(info.busNumber) << 8) | info.deviceAddress;
                    if (const char *s = udev_device_get_property_value(d, "ID_SERIAL_SHORT")) info.serial = s;
                    if (const char *n = udev_device_get_devnode(d)) info.path = n;
                    fn(add, info);
                }
            }
            udev_device_unref(d);
        }
    }
};

void HotplugMonitorDeleter::operator()(HotplugMonitor *m) const { delete m; }

using HotplugMonitorPtr = std::unique_ptr<HotplugMonitor, HotplugMonitorDeleter>;

HotplugMonitorPtr startUdevHotplug(std::uint16_t vid, UsbEnumerator::HotplugFn fn)
{
    HotplugMonitorPtr m(new HotplugMonitor);
    m->udev = udev_new();
    if (!m->udev) return nullptr;
    m->mon = udev_monitor_new_from_netlink(m->udev, "udev");
    if (!m->mon) return nullptr;
    if (udev_monitor_filter_add_match_subsystem_devtype(m->mon, "usb", "usb_device") < 0 ||
        udev_monitor_enable_receiving(m->mon) < 0)
        return nullptr;
    m->vid = vid;
    m->fn = std::move(fn);
    HotplugMonitor *raw = m.get();
    m->thread = std::thread([raw] { raw->run(); });
    return m;
}

} // namespace platform
} // namespace sdrplay
