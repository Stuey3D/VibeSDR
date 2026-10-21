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

// macOS daemon host for launchd (LaunchDaemon com.sdrplay.service, RunAtLoad +
// KeepAlive, root). A plain foreground
// process (launchd daemons must not fork/daemonise), SIGINT/SIGQUIT/SIGTERM
// request stop (launchctl bootout/unload sends SIGTERM, then SIGKILL after
// ExitTimeOut, 20 s by default), SIGPIPE ignored, everything else default.
// No arguments or environment variables are read here; the service's own
// variables arrive through the plist's EnvironmentVariables.
#include "platform/ServiceHost.h"

#include <mach-o/dyld.h>

#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace sdrplay {
namespace platform {

namespace {
volatile std::sig_atomic_t g_stop = 0;

extern "C" void onStopSignal(int) { g_stop = 1; }

class SignalStop final : public StopSignal {
public:
    bool stopRequested() const override { return g_stop != 0; }
};
} // namespace

int runServiceHost(const std::function<int(const StopSignal &)> &body)
{
    struct sigaction sa {};
    sa.sa_handler = onStopSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    SignalStop stop;
    return body(stop);
}

std::string executableDirectory()
{
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> raw(size + 1, '\0');
    if (_NSGetExecutablePath(raw.data(), &size) != 0) return std::string();
    char resolved[PATH_MAX];
    std::string p = realpath(raw.data(), resolved) ? std::string(resolved) : std::string(raw.data());
    const std::size_t slash = p.rfind('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

} // namespace platform
} // namespace sdrplay
