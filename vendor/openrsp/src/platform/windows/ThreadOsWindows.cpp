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

// Windows thread naming, priority and ids (common/ThreadStd.cpp runs the threads).
#include "platform/Thread.h"
#include "platform/windows/WinCommon.h"

namespace sdrplay {
namespace platform {

void setCurrentThreadNameOs(const char *name)
{
    // SetThreadDescription exists from Windows 10 1607; resolved at run time so the binaries
    // still load elsewhere. Debugging aid only.
    using Fn = HRESULT(WINAPI *)(HANDLE, PCWSTR);
    static const Fn fn = [] {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        return k ? reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(k, "SetThreadDescription")))
                 : nullptr;
    }();
    if (!fn || !name) return;
    const std::wstring w = win::toWide(name);
    fn(GetCurrentThread(), w.c_str());
}

std::int32_t currentThreadId() { return static_cast<std::int32_t>(GetCurrentThreadId()); }
std::int32_t currentProcessId() { return static_cast<std::int32_t>(GetCurrentProcessId()); }

bool setCurrentThreadPriority(ThreadPriority prio)
{
    // Best effort for callers that ask.
    int p = THREAD_PRIORITY_NORMAL;
    if (prio == ThreadPriority::High) p = THREAD_PRIORITY_ABOVE_NORMAL;
    else if (prio == ThreadPriority::Realtime) p = THREAD_PRIORITY_TIME_CRITICAL;
    return SetThreadPriority(GetCurrentThread(), p) != 0;
}

} // namespace platform
} // namespace sdrplay
