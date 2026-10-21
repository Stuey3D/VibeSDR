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

// Shared-memory object names.
//
// The names are visible to any process: on Linux each object (map, semaphore, mutex) is the
// file /dev/shm/<name>, mode 0666, for as long as it exists. `ls /dev/shm` shows the four
// global objects from service start to service exit, and the <prefix>_... device objects from
// SelectDevice to ReleaseDevice; the prefix itself is in the SelectDevice answer, which stays in
// the global mailbox. tools/derive_ipc derives the whole scheme this way (ctest
// derive_ipc_check).
//
// The names contain a literal backslash: "Glbl\sdrSrv..." is the Windows
// "Global\" kernel namespace carried over; on Linux it is just part of the
// /dev/shm file name.
#pragma once

#include <cstddef>
#include <string>

namespace sdrplay {
namespace ipc {

// Kernel-object namespace of every name. Windows: the real Win32 "Global\"
// namespace; it is also part of the device prefix the service sends in the
// SelectDevice reply, which existing client libraries open verbatim, so it must be
// spelt "Global\" on the wire. Linux/macOS/Android:
// the shortened form "Glbl\" (part of the /dev/shm file name).
#if defined(_WIN32)
#define SDRPLAY_IPC_NS "Global\\"
#else
#define SDRPLAY_IPC_NS "Glbl\\"
#endif

// Global channel, created at service start, unlinked at service exit.
constexpr const char kComShMemName[] = SDRPLAY_IPC_NS "sdrSrvComShMem"; // map (ComShMem)
constexpr const char kComMtxName[] = SDRPLAY_IPC_NS "sdrSrvComMtx";     // mutex
constexpr const char kCmdSemaName[] = SDRPLAY_IPC_NS "sdrSrvCmdSema";   // sem 0/1
constexpr const char kRespSemaName[] = SDRPLAY_IPC_NS "sdrSrvRespSema"; // sem 0/1

constexpr unsigned kMaxDeviceSlots = 16; // service device table (SDRPLAY_MAX_DEVICES)

// Service-side object names are at most 29 characters (a longer name is a fatal
// service error). The client uses a 64-byte buffer.
constexpr std::size_t kServiceMaxNameLen = 29;
// Buffer for the formatted device prefix (devicePrefix), which is truncated to
// fit: at most 30 characters on Windows, 22 elsewhere.
#if defined(_WIN32)
constexpr std::size_t kDevicePrefixBufLen = 31;
#else
constexpr std::size_t kDevicePrefixBufLen = 23;
#endif
constexpr std::size_t kPrefixFieldLen = 32;        // SelectDevicePayload::prefix

enum class Side { Master, Slave }; // "m" / "s" (slave only for RSPduo dual tuner)
enum class Channel { Hrtbt, Cmd, Data, Event };
enum class Kind { Sem, RSem, Map };

// TEST HOOK: a string prepended to every object name (default empty = the
// standard names). Lets unit tests run a service and a client in one process
// without touching the real Glbl\... objects of a running service.
// Not thread-safe: set it before creating any object. Keep it short (<= 8
// chars): the device prefix must fit the 32-byte ComShMem field.
void setObjectNamePrefix(const std::string &prefix);
const std::string &objectNamePrefix();

// objectNamePrefix() + base, for the global objects (kComShMemName, ...).
std::string globalName(const char *base);

// objectNamePrefix() + SDRPLAY_IPC_NS "sdrSrvDv%02d" for slot 0..15 ("Glbl\sdrSrvDv00",
// on Windows "Global\sdrSrvDv00", is the first RSP).
std::string devicePrefix(unsigned slot);

// "<prefix>_<m|s><Hrtbt|Cmd|Data|Event><Sem|RSem|Map>"
// (format "%s_%sSem" / "%s_%sRSem" / "%s_%sMap"). There is no Hrtbt map.
std::string deviceObjectName(const std::string &prefix, Side side, Channel ch, Kind kind);

// True if the service would accept the name (<= 29 chars, test prefix excluded).
bool fitsServiceNameLimit(const std::string &name);

} // namespace ipc
} // namespace sdrplay
