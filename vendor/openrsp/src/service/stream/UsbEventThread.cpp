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

#include "service/stream/UsbEventThread.h"

#include <system_error>

namespace sdrplay {
namespace platform {
void setCurrentThreadNameOs(const char *name); // per-OS (platform/*/ThreadOs*.cpp, stub)
}
namespace service {
namespace stream {

UsbEventThread::UsbEventThread(platform::UsbEnumerator &e)
    : pump_([&e](std::uint32_t ms) { e.handleEvents(ms); }), wake_([&e]() { e.interruptEvents(); })
{
}

UsbEventThread::UsbEventThread(std::function<void(std::uint32_t)> pump, std::function<void()> wake)
    : pump_(std::move(pump)), wake_(std::move(wake))
{
}

bool UsbEventThread::start()
{
    if (thread_.joinable()) return true;
    stop_.store(false);
    try {
        thread_ = std::thread([this]() {
            platform::setCurrentThreadNameOs("usbEvents"); // named: the iPad demo counts it as driver CPU
            while (!stop_.load(std::memory_order_acquire)) {
                pump_(kTimeoutMs);
                passes_.fetch_add(1, std::memory_order_relaxed);
            }
        });
    } catch (const std::system_error &) {
        return false;
    }
    return true;
}

void UsbEventThread::stop()
{
    if (!thread_.joinable()) return;
    stop_.store(true, std::memory_order_release);
    if (wake_) wake_();
    thread_.join();
}

} // namespace stream
} // namespace service
} // namespace sdrplay
