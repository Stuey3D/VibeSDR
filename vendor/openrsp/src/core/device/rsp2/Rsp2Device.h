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

// RSP2 / RSP2pro model (USB 1df7:3010, hwVer 2): the same MSi2500-like bridge
// and MSi001-like tuner as the RSP1B, its own front-end expander bits (antenna
// A/B, Hi-Z AM port, bias-T, RF notch, 24 MHz ext-ref output; req 0x49 at 0x40).
//
// The service wiring is the model-independent common::CommonRspDevice (the RSP1B
// wiring) around rsp2::Protocol. Both models stream the same way, so the stream path is
// identical; only model() and the AGC set-point limit's hwVer argument differ.
#pragma once

#include "core/device/common/CommonRspDevice.h"
#include "core/device/rsp2/Rsp2Protocol.h"

namespace sdrplay {
namespace device {

class Rsp2Device final : public common::CommonRspDevice<Rsp2Device, rsp2::Protocol> {
public:
    explicit Rsp2Device(const DeviceContext &ctx);
    ~Rsp2Device() override;

    // Single tuner, not an RSPduo: rspDuoMode 0. The defaults
    // are model-independent; they already set rsp2TunerParams.antennaSel = ANTENNA_A (5),
    // amPortSel = AMPORT_2 (0), biasT/notch/extRef 0.

private:
    friend class common::CommonRspDevice<Rsp2Device, rsp2::Protocol>;
};

} // namespace device
} // namespace sdrplay
