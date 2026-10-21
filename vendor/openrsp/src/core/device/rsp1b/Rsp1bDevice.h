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

// RSP1B model (USB 1df7:3050, hwVer 6): MSi2500-like bridge + MSi001-like
// tuner + front-end expander (kReqGpioExt).
//
// Wiring of the pure engines into a service device:
//   - rsp1b::Protocol (Rsp1bProtocol.h) executes Init / Uninit / Update (every
//     reason bit, foreign models' bits included) / InternalUpdate / Release /
//     AGC gain writes over platform::UsbDevice (UsbControlIo). Device answers
//     (FNADDR, tuner trims) come from real control IN transfers.
//   - The stream thread (DSP thread, runStream()): packets from the
//     StreamTransport, dsp::StreamProcessor (packet header toggles + per-block
//     pipeline), agc::PowerEstimator (overload events) and agc::AgcChannel
//     (programGain from the DSP thread), output blocks to the StreamSink.
//   - Events (GainChange, PowerOverloadChange) go to the EventSink; error
//     records to the device record (DeviceContext::lastError).
//
// The wiring itself (locking, DSP thread, change-flag reports, LOG lines) is the
// model-independent common::CommonRspDevice (core/device/common/CommonRspDevice.h);
// the RSP1B uses every default hook.
#pragma once

#include "core/device/common/CommonRspDevice.h"
#include "core/device/rsp1b/Rsp1bProtocol.h"

namespace sdrplay {
namespace device {
namespace rsp1b {

// Protocol::Io control transfers over platform::UsbDevice (1000 ms
// timeout; common::UsbControlIoT).
using UsbControlIo = common::UsbControlIoT<Io>;

} // namespace rsp1b

class Rsp1bDevice final : public common::CommonRspDevice<Rsp1bDevice, rsp1b::Protocol> {
public:
    explicit Rsp1bDevice(const DeviceContext &ctx);
    ~Rsp1bDevice() override;

private:
    friend class common::CommonRspDevice<Rsp1bDevice, rsp1b::Protocol>;
};

} // namespace device
} // namespace sdrplay
