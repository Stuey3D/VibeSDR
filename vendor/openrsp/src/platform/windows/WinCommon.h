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

// Shared helpers of the Windows platform layer.
// Windows-only header: includes <windows.h>.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>

namespace sdrplay {
namespace platform {
namespace win {

// Kernel-object name for a portable IPC name:
//   "Global\..." / "Local\..." / "Session\..."  unchanged (the Windows IPC names)
//   "Glbl\..."                                  -> "Global\..." (the core still spells the
//                                                  Linux prefix)
//   any other name containing '\'               -> "Local\" + name with '\' -> '_'
//                                                  (unit-test prefixes such as "t123_Glbl\...":
//                                                  session-private, no SeCreateGlobalPrivilege)
//   names without '\'                           unchanged (session namespace)
std::string objectName(const std::string &name);

// The one SECURITY_ATTRIBUTES of the service: a NULL DACL (every user may open the
// objects, like the Linux mode 0666), bInheritHandle FALSE. Process-lifetime storage.
SECURITY_ATTRIBUTES *nullDaclAttributes();

// NtQuerySemaphore (ntdll, SemaphoreBasicInformation). False if unavailable or it failed.
bool querySemaphore(HANDLE h, long *current, long *maximum);
// NtQuerySection (ntdll, SectionBasicInformation): the section's exact size. False if unknown.
bool querySectionSize(HANDLE h, std::uint64_t *size);

// UTF-16 <-> UTF-8.
std::string toUtf8(const wchar_t *w);
std::wstring toWide(const std::string &s);

// Integer value of a HANDLE for the int-typed nativeHandle() accessors (kernel handles are
// small multiples of 4, so they fit; -1 for NULL).
int handleToInt(HANDLE h);

} // namespace win
} // namespace platform
} // namespace sdrplay
