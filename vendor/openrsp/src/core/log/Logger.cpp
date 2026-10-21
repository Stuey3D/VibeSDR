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

#include "core/log/Logger.h"

#include "platform/Thread.h"

#include <cstdio>
#include <vector>

namespace sdrplay {
namespace log {

Logger::Logger(platform::LogSink *sink, std::string component, Level level)
    : sink_(sink), component_(std::move(component)), level_(static_cast<int>(level))
{
}

bool Logger::enabled(Level l) const
{
    int cur = level_.load();
    return cur != 0 && static_cast<int>(l) >= cur;
}

std::string vformat(const char *fmt, std::va_list ap)
{
    std::va_list ap2;
    va_copy(ap2, ap);
    char small[512];
    int n = std::vsnprintf(small, sizeof small, fmt, ap2);
    va_end(ap2);
    if (n < 0) return std::string();
    if (static_cast<std::size_t>(n) < sizeof small) return std::string(small, static_cast<std::size_t>(n));
    std::vector<char> big(static_cast<std::size_t>(n) + 1);
    std::vsnprintf(big.data(), big.size(), fmt, ap);
    return std::string(big.data(), static_cast<std::size_t>(n));
}

platform::LogPriority Logger::priorityFor(Level l)
{
    switch (l) {
    case Level::Error: return platform::LogPriority::Error;
    case Level::Warning: return platform::LogPriority::Warning;
    default: return platform::LogPriority::Info;
    }
}

std::string Logger::format(std::int32_t tid, const std::string &component, Level l, const char *func,
                           const std::string &msg)
{
    const char *tag = "";
    if (l == Level::Error) tag = "ERROR: ";
    else if (l == Level::Warning) tag = "WARNING: ";
    else if (l == Level::Message) tag = "MSG: ";
    std::string s = "[" + std::to_string(tid) + "]: " + component + ": " + (func ? func : "") + ": " + tag;
    s += msg;
    return s;
}

void Logger::vlog(Level l, const char *func, const char *fmt, std::va_list ap)
{
    if (!sink_ || !enabled(l)) return;
    sink_->write(priorityFor(l), format(platform::currentThreadId(), component_, l, func, vformat(fmt, ap)));
}

void Logger::log(Level l, const char *func, const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    vlog(l, func, fmt, ap);
    va_end(ap);
}

void Logger::raw(const char *fmt, ...)
{
    if (!sink_) return;
    std::va_list ap;
    va_start(ap, fmt);
    std::string msg = vformat(fmt, ap);
    va_end(ap);
    sink_->write(platform::LogPriority::Info, "[" + std::to_string(platform::currentThreadId()) + "]: " + msg);
}

} // namespace log
} // namespace sdrplay
