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

// Windows LogSink: OutputDebugStringA only (no Event Log, no file). View with DebugView or a
// debugger.
//
// Windows log lines use the API's Windows format, without a thread tag
// ("sdrplay_apiService: %s: %s" in the service, "sdrplay_api: %s: %s" in the client), so the
// sink strips the "[%d]: " tag the portable core Logger prepends. No newline is added (the
// formatted buffer is passed as is).
#include "platform/LogSink.h"
#include "platform/windows/WinCommon.h"

namespace sdrplay {
namespace platform {

namespace {
// Length of a leading "[<digits>]: " (optionally "-<digits>"), 0 if the line has none.
std::size_t tidTagLength(const std::string &s)
{
    if (s.size() < 4 || s[0] != '[') return 0;
    std::size_t i = 1;
    if (i < s.size() && s[i] == '-') ++i;
    const std::size_t digits = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == digits || s.compare(i, 3, "]: ") != 0) return 0;
    return i + 3;
}

class DebugOutputSink final : public LogSink {
public:
    void write(LogPriority, const std::string &line) override
    {
        OutputDebugStringA(line.c_str() + tidTagLength(line));
    }
};
} // namespace

std::unique_ptr<LogSink> createSystemLogSink() { return std::make_unique<DebugOutputSink>(); }

} // namespace platform
} // namespace sdrplay
