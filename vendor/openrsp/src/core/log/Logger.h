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

// Log line formats (tools and users parse them).
//
//   client : "[tid]: sdrplay_api: <func>: [ERROR: |WARNING: |MSG: ]<msg>"
//   service: "[tid]: sdrplay_apiService: <func>: [ERROR: |WARNING: |MSG: ]<msg>"
//   device : "[tid]: devIdx<N>: rsp: <func>: [ERROR: ]<msg>"
// Priorities: ERROR 3, WARNING 4, verbose and MSG 6.
// A line is emitted when its level >= the logger level and the logger level != 0
// (sdrplay_api_DbgLvl_t order: Disable 0, Verbose 1, Warning 2, Error 3, Message 4).
#pragma once

#include "platform/LogSink.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <string>

namespace sdrplay {
namespace log {

enum class Level : int { Disable = 0, Verbose = 1, Warning = 2, Error = 3, Message = 4 };

class Logger {
public:
    // `component` is the text after "[tid]: ", e.g. "sdrplay_api",
    // "sdrplay_apiService" or "devIdx0: rsp".
    Logger(platform::LogSink *sink, std::string component, Level level = Level::Error);

    void setLevel(Level l) { level_.store(static_cast<int>(l)); }
    Level level() const { return static_cast<Level>(level_.load()); }
    bool enabled(Level l) const;

    void log(Level l, const char *func, const char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;
    void vlog(Level l, const char *func, const char *fmt, std::va_list ap)
#if defined(__GNUC__)
        __attribute__((format(printf, 4, 0)))
#endif
        ;

    // Unconditional prio-6 line "[tid]: <text>" (Entry/Exit lines, "sdrplay_api_Open").
    void raw(const char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;

    // Pure formatter, exposed for tests.
    static std::string format(std::int32_t tid, const std::string &component, Level l, const char *func,
                              const std::string &msg);
    static platform::LogPriority priorityFor(Level l);

private:
    platform::LogSink *sink_;
    std::string component_;
    std::atomic<int> level_;
};

std::string vformat(const char *fmt, std::va_list ap)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 0)))
#endif
    ;

} // namespace log
} // namespace sdrplay
