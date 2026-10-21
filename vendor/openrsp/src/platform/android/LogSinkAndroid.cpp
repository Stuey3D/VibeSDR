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

// Android LogSink: logcat via __android_log_write. There is no existing Android
// stack to match (no syslog ident to copy), so every line goes out under one tag,
// "sdrplay_api", with the syslog priorities mapped onto logcat's.
#include "platform/LogSink.h"

#include <android/log.h>

namespace sdrplay {
namespace platform {

namespace {
constexpr const char *kTag = "sdrplay_api";

int toLogcat(LogPriority p)
{
    switch (p) {
    case LogPriority::Error: return ANDROID_LOG_ERROR;
    case LogPriority::Warning: return ANDROID_LOG_WARN;
    case LogPriority::Info: return ANDROID_LOG_INFO;
    }
    return ANDROID_LOG_INFO;
}

class LogcatSink final : public LogSink {
public:
    void write(LogPriority prio, const std::string &line) override
    {
        __android_log_write(toLogcat(prio), kTag, line.c_str());
    }
};
} // namespace

std::unique_ptr<LogSink> createSystemLogSink() { return std::make_unique<LogcatSink>(); }

} // namespace platform
} // namespace sdrplay
