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

#include "core/device/DefaultParams.h"

#include <cstddef>
#include <cstring>

namespace sdrplay {
namespace device {

DefaultParamsResult writeDefaultParams(ipc::CmdMap &map, std::int32_t rspDuoMode, double rspDuoSampleFreq,
                                       bool slave)
{
    DefaultParamsResult r;
    // Device parameters, both channels and the internal area start out zero.
    std::memset(static_cast<void *>(&map.devParams), 0,
                offsetof(ipc::CmdMap, lastError) - offsetof(ipc::CmdMap, devParams));

    sdrplay_api_DevParamsT &dev = map.devParams;
    sdrplay_api_RxChannelParamsT &a = map.rxChannelA;

    // An RSPduo in dual-tuner or master mode, or a slave, runs at its configured rate with a low IF;
    // everything else starts at 2 MS/s, zero IF.
    const bool shared = rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner || rspDuoMode == sdrplay_api_RspDuoMode_Master;
    if (shared || slave) {
        if (rspDuoSampleFreq == 8000000.0 || rspDuoSampleFreq == 8192000.0) {
            a.tunerParams.ifType = sdrplay_api_IF_2_048;
            dev.fsFreq.fsHz = rspDuoSampleFreq;
            r.fs8192 = rspDuoSampleFreq == 8192000.0;
        } else {
            a.tunerParams.ifType = sdrplay_api_IF_1_620;
            dev.fsFreq.fsHz = 6000000.0;
        }
    } else {
        a.tunerParams.ifType = sdrplay_api_IF_Zero;
        dev.fsFreq.fsHz = 2000000.0;
    }

    a.tunerParams.bwType = sdrplay_api_BW_0_200;
    a.tunerParams.loMode = sdrplay_api_LO_Auto;
    a.tunerParams.gain.gRdB = 50;
    a.tunerParams.gain.minGr = sdrplay_api_NORMAL_MIN_GR;
    a.tunerParams.rfFreq.rfHz = 200000000.0;
    a.tunerParams.dcOffsetTuner.dcCal = 3;
    a.tunerParams.dcOffsetTuner.trackTime = 1;
    a.tunerParams.dcOffsetTuner.refreshRateTime = 2048;
    a.ctrlParams.dcOffset.DCenable = 1;
    a.ctrlParams.dcOffset.IQenable = 1;
    a.ctrlParams.decimation.decimationFactor = 1;
    a.ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
    a.ctrlParams.agc.setPoint_dBfs = -60;
    a.rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
    a.rspDxTunerParams.hdrBw = sdrplay_api_RspDx_HDRMODE_BW_1_700;

    map.rxChannelB = a;
    return r;
}

} // namespace device
} // namespace sdrplay
