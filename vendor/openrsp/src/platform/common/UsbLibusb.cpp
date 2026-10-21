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

// libusb-1.0 implementation of UsbEnumerator / UsbDevice (portable: Linux,
// Windows/WinUSB, macOS/IOKit, Android via libusb_wrap_sys_device).
// Hotplug is OS-specific (see setHotplugCallback).
#include "platform/Usb.h"

// <winsock2.h> (struct timeval) must precede <libusb.h>, which pulls in <windows.h>
// (MinGW: "Please include winsock2.h before windows.h").
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/time.h>
#endif
#include <libusb.h>

#include <cstdio>
#include <memory>

namespace sdrplay {
namespace platform {

#if defined(SDRPLAY_HAVE_LIBUDEV)
// linux/HotplugUdev.cpp
bool udevHotplugAvailable();
struct HotplugMonitor;
struct HotplugMonitorDeleter {
    void operator()(HotplugMonitor *m) const;
};
using HotplugMonitorPtr = std::unique_ptr<HotplugMonitor, HotplugMonitorDeleter>;
HotplugMonitorPtr startUdevHotplug(std::uint16_t vid, UsbEnumerator::HotplugFn fn);
#endif

namespace {

// UsbTransfer backed by one libusb_transfer (allocated with room for
// `numIso` iso packet descriptors, like libusb_alloc_transfer(numIso)).
class LibusbTransfer final : public UsbTransfer {
public:
    LibusbTransfer(libusb_transfer *t, int numIso) : t_(t) { iso.resize(static_cast<std::size_t>(numIso)); }
    ~LibusbTransfer() override { libusb_free_transfer(t_); }
    LibusbTransfer(const LibusbTransfer &) = delete;
    LibusbTransfer &operator=(const LibusbTransfer &) = delete;

    libusb_transfer *raw() const { return t_; }

    // Runs on the thread inside libusb_handle_events_timeout().
    static void LIBUSB_CALL onComplete(libusb_transfer *lt)
    {
        auto *x = static_cast<LibusbTransfer *>(lt->user_data);
        x->status = static_cast<int>(lt->status);
        x->actualLength = lt->actual_length;
        const int n = lt->num_iso_packets < x->numIsoPackets() ? lt->num_iso_packets : x->numIsoPackets();
        for (int i = 0; i < n; ++i) {
            const libusb_iso_packet_descriptor &d = lt->iso_packet_desc[i];
            UsbIsoPacketResult &r = x->iso[static_cast<std::size_t>(i)];
            r.length = d.length;
            r.actualLength = d.actual_length;
            r.status = static_cast<int>(d.status);
        }
        if (x->callback) x->callback(*x);
    }

private:
    libusb_transfer *t_;
};

class LibusbDevice final : public UsbDevice {
public:
    explicit LibusbDevice(libusb_device_handle *h) : h_(h) {}
    ~LibusbDevice() override
    {
        if (h_) libusb_close(h_);
    }
    int controlTransfer(std::uint8_t bmRequestType, std::uint8_t bRequest, std::uint16_t wValue,
                        std::uint16_t wIndex, std::uint8_t *data, std::uint16_t wLength,
                        std::uint32_t timeoutMs) override
    {
        return libusb_control_transfer(h_, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeoutMs);
    }
    std::uint8_t *allocStreamMemory(std::size_t len) override { return libusb_dev_mem_alloc(h_, len); }
    void freeStreamMemory(std::uint8_t *p, std::size_t len) override
    {
        if (p) libusb_dev_mem_free(h_, p, len);
    }
    int claimInterface(int iface) override { return libusb_claim_interface(h_, iface); }
    int releaseInterface(int iface) override { return libusb_release_interface(h_, iface); }
    int setAltSetting(int iface, int alt) override { return libusb_set_interface_alt_setting(h_, iface, alt); }
    int getStringDescriptorAscii(std::uint8_t index, char *buf, int len) override
    {
        return libusb_get_string_descriptor_ascii(h_, index, reinterpret_cast<unsigned char *>(buf), len);
    }
    int getConfiguration(int *config) override { return libusb_get_configuration(h_, config); }
    int setConfiguration(int config) override { return libusb_set_configuration(h_, config); }
    int kernelDriverActive(int iface) override { return libusb_kernel_driver_active(h_, iface); }
    int detachKernelDriver(int iface) override { return libusb_detach_kernel_driver(h_, iface); }
    int attachKernelDriver(int iface) override { return libusb_attach_kernel_driver(h_, iface); }
    int probeReopen() override
    {
        // Heartbeat presence probe: libusb_open on the device
        // this handle belongs to.
        libusb_device_handle *h2 = nullptr;
        int rc = libusb_open(libusb_get_device(h_), &h2);
        if (rc == 0) libusb_close(h2);
        return rc;
    }

    std::unique_ptr<UsbTransfer> allocTransfer(int numIsoPackets) override
    {
        if (numIsoPackets < 0) return nullptr;
        libusb_transfer *t = libusb_alloc_transfer(numIsoPackets);
        if (!t) return nullptr;
        return std::unique_ptr<UsbTransfer>(new LibusbTransfer(t, numIsoPackets));
    }
    int submitTransfer(UsbTransfer &x) override
    {
        // Only transfers from allocTransfer() of a LibusbDevice reach here.
        auto &lx = static_cast<LibusbTransfer &>(x);
        libusb_transfer *t = lx.raw();
        t->dev_handle = h_;
        t->flags = 0;
        t->endpoint = x.endpoint;
        t->timeout = x.timeoutMs;
        t->buffer = x.buffer;
        t->length = x.length;
        t->callback = &LibusbTransfer::onComplete;
        t->user_data = &lx;
        if (x.type == UsbTransferType::Isochronous) {
            const int n = x.numIsoPackets();
            if (n <= 0 || x.isoPacketLength <= 0) return LIBUSB_ERROR_INVALID_PARAM;
            t->type = LIBUSB_TRANSFER_TYPE_ISOCHRONOUS;
            t->num_iso_packets = n;
            libusb_set_iso_packet_lengths(t, static_cast<unsigned>(x.isoPacketLength));
        } else {
            t->type = LIBUSB_TRANSFER_TYPE_BULK;
            t->num_iso_packets = 0;
        }
        x.status = kXferCompleted;
        x.actualLength = 0;
        return libusb_submit_transfer(t);
    }
    int cancelTransfer(UsbTransfer &x) override
    {
        return libusb_cancel_transfer(static_cast<LibusbTransfer &>(x).raw());
    }
    int bulkTransfer(std::uint8_t endpoint, std::uint8_t *data, int length, int *actual,
                     std::uint32_t timeoutMs) override
    {
        int got = 0;
        const int rc = libusb_bulk_transfer(h_, endpoint, data, length, &got, timeoutMs);
        if (actual) *actual = got;
        return rc;
    }

private:
    libusb_device_handle *h_;
};

std::uint64_t makeId(libusb_device *d)
{
    return (static_cast<std::uint64_t>(libusb_get_bus_number(d)) << 8) | libusb_get_device_address(d);
}

class LibusbEnumerator final : public UsbEnumerator {
public:
    explicit LibusbEnumerator(libusb_context *ctx) : ctx_(ctx) {}
    ~LibusbEnumerator() override
    {
#if defined(SDRPLAY_HAVE_LIBUDEV)
        hotplug_.reset();
#endif
        libusb_exit(ctx_);
    }

    UsbBackendInfo backendInfo() const override
    {
        UsbBackendInfo b;
        const libusb_version *v = libusb_get_version();
        b.major = v->major;
        b.minor = v->minor;
        b.micro = v->micro;
        b.apiVersion = LIBUSB_API_VERSION;
        b.hasHotplug = libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) != 0;
        b.supportsDetachKernelDriver = libusb_has_capability(LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER) != 0;
        return b;
    }

    int handleEvents(std::uint32_t timeoutMs) override
    {
        timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeoutMs / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeoutMs % 1000) * 1000);
        return libusb_handle_events_timeout(ctx_, &tv);
    }
    void interruptEvents() override
    {
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000105
        libusb_interrupt_event_handler(ctx_);
#endif
    }

    std::vector<UsbDeviceInfo> enumerate(std::uint16_t vid, bool readSerial) override
    {
        std::vector<UsbDeviceInfo> out;
        libusb_device **list = nullptr;
        ssize_t n = libusb_get_device_list(ctx_, &list);
        if (n < 0) return out;
        for (ssize_t i = 0; i < n; ++i) {
            libusb_device *d = list[i];
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(d, &desc) != 0 || desc.idVendor != vid) continue;
            UsbDeviceInfo info;
            info.vid = desc.idVendor;
            info.pid = desc.idProduct;
            info.bcdDevice = desc.bcdDevice;
            info.iSerialNumber = desc.iSerialNumber;
            info.busNumber = libusb_get_bus_number(d);
            info.portNumber = libusb_get_port_number(d);
            info.deviceAddress = libusb_get_device_address(d);
            info.opaqueId = makeId(d);
            char path[64];
            std::snprintf(path, sizeof path, "/dev/bus/usb/%03u/%03u", info.busNumber, info.deviceAddress);
            info.path = path;
            if (readSerial && desc.iSerialNumber != 0) {
                libusb_device_handle *h = nullptr;
                if (libusb_open(d, &h) == 0) {
                    unsigned char buf[256];
                    int r = libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, buf, sizeof buf);
                    if (r > 0) info.serial.assign(reinterpret_cast<const char *>(buf), static_cast<std::size_t>(r));
                    libusb_close(h);
                }
            }
            out.push_back(info);
        }
        libusb_free_device_list(list, 1);
        return out;
    }

    std::unique_ptr<UsbDevice> open(const UsbDeviceInfo &info, int *err) override
    {
        libusb_device **list = nullptr;
        ssize_t n = libusb_get_device_list(ctx_, &list);
        if (n < 0) {
            if (err) *err = static_cast<int>(n);
            return nullptr;
        }
        std::unique_ptr<UsbDevice> dev;
        int rc = LIBUSB_ERROR_NO_DEVICE;
        for (ssize_t i = 0; i < n; ++i) {
            if (makeId(list[i]) != info.opaqueId) continue;
            libusb_device_handle *h = nullptr;
            rc = libusb_open(list[i], &h);
            if (rc == 0) dev = std::make_unique<LibusbDevice>(h);
            break;
        }
        libusb_free_device_list(list, 1);
        if (!dev && err) *err = rc;
        return dev;
    }

    std::unique_ptr<UsbDevice> openWrapped(std::intptr_t sysDevice, int *err) override
    {
        libusb_device_handle *h = nullptr;
        int rc = libusb_wrap_sys_device(ctx_, sysDevice, &h);
        if (rc != 0) {
            if (err) *err = rc;
            return nullptr;
        }
        return std::make_unique<LibusbDevice>(h);
    }

    bool setHotplugCallback(std::uint16_t vid, HotplugFn fn) override
    {
#if defined(SDRPLAY_HAVE_LIBUDEV)
        hotplug_.reset();
        if (!fn) return true;
        hotplug_ = startUdevHotplug(vid, std::move(fn));
        return hotplug_ != nullptr;
#else
        // TODO(windows/macos/android): libusb_hotplug_register_callback where
        // LIBUSB_CAP_HAS_HOTPLUG, or OS notifications (Android: UsbManager broadcasts).
        (void)vid;
        (void)fn;
        return false;
#endif
    }

private:
    libusb_context *ctx_;
#if defined(SDRPLAY_HAVE_LIBUDEV)
    HotplugMonitorPtr hotplug_;
#endif
};

} // namespace

std::unique_ptr<UsbEnumerator> createUsbEnumerator()
{
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) return nullptr;
    // libusb log level 2 (warnings): libusb warnings on
    // stderr (journal), e.g. "libusb: warning [libusb_exit] ...".
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#else
    libusb_set_debug(ctx, LIBUSB_LOG_LEVEL_WARNING);
#endif
    return std::make_unique<LibusbEnumerator>(ctx);
}

} // namespace platform
} // namespace sdrplay
