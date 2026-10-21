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

// Linux daemon host (systemd unit "sdrplay" / init.d): foreground process,
// SIGINT/SIGQUIT/SIGTERM request stop, SIGPIPE ignored, everything else default
// No args / env parsed. A stop that does not complete within kStopGraceSeconds
// ends the process.
#include "platform/ServiceHost.h"

#include <csignal>
#include <climits>
#include <unistd.h>

namespace sdrplay {
namespace platform {

namespace {
volatile std::sig_atomic_t g_stop = 0;

// Bounded stop: a thread stuck in the IPC objects must not make the service ignore
// SIGTERM for systemd's whole TimeoutStopSec. A normal shutdown takes ~1-2 s;
// if a clean shutdown has not finished kStopGraceSeconds after the first stop signal,
// the process exits with status 1 (stale /dev/shm objects are discarded by the next
// start). Both calls are async-signal-safe.
constexpr unsigned kStopGraceSeconds = 20;

extern "C" void onStopGraceExpired(int) { _exit(1); }

extern "C" void onStopSignal(int)
{
    if (!g_stop) alarm(kStopGraceSeconds);
    g_stop = 1;
}

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
    struct sigaction ga {};
    ga.sa_handler = onStopGraceExpired;
    sigemptyset(&ga.sa_mask);
    sigaction(SIGALRM, &ga, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    SignalStop stop;
    return body(stop);
}

std::string executableDirectory()
{
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return std::string();
    std::string p(buf, static_cast<std::size_t>(n));
    const std::size_t slash = p.rfind('/');
    return slash == std::string::npos ? std::string() : p.substr(0, slash);
}

} // namespace platform
} // namespace sdrplay
