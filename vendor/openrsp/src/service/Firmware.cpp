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

#include "service/Firmware.h"

#include "core/firmware/FirmwareCatalog.h"
#include "core/firmware/FirmwareDownload.h"
#include "core/log/Logger.h"
#include "platform/Usb.h"
#include "core/util/Sha256.h"

#include <cstring>
#include <map>
#include <sstream>

#include <fstream>
#include <iterator>

namespace sdrplay {
namespace service {

namespace {
// Bridge vendor requests used around the firmware upload.
constexpr std::uint8_t kVendorOut = 0x40;     // bmRequestType: vendor request, host to device
constexpr std::uint8_t kVendorIn = 0xC0;      // bmRequestType: vendor request, device to host
constexpr std::uint8_t kReqRestartCpu = 0x40; // wValue = RAM select
constexpr std::uint8_t kReqWriteReg = 0x41;   // wValue = register | value << 8
constexpr std::uint8_t kReqEepromRead = 0x46; // wValue = EEPROM address
// Bridge register that holds the CPU in reset.
constexpr std::uint16_t kCpuResetReg = 8;
constexpr std::uint16_t kCpuResetHold = 0x80;
} // namespace

bool isFirmwareSetName(const char *value)
{
    if (!value) return false;
    const std::size_t n = std::strlen(value);
    if (n == 0 || n > 64 || std::strcmp(value, ".") == 0 || std::strcmp(value, "..") == 0) return false;
    for (const char *p = value; *p; ++p) {
        const char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-'))
            return false;
    }
    return true;
}

std::string resolveFirmwareSet(const char *value, const std::string &def, std::string *warning)
{
    if (warning) warning->clear();
    if (!value || !*value) return def;
    if (isFirmwareSetName(value)) return value;
    if (warning)
        *warning = std::string(kFirmwareSetEnv) + "=" + value +
                   ": not a firmware set name (letters, digits, '.', '_', '-'); using " + def;
    return def;
}

std::string firmwareSetDir(const std::string &firmwareDir, const std::string &name, const std::string &ownDir)
{
    if (name.empty()) return firmwareDir;
    if (name == kOwnFirmwareSet && !ownDir.empty()) return ownDir;
    return firmwareDir + "/" + name;
}

std::vector<std::uint8_t> loadFirmware(const std::string &setName, const std::string &setDir, std::uint8_t hwVer,
                                       std::string *error, std::string *sha256Hex)
{
    const char *file = firmware::imageForHwVer(hwVer).fileName;
    auto fail = [&](const std::string &why) {
        if (error) *error = (setName.empty() ? std::string() : setName + " ") + "firmware " + file + ": " + why;
        return std::vector<std::uint8_t>();
    };
    const std::string manifestPath = setDir + "/" + kFirmwareManifestFile;
    std::ifstream mf(manifestPath, std::ios::binary);
    if (!mf) return fail("cannot open " + manifestPath);
    std::ostringstream text;
    text << mf.rdbuf();
    std::map<std::string, std::string> manifest;
    if (!firmware::parseManifest(text.str(), &manifest)) return fail("malformed " + manifestPath);
    const auto it = manifest.find(file);
    if (it == manifest.end()) return fail(std::string("not listed in ") + manifestPath);

    const std::string path = setDir + "/" + file;
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + path);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty() || data.size() > firmware::kMaxImageSize)
        return fail("size " + std::to_string(data.size()) + " outside 1.." + std::to_string(firmware::kMaxImageSize));
    const std::string got = util::Sha256::hexDigest(data.data(), data.size());
    if (got != it->second) return fail("sha256 " + got + " does not match " + manifestPath);
    if (sha256Hex) *sha256Hex = got;
    return data;
}

int usbCtrlTransfer(platform::UsbDevice &usb, std::uint8_t bmRequestType, std::uint8_t bRequest,
                    std::uint16_t wValue, std::uint16_t wIndex, std::uint8_t *data, std::uint16_t wLength,
                    log::Logger *log)
{
    const int r = usb.controlTransfer(bmRequestType, bRequest, wValue, wIndex, data, wLength, firmware::kFwTimeoutMs);
    if (r == static_cast<int>(wLength)) return 0;
    if (log) log->log(log::Level::Error, __func__, "libusb_control_transfer() %d", r);
    return 1;
}

int uploadFirmware(platform::UsbDevice &usb, const std::uint8_t *image, std::size_t len, log::Logger *log)
{
    // The image goes to the loader in 4 KiB pieces, each addressed by its offset in wValue.
    constexpr std::size_t kPiece = firmware::kFwChunk;
    if (log) log->log(log::Level::Verbose, __func__, "FW image size = %d", static_cast<int>(len));
    std::size_t off = 0;
    do {
        const std::size_t n = len - off < kPiece ? len - off : kPiece;
        if (usbCtrlTransfer(usb, firmware::kFwRequestType, firmware::kFwRequest, static_cast<std::uint16_t>(off), 0,
                            const_cast<std::uint8_t *>(image + off), static_cast<std::uint16_t>(n), log) != 0) {
            if (log)
                log->log(log::Level::Error, __func__, "Transfer failed (offset = %d, size = %d)",
                         static_cast<int>(off), static_cast<int>(n));
            return 1;
        }
        off += n;
    } while (off < len);
    return 0;
}

int resetBridgeCpu(platform::UsbDevice &usb, std::uint8_t ramSel, log::Logger *log)
{
    // Hold the CPU in reset, then restart it from the selected RAM.
    if (usbCtrlTransfer(usb, kVendorOut, kReqWriteReg, static_cast<std::uint16_t>(kCpuResetReg | kCpuResetHold << 8), 0,
                        nullptr, 0, log) != 0) {
        if (log) log->log(log::Level::Error, __func__, "transfer failed");
        return 1;
    }
    if (log) log->log(log::Level::Verbose, __func__, "RamSel = %d", ramSel);
    if (usbCtrlTransfer(usb, kVendorOut, kReqRestartCpu, ramSel, 0, nullptr, 0, log) != 0) {
        // The CPU resets while answering, so this transfer usually "fails".
        if (log) log->log(log::Level::Warning, __func__, "Ignoring as side effect of this function");
    }
    return 0;
}

int eepromRead(platform::UsbDevice &usb, std::uint16_t addr, std::uint8_t *buf, std::uint16_t len, log::Logger *log)
{
    const int r = usbCtrlTransfer(usb, kVendorIn, kReqEepromRead, addr, 0, buf, len, log);
    if (r != 0 && log) log->log(log::Level::Error, __func__, "Transfer failed");
    return r;
}

} // namespace service
} // namespace sdrplay
