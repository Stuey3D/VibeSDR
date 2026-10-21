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

// Behaviour shared by every model's protocol engine.
//
// Parameter limit checks, Update and their LOG lines
// do not depend on the model. The per-model engines (rsp1b, rsp1a, rsp2, rspdx, rspduo)
// keep their own tables and hardware hooks and take the shared pieces from here:
//   - ProtocolBase: the Update-step bookkeeping behind the
//     "Update: ERROR: Tuner%d: <step> err=%d" lines (updateFailStep) and the Init stage
//     behind "device parameter check returned error" & co. (initFailStage);
//   - the limit-message texts: values are printed as a 32-bit %d ("= -1", never 4294967295).
#pragma once

#include <string>

namespace sdrplay {
namespace device {
namespace common {

// "<what> out of range (<lo>:<hi>) = <v>", every number as a 32-bit %d.
std::string limitMsg(const char *what, long long lo, long long hi, long long v);
// "Tuner<t>: <what> out of range (<lo>:<hi>) = <v>"
std::string tunerLimitMsg(int tuner, const char *what, long long lo, long long hi, long long v);

// Update step labels that Update prints without " err=%d".
constexpr const char kStepProgramGr[] = "gain programming";                           // printed as Tuner<t>: <label> failed
constexpr const char kStepDuoBiasTWrongTuner[] = "bias-T control: wrong tuner";       // printed as is

// Protocol engine state (state()).
enum ProtocolState : int { kStateIdle, kStateInitialised, kStateStopped, kStateStreaming };

// Stage at which init() failed (initFailStage()).
enum InitStage : int { kInitOk = 0, kInitParams = 1, kInitChannelState = 2, kInitHardware = 3, kInitStart = 4 };

class ProtocolBase {
public:
    // Stage at which the last init() failed (an InitStage; selects Init's LOG line).
    int initFailStage() const { return initFailStage_; }
    // Update's LOG label of the step that failed ("Tuner%d: <label> err=%d"), or null.
    const char *updateFailStep() const { return updStep_; }

protected:
    // Result of one Update step: a failure keeps its label for the device's LOG line.
    int step(const char *label, int r)
    {
        if (r != 0) updStep_ = label;
        return r;
    }

    int initFailStage_ = 0;
    const char *updStep_ = nullptr;
};

} // namespace common
} // namespace device
} // namespace sdrplay
