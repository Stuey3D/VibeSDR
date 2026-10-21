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

#include "core/api/LastError.h"

#include <cstdio>

namespace sdrplay {
namespace api {

void setLastErrorV(sdrplay_api_TimedErrorInfoT *rec, std::uint64_t timeUs, const char *file, const char *function,
                   int line, const char *fmt, std::va_list ap)
{
    if (!rec) return;
    rec->timeUs = timeUs;
    // File and function are truncated to fit their fields; a null pointer clears the field.
    std::snprintf(rec->info.file, sizeof rec->info.file, "%s", file ? file : "");
    std::snprintf(rec->info.function, sizeof rec->info.function, "%s", function ? function : "");
    rec->info.line = line;
    // The message limit is one byte short of the field.
    if (fmt)
        std::vsnprintf(rec->info.message, sizeof rec->info.message - 1, fmt, ap);
    else
        rec->info.message[0] = '\0';
}

void setLastError(sdrplay_api_TimedErrorInfoT *rec, std::uint64_t timeUs, const char *file, const char *function,
                  int line, const char *fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    setLastErrorV(rec, timeUs, file, function, line, fmt, ap);
    va_end(ap);
}

const sdrplay_api_TimedErrorInfoT *newestError(const sdrplay_api_TimedErrorInfoT *const *recs, int n)
{
    const sdrplay_api_TimedErrorInfoT *best = nullptr;
    for (int i = 0; i < n; ++i) {
        const auto *r = recs[i];
        if (!r || r->timeUs == 0) continue;
        if (!best || r->timeUs > best->timeUs) best = r;
    }
    return best;
}

} // namespace api
} // namespace sdrplay
