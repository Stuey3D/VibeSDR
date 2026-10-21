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

// Firmware loading and the ROM-loader upload (uploadFirmware, resetBridgeCpu, control
// transfers).
//
// Firmware sets: a set is a directory holding fw_rsp1.bin, fw_common.bin and fw_rspduo.bin
// plus a sha256sum-format manifest.sha256 listing them. Installed sets live in
// <firmwareDir>/<name>/. The service uploads kDefaultFirmwareSet ("own": our bridge firmware,
// built from firmware/src) unless SDRPLAY_FIRMWARE=<name> selects another installed set.
// Every image is checked against its set's manifest and the loader size limit before upload;
// a missing or bad image fails the upload (no fallback to another set).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sdrplay {
namespace log {
class Logger;
}
namespace platform {
class UsbDevice;
}
namespace service {

// Default install location of the firmware sets.
constexpr const char kDefaultFirmwareDir[] = "/usr/share/sdrplay_api/firmware";

// Read once at service start (main.cpp): the name of the set to upload.
constexpr const char kFirmwareSetEnv[] = "SDRPLAY_FIRMWARE";
// Optional override of the own set's directory (build-tree runs: <build>/fw_own).
constexpr const char kOwnFirmwareDirEnv[] = "SDRPLAY_OWN_FIRMWARE_DIR";
// Our own images: <firmwareDir>/own.
constexpr const char kOwnFirmwareSet[] = "own";
constexpr const char kFirmwareManifestFile[] = "manifest.sha256";

// Compiled-in default: CMake SDRPLAY_FIRMWARE_DEFAULT (default own).
#if defined(SDRPLAY_FIRMWARE_DEFAULT_SET)
constexpr const char kDefaultFirmwareSet[] = SDRPLAY_FIRMWARE_DEFAULT_SET;
#else
constexpr const char kDefaultFirmwareSet[] = "own";
#endif

// A usable set name: 1..64 characters from [A-Za-z0-9._-], not "." or "..", so it always
// names a single directory below the firmware dir.
bool isFirmwareSetName(const char *value);
// The set for an environment value: null or "" -> def; a valid name -> it; anything else
// -> def and *warning is set (empty otherwise).
std::string resolveFirmwareSet(const char *value, const std::string &def, std::string *warning);
// The directory of set `name`: ownDir for the own set when ownDir is non-empty,
// <firmwareDir>/<name> otherwise; "" names firmwareDir itself.
std::string firmwareSetDir(const std::string &firmwareDir, const std::string &name,
                           const std::string &ownDir = std::string());

// <setDir>/<file name for hwVer> (firmware::imageForHwVer), verified against
// <setDir>/manifest.sha256 (firmware::parseManifest) and the loader size limit
// (firmware::kMaxImageSize). Empty on failure; *error then reads "<set> firmware <file>: <why>"
// ("firmware <file>: <why>" for an unnamed set). *sha256Hex receives the verified digest.
std::vector<std::uint8_t> loadFirmware(const std::string &setName, const std::string &setDir, std::uint8_t hwVer,
                                       std::string *error, std::string *sha256Hex = nullptr);

// One control transfer with a 1000 ms timeout.
// 0 on success (transferred == wLength), 1 otherwise (logs an error with the
// libusb_control_transfer() result).
int usbCtrlTransfer(platform::UsbDevice &usb, std::uint8_t bmRequestType, std::uint8_t bRequest,
                    std::uint16_t wValue, std::uint16_t wIndex, std::uint8_t *data, std::uint16_t wLength,
                    log::Logger *log);

// uploadFirmware: vendor OUT request firmware::kFwRequest, wValue = offset, wIndex 0,
// chunks of at most 4096 bytes. 0 on success, 1 on the first failed chunk.
int uploadFirmware(platform::UsbDevice &usb, const std::uint8_t *image, std::size_t len, log::Logger *log);

// resetBridgeCpu: holds the bridge CPU in reset (register write), then restarts it
// from RAM `ramSel` (a failure of this second transfer is expected and ignored).
int resetBridgeCpu(platform::UsbDevice &usb, std::uint8_t ramSel, log::Logger *log);

// EEPROM read: vendor IN request, wValue = addr, wIndex 0. 0 on success.
int eepromRead(platform::UsbDevice &usb, std::uint16_t addr, std::uint8_t *buf, std::uint16_t len,
               log::Logger *log);

} // namespace service
} // namespace sdrplay
