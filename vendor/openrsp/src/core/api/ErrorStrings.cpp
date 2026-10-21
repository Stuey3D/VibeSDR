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

#include "core/api/ErrorStrings.h"

namespace sdrplay {
namespace api {

const char *errorString(int err)
{
    static const char *const table[] = {
        "sdrplay_api_Success",
        "sdrplay_api_Fail",
        "sdrplay_api_InvalidParam",
        "sdrplay_api_OutOfRange",
        "sdrplay_api_GainUpdateError",
        "sdrplay_api_RfUpdateError",
        "sdrplay_api_FsUpdateError",
        "sdrplay_api_HwError",
        "sdrplay_api_AliasingError",
        "sdrplay_api_AlreadyInitialised",
        "sdrplay_api_NotInitialised",
        "sdrplay_api_NotEnabled",
        "sdrplay_api_HwVerError",
        "sdrplay_api_OutOfMemError",
        "sdrplay_api_ServiceNotResponding",
        "sdrplay_api_StartPending",
        "sdrplay_api_StopPending",
        "sdrplay_api_InvalidMode",
        "sdrplay_api_FailedVerification1",
        "sdrplay_api_FailedVerification2",
        "sdrplay_api_FailedVerification3",
        "sdrplay_api_FailedVerification4",
        "sdrplay_api_FailedVerification5",
        "sdrplay_api_FailedVerification6",
        "sdrplay_api_InvalidServiceVersion",
    };
    static_assert(sizeof(table) / sizeof(table[0]) == sdrplay_api_InvalidServiceVersion + 1, "one per ErrT");
    // Values outside the enum, negative ones included, return UnknownError.
    if (err < 0 || err > sdrplay_api_InvalidServiceVersion) return "sdrplay_api_UnknownError";
    return table[err];
}

} // namespace api
} // namespace sdrplay
