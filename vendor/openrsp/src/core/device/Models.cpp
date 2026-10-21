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

#include "core/device/Models.h"

namespace sdrplay {
namespace device {

namespace {
const ModelInfo kModels[] = {
    {kPidRsp1, kHwRsp1, "RSP1", true},
    {kPidRsp1A, kHwRsp1A, "RSP1A", true},
    {kPidRsp2, kHwRsp2, "RSP2", true},
    {kPidRspDuo, kHwRspDuo, "RSPduo", true},
    {kPidRspDx, kHwRspDx, "RSPdx", true},
    {kPidRsp1B, kHwRsp1B, "RSP1B", true},
    {kPidRspDxR2, kHwRspDxR2, "RSPdxR2", true},
};
} // namespace

const ModelInfo *modelByPid(std::uint16_t pid)
{
    for (const auto &m : kModels)
        if (m.pid == pid) return &m;
    return nullptr;
}

const ModelInfo *modelByHwVer(std::uint8_t hwVer)
{
    for (const auto &m : kModels)
        if (m.hwVer == hwVer) return &m;
    return nullptr;
}

const ModelInfo *allModels(int *count)
{
    if (count) *count = static_cast<int>(sizeof kModels / sizeof kModels[0]);
    return kModels;
}

} // namespace device
} // namespace sdrplay
