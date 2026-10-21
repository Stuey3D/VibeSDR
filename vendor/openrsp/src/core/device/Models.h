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

// RSP model table (USB PID, hwVer, name).
#pragma once

#include <cstdint>

namespace sdrplay {
namespace device {

constexpr std::uint16_t kSdrplayVid = 0x1df7;
// bcdDevice of a device still in ROM-loader mode (needs firmware).
constexpr std::uint16_t kBcdRomLoader = 0x0200;

// USB product IDs.
enum Pid : std::uint16_t {
    kPidRsp1 = 0x2500,
    kPidRsp1A = 0x3000,
    kPidRsp2 = 0x3010,
    kPidRspDuo = 0x3020,
    kPidRspDx = 0x3030,
    kPidRsp1B = 0x3050,
    kPidRspDxR2 = 0x3060,
};

enum HwVer : std::uint8_t {
    kHwRsp1 = 1,
    kHwRsp2 = 2,
    kHwRspDuo = 3,
    kHwRspDx = 4,
    kHwRsp1B = 6,
    kHwRspDxR2 = 7,
    kHwRsp1A = 255,
};

struct ModelInfo {
    std::uint16_t pid;
    std::uint8_t hwVer;
    const char *name;
    bool supported;        // implemented by this driver
};

// PID filter: the set of devices enumerated (the PIDs of the model table).
constexpr bool isSdrplayPid(std::uint16_t pid)
{
    switch (pid) {
    case kPidRsp1:
    case kPidRsp1A:
    case kPidRsp2:
    case kPidRspDuo:
    case kPidRspDx:
    case kPidRsp1B:
    case kPidRspDxR2:
        return true;
    default:
        return false;
    }
}

// nullptr if unknown.
const ModelInfo *modelByPid(std::uint16_t pid);
const ModelInfo *modelByHwVer(std::uint8_t hwVer);
// All known models (7 entries).
const ModelInfo *allModels(int *count);

} // namespace device
} // namespace sdrplay
