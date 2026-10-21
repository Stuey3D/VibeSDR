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

// Portable Thread on std::thread (all OSes). OS-specific bits (naming,
// priority, thread ids) are in <os>/ThreadOs*.cpp via the hooks below.
#include "platform/Thread.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace sdrplay {
namespace platform {

// Implemented per OS.
void setCurrentThreadNameOs(const char *name);

struct Thread::Impl {
    std::thread thread;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};

Thread::Thread(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<Thread> Thread::start(std::function<void()> fn, const char *name, ThreadPriority prio)
{
    auto impl = std::make_unique<Impl>();
    Impl *raw = impl.get();
    std::string nm = name ? name : "";
    raw->thread = std::thread([raw, nm, prio, f = std::move(fn)]() {
        if (!nm.empty()) setCurrentThreadNameOs(nm.c_str());
        if (prio != ThreadPriority::Normal) setCurrentThreadPriority(prio);
        f();
        std::lock_guard<std::mutex> lk(raw->m);
        raw->done = true;
        raw->cv.notify_all();
    });
    return std::unique_ptr<Thread>(new Thread(std::move(impl)));
}

Thread::~Thread()
{
    if (!impl_ || !impl_->thread.joinable()) return;
    if (impl_->thread.get_id() == std::this_thread::get_id())
        impl_->thread.detach(); // destroyed by the thread itself: cannot join
    else
        impl_->thread.join();
}

bool Thread::join(std::uint32_t timeoutMs)
{
    if (!impl_->thread.joinable()) return true;
    // Joining the calling thread itself fails (pthread_join: EDEADLK) instead
    // of throwing.
    if (impl_->thread.get_id() == std::this_thread::get_id()) return false;
    if (timeoutMs != UINT32_MAX) {
        std::unique_lock<std::mutex> lk(impl_->m);
        if (!impl_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return impl_->done; }))
            return false;
    }
    impl_->thread.join();
    return true;
}

void sleepMs(std::uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace platform
} // namespace sdrplay
