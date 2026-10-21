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

// See StreamTransport.h.
#include "service/stream/StreamTransport.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>

namespace sdrplay {
namespace service {
namespace stream {

using platform::UsbDevice;
using platform::UsbTransfer;
using platform::UsbTransferType;

namespace {

void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::uint32_t le32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

} // namespace

StreamGeometry geometryForAlt(int alt)
{
    StreamGeometry g;
    g.alt = alt;
    if (alt == kBulkAlt) {
        g.bulk = true;
        g.transferLength = 3072 * 64;
        g.timeoutMs = 4000;
        return g;
    }
    if (alt == 2) {
        g.isoPacketLength = 1024;
        g.isoPackets = 128;
    } else {
        g.isoPacketLength = 3072;
        g.isoPackets = 64;
    }
    g.transferLength = g.isoPacketLength * g.isoPackets;
    g.timeoutMs = 1000;
    return g;
}

std::size_t bulkAlignmentOffset(const std::uint8_t *b, std::size_t len)
{
    // Each packet starts with its 32-bit sample counter. The comparison skips
    // the first kSettlePackets packets of the buffer (the stream start) and
    // looks at the three after them: equal counter steps mean the data is
    // aligned; equal steps half a packet further on mean it starts half a
    // packet early. The differences wrap; they are taken in uint32 so the wrap
    // is defined.
    constexpr std::size_t kSettlePackets = 40;
    constexpr std::size_t kHalfPacket = kPacketBytes / 2;
    auto counter = [b](std::size_t packet, std::size_t shift) {
        return le32(b + (kSettlePackets + packet) * kPacketBytes + shift);
    };
    if (len < (kSettlePackets + 2) * kPacketBytes + kHalfPacket + 4) return 0;
    if (counter(1, 0) - counter(0, 0) == counter(2, 0) - counter(1, 0)) return 0;
    return counter(1, kHalfPacket) - counter(0, kHalfPacket) == counter(2, kHalfPacket) - counter(1, kHalfPacket)
               ? kHalfPacket
               : 0;
}

StreamTransport::StreamTransport(UsbDevice *dev, Hooks hooks) : dev_(dev), hooks_(std::move(hooks))
{
    for (int i = 0; i < kNumTransfers; ++i) {
        slots_[static_cast<std::size_t>(i)].owner = this;
        slots_[static_cast<std::size_t>(i)].index = i;
    }
    geo_ = geometryForAlt(0);
}

StreamTransport::~StreamTransport()
{
    if (inFlight_.load() > 0) {
        stopping_.store(true);
        if (!cancelAndWait(timing_.cancelWaitMs)) {
            // Transfers the backend never returned: their buffers must outlive
            // them (libusb may still write into them), so leak deliberately.
            log("~StreamTransport", "Warning: %d transfer(s) still in flight; leaking their buffers",
                inFlight_.load());
            (void)heapBuffers_.release();
            buffers_ = nullptr; // device memory is leaked with them (never freed under libusb)
            for (auto &s : slots_)
                if (s.inFlight.load()) (void)s.xfer.release();
        }
    }
    if (buffers_ && devMemBuffers_ && dev_) dev_->freeStreamMemory(buffers_, std::size_t(kNumTransfers) * kTransferBufferBytes);
}

void StreamTransport::log(const char *func, const char *fmt, ...)
{
    if (!hooks_.log) return;
    char buf[512];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    hooks_.log(1, func, buf);
}

int StreamTransport::setInterface(int alt)
{
    log("set interface", "set interface (%d)", alt);
    alt_ = alt;
    if (!dev_) return 1;
    geo_ = geometryForAlt(alt);
    const int rc = dev_->setAltSetting(0, alt);
    if (rc == 0) return 0;
    log("set interface", "libusb_set_interface_alt_setting() %d", rc);
    return 1;
}

int StreamTransport::submitRaw(Slot &s)
{
    const int rc = dev_->submitTransfer(*s.xfer);
    if (rc == 0) stSubmits_.fetch_add(1, std::memory_order_relaxed);
    else if (rc == UsbDevice::kErrNoDevice) noDeviceSeen_.store(true, std::memory_order_release);
    return rc;
}

int StreamTransport::submitFirst(Slot &s)
{
    s.inFlight.store(true, std::memory_order_release);
    const int now = inFlight_.fetch_add(1, std::memory_order_acq_rel) + 1;
    int m = stMaxInFlight_.load(std::memory_order_relaxed);
    while (now > m && !stMaxInFlight_.compare_exchange_weak(m, now, std::memory_order_relaxed)) {
    }
    const int rc = submitRaw(s);
    if (rc != 0) {
        s.inFlight.store(false, std::memory_order_release);
        inFlight_.fetch_sub(1, std::memory_order_acq_rel);
    }
    return rc;
}

int StreamTransport::streamInit()
{
    if (!dev_) return 1;
    if (inFlight_.load() > 0) {
        // Only after a stream-uninit timeout. The slots cannot be reused while
        // transfers are in flight, so cancel them first.
        if (!cancelAndWait(timing_.cancelWaitMs)) {
            log("stream start", "Error: %d transfer(s) of the previous stream still in flight", inFlight_.load());
            return 1;
        }
    }
    if (!buffers_) {
        buffers_ = dev_->allocStreamMemory(std::size_t(kNumTransfers) * kTransferBufferBytes);
        devMemBuffers_ = buffers_ != nullptr;
        if (!buffers_) {
            heapBuffers_.reset(new std::uint8_t[kNumTransfers * kTransferBufferBytes]);
            buffers_ = heapBuffers_.get();
        }
    }
    geo_ = geometryForAlt(alt_);
    ring_.reset();
    retired_.store(0);
    stopping_.store(false);
    removed_.store(false);
    noDeviceSeen_.store(false);
    cancelling_.store(false);
    hookFired_.store(false);
    overflowing_.store(false);
    // Set before the first submit: completions may arrive while we are still
    // submitting (set under the mutex the callback takes).
    realign_.store(true);
    sessionTransfers_ = kNumTransfers;
    sessionBulk_.store(geo_.bulk);
    const int nIso = geo_.bulk ? 0 : geo_.isoPackets;
    // Allocate every transfer before submitting the first one. libusb_alloc_transfer has no USB
    // effect, so the wire sequence is unchanged; it lets backends that stream through a fixed set
    // of slots (the iPadOS dext) see the whole set at the first submit.
    for (int i = 0; i < kNumTransfers; ++i) {
        Slot &s = slots_[static_cast<std::size_t>(i)];
        if (!s.xfer || s.xfer->numIsoPackets() != nIso) {
            s.xfer.reset();
            s.xfer = dev_->allocTransfer(nIso);
        }
        if (!s.xfer) {
            log("stream start", "Error: libusb_alloc_transfer() returned NULL");
            sessionTransfers_ = 0;
            stopping_.store(true);
            cancelAndWait(timing_.cancelWaitMs);
            return 1;
        }
    }
    for (int i = 0; i < kNumTransfers; ++i) {
        Slot &s = slots_[static_cast<std::size_t>(i)];
        s.drained.store(false);
        UsbTransfer &x = *s.xfer;
        x.type = geo_.bulk ? UsbTransferType::Bulk : UsbTransferType::Isochronous;
        x.endpoint = kStreamEndpoint;
        x.timeoutMs = geo_.timeoutMs;
        x.buffer = buffers_ + static_cast<std::size_t>(i) * kTransferBufferBytes;
        x.length = geo_.transferLength;
        x.isoPacketLength = geo_.isoPacketLength;
        x.callback = &StreamTransport::onComplete;
        x.user = &s;
        const int rc = submitFirst(s);
        if (rc != 0) {
            log("stream start", "Error: libusb_submit_transfer() %d", rc);
            // Cancel the already-submitted transfers and wait for them before
            // their buffers are freed.
            sessionTransfers_ = i;
            stopping_.store(true);
            cancelAndWait(timing_.cancelWaitMs);
            return 1;
        }
    }
    streaming_.store(true, std::memory_order_release);
    return 0;
}

bool StreamTransport::cancelAndWait(unsigned waitMs)
{
    if (!dev_) return inFlight_.load() == 0;
    cancelling_.store(true);
    for (auto &s : slots_)
        if (s.inFlight.load(std::memory_order_acquire) && s.xfer) dev_->cancelTransfer(*s.xfer);
    for (unsigned t = 0; inFlight_.load() > 0 && t < waitMs; t += 10) sleepMs(10);
    return inFlight_.load() == 0;
}

bool StreamTransport::waitAllRetired()
{
    unsigned waited = 0;
    do {
        sleepMs(timing_.uninitPollMs);
        waited += timing_.uninitPollMs;
        if (inFlight_.load() == 0) return true;
    } while (waited < timing_.uninitTimeoutMs);
    return false;
}

int StreamTransport::streamUninit()
{
    if (!dev_) return 1;
    if (streaming_.load()) {
        stopping_.store(true, std::memory_order_release);
        if (inFlight_.load() != 0 && !waitAllRetired()) {
            log("stream stop", "stream stop timeout (%d %d)", static_cast<int>(timing_.uninitTimeoutMs),
                retired_.load());
            // Cancel the stragglers so their buffers are not reused while
            // in flight.
            cancelAndWait(timing_.cancelWaitMs);
        }
    }
    streaming_.store(false, std::memory_order_release);
    return 0;
}

int StreamTransport::flush()
{
    if (!dev_) return 1;
    if (!flushBuf_) flushBuf_.reset(new std::uint8_t[kPacketBytes]);
    for (int k = timing_.flushReads; k > 0; --k) {
        int got = 0;
        const int rc = dev_->bulkTransfer(kStreamEndpoint, flushBuf_.get(), static_cast<int>(kPacketBytes), &got,
                                          timing_.flushTimeoutMs);
        if (rc == UsbDevice::kErrTimeout) break;
    }
    return 0;
}

void StreamTransport::onComplete(UsbTransfer &x)
{
    Slot &s = *static_cast<Slot *>(x.user);
    s.owner->handleCompletion(s);
}

void StreamTransport::queue(const std::uint8_t *p, std::size_t n)
{
    if (n == 0) return;
    if (ring_.write(p, n)) {
        overflowing_.store(false, std::memory_order_relaxed);
        return;
    }
    stOverflows_.fetch_add(1, std::memory_order_relaxed);
    if (!overflowing_.exchange(true, std::memory_order_relaxed))
        log("USB transfer callback", "Error: USB buffer overflow");
}

void StreamTransport::retire(Slot &s, int status, bool removedCheck)
{
    const int r = retired_.fetch_add(1, std::memory_order_acq_rel) + 1;
    stRetired_.fetch_add(1, std::memory_order_relaxed);
    if (status == platform::kXferNoDevice) noDeviceSeen_.store(true, std::memory_order_release);
    // The device counts as removed when the 8th retirement is a
    // NO_DEVICE / CANCELLED / iso ERROR one. Our own cancellations don't count.
    if (removedCheck && r == sessionTransfers_ && !cancelling_.load()) removed_.store(true, std::memory_order_release);
    // Everything this needs from *this is read BEFORE the last in-flight count drops: a thread in
    // cancelAndWait (~StreamTransport, streamInit) may free the transport as soon as it sees 0.
    // The hooks only run when nobody is cancelling, i.e. nobody is waiting to destroy us.
    const bool cancelling = cancelling_.load();
    const bool noDevice = noDeviceSeen_.load();
    const bool stopping = stopping_.load();
    s.inFlight.store(false, std::memory_order_release);
    const int left = inFlight_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0 && !cancelling) {
        if (noDevice) {
            if (!hookFired_.exchange(true) && hooks_.deviceRemoved) hooks_.deviceRemoved();
        } else if (!stopping) {
            if (!hookFired_.exchange(true) && hooks_.streamDied) hooks_.streamDied(status);
        }
    }
}

void StreamTransport::handleCompletion(Slot &s)
{
    UsbTransfer &x = *s.xfer;
    const int st = x.status;
    stCompletions_.fetch_add(1, std::memory_order_relaxed);
    stStatus_[static_cast<std::size_t>(st) & 7].fetch_add(1, std::memory_order_relaxed);
    const bool bulk = sessionBulk_.load(std::memory_order_relaxed);
    const bool stopping = stopping_.load(std::memory_order_acquire);

    // A completed transfer stays counted in flight until it is resubmitted or retired.
    auto resubmitOrRetire = [&](int status) {
        const int rc = submitRaw(s);
        if (rc != 0) {
            log("USB transfer callback", "Error: libusb_submit_transfer(%p) %d", static_cast<void *>(&x), rc);
            retire(s, status, false);
        }
    };

    switch (st) {
    case platform::kXferNoDevice:
        log("USB transfer callback", "Warning: USB transfer callback: device removed");
        retire(s, st, true);
        return;
    case platform::kXferTimedOut:
        log("USB transfer callback", "Warning: USB transfer callback: transfer timed out %p %d",
            static_cast<void *>(&x), retired_.load());
        retire(s, st, false);
        return;
    case platform::kXferCancelled:
        log("USB transfer callback", "Warning: USB transfer callback: transfer cancelled %p %d",
            static_cast<void *>(&x), retired_.load());
        retire(s, st, true);
        return;
    case platform::kXferError:
        log("USB transfer callback", "Warning: USB transfer callback: transfer error %p %d", static_cast<void *>(&x),
            retired_.load());
        if (!bulk) {
            retire(s, st, true);
            return;
        }
        realign_.store(true, std::memory_order_relaxed);
        if (stopping) retire(s, st, false);
        else resubmitOrRetire(st);
        return;
    case platform::kXferOverflow:
        log("USB transfer callback", "Warning: USB transfer callback: transfer overflow %p %d %d %d",
            static_cast<void *>(&x), retired_.load(), static_cast<int>(kTransferBufferBytes), x.actualLength);
        if (!bulk) {
            retire(s, st, false);
            return;
        }
        realign_.store(true, std::memory_order_relaxed);
        if (stopping) retire(s, st, false);
        else resubmitOrRetire(st);
        return;
    case platform::kXferCompleted:
        break;
    default: // STALL and anything else
        log("USB transfer callback", "Warning: USB transfer callback: transfer did not complete status = %d", st);
        retire(s, st, false);
        return;
    }

    if (!bulk) {
        if (!stopping) {
            const int n = x.numIsoPackets();
            for (int i = 0; i < n; ++i) {
                const std::uint32_t len = x.iso[static_cast<std::size_t>(i)].actualLength;
                if (len == 0) continue;
                stIsoData_.fetch_add(1, std::memory_order_relaxed);
                queue(x.buffer + static_cast<std::size_t>(x.isoPacketLength) * static_cast<std::size_t>(i), len);
            }
            resubmitOrRetire(st);
            return;
        }
        // Stopping: every iso transfer goes round once more, then retires.
        if (s.drained.exchange(true)) {
            retire(s, st, false);
            return;
        }
        resubmitOrRetire(st);
        return;
    }

    // Bulk: only whole transfers (kTransferBufferBytes) are used.
    if (x.actualLength == static_cast<int>(kTransferBufferBytes)) {
        std::size_t off = 0;
        if (realign_.exchange(false, std::memory_order_relaxed)) {
            off = bulkAlignmentOffset(x.buffer, kTransferBufferBytes);
            stRealign_.fetch_add(1, std::memory_order_relaxed);
            log("USB transfer callback", "bufOffset=%d", static_cast<int>(off));
        }
        queue(x.buffer + off, kTransferBufferBytes - off);
    }
    if (stopping) retire(s, st, false);
    else resubmitOrRetire(st);
}

int StreamTransport::waitFor(std::size_t n)
{
    if (!dev_ || n == 0) return kReadFailed;
    // Up to readPolls sleeps; the removal flag is checked between them.
    for (unsigned sleeps = 1;; ++sleeps) {
        if (ring_.available() >= n) return kReadOk;
        sleepMs(timing_.readPollMs);
        if (sleeps >= timing_.readPolls) break;
        if (removed_.exchange(false, std::memory_order_acq_rel)) return kReadDeviceRemoved;
    }
    log("stream read", "stream read: not enough data: requested %d read %d",
        static_cast<int>(n), static_cast<int>(ring_.available()));
    return kReadFailed;
}

int StreamTransport::read(std::uint8_t *dst, std::size_t n)
{
    if (!dst) return kReadFailed;
    const int r = waitFor(n);
    if (r != kReadOk) return r;
    return ring_.read(dst, n) ? kReadOk : kReadFailed;
}

int StreamTransport::acquire(std::size_t n, PacketRing::Spans &spans)
{
    const int r = waitFor(n);
    if (r != kReadOk) return r;
    return ring_.peek(n, spans) ? kReadOk : kReadFailed;
}

int StreamTransport::readPackets(std::uint8_t *dst, std::size_t maxPackets, std::uint32_t timeoutMs)
{
    if (!dev_ || !dst || maxPackets == 0) return 0;
    const std::uint32_t w = wake_.load(std::memory_order_acquire);
    // the clock is only read once the caller actually has to wait (it is a syscall on some hosts)
    std::chrono::steady_clock::time_point t0{};
    bool waited = false;
    for (;;) {
        const std::size_t have = ring_.available() / kPacketBytes;
        if (have > 0) {
            const std::size_t n = have < maxPackets ? have : maxPackets;
            ring_.read(dst, n * kPacketBytes);
            return static_cast<int>(n);
        }
        if (removed_.load(std::memory_order_acquire) || (noDeviceSeen_.load() && inFlight_.load() == 0)) return -1;
        if (wake_.load(std::memory_order_acquire) != w) return 0;
        // A reader that arrives after stop() woke the others must not wait out
        // its timeout for a stream that is going away.
        if (stopping_.load(std::memory_order_acquire)) return 0;
        if (!waited) {
            t0 = std::chrono::steady_clock::now();
            waited = true;
        } else if (std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(timeoutMs)) {
            return 0;
        }
        sleepMs(timing_.readPollMs);
    }
}

StreamTransport::Stats StreamTransport::stats() const
{
    Stats s;
    s.submits = stSubmits_.load();
    s.completions = stCompletions_.load();
    s.retired = stRetired_.load();
    s.isoPacketsWithData = stIsoData_.load();
    s.bytesQueued = ring_.produced();
    s.bytesDropped = ring_.droppedBytes();
    s.overflows = stOverflows_.load();
    s.realignments = stRealign_.load();
    for (std::size_t i = 0; i < 8; ++i) s.statusCounts[i] = stStatus_[i].load();
    s.maxInFlight = stMaxInFlight_.load();
    return s;
}

} // namespace stream
} // namespace service
} // namespace sdrplay
