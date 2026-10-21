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

// Public API types (from the API headers, see cmake/SdrplayHeaders.cmake) plus
// the undocumented internal types that appear in the signatures of the
// library's exported C++ functions (so their names are ABI).
#pragma once

#include <sdrplay_api.h>

#include <cstddef>
#include <cstdint>

// One last-error record: the time of the error (microseconds, wall clock) and its details.
struct sdrplay_api_TimedErrorInfoT {
    std::uint64_t timeUs;
    sdrplay_api_ErrorInfoT info;
};

// RF-test settings of a device, changed through sdrplay_api_InternalUpdate. The struct
// name is ours (not in any export).
struct sdrplay_api_InternalRfTestT {
    int rsp1aRfTestEn;
    int rsp1aRfTestSrcStability;
    int rspDuoRfTestEn;
};

// What sdrplay_api_GetInternalDeviceParams returns: a struct holding a pointer to the
// device's RF-test settings. The type name and its layout are ABI (they appear in exported
// signatures).
struct sdrplay_api_InternalDeviceParamsT {
    sdrplay_api_InternalRfTestT *rfTest;
};

// Reason bits for sdrplay_api_InternalUpdate.
// Enumerator names are ours; the type name is ABI (exported signatures).
// Fixed underlying type: any int bit mask is a valid value (apps pass e.g. 4);
// the exported symbol is the same as for a plain typedef.
enum sdrplay_api_ReasonForInternalUpdateT : int {
    sdrplay_api_InternalUpdate_None = 0x00000000,
    sdrplay_api_InternalUpdate_Rsp1aRfTest = 0x00000001,
    sdrplay_api_InternalUpdate_RspDuoRfTest = 0x00000002
};

// Layout checks (ABI and IPC).
static_assert(sizeof(sdrplay_api_ErrorInfoT) == 1540, "ErrorInfoT layout");
static_assert(sizeof(sdrplay_api_InternalRfTestT) == 12, "InternalRfTestT layout");
static_assert(sizeof(sdrplay_api_ReasonForInternalUpdateT) == 4, "ReasonForInternalUpdateT size");
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
static_assert(sizeof(sdrplay_api_TimedErrorInfoT) == 1552, "TimedErrorInfoT layout");
static_assert(offsetof(sdrplay_api_TimedErrorInfoT, info) == 8, "TimedErrorInfoT layout");
static_assert(sizeof(sdrplay_api_DeviceT) == 96, "DeviceT layout");
static_assert(offsetof(sdrplay_api_DeviceT, hwVer) == 64, "DeviceT layout");
static_assert(offsetof(sdrplay_api_DeviceT, tuner) == 68, "DeviceT layout");
static_assert(offsetof(sdrplay_api_DeviceT, rspDuoMode) == 72, "DeviceT layout");
static_assert(offsetof(sdrplay_api_DeviceT, valid) == 76, "DeviceT layout");
static_assert(offsetof(sdrplay_api_DeviceT, rspDuoSampleFreq) == 80, "DeviceT layout");
static_assert(offsetof(sdrplay_api_DeviceT, dev) == 88, "DeviceT layout");
static_assert(sizeof(sdrplay_api_DevParamsT) == 64, "DevParamsT layout");
static_assert(sizeof(sdrplay_api_RxChannelParamsT) == 144, "RxChannelParamsT layout");
#endif
