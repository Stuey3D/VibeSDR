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

// Service-side heartbeat watchdog logic (run by the service's watchdog thread),
// as a pure state machine so it is unit-testable without threads or time.
//
// Every 1000 ms per selected device: if heartbeat disabled (slot hbDisabled) ->
// skip; if no ping arrived since the last period -> miss++, else miss = 0;
// miss == 3 -> release the device (logged at MSG level,
// ~3.2 s after the client died). For an RSPduo the caller keeps separate
// master and slave counters (ServiceCore::watchdogOnce).
#pragma once

#include "core/ipc/Commands.h"

#include <atomic>
#include <cstdint>

namespace sdrplay {
namespace service {

class HeartbeatMonitor {
public:
    enum class Verdict { Alive, Missed, Release, Disabled };

    // One watchdog period for one device: `pings` received since the last
    // period, `misses` the slot's miss counter, updated in place.
    static Verdict step(std::uint32_t pings, std::int32_t &misses, bool disabled)
    {
        if (disabled) return Verdict::Disabled;
        if (pings != 0) {
            misses = 0;
            return Verdict::Alive;
        }
        ++misses;
        return misses == static_cast<std::int32_t>(ipc::kServiceHeartbeatMissLimit) ? Verdict::Release
                                                                                       : Verdict::Missed;
    }

    explicit HeartbeatMonitor(bool disabled = false) : disabled_(disabled) {}

    // Called by the device's master heartbeat thread for every ping received.
    void onPing() { pings_.fetch_add(1); }
    // Called once per watchdog period.
    Verdict tick() { return step(pings_.exchange(0), misses_, disabled_); }
    std::int32_t misses() const { return misses_; }

private:
    bool disabled_;
    std::atomic<std::uint32_t> pings_{0};
    std::int32_t misses_ = 0;
};

} // namespace service
} // namespace sdrplay
