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

// Shared (inter-process) futex helpers for the Android v2 sync objects (AndroidShm.h).
#pragma once

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <ctime>
#include <linux/futex.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace sdrplay {
namespace platform {
namespace android_shm {

// Waits while *word == expected, at most `ms` milliseconds (relative, CLOCK_MONOTONIC).
// Returns 0 when woken (or the value differed), ETIMEDOUT, or EINTR.
inline int futexWaitMs(std::atomic<std::uint32_t> *word, std::uint32_t expected, std::uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ms / 1000u);
    ts.tv_nsec = static_cast<long>(ms % 1000u) * 1000000L;
    const long rc = syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(word), FUTEX_WAIT, expected, &ts, nullptr, 0);
    if (rc == 0) return 0;
    if (errno == EAGAIN) return 0; // value already changed
    return errno;
}

inline void futexWake(std::atomic<std::uint32_t> *word, int n)
{
    syscall(SYS_futex, reinterpret_cast<std::uint32_t *>(word), FUTEX_WAKE, n, nullptr, nullptr, 0);
}

inline void futexWakeAll(std::atomic<std::uint32_t> *word) { futexWake(word, INT_MAX); }

// True when thread/process id `tid` certainly no longer exists. kill(tid, 0) works on any
// thread id of the pid namespace, also across apps (EPERM = exists) where /proc is hidden.
inline bool tidIsDead(std::uint32_t tid)
{
    if (tid == 0 || tid > static_cast<std::uint32_t>(INT_MAX)) return false;
    return kill(static_cast<pid_t>(tid), 0) != 0 && errno == ESRCH;
}

} // namespace android_shm
} // namespace platform
} // namespace sdrplay
