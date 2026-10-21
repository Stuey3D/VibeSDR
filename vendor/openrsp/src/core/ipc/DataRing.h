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

// Service-side producers of the per-device data ring (<P>_mDataMap, 8 DataSlots)
// and event ring (<P>_mEventMap, 8 EventRecords).
//
// Signalling (classic bounded ring, shared with the client's strmHandlerThread
// and eventHandlerThread):
//   <obj>Sem  = free slots   (created 8/8), producer Waits, consumer Releases
//   <obj>RSem = filled slots (created 0/8), producer Releases, consumer Waits
// Both indices wrap modulo 8. The producer's slot index and bufIdx counter live for
// the device object's lifetime (SelectDevice..ReleaseDevice); Init/Uninit never
// reset them, exactly like the client's read index and expected-bufIdx.
//
// Back-pressure / drop policy:
//   * data: before processing a block the producer waits for a free slot with
//     Wait(mDataSem, 10 ms) up to 10 times (100 ms). If none frees up, the
//     block is still processed (into a scratch slot, so DSP/AGC state advances)
//     but NOT published: one block is dropped, "stream thread discarding packet
//     after 100ms" is logged at Warning (hidden at the default level
//     3), bufIdx does not advance, no event and no reset flag. The consumer
//     sees a firstSampleNum jump only. If the device is in the hardware-error
//     state or no master client is attached the
//     wait is skipped and every block is discarded at once.
//   * events: Wait(mEventSem, 500) in a loop, logging (Verbose) "Tuner%d:
//     Waiting for event %d (0x%.8x)..." after each miss: the calling
//     service thread BLOCKS while the client holds 8 events. Only device
//     teardown (the stream thread's kill flag) ends the loop, and
//     then the record is still written into the next slot and
//     Releases mEventRSem (a no-op if the client is gone).
#pragma once

#include "core/api/ApiTypes.h"
#include "core/ipc/Commands.h"
#include "core/ipc/Layouts.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace sdrplay {
namespace platform {
class InterProcessSemaphore;
}
namespace log {
class Logger;
}
namespace ipc {

class DataRingProducer {
public:
    // `map`, `freeSlots` (mDataSem) and `filledSlots` (mDataRSem) must outlive
    // the producer. `log` (the device logger) may be null.
    DataRingProducer(DataMap *map, platform::InterProcessSemaphore *freeSlots,
                     platform::InterProcessSemaphore *filledSlots, log::Logger *log);
    ~DataRingProducer();

    DataRingProducer(const DataRingProducer &) = delete;
    DataRingProducer &operator=(const DataRingProducer &) = delete;

    // Step 1 of every block: returns the memory the block must be processed
    // into, in place. That is ring slot w when a free slot was obtained within
    // 10 x 10 ms, otherwise a private scratch slot (the block will be dropped;
    // the discard line is logged here). `consumerReady` false = the
    // "no wait" state (hardware error / no master attached): drop immediately.
    // Exactly one publish() must follow each acquire().
    // `tries` = number of 10 ms waits (normally 10).
    DataSlot *acquire(bool consumerReady = true, std::uint32_t tries = kServiceDataSlotTries);

    // True if the last acquire() returned a ring slot (publish() will deliver).
    bool holdsSlot() const { return holds_; }

    // Step 2+3: if a ring slot is held, write params A (numSamples first, then
    // firstSampleNum/grChanged/rfChanged/fsChanged), params B likewise (RSPduo
    // dual tuner; zeros when `paramsB` is null), reset, bufIdx = ++counter, then
    // signal mDataRSem and move on to the next slot. A dropped block changes nothing.
    // Returns true if the block was delivered.
    bool publish(const sdrplay_api_StreamCbParamsT &paramsA, const sdrplay_api_StreamCbParamsT *paramsB,
                 std::uint32_t reset);

    // Gives an acquired ring slot back unpublished (Release(mDataSem, 1), w
    // unchanged): the stream restarted while the block was being processed.
    void cancel();

    // Capacity in samples of each sample array (xiA, xqA, xiB, xqB) of the
    // held slot. 0 when no slot is held.
    std::size_t sampleCapacity() const { return holds_ ? kMaxSamplesPerCallback : 0; }

    // Convenience for synthetic producers and tests: acquire, copy n samples
    // (n <= kMaxSamplesPerCallback) of xi/xq into the slot, publish.
    bool deliver(const std::int16_t *xi, const std::int16_t *xq, const sdrplay_api_StreamCbParamsT &paramsA,
                 std::uint32_t reset, bool consumerReady = true);

    // Producer write index and last bufIdx written.
    unsigned writeIndex() const { return write_; }
    std::uint32_t lastBufIdx() const { return bufIdx_; }
    // RSPduo slave Init resets the slave ring index.
    void resetWriteIndex() { write_ = 0; }

    std::uint64_t published() const { return published_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }
    // Set while waiting for a slot failed, cleared on success.
    bool stalled() const { return stalled_.load(); }

private:
    DataMap *map_;
    platform::InterProcessSemaphore *free_;
    platform::InterProcessSemaphore *filled_;
    log::Logger *log_;
    std::unique_ptr<DataSlot> scratch_;
    unsigned write_ = 0;
    std::uint32_t bufIdx_ = 0;
    bool holds_ = false;
    bool acquired_ = false;
    std::atomic<bool> stalled_{false};
    std::atomic<std::uint64_t> published_{0}, dropped_{0};
};

class EventRingProducer {
public:
    EventRingProducer(EventMap *map, platform::InterProcessSemaphore *freeSlots,
                      platform::InterProcessSemaphore *filledSlots, log::Logger *log);

    // Blocks (500 ms waits) until the client frees a slot or
    // stop() is called, then writes {id, tuner, params} into the current slot,
    // signals mEventRSem and moves on to the next slot. Returns true if a free slot was
    // obtained (false: written on teardown without one).
    // Callers are serialised internally.
    bool post(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner, const sdrplay_api_EventParamsT &params);

    // Teardown (the stream thread's kill flag): a blocked post() returns
    // after its current 500 ms wait.
    void stop() { stopping_ = true; }
    bool stopping() const { return stopping_.load(); }

    unsigned writeIndex() const { return write_; }
    void resetWriteIndex() { write_ = 0; }
    std::uint64_t posted() const { return posted_.load(); }

private:
    EventMap *map_;
    platform::InterProcessSemaphore *free_;
    platform::InterProcessSemaphore *filled_;
    log::Logger *log_;
    std::mutex mtx_;
    unsigned write_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> posted_{0};
};

} // namespace ipc
} // namespace sdrplay
