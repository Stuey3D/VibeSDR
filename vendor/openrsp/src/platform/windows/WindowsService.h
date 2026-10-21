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

// Windows-only extras of the platform layer that have no portable interface yet
// (service host, power events, registry). Portable header: no
// <windows.h>, so the service core can include it under #ifdef _WIN32.
#pragma once

#include <functional>
#include <string>

namespace sdrplay {
namespace platform {
namespace windows {

// ---- service identity ----
constexpr const wchar_t kServiceTableName[] = L"sdrplay_apiService"; // SERVICE_TABLE / ctrl handler name
constexpr const char kScmServiceName[] = "SDRplayAPIService";        // sc create name (installer)
constexpr const char kScmDisplayName[] = "SDRplay API Service";

// Power events (a callback registered with
// RegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK)): suspend = PBT_APMSUSPEND
// -> suspendAllDevices, resume = PBT_APMRESUMESUSPEND -> resumeAllDevices; every other
// type is ignored. Set before runServiceHost(); the handler runs on a system thread.
// An empty function unregisters.
void setPowerEventHandler(std::function<void(bool suspend)> handler);

// True when runServiceHost() runs under the SCM (false: console mode).
bool runningAsService();

// ---- registry, read-only ----
// HKLM\SOFTWARE\SdrplayServiceApi\Debug_Override (DWORD); the service reads it once at
// start and uses it as the log level when > 0. Returns 0 when absent or <= 0.
int readDebugOverride();

} // namespace windows
} // namespace platform
} // namespace sdrplay
