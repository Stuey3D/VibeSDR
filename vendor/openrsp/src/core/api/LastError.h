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

// TimedErrorInfo records (the last-error records of the client and the service).
#pragma once

#include "core/api/ApiTypes.h"

#include <cstdarg>
#include <cstdint>

namespace sdrplay {
namespace api {

// Fills `rec`: time = timeUs, file/function copied and truncated to 255
// characters (null clears the field), line, message = vsnprintf(fmt, ...) with
// a 1023-byte buffer limit (at most 1022 characters).
void setLastErrorV(sdrplay_api_TimedErrorInfoT *rec, std::uint64_t timeUs, const char *file, const char *function,
                   int line, const char *fmt, std::va_list ap)
#if defined(__GNUC__)
    __attribute__((format(printf, 6, 0)))
#endif
    ;
void setLastError(sdrplay_api_TimedErrorInfoT *rec, std::uint64_t timeUs, const char *file, const char *function,
                  int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 6, 7)))
#endif
    ;

// Newest-record selection used by sdrplay_api_GetLastError:
// candidates are compared against the first; ties keep the earlier one.
// Returns nullptr if every record has time 0.
const sdrplay_api_TimedErrorInfoT *newestError(const sdrplay_api_TimedErrorInfoT *const *recs, int n);

} // namespace api
} // namespace sdrplay
