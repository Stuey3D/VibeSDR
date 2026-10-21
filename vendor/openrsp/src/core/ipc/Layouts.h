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

// Byte layouts of the shared-memory maps.
//
// Each map is the file /dev/shm/<name>, of exactly the size below, readable by any process.
// The last request and its answer stay in a map after the call, so every field here is found
// by passing a distinctive value through the API (a maxDevs, a debug level, an rfHz, an update
// reason, ...) and locating it in the file; offsets inside the API structs come from the public
// headers. tools/derive_ipc derives the map sizes and these offsets that way.
//
// Every field position here is part of the wire protocol between library and
// service and of the app-visible ABI (apps write params straight into mCmdMap through
// GetDeviceParams pointers). Structs use the API header types at their native
// alignment; the layout checks at the end lock them on 64-bit targets. They use
// no `long` and no OS types.
//
// TODO(arm64/armhf/Windows/macOS): the layout for other ABIs is not yet
// verified. On LP64 targets it is expected to be identical; on
// ILP32 (armhf) DeviceT is still 96 bytes but pointer fields shrink, so the
// pointer-dependent checks are compiled only on 64-bit and the ILP32 layout must
// be verified against the API 3.15 armhf ABI before that target is enabled.
#pragma once

#include "core/api/ApiTypes.h"
#include "core/ipc/Commands.h"

#include <cstddef>
#include <cstdint>

namespace sdrplay {
namespace ipc {

// ---------------------------------------------------------------- global map
// Glbl\sdrSrvComShMem. One mailbox: request and response share it.
constexpr std::size_t kComShMemSize = 1560;
constexpr std::uint32_t kMaxGetDevices = 16; // client clamps maxDevs to <= 16

struct ApiVersionPayload {        // GlobalCmd::ApiVersion
    float serviceIpcVersion;      // response: 1.02f
};
struct GetDevicesPayload {        // GlobalCmd::GetDevices
    std::uint32_t maxDevs;        // request
    std::uint32_t numDevs;        // response
    sdrplay_api_DeviceT devices[kMaxGetDevices]; // response
};
struct SelectDevicePayload {      // GlobalCmd::SelectDevice
    sdrplay_api_DeviceT device;   // request (copied whole from the app's struct)
    char prefix[32];              // response: e.g. "Glbl\sdrSrvDv00"
    std::uint8_t heartbeatDisabled; // request (the client's DisableHeartbeat flag)
};
struct ReleaseDevicePayload {     // GlobalCmd::ReleaseDevice
    sdrplay_api_DeviceT device;   // request
};
struct DebugEnablePayload {       // GlobalCmd::DebugEnable
    std::uint32_t dbgLvl;         // request; service level is process-wide
};
struct GetLastErrorPayload {      // GlobalCmd::GetLastError
    sdrplay_api_TimedErrorInfoT error; // response
};

struct ComShMem {
    std::uint32_t cmd;            // client -> service
    std::int32_t result;          // service -> client, sdrplay_api_ErrT
    union {
        ApiVersionPayload apiVersion;
        GetDevicesPayload getDevices;
        SelectDevicePayload selectDevice;
        ReleaseDevicePayload releaseDevice;
        DebugEnablePayload debugEnable;
        GetLastErrorPayload getLastError;
        std::uint8_t raw[kComShMemSize - 8];
    } u;
};

// ---------------------------------------------------------- device cmd map
// <P>_mCmdMap. IS the sdrplay_api_DeviceParamsT storage.
constexpr std::size_t kCmdMapSize = 1952;
constexpr std::size_t kInternalParamsAreaSize = 16;

struct CmdMap {
    std::uint32_t cmd;
    std::int32_t result;
    std::uint32_t dbgLvl;         // DeviceCmd::DebugEnable
    std::uint32_t tuner;          // SwapTuner, Update (sdrplay_api_TunerSelectT)
    std::uint32_t reason;         // Update (ReasonForUpdateT)
    std::uint32_t reasonExt1;     // Update (ReasonForUpdateExtension1T)
    std::uint32_t internalReason; // InternalUpdate (ReasonForInternalUpdateT)
    std::uint32_t reserved;
    sdrplay_api_DevParamsT devParams;             // -> deviceParams->devParams
    sdrplay_api_RxChannelParamsT rxChannelA;      // -> deviceParams->rxChannelA
    sdrplay_api_RxChannelParamsT rxChannelB;      // -> deviceParams->rxChannelB
    union {
        sdrplay_api_InternalRfTestT params;       // all zero on RSP1B
        std::uint8_t raw[kInternalParamsAreaSize];
    } internal;
    sdrplay_api_TimedErrorInfoT lastError;        // filled by DeviceCmd::GetLastError
};

// --------------------------------------------------------------- data ring
// <P>_mDataMap: kDataRingSlots slots. Pointer-free: same on every ABI.
constexpr std::size_t kMaxSamplesPerCallback = 16128;
struct DataSlot {
    std::int16_t xiA[kMaxSamplesPerCallback];
    std::int16_t xqA[kMaxSamplesPerCallback];
    std::int16_t xiB[kMaxSamplesPerCallback];
    std::int16_t xqB[kMaxSamplesPerCallback];
    sdrplay_api_StreamCbParamsT paramsA;
    sdrplay_api_StreamCbParamsT paramsB;         // RSPduo dual tuner only
    std::uint32_t reset;                         // both callbacks
    std::uint32_t bufIdx;                        // running counter, 1 = first after Init
};
constexpr std::size_t kDataSlotSize = sizeof(DataSlot);
constexpr std::size_t kDataMapSize = kDataRingSlots * kDataSlotSize;

struct DataMap {
    DataSlot slot[kDataRingSlots];
};

// -------------------------------------------------------------- event ring
// <P>_mEventMap: kEventRingSlots records.
struct EventRecord {
    std::uint32_t eventId;               // sdrplay_api_EventT
    std::uint32_t tuner;                 // sdrplay_api_TunerSelectT
    sdrplay_api_EventParamsT params;     // 16-byte union
};
constexpr std::size_t kEventRecordSize = sizeof(EventRecord);
constexpr std::size_t kEventMapSize = kEventRingSlots * kEventRecordSize;

struct EventMap {
    EventRecord slot[kEventRingSlots];
};

// Sizes of the maps by channel (Hrtbt has no map).
constexpr std::size_t kHrtbtMapSize = 0;

// ------------------------------------------------------------- layout checks
static_assert(sizeof(sdrplay_api_StreamCbParamsT) == 20, "DataSlot layout");
static_assert(offsetof(DataSlot, xqA) == 32256, "DataSlot layout");
static_assert(offsetof(DataSlot, xiB) == 64512, "DataSlot layout");
static_assert(offsetof(DataSlot, xqB) == 96768, "DataSlot layout");
static_assert(offsetof(DataSlot, paramsA) == 129024, "DataSlot layout");
static_assert(offsetof(DataSlot, paramsB) == 129044, "DataSlot layout");
static_assert(offsetof(DataSlot, reset) == 129064, "DataSlot layout");
static_assert(offsetof(DataSlot, bufIdx) == 129068, "DataSlot layout");
static_assert(sizeof(DataSlot) == 129072, "DataSlot layout");
static_assert(sizeof(DataMap) == 1032576, "DataMap size");

static_assert(sizeof(sdrplay_api_EventParamsT) == 16, "EventRecord layout");
static_assert(offsetof(EventRecord, params) == 8, "EventRecord layout");
static_assert(sizeof(EventRecord) == 24, "EventRecord layout");
static_assert(sizeof(EventMap) == 192, "EventMap size");

#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
static_assert(offsetof(ComShMem, cmd) == 0, "ComShMem layout");
static_assert(offsetof(ComShMem, result) == 4, "ComShMem layout");
static_assert(offsetof(ComShMem, u) == 8, "ComShMem layout");
static_assert(offsetof(ComShMem, u.apiVersion.serviceIpcVersion) == 8, "ComShMem layout");
static_assert(offsetof(ComShMem, u.getDevices.maxDevs) == 8, "ComShMem layout");
static_assert(offsetof(ComShMem, u.getDevices.numDevs) == 12, "ComShMem layout");
static_assert(offsetof(ComShMem, u.getDevices.devices) == 16, "ComShMem layout");
static_assert(offsetof(ComShMem, u.getDevices.devices) + kMaxGetDevices * sizeof(sdrplay_api_DeviceT) == 1552,
              "ComShMem layout");
static_assert(offsetof(ComShMem, u.selectDevice.device) == 8, "ComShMem layout");
static_assert(offsetof(ComShMem, u.selectDevice.prefix) == 104, "ComShMem layout");
static_assert(offsetof(ComShMem, u.selectDevice.heartbeatDisabled) == 136, "ComShMem layout");
static_assert(offsetof(ComShMem, u.releaseDevice.device) == 8, "ComShMem layout");
static_assert(offsetof(ComShMem, u.debugEnable.dbgLvl) == 8, "ComShMem layout");
static_assert(offsetof(ComShMem, u.getLastError.error) == 8, "ComShMem layout");
static_assert(offsetof(ComShMem, u.getLastError.error.info) == 16, "ComShMem layout");
static_assert(sizeof(ComShMem) == kComShMemSize, "ComShMem size");

static_assert(offsetof(CmdMap, dbgLvl) == 8, "CmdMap layout");
static_assert(offsetof(CmdMap, tuner) == 12, "CmdMap layout");
static_assert(offsetof(CmdMap, reason) == 16, "CmdMap layout");
static_assert(offsetof(CmdMap, reasonExt1) == 20, "CmdMap layout");
static_assert(offsetof(CmdMap, internalReason) == 24, "CmdMap layout");
static_assert(offsetof(CmdMap, devParams) == 32, "CmdMap layout");
static_assert(offsetof(CmdMap, rxChannelA) == 96, "CmdMap layout");
static_assert(offsetof(CmdMap, rxChannelB) == 240, "CmdMap layout");
static_assert(offsetof(CmdMap, internal) == 384, "CmdMap layout");
static_assert(offsetof(CmdMap, lastError) == 400, "CmdMap layout");
static_assert(offsetof(CmdMap, lastError.info) == 408, "CmdMap layout");
static_assert(sizeof(CmdMap) == kCmdMapSize, "CmdMap size");
#endif

} // namespace ipc
} // namespace sdrplay
