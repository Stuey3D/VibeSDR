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

// RSPdx (USB 1df7:3030, hwVer 4) and RSPdx-R2 (1df7:3060, hwVer 7) model:
// the RSP1B bridge and tuner, the DX front-end expander (antenna A/B/C, bias-T,
// MW/FM and DAB notches, step attenuator + pad) and the HDR direct-sampling
// path.
//
// Wiring of rspdx::Protocol into a service device: the model-independent
// common::CommonRspDevice (the RSP1B wiring) with these hooks:
//   - the protocol engine is rspdx::Protocol (hwVer 4 or 7);
//   - model() / the AGC set-point limit use the DX hwVer;
//   - in HDR the stream thread runs no AGC and no dcCal = 4
//     re-trigger, and the overload detector uses its HDR variant;
//   - the HDR sample path: in HDR the DSP runs LIF with the
//     HDR down-converter configuration (fs 6 MHz: the decimate-by-3 path, count / 3; else the
//     decimate-by-4 path with the count shift 4, i.e. n/16) and the HdrDecim3 / HdrDecim4 converters (RspdxHdrDsp.h) instead of the normal
//     converter; reset when hdrBw / hdrActive / Frf changed or on a stream reset (HdrDspTracker).
#pragma once

#include "core/device/common/CommonRspDevice.h"
#include "core/device/rspdx/RspdxHdrDsp.h"
#include "core/device/rspdx/RspdxProtocol.h"

namespace sdrplay {
namespace device {

class RspdxDevice final : public common::CommonRspDevice<RspdxDevice, rspdx::Protocol> {
public:
    // hwVer: kHwRspDx (4) or kHwRspDxR2 (7).
    RspdxDevice(const DeviceContext &ctx, std::uint8_t hwVer);
    ~RspdxDevice() override;

private:
    friend class common::CommonRspDevice<RspdxDevice, rspdx::Protocol>;

    bool hdrActive() const { return proto_->hdrActive(); }
    void blockProcessed(const dsp::BlockInfo &info);
    bool modelDownConvert(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool reset);
    void configureDspAtInit(const Snapshot &s);
    void applyDspRequests();
    void hdrOverride(); // the DSP's down-converter configuration in HDR

    rspdx::HdrDspTracker hdrTrack_;
    rspdx::HdrDecim4 hdrDecim4_;
    rspdx::HdrDecim3 hdrDecim3_;
};

} // namespace device
} // namespace sdrplay
