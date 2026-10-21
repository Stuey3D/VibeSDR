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

// macOS thread naming, priority and ids.
#include "platform/Thread.h"

#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <unistd.h>

#include <cstring>

namespace sdrplay {
namespace platform {

void setCurrentThreadNameOs(const char *name)
{
    // Darwin: names only the calling thread; up to 63 characters. Truncated to
    // 15 like Linux so names look the same in both debuggers.
    char buf[16];
    std::strncpy(buf, name, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    pthread_setname_np(buf);
}

// System-wide unique thread id (what Activity Monitor, lldb and the unified log
// show), folded into the int32 thread id at the start of each log line.
std::int32_t currentThreadId()
{
    std::uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    return static_cast<std::int32_t>(tid & 0x7fffffffu);
}

std::int32_t currentProcessId() { return static_cast<std::int32_t>(getpid()); }

bool setCurrentThreadPriority(ThreadPriority prio)
{
    // Best effort. Darwin schedules by QoS class; SCHED_RR/FIFO via
    // pthread_setschedparam is accepted for unprivileged threads but only
    // within the default priority band, so the QoS class is set as well.
    const qos_class_t qos = prio == ThreadPriority::Normal ? QOS_CLASS_DEFAULT : QOS_CLASS_USER_INTERACTIVE;
    const bool qosOk = pthread_set_qos_class_self_np(qos, 0) == 0;
    if (prio == ThreadPriority::Normal) return qosOk;
    sched_param sp{};
    const int policy = prio == ThreadPriority::Realtime ? SCHED_FIFO : SCHED_RR;
    sp.sched_priority = sched_get_priority_max(policy);
    return pthread_setschedparam(pthread_self(), policy, &sp) == 0 || qosOk;
}

} // namespace platform
} // namespace sdrplay
