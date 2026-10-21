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

// Android thread naming, priority and ids (bionic).
#include "platform/Thread.h"

#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstring>

namespace sdrplay {
namespace platform {

void setCurrentThreadNameOs(const char *name)
{
    char buf[16]; // bionic: 15 chars + NUL, ERANGE otherwise
    std::strncpy(buf, name, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    pthread_setname_np(pthread_self(), buf);
}

std::int32_t currentThreadId() { return static_cast<std::int32_t>(gettid()); }
std::int32_t currentProcessId() { return static_cast<std::int32_t>(getpid()); }

// android.os.Process nice values of Android's own audio threads.
constexpr int kThreadPriorityAudio = -16;
constexpr int kThreadPriorityUrgentAudio = -19;

bool setCurrentThreadPriority(ThreadPriority prio)
{
    // Apps may not use SCHED_FIFO/RR; Android's own audio threads use nice values, which an
    // app may set for its own threads. setpriority(PRIO_PROCESS, tid) is per thread
    // on Linux. Best effort: false if refused (e.g. the adb shell user).
    int nice = 0;
    if (prio == ThreadPriority::High) nice = kThreadPriorityAudio;
    else if (prio == ThreadPriority::Realtime) nice = kThreadPriorityUrgentAudio;
    return setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), nice) == 0;
}

} // namespace platform
} // namespace sdrplay
