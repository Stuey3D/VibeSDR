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

#include "service/GlobalChannel.h"

#include "core/api/LastError.h"
#include "core/ipc/Names.h"
#include "core/log/Logger.h"
#include "platform/Clock.h"

#include <cstring>

namespace sdrplay {
namespace service {

GlobalChannel::GlobalChannel(log::Logger &log, Handlers h) : log_(log), h_(std::move(h)) {}

GlobalChannel::~GlobalChannel() = default; // creators' destructors unlink

bool GlobalChannel::create()
{
    // Creation order: ComShMem, ComMtx, CmdSema, RespSema.
    int err = 0;
    const char *what = nullptr;
    shm_ = platform::SharedMemory::create(ipc::globalName(ipc::kComShMemName), ipc::kComShMemSize, &err);
    if (!shm_) what = "Could not create file mapping object";
    if (!what) {
        mtx_ = platform::InterProcessMutex::create(ipc::globalName(ipc::kComMtxName), &err);
        if (!mtx_) what = "Could not create common mutex";
    }
    if (!what) {
        cmdSem_ = platform::InterProcessSemaphore::create(ipc::globalName(ipc::kCmdSemaName),
                                                          ipc::kGlobalSemInit.initial, ipc::kGlobalSemInit.maximum, &err);
        if (cmdSem_)
            respSem_ = platform::InterProcessSemaphore::create(
                ipc::globalName(ipc::kRespSemaName), ipc::kGlobalSemInit.initial, ipc::kGlobalSemInit.maximum, &err);
        if (!cmdSem_ || !respSem_) what = "Could not create command semaphore";
    }
    if (what) {
        log_.log(log::Level::Error, __func__, "%s", what);
        respSem_.reset();
        cmdSem_.reset();
        mtx_.reset();
        shm_.reset();
        return false;
    }
    return true;
}

void GlobalChannel::destroy()
{
    // Teardown order at worker exit: RespSema, CmdSema, ComMtx, ComShMem.
    respSem_.reset();
    cmdSem_.reset();
    mtx_.reset();
    shm_.reset();
}

ipc::ComShMem *GlobalChannel::com() const { return shm_ ? static_cast<ipc::ComShMem *>(shm_->data()) : nullptr; }

bool GlobalChannel::serveOne(std::uint32_t waitMs)
{
    if (!cmdSem_ || cmdSem_->wait(waitMs) != platform::InterProcessSemaphore::Ok) return false;
    ipc::ComShMem *cp = com();
    if (!cp) return false; // not created (cmdSem_ implies shm_; keeps -Wnull-dereference quiet)
    ipc::ComShMem &c = *cp;
    log_.log(log::Level::Verbose, __func__, "Got cmdId %d", static_cast<int>(c.cmd));
    dispatch(c);
    respSem_->release(1, nullptr); // always, even for unknown commands
    return true;
}

void GlobalChannel::dispatch(ipc::ComShMem &c)
{
    using ipc::GlobalCmd;
    switch (static_cast<GlobalCmd>(c.cmd)) {
    case GlobalCmd::ApiVersion:
        c.result = sdrplay_api_Success;
        c.u.apiVersion.serviceIpcVersion = ipc::kServiceIpcVersion;
        return;
    // GetDevices, SelectDevice and ReleaseDevice fail when no handler is plugged in.
    case GlobalCmd::GetDevices:
        c.result = h_.getDevices ? h_.getDevices(c) : sdrplay_api_Fail;
        return;
    case GlobalCmd::SelectDevice:
        c.result = h_.selectDevice ? h_.selectDevice(c) : sdrplay_api_Fail;
        return;
    case GlobalCmd::ReleaseDevice:
        c.result = h_.releaseDevice ? h_.releaseDevice(c) : sdrplay_api_Fail;
        return;
    case GlobalCmd::DebugEnable: {
        // Process-wide, persists across clients.
        const int lvl = static_cast<int>(c.u.debugEnable.dbgLvl);
        log_.raw("service log level: %d", lvl);
        log_.setLevel(static_cast<log::Level>(lvl));
        c.result = sdrplay_api_Success;
        return;
    }
    case GlobalCmd::GetLastError:
        std::memcpy(&c.u.getLastError.error, &lastError_, sizeof lastError_);
        c.result = sdrplay_api_Success;
        return;
    }
    // Unknown command: Fail and a service error record.
    api::setLastError(&lastError_, platform::realtimeMicros(), __FILE__, __func__, __LINE__, "Unknown command");
    c.result = sdrplay_api_Fail;
}

} // namespace service
} // namespace sdrplay
