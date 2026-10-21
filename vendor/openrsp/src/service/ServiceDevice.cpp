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

#include "service/ServiceDevice.h"

#include "core/api/LastError.h"
#include "core/device/DefaultParams.h"
#include "core/ipc/DataRing.h"
#include "platform/Clock.h"
#include "core/ipc/Commands.h"
#include "service/stream/UsbStreamAdapter.h"

#include <cstring>
#include <string>

namespace sdrplay {
namespace service {

using log::Level;

ServiceDevice::ServiceDevice(const Params &p)
    : p_(p),
      log_(p.logSink, "devIdx" + std::to_string(p.slot) + ": rsp", Level::Error), // level 3
      channel_(p.slot, ipc::Side::Master, log_)
{
    if (p_.hwVer == device::kHwRspDuo) slave_.reset(new DeviceChannel(p.slot, ipc::Side::Slave, log_));
}

ServiceDevice::~ServiceDevice() { teardown(); }

bool ServiceDevice::createChannels()
{
    std::string what;
    if (!channel_.create()) {
        what = std::string("Could not create ") + channel_.failedChannel();
    } else if (slave_ && !slave_->create()) {
        what = std::string("Could not create ") + slave_->failedChannel();
    } else {
        ipc::CmdMap &m = *channel_.cmdMap();
        std::memset(&m, 0, sizeof m);
        // Model-independent defaults for the master map.
        device::writeDefaultParams(m, p_.rspDuoMode, p_.rspDuoSampleFreq, false);
        if (slave_) resetSlaveMap(); // the same defaults for the slave map
        if (p_.startThreads) {
            hrtbtThread_ = platform::Thread::start(
                [this] {
                    log_.log(Level::Verbose, __func__, "master heartbeat thread: Entry");
                    while (!killHrtbt_) channel_.serveHeartbeatOnce(ipc::kServiceHeartbeatPollMs);
                    log_.log(Level::Verbose, __func__, "master heartbeat thread: Exit");
                },
                "heartbeat-m");
            cmdThread_ = platform::Thread::start(
                [this] {
                    log_.raw("device: master command thread: Entry");
                    while (!killCmd_) channel_.serveCommandOnce(ipc::kServiceDeviceCmdPollMs, modelPtr_.load());
                    log_.raw("device: master command thread: Exit");
                },
                "command-m");
            if (slave_) {
                sHrtbtThread_ = platform::Thread::start(
                    [this] {
                        log_.log(Level::Verbose, __func__, "slave heartbeat thread: Entry");
                        while (!killHrtbt_) slave_->serveHeartbeatOnce(ipc::kServiceHeartbeatPollMs);
                        log_.log(Level::Verbose, __func__, "slave heartbeat thread: Exit");
                    },
                    "heartbeat-s");
                sCmdThread_ = platform::Thread::start(
                    [this] {
                        log_.raw("device: slave command thread: Entry");
                        while (!killCmd_) serveSlaveCommandOnce(ipc::kServiceDeviceCmdPollMs);
                        log_.raw("device: slave command thread: Exit");
                    },
                    "command-s");
            }
            if (!hrtbtThread_) what = "Could not create master heartbeat thread";
            else if (!cmdThread_) what = "Could not create master command thread";
            else if (slave_ && !sHrtbtThread_) what = "Could not create slave heartbeat thread";
            else if (slave_ && !sCmdThread_) what = "Could not create slave command thread";
        }
    }
    if (!what.empty()) {
        log_.log(Level::Error, __func__, "%s", what.c_str());
        teardown();
        return false;
    }
    return true;
}

void ServiceDevice::finishSetup(sdrplay_api_TunerSelectT tuner)
{
    tuner_ = tuner;
    if (!p_.stream && p_.usb && p_.ownUsbStream) {
        stream::StreamTransport::Hooks hooks;
        // Transport lines go to the device logger (log level 1 = verbose).
        hooks.log = [this](int level, const char *func, const std::string &msg) {
            log_.log(static_cast<Level>(level), func, "%s", msg.c_str());
        };
        // DeviceRemoved is raised by the watchdog's presence probe
        // (ServiceCore::watchdogOnce); the transport's own flag makes
        // the DSP thread's reads return -1 meanwhile.
        usbStream_.reset(new stream::UsbStreamAdapter(p_.usb, std::move(hooks)));
        p_.stream = usbStream_.get();
    }
    {
        std::lock_guard<std::mutex> api(channel_.apiMutex());
        device::DeviceContext ctx;
        ctx.usb = p_.usb;
        ctx.params = channel_.cmdMap();
        ctx.events = &router_;
        ctx.log = &log_;
        ctx.lastError = &channel_.lastError();
        ctx.tuner = tuner;
        // RSPduo: the select request's mode and sample rate build the device in the right mode
        // (ICtrl/QCtrl routing, Single-mode stream). Ignored by other models.
        ctx.rspDuoMode = p_.rspDuoMode;
        ctx.rspDuoSampleFreq = p_.rspDuoSampleFreq;
        if (slave_) {
            ctx.slaveParams = slave_->cmdMap();
            ctx.slaveSink = slave_.get();
            // In master mode the slave gets the tuner the master does not use.
            if (p_.rspDuoMode == sdrplay_api_RspDuoMode_Master)
                slaveTuner_ = tuner == sdrplay_api_Tuner_B ? sdrplay_api_Tuner_A : sdrplay_api_Tuner_B;
            else
                slaveTuner_ = 0;
        }
        ctx.stream = p_.stream;
        ctx.sink = p_.sink ? p_.sink : &channel_; // DeviceChannel's data ring
        ctx.recordMutex = &channel_.grMutex(); // held by master command thread while serving DeviceCmd::GetLastError
        model_ = device::createDevice(p_.hwVer, ctx);
        modelPtr_ = model_.get();
        // Without a model (an hwVer with no device implementation), the device
        // commands Init, Uninit, Update and InternalUpdate answer Fail.
    }
    if (p_.startThreads)
        streamThread_ = platform::Thread::start([this] { streamThreadBody(); }, "stream");
    // No priority change: every service thread runs SCHED_OTHER nice 0.
}

void ServiceDevice::streamThreadBody()
{
    log_.raw("rsp: stream thread: Entry");
    // The model's DSP thread (packet fetch + per-block pipeline + AGC/overload,
    // idle until Init); without a model the thread only idles so that its
    // lifecycle (and log lines) stay the same.
    if (device::Device *m = modelPtr_.load()) m->runStream(killStream_);
    else
        while (!killStream_) platform::sleepMs(10);
    log_.raw("rsp: stream thread: Exit");
}

void ServiceDevice::postEvent(sdrplay_api_EventT id, const sdrplay_api_EventParamsT &params)
{
    channel_.postEvent(id, tuner_, params);
}

bool ServiceDevice::suspendStream()
{
    std::lock_guard<std::mutex> api(channel_.apiMutex());
    std::lock_guard<std::mutex> gr(channel_.grMutex());
    if (!model_ || !channel_.streaming()) return false;
    log_.log(Level::Message, __func__, "DevIdx%u: Stopping", p_.slot);
    model_->uninit();
    channel_.setStreaming(false);
    suspended_ = true;
    return true;
}

bool ServiceDevice::resumeStream()
{
    std::lock_guard<std::mutex> api(channel_.apiMutex());
    std::lock_guard<std::mutex> gr(channel_.grMutex());
    if (!model_ || !suspended_) return false;
    suspended_ = false;
    log_.log(Level::Message, __func__, "DevIdx%u: Restarting", p_.slot);
    if (model_->init() == sdrplay_api_Success) channel_.setStreaming(true);
    return true;
}

void ServiceDevice::teardown()
{
    if (released_) return;
    released_ = true;
    channel_.stop(); // unblocks a pending postEvent()
    // Teardown: Uninit if streaming, then stop the stream thread.
    if (model_) {
        std::lock_guard<std::mutex> api(channel_.apiMutex());
        model_->releaseHardware();
    }
    if (streamThread_) {
        killStream_ = true;
        streamThread_->join();
        streamThread_.reset();
    }
    if (slave_) slave_->stop();
    if (cmdThread_ || hrtbtThread_) {
        log_.log(Level::Verbose, __func__, "Stopping the heartbeat threads");
        killHrtbt_ = true;
        if (hrtbtThread_) hrtbtThread_->join();
        hrtbtThread_.reset();
        if (sHrtbtThread_) sHrtbtThread_->join();
        sHrtbtThread_.reset();
        log_.log(Level::Verbose, __func__, "heartbeat threads stopped");
        log_.log(Level::Verbose, __func__, "Stopping the command threads");
        killCmd_ = true;
        if (cmdThread_) cmdThread_->join();
        cmdThread_.reset();
        if (sCmdThread_) sCmdThread_->join();
        sCmdThread_.reset();
        log_.log(Level::Verbose, __func__, "command threads stopped");
    }
    channel_.destroy();
    if (slave_) slave_->destroy();
    modelPtr_ = nullptr;
    model_.reset();
    usbStream_.reset(); // after the model: it may still reference the adapter
}

// ------------------------------------------------------------- RSPduo slave side
void ServiceDevice::EventRouter::postEvent(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner,
                                           const sdrplay_api_EventParamsT &params)
{
    if (d_.slave_ && d_.slaveTuner_ != 0 && static_cast<int>(tuner) == d_.slaveTuner_) d_.slave_->postEvent(id, tuner, params);
    else d_.channel_.postEvent(id, tuner, params);
}

void ServiceDevice::resetSlaveMap()
{
    if (!slave_ || !slave_->cmdMap()) return;
    ipc::CmdMap &m = *slave_->cmdMap();
    std::memset(&m, 0, sizeof m);
    device::writeDefaultParams(m, p_.rspDuoMode, p_.rspDuoSampleFreq, true);
    if (ipc::DataRingProducer *ring = slave_->dataRing()) ring->resetWriteIndex();
}

void ServiceDevice::slaveAttach()
{
    if (device::Device *m = modelPtr_.load()) m->rspDuoSlaveAttached(); // sdrplay_api_SlaveAttached to the master tuner
}

void ServiceDevice::slaveDetach(bool dllDisappeared)
{
    std::lock_guard<std::mutex> api(channel_.apiMutex());
    std::lock_guard<std::mutex> gr(channel_.grMutex());
    device::Device *m = modelPtr_.load();
    if (dllDisappeared && m && m->rspDuoSlaveInitialised()) m->rspDuoUninitSlave();
    resetSlaveMap();
    if (m) m->rspDuoSlaveDetached(dllDisappeared); // sdrplay_api_SlaveDetached [+ SlaveDllDisappeared]
}

void ServiceDevice::masterDisappeared()
{
    std::lock_guard<std::mutex> api(channel_.apiMutex());
    std::lock_guard<std::mutex> gr(channel_.grMutex());
    if (device::Device *m = modelPtr_.load()) m->rspDuoMasterDisappeared(); // Uninit + sdrplay_api_MasterDllDisappeared
    channel_.setStreaming(false);
}

bool ServiceDevice::serveSlaveCommandOnce(std::uint32_t waitMs)
{
    // slave command thread: Init, Uninit, Update, InternalUpdate and GetLastError;
    // every other command is invalid.
    DeviceChannel *sc = slave_.get();
    if (!sc) return false;
    return sc->serveWith(waitMs, [this](ipc::CmdMap &m) {
        std::lock_guard<std::mutex> api(channel_.apiMutex());
        std::lock_guard<std::mutex> gr(channel_.grMutex());
        device::Device *dev = modelPtr_.load();
        using ipc::DeviceCmd;
        switch (m.cmd) {
        case static_cast<std::uint32_t>(DeviceCmd::Init): m.result = dev ? dev->rspDuoInitSlave() : sdrplay_api_Fail; break;
        case static_cast<std::uint32_t>(DeviceCmd::Uninit):
            m.result = dev ? dev->rspDuoUninitSlave() : sdrplay_api_Fail;
            break;
        case static_cast<std::uint32_t>(DeviceCmd::Update): {
            // Reasons reserved for the master (every bit below Update_Tuner_Gr).
            constexpr std::uint32_t kMasterOnlyReasons =
                sdrplay_api_Update_Dev_Fs | sdrplay_api_Update_Dev_Ppm | sdrplay_api_Update_Dev_SyncUpdate |
                sdrplay_api_Update_Dev_ResetFlags | sdrplay_api_Update_Rsp1a_BiasTControl |
                sdrplay_api_Update_Rsp1a_RfNotchControl | sdrplay_api_Update_Rsp1a_RfDabNotchControl |
                sdrplay_api_Update_Rsp2_BiasTControl | sdrplay_api_Update_Rsp2_AmPortSelect |
                sdrplay_api_Update_Rsp2_AntennaControl | sdrplay_api_Update_Rsp2_RfNotchControl |
                sdrplay_api_Update_Rsp2_ExtRefControl | sdrplay_api_Update_RspDuo_ExtRefControl |
                sdrplay_api_Update_Master_Spare_1 | sdrplay_api_Update_Master_Spare_2;
            static_assert(kMasterOnlyReasons == sdrplay_api_Update_Tuner_Gr - 1u, "master-only reason bits");
            const std::uint32_t reason = m.reason;
            if (reason & kMasterOnlyReasons)
                log_.log(Level::Error, __func__, "Invalid slave reasonForUpdate 0x%x", reason);
            m.result = dev ? dev->rspDuoUpdateSlave(static_cast<sdrplay_api_TunerSelectT>(m.tuner),
                                                    static_cast<sdrplay_api_ReasonForUpdateT>(reason),
                                                    static_cast<sdrplay_api_ReasonForUpdateExtension1T>(m.reasonExt1))
                           : sdrplay_api_Fail;
            break;
        }
        case static_cast<std::uint32_t>(DeviceCmd::InternalUpdate):
            m.result = m.internalReason != 0 ? sdrplay_api_InvalidParam : sdrplay_api_Success; // no RF test for a slave
            break;
        case static_cast<std::uint32_t>(DeviceCmd::GetLastError):
            std::memcpy(&m.lastError, &channel_.lastError(), sizeof m.lastError);
            m.result = sdrplay_api_Success;
            break;
        default:
            // record only: no log line at the default level (the client's DebugEnable after a slave
            // SelectDevice lands here)
            log_.log(Level::Verbose, __func__, "Invalid slave command %d", static_cast<int>(m.cmd));
            api::setLastError(&channel_.lastError(), platform::realtimeMicros(), __FILE__, __func__, __LINE__, "Invalid slave command %d", static_cast<int>(m.cmd));
            m.result = sdrplay_api_Fail;
            break;
        }
    });
}

} // namespace service
} // namespace sdrplay
