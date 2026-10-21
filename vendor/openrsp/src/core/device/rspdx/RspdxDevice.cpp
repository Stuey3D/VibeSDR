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

#include "core/device/rspdx/RspdxDevice.h"

namespace sdrplay {
namespace device {

RspdxDevice::RspdxDevice(const DeviceContext &ctx, std::uint8_t hwVer) : CommonRspDevice(ctx, hwVer)
{
    setIo(std::unique_ptr<rsp1b::Io>(new DeviceIo(*this)));
    ipc::CmdMap *m = ctx_.params;
    setProtocol(std::unique_ptr<rspdx::Protocol>(new rspdx::Protocol(hwVer_, m ? &m->devParams : nullptr,
                                                                     m ? &m->rxChannelA : nullptr,
                                                                     m ? &m->internal.params : nullptr, io())));
}

RspdxDevice::~RspdxDevice() = default;

void RspdxDevice::blockProcessed(const dsp::BlockInfo &)
{
    if (!proto_->hdrDsp()) hdrTrack_.valid = false;
}

void RspdxDevice::hdrOverride()
{
    if (!proto_->hdrDsp()) {
        proc_.setDownConverterOverride(false, dsp::DownConverterConfig{});
        return;
    }
    // HDR sets LIF = 1, enable = 1; fs 6 MHz: the decimate-by-3 path (count / 3), else the
    // decimate-by-4 path with count shift 4; Ionly 0
    const rsp1b::DownConv dc = proto_->downConv();
    dsp::DownConverterConfig c;
    c.lif = true;
    c.enable = true;
    c.path = dc.path;
    c.countShift = static_cast<std::uint32_t>(dc.countShift);
    c.countDivisor = static_cast<std::uint32_t>(dc.countDivisor);
    c.adsbCfg = false;
    c.ionly = false;
    c.ifType = 0;
    proc_.setDownConverterOverride(true, c);
}

void RspdxDevice::configureDspAtInit(const Snapshot &s)
{
    hdrOverride();
    hdrTrack_.valid = false;
    CommonRspDevice::configureDspAtInit(s);
}

void RspdxDevice::applyDspRequests()
{
    hdrOverride(); // the engine's current choice (it requests downConverterReinit when it changes)
    CommonRspDevice::applyDspRequests();
}

bool RspdxDevice::modelDownConvert(std::int16_t *xi, std::int16_t *xq, std::uint32_t n, bool reset)
{
    if (!proto_->hdrDsp()) return false;
    const std::int32_t frf = proto_->frf();
    const int bw = proto_->hdrBw();
    if (hdrTrack_.update(bw, proto_->hdrActive(), frf, reset)) {
        hdrDecim4_.reset(frf);
        hdrDecim3_.reset(frf);
        ++stats_.hdrDspResets;
    }
    ++stats_.hdrBlocks;
    if (proto_->downConv().path == dsp::DownConvPath::kLif1620Decim3) (void)hdrDecim3_.process(xi, n, bw, xi, xq); // fs 6 MHz
    else (void)hdrDecim4_.process(xi, n, bw, xi, xq);                                    // any other fs
    return true;
}

} // namespace device
} // namespace sdrplay
