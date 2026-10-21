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

// Service side of the per-device channel set: 11 objects
// "<Glbl\sdrSrvDvNN>_m{Hrtbt,Cmd,Data,Event}{Sem,RSem,Map}" created on
// SelectDevice and unlinked on release.
#pragma once

#include "core/device/Device.h"
#include "core/ipc/DataRing.h"
#include "core/ipc/Layouts.h"
#include "core/ipc/Names.h"
#include "platform/InterProcessSemaphore.h"
#include "platform/SharedMemory.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace sdrplay {
namespace log {
class Logger;
}
namespace service {

// Android: every Init streams with USB bulk transfers, whatever devParams.mode the client
// set. Phone USB controllers drop isochronous packets; bulk is retried. Process-wide, off by
// default; only the Android embedded service turns it on.
void setForceBulkTransfers(bool on);
bool forceBulkTransfers();

class DeviceChannel final : public device::EventSink, public device::StreamSink {
public:
    // `log` is the device logger ("devIdx<N>: rsp").
    DeviceChannel(unsigned slot, ipc::Side side, log::Logger &log);
    ~DeviceChannel() override; // destroy()

    // Creates the objects in this order (the order clients can observe):
    // Hrtbt Sem/RSem, Cmd Sem/RSem/Map, Data Sem/RSem/Map,
    // Event Sem/RSem/Map. The CmdMap is zeroed; the caller writes the defaults.
    // On failure logs which object of which channel could not be created, with its name.
    bool create();
    // After a failed create(): "mHrtbt", "mCmd", "mData" or "mEvent".
    const char *failedChannel() const { return failed_; }
    // Unlinks everything in release order (Hrtbt, Cmd, Data,
    // Event; each Sem, RSem, Map). Idempotent.
    void destroy();

    unsigned slot() const { return slot_; }
    const std::string &prefix() const { return prefix_; }
    ipc::CmdMap *cmdMap() const;
    ipc::DataMap *dataMap() const;
    ipc::EventMap *eventMap() const;

    // master command thread body, one iteration: Wait(mCmdSem, waitMs); on a
    // request take apiMutex() + grMutex(), dispatch the ipc::DeviceCmd codes (others: Fail), write
    // the result, unlock, Release(mCmdRSem, 1). Returns true if served.
    // `dev` may be null (commands needing the model then fail).
    bool serveCommandOnce(std::uint32_t waitMs, device::Device *dev);
    // The same request/response frame with a caller-supplied dispatcher (RSPduo
    // slave command thread): Wait(cmdSem, waitMs), serve(map), Release(cmdRSem, 1).
    bool serveWith(std::uint32_t waitMs, const std::function<void(ipc::CmdMap &)> &serve);

    // master heartbeat thread body, one iteration: Wait(mHrtbtSem, waitMs) ->
    // ping counter++, Release(mHrtbtRSem, 1). Returns true if a ping was answered.
    bool serveHeartbeatOnce(std::uint32_t waitMs);
    // Pings since the last call (the watchdog's per-device counter).
    std::uint32_t takePings() { return pings_.exchange(0); }

    // Event post (ipc::EventRingProducer): Wait(mEventSem, 500) in a
    // loop until a slot is free or stop() is called, write the next ring slot,
    // Release(mEventRSem, 1). Blocks while the client holds 8 events.
    void postEvent(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner,
                   const sdrplay_api_EventParamsT &params) override;

    // device::StreamSink (the model's DSP thread -> data ring, ipc::DataRingProducer):
    // restart() is a no-op ON PURPOSE: the producer's slot index and bufIdx
    // counter run from SelectDevice to ReleaseDevice and are not reset by
    // Init (clients expect this); resetting bufIdx would make every client
    // log a bufIdx mismatch warning after a re-Init.
    void restart() override {}
    // 10 x Wait(mDataSem, 10 ms) (timeoutMs / 10 tries; 0 = drop at once).
    // false: no slot; the discard line has been logged, process into scratch.
    bool acquire(device::StreamBlock &b, std::uint32_t timeoutMs) override;
    void publish(const device::StreamBlock &b, const sdrplay_api_StreamCbParamsT &p, std::uint32_t reset) override;
    void publishDual(const device::StreamBlock &b, const sdrplay_api_StreamCbParamsT &pA,
                     const sdrplay_api_StreamCbParamsT &pB, std::uint32_t reset) override;
    void cancel(const device::StreamBlock &b) override;

    // Producer side of the data ring for the device's stream thread (Device::runStream):
    // acquire() a slot (10 x 10 ms, else a scratch slot = drop), process the
    // block in place, publish(). Null before create() / after destroy().
    // The stream thread must be joined before destroy().
    ipc::DataRingProducer *dataRing() const { return dataRing_.get(); }
    ipc::EventRingProducer *eventRing() const { return eventRing_.get(); }

    // Service-device error record, returned by DeviceCmd::GetLastError.
    sdrplay_api_TimedErrorInfoT &lastError() { return lastError_; }
    // Model state shared with the stream thread (API and gain mutexes).
    std::mutex &apiMutex() { return apiMtx_; }
    std::mutex &grMutex() { return grMtx_; }

    // Values stored by DeviceCmd::SwapTuner / ChangeSampleRate (RSPduo; harmless elsewhere).
    std::uint32_t swappedTuner() const { return swapTuner_; }
    double changedSampleRate() const { return changedFs_; }

    void stop()
    {
        stopping_ = true;
        if (auto *e = eventRing_.get()) e->stop();
    }
    bool stopping() const { return stopping_; }
    // True between a successful DeviceCmd::Init and a successful DeviceCmd::Uninit;
    // the Windows power hooks (ServiceCore::suspendAllDevices) stop only these.
    // Written with apiMutex() held.
    bool streaming() const { return streaming_; }
    void setStreaming(bool on) { streaming_ = on; }
    std::uint32_t pingsReceived() const { return pingsTotal_.load(); }

private:
    struct Triple {
        std::unique_ptr<platform::InterProcessSemaphore> sem, rsem;
        std::unique_ptr<platform::SharedMemory> map;
        void reset()
        {
            sem.reset();
            rsem.reset();
            map.reset();
        }
    };
    bool createTriple(Triple &t, ipc::Channel ch, const char *what, ipc::SemInit sem, ipc::SemInit rsem,
                      std::size_t mapSize);

    unsigned slot_;
    ipc::Side side_;
    log::Logger &log_;
    std::string prefix_;
    Triple hrtbt_, cmd_, data_, event_;
    const char *failed_ = "";
    std::unique_ptr<ipc::DataRingProducer> dataRing_;
    std::unique_ptr<ipc::EventRingProducer> eventRing_;
    std::mutex eventMtx_; // destroy() waits for an in-flight postEvent()
    std::mutex apiMtx_, grMtx_;
    sdrplay_api_TimedErrorInfoT lastError_{};
    std::uint32_t swapTuner_ = 0;
    double changedFs_ = 0.0;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<std::uint32_t> pings_{0};
    std::atomic<std::uint32_t> pingsTotal_{0};
};

} // namespace service
} // namespace sdrplay
