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

#include "service/ServiceCore.h"

#include "core/log/Here.h"

#include "core/api/LastError.h"
#include "core/device/Models.h"
#include "core/ipc/Commands.h"
#include "core/util/Strings.h"
#include "platform/Clock.h"
#include "service/Firmware.h"
#include "service/Heartbeat.h"
#include "service/ServiceDevice.h"

#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace sdrplay {
namespace service {

using log::Level;

namespace {

// The request serial: the C string at the start of the request's DeviceT,
// bounded by the end of the mailbox.
std::string requestSerial(const ipc::ComShMem &c)
{
    const char *p = c.u.selectDevice.device.SerNo;
    return std::string(p, util::strLenBounded(p, sizeof c.u));
}
} // namespace

ServiceCore::ServiceCore(ServiceConfig cfg)
    : cfg_(std::move(cfg)),
      log_(cfg_.logSink, "sdrplay_apiService", Level::Error), // level 3 at start
      enumerator_([this] {
          if (cfg_.firmwareDir.empty()) cfg_.firmwareDir = kDefaultFirmwareDir;
          Enumerator::Deps d;
          d.usb = cfg_.usb;
          d.log = &log_;
          d.sleepMs = cfg_.sleepMs;
          d.firmwareDir = cfg_.firmwareDir;
          d.firmwareSet = resolveFirmwareSet(cfg_.firmwareSelection.c_str(), kDefaultFirmwareSet,
                                             &firmwareSelectionWarning_);
          d.ownFirmwareDir = cfg_.ownFirmwareDir;
          d.errlog = [this](const char *file, const char *func, int line, const std::string &msg) {
              errlog(file, func, line, "%s", msg.c_str());
          };
          return d;
      }())
{
    if (!firmwareSelectionWarning_.empty()) warnAlways("device control", firmwareSelectionWarning_);

    GlobalChannel::Handlers h;
    h.getDevices = [this](ipc::ComShMem &c) { return onGetDevices(c); };
    h.selectDevice = [this](ipc::ComShMem &c) { return onSelectDevice(c); };
    h.releaseDevice = [this](ipc::ComShMem &c) { return onReleaseDevice(c); };
    global_ = std::make_unique<GlobalChannel>(log_, std::move(h));

    enumerator_.setOnListUpdated([this] {
        std::lock_guard<std::mutex> lk(regMtx_);
        regWake_ = true;
        regCv_.notify_all();
    });
    enumerator_.setOnSlotRemoved([this](int slot) { destroyDevice(slot); });
}

ServiceCore::~ServiceCore()
{
    if (started_ && !shutDown_) shutdown();
}

void ServiceCore::warnAlways(const char *func, const std::string &msg)
{
    if (cfg_.logSink)
        cfg_.logSink->write(platform::LogPriority::Warning,
                            log::Logger::format(platform::currentThreadId(), "sdrplay_apiService", Level::Warning,
                                                func, msg));
}

void ServiceCore::errlog(const char *file, const char *func, int line, const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    api::setLastErrorV(&global_->lastError(), platform::realtimeMicros(), file, func, line, fmt, ap);
    va_end(ap);
}

bool ServiceCore::start()
{
    log_.raw("sdrplay_apiService: service worker thread: Entry");
    if (!global_->create()) return false;
    std::memset(&global_->lastError(), 0, sizeof(sdrplay_api_TimedErrorInfoT));
    started_ = true;
    if (cfg_.backgroundThreads) {
        // Start order: watchdog thread, then registration thread.
        watchdog_ = platform::Thread::start(
            [this] {
                log_.raw("device: heartbeat thread: Entry");
                while (!waitStop(ipc::kServiceWatchdogPeriodMs)) watchdogOnce();
                log_.raw("device: heartbeat thread: Exit");
            },
            "heartbeat");
        registerDev_ = platform::Thread::start(
            [this] {
                // Woken after every device-list update; it does no work and
                // never touches the network, only logs its entry and exit.
                log_.raw("device: registration thread: Entry");
                std::unique_lock<std::mutex> lk(regMtx_);
                while (!regStop_) {
                    regCv_.wait_for(lk, std::chrono::milliseconds(1000), [this] { return regWake_ || regStop_; });
                    regWake_ = false;
                }
                lk.unlock();
                log_.raw("device: registration thread: Exit");
            },
            "registration");
    }
    return true;
}

void ServiceCore::requestStop()
{
    stop_ = true;
}

bool ServiceCore::waitStop(std::uint32_t ms)
{
    std::unique_lock<std::mutex> lk(stopMtx_);
    return stopCv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return wdStop_; });
}

int ServiceCore::run(const std::function<bool()> &stopRequested)
{
    if (!started_) return 1;
    auto stopping = [&] { return stop_.load() || (stopRequested && stopRequested()); };
    while (!stopping()) {
        if (global_->serveOne(ipc::kServiceGlobalPollMs)) {
            // Poll the stop event with a 10 ms timed wait after every response,
            // so back-to-back global commands are ~10 ms apart.
            platform::sleepMs(ipc::kServiceAfterResponseStopPollMs);
        }
    }
    shutdown();
    return 0;
}

void ServiceCore::shutdown()
{
    if (shutDown_) return;
    shutDown_ = true;
    stop_ = true;
    // Shutdown order: device objects first (stream and command threads exit,
    // objects unlinked) ...
    {
        std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
        for (int i = 0; i < static_cast<int>(ipc::kMaxDeviceSlots); ++i) {
            if (!enumerator_.slot(i).dev) continue;
            destroyDevice(i);
            // Close the USB handle cleanly (otherwise libusb_exit warns
            // "device ... still referenced").
            enumerator_.closeDriver(i);
        }
    }
    if (usbEvents_) usbEvents_->stop();
    usbEvents_.reset();
    // ... then registration thread, then the watchdog thread.
    {
        std::lock_guard<std::mutex> lk(regMtx_);
        regStop_ = true;
        regCv_.notify_all();
    }
    if (registerDev_) registerDev_->join();
    registerDev_.reset();
    {
        std::lock_guard<std::mutex> lk(stopMtx_);
        wdStop_ = true;
        stopCv_.notify_all();
    }
    if (watchdog_) watchdog_->join();
    watchdog_.reset();
    global_->destroy(); // RespSema, CmdSema, ComMtx, ComShMem
    if (started_) log_.raw("sdrplay_apiService: service worker thread: Exit");
}

void ServiceCore::destroyDevice(int slot)
{
    std::shared_ptr<ServiceDevice> d;
    {
        std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
        d = std::move(enumerator_.slot(slot).dev);
    }
    if (!d) return;
    d->channel().stop(); // a watchdog blocked in postEvent() lets go within 500 ms
    while (d.use_count() > 1) platform::sleepMs(1);
    d.reset(); // ServiceDevice::teardown(): threads stopped, the 11 objects unlinked
}

// ------------------------------------------------- GlobalCmd::GetDevices
sdrplay_api_ErrT ServiceCore::onGetDevices(ipc::ComShMem &c)
{
    std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
    int err = enumerator_.scanUsbDevices();
    if (cfg_.usb && cfg_.backgroundThreads && !usbEvents_) {
        usbEvents_.reset(new stream::UsbEventThread(*cfg_.usb));
        if (!usbEvents_->start()) usbEvents_.reset();
    }
    if (err != 0) {
        log_.log(Level::Error, __func__, "device list update failed %d", err);
        return static_cast<sdrplay_api_ErrT>(err);
    }
    err = enumerator_.refreshDeviceList();
    if (err != 0) {
        log_.log(Level::Error, __func__, "device list update failed %d", err);
        return static_cast<sdrplay_api_ErrT>(err);
    }
    // The client's maxDevs is ignored.
    const sdrplay_api_ErrT r = enumerator_.getDevices(c.u.getDevices.devices, &c.u.getDevices.numDevs);
    if (r != sdrplay_api_Success)
        log_.log(Level::Error, __func__, "GetDevices() failed %d", static_cast<int>(r));
    return r;
}

// ----------------------------------------------- GlobalCmd::SelectDevice
sdrplay_api_ErrT ServiceCore::onSelectDevice(ipc::ComShMem &c)
{
    std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
    const sdrplay_api_ErrT r = [&]() -> sdrplay_api_ErrT {
        const std::string serial = requestSerial(c);
        const sdrplay_api_DeviceT req = c.u.selectDevice.device;
        const std::uint8_t hbDisabled = c.u.selectDevice.heartbeatDisabled;
        const int i = enumerator_.findSerial(serial.c_str());
        if (i < 0) {
            // No service error record for an unknown serial.
            log_.log(Level::Error, __func__, "Serial number %s not found", serial.c_str());
            return sdrplay_api_Fail;
        }
        DeviceSlot &s = enumerator_.slot(i);
        log_.log(Level::Verbose, __func__, "Serial number %s found at pos=%d", s.rec.serNo, i);
        if (s.rec.devAvail != 1) {
            if (s.rec.hwVer != device::kHwRspDuo) {
                log_.log(Level::Error, __func__, "Can't open device %s as it is already open", s.rec.serNo);
                errlog(OPENRSP_HERE, "Device %s already open", s.rec.serNo);
                return sdrplay_api_Fail;
            }
            if (req.rspDuoMode == sdrplay_api_RspDuoMode_Slave && s.dev) {
                // Slave attach: reply with the master's prefix, send SlaveAttached to the
                // master's tuner and add the caller's tuner to the slot's tuners. No USB traffic.
                std::memset(c.u.selectDevice.prefix, 0, sizeof c.u.selectDevice.prefix);
                std::strncpy(c.u.selectDevice.prefix, s.dev->prefix().c_str(), sizeof c.u.selectDevice.prefix - 1);
                s.dev->slaveAttach();
                s.rec.tuner = static_cast<std::uint8_t>(s.rec.tuner | static_cast<std::uint8_t>(req.tuner));
                s.rec.missSlave = 0;
                return sdrplay_api_Success;
            }
            log_.log(Level::Error, __func__, "Can't have 2 masters on the same device");
            errlog(OPENRSP_HERE, "Can't have 2 masters on the same device");
            return sdrplay_api_Fail;
        }
        if (enumerator_.openDriver(i) != 0) {
            log_.log(Level::Error, __func__, "Timeout expired for connection with the device");
            errlog(OPENRSP_HERE, "Failed to initialise correctly - NULL ptrs");
            return sdrplay_api_Fail;
        }
        s.rec.devAvail = 0;
        s.rec.tuner = static_cast<std::uint8_t>(req.tuner);
        s.rec.rspDuoMode = static_cast<std::uint8_t>(req.rspDuoMode);
        s.rec.hbDisabled = hbDisabled;
        s.rec.masterDisconnected = 0;
        log_.log(Level::Verbose, __func__, "Open device with driver %.8llx hwVer %d",
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s.handle.get())), s.rec.hwVer);

        ServiceDevice::Params p;
        p.slot = static_cast<unsigned>(i);
        p.hwVer = s.rec.hwVer;
        p.rspDuoMode = static_cast<std::int32_t>(req.rspDuoMode);
        p.rspDuoSampleFreq = req.rspDuoSampleFreq;
        p.usb = s.handle.get();
        p.logSink = cfg_.logSink;
        p.startThreads = cfg_.backgroundThreads;
        p.ownUsbStream = true;
        auto dev = std::make_shared<ServiceDevice>(p);
        if (!dev->createChannels()) {
            log_.log(Level::Error, __func__, "Failed to create device interface objects");
            errlog(OPENRSP_HERE, "Failed to create device interface objects");
            // Give the device back (close it and mark it available again).
            enumerator_.closeDriver(i);
            s.rec.devAvail = 1;
            return sdrplay_api_Fail;
        }
        s.dev = dev;
        std::memset(c.u.selectDevice.prefix, 0, sizeof c.u.selectDevice.prefix);
        std::strncpy(c.u.selectDevice.prefix, dev->prefix().c_str(), sizeof c.u.selectDevice.prefix - 1);
        dev->finishSetup(req.tuner);
        return sdrplay_api_Success;
    }();
    if (r != sdrplay_api_Success)
        log_.log(Level::Error, __func__, "SelectDevice() failed %d", static_cast<int>(r));
    return r;
}

// ---------------------------------------------- GlobalCmd::ReleaseDevice
sdrplay_api_ErrT ServiceCore::onReleaseDevice(ipc::ComShMem &c)
{
    std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
    const std::string serial = requestSerial(c);
    const sdrplay_api_DeviceT req = c.u.releaseDevice.device;
    log_.log(Level::Message, __func__, "Releasing %s", serial.c_str());
    const int i = enumerator_.findSerial(serial.c_str());
    if (i >= 0) {
        DeviceSlot &s = enumerator_.slot(i);
        log_.log(Level::Verbose, __func__, "Serial number %s found at pos=%d", s.rec.serNo, i);
        bool keepOpen = false;
        if (s.rec.hwVer == device::kHwRspDuo && req.rspDuoMode == sdrplay_api_RspDuoMode_Slave) {
            // Slave release (ReleaseDevice): clear the slave's tuner bit; while the master
            // remains (and this is its slave), reset the slave map and send
            // sdrplay_api_SlaveDetached.
            s.rec.tuner = static_cast<std::uint8_t>(s.rec.tuner & ~static_cast<std::uint8_t>(req.tuner));
            if (s.rec.tuner != 0 && s.rec.masterDisconnected != 1 && s.dev &&
                s.dev->slaveTuner() == static_cast<int>(req.tuner)) {
                s.dev->slaveDetach(false);
                s.rec.missSlave = 0;
                keepOpen = true;
            }
        }
        if (!keepOpen) { // otherwise the device stays open for the master
            destroyDevice(i);
            enumerator_.closeDriver(i);
            s.rec.devAvail = 1;
            s.rec.tuner = 0;
            s.rec.rspDuoMode = 0;
            s.rec.hbDisabled = 0;
            s.rec.missMaster = 0;
            s.rec.missSlave = 0;
            s.rec.masterDisconnected = 0;
        }
    }
    if (i < 0) {
        log_.log(Level::Error, __func__, "Serial number %s not found", serial.c_str());
        errlog(OPENRSP_HERE, "Serial number %s not found", serial.c_str());
        return sdrplay_api_Fail;
    }
    log_.log(Level::Verbose, __func__, "Exit");
    return sdrplay_api_Success;
}

// ------------------------------------------------------- power (Windows)
void ServiceCore::suspendAllDevices()
{
    log_.log(Level::Verbose, __func__, "power type PBT_APMSUSPEND");
    std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
    for (int i = 0; i < static_cast<int>(ipc::kMaxDeviceSlots); ++i) {
        DeviceSlot &s = enumerator_.slot(i);
        if (!s.dev) continue;
        log_.log(Level::Message, __func__, "DevIdx%d: Found", i);
        s.dev->suspendStream();
    }
}

void ServiceCore::resumeAllDevices()
{
    log_.log(Level::Verbose, __func__, "power type PBT_APMRESUMESUSPEND");
    std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
    for (int i = 0; i < static_cast<int>(ipc::kMaxDeviceSlots); ++i) {
        DeviceSlot &s = enumerator_.slot(i);
        if (!s.dev) continue;
        static const char kFn[] = "resume all devices";
        log_.log(Level::Message, kFn, "DevIdx%d: Found", i);
        if (s.handle && s.handle->probeReopen() == platform::UsbDevice::kErrNoDevice) {
            log_.log(Level::Message, kFn, "DevIdx%d: Removed, releasing device", i);
            destroyDevice(i);
            enumerator_.closeDriver(i);
            s.rec.devAvail = 1;
            s.rec.tuner = 0;
            s.rec.missMaster = 0;
            s.rec.missSlave = 0;
            s.rec.hbDisabled = 0;
            log_.log(Level::Message, kFn, "DevIdx%d: RELEASED", i);
            continue;
        }
        log_.log(Level::Message, kFn, "DevIdx%d: Not removed", i);
        s.dev->resumeStream();
    }
}

// ------------------------------------------------------- watchdog thread
void ServiceCore::watchdogOnce()
{
    // Phase 1: presence probe (a reopen that reports no device = removed).
    std::vector<std::shared_ptr<ServiceDevice>> removed;
    {
        std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
        for (int i = 0; i < static_cast<int>(ipc::kMaxDeviceSlots); ++i) {
            DeviceSlot &s = enumerator_.slot(i);
            if (!s.dev || !s.handle) continue;
            if (s.handle->probeReopen() == platform::UsbDevice::kErrNoDevice) {
                log_.log(Level::Error, __func__, "DevIdx%d: device removed", i);
                removed.push_back(s.dev);
            }
        }
    }
    // The DeviceRemoved event is posted outside the table lock, so a client
    // that does not consume events blocks only the watchdog, not the table.
    for (auto &d : removed) {
        sdrplay_api_EventParamsT p{};
        d->postEvent(sdrplay_api_DeviceRemoved, p);
    }
    removed.clear();

    // Phase 2: heartbeat accounting (3 missed periods -> release).
    std::lock_guard<std::recursive_mutex> lk(enumerator_.mutex());
    for (int i = 0; i < static_cast<int>(ipc::kMaxDeviceSlots); ++i) {
        DeviceSlot &s = enumerator_.slot(i);
        if (!s.dev || s.rec.hbDisabled) continue;
        // RSPduo: master and slave misses are counted separately (missMaster / missSlave)
        const int st = s.dev->slaveTuner();
        DeviceChannel *sc = s.dev->slaveChannel();
        const bool slaveOn = sc && st != 0 && (s.rec.tuner & st) != 0;
        const std::uint32_t mp = s.dev->channel().takePings();
        const std::uint32_t sp = sc ? sc->takePings() : 0;
        if (slaveOn) {
            const bool masterGone = s.rec.masterDisconnected == 1 ||
                                    HeartbeatMonitor::step(mp, s.rec.missMaster, false) == HeartbeatMonitor::Verdict::Release;
            const bool slaveGone = HeartbeatMonitor::step(sp, s.rec.missSlave, false) == HeartbeatMonitor::Verdict::Release;
            if (!masterGone && !slaveGone) continue;
            if (!masterGone) {
                // slave gone, master alive: uninitSlave, slave map reset,
                // sdrplay_api_SlaveDetached + sdrplay_api_SlaveDllDisappeared to the master
                log_.log(Level::Message, __func__, "DevIdx%d Slave: Disconnected", i);
                s.dev->slaveDetach(true);
                s.rec.tuner = static_cast<std::uint8_t>(s.rec.tuner & ~st);
                s.rec.missSlave = 0;
                continue;
            }
            if (!slaveGone) {
                if (s.rec.masterDisconnected != 1) {
                    // master gone, slave alive: Uninit, sdrplay_api_MasterDllDisappeared to the slave
                    log_.log(Level::Message, __func__, "DevIdx%d Master: Disconnected", i);
                    s.dev->masterDisappeared();
                    s.rec.masterDisconnected = 1;
                }
                continue;
            }
            // both gone: destroyed below
        } else {
            const auto v = HeartbeatMonitor::step(mp, s.rec.missMaster, false);
            if (v != HeartbeatMonitor::Verdict::Release) continue;
        }
        destroyDevice(i);
        enumerator_.closeDriver(i);
        s.rec.devAvail = 1;
        s.rec.tuner = 0;
        s.rec.missMaster = 0;
        s.rec.missSlave = 0;
        s.rec.hbDisabled = 0;
        s.rec.masterDisconnected = 0;
        log_.log(Level::Message, __func__, "DevIdx%d: RELEASED", i);
    }
}

} // namespace service
} // namespace sdrplay
