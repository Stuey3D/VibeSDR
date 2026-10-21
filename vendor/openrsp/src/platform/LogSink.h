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

// Platform interface: system log sink.
//
// Linux: syslog(3) WITHOUT openlog (ident = program name, facility LOG_USER).
// Other OSes (TODO): Windows debug output / Event Log, os_log, logcat.
#pragma once

#include <memory>
#include <string>

namespace sdrplay {
namespace platform {

// Values are syslog priorities.
enum class LogPriority : int { Error = 3, Warning = 4, Info = 6 };

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(LogPriority prio, const std::string &line) = 0;
};

// The OS system log. Never null.
std::unique_ptr<LogSink> createSystemLogSink();

} // namespace platform
} // namespace sdrplay
