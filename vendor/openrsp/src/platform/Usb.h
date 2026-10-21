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

// Platform interface: USB device discovery, hotplug and device I/O.
//
// USB I/O is libusb-1.0 on every OS: WinUSB on
// Windows, IOKit on macOS, and on Android libusb_wrap_sys_device() on the fd
// obtained from UsbManager (no enumeration rights there: devices are *injected*
// with addWrappedDevice()). Discovery/hotplug differ per OS and sit behind
// UsbEnumerator: libudev + libusb on Linux, libusb hotplug or OS APIs elsewhere.
//
// Core device models only see UsbDevice (pure virtual), so they are unit-tested
// against a fake that records control transfers (byte-identity tests).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sdrplay {
namespace platform {

struct UsbDeviceInfo {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::uint16_t bcdDevice = 0;  // 0x0200 = ROM loader (needs firmware), else running
    std::uint8_t busNumber = 0;
    std::uint8_t portNumber = 0;
    std::uint8_t deviceAddress = 0;
    std::uint8_t iSerialNumber = 0;  // descriptor index (0 = none)
    std::string serial;              // iSerialNumber string; empty if not read / none
    std::string path;                // OS path, e.g. /dev/bus/usb/001/004 (informational)
    std::uint64_t opaqueId = 0;      // stable key while attached (bus/address on Linux)
};

// ---------------------------------------------------------------------------
// Asynchronous data transfers (libusb_transfer mirror, used by the service's
// stream transport, src/service/stream/). One UsbTransfer is allocated per
// in-flight slot with UsbDevice::allocTransfer() and reused across submits;
// its buffer is owned by the caller and must outlive every submission.
// Completion callbacks run on the thread that pumps UsbEnumerator::handleEvents()
// (the service's global libusb event thread). A transfer must not be
// destroyed while it is in flight (submitted and its callback not yet run).
enum class UsbTransferType : std::uint8_t { Isochronous = 1, Bulk = 2 };

// libusb_transfer_status values (identical numbering).
enum UsbTransferStatus : int {
    kXferCompleted = 0,
    kXferError = 1,
    kXferTimedOut = 2,
    kXferCancelled = 3,
    kXferStall = 4,
    kXferNoDevice = 5,
    kXferOverflow = 6,
};

struct UsbIsoPacketResult {
    std::uint32_t length = 0;       // requested (set-up) length
    std::uint32_t actualLength = 0; // bytes received for this packet
    int status = 0;                 // UsbTransferStatus
};

class UsbTransfer {
public:
    virtual ~UsbTransfer() = default;
    // --- set-up (before submitTransfer) ---
    UsbTransferType type = UsbTransferType::Bulk;
    std::uint8_t endpoint = 0x81;
    std::uint32_t timeoutMs = 0;
    std::uint8_t *buffer = nullptr; // caller-owned, `length` bytes
    int length = 0;
    int isoPacketLength = 0;        // iso: every packet gets this length (length = n * isoPacketLength)
    void (*callback)(UsbTransfer &) = nullptr;
    void *user = nullptr;
    // --- results (valid inside the callback) ---
    int status = kXferCompleted;
    int actualLength = 0;           // bulk; for iso see iso[]
    std::vector<UsbIsoPacketResult> iso; // sized numIsoPackets at allocation
    int numIsoPackets() const { return static_cast<int>(iso.size()); }
};

// Control-transfer and streaming access to one opened device.
class UsbDevice {
public:
    virtual ~UsbDevice() = default;
    // bmRequestType bit 7 selects direction. Returns bytes transferred (>= 0)
    // or a negative libusb-style error code.
    virtual int controlTransfer(std::uint8_t bmRequestType, std::uint8_t bRequest, std::uint16_t wValue,
                                std::uint16_t wIndex, std::uint8_t *data, std::uint16_t wLength,
                                std::uint32_t timeoutMs) = 0;
    virtual int claimInterface(int iface) = 0;
    virtual int releaseInterface(int iface) = 0;
    virtual int setAltSetting(int iface, int alt) = 0;

    // The calls below are the libusb operations the service needs for the
    // device scan, opening/closing a device and the heartbeat presence probe.
    // Defaults return LIBUSB_ERROR_NOT_SUPPORTED (-12) so fakes and stubs only
    // implement what they need.
    // ASCII string descriptor (libusb_get_string_descriptor_ascii): bytes
    // written (NUL-terminated) or a negative error.
    virtual int getStringDescriptorAscii(std::uint8_t index, char *buf, int len)
    {
        (void)index;
        (void)buf;
        (void)len;
        return kErrNotSupported;
    }
    virtual int getConfiguration(int *config)
    {
        if (config) *config = 1;
        return 0;
    }
    virtual int setConfiguration(int config)
    {
        (void)config;
        return kErrNotSupported;
    }
    // 1 = a kernel driver is bound, 0 = none, < 0 = error.
    virtual int kernelDriverActive(int iface)
    {
        (void)iface;
        return kErrNotSupported;
    }
    virtual int detachKernelDriver(int iface)
    {
        (void)iface;
        return kErrNotSupported;
    }
    virtual int attachKernelDriver(int iface)
    {
        (void)iface;
        return kErrNotSupported;
    }
    // Opens the underlying device a second time and closes it again: the
    // watchdog's presence probe (libusb_open on the stored device;
    // LIBUSB_ERROR_NO_DEVICE (-4) = unplugged). 0 = present.
    virtual int probeReopen() { return 0; }

    // --- streaming ---
    // Zero-copy stream memory (libusb_dev_mem_alloc: usbfs mmap on Linux/Android, so the kernel
    // fills the transfer buffers directly instead of copying into them). nullptr = unsupported:
    // the caller uses ordinary memory. Freed with freeStreamMemory before the device is closed.
    virtual std::uint8_t *allocStreamMemory(std::size_t len)
    {
        (void)len;
        return nullptr;
    }
    virtual void freeStreamMemory(std::uint8_t *p, std::size_t len)
    {
        (void)p;
        (void)len;
    }
    // libusb_alloc_transfer(numIsoPackets): nullptr when unsupported / out of memory.
    virtual std::unique_ptr<UsbTransfer> allocTransfer(int numIsoPackets)
    {
        (void)numIsoPackets;
        return nullptr;
    }
    // libusb_submit_transfer: 0 or a negative libusb error (kErrNoDevice when unplugged).
    virtual int submitTransfer(UsbTransfer &t)
    {
        (void)t;
        return kErrNotSupported;
    }
    // libusb_cancel_transfer: 0, or kErrNotFound when it is not in flight.
    virtual int cancelTransfer(UsbTransfer &t)
    {
        (void)t;
        return kErrNotSupported;
    }
    // Synchronous libusb_bulk_transfer (the bulk flush). Returns 0 or a negative
    // libusb error (kErrTimeout = -7); *actual = bytes received.
    virtual int bulkTransfer(std::uint8_t endpoint, std::uint8_t *data, int length, int *actual,
                             std::uint32_t timeoutMs)
    {
        (void)endpoint;
        (void)data;
        (void)length;
        (void)timeoutMs;
        if (actual) *actual = 0;
        return kErrNotSupported;
    }

    static constexpr int kErrIo = -1;            // LIBUSB_ERROR_IO
    static constexpr int kErrAccess = -3;        // LIBUSB_ERROR_ACCESS (access denied)
    static constexpr int kErrNoDevice = -4;      // LIBUSB_ERROR_NO_DEVICE
    static constexpr int kErrNotFound = -5;      // LIBUSB_ERROR_NOT_FOUND
    static constexpr int kErrBusy = -6;          // LIBUSB_ERROR_BUSY
    static constexpr int kErrTimeout = -7;       // LIBUSB_ERROR_TIMEOUT
    static constexpr int kErrPipe = -9;          // LIBUSB_ERROR_PIPE (stall)
    static constexpr int kErrNoMem = -11;        // LIBUSB_ERROR_NO_MEM
    static constexpr int kErrNotSupported = -12; // LIBUSB_ERROR_NOT_SUPPORTED
};

// Library facts the service logs on its first enumeration (libusb version, API version and
// whether hotplug is supported).
struct UsbBackendInfo {
    std::uint16_t major = 0, minor = 0, micro = 0;
    std::uint32_t apiVersion = 0; // LIBUSB_API_VERSION of the libusb headers built against
    bool hasHotplug = false;
    bool supportsDetachKernelDriver = false;
};

class UsbEnumerator {
public:
    virtual ~UsbEnumerator() = default;
    // Lists attached devices with the given VID. If readSerial is true the
    // device is opened briefly to read the iSerialNumber string (as the
    // service's device scan does); this touches the device.
    virtual std::vector<UsbDeviceInfo> enumerate(std::uint16_t vid, bool readSerial) = 0;
    // Opens a device found by enumerate(). nullptr on failure; *err = libusb error.
    virtual std::unique_ptr<UsbDevice> open(const UsbDeviceInfo &info, int *err = nullptr) = 0;
    // Android: wrap an fd from UsbManager (libusb_wrap_sys_device). Not supported elsewhere.
    virtual std::unique_ptr<UsbDevice> openWrapped(std::intptr_t sysDevice, int *err = nullptr) = 0;

    // Hotplug: callback(arrived, info). Returns false if unsupported.
    // TODO(linux): libudev monitor; elsewhere libusb hotplug / OS notifications.
    // Linux: libudev monitor on its own thread (callbacks run on that thread);
    // passing an empty fn stops it. Elsewhere: libusb hotplug / OS notifications (TODO).
    using HotplugFn = std::function<void(bool arrived, const UsbDeviceInfo &)>;
    virtual bool setHotplugCallback(std::uint16_t vid, HotplugFn fn) = 0;

    virtual UsbBackendInfo backendInfo() const { return UsbBackendInfo{}; }

    // One pass of the context's event handling (libusb_handle_events_timeout):
    // runs the completion callbacks of every device opened through this
    // enumerator. The service pumps it from one global thread with a
    // 500 ms timeout, started by the first device scan.
    virtual int handleEvents(std::uint32_t timeoutMs)
    {
        (void)timeoutMs;
        return UsbDevice::kErrNotSupported;
    }
    // Wakes a thread blocked in handleEvents() (libusb_interrupt_event_handler).
    virtual void interruptEvents() {}
};

// The platform's enumerator (libusb context owned inside). nullptr if USB is unavailable.
std::unique_ptr<UsbEnumerator> createUsbEnumerator();

} // namespace platform
} // namespace sdrplay
