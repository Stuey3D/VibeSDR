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

// eventHandlerThread and the client heartBeatThread.
#include "client/ClientAbi.h"
#include "client/ServiceConnection.h"

#include "core/ipc/Commands.h"
#include "core/ipc/Layouts.h"
#include "platform/Thread.h"

using namespace sdrplay;

namespace {
void idleAfterError(sdrplay_api_device *d, std::uint32_t ms)
{
    for (std::uint32_t t = 0; t < ms && !d->killThreads_; t += 10) sdrplay_Sleep(10);
}

// Deliver the DeviceRemoved event queued by
// onServiceLost() on the event thread, like a service-posted event.
void deliverRemoved(sdrplay_api_device *d)
{
    if (!d->removedPending_.exchange(false)) return;
    sdrplay_api_EventParamsT params{};
    if (sdrplay_api_EventCallback_t cb = d->eventCb_.load())
        cb(sdrplay_api_DeviceRemoved, d->tuner_, &params, d->cbContext_.load());
}
} // namespace

void *eventHandlerThread(void *arg)
{
    auto *d = static_cast<sdrplay_api_device *>(arg);
    ++d->threadsAlive_;
    client::setCurrentWorker(d, client::kEventWorker);
    client::syslogRaw("device: event thread: Entry");
    unsigned slot = 0; // continues across Init/Uninit
    while (!d->killThreads_) {
        deliverRemoved(d);
        const int r = sdrplay_SharedSemaphoreWait(d->event_.rsem, ipc::kClientEventWaitMs); // no log on timeout
        if (r == kOsalOk) {
            auto *map = static_cast<ipc::EventMap *>(d->event_.map);
            ipc::EventRecord &e = map->slot[slot];
            // Consumed and dropped when no callback is registered.
            if (sdrplay_api_EventCallback_t cb = d->eventCb_.load())
                cb(static_cast<sdrplay_api_EventT>(e.eventId), static_cast<sdrplay_api_TunerSelectT>(e.tuner),
                   &e.params, d->cbContext_.load());
            slot = (slot + 1) % ipc::kEventRingSlots;
            long prev = 0;
            sdrplay_SharedSemaphoreRelease(d->event_.sem, 1, &prev);
        } else if (r != kOsalTimeout) {
            // Paced retries, not a 100% CPU spin - see strmHandlerThread.
            if (client::serviceGone()) d->onServiceLost("event semaphore broken");
            idleAfterError(d, ipc::kClientEventWaitMs);
        }
    }
    --d->threadsAlive_; // last access to *d
    client::syslogRaw("device: event thread: Exit");
    return nullptr;
}

// After an initial 1000 ms delay, pings the service every 500 ms (signal
// mHrtbtSem, wait up to 5000 ms for mHrtbtRSem). The kill flag is checked only
// after each 500 ms sleep, so the exit takes up to 0.5 s (5.5 s with an
// unresponsive service).
void *heartBeatThread(void *arg)
{
    auto *d = static_cast<sdrplay_api_device *>(arg);
    ++d->threadsAlive_;
    client::setCurrentWorker(d, client::kHeartbeatWorker);
    client::syslogRaw("device: heartbeat thread: Entry");
    sdrplay_Sleep(ipc::kClientHeartbeatInitialDelayMs);
    unsigned consecutiveTimeouts = 0;
    while (!d->killThreads_) {
        long prev = 0;
        sdrplay_SharedSemaphoreRelease(d->hrtbt_.sem, 1, &prev); // a failed release (count at max) is ignored
        const int r = sdrplay_SharedSemaphoreWait(d->hrtbt_.rsem, ipc::kClientHeartbeatReplyTimeoutMs);
        if (r == kOsalTimeout) {
            client::threadLog(sdrplay_api_DbgLvl_Error, __func__,
                              "response to heartbeat timed out - suspect Service problem");
            // Two unanswered heartbeats in a row (>= 10 s; the service itself
            // releases a device after ~3 s of missed heartbeats) mean the
            // service died (e.g. SIGKILL).
            if (++consecutiveTimeouts >= 2) d->onServiceLost("heartbeat not answered");
        } else if (r != kOsalOk) {
            client::threadLog(sdrplay_api_DbgLvl_Error, __func__, "heartbeat response error");
            if (client::serviceGone()) d->onServiceLost("heartbeat semaphore broken");
        } else {
            consecutiveTimeouts = 0;
        }
        sdrplay_Sleep(ipc::kClientHeartbeatPeriodMs);
    }
    --d->threadsAlive_; // last access to *d
    client::syslogRaw("device: heartbeat thread: Exit");
    return nullptr;
}
