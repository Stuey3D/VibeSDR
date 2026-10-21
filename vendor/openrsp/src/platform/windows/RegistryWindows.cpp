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

// Read-only registry lookups of the service (it never writes the registry). The key and
// value names are the ones existing installations of the API use, so their settings apply.
#include "platform/windows/WindowsService.h"
#include "platform/windows/WinCommon.h"

namespace sdrplay {
namespace platform {
namespace windows {

namespace {
// Reads a 4-byte value under HKLM\<path>. The value type is not checked: any value of 4
// bytes or fewer is accepted.
bool readDword(const wchar_t *path, const wchar_t *value, DWORD *out)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD v = 0, len = sizeof v;
    const LSTATUS rc = RegQueryValueExW(k, value, nullptr, nullptr, reinterpret_cast<BYTE *>(&v), &len);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) return false;
    *out = v;
    return true;
}
} // namespace

int readDebugOverride()
{
    DWORD v = 0;
    if (!readDword(L"SOFTWARE\\SdrplayServiceApi", L"Debug_Override", &v)) return 0;
    const int level = static_cast<int>(v);
    return level > 0 ? level : 0;
}

} // namespace windows
} // namespace platform
} // namespace sdrplay
