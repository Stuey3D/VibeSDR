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

// USB streaming transport of the service: set interface, stream init,
// transfer completion, stream uninit, bulk flush and stream read, on top of
// platform::UsbDevice async transfers.
//
//   alt | type | transfer geometry      | bytes  | timeout
//   ----+------+------------------------+--------+--------
//    1  | iso  | 64 packets x 3072 B    | 196608 | 1000 ms  (high-bandwidth, 3 x 1024 per uframe)
//    2  | iso  | 128 packets x 1024 B   | 131072 | 1000 ms
//    3  | bulk | 196608 B               | 196608 | 4000 ms
//
// Always 8 transfers in flight on EP 0x81.
// Completed iso packets contribute actual_length bytes each
// (0, 1024 or 3072) to a 12 MiB ring; bulk transfers contribute the whole
// buffer, the first one after streamInit (or after an error) checked for a
// half-packet (512-byte) misalignment. The reader takes kReadChunkBytes (384
// packets) at a time: Protocol::resyncToggles during Init, then the DSP thread.
//
// Stop does not use libusb_cancel_transfer: uninit sets
// a stopping flag; each iso transfer is resubmitted once more and retired on
// its next completion, bulk transfers retire on their next completion; the
// caller polls every 100 ms for up to 4000 ms until all 8 are back.
//
// Threads: the completion callbacks run on the libusb event thread
// (UsbEventThread); the ring handoff to the reader is lock-free (PacketRing).
// setInterface/streamInit/streamUninit/flush are called from the control
// (API) thread, which the caller serialises; read() from one reader at a time.
// No thread priorities are changed: every service thread runs SCHED_OTHER
// nice 0.
#pragma once

#include "platform/Usb.h"
#include "service/stream/PacketRing.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sdrplay {
namespace service {
namespace stream {

constexpr int kNumTransfers = 8;
constexpr std::size_t kPacketBytes = 1024;                  // one device packet: 16 B header + 1008 B payload
constexpr std::size_t kTransferBufferBytes = 192 * 1024;    // per-transfer buffer
constexpr std::size_t kRingBytes = 12 * 1024 * 1024;        // packet ring
constexpr std::size_t kReadChunkBytes = 384 * kPacketBytes; // packet fetch / Protocol::resyncToggles
constexpr std::uint8_t kStreamEndpoint = 0x81;
constexpr int kBulkAlt = 3;

struct StreamGeometry {
    int alt = 0;
    bool bulk = false;
    int isoPackets = 0;      // per transfer (0 for bulk)
    int isoPacketLength = 0; // bytes per iso packet
    int transferLength = 0;  // bytes per transfer
    std::uint32_t timeoutMs = 0;
};
// Iso geometry {3072, 64} unless alt == 2 ({1024, 128});
// alt 3 is bulk with length 3072*64 and a 4000 ms timeout.
StreamGeometry geometryForAlt(int alt);

// The alignment test of the first bulk completion: compares the sample counters
// of packets 40..42 at packet boundaries, then half a packet further on.
// Returns the number of leading bytes to drop (0 or 512).
std::size_t bulkAlignmentOffset(const std::uint8_t *buf, std::size_t len);

// Results of read().
enum ReadResult : int { kReadOk = 0, kReadFailed = 1, kReadDeviceRemoved = 2 };

class StreamTransport {
public:
    // level = the log level (1 for every transport line);
    // func = the function name in the log line ("stream start", "USB transfer callback", ...).
    using LogFn = std::function<void(int level, const char *func, const std::string &msg)>;
    struct Hooks {
        LogFn log;
        // Called once per stream session, on the USB event thread, when every
        // transfer has retired and at least one retired with NO_DEVICE (or a
        // resubmit failed with LIBUSB_ERROR_NO_DEVICE): the device is gone.
        // The service raises DeviceRemoved from here (post, don't block).
        std::function<void()> deviceRemoved;
        // Called on the USB event thread when the last in-flight transfer of a
        // running (not stopping) session retires for another reason (stall,
        // error, timeout, failed resubmit): the stream has stopped on its own.
        std::function<void(int lastStatus)> streamDied;
    };
    // Poll/timeout constants (tests shorten them).
    struct Timing {
        unsigned uninitPollMs = 100;    // streamUninit() sleep between checks
        unsigned uninitTimeoutMs = 4000;
        unsigned readPollMs = 1;        // read() sleep between checks
        unsigned readPolls = 1000;      // ... at most this many sleeps (>= 1): a 1 s read timeout
        unsigned cancelWaitMs = 1000;   // ours: after an uninit timeout, wait this long for cancelled transfers
        unsigned flushTimeoutMs = 100;
        int flushReads = 32;
    };
    struct Stats {
        std::uint64_t submits = 0, completions = 0, retired = 0, isoPacketsWithData = 0;
        std::uint64_t bytesQueued = 0, bytesDropped = 0, overflows = 0, realignments = 0;
        std::uint64_t statusCounts[8] = {};
        int maxInFlight = 0;
    };

    // dev == nullptr: no device handle (every call returns 1).
    explicit StreamTransport(platform::UsbDevice *dev, Hooks hooks = Hooks());
    ~StreamTransport();
    StreamTransport(const StreamTransport &) = delete;
    StreamTransport &operator=(const StreamTransport &) = delete;

    void setTiming(const Timing &t) { timing_ = t; }

    // ---- control thread ----
    // Records alt + geometry, libusb_set_interface_alt_setting(0, alt). 0 / 1.
    int setInterface(int alt);
    // Records alt + geometry only, for callers that already switched the
    // interface through UsbDevice::setAltSetting (Rsp1bDevice's control Io).
    void selectAlt(int alt)
    {
        alt_ = alt;
        geo_ = geometryForAlt(alt);
    }
    int alt() const { return alt_; }
    StreamGeometry geometry() const { return geometryForAlt(alt_); }
    // Reset the ring, allocate + submit 8 transfers. 0 / 1.
    int streamInit();
    // Stop resubmitting, wait (100 ms polls, <= 4000 ms) for all 8. 0 / 1.
    int streamUninit();
    // Bulk flush: up to 32 x bulk read of 1024 B / 100 ms, stop at the first timeout. 0 / 1.
    int flush();
    bool streaming() const { return streaming_.load(std::memory_order_acquire); }
    int inFlight() const { return inFlight_.load(std::memory_order_acquire); }
    bool stopping() const { return stopping_.load(std::memory_order_acquire); }

    // ---- reader (Protocol::resyncToggles, then the DSP thread) ----
    // Copies exactly n bytes; kReadOk / kReadFailed (timeout before n bytes arrived) /
    // kReadDeviceRemoved.
    int read(std::uint8_t *dst, std::size_t n);
    // Zero-copy variant: waits like read(), then exposes the next n bytes; call
    // release(n) when done with them.
    int acquire(std::size_t n, PacketRing::Spans &spans);
    void release(std::size_t n) { ring_.consume(n); }
    std::size_t available() { return ring_.available(); } // ring fill level
    // Packet-wise reader for device::StreamTransport::read (Rsp1bDevice's DSP
    // thread): waits up to timeoutMs (1 ms polls) for one whole packet, then
    // copies min(maxPackets, available) packets. Returns the number copied,
    // 0 on timeout or wakeReaders(), -1 once the device is gone.
    int readPackets(std::uint8_t *dst, std::size_t maxPackets, std::uint32_t timeoutMs);
    // Makes a blocked readPackets() return 0 now (stream stop).
    void wakeReaders() { wake_.fetch_add(1, std::memory_order_acq_rel); }
    void resetRing() { ring_.reset(); }                    // producer must be idle

    bool deviceRemovedFlag() const { return removed_.load(std::memory_order_acquire); }
    Stats stats() const;

private:
    struct Slot {
        StreamTransport *owner = nullptr;
        int index = 0;
        std::unique_ptr<platform::UsbTransfer> xfer;
        std::atomic<bool> inFlight{false};
        std::atomic<bool> drained{false};
    };

    static void onComplete(platform::UsbTransfer &x);
    void handleCompletion(Slot &s);
    void retire(Slot &s, int status, bool removedCheck);
    int submitRaw(Slot &s);   // resubmit from the callback (slot stays in flight)
    int submitFirst(Slot &s); // first submit of a session (counts the slot in flight)
    void queue(const std::uint8_t *p, std::size_t n);
    int waitFor(std::size_t n);
    bool cancelAndWait(unsigned waitMs);
    // streamUninit: polls every uninitPollMs (sleeping first) until no transfer is in
    // flight; false after uninitTimeoutMs.
    bool waitAllRetired();
    void log(const char *func, const char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;

    platform::UsbDevice *dev_;
    Hooks hooks_;
    Timing timing_;
    int alt_ = 0;
    StreamGeometry geo_;
    // kNumTransfers x kTransferBufferBytes, allocated on first streamInit: zero-copy device
    // memory when the backend has it
    // (dev_->allocStreamMemory), else ordinary memory (heapBuffers_).
    std::uint8_t *buffers_ = nullptr;
    bool devMemBuffers_ = false;
    std::unique_ptr<std::uint8_t[]> heapBuffers_;
    std::array<Slot, kNumTransfers> slots_;
    int sessionTransfers_ = 0;
    PacketRing ring_{kRingBytes};

    std::atomic<bool> streaming_{false};
    std::atomic<bool> sessionBulk_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> realign_{false};      // bulk
    std::atomic<bool> removed_{false};
    std::atomic<bool> overflowing_{false};
    std::atomic<bool> noDeviceSeen_{false};
    std::atomic<bool> cancelling_{false};
    std::atomic<bool> hookFired_{false};
    std::atomic<int> retired_{0};
    std::atomic<std::uint32_t> wake_{0};
    std::atomic<int> inFlight_{0};

    // statistics (relaxed; written on the event thread)
    std::atomic<std::uint64_t> stSubmits_{0}, stCompletions_{0}, stRetired_{0}, stIsoData_{0};
    std::atomic<std::uint64_t> stOverflows_{0}, stRealign_{0};
    std::array<std::atomic<std::uint64_t>, 8> stStatus_{};
    std::atomic<int> stMaxInFlight_{0};
    std::unique_ptr<std::uint8_t[]> flushBuf_;
};

} // namespace stream
} // namespace service
} // namespace sdrplay
