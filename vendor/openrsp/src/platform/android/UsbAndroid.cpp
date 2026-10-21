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

// Android UsbEnumerator / UsbDevice on libusb-1.0 (built from source for the NDK,
// src/platform/android/Android.cmake), with INJECTED devices.
//
// An app has no access to /dev/bus/usb enumeration, so libusb runs with
// LIBUSB_OPTION_NO_DEVICE_DISCOVERY and every device comes from UsbManager: the app
// passes the UsbDeviceConnection fd to sdrplay_android_usb_attach() (JNI: Jni.cpp).
// The registry below is process-wide; every UsbEnumerator lists the registered devices
// in enumerate() and opens them with libusb_wrap_sys_device() on a dup() of the fd.
// Hotplug = the app's USB_DEVICE_ATTACHED/DETACHED broadcasts forwarded through
// attach/detach, delivered to setHotplugCallback() listeners on the calling thread.
//
// The libusb transfer/device glue mirrors src/platform/common/UsbLibusb.cpp (duplicated
// here instead of hooking into it).
#include "platform/Usb.h"
#include "sdrplay_android.h"

#include <libusb.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

namespace sdrplay {
namespace platform {

namespace {

// ------------------------------------------------------------------ device registry
struct Entry {
    int fd = -1;                // registry-owned dup of the app's fd
    int appFd = -1;             // the fd number the app passed (detach by fd)
    std::uint64_t generation = 0;
    UsbDeviceInfo info;         // descriptor fields; serial empty
    std::uint32_t claimed = 0;  // interfaces (bit n = interface n) claimed by one of our handles
};

struct Listener {
    std::uint16_t vid;
    std::shared_ptr<UsbEnumerator::HotplugFn> fn;
};

struct Registry {
    std::mutex m;
    std::map<std::uint64_t, Entry> devices; // key: info.opaqueId
    std::map<const void *, Listener> listeners;
    std::uint64_t nextGeneration = 1;
    std::uint64_t nextSynthetic = 0x10000; // ids for devices without bus/address
};

Registry &registry()
{
    static Registry *r = new Registry(); // never destroyed (used during static teardown)
    return *r;
}

int dupFd(int fd) { return fcntl(fd, F_DUPFD_CLOEXEC, 0); }

// usbfs: read() at the start of the file returns the cached device descriptor.
// Reads the device descriptor from the start of a usbfs fd, multi-byte fields in host order.
bool readDeviceDescriptor(int fd, libusb_device_descriptor &d)
{
    static_assert(sizeof(libusb_device_descriptor) == LIBUSB_DT_DEVICE_SIZE, "device descriptor size");
    const ssize_t n = pread(fd, &d, sizeof d, 0);
    if (n != static_cast<ssize_t>(sizeof d) || d.bLength != LIBUSB_DT_DEVICE_SIZE ||
        d.bDescriptorType != LIBUSB_DT_DEVICE)
        return false;
    d.bcdUSB = libusb_le16_to_cpu(d.bcdUSB);
    d.idVendor = libusb_le16_to_cpu(d.idVendor);
    d.idProduct = libusb_le16_to_cpu(d.idProduct);
    d.bcdDevice = libusb_le16_to_cpu(d.bcdDevice);
    return true;
}

bool parseDeviceName(const char *name, unsigned &bus, unsigned &addr)
{
    if (!name) return false;
    const char *p = std::strstr(name, "/dev/bus/usb/");
    if (!p) return false;
    return std::sscanf(p, "/dev/bus/usb/%u/%u", &bus, &addr) == 2 && bus < 256 && addr < 256;
}

void notify(bool arrived, const UsbDeviceInfo &info)
{
    std::vector<std::shared_ptr<UsbEnumerator::HotplugFn>> fns;
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lk(r.m);
        for (auto &kv : r.listeners)
            if (kv.second.vid == info.vid) fns.push_back(kv.second.fn);
    }
    for (auto &f : fns) (*f)(arrived, info);
}

bool stillAttached(std::uint64_t id, std::uint64_t generation)
{
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    auto it = r.devices.find(id);
    return it != r.devices.end() && it->second.generation == generation;
}

// ------------------------------------------------------------------ libusb glue
class LibusbTransfer final : public UsbTransfer {
public:
    LibusbTransfer(libusb_transfer *t, int numIso) : t_(t) { iso.resize(static_cast<std::size_t>(numIso)); }
    ~LibusbTransfer() override { libusb_free_transfer(t_); }
    LibusbTransfer(const LibusbTransfer &) = delete;
    LibusbTransfer &operator=(const LibusbTransfer &) = delete;

    libusb_transfer *raw() const { return t_; }

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

class AndroidUsbDevice final : public UsbDevice {
public:
    // h wraps `fd` (our dup, closed here after libusb_close: wrapped fds are kept by libusb).
    AndroidUsbDevice(libusb_device_handle *h, int fd, std::uint64_t id, std::uint64_t generation)
        : h_(h), fd_(fd), id_(id), generation_(generation)
    {
    }
    ~AndroidUsbDevice() override
    {
        if (h_) libusb_close(h_); // releases this handle's claims
        for (int i = 0; i < 32; ++i)
            if (held_ & (1u << i)) unreserve(i);
        if (fd_ >= 0) close(fd_);
    }
    int controlTransfer(std::uint8_t bmRequestType, std::uint8_t bRequest, std::uint16_t wValue,
                        std::uint16_t wIndex, std::uint8_t *data, std::uint16_t wLength,
                        std::uint32_t timeoutMs) override
    {
        return libusb_control_transfer(h_, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeoutMs);
    }
    // Every handle of a device wraps a dup() of the same UsbManager fd, and dup()s share one
    // set of usbfs claims: the kernel grants a second handle's claim of an interface another
    // handle is streaming on, and that handle's release cancels the other's transfers. Claims
    // are therefore arbitrated per device in the registry: an interface held by another handle
    // is BUSY, and a handle releases only what it claimed itself.
    int claimInterface(int iface) override
    {
        if (iface < 0 || iface >= 32) return LIBUSB_ERROR_INVALID_PARAM;
        const bool mine = (held_ & (1u << iface)) != 0;
        if (!mine && !reserve(iface)) return LIBUSB_ERROR_BUSY;
        const int rc = libusb_claim_interface(h_, iface);
        if (rc != 0 && !mine) unreserve(iface);
        return rc;
    }
    int releaseInterface(int iface) override
    {
        if (iface < 0 || iface >= 32 || !(held_ & (1u << iface))) return LIBUSB_ERROR_NOT_FOUND;
        const int rc = libusb_release_interface(h_, iface);
        unreserve(iface);
        return rc;
    }
    int setAltSetting(int iface, int alt) override { return libusb_set_interface_alt_setting(h_, iface, alt); }
    int getStringDescriptorAscii(std::uint8_t index, char *buf, int len) override
    {
        return libusb_get_string_descriptor_ascii(h_, index, reinterpret_cast<unsigned char *>(buf), len);
    }
    int getConfiguration(int *config) override { return libusb_get_configuration(h_, config); }
    int setConfiguration(int config) override { return libusb_set_configuration(h_, config); }
    // Android's USB host stack never binds a kernel driver to a vendor-class interface
    // the app talks to, and usbfs disconnect/connect ioctls are refused for apps.
    int kernelDriverActive(int iface) override { return libusb_kernel_driver_active(h_, iface); }
    int detachKernelDriver(int iface) override { return libusb_detach_kernel_driver(h_, iface); }
    int attachKernelDriver(int iface) override { return libusb_attach_kernel_driver(h_, iface); }
    int probeReopen() override
    {
        // The watchdog's probe re-opens the device node elsewhere; an app cannot. A device is
        // "present" while the app has not reported it detached (UsbManager broadcast).
        if (id_ == 0) return 0; // openWrapped(): no registry entry to consult
        return stillAttached(id_, generation_) ? 0 : kErrNoDevice;
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
    // Marks iface as claimed by this handle; false if another handle of the device holds it.
    bool reserve(int iface)
    {
        const std::uint32_t bit = 1u << iface;
        if (id_ != 0) { // openWrapped(): no registry entry, the kernel arbitrates alone
            Registry &r = registry();
            std::lock_guard<std::mutex> lk(r.m);
            auto it = r.devices.find(id_);
            if (it != r.devices.end() && it->second.generation == generation_) {
                if (it->second.claimed & bit) return false;
                it->second.claimed |= bit;
            }
        }
        held_ |= bit;
        return true;
    }
    void unreserve(int iface)
    {
        const std::uint32_t bit = 1u << iface;
        held_ &= ~bit;
        if (id_ == 0) return;
        Registry &r = registry();
        std::lock_guard<std::mutex> lk(r.m);
        auto it = r.devices.find(id_);
        if (it != r.devices.end() && it->second.generation == generation_) it->second.claimed &= ~bit;
    }

    libusb_device_handle *h_;
    int fd_;
    std::uint64_t id_;
    std::uint64_t generation_;
    std::uint32_t held_ = 0; // interfaces this handle claimed
};

class AndroidUsbEnumerator final : public UsbEnumerator {
public:
    explicit AndroidUsbEnumerator(libusb_context *ctx) : ctx_(ctx) {}
    ~AndroidUsbEnumerator() override
    {
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lk(r.m);
            r.listeners.erase(this);
        }
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
        // libusb itself has no hotplug here (no discovery); ours comes from UsbManager.
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
    void interruptEvents() override { libusb_interrupt_event_handler(ctx_); }

    std::vector<UsbDeviceInfo> enumerate(std::uint16_t vid, bool readSerial) override
    {
        std::vector<std::pair<UsbDeviceInfo, int>> found; // info, dup'd fd (readSerial only)
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lk(r.m);
            for (auto &kv : r.devices) {
                if (kv.second.info.vid != vid) continue;
                found.emplace_back(kv.second.info, readSerial ? dupFd(kv.second.fd) : -1);
            }
        }
        std::vector<UsbDeviceInfo> out;
        for (auto &f : found) {
            UsbDeviceInfo info = f.first;
            if (f.second >= 0) {
                if (info.iSerialNumber != 0) {
                    libusb_device_handle *h = nullptr;
                    if (libusb_wrap_sys_device(ctx_, static_cast<intptr_t>(f.second), &h) == 0) {
                        unsigned char buf[256];
                        int n = libusb_get_string_descriptor_ascii(h, info.iSerialNumber, buf, sizeof buf);
                        if (n > 0) info.serial.assign(reinterpret_cast<const char *>(buf), static_cast<std::size_t>(n));
                        libusb_close(h);
                    }
                }
                close(f.second);
            }
            out.push_back(info);
        }
        return out;
    }

    std::unique_ptr<UsbDevice> open(const UsbDeviceInfo &info, int *err) override
    {
        int fd = -1;
        std::uint64_t generation = 0;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lk(r.m);
            auto it = r.devices.find(info.opaqueId);
            if (it != r.devices.end()) {
                fd = dupFd(it->second.fd);
                generation = it->second.generation;
            }
        }
        if (fd < 0) {
            if (err) *err = LIBUSB_ERROR_NO_DEVICE;
            return nullptr;
        }
        libusb_device_handle *h = nullptr;
        const int rc = libusb_wrap_sys_device(ctx_, static_cast<intptr_t>(fd), &h);
        if (rc != 0) {
            close(fd);
            if (err) *err = rc;
            return nullptr;
        }
        return std::make_unique<AndroidUsbDevice>(h, fd, info.opaqueId, generation);
    }

    std::unique_ptr<UsbDevice> openWrapped(std::intptr_t sysDevice, int *err) override
    {
        const int fd = dupFd(static_cast<int>(sysDevice));
        if (fd < 0) {
            if (err) *err = LIBUSB_ERROR_INVALID_PARAM;
            return nullptr;
        }
        libusb_device_handle *h = nullptr;
        const int rc = libusb_wrap_sys_device(ctx_, static_cast<intptr_t>(fd), &h);
        if (rc != 0) {
            close(fd);
            if (err) *err = rc;
            return nullptr;
        }
        return std::make_unique<AndroidUsbDevice>(h, fd, 0, 0);
    }

    bool setHotplugCallback(std::uint16_t vid, HotplugFn fn) override
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lk(r.m);
        r.listeners.erase(this);
        if (fn) r.listeners[this] = Listener{vid, std::make_shared<HotplugFn>(std::move(fn))};
        return true;
    }

private:
    libusb_context *ctx_;
};

} // namespace

std::unique_ptr<UsbEnumerator> createUsbEnumerator()
{
    // Apps may not scan /dev/bus/usb nor open a netlink uevent socket: devices are
    // only ever wrapped from UsbManager fds.
    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) return nullptr;
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
    return std::make_unique<AndroidUsbEnumerator>(ctx);
}

} // namespace platform
} // namespace sdrplay

// ---------------------------------------------------------------------- C API
using sdrplay::platform::UsbDeviceInfo;

extern "C" SDRPLAY_ANDROID_API int sdrplay_android_usb_attach(int fd, const char *deviceName)
{
    using namespace sdrplay::platform;
    if (fd < 0) return -EBADF;
    libusb_device_descriptor d{};
    if (!readDeviceDescriptor(fd, d)) return -EBADF;
    const int mine = dupFd(fd);
    if (mine < 0) return -errno;

    UsbDeviceInfo info;
    info.vid = d.idVendor;
    info.pid = d.idProduct;
    info.bcdDevice = d.bcdDevice;
    info.iSerialNumber = d.iSerialNumber;
    unsigned bus = 0, addr = 0;
    const bool haveAddr = parseDeviceName(deviceName, bus, addr);
    info.busNumber = static_cast<std::uint8_t>(bus);
    info.deviceAddress = static_cast<std::uint8_t>(addr);
    info.portNumber = 0; // not visible to apps
    info.path = deviceName ? deviceName : "";

    std::vector<UsbDeviceInfo> gone;
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lk(r.m);
        info.opaqueId = haveAddr ? ((static_cast<std::uint64_t>(bus) << 8) | addr) : r.nextSynthetic++;
        auto it = r.devices.find(info.opaqueId);
        if (it != r.devices.end()) { // same bus/address re-attached: the old one is gone
            gone.push_back(it->second.info);
            close(it->second.fd);
            r.devices.erase(it);
        }
        Entry e;
        e.fd = mine;
        e.appFd = fd;
        e.generation = r.nextGeneration++;
        e.info = info;
        r.devices.emplace(info.opaqueId, std::move(e));
    }
    for (auto &g : gone) notify(false, g);
    notify(true, info);
    return 0;
}

extern "C" SDRPLAY_ANDROID_API int sdrplay_android_usb_detach(const char *deviceName, int fd)
{
    using namespace sdrplay::platform;
    std::vector<UsbDeviceInfo> gone;
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lk(r.m);
        for (auto it = r.devices.begin(); it != r.devices.end();) {
            const bool match = deviceName ? it->second.info.path == deviceName : it->second.appFd == fd;
            if (match) {
                gone.push_back(it->second.info);
                close(it->second.fd);
                it = r.devices.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto &g : gone) notify(false, g);
    return gone.empty() ? -ENOENT : 0;
}

extern "C" SDRPLAY_ANDROID_API int sdrplay_android_usb_count(void)
{
    using namespace sdrplay::platform;
    Registry &r = registry();
    std::lock_guard<std::mutex> lk(r.m);
    return static_cast<int>(r.devices.size());
}
