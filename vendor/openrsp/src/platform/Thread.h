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

// Platform interface: threads, priorities, sleeping and thread ids.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace sdrplay {
namespace platform {

enum class ThreadPriority { Normal, High, Realtime };

class Thread {
public:
    // Starts fn on a new thread. `name` (<= 15 chars on Linux) is a debugging aid.
    static std::unique_ptr<Thread> start(std::function<void()> fn, const char *name,
                                         ThreadPriority prio = ThreadPriority::Normal);
    // Joins. Destructor joins if still joinable.
    ~Thread();
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;

    // Waits for the thread to finish. timeoutMs == UINT32_MAX waits forever.
    // Returns true when the thread has finished and was joined.
    bool join(std::uint32_t timeoutMs = UINT32_MAX);

    struct Impl;

private:
    explicit Thread(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Kernel thread id as printed in the "[%d]: " log prefix (gettid on Linux).
std::int32_t currentThreadId();
// Process id.
std::int32_t currentProcessId();
// Best effort; returns false if not permitted.
bool setCurrentThreadPriority(ThreadPriority prio);
void sleepMs(std::uint32_t ms);

} // namespace platform
} // namespace sdrplay
