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

// Service side of the global channel:
// Glbl\sdrSrv{ComShMem, ComMtx, CmdSema 0/1, RespSema 0/1}.
#pragma once

#include "core/api/ApiTypes.h"
#include "core/ipc/Layouts.h"
#include "platform/InterProcessMutex.h"
#include "platform/InterProcessSemaphore.h"
#include "platform/SharedMemory.h"

#include <functional>
#include <memory>

namespace sdrplay {
namespace log {
class Logger;
}
namespace service {

class GlobalChannel {
public:
    // Handlers the service core plugs in for GetDevices, SelectDevice and
    // ReleaseDevice. Each fills the response fields in `com` and returns the
    // result code.
    struct Handlers {
        std::function<sdrplay_api_ErrT(ipc::ComShMem &)> getDevices;
        std::function<sdrplay_api_ErrT(ipc::ComShMem &)> selectDevice;
        std::function<sdrplay_api_ErrT(ipc::ComShMem &)> releaseDevice;
    };

    GlobalChannel(log::Logger &log, Handlers h);
    ~GlobalChannel(); // unlinks all 4 objects (service exit)

    // Creates the 4 objects (stale ones discarded). false on failure.
    bool create();
    // Unlinks the 4 objects (also done by the destructor).
    void destroy();

    // One iteration of the global command loop: waits up to waitMs for a
    // request; on a request: dispatch, then always signal the response.
    // Returns true if a command was served.
    bool serveOne(std::uint32_t waitMs);

    // Service-global last-error record (returned by GlobalCmd::GetLastError).
    sdrplay_api_TimedErrorInfoT &lastError() { return lastError_; }

    ipc::ComShMem *com() const;

private:
    void dispatch(ipc::ComShMem &com);

    log::Logger &log_;
    Handlers h_;
    std::unique_ptr<platform::SharedMemory> shm_;
    std::unique_ptr<platform::InterProcessMutex> mtx_;
    std::unique_ptr<platform::InterProcessSemaphore> cmdSem_;
    std::unique_ptr<platform::InterProcessSemaphore> respSem_;
    sdrplay_api_TimedErrorInfoT lastError_{};
};

} // namespace service
} // namespace sdrplay
