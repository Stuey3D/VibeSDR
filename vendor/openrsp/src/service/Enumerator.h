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

// The service device table and USB enumeration:
//
//   scanUsbDevices()     = libusb scan
//                         with the exact PID filter, firmware upload for
//                         bcdDevice 0x0200 + re-enumeration wait (1000 ms, then
//                         500 ms steps, 15 s budget), per device: open,
//                         claim-test (DevAvail), iSerial, bus/port, hwVer.
//                         Max 16 devices.
//   refreshDeviceList() = merge the scan into the
//                         persistent 16-slot table (stable slots, removals).
//   getDevices()        = the DeviceT[] reply, with the variant code appended
//                         to serial numbers that carry one.
//   openDriver/closeDriver = open / close the slot's USB handle.
//
// Two tables exist: the scan table (refilled by every scan; numDevices() counts
// the entries filled by the latest scan) and the device table (slot index = "pos" = the Glbl\sdrSrvDv%02d
// number). The caller serialises access with mutex().
#pragma once

#include "service/Firmware.h"

#include "core/api/ApiTypes.h"
#include "core/ipc/Names.h"
#include "platform/Usb.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sdrplay {
namespace log {
class Logger;
}
namespace service {

class ServiceDevice;

// Device table entry.
struct DeviceRecord {
    std::uint32_t vid = 0;
    std::uint32_t pid = 0;
    std::uint32_t rev = 0;            // bcdDevice
    char serNo[64] = {};
    platform::UsbDeviceInfo usb;      // the bus device
    std::uint8_t devAvail = 0;
    std::uint8_t hwVer = 0;
    std::uint8_t tuner = 0;           // tuners in use
    std::uint8_t rspDuoMode = 0;
    std::int32_t missMaster = 0;      // heartbeat misses
    std::int32_t missSlave = 0;
    std::uint8_t hbDisabled = 0;
    std::uint8_t masterDisconnected = 0;
    std::uint8_t valid = 0;           // DeviceT.valid (1 for every scanned device)
    std::uint8_t bus = 0;
    std::uint8_t port = 0;
    bool kernelDriverDetached = false;

    bool empty() const { return serNo[0] == '\0'; }
};

struct DeviceSlot {
    DeviceRecord rec;
    std::unique_ptr<platform::UsbDevice> handle; // openDriver
    std::shared_ptr<ServiceDevice> dev;          // device object
};

class Enumerator {
public:
    struct Deps {
        platform::UsbEnumerator *usb = nullptr;  // null: no USB, no devices
        log::Logger *log = nullptr;              // "sdrplay_apiService" logger
        std::function<void(std::uint32_t)> sleepMs;           // firmware re-enumeration wait
        std::string firmwareDir;
        // The firmware set the ROM-loader upload sends (firmwareSetDir): "" = the images
        // directly in firmwareDir.
        std::string firmwareSet;
        std::string ownFirmwareDir; // own images; empty = firmwareDir + "/own"
        // Writes the service-global error record (ServiceCore::errlog).
        std::function<void(const char *file, const char *func, int line, const std::string &msg)> errlog;
    };

    explicit Enumerator(Deps deps);
    ~Enumerator();

    // Scans the bus and logs "device scan: err = %d numDevices = %d". 0 or 1.
    int scanUsbDevices();
    // Updates the device table from the last scan: 0, or 1 when the table is
    // full ("Device list full").
    int refreshDeviceList();
    // GetDevices: fills out[] (at most 16 entries, each cleared first) and *numDevs.
    sdrplay_api_ErrT getDevices(sdrplay_api_DeviceT *out, std::uint32_t *numDevs);

    // strcasecmp(serial, slot.serNo) over the 16 slots in order, empty slots
    // included (SelectDevice/ReleaseDevice). -1 if none matches.
    int findSerial(const char *serial) const;

    // Open, configuration 1, claim interface 0 (detach the kernel
    // driver on BUSY). 0 on success, 1 on failure (handle left empty).
    int openDriver(int slot);
    // Release interface 0, re-attach a detached kernel driver, close.
    void closeDriver(int slot);
    // EEPROM read through the slot's open handle.
    bool readEeprom(int slot, std::uint16_t addr, std::uint8_t *buf, std::uint16_t len);

    DeviceSlot &slot(int i) { return slots_[static_cast<std::size_t>(i)]; }
    const DeviceRecord &scanRecord(int i) const { return scan_[static_cast<std::size_t>(i)]; }
    int numDevices() const { return numDevices_; }
    std::recursive_mutex &mutex() { return m_; }

    // Wakes the (network-free) registration thread after each list update.
    void setOnListUpdated(std::function<void()> fn) { onListUpdated_ = std::move(fn); }
    // Called before a slot that still has a device object is cleared (unplug
    // while selected); we tear the device object down.
    void setOnSlotRemoved(std::function<void(int)> fn) { onSlotRemoved_ = std::move(fn); }

private:
    int scanDevicesLocked();
    // The device-table record of an attached bus device, or nullptr if it is not in the table.
    const DeviceRecord *knownRecord(const platform::UsbDeviceInfo &info) const;
    bool firmwareUploadAll(const std::vector<platform::UsbDeviceInfo> &list, bool *failed);
    void logBackendOnce();

    Deps d_;
    std::array<DeviceRecord, ipc::kMaxDeviceSlots> scan_{};
    std::array<DeviceSlot, ipc::kMaxDeviceSlots> slots_{};
    int numDevices_ = 0;
    bool backendLogged_ = false;
    std::function<void()> onListUpdated_;
    std::function<void(int)> onSlotRemoved_;
    mutable std::recursive_mutex m_;
};

} // namespace service
} // namespace sdrplay
