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

// Android "service host". Apps have no daemon: the service core is embedded
// in-process with sdrplay_android_service_start() (EmbeddedService.cpp), which does
// not use runServiceHost(). This implementation exists for the stand-alone
// sdrplay_apiService executable when it is run from an adb shell (tests): same
// behaviour as Linux (SIGINT/SIGQUIT/SIGTERM request stop, SIGPIPE ignored).
#include "platform/ServiceHost.h"

#include <climits>
#include <csignal>
#include <unistd.h>

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

// Inside an app this is the zygote's app_process directory (meaningless for data
// files); the embedded service is given its firmware directory explicitly.
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
