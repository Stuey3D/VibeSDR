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

// RSPduo model (USB 1df7:3020, hwVer 3): MSi2500-like bridge, two MSi001-like
// tuners behind an SPI chip-select, two GPIO expanders (requests 0x4A/0x4B).
//
// Wiring of the pure engine (rspduo::Protocol) into a service device: the
// model-independent common::CommonRspDevice (the RSP1B wiring: locking,
// DSP thread, LOG lines) with RSPduo hooks, plus:
//   - Init / Uninit / Update / InternalUpdate / Release of the master (or the
//     single/dual-tuner owner) through the Device interface;
//   - the RSPduo-only service paths as extra entry points the service's
//     command threads call: slave cmd 8/9/12 (initSlave, uninitSlave,
//     updateSlave), master cmd 10/11 (swapTuner, changeSampleRate), slave
//     attach/detach and heartbeat losses;
//   - events (GainChange, PowerOverloadChange, RspDuoModeChange) to the
//     EventSink with the tuner argument clients expect; the service routes an event
//     whose tuner == slaveTuner() to the slave's sEvent ring, every other to mEvent.
//
// Streaming: Single-tuner mode runs the same per-block pipeline as
// the RSP1B (dsp::StreamProcessor + AGC). Dual/Master/Slave streams carry one
// tuner per ADC channel, each a real low-IF signal (I = master/A, Q = B/slave):
// two channel pipelines (DuoChan: StreamProcessor
// with RealInput I/Q, the model's down-converter selection, its own AGC and
// power estimator) run over the same packets. The protocol confirms the shared
// header toggles (rspduo::ToggleResult) and its tuner masks become each
// channel's grChanged/rfChanged/fsChanged. Dual publishes A and B into one mData
// slot (publishDual); Master publishes the master channel to mData and, while a
// slave is initialised, the slave channel to the slave sink (sData).
#pragma once

#include "core/device/common/CommonRspDevice.h"
#include "core/device/rspduo/RspDuoProtocol.h"

namespace sdrplay {
namespace device {

// RSPduo selection parameters (the caller's DeviceT at master SelectDevice) and
// the slave parameter map (<P>_sCmdMap, always created for hwVer 3).
struct RspDuoConfig {
    int rspDuoMode = 1;               // sdrplay_api_RspDuoModeT (1 single, 2 dual, 4 master)
    double rspDuoSampleFreq = 0.0;    // DeviceT.rspDuoSampleFreq
    ipc::CmdMap *slaveParams = nullptr;
};

class RspDuoDevice final : public common::CommonRspDevice<RspDuoDevice, rspduo::Protocol> {
public:
    explicit RspDuoDevice(const DeviceContext &ctx, const RspDuoConfig &cfg = RspDuoConfig{});
    ~RspDuoDevice() override;

    const ModelInfo &model() const override;
    // Master map defaults: fs/IF by mode and rspDuoSampleFreq.
    void writeDefaults(ipc::CmdMap &map) override;
    // Slave map defaults (also written on slave detach).
    void writeSlaveDefaults(ipc::CmdMap &map);

    // ---- RSPduo service paths
    void setSlaveParams(ipc::CmdMap *slaveMap); // (re)wire the slave map (finishSetup)
    sdrplay_api_ErrT initSlave();               // slave cmd 8
    sdrplay_api_ErrT uninitSlave();             // slave cmd 9 / locked slave Uninit
    // slave cmd 12: Update(T, reason, ext1, fromMaster = 0). The service logs an error for the
    // master-only bits (below Tuner_Gr) before calling it.
    sdrplay_api_ErrT updateSlave(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                                 sdrplay_api_ReasonForUpdateExtension1T ext1);
    void slaveAttached();                       // slave SelectDevice -> SlaveAttached
    void slaveDetached(bool dllDisappeared);    // slave release / heartbeat loss (after the map reset)
    void masterDisappeared();                   // master heartbeat loss with the slave alive
    void swapTuner(int newTuner);               // master cmd 10
    void changeSampleRate(double fs);           // master cmd 11
    // The slave's data ring (<P>_sData, service side). Event routing to <P>_sEvent (tuner ==
    // slaveTuner()) is the service's EventSink.
    void setSlaveSink(StreamSink *data);
    void rspDuoSwapTuner(int newTuner) override { swapTuner(newTuner); }
    void rspDuoChangeSampleRate(double fs) override { changeSampleRate(fs); }
    sdrplay_api_ErrT rspDuoInitSlave() override { return initSlave(); }
    sdrplay_api_ErrT rspDuoUninitSlave() override { return uninitSlave(); }
    sdrplay_api_ErrT rspDuoUpdateSlave(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                                       sdrplay_api_ReasonForUpdateExtension1T ext1) override
    {
        return updateSlave(tuner, reason, ext1);
    }
    void rspDuoSlaveAttached() override { slaveAttached(); }
    void rspDuoSlaveDetached(bool dllDisappeared) override { slaveDetached(dllDisappeared); }
    void rspDuoMasterDisappeared() override { masterDisappeared(); }
    bool rspDuoSlaveInitialised() const override { return slaveInitialised(); }
    double rspDuoMasterSampleFreq() const override { return rspDuoSampleFreq(); }

    int rspDuoMode() const;
    int tuner() const;
    int slaveTuner() const;                     // event routing
    double rspDuoSampleFreq() const;            // GetDevices slave offer
    bool slaveInitialised() const;

private:
    friend class common::CommonRspDevice<RspDuoDevice, rspduo::Protocol>;
    class DuoIo;

    void wireChannels();
    sdrplay_api_RxChannelParamsT *masterChannel() const; // the single-mode tuner's shared params
    bool singleMode() const;
    int chan() const { return proto_->tuner() == 2 ? 1 : 0; }

    // ---- CommonRspDevice hooks
    int protoUpdate(int tuner, std::uint32_t reason, std::uint32_t ext1)
    {
        return proto_->update(tuner, reason, ext1, true);
    }
    int protoAgcGain(int gRdB) { return proto_->applyAgcGain(proto_->tuner(), gRdB); }
    int protoAppliedGr() const { return proto_->appliedGr(chan()); }
    bool protoSampleNumSync() { return true; } // no deferred programSyncUpdate in the RSPduo engine
    sdrplay_api_RxChannelParamsT *liveChannel() const { return masterChannel(); }
    int logTuner() const { return proto_->tuner(); }
    sdrplay_api_TunerSelectT overloadTuner() const { return static_cast<sdrplay_api_TunerSelectT>(proto_->tuner()); }
    int agcMode() const { return proto_->mode(); }
    bool trackArms() const { return singleMode(); }
    // Also the Dual/Master/Slave channel pipelines (a dropped gain write clears every slot).
    void gainWriteDropped()
    {
        clearAgcWait(proc_, agc_);
        for (DuoChan &c : duo_) clearAgcWait(c.proc, c.agc);
    }
    bool uninitIdle() const { return !proto_->slaveInitialised() && proto_->state() == common::kStateIdle; }
    bool uninitClearsDsp(int rc) const { return rc == 0; }
    bool streamOther(std::uint32_t g, bool *result);
    bool streamsOther() const { return !singleMode(); }
    void latchDspForUpdate(int tuner);
    void configureDspAtInit(const Snapshot &s);
    void applyDspRequests();

    // ---- Dual / Master / Slave channel pipelines (see the header comment)
    class DuoHooks;
    struct DuoChan {
        dsp::StreamProcessor proc;
        dsp::agc::AgcChannel agc;
        dsp::agc::PowerEstimator pe;
        dsp::StreamProcessor::ExternalToggles ext;
        std::unique_ptr<DuoHooks> hooks;
        int agcGr = 0;
        double lastPow = 0.0;
        bool active = false;
    };
    int chanTuner(int c) const;                          // tuner number of channel c (1 = A, 2 = B)
    sdrplay_api_RxChannelParamsT *chanParams(int c) const; // the shared params the channel's tuner reads live
    void configureChan(int c);                           // channel init (Init, slave Init)
    void latchChan(int c);                               // the engine's latched params -> the channel DSP
    void syncBlockSize();
    bool duoStreamOnce(std::uint32_t g);

    RspDuoConfig cfg_;
    DuoChan duo_[2];
    int duoUpdTuner_ = 0;
    StreamSink *slaveSink_ = nullptr;
    std::vector<std::int16_t> scratchBI_, scratchBQ_;
};

} // namespace device
} // namespace sdrplay
