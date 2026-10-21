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

// Client side of the global channel and shared client helpers (internal; not
// exported).
#pragma once

#include "client/ClientAbi.h"
#include "core/ipc/Layouts.h"
#include "platform/LogSink.h"

namespace sdrplay {
namespace client {

sdrplay_api_ErrT open();
sdrplay_api_ErrT close();
// Success = locked; Fail = no session ("NULL ptr"); ServiceNotResponding = the
// mutex is broken because the service is gone (reported rather than waiting
// forever).
sdrplay_api_ErrT lockApi();
sdrplay_api_ErrT unlockApi();
// What a caller that does `if (LockDeviceApi() != 0) return 1;` returns:
// Fail, or ServiceNotResponding when the service is gone.
inline sdrplay_api_ErrT lockFailure(sdrplay_api_ErrT r)
{
    return r == sdrplay_api_ServiceNotResponding ? r : sdrplay_api_Fail;
}

// True between Close and the next Open: the session's handles are gone then,
// so every call that would touch them returns an error instead.
bool sessionClosed();

ipc::ComShMem *com();

// True if the global channel shows that the
// service is gone: its ComMtx was destroyed in place or ComShMem was unlinked
// (the service stopped). Used only after a per-device semaphore error.
bool serviceGone();

// Invalid inputs never crash deliberately: a NULL logger before Open/after
// Close, NULL out-pointers, handles used after Close or release all return an
// error (with a record where an error code is returned).

// sdrplay_api_log::log through m_serviceState.log; writes nothing when the
// logger is NULL.
void apiLog(sdrplay_api_DbgLvl_t lvl, const char *function, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
// Same, but silently skipped without a logger (device threads after Close,
// when the logger may already be freed).
void threadLog(sdrplay_api_DbgLvl_t lvl, const char *function, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
// Client-global error record (m_serviceState.lastErr).
void setClientError(const char *file, const char *function, int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
// Unconditional LOG_INFO line "[tid]: <text>" (Entry/Exit, Open/Close, ...).
void syslogRaw(const char *text);

// Registry of live sdrplay_api_device objects. ReleaseDevice frees the object
// but leaves DeviceT.dev pointing at it, so later use would be a use-after-free.
// We use the registry to keep those paths safe.
void registerDevice(sdrplay_api_device *d);
void unregisterDevice(sdrplay_api_device *d);
bool isLiveDevice(const void *d);
bool anyLiveDevice();
// True for a pointer to a device object this library freed in ReleaseDevice
// and has not reallocated since (calls on it return an error instead of
// touching freed memory).
void noteFreedDevice(const void *d);
// Releases the one-slot device storage cache (api.cpp, sdrplay_api_device::operator new).
void dropFreedDeviceStorage();
bool isFreedDevice(const void *d);

// Selections this process holds: one entry per successful SelectDevice (serial
// number, tuner and RSPduo mode from the caller's DeviceT, and the device object),
// removed by the ReleaseDevice that releases it. Open and Close leave them (the
// device objects and the service's selection outlive the session).
void noteSelection(const sdrplay_api_DeviceT &d, sdrplay_api_device *obj);
// Removes the selection a release request refers to and returns its device
// object, or returns nullptr when this process holds no such selection. The
// serial number matches case-insensitively. A request in RSPduo slave mode
// matches only a slave selection of the same tuner; any other request matches
// only a selection that is not a slave. Among several matches the one whose
// device object is `d.dev` is taken.
sdrplay_api_device *takeSelection(const sdrplay_api_DeviceT &d);
void clearSelections();

// A device object released from one of its own callback threads cannot be
// freed under that thread. It is parked here and freed once
// the thread has left (checked on later Select/Release/Close calls).
void parkSelfReleasedDevice(sdrplay_api_device *d, void *selfThread);
void reapParkedDevices();

// Worker-thread identity: which device and role the calling thread works
// for, if any.
enum WorkerRole : int { kStreamWorker, kEventWorker, kHeartbeatWorker };
void setCurrentWorker(const sdrplay_api_device *d, WorkerRole role);
bool isCurrentWorker(const sdrplay_api_device *d, WorkerRole role);

// The sink every client log line goes to (the system log unless a test
// redirected it).
platform::LogSink &logSink();

// Test hooks (internal, not exported): redirect log lines; return to the
// pristine "library just loaded" state (m_serviceState zeroed, not closed).
void setLogSinkForTesting(platform::LogSink *sink);
void resetSessionForTesting();

} // namespace client
} // namespace sdrplay
