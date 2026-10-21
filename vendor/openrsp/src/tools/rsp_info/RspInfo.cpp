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

#include "RspInfo.h"

#include "core/firmware/FirmwareCatalog.h"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace rspinfo {

// --------------------------------------------------------------- analysis
Analysis analyse(const RawDevice &d)
{
    Analysis a;
    a.model = sdrplay::device::modelByPid(d.pid);
    a.romLoader = d.bcdDevice == sdrplay::device::kBcdRomLoader;
    if (a.model) a.firmwareImage = sdrplay::firmware::imageForHwVer(a.model->hwVer).fileName;
    return a;
}

const char *inUseName(InUse u)
{
    switch (u) {
    case InUse::No: return "no";
    case InUse::Yes: return "yes";
    default: return "unknown";
    }
}

// --------------------------------------------------------------- output
namespace {
std::string hex4(unsigned v)
{
    char b[16];
    std::snprintf(b, sizeof b, "%04x", v & 0xffffu);
    return b;
}
std::string bcdText(std::uint16_t bcd)
{
    char b[32];
    std::snprintf(b, sizeof b, "0x%04x (%x.%02x)", static_cast<unsigned>(bcd), static_cast<unsigned>(bcd >> 8),
                  static_cast<unsigned>(bcd & 0xffu));
    return b;
}
std::string modelName(const Analysis &a) { return a.model ? a.model->name : "unknown SDRplay device"; }
std::string stateText(const Analysis &a)
{
    if (a.romLoader)
        return "ROM loader, awaiting firmware (the service uploads " +
               (a.firmwareImage.empty() ? std::string("?") : a.firmwareImage) + " on GetDevices)";
    return "firmware running";
}
void field(std::ostringstream &o, const char *k, const std::string &v) { o << "    " << k << std::string(13 - std::strlen(k), ' ') << v << "\n"; }
} // namespace

std::string formatText(const std::vector<RawDevice> &devs, bool verbose)
{
    std::ostringstream o;
    if (devs.empty()) {
        o << "No SDRplay RSP devices found.\n";
        return o.str();
    }
    o << "Found " << devs.size() << " SDRplay RSP device" << (devs.size() == 1 ? "" : "s") << ".\n";
    for (std::size_t i = 0; i < devs.size(); ++i) {
        const RawDevice &d = devs[i];
        const Analysis a = analyse(d);
        o << "\n[" << i << "] " << modelName(a);
        if (a.model) o << "  (hwVer " << static_cast<unsigned>(a.model->hwVer) << ")";
        o << "\n";
        char bus[64];
        std::snprintf(bus, sizeof bus, "bus %03u port %03u", static_cast<unsigned>(d.bus), static_cast<unsigned>(d.port));
        field(o, "USB", hex4(d.vid) + ":" + hex4(d.pid) + "  " + bus + "  (path " +
                            (d.portPath.empty() ? std::string("?") : d.portPath) + ", address " +
                            std::to_string(d.address) + ")");
        field(o, "bcdDevice", bcdText(d.bcdDevice));
        field(o, "state", stateText(a));
        std::string ser;
        if (d.serialPresent)
            ser = d.serial;
        else if (a.romLoader)
            ser = "(none: not reported in ROM-loader mode)";
        else
            ser = "(none" + (d.serialError.empty() ? std::string("") : ": " + d.serialError) + ")";
        field(o, "serial", ser);
        field(o, "in use", std::string(inUseName(d.inUse)) + (d.inUseDetail.empty() ? "" : " (" + d.inUseDetail + ")"));
        if (verbose) {
            if (d.opened || !d.openError.empty())
                field(o, "opened", d.opened ? std::string("yes (read-only use: no claim, no writes)") : "no: " + d.openError);
            field(o, "iSerial", "index " + std::to_string(d.iSerialIndex) +
                                    (d.serialSource.empty() ? "" : ", read from " + d.serialSource));
            if (a.model)
                field(o, "firmware img", a.firmwareImage + (a.romLoader ? " (needed now)" : " (only used in ROM-loader state)"));
        }
    }
    return o.str();
}

std::string jsonEscape(const std::string &s)
{
    std::string o;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += ch;
            }
        }
    }
    return o;
}

namespace {
std::string q(const std::string &s) { return "\"" + jsonEscape(s) + "\""; }
} // namespace

std::string formatJson(const std::vector<RawDevice> &devs, bool verbose)
{
    std::ostringstream o;
    o << "{\n  \"devices\": [";
    for (std::size_t i = 0; i < devs.size(); ++i) {
        const RawDevice &d = devs[i];
        const Analysis a = analyse(d);
        o << (i ? "," : "") << "\n    {\n";
        o << "      \"model\": " << (a.model ? q(a.model->name) : "null") << ",\n";
        o << "      \"hwVer\": " << (a.model ? std::to_string(a.model->hwVer) : "null") << ",\n";
        o << "      \"vid\": " << q(hex4(d.vid)) << ",\n";
        o << "      \"pid\": " << q(hex4(d.pid)) << ",\n";
        o << "      \"bus\": " << static_cast<unsigned>(d.bus) << ",\n";
        o << "      \"port\": " << static_cast<unsigned>(d.port) << ",\n";
        o << "      \"port_path\": " << q(d.portPath) << ",\n";
        o << "      \"address\": " << static_cast<unsigned>(d.address) << ",\n";
        o << "      \"bcdDevice\": " << q("0x" + hex4(d.bcdDevice)) << ",\n";
        o << "      \"state\": " << q(a.romLoader ? "rom_loader" : "running") << ",\n";
        o << "      \"serial\": " << (d.serialPresent ? q(d.serial) : "null") << ",\n";
        o << "      \"in_use\": " << q(inUseName(d.inUse));
        if (verbose) {
            o << ",\n      \"verbose\": {\n";
            o << "        \"in_use_detail\": " << q(d.inUseDetail) << ",\n";
            o << "        \"opened\": " << (d.opened ? "true" : "false") << ",\n";
            o << "        \"open_error\": " << q(d.openError) << ",\n";
            o << "        \"iSerial_index\": " << static_cast<unsigned>(d.iSerialIndex) << ",\n";
            o << "        \"serial_source\": " << q(d.serialSource) << ",\n";
            o << "        \"serial_error\": " << q(d.serialError) << ",\n";
            o << "        \"firmware_image\": " << q(a.firmwareImage) << "\n";
            o << "      }";
        }
        o << "\n    }";
    }
    o << (devs.empty() ? "" : "\n  ") << "]\n}\n";
    return o.str();
}

} // namespace rspinfo
