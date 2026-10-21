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

#include "core/ipc/DataRing.h"

#include "core/log/Logger.h"
#include "platform/InterProcessSemaphore.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace sdrplay {
namespace ipc {

using platform::InterProcessSemaphore;

DataRingProducer::DataRingProducer(DataMap *map, InterProcessSemaphore *freeSlots, InterProcessSemaphore *filledSlots,
                                   log::Logger *log)
    : map_(map), free_(freeSlots), filled_(filledSlots), log_(log), scratch_(new DataSlot())
{
}

DataRingProducer::~DataRingProducer() = default;

DataSlot *DataRingProducer::acquire(bool consumerReady, std::uint32_t tries)
{
    acquired_ = true;
    holds_ = false;
    if (consumerReady && map_ && free_) {
        for (std::uint32_t i = 0; i < tries; ++i) {
            const InterProcessSemaphore::WaitResult r = free_->wait(kServiceDataSlotWaitMs);
            if (r == InterProcessSemaphore::Ok) {
                stalled_ = false;
                holds_ = true;
                return &map_->slot[write_];
            }
            if (r != InterProcessSemaphore::Timeout) {
                // Logged at Verbose, then the block is processed
                // into scratch WITHOUT the discard line.
                if (log_)
                    log_->log(log::Level::Verbose, __func__,
                              "Master stream loop ERROR waiting on the data semaphore");
                dropped_.fetch_add(1);
                return scratch_.get();
            }
            stalled_ = true;
        }
    }
    if (log_)
        log_->log(log::Level::Warning, __func__, "stream thread discarding packet after %dms",
                  static_cast<int>(kServiceDataSlotTries * kServiceDataSlotWaitMs));
    dropped_.fetch_add(1);
    return scratch_.get();
}

bool DataRingProducer::publish(const sdrplay_api_StreamCbParamsT &paramsA, const sdrplay_api_StreamCbParamsT *paramsB,
                               std::uint32_t reset)
{
    const bool deliver = acquired_ && holds_;
    acquired_ = false;
    holds_ = false;
    if (!deliver) return false;
    DataSlot &s = map_->slot[write_];
    // Store order: numSamples, then firstSampleNum, grChanged, rfChanged and
    // fsChanged; B likewise (clients rely on it).
    s.paramsA.numSamples = paramsA.numSamples;
    s.paramsA.firstSampleNum = paramsA.firstSampleNum;
    s.paramsA.grChanged = paramsA.grChanged;
    s.paramsA.rfChanged = paramsA.rfChanged;
    s.paramsA.fsChanged = paramsA.fsChanged;
    const sdrplay_api_StreamCbParamsT noB{};
    const sdrplay_api_StreamCbParamsT &b = paramsB ? *paramsB : noB;
    s.paramsB.numSamples = b.numSamples;
    s.paramsB.firstSampleNum = b.firstSampleNum;
    s.paramsB.grChanged = b.grChanged;
    s.paramsB.rfChanged = b.rfChanged;
    s.paramsB.fsChanged = b.fsChanged;
    s.reset = reset;
    s.bufIdx = ++bufIdx_;
    // The semaphore's pshared mutex orders the slot stores before the signal.
    if (filled_) filled_->release(1, nullptr);
    write_ = (write_ + 1) % kDataRingSlots;
    published_.fetch_add(1);
    return true;
}

void DataRingProducer::cancel()
{
    const bool held = acquired_ && holds_;
    acquired_ = false;
    holds_ = false;
    if (held && free_) free_->release(1, nullptr);
}

bool DataRingProducer::deliver(const std::int16_t *xi, const std::int16_t *xq,
                               const sdrplay_api_StreamCbParamsT &paramsA, std::uint32_t reset, bool consumerReady)
{
    DataSlot *s = acquire(consumerReady);
    const std::size_t n = std::min<std::size_t>(paramsA.numSamples, kMaxSamplesPerCallback);
    if (xi) std::memcpy(s->xiA, xi, n * sizeof(std::int16_t));
    if (xq) std::memcpy(s->xqA, xq, n * sizeof(std::int16_t));
    return publish(paramsA, nullptr, reset);
}

// ------------------------------------------------------------------ events

EventRingProducer::EventRingProducer(EventMap *map, InterProcessSemaphore *freeSlots,
                                     InterProcessSemaphore *filledSlots, log::Logger *log)
    : map_(map), free_(freeSlots), filled_(filledSlots), log_(log)
{
}

bool EventRingProducer::post(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner,
                             const sdrplay_api_EventParamsT &params)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!map_ || !free_ || !filled_) return false;
    bool gotSlot = false;
    do {
        const InterProcessSemaphore::WaitResult r = free_->wait(kServiceEventSlotWaitMs);
        if (r == InterProcessSemaphore::Ok) {
            gotSlot = true;
            break;
        }
        if (log_)
            log_->log(log::Level::Verbose, __func__, "Tuner%d: Waiting for event %d (0x%.8x)...",
                      static_cast<int>(tuner), static_cast<int>(r), static_cast<unsigned>(r));
    } while (!stopping_);
    EventRecord &rec = map_->slot[write_];
    rec.eventId = static_cast<std::uint32_t>(id);
    rec.tuner = static_cast<std::uint32_t>(tuner);
    std::memcpy(&rec.params, &params, sizeof params);
    filled_->release(1, nullptr); // fails harmlessly (count at max) on a teardown overwrite
    write_ = (write_ + 1) % kEventRingSlots;
    posted_.fetch_add(1);
    return gotSlot;
}

} // namespace ipc
} // namespace sdrplay
