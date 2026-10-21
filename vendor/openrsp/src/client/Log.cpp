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

// sdrplay_api_log: level 3 by default; a
// message is printed iff level != 0 && msgLvl >= level and
// msgLvl is 1..4; other levels pass the gate, are not printed and return -1.
// Lines go to the system log (client::logSink()) as
// "[tid]: sdrplay_api: <func>: [WARNING: |ERROR: |MSG: ]<msg>".
#include "client/ClientAbi.h"
#include "client/ServiceConnection.h"

#include "core/log/Logger.h"
#include "platform/Thread.h"

#include <cstdarg>

sdrplay_api_log::sdrplay_api_log() : level_(sdrplay_api_DbgLvl_Error) {}

sdrplay_api_log::~sdrplay_api_log() = default;

void sdrplay_api_log::log_enable(sdrplay_api_DbgLvl_t lvl) { level_ = static_cast<int>(lvl); } // no validation

sdrplay_api_DbgLvl_t sdrplay_api_log::get_log_enable() { return static_cast<sdrplay_api_DbgLvl_t>(level_.load()); }

bool sdrplay_api_log::passes(sdrplay_api_DbgLvl_t msgLvl) const
{
    const int level = level_.load();
    return level != 0 && static_cast<int>(msgLvl) >= level;
}

void sdrplay_api_log::vwrite(sdrplay_api_DbgLvl_t msgLvl, const char *function, const char *fmt, std::va_list ap)
{
    using sdrplay::log::Level;
    using sdrplay::log::Logger;
    const auto l = static_cast<Level>(msgLvl);
    sdrplay::client::logSink().write(Logger::priorityFor(l),
                                     Logger::format(sdrplay::platform::currentThreadId(), "sdrplay_api", l, function,
                                                    sdrplay::log::vformat(fmt, ap)));
}

int sdrplay_api_log::vlog(sdrplay_api_DbgLvl_t msgLvl, const char *function, const char *fmt, std::va_list ap)
{
    if (!passes(msgLvl)) return 0;
    if (msgLvl < sdrplay_api_DbgLvl_Verbose || msgLvl > sdrplay_api_DbgLvl_Message) return -1;
    vwrite(msgLvl, function, fmt, ap);
    return 0;
}

int sdrplay_api_log::log(sdrplay_api_DbgLvl_t lvl, const char *function, char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    const int r = vlog(lvl, function, fmt, ap);
    va_end(ap);
    return r;
}
