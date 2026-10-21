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

// The service's per-device object: creates the interface objects, binds the
// model and starts streaming (finishSetup), and releases everything on destruction.
//
// Lifetime: created by SelectDevice, destroyed by ReleaseDevice, by the
// heartbeat watchdog after 3 missed periods, or at service stop. It owns:
//   - the 11 IPC objects (DeviceChannel) with the default parameters,
//   - the device logger (devIdx<N>, level 3),
//   - master heartbeat thread, master command thread and the stream thread (Device::runStream),
//   - RSPduo: the slave set s{Hrtbt,Cmd,Data,Event} with the default slave
//     parameters, slave heartbeat thread and slave command thread (always, even in Single/Dual
//     mode), and the event routing (tuner == slave tuner -> sEvent),
//   - the hardware model (core/device) for the selected hwVer.
#pragma once

#include "core/device/Device.h"
#include "core/log/Logger.h"
#include "platform/Thread.h"
#include "service/DeviceChannel.h"

#include <atomic>
#include <memory>
#include <string>

namespace sdrplay {
namespace platform {
class LogSink;
class UsbDevice;
} // namespace platform
namespace service {
namespace stream {
class UsbStreamAdapter;
}

class ServiceDevice {
public:
    struct Params {
        unsigned slot = 0;
        std::uint8_t hwVer = 0;
        std::int32_t rspDuoMode = 0;      // DeviceT.rspDuoMode of the select request
        double rspDuoSampleFreq = 0.0;    // DeviceT.rspDuoSampleFreq of the select request
        platform::UsbDevice *usb = nullptr; // the slot's open handle (Enumerator::openDriver)
        platform::LogSink *logSink = nullptr;
        bool startThreads = true;         // false: unit tests drive serve*Once() by hand
        // Stream path seams (core/device/Device.h). stream: the slot's
        // service::stream::UsbStreamAdapter (needs the service's UsbEventThread);
        // null -> Init fails at stream start (sdrplay_api_HwError). sink: null -> this
        // device's DeviceChannel data ring.
        device::StreamTransport *stream = nullptr;
        device::StreamSink *sink = nullptr;
        // true (ServiceCore): when `stream` is null and `usb` is set, the device
        // builds its own service::stream::UsbStreamAdapter on `usb` (the
        // service's UsbEventThread must be running for completions).
        bool ownUsbStream = false;
    };

    explicit ServiceDevice(const Params &p);
    ~ServiceDevice(); // releases everything

    ServiceDevice(const ServiceDevice &) = delete;
    ServiceDevice &operator=(const ServiceDevice &) = delete;

    // createChannels: objects (DeviceChannel::create order), zeroed CmdMap + default
    // params, master heartbeat thread, then master command thread. false on failure
    // (everything already released, the channel that could not be created logged).
    bool createChannels();

    // Binds the model to the selected tuner (DeviceT.tuner) and
    // starts the stream thread ("rsp: stream thread: Entry").
    void finishSetup(sdrplay_api_TunerSelectT tuner);

    const std::string &prefix() const { return channel_.prefix(); }
    DeviceChannel &channel() { return channel_; }
    // RSPduo slave channel (null for other models).
    DeviceChannel *slaveChannel() { return slave_.get(); }
    int slaveTuner() const { return slaveTuner_; }

    // RSPduo slave paths (ServiceCore): SelectDevice attach (SlaveAttached to the master),
    // ReleaseDevice / heartbeat loss of the slave (map reset + SlaveDetached [+ SlaveDll
    // Disappeared]), master heartbeat loss with the slave alive (Uninit + MasterDllDisappeared).
    void slaveAttach();
    void slaveDetach(bool dllDisappeared);
    void masterDisappeared();
    // slave command thread body, one iteration (Init, Uninit, Update, InternalUpdate,
    // GetLastError; anything else invalid).
    bool serveSlaveCommandOnce(std::uint32_t waitMs);
    log::Logger &logger() { return log_; }
    device::Device *model() { return model_.get(); }
    sdrplay_api_TunerSelectT tuner() const { return tuner_; }
    unsigned slot() const { return p_.slot; }

    // Windows power events (suspend / resume all devices).
    // suspendStream(): if the device
    // streams, log "DevIdx%d: Stopping", stop it under the device's two mutexes
    // and remember it (true). resumeStream(): restart a device suspendStream()
    // stopped (true if it was suspended). Portable; only the Windows host calls them.
    bool suspendStream();
    bool resumeStream();
    bool suspended() const { return suspended_; }

    // Watchdog DeviceRemoved event (watchdog thread).
    void postEvent(sdrplay_api_EventT id, const sdrplay_api_EventParamsT &params);

private:
    void teardown();
    void streamThreadBody();
    void resetSlaveMap(); // slave map defaults, slave ring index 0

    // Event routing: an event whose tuner is the slave tuner goes to sEvent.
    class EventRouter final : public device::EventSink {
    public:
        explicit EventRouter(ServiceDevice &d) : d_(d) {}
        void postEvent(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner,
                       const sdrplay_api_EventParamsT &params) override;

    private:
        ServiceDevice &d_;
    };

    Params p_;
    log::Logger log_;
    std::unique_ptr<stream::UsbStreamAdapter> usbStream_; // ownUsbStream; outlives model_
    DeviceChannel channel_;
    std::unique_ptr<DeviceChannel> slave_; // RSPduo s* objects
    EventRouter router_{*this};
    int slaveTuner_ = 0;
    std::unique_ptr<device::Device> model_;
    std::atomic<device::Device *> modelPtr_{nullptr}; // read by master command thread
    sdrplay_api_TunerSelectT tuner_ = sdrplay_api_Tuner_A;
    std::unique_ptr<platform::Thread> hrtbtThread_, cmdThread_, streamThread_, sHrtbtThread_, sCmdThread_;
    std::atomic<bool> killHrtbt_{false}, killCmd_{false}, killStream_{false};
    bool released_ = false;
    std::atomic<bool> suspended_{false};
};

} // namespace service
} // namespace sdrplay
