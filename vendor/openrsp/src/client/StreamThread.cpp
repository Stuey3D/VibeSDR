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

// strmHandlerThread: consumer of the per-device data ring. Calls StreamA (and StreamB in
// RSPduo dual-tuner mode) with pointers straight into the shared data map.
#include "client/ClientAbi.h"
#include "client/ServiceConnection.h"

#include "core/ipc/Commands.h"
#include "core/ipc/Layouts.h"
#include "platform/Thread.h"

using namespace sdrplay;

namespace {
// A broken semaphore fails every wait at once; retrying immediately would spin
// at 100% CPU. We pace the retries like the normal 1000 ms wait and stay
// responsive to KillThreads.
void idleAfterError(sdrplay_api_device *d, std::uint32_t ms)
{
    for (std::uint32_t t = 0; t < ms && !d->killThreads_; t += 10) sdrplay_Sleep(10);
}
} // namespace

void *strmHandlerThread(void *arg)
{
    auto *d = static_cast<sdrplay_api_device *>(arg);
    ++d->threadsAlive_;
    client::setCurrentWorker(d, client::kStreamWorker);
    client::syslogRaw("device: stream thread: Entry");
    unsigned slot = 0; // thread-local, never reset by Init/Uninit
    // The kill flag is checked only after a timeout or a dispatch.
    while (!d->killThreads_) {
        const int r = sdrplay_SharedSemaphoreWait(d->data_.rsem, ipc::kClientDataWaitMs);
        if (r != kOsalOk) {
            client::threadLog(sdrplay_api_DbgLvl_Verbose, __func__, "Waiting in data Loop %d (0x%.8x)...", r,
                              static_cast<unsigned>(r));
            if (r != kOsalTimeout) {
                if (client::serviceGone()) d->onServiceLost("data semaphore broken");
                idleAfterError(d, ipc::kClientDataWaitMs);
            }
            continue;
        }
        auto *map = static_cast<ipc::DataMap *>(d->data_.map);
        ipc::DataSlot &s = map->slot[slot];
        const unsigned expected = ++d->bufIdxExpected_;
        if (expected != s.bufIdx) {
            client::threadLog(sdrplay_api_DbgLvl_Warning, __func__, "data slot %d bufIdx=%d (Error - expected %d)",
                              static_cast<int>(slot), static_cast<int>(s.bufIdx), static_cast<int>(expected));
            d->bufIdxExpected_ = s.bufIdx; // resync, still dispatched
        }
        void *ctx = d->cbContext_.load();
        if (sdrplay_api_StreamCallback_t a = d->streamACb_.load())
            a(s.xiA, s.xqA, &s.paramsA, s.paramsA.numSamples, s.reset, ctx);
        if (d->rspDuoMode_ == sdrplay_api_RspDuoMode_Dual_Tuner) { // independent of hwVer
            if (sdrplay_api_StreamCallback_t b = d->streamBCb_.load())
                b(s.xiB, s.xqB, &s.paramsB, s.paramsB.numSamples, s.reset, ctx);
        }
        slot = (slot + 1) % ipc::kDataRingSlots;
        long prev = 0;
        sdrplay_SharedSemaphoreRelease(d->data_.sem, 1, &prev); // hands the slot back
    }
    --d->threadsAlive_; // last access to *d
    client::syslogRaw("device: stream thread: Exit");
    return nullptr;
}
