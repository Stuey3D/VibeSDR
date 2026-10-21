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

// Default device parameters written into a fresh device command map (ipc::CmdMap) by the service
// when the map is created, right after it is zeroed (clients do not set
// defaults themselves; they come from the service).
//
// The defaults are model-independent: they depend only on the RSPduo
// mode and sample rate. This fills devParams, rxChannelA, and then copies
// rxChannelA verbatim into rxChannelB (so the map's rxB area is NOT zero even
// on single-tuner models whose client-side rxChannelB pointer is NULL).
#pragma once

#include "core/ipc/Layouts.h"

#include <cstdint>

namespace sdrplay {
namespace device {

struct DefaultParamsResult {
    // Set when the RSPduo sample rate is
    // 8.192 MHz (ADS-B style IF 2048 at 8.192 MHz).
    bool fs8192 = false;
};

// rspDuoMode: sdrplay_api_RspDuoModeT of the select request (sdrplay_api_RspDuoMode_Dual_Tuner
// and sdrplay_api_RspDuoMode_Master use the RSPduo rate). slave: true for the RSPduo slave map.
// rspDuoSampleFreq: DeviceT.rspDuoSampleFreq of the request.
//
// Writes devParams, rxChannelA, rxChannelB and the internal area; the command
// header and the error record are left untouched.
DefaultParamsResult writeDefaultParams(ipc::CmdMap &map, std::int32_t rspDuoMode, double rspDuoSampleFreq,
                                       bool slave);

} // namespace device
} // namespace sdrplay
