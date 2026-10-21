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

// RSP1A (USB 1df7:3000, hwVer 255) and RSP1 (USB 1df7:2500, hwVer 1) models.
//
// One device class for both, because they share one protocol engine.
// The hardware programming is rsp1a::Protocol (Rsp1aProtocol.h); the wiring
// into a service device is the model-independent common::CommonRspDevice (as
// for the RSP1B) with every default hook: the stream path, the DSP, the AGC and
// overload behave as on the RSP1B.
#pragma once

#include "core/device/common/CommonRspDevice.h"
#include "core/device/rsp1a/Rsp1aProtocol.h"

namespace sdrplay {
namespace device {

class Rsp1aDevice final : public common::CommonRspDevice<Rsp1aDevice, rsp1a::Protocol> {
public:
    // hwVer: kHwRsp1A (255) or kHwRsp1 (1).
    Rsp1aDevice(const DeviceContext &ctx, std::uint8_t hwVer);
    ~Rsp1aDevice() override;

private:
    friend class common::CommonRspDevice<Rsp1aDevice, rsp1a::Protocol>;
};

} // namespace device
} // namespace sdrplay
