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

#include "service/Enumerator.h"

#include "core/log/Here.h"
#include "service/ServiceDevice.h"

#include "core/device/Models.h"
#include "core/firmware/FirmwareCatalog.h"
#include "core/log/Logger.h"
#include "core/util/Strings.h"
#include "platform/Thread.h"
#include "service/Firmware.h"

#include <cstring>

namespace sdrplay {
namespace service {

using log::Level;

namespace {
constexpr int kMaxDevices = static_cast<int>(ipc::kMaxDeviceSlots);
constexpr std::uint32_t kFirstRescanWaitMs = 1000; // device scan: first wait after resetBridgeCpu
constexpr std::uint32_t kNextRescanWaitMs = 500;   // then every 500 ms ...
constexpr int kRescanBudgetMs = 15000;             // ... for at most 15 s
// Serial numbers with kVariantMark at kVariantMarkPos carry a variant code, which is
// reported in the last kVariantCodeLen characters of DeviceT.SerNo before its terminator.
constexpr std::size_t kVariantMarkPos = 9;
constexpr char kVariantMark = 'F';
constexpr std::uint16_t kVariantCodeEepromAddr = 0x11; // EEPROM address of the variant code
constexpr std::uint16_t kVariantCodeLen = 4;
constexpr std::size_t kVariantCodePos = SDRPLAY_MAX_SER_NO_LEN - kVariantCodeLen - 1;
static_assert(kVariantCodePos == 59, "variant code position in SerNo");
} // namespace

Enumerator::Enumerator(Deps deps) : d_(std::move(deps))
{
    if (!d_.sleepMs) d_.sleepMs = [](std::uint32_t ms) { platform::sleepMs(ms); };
}

Enumerator::~Enumerator() = default;

void Enumerator::logBackendOnce()
{
    if (backendLogged_ || !d_.usb || !d_.log) return;
    backendLogged_ = true;
    const platform::UsbBackendInfo b = d_.usb->backendInfo();
    d_.log->log(Level::Verbose, __func__, "libusb version %d.%d.%d (0x%08x)", b.major, b.minor, b.micro,
                static_cast<unsigned>(b.apiVersion));
    d_.log->log(Level::Verbose, __func__, "libusb does%s have hotplug capability", b.hasHotplug ? "" : "n't");
    d_.log->log(Level::Verbose, __func__, "libusb does%s support kernel detach capability",
                b.supportsDetachKernelDriver ? "" : "n't");
}

bool Enumerator::firmwareUploadAll(const std::vector<platform::UsbDeviceInfo> &list, bool *failed)
{
    const char *const fn = __func__;
    // Uploads the firmware to one ROM-loader device and restarts its CPU.
    auto uploadTo = [&](const platform::UsbDeviceInfo &info) -> bool {
        int err = 0;
        auto h = d_.usb->open(info, &err);
        if (!h) {
            if (d_.log) d_.log->log(Level::Error, fn, "libusb_open() %d", err);
            return false;
        }
        const int claimed = h->claimInterface(0);
        if (claimed != 0) {
            if (d_.log) d_.log->log(Level::Error, fn, "libusb_claim_interface() %d", claimed);
            return false;
        }
        const device::ModelInfo *m = device::modelByPid(info.pid);
        const std::uint8_t hwVer = m ? m->hwVer : 0;
        std::string why;
        std::string sha;
        auto img = loadFirmware(d_.firmwareSet, firmwareSetDir(d_.firmwareDir, d_.firmwareSet, d_.ownFirmwareDir),
                                hwVer, &why, &sha);
        bool uploaded = false;
        if (img.empty()) {
            if (d_.log) d_.log->log(Level::Error, fn, "%s", why.c_str());
        } else {
            if (d_.log)
                d_.log->log(Level::Verbose, fn, "using %s%sfirmware %s sha256 %s", d_.firmwareSet.c_str(),
                            d_.firmwareSet.empty() ? "" : " ", firmware::imageForHwVer(hwVer).fileName, sha.c_str());
            uploaded = uploadFirmware(*h, img.data(), img.size(), d_.log) == 0;
        }
        if (!uploaded) {
            if (d_.log) d_.log->log(Level::Error, fn, "firmware upload failed (%d)", hwVer);
            return false;
        }
        if (resetBridgeCpu(*h, 1, d_.log) != 0) {
            if (d_.log) d_.log->log(Level::Error, fn, "bridge CPU reset failed");
            return false;
        }
        h->releaseInterface(0);
        return true; // the handle is closed here
    };
    bool downloaded = false;
    for (const auto &info : list) {
        if (!device::isSdrplayPid(info.pid) || info.bcdDevice != device::kBcdRomLoader) continue;
        if (uploadTo(info))
            downloaded = true;
        else
            *failed = true;
    }
    return downloaded;
}

int Enumerator::scanDevicesLocked()
{
    if (!d_.usb) {
        // Portable builds / tests without a USB backend: an empty bus.
        numDevices_ = 0;
        return 0;
    }
    logBackendOnce();
    std::vector<platform::UsbDeviceInfo> list = d_.usb->enumerate(device::kSdrplayVid, false);
    if (list.empty()) {
        numDevices_ = 0;
        return 0;
    }
    int matched = 0;
    for (const auto &i : list)
        if (i.vid == device::kSdrplayVid && device::isSdrplayPid(i.pid)) ++matched;

    bool failed = false;
    const bool downloaded = firmwareUploadAll(list, &failed);
    if (matched == 1 && failed) {
        numDevices_ = 0;
        return 1;
    }
    int count = matched;
    if (downloaded) {
        // Wait for the re-enumeration with the new firmware (bcdDevice != 0x0200).
        std::uint32_t wait = kFirstRescanWaitMs;
        int remaining = kRescanBudgetMs;
        int found = 0;
        do {
            remaining -= static_cast<int>(wait);
            d_.sleepMs(wait);
            list = d_.usb->enumerate(device::kSdrplayVid, false);
            found = 0;
            for (const auto &info : list) {
                if (info.vid != device::kSdrplayVid || !device::isSdrplayPid(info.pid)) continue;
                const int k = found + 1;
                if (d_.log)
                    d_.log->log(Level::Verbose, __func__, "found device %d with fw=0x%04x after reset (%d)", k,
                                info.bcdDevice, remaining);
                if (info.bcdDevice == device::kBcdRomLoader) continue;
                int err = 0;
                auto h = d_.usb->open(info, &err);
                if (h) {
                    found = k;
                } else {
                    if (d_.log) d_.log->log(Level::Error, __func__, "libusb_open() %d", err);
                    if (err == platform::UsbDevice::kErrAccess) found = k; // a device we may not open still counts
                }
            }
            wait = kNextRescanWaitMs;
        } while (remaining > 0 && found < matched);
        count = found;
    }
    if (count > kMaxDevices) {
        if (d_.log)
            d_.log->log(Level::Warning, __func__,
                        "Found more devices than expected (%d) - will only evaluate first %d devices", count,
                        kMaxDevices);
        count = kMaxDevices;
    }
    if (count == 0) {
        numDevices_ = 0;
        return 0;
    }

    int idx = 0;
    for (const auto &info : list) {
        if (info.vid != device::kSdrplayVid || !device::isSdrplayPid(info.pid)) continue;
        if (idx >= count) break; // at most `count` (<= 16) devices are listed
        if (d_.log) d_.log->log(Level::Verbose, __func__, "vid=0x%.4x pid=0x%.4x", info.vid, info.pid);
        int err = 0;
        auto h = d_.usb->open(info, &err);
        if (!h) {
            if (d_.log) d_.log->log(Level::Error, __func__, "libusb_open() %d", err);
            // A device that is still attached but cannot be opened right now keeps the record
            // read when it was last opened (matched by its bus identity), as unavailable.
            if (const DeviceRecord *known = knownRecord(info)) {
                DeviceRecord &r = scan_[static_cast<std::size_t>(idx)];
                r = *known;
                r.usb = info;
                r.devAvail = 0;
                ++idx;
            }
            continue;
        }
        DeviceRecord &r = scan_[static_cast<std::size_t>(idx)];
        r = DeviceRecord{};
        r.usb = info;
        if (h->claimInterface(0) == 0) {
            r.devAvail = 1;
            h->releaseInterface(0);
        } else {
            r.devAvail = 0; // claimed elsewhere, e.g. by this service for a selected device
        }
        if (d_.log) d_.log->log(Level::Verbose, __func__, "desc.iSerialNumber = %x", info.iSerialNumber);
        const int serNoLen = static_cast<int>(sizeof r.serNo);
        const int sr = h->getStringDescriptorAscii(info.iSerialNumber, r.serNo, serNoLen);
        if (sr < 0) {
            if (d_.log) d_.log->log(Level::Error, __func__, "libusb_get_string_descriptor_ascii() %d", sr);
            numDevices_ = 0;
            return 1;
        }
        if (sr < serNoLen) r.serNo[sr] = '\0';
        r.vid = info.vid;
        r.pid = info.pid;
        r.rev = info.bcdDevice;
        r.bus = info.busNumber;
        r.port = info.portNumber;
        const device::ModelInfo *m = device::modelByPid(info.pid);
        r.hwVer = m ? m->hwVer : 0;
        if (d_.log)
            d_.log->log(Level::Verbose, __func__,
                        "Dev%d:vid=%04x pid=%04x rev=%04x serno=%s bus=%03d port=%03d devAvail=%d", idx, r.vid, r.pid,
                        info.bcdDevice, r.serNo, r.bus, r.port, r.devAvail);
        r.valid = 1;
        ++idx;
    }
    numDevices_ = idx;
    return 0;
}

const DeviceRecord *Enumerator::knownRecord(const platform::UsbDeviceInfo &info) const
{
    for (const auto &s : slots_) {
        const DeviceRecord &r = s.rec;
        if (!r.empty() && r.usb.opaqueId == info.opaqueId && r.vid == info.vid && r.pid == info.pid) return &r;
    }
    return nullptr;
}

int Enumerator::scanUsbDevices()
{
    std::lock_guard<std::recursive_mutex> lk(m_);
    const int err = scanDevicesLocked();
    if (d_.log) d_.log->log(Level::Verbose, __func__, "device scan: err = %d numDevices = %d", err, numDevices_);
    return err;
}

int Enumerator::refreshDeviceList()
{
    std::lock_guard<std::recursive_mutex> lk(m_);
    for (int i = 0; i < kMaxDevices; ++i) {
        const DeviceRecord &r = slots_[static_cast<std::size_t>(i)].rec;
        if (!r.empty() && d_.log)
            d_.log->log(Level::Verbose, __func__,
                        "IN Dev%d SerNo=%s HwVer=%d tuner=%.1x rspDuoMode=%.1x DevAvail=%d ", i, r.serNo, r.hwVer,
                        r.tuner, r.rspDuoMode, r.devAvail);
    }
    const int n = numDevices_ < kMaxDevices ? numDevices_ : kMaxDevices;
    for (int i = 0; i < n; ++i) {
        const DeviceRecord &r = scan_[static_cast<std::size_t>(i)];
        if (d_.log)
            d_.log->log(Level::Verbose, __func__, "Dev%d SerNo=%s HwVer=%d DevAvail=%d", i, r.serNo, r.hwVer,
                        r.devAvail);
    }
    // Remove table entries that are no longer on the bus.
    for (int i = 0; i < kMaxDevices; ++i) {
        DeviceSlot &s = slots_[static_cast<std::size_t>(i)];
        if (s.rec.empty()) continue;
        bool present = false;
        for (int k = 0; k < n && !present; ++k)
            present = util::strCaseCmp(scan_[static_cast<std::size_t>(k)].serNo, s.rec.serNo) == 0;
        if (present) continue;
        if (s.dev && onSlotRemoved_) onSlotRemoved_(i); // our addition: no leaked device object
        s.dev.reset();
        s.handle.reset();
        s.rec = DeviceRecord{};
        if (d_.log) d_.log->log(Level::Verbose, __func__, "Removed device at pos=%d", i);
    }
    // Update known devices, add new ones in the first free slot.
    for (int k = 0; k < n; ++k) {
        const DeviceRecord &sr = scan_[static_cast<std::size_t>(k)];
        bool known = false;
        for (int i = 0; i < kMaxDevices && !known; ++i) {
            DeviceRecord &r = slots_[static_cast<std::size_t>(i)].rec;
            if (r.empty() || util::strCaseCmp(sr.serNo, r.serNo) != 0) continue;
            r.valid = sr.valid;
            r.devAvail = sr.devAvail;
            r.usb = sr.usb;
            known = true;
        }
        if (known) continue;
        int freeSlot = -1;
        for (int i = 0; i < kMaxDevices && freeSlot < 0; ++i)
            if (slots_[static_cast<std::size_t>(i)].rec.empty()) freeSlot = i;
        if (freeSlot < 0) {
            if (d_.log) d_.log->log(Level::Verbose, __func__, "Device list full!");
            if (d_.errlog) d_.errlog(OPENRSP_HERE, "Device list full");
            return 1;
        }
        slots_[static_cast<std::size_t>(freeSlot)].rec = sr;
        if (d_.log) d_.log->log(Level::Verbose, __func__, "Added device at pos=%d", freeSlot);
    }
    for (int i = 0; i < kMaxDevices; ++i) {
        const DeviceRecord &r = slots_[static_cast<std::size_t>(i)].rec;
        if (!r.empty() && d_.log)
            d_.log->log(Level::Verbose, __func__,
                        "OUT Dev%d SerNo=%s HwVer=%d tuner=%.1x rspDuoMode=%.1x DevAvail=%d", i, r.serNo, r.hwVer,
                        r.tuner, r.rspDuoMode, r.devAvail);
    }
    if (onListUpdated_) onListUpdated_();
    return 0;
}

sdrplay_api_ErrT Enumerator::getDevices(sdrplay_api_DeviceT *out, std::uint32_t *numDevs)
{
    std::lock_guard<std::recursive_mutex> lk(m_);
    std::uint32_t n = 0;
    for (int i = 0; i < kMaxDevices; ++i) {
        const DeviceRecord &r = slots_[static_cast<std::size_t>(i)].rec;
        if (r.devAvail != 1) {
            // An in-use RSPduo master (rspDuoMode Master, one tuner free, not stalled) is
            // listed as a slave candidate with rspDuoMode Slave, the free tuner and the
            // master's sample rate. dev is non-null there too: the service's device object
            // address, opaque to the client. (No variant code is read for such
            // an entry.)
            const DeviceSlot &sl = slots_[static_cast<std::size_t>(i)];
            const ServiceDevice *sd = sl.dev.get();
            device::Device *model = sd ? const_cast<ServiceDevice *>(sd)->model() : nullptr;
            const ipc::DataRingProducer *ring = sd ? const_cast<ServiceDevice *>(sd)->channel().dataRing() : nullptr;
            if (r.hwVer != device::kHwRspDuo || r.rspDuoMode != sdrplay_api_RspDuoMode_Master ||
                (r.tuner & sdrplay_api_Tuner_Both) == sdrplay_api_Tuner_Both || r.masterDisconnected == 1 || !model ||
                (ring && ring->stalled()))
                continue;
            sdrplay_api_DeviceT &d = out[n];
            std::memset(static_cast<void *>(&d), 0, sizeof d);
            std::memcpy(d.SerNo, r.serNo, sizeof d.SerNo);
            d.hwVer = r.hwVer;
            d.valid = r.valid;
            d.tuner = static_cast<sdrplay_api_TunerSelectT>(sdrplay_api_Tuner_Both & ~r.tuner); // the free tuner
            d.rspDuoMode = sdrplay_api_RspDuoMode_Slave;
            d.rspDuoSampleFreq = model->rspDuoMasterSampleFreq();
            d.dev = const_cast<ServiceDevice *>(sd);
            ++n;
            continue;
        }
        sdrplay_api_DeviceT &d = out[n];
        std::memset(static_cast<void *>(&d), 0, sizeof d);
        std::memcpy(d.SerNo, r.serNo, sizeof d.SerNo);
        d.hwVer = r.hwVer;
        d.valid = r.valid;
        d.dev = nullptr;
        if (d.SerNo[kVariantMarkPos] == kVariantMark) {
            if (openDriver(i) != 0) {
                if (d_.log) d_.log->log(Level::Error, __func__, "Timeout expired for connection with the device");
                if (d_.errlog) d_.errlog(OPENRSP_HERE, "Failed to initialise correctly - NULL ptrs");
                return sdrplay_api_Fail;
            }
            readEeprom(i, kVariantCodeEepromAddr, reinterpret_cast<std::uint8_t *>(d.SerNo) + kVariantCodePos,
                       kVariantCodeLen);
            closeDriver(i);
            if (d_.log) {
                const char *v = d.SerNo + kVariantCodePos;
                const std::string variant(v, util::strLenBounded(v, sizeof d.SerNo - kVariantCodePos));
                d_.log->log(Level::Verbose, __func__, "Append variant code %s for serial number %s", variant.c_str(),
                            d.SerNo);
            }
        }
        d.rspDuoSampleFreq = 0.0;
        if (d.hwVer == device::kHwRspDuo) {
            d.tuner = sdrplay_api_Tuner_Both;
            d.rspDuoMode = static_cast<sdrplay_api_RspDuoModeT>(sdrplay_api_RspDuoMode_Single_Tuner |
                                                                sdrplay_api_RspDuoMode_Dual_Tuner | sdrplay_api_RspDuoMode_Master);
        } else {
            d.tuner = sdrplay_api_Tuner_A;
            d.rspDuoMode = sdrplay_api_RspDuoMode_Unknown;
        }
        ++n;
    }
    *numDevs = n;
    return sdrplay_api_Success;
}

int Enumerator::findSerial(const char *serial) const
{
    std::lock_guard<std::recursive_mutex> lk(m_);
    for (int i = 0; i < kMaxDevices; ++i)
        if (util::strCaseCmp(serial, slots_[static_cast<std::size_t>(i)].rec.serNo) == 0) return i;
    return -1;
}

int Enumerator::openDriver(int i)
{
    std::lock_guard<std::recursive_mutex> lk(m_);
    DeviceSlot &s = slots_[static_cast<std::size_t>(i)];
    s.handle.reset();
    int err = platform::UsbDevice::kErrNoDevice; // reported when there is no USB backend
    std::unique_ptr<platform::UsbDevice> h = d_.usb ? d_.usb->open(s.rec.usb, &err) : nullptr;
    if (!h) {
        if (d_.log) d_.log->log(Level::Error, __func__, "libusb_open() %d", err);
        return 1;
    }
    if (d_.log)
        d_.log->log(Level::Verbose, __func__,
                    "Opened device with idVendor = 0x%.4x idProduct = 0x%.4x fwVersion = 0x%.4x busNum = %03d portNum = %03d",
                    s.rec.vid, s.rec.pid, s.rec.rev, s.rec.bus, s.rec.port);
    int config = 0;
    int rc = h->getConfiguration(&config);
    if (rc != 0) {
        if (d_.log) d_.log->log(Level::Error, __func__, "libusb_get_configuration() %d", rc);
        return 1;
    }
    if (d_.log) d_.log->log(Level::Verbose, __func__, "libusb_get_configuration() = %d", config);
    if (config != 1) {
        rc = h->setConfiguration(1);
        if (rc != 0) {
            if (d_.log) d_.log->log(Level::Error, __func__, "libusb_set_configuration() %d", rc);
            return 1;
        }
    }
    s.rec.kernelDriverDetached = false;
    rc = h->claimInterface(0);
    if (rc == 0) {
        s.handle = std::move(h);
        return 0;
    }
    if (d_.log) d_.log->log(Level::Verbose, __func__, "libusb_claim_interface() %d", rc);
    if (rc == platform::UsbDevice::kErrBusy) {
        if (d_.log) d_.log->log(Level::Verbose, __func__, "Checking if kernel driver has interface 0");
        rc = h->kernelDriverActive(0);
        if (rc == 0) {
            if (d_.log)
                d_.log->log(Level::Verbose, __func__,
                            "kernel driver not active - should have been able to claim interface!");
        } else if (rc == 1) {
            if (d_.log) d_.log->log(Level::Verbose, __func__, "kernel driver has interface 0 - trying to detach");
            rc = h->detachKernelDriver(0);
            if (rc == 0) {
                s.rec.kernelDriverDetached = true;
                if (d_.log)
                    d_.log->log(Level::Verbose, __func__, "kernel driver detached - trying again to claim interface 0");
                rc = h->claimInterface(0);
                if (rc == 0) {
                    s.handle = std::move(h);
                    return 0;
                }
                if (d_.log) d_.log->log(Level::Verbose, __func__, "libusb_claim_interface() %d", rc);
                s.rec.kernelDriverDetached = false;
                rc = h->attachKernelDriver(0);
                if (rc != 0 && d_.log) d_.log->log(Level::Verbose, __func__, "libusb_attach_kernel_driver() %d", rc);
            } else if (d_.log) {
                d_.log->log(Level::Verbose, __func__, "libusb_detach_kernel_driver() %d", rc);
            }
        } else if (d_.log) {
            d_.log->log(Level::Verbose, __func__, "libusb_kernel_driver_active() %d", rc);
        }
    } else if (d_.log) {
        d_.log->log(Level::Verbose, __func__, "libusb_claim_interface() %d", rc);
    }
    return 1;
}

void Enumerator::closeDriver(int i)
{
    std::lock_guard<std::recursive_mutex> lk(m_);
    DeviceSlot &s = slots_[static_cast<std::size_t>(i)];
    if (!s.handle) return;
    const int rc = s.handle->releaseInterface(0);
    if (rc != 0 && d_.log) d_.log->log(Level::Error, __func__, "libusb_release_interface() %d", rc);
    if (s.rec.kernelDriverDetached) {
        s.rec.kernelDriverDetached = false;
        const int a = s.handle->attachKernelDriver(0);
        if (a != 0 && d_.log) d_.log->log(Level::Error, __func__, "libusb_attach_kernel_driver() %d", a);
        if (d_.log) d_.log->log(Level::Verbose, __func__, "kernel driver re-attached to interface 0");
    }
    s.handle.reset();
}

bool Enumerator::readEeprom(int i, std::uint16_t addr, std::uint8_t *buf, std::uint16_t len)
{
    std::lock_guard<std::recursive_mutex> lk(m_);
    DeviceSlot &s = slots_[static_cast<std::size_t>(i)];
    if (!s.handle) return false;
    return eepromRead(*s.handle, addr, buf, len, d_.log) == 0;
}

} // namespace service
} // namespace sdrplay
