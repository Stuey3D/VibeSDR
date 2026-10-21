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

// The service's global USB event thread (created by the
// first device scan and kept until shutdown): loops
// libusb_handle_events_timeout(ctx, 500 ms) until stopped. Every transfer
// completion callback of every device (StreamTransport) runs on it.
// Created with default attributes (pthread_create, no scheduling policy or
// priority change: SCHED_OTHER, nice 0).
#pragma once

#include "platform/Usb.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace sdrplay {
namespace service {
namespace stream {

class UsbEventThread {
public:
    static constexpr std::uint32_t kTimeoutMs = 500;

    // Pumps enumerator.handleEvents(kTimeoutMs); stop() also calls interruptEvents().
    explicit UsbEventThread(platform::UsbEnumerator &enumerator);
    // Generic pump (tests): pump(timeoutMs) is called in a loop; wake() is called by stop().
    UsbEventThread(std::function<void(std::uint32_t)> pump, std::function<void()> wake);
    ~UsbEventThread() { stop(); }
    UsbEventThread(const UsbEventThread &) = delete;
    UsbEventThread &operator=(const UsbEventThread &) = delete;

    bool start(); // idempotent; false if the thread could not be created
    void stop();  // sets the flag, wakes the pump, joins (<= one pump timeout)
    bool running() const { return thread_.joinable(); }
    std::uint64_t passes() const { return passes_.load(std::memory_order_relaxed); }

private:
    std::function<void(std::uint32_t)> pump_;
    std::function<void()> wake_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> passes_{0};
    std::thread thread_;
};

} // namespace stream
} // namespace service
} // namespace sdrplay
