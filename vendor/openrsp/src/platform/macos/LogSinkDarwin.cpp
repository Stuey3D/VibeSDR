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

// macOS LogSink: syslog(3) WITHOUT openlog() and no os_log. On current macOS syslog() feeds the
// unified log: `log show --predicate 'process == "sdrplay_apiService"'`.
#include "platform/LogSink.h"

#include <syslog.h>

namespace sdrplay {
namespace platform {

namespace {
class SyslogSink final : public LogSink {
public:
    void write(LogPriority prio, const std::string &line) override
    {
        syslog(static_cast<int>(prio), "%s", line.c_str());
    }
};
} // namespace

std::unique_ptr<LogSink> createSystemLogSink() { return std::make_unique<SyslogSink>(); }

} // namespace platform
} // namespace sdrplay
