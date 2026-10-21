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

#include "UsbScan.h"

#include <libusb.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

namespace rspinfo {

namespace {

#if defined(__linux__)
std::string readSysfs(const std::string &path)
{
    std::ifstream f(path);
    std::string s;
    if (!f || !std::getline(f, s)) return {};
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string linkBase(const std::string &path)
{
    char buf[512];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    const std::string s(buf);
    const auto slash = s.rfind('/');
    return slash == std::string::npos ? s : s.substr(slash + 1);
}

// In use = any interface of the active configuration bound to a driver:
// "usbfs" means a user-space program (an sdrplay_apiService, SoapySDR...) has
// claimed it; anything else is a kernel driver (e.g. msi2500). Sysfs only, no
// USB traffic and no libusb_kernel_driver_active (which hides usbfs claims).
void linuxInUse(RawDevice &d)
{
    const std::string base = "/sys/bus/usb/devices/" + d.portPath;
    if (readSysfs(base + "/idVendor").empty()) {
        d.inUse = InUse::Unknown;
        d.inUseDetail = "no sysfs entry " + base;
        return;
    }
    DIR *dir = ::opendir(base.c_str());
    if (!dir) {
        d.inUse = InUse::Unknown;
        d.inUseDetail = "cannot list " + base;
        return;
    }
    std::vector<std::string> bound;
    const std::string prefix = d.portPath + ":";
    while (dirent *e = ::readdir(dir)) {
        const std::string name = e->d_name;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string drv = linkBase(base + "/" + name + "/driver");
        if (!drv.empty()) bound.push_back("interface " + name + " bound to " + drv);
    }
    ::closedir(dir);
    std::sort(bound.begin(), bound.end());
    d.inUse = bound.empty() ? InUse::No : InUse::Yes;
    for (const auto &b : bound) d.inUseDetail += (d.inUseDetail.empty() ? "" : "; ") + b;
    if (bound.empty()) d.inUseDetail = "no interface claimed";
}
#endif

std::string usbErr(int rc) { return std::string(libusb_error_name(rc)); }
} // namespace

bool scanDevices(std::vector<RawDevice> &devs, std::string *error)
{
    devs.clear();
    libusb_context *ctx = nullptr;
    int rc = libusb_init(&ctx);
    if (rc != 0) {
        if (error) *error = "libusb_init failed: " + usbErr(rc);
        return false;
    }
    libusb_device **list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        if (error) *error = "libusb_get_device_list failed: " + usbErr(static_cast<int>(n));
        libusb_exit(ctx);
        return false;
    }
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device *dev = list[i];
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(dev, &desc) != 0) continue; // cached, no I/O
        if (desc.idVendor != sdrplay::device::kSdrplayVid) continue;

        RawDevice d;
        d.vid = desc.idVendor;
        d.pid = desc.idProduct;
        d.bcdDevice = desc.bcdDevice;
        d.iSerialIndex = desc.iSerialNumber;
        d.bus = libusb_get_bus_number(dev);
        d.address = libusb_get_device_address(dev);
        d.port = libusb_get_port_number(dev);
        std::uint8_t ports[8];
        const int np = libusb_get_port_numbers(dev, ports, static_cast<int>(sizeof ports));
        d.portPath = std::to_string(d.bus);
        for (int k = 0; k < np; ++k) d.portPath += (k ? "." : "-") + std::to_string(ports[k]);
        const bool known = sdrplay::device::isSdrplayPid(d.pid);

#if defined(__linux__)
        linuxInUse(d);
        // The kernel caches iSerial at enumeration: no USB traffic needed.
        if (d.iSerialIndex != 0) {
            const std::string s = readSysfs("/sys/bus/usb/devices/" + d.portPath + "/serial");
            if (!s.empty()) {
                d.serial = s;
                d.serialPresent = true;
                d.serialSource = "sysfs";
            }
        }
#else
        d.inUse = InUse::Unknown;
        d.inUseDetail = "not determinable on this OS";
#endif
        if (!known) { // not an RSP the service handles: descriptor data only
            devs.push_back(d);
            continue;
        }

        if (d.iSerialIndex == 0) {
            d.serialError = "iSerial absent";
        } else if (!d.serialPresent) {
            // Not cached by the OS: one GET_DESCRIPTOR string request on EP0.
            libusb_device_handle *h = nullptr;
            rc = libusb_open(dev, &h); // no claim, no configuration change
            if (rc != 0) {
                d.openError = "libusb_open: " + usbErr(rc);
                d.serialError = "device not opened";
            } else {
                d.opened = true;
                unsigned char buf[256] = {};
                rc = libusb_get_string_descriptor_ascii(h, d.iSerialIndex, buf, sizeof buf);
                if (rc > 0) {
                    d.serial.assign(reinterpret_cast<char *>(buf), static_cast<std::size_t>(rc));
                    d.serialPresent = true;
                    d.serialSource = "descriptor";
                } else {
                    d.serialError = "string descriptor read failed: " + usbErr(rc);
                }
                libusb_close(h);
            }
        }
        devs.push_back(d);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    std::sort(devs.begin(), devs.end(), [](const RawDevice &a, const RawDevice &b) {
        return a.bus != b.bus ? a.bus < b.bus : a.portPath < b.portPath;
    });
    return true;
}

} // namespace rspinfo
