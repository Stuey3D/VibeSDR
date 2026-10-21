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

// Linux thread naming, priority and ids.
#include "platform/Thread.h"

#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstring>

namespace sdrplay {
namespace platform {

void setCurrentThreadNameOs(const char *name)
{
    char buf[16];
    std::strncpy(buf, name, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    pthread_setname_np(pthread_self(), buf);
}

std::int32_t currentThreadId() { return static_cast<std::int32_t>(syscall(SYS_gettid)); }
std::int32_t currentProcessId() { return static_cast<std::int32_t>(getpid()); }

// Static priorities of the two raised levels (1..99 range of SCHED_FIFO / SCHED_RR).
constexpr int kRealtimeFifoPriority = 10;
constexpr int kHighRrPriority = 5;

bool setCurrentThreadPriority(ThreadPriority prio)
{
    // Best effort only: a failure to raise the priority is not an error.
    sched_param sp{};
    int policy = SCHED_OTHER;
    if (prio == ThreadPriority::Realtime) {
        policy = SCHED_FIFO;
        sp.sched_priority = kRealtimeFifoPriority;
    } else if (prio == ThreadPriority::High) {
        policy = SCHED_RR;
        sp.sched_priority = kHighRrPriority;
    }
    return pthread_setschedparam(pthread_self(), policy, &sp) == 0;
}

} // namespace platform
} // namespace sdrplay
