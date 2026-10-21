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

#include "core/device/common/CommonRspDevice.h"

namespace sdrplay {
namespace device {
namespace common {

dsp::agc::AgcParams agcParamsOf(const sdrplay_api_RxChannelParamsT &rx)
{
    const auto &a = rx.ctrlParams.agc;
    dsp::agc::AgcParams p;
    p.enable = raw(a.enable);
    p.setPoint_dBfs = a.setPoint_dBfs;
    p.attack_ms = a.attack_ms;
    p.decay_ms = a.decay_ms;
    p.decay_delay_ms = a.decay_delay_ms;
    p.decay_threshold_dB = a.decay_threshold_dB;
    p.syncUpdate = a.syncUpdate;
    return p;
}

// The LOG line that accompanies an error record: the record text, at the level the record asks for.
void logRecordDefault(log::Logger &L, const rsp1b::ErrorRecord &r, int /*tuner*/)
{
    if (r.logOnly) { // a LOG line without a record
        L.log(r.logLevel, r.function, "%s", r.message.c_str());
        return;
    }
    switch (r.logAs) {
    case rsp1b::LogAs::None: return;
    case rsp1b::LogAs::Warning: L.log(log::Level::Warning, r.function, "%s", r.message.c_str()); return;
    case rsp1b::LogAs::Verbose: L.log(log::Level::Verbose, r.function, "%s", r.message.c_str()); return;
    case rsp1b::LogAs::Error: break;
    }
    L.log(log::Level::Error, r.function, "%s", r.message.c_str());
}

} // namespace common
} // namespace device
} // namespace sdrplay
