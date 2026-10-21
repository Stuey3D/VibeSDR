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

// The service engine as a LIBRARY (part of target sdrplay_core): hosted by the
// sdrplay_apiService daemon (src/service/main.cpp) on Linux/Windows/macOS, and
// embeddable in-process (Android, unit/integration tests).
//
// It is the service worker thread plus the device control:
//   start()  the worker thread's entry log line, the 4 global objects,
//            the watchdog thread and the registration thread (kept for its Entry/Exit log lines; it does no work).
//   run()    global commands (ipc::GlobalCmd) until stopped, then the shutdown:
//            devices released, registration and watchdog threads exit, global
//            objects unlinked, "...service worker thread: Exit".
//
// Portable: talks to the OS only through src/platform/*.h interfaces.
#pragma once

#include "core/log/Logger.h"
#include "platform/LogSink.h"
#include "platform/Thread.h"
#include "platform/Usb.h"
#include "service/Enumerator.h"
#include "service/GlobalChannel.h"
#include "service/stream/UsbEventThread.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace sdrplay {
namespace service {

struct ServiceConfig {
    platform::LogSink *logSink = nullptr;       // required
    platform::UsbEnumerator *usb = nullptr;     // may be null (no USB: GetDevices returns 0 devices)
    std::string firmwareDir;                    // firmware images; default kDefaultFirmwareDir
    // Sleep used by the firmware re-enumeration wait (tests inject a fake clock).
    std::function<void(std::uint32_t)> sleepMs;
    // false: no watchdog/registration/device threads; tests call watchdogOnce()
    // and drive the device channels by hand.
    bool backgroundThreads = true;
    // Firmware set selection: the raw SDRPLAY_FIRMWARE value ("" = unset). A valid set
    // name (isFirmwareSetName) overrides the compiled-in kDefaultFirmwareSet; any other
    // value logs a warning and keeps the default. The embedded (app) services leave it empty.
    std::string firmwareSelection;
    // Own images' directory (SDRPLAY_OWN_FIRMWARE_DIR); empty = firmwareDir + "/own".
    std::string ownFirmwareDir;
};

class ServiceCore {
public:
    explicit ServiceCore(ServiceConfig cfg);
    ~ServiceCore();

    // Logs Entry, creates the global IPC objects, starts the watchdog thread and
    // registration thread. false on failure (objects already released).
    bool start();

    // Runs the global worker on the calling thread until stopRequested()
    // returns true or requestStop() is called, then shuts down. Returns 0
    // (the worker exit code after a normal stop).
    int run(const std::function<bool()> &stopRequested);

    void requestStop();

    // One watchdog period (watchdog thread body without its 1000 ms sleep).
    void watchdogOnce();

    // Windows power callback (the daemon wires them to
    // platform::windows::setPowerEventHandler). Portable.
    // suspendAllDevices: PBT_APMSUSPEND, stops every streaming device.
    // resumeAllDevices: PBT_APMRESUMESUSPEND, restarts them, or releases a device
    // that disappeared while suspended.
    void suspendAllDevices();
    void resumeAllDevices();

    log::Logger &logger() { return log_; }
    GlobalChannel &globalChannel() { return *global_; }
    Enumerator &enumerator() { return enumerator_; }

private:
    sdrplay_api_ErrT onGetDevices(ipc::ComShMem &c);
    sdrplay_api_ErrT onSelectDevice(ipc::ComShMem &c);
    sdrplay_api_ErrT onReleaseDevice(ipc::ComShMem &c);
    // A WARNING line that is written whatever the debug level.
    void warnAlways(const char *func, const std::string &msg);
    void errlog(const char *file, const char *func, int line, const char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 5, 6)))
#endif
        ;
    // Destroys the device object of a slot (waits for in-flight users).
    void destroyDevice(int slot);
    void shutdown();
    bool waitStop(std::uint32_t ms);

    ServiceConfig cfg_;
    log::Logger log_;
    std::string firmwareSelectionWarning_; // set while building enumerator_ (declared before it)
    Enumerator enumerator_;
    std::unique_ptr<GlobalChannel> global_;

    std::unique_ptr<platform::Thread> watchdog_, registerDev_;
    // Global libusb event thread: started by the first
    // GetDevices with a USB backend and background threads, stopped at shutdown
    // after the devices are gone.
    std::unique_ptr<stream::UsbEventThread> usbEvents_;
    std::atomic<bool> stop_{false}; // worker loop
    bool wdStop_ = false;           // watchdog thread (guarded by stopMtx_)
    bool regStop_ = false;          // registration thread (guarded by regMtx_)
    std::mutex stopMtx_;
    std::condition_variable stopCv_;
    std::mutex regMtx_;
    std::condition_variable regCv_;
    bool regWake_ = false;
    bool started_ = false;
    bool shutDown_ = false;
};

} // namespace service
} // namespace sdrplay
