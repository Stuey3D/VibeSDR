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

// Windows platform helpers: object-name mapping, NULL-DACL attributes, ntdll queries.
#include "platform/windows/WinCommon.h"

#include <mutex>

namespace sdrplay {
namespace platform {
namespace win {

namespace {
bool startsWith(const std::string &s, const char *p)
{
    const std::string pre(p);
    return s.compare(0, pre.size(), pre) == 0;
}

// ntdll information classes / structures (stable since NT 3.x; declared locally so no DDK
// headers are needed).
struct SemaphoreBasicInformation {
    LONG CurrentCount;
    LONG MaximumCount;
};
struct SectionBasicInformation {
    PVOID BaseAddress;
    ULONG AllocationAttributes;
    LARGE_INTEGER MaximumSize;
};
using NtQuerySemaphoreFn = LONG(WINAPI *)(HANDLE, int, PVOID, ULONG, PULONG);
using NtQuerySectionFn = LONG(WINAPI *)(HANDLE, int, PVOID, SIZE_T, PSIZE_T);

FARPROC ntdllProc(const char *name)
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt ? GetProcAddress(nt, name) : nullptr;
}
} // namespace

std::string objectName(const std::string &name)
{
    if (startsWith(name, "Global\\") || startsWith(name, "Local\\") || startsWith(name, "Session\\")) return name;
    if (startsWith(name, "Glbl\\")) return "Global\\" + name.substr(5);
    if (name.find('\\') == std::string::npos) return name;
    std::string n = name;
    for (char &c : n)
        if (c == '\\') c = '_';
    return "Local\\" + n;
}

SECURITY_ATTRIBUTES *nullDaclAttributes()
{
    // A security descriptor with a NULL DACL grants everyone access, so a client running as
    // another user can open the service's named objects. Handles are not inheritable.
    static SECURITY_DESCRIPTOR sd;
    static SECURITY_ATTRIBUTES sa;
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ok = InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
             SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
    });
    return ok ? &sa : nullptr;
}

bool querySemaphore(HANDLE h, long *current, long *maximum)
{
    static const auto fn = reinterpret_cast<NtQuerySemaphoreFn>(
        reinterpret_cast<void (*)()>(ntdllProc("NtQuerySemaphore")));
    if (!fn || !h) return false;
    SemaphoreBasicInformation info{};
    ULONG len = 0;
    if (fn(h, 0 /* SemaphoreBasicInformation */, &info, sizeof info, &len) < 0) return false;
    if (current) *current = info.CurrentCount;
    if (maximum) *maximum = info.MaximumCount;
    return true;
}

bool querySectionSize(HANDLE h, std::uint64_t *size)
{
    static const auto fn = reinterpret_cast<NtQuerySectionFn>(
        reinterpret_cast<void (*)()>(ntdllProc("NtQuerySection")));
    if (!fn || !h) return false;
    SectionBasicInformation info{};
    SIZE_T len = 0;
    if (fn(h, 0 /* SectionBasicInformation */, &info, sizeof info, &len) < 0) return false;
    if (size) *size = static_cast<std::uint64_t>(info.MaximumSize.QuadPart);
    return true;
}

std::string toUtf8(const wchar_t *w)
{
    if (!w || !*w) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(static_cast<std::size_t>(n - 1));
    return s;
}

std::wstring toWide(const std::string &s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(static_cast<std::size_t>(n - 1));
    return w;
}

int handleToInt(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE) return -1;
    return static_cast<int>(reinterpret_cast<std::intptr_t>(h));
}

} // namespace win
} // namespace platform
} // namespace sdrplay
