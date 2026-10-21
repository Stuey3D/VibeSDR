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

// sdrplay_apiService daemon: hosts the service core (sdrplay_core) via the
// platform ServiceHost. It takes no arguments and reads no environment
// variables except the firmware set selection SDRPLAY_FIRMWARE=<name> + SDRPLAY_OWN_FIRMWARE_DIR
// (unset = the compiled-in default); SIGINT/SIGQUIT/SIGTERM stop it cleanly,
// unlinking every Glbl\... object.
// Installed as /opt/sdrplay_api/sdrplay_apiService.
//
// Firmware images are looked up in SDRPLAY_FIRMWARE_DIR (compile time) and then in "firmware/"
// next to the executable.
#include "platform/InterProcessSemaphore.h"
#include "platform/LogSink.h"
#include "platform/ServiceHost.h"
#include "platform/Usb.h"
#include "service/Firmware.h"
#include "service/ServiceCore.h"

#include <cstdlib>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include "platform/windows/WindowsService.h"
#endif

#ifndef SDRPLAY_FIRMWARE_DIR
#define SDRPLAY_FIRMWARE_DIR "/usr/share/sdrplay_api/firmware"
#endif

namespace {
std::string firmwareDirectory()
{
    const std::string candidates[] = {SDRPLAY_FIRMWARE_DIR, sdrplay::platform::executableDirectory() + "/firmware"};
    for (const auto &d : candidates)
        if (std::ifstream(d + "/own/manifest.sha256").good()) return d;   // our firmware
    return SDRPLAY_FIRMWARE_DIR;
}
// Build-tree runs: <build>/firmware is a symlink to the source firmware/, and
// CMake SDRPLAY_OWN_FIRMWARE=ON builds the own images into <build>/fw_own.
// "" (= <firmwareDir>/own) for an installed service.
std::string buildTreeOwnFirmwareDir(const std::string &firmwareDir)
{
    const std::string exeDir = sdrplay::platform::executableDirectory();
    const std::string own = exeDir + "/fw_own";
    if (firmwareDir == exeDir + "/firmware" &&
        std::ifstream(own + "/" + sdrplay::service::kFirmwareManifestFile).good())
        return own;
    return std::string();
}
} // namespace

int main()
{
    using namespace sdrplay;
    // A peer that dies inside a shared semaphore operation must not wedge the
    // service. Process-wide; our client library never
    // enables it.
    platform::InterProcessSemaphore::enableStallRecovery(platform::InterProcessSemaphore::kDefaultStallMs);
#if defined(_WIN32)
    // Debug_Override is read once, before the service dispatcher.
    auto bootSink = platform::createSystemLogSink();
    const int debugOverride = platform::windows::readDebugOverride();
    if (debugOverride > 0) {
        bootSink->write(platform::LogPriority::Info, "service log level: " + std::to_string(debugOverride));
        bootSink->write(platform::LogPriority::Info, "Debug_override enabled");
    }
#endif
    return platform::runServiceHost([&](const platform::StopSignal &stop) {
        auto sink = platform::createSystemLogSink();
        auto usb = platform::createUsbEnumerator();
        service::ServiceConfig cfg;
        cfg.logSink = sink.get();
        cfg.usb = usb.get();
        cfg.firmwareDir = firmwareDirectory();
        if (const char *v = std::getenv(service::kFirmwareSetEnv)) cfg.firmwareSelection = v;
        if (const char *v = std::getenv(service::kOwnFirmwareDirEnv)) cfg.ownFirmwareDir = v;
        else cfg.ownFirmwareDir = buildTreeOwnFirmwareDir(cfg.firmwareDir);
        {
            service::ServiceCore core(cfg);
#if defined(_WIN32)
            if (debugOverride > 0) core.logger().setLevel(static_cast<log::Level>(debugOverride));
            platform::windows::setPowerEventHandler([&core](bool suspend) {
                if (suspend) core.suspendAllDevices();
                else core.resumeAllDevices();
            });
#endif
            if (core.start()) core.run([&stop] { return stop.stopRequested(); });
#if defined(_WIN32)
            platform::windows::setPowerEventHandler({});
#endif
            // The core (and with it every device handle) goes before libusb_exit.
        }
        usb.reset();
        // Returns 0 whenever the stop event could be created, even if the
        // worker failed to create its objects.
        return 0;
    });
}
