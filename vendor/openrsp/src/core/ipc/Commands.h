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

// Command codes, timeouts and object parameters of the client<->service IPC.
#pragma once

#include <cstdint>

namespace sdrplay {
namespace ipc {

// Command codes. The cmd field of the global mailbox (/dev/shm/<ns>sdrSrvComShMem) and of a
// device's command map (/dev/shm/<prefix>_mCmdMap) keeps the last command's code after the
// call returns, so reading the file after each API call shows the code that call sends.

// Global channel commands (ComShMem::cmd).
enum class GlobalCmd : std::uint32_t {
    ApiVersion = 1,
    GetDevices = 2,
    SelectDevice = 3,
    ReleaseDevice = 4,
    DebugEnable = 5,
    GetLastError = 6,
};

// Per-device commands (CmdMap::cmd).
enum class DeviceCmd : std::uint32_t {
    DebugEnable = 7,
    Init = 8,
    Uninit = 9,
    SwapTuner = 10,
    ChangeSampleRate = 11,
    Update = 12,
    InternalUpdate = 13,
    GetLastError = 14,
};

// Version handshake: the service answers GlobalCmd::ApiVersion with this version; the client
// compares it exactly and then reports its own API version.
constexpr float kServiceIpcVersion = 1.02f;
constexpr float kClientApiVersion = 3.15f;

// Timeouts (ms). The client-side ones are measured from outside: with the service paused
// (kill -STOP) the API call returns ServiceNotResponding after exactly this long.
// tools/derive_ipc measures the global, GetDevices and device command timeouts that way.
// Client: wait for the service's answer to a global command; GetDevices gets longer because
// the service scans USB while the client waits.
constexpr std::uint32_t kGlobalResponseTimeoutMs = 1000;
constexpr std::uint32_t kGetDevicesResponseTimeoutMs = 20000;
// Client: wait for the service's answer to a device command.
constexpr std::uint32_t kDeviceResponseTimeoutMs = 5000;
// Service: how long the global channel waits for a command before checking for shutdown.
constexpr std::uint32_t kServiceGlobalPollMs = 1000;
// Service: pause after answering a global command (consecutive global commands are served at
// most every 10 ms; none on Windows).
#if defined(_WIN32)
constexpr std::uint32_t kServiceAfterResponseStopPollMs = 0;
#else
constexpr std::uint32_t kServiceAfterResponseStopPollMs = 10;
#endif
// Service: how long a device command thread waits for a command before checking its stop flag.
constexpr std::uint32_t kServiceDeviceCmdPollMs = 100;
// Client: slice of the retry loop that takes the inter-process API lock.
constexpr std::uint32_t kComMtxLockSliceMs = 1000;
// Client: slice of the retry loop that takes a device object's mutex.
constexpr std::uint32_t kDeviceMutexSliceMs = 1000;

// Heartbeat.
constexpr std::uint32_t kClientHeartbeatInitialDelayMs = 1000;
constexpr std::uint32_t kClientHeartbeatPeriodMs = 500;
constexpr std::uint32_t kClientHeartbeatReplyTimeoutMs = 5000;
constexpr std::uint32_t kServiceHeartbeatPollMs = 100;      // service heartbeat thread wait per pass
constexpr std::uint32_t kServiceWatchdogPeriodMs = 1000;    // service watchdog thread period
constexpr std::uint32_t kServiceHeartbeatMissLimit = 3;     // missed periods -> device released (~3.2 s)

// Streaming / events.
constexpr std::uint32_t kDataRingSlots = 8;
constexpr std::uint32_t kEventRingSlots = 8;
// Client stream thread: wait for a filled data slot before checking its stop flag.
constexpr std::uint32_t kClientDataWaitMs = 1000;
// Service: wait for a free data slot, up to kServiceDataSlotTries times; after that the
// block is discarded.
constexpr std::uint32_t kServiceDataSlotWaitMs = 10;
constexpr std::uint32_t kServiceDataSlotTries = 10;
// Client event thread: wait for a posted event before checking its stop flag.
constexpr std::uint32_t kClientEventWaitMs = 100;
// Service: wait per attempt for a free event slot (retried until one is free).
constexpr std::uint32_t kServiceEventSlotWaitMs = 500;

// Open: retry period and the delay armed after any failure.
constexpr std::uint32_t kOpenRetrySliceMs = 10;
constexpr int kServiceConnectDelayAfterFailureMs = 200;

// Semaphore initial/max values.
struct SemInit {
    std::int64_t initial;
    std::int64_t maximum;
};
constexpr SemInit kGlobalSemInit{0, 1};   // global command and response
constexpr SemInit kHrtbtSemInit{0, 1};    // heartbeat request and reply
constexpr SemInit kCmdSemInit{0, 1};      // device command and response
constexpr SemInit kDataSemInit{8, 8};     // data ring: free slots
constexpr SemInit kDataRSemInit{0, 8};    // data ring: filled slots
constexpr SemInit kEventSemInit{8, 8};    // event ring: free slots
constexpr SemInit kEventRSemInit{0, 8};   // event ring: filled slots

} // namespace ipc
} // namespace sdrplay
