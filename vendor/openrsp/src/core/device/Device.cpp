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

#include "core/device/Device.h"

#include "core/device/rsp1a/Rsp1aDevice.h"
#include "core/device/rsp1b/Rsp1bDevice.h"
#include "core/device/rsp2/Rsp2Device.h"
#include "core/device/rspduo/RspDuoDevice.h"
#include "core/device/rspdx/RspdxDevice.h"
#include "platform/Thread.h"

namespace sdrplay {
namespace device {

void Device::runStream(const std::atomic<bool> &kill)
{
    while (!kill) platform::sleepMs(10);
}

std::unique_ptr<Device> createDevice(std::uint8_t hwVer, const DeviceContext &ctx)
{
    switch (hwVer) {
    case kHwRsp1B: return std::make_unique<Rsp1bDevice>(ctx);
    case kHwRsp1A: // the RSP1 shares the RSP1A device class
    case kHwRsp1: return std::make_unique<Rsp1aDevice>(ctx, hwVer);
    case kHwRsp2: return std::make_unique<Rsp2Device>(ctx); // RSP2 / RSP2pro
    case kHwRspDx:
    case kHwRspDxR2: return std::make_unique<RspdxDevice>(ctx, hwVer);
    case kHwRspDuo: {
        RspDuoConfig cfg;
        cfg.rspDuoMode = ctx.rspDuoMode;
        cfg.rspDuoSampleFreq = ctx.rspDuoSampleFreq;
        cfg.slaveParams = ctx.slaveParams;
        auto d = std::make_unique<RspDuoDevice>(ctx, cfg);
        if (ctx.slaveSink) d->setSlaveSink(ctx.slaveSink);
        return d;
    }
    default: return nullptr;
    }
}

} // namespace device
} // namespace sdrplay
