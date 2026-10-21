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

// Platform interface: hosting the service core as a daemon.
//
// Linux (systemd / init.d): plain foreground process; SIGINT/SIGQUIT/SIGTERM set
// the stop request, SIGPIPE ignored, other signals keep their default action
// No arguments, no environment variables read.
// TODO: Windows Service (ServiceMain/SCM), macOS launchd (same as POSIX),
// Android: no daemon; the core is embedded in-process instead.
#pragma once

#include <functional>
#include <string>

namespace sdrplay {
namespace platform {

class StopSignal {
public:
    virtual ~StopSignal() = default;
    virtual bool stopRequested() const = 0;
};

// Installs the OS stop hooks, runs `body(stop)` on the calling thread and
// returns its exit code.
int runServiceHost(const std::function<int(const StopSignal &)> &body);

// Directory of the running executable (no trailing slash), "" if unknown.
// Used to find data files installed next to the daemon (firmware/).
std::string executableDirectory();

} // namespace platform
} // namespace sdrplay
