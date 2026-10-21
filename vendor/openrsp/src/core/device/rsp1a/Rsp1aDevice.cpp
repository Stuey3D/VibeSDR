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

#include "core/device/rsp1a/Rsp1aDevice.h"

namespace sdrplay {
namespace device {

Rsp1aDevice::Rsp1aDevice(const DeviceContext &ctx, std::uint8_t hwVer)
    : CommonRspDevice(ctx, hwVer == kHwRsp1 ? kHwRsp1 : kHwRsp1A)
{
    setIo(std::unique_ptr<rsp1b::Io>(new DeviceIo(*this)));
    ipc::CmdMap *m = ctx_.params;
    setProtocol(std::unique_ptr<rsp1a::Protocol>(new rsp1a::Protocol(hwVer_, m ? &m->devParams : nullptr,
                                                                     m ? &m->rxChannelA : nullptr,
                                                                     m ? &m->internal.params : nullptr, io())));
}

Rsp1aDevice::~Rsp1aDevice() = default;

} // namespace device
} // namespace sdrplay
