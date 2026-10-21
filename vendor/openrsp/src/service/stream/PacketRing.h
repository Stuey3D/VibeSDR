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

// Lock-free single-producer / single-consumer byte ring that hands raw RSP
// packets from the USB completion callbacks (producer: the libusb event
// thread) to the reader (consumer: Protocol::resyncToggles on the API thread during
// Init, then the DSP thread). Neither side takes a lock, so a completion
// callback never waits for the reader.
//
// Positions are monotonic 64-bit byte counters; index = counter % capacity.
// Writes are all-or-nothing: a chunk that does not fit is dropped whole, so
// the 1024-byte packet alignment of the ring is never broken (unread data is
// never overwritten).
//
// reset() may be called from a third (control) thread while the producer is
// quiescent (no transfer in flight, i.e. inside stream init); the consumer
// applies it lazily at its next call, so it never races a reader that is
// polling for data on the DSP thread.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace sdrplay {
namespace service {
namespace stream {

class PacketRing {
public:
    explicit PacketRing(std::size_t capacity) : buf_(new std::uint8_t[capacity]), cap_(capacity) {}
    PacketRing(const PacketRing &) = delete;
    PacketRing &operator=(const PacketRing &) = delete;

    std::size_t capacity() const { return cap_; }

    // ---- producer side (one thread at a time) ----
    // Appends n bytes, or nothing if they do not fit (returns false).
    bool write(const std::uint8_t *src, std::size_t n)
    {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (n > cap_ - static_cast<std::size_t>(h - effectiveTail())) {
            dropped_.fetch_add(n, std::memory_order_relaxed);
            return false;
        }
        const std::size_t at = static_cast<std::size_t>(h % cap_);
        const std::size_t first = n < cap_ - at ? n : cap_ - at;
        std::memcpy(buf_.get() + at, src, first);
        if (n > first) std::memcpy(buf_.get(), src + first, n - first);
        head_.store(h + n, std::memory_order_release);
        return true;
    }
    std::uint64_t produced() const { return head_.load(std::memory_order_acquire); }
    std::uint64_t droppedBytes() const { return dropped_.load(std::memory_order_relaxed); }

    // ---- consumer side (one thread at a time) ----
    std::size_t available()
    {
        applyReset();
        return static_cast<std::size_t>(head_.load(std::memory_order_acquire) -
                                        tail_.load(std::memory_order_relaxed));
    }
    // Zero-copy view of the next n bytes (two spans when it wraps).
    struct Spans {
        const std::uint8_t *p[2] = {nullptr, nullptr};
        std::size_t n[2] = {0, 0};
    };
    bool peek(std::size_t n, Spans &s)
    {
        if (available() < n) return false;
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        const std::size_t at = static_cast<std::size_t>(t % cap_);
        const std::size_t first = n < cap_ - at ? n : cap_ - at;
        s.p[0] = buf_.get() + at;
        s.n[0] = first;
        s.p[1] = n > first ? buf_.get() : nullptr;
        s.n[1] = n - first;
        return true;
    }
    void consume(std::size_t n) { tail_.store(tail_.load(std::memory_order_relaxed) + n, std::memory_order_release); }
    bool read(std::uint8_t *dst, std::size_t n)
    {
        Spans s;
        if (!peek(n, s)) return false;
        std::memcpy(dst, s.p[0], s.n[0]);
        if (s.n[1]) std::memcpy(dst + s.n[0], s.p[1], s.n[1]);
        consume(n);
        return true;
    }

    // ---- control (producer quiescent) ----
    // Discards everything written so far (ring reset at stream init).
    void reset()
    {
        resetPoint_.store(head_.load(std::memory_order_acquire), std::memory_order_relaxed);
        resetGen_.fetch_add(1, std::memory_order_release);
    }

private:
    std::uint64_t effectiveTail() const
    {
        const std::uint64_t t = tail_.load(std::memory_order_acquire);
        const std::uint64_t r = resetPoint_.load(std::memory_order_acquire);
        return t > r ? t : r;
    }
    void applyReset()
    {
        const std::uint32_t g = resetGen_.load(std::memory_order_acquire);
        if (g == seenGen_) return;
        seenGen_ = g;
        const std::uint64_t r = resetPoint_.load(std::memory_order_relaxed);
        if (tail_.load(std::memory_order_relaxed) < r) tail_.store(r, std::memory_order_release);
    }

    std::unique_ptr<std::uint8_t[]> buf_;
    const std::size_t cap_;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    std::uint32_t seenGen_ = 0; // consumer-local
    alignas(64) std::atomic<std::uint64_t> resetPoint_{0};
    std::atomic<std::uint32_t> resetGen_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

} // namespace stream
} // namespace service
} // namespace sdrplay
