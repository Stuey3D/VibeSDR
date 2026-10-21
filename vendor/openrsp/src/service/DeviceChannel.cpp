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

#include "service/DeviceChannel.h"

#include "core/log/Logger.h"

#include <atomic>
#include <cstddef>
#include <cstring>

namespace sdrplay {
namespace service {

namespace {
std::atomic<bool> g_forceBulk{false};
} // namespace

void setForceBulkTransfers(bool on) { g_forceBulk.store(on); }
bool forceBulkTransfers() { return g_forceBulk.load(); }

using platform::InterProcessSemaphore;
using platform::SharedMemory;

DeviceChannel::DeviceChannel(unsigned slot, ipc::Side side, log::Logger &log)
    : slot_(slot), side_(side), log_(log), prefix_(ipc::devicePrefix(slot))
{
}

DeviceChannel::~DeviceChannel() { destroy(); }

void DeviceChannel::destroy()
{
    stop();
    std::lock_guard<std::mutex> lk(eventMtx_); // a blocked postEvent() leaves within 500 ms
    dataRing_.reset();
    eventRing_.reset();
    // Release order: mHrtbt, mCmd, mData, mEvent, each
    // closing cSem, rSem, then the map. Creators destroy in place and unlink.
    hrtbt_.reset();
    cmd_.reset();
    data_.reset();
    event_.reset();
}

bool DeviceChannel::createTriple(Triple &t, ipc::Channel ch, const char *what, ipc::SemInit sem, ipc::SemInit rsem,
                                 std::size_t mapSize)
{
    using ipc::Kind;
    const std::string semName = ipc::deviceObjectName(prefix_, side_, ch, Kind::Sem);
    const std::string rsemName = ipc::deviceObjectName(prefix_, side_, ch, Kind::RSem);
    const std::string mapName = ipc::deviceObjectName(prefix_, side_, ch, Kind::Map);
    // Names longer than ipc::kServiceMaxNameLen (the IPC object-name length limit) fail
    // channel creation; the names for slots 0..15 always fit.
    failed_ = what;
    if (!ipc::fitsServiceNameLimit(rsemName) || !ipc::fitsServiceNameLimit(mapName)) return false;
    int err = 0;
    t.sem = InterProcessSemaphore::create(semName, sem.initial, sem.maximum, &err);
    if (!t.sem) {
        log_.log(log::Level::Error, __func__, "Could not create %s->cSem %s", what, semName.c_str());
        return false;
    }
    t.rsem = InterProcessSemaphore::create(rsemName, rsem.initial, rsem.maximum, &err);
    if (!t.rsem) {
        log_.log(log::Level::Error, __func__, "Could not create %s->rSem %s", what, rsemName.c_str());
        return false;
    }
    if (mapSize) {
        t.map = SharedMemory::create(mapName, mapSize, &err);
        if (!t.map) {
            log_.log(log::Level::Error, __func__, "Could not create %s->map %s", what, mapName.c_str());
            return false;
        }
    }
    failed_ = "";
    return true;
}

bool DeviceChannel::create()
{
    const bool m = side_ == ipc::Side::Master;
    const bool ok = createTriple(hrtbt_, ipc::Channel::Hrtbt, m ? "mHrtbt" : "sHrtbt", ipc::kHrtbtSemInit, ipc::kHrtbtSemInit,
                        ipc::kHrtbtMapSize) &&
           createTriple(cmd_, ipc::Channel::Cmd, m ? "mCmd" : "sCmd", ipc::kCmdSemInit, ipc::kCmdSemInit,
                        ipc::kCmdMapSize) &&
           createTriple(data_, ipc::Channel::Data, m ? "mData" : "sData", ipc::kDataSemInit, ipc::kDataRSemInit,
                        ipc::kDataMapSize) &&
           createTriple(event_, ipc::Channel::Event, m ? "mEvent" : "sEvent", ipc::kEventSemInit, ipc::kEventRSemInit,
                        ipc::kEventMapSize);
    if (ok) {
        dataRing_.reset(new ipc::DataRingProducer(dataMap(), data_.sem.get(), data_.rsem.get(), &log_));
        eventRing_.reset(new ipc::EventRingProducer(eventMap(), event_.sem.get(), event_.rsem.get(), &log_));
        if (stopping_) eventRing_->stop();
    }
    return ok;
}

ipc::CmdMap *DeviceChannel::cmdMap() const
{
    return cmd_.map ? static_cast<ipc::CmdMap *>(cmd_.map->data()) : nullptr;
}
ipc::DataMap *DeviceChannel::dataMap() const
{
    return data_.map ? static_cast<ipc::DataMap *>(data_.map->data()) : nullptr;
}
ipc::EventMap *DeviceChannel::eventMap() const
{
    return event_.map ? static_cast<ipc::EventMap *>(event_.map->data()) : nullptr;
}

bool DeviceChannel::serveCommandOnce(std::uint32_t waitMs, device::Device *dev)
{
    if (!cmd_.sem || cmd_.sem->wait(waitMs) != InterProcessSemaphore::Ok) return false;
    ipc::CmdMap &m = *cmdMap();
    {
        std::lock_guard<std::mutex> api(apiMtx_);
        std::lock_guard<std::mutex> gr(grMtx_);
        using ipc::DeviceCmd;
        switch (m.cmd) {
        case static_cast<std::uint32_t>(DeviceCmd::DebugEnable):
            // Per-device logger level + unconditional line.
            log_.setLevel(static_cast<log::Level>(static_cast<int>(m.dbgLvl)));
            log_.raw("device log level %d", static_cast<int>(m.dbgLvl));
            m.result = sdrplay_api_Success;
            break;
        case static_cast<std::uint32_t>(DeviceCmd::Init):
            // Set before Init only: the mode is read at Init, and a live change wedges an RSP1B.
            // Written to the shared params, so the client's GetDeviceParams shows the mode in use.
            if (forceBulkTransfers()) m.devParams.mode = sdrplay_api_BULK;
            m.result = dev ? dev->init() : sdrplay_api_Fail;
            if (m.result == sdrplay_api_Success) streaming_ = true;
            break;
        case static_cast<std::uint32_t>(DeviceCmd::Uninit):
            m.result = dev ? dev->uninit() : sdrplay_api_Fail;
            if (m.result == sdrplay_api_Success) streaming_ = false;
            break;
        case static_cast<std::uint32_t>(DeviceCmd::SwapTuner):
            // Stores the tuner and re-points the parameter
            // blocks (RSPduo). Always Success.
            swapTuner_ = m.tuner;
            if (dev) dev->rspDuoSwapTuner(static_cast<int>(m.tuner)); // RSPduo only
            m.result = sdrplay_api_Success;
            break;
        case static_cast<std::uint32_t>(DeviceCmd::ChangeSampleRate): {
            // Stores devParams.fsFreq.fsHz. Always Success.
            changedFs_ = m.devParams.fsFreq.fsHz;
            if (dev) dev->rspDuoChangeSampleRate(changedFs_); // RSPduo only
            m.result = sdrplay_api_Success;
            break;
        }
        case static_cast<std::uint32_t>(DeviceCmd::Update):
            m.result = dev ? dev->update(static_cast<sdrplay_api_TunerSelectT>(m.tuner),
                                         static_cast<sdrplay_api_ReasonForUpdateT>(m.reason),
                                         static_cast<sdrplay_api_ReasonForUpdateExtension1T>(m.reasonExt1))
                           : sdrplay_api_Fail;
            break;
        case static_cast<std::uint32_t>(DeviceCmd::InternalUpdate):
            m.result = dev ? dev->internalUpdate(static_cast<sdrplay_api_ReasonForInternalUpdateT>(m.internalReason))
                           : sdrplay_api_Fail;
            break;
        case static_cast<std::uint32_t>(DeviceCmd::GetLastError):
            std::memcpy(&m.lastError, &lastError_, sizeof lastError_);
            m.result = sdrplay_api_Success;
            break;
        default: m.result = sdrplay_api_Fail; break;
        }
    }
    cmd_.rsem->release(1, nullptr);
    return true;
}

bool DeviceChannel::serveWith(std::uint32_t waitMs, const std::function<void(ipc::CmdMap &)> &serve)
{
    if (!cmd_.sem || cmd_.sem->wait(waitMs) != InterProcessSemaphore::Ok) return false;
    serve(*cmdMap());
    cmd_.rsem->release(1, nullptr);
    return true;
}

bool DeviceChannel::serveHeartbeatOnce(std::uint32_t waitMs)
{
    if (!hrtbt_.sem || hrtbt_.sem->wait(waitMs) != InterProcessSemaphore::Ok) return false;
    pings_.fetch_add(1);
    pingsTotal_.fetch_add(1);
    hrtbt_.rsem->release(1, nullptr);
    return true;
}

void DeviceChannel::postEvent(sdrplay_api_EventT id, sdrplay_api_TunerSelectT tuner,
                              const sdrplay_api_EventParamsT &params)
{
    std::lock_guard<std::mutex> lk(eventMtx_);
    if (eventRing_) eventRing_->post(id, tuner, params);
}

bool DeviceChannel::acquire(device::StreamBlock &b, std::uint32_t timeoutMs)
{
    b = device::StreamBlock{};
    if (!dataRing_) return false;
    const std::uint32_t tries = timeoutMs / ipc::kServiceDataSlotWaitMs;
    ipc::DataSlot *s = dataRing_->acquire(tries != 0, tries);
    if (!dataRing_->holdsSlot()) return false;
    // Each array is readable up to its own capacity.
    const std::size_t cap = dataRing_->sampleCapacity();
    b.xi = s->xiA;
    b.xq = s->xqA;
    b.xiReadable = cap;
    b.xqReadable = cap;
    b.xiB = s->xiB;
    b.xqB = s->xqB;
    b.xiBReadable = cap;
    b.xqBReadable = cap;
    b.slot = dataRing_->writeIndex();
    return true;
}

void DeviceChannel::publish(const device::StreamBlock &, const sdrplay_api_StreamCbParamsT &p, std::uint32_t reset)
{
    // Single tuner: the params B of the slot are written as zeros.
    if (dataRing_) dataRing_->publish(p, nullptr, reset);
}

void DeviceChannel::publishDual(const device::StreamBlock &, const sdrplay_api_StreamCbParamsT &pA,
                                const sdrplay_api_StreamCbParamsT &pB, std::uint32_t reset)
{
    if (dataRing_) dataRing_->publish(pA, &pB, reset);
}

void DeviceChannel::cancel(const device::StreamBlock &)
{
    if (dataRing_) dataRing_->cancel();
}

} // namespace service
} // namespace sdrplay
