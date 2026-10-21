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

#include "core/device/rspduo/RspDuoDevice.h"

#include "core/dsp/Packet.h"

namespace sdrplay {
namespace device {

// rspduo::Io extension: RspDuoModeChange events.
class RspDuoDevice::DuoIo final : public DeviceIoT<rspduo::Io> {
public:
    explicit DuoIo(RspDuoDevice &d) : DeviceIoT<rspduo::Io>(d), duo_(d) {}

    void modeChange(int type, int tuner) override
    {
        sdrplay_api_EventParamsT p;
        std::memset(&p, 0, sizeof p);
        p.rspDuoModeParams.modeChangeType = static_cast<sdrplay_api_RspDuoModeCbEventIdT>(type);
        duo_.postEvent(sdrplay_api_RspDuoModeChange, static_cast<sdrplay_api_TunerSelectT>(tuner), p);
    }

private:
    RspDuoDevice &duo_;
};

RspDuoDevice::RspDuoDevice(const DeviceContext &ctx, const RspDuoConfig &cfg) : CommonRspDevice(ctx, kHwRspDuo), cfg_(cfg)
{
    DuoIo *io = new DuoIo(*this);
    setIo(std::unique_ptr<rsp1b::Io>(io));
    rspduo::Config pc;
    ipc::CmdMap *m = ctx_.params;
    pc.dev = m ? &m->devParams : nullptr;
    pc.internal = m ? &m->internal.params : nullptr;
    pc.mode = cfg_.rspDuoMode;
    pc.tuner = static_cast<int>(ctx_.tuner);
    pc.rspDuoSampleFreq = cfg_.rspDuoSampleFreq;
    setProtocol(std::unique_ptr<rspduo::Protocol>(new rspduo::Protocol(pc, *io)));
    wireChannels();
}

RspDuoDevice::~RspDuoDevice() = default;

const ModelInfo &RspDuoDevice::model() const
{
    static const ModelInfo kInfo = {kPidRspDuo, kHwRspDuo, "RSPduo", true};
    const ModelInfo *m = modelByHwVer(kHwRspDuo);
    return m ? *m : kInfo;
}

void RspDuoDevice::writeDefaults(ipc::CmdMap &map) { writeDefaultParams(map, cfg_.rspDuoMode, cfg_.rspDuoSampleFreq, false); }

void RspDuoDevice::writeSlaveDefaults(ipc::CmdMap &map)
{
    writeDefaultParams(map, cfg_.rspDuoMode, cfg_.rspDuoSampleFreq, true);
}

void RspDuoDevice::wireChannels()
{
    // Service channel wiring
    ipc::CmdMap *m = ctx_.params;
    ipc::CmdMap *s = cfg_.slaveParams;
    sdrplay_api_RxChannelParamsT *a = nullptr, *b = nullptr;
    const int mode = proto_->mode();
    const int t = proto_->tuner();
    if (mode == sdrplay_api_RspDuoMode_Single_Tuner || mode == sdrplay_api_RspDuoMode_Master) {
        if (t == 1) {
            a = m ? &m->rxChannelA : nullptr;
            b = s ? &s->rxChannelB : nullptr;
        } else {
            a = s ? &s->rxChannelA : nullptr;
            b = m ? &m->rxChannelB : nullptr;
        }
    } else {
        a = m ? &m->rxChannelA : nullptr;
        b = m ? &m->rxChannelB : nullptr;
    }
    proto_->setChannelSources(a, b);
}

void RspDuoDevice::setSlaveParams(ipc::CmdMap *slaveMap)
{
    std::lock_guard<std::mutex> l(eng_);
    cfg_.slaveParams = slaveMap;
    wireChannels();
}

bool RspDuoDevice::singleMode() const { return proto_->mode() == sdrplay_api_RspDuoMode_Single_Tuner; }

sdrplay_api_RxChannelParamsT *RspDuoDevice::masterChannel() const
{
    if (!ctx_.params) return nullptr;
    return proto_->tuner() == 2 ? &ctx_.params->rxChannelB : &ctx_.params->rxChannelA;
}

int RspDuoDevice::rspDuoMode() const { return proto_->mode(); }
int RspDuoDevice::tuner() const
{
    std::lock_guard<std::mutex> l(eng_);
    return proto_->tuner();
}
int RspDuoDevice::slaveTuner() const { return proto_->slaveTuner(); }
double RspDuoDevice::rspDuoSampleFreq() const
{
    std::lock_guard<std::mutex> l(eng_);
    return proto_->rspDuoSampleFreq();
}
bool RspDuoDevice::slaveInitialised() const
{
    std::lock_guard<std::mutex> l(eng_);
    return proto_->slaveInitialised();
}

sdrplay_api_ErrT RspDuoDevice::updateSlave(sdrplay_api_TunerSelectT tuner, sdrplay_api_ReasonForUpdateT reason,
                                           sdrplay_api_ReasonForUpdateExtension1T ext1)
{
    std::unique_lock<std::mutex> l(eng_);
    if (proto_->state() == common::kStateIdle) {
        if (ctx_.log) ctx_.log->log(log::Level::Error, __func__, "Wrong state: Uninitialised");
        return sdrplay_api_NotInitialised;
    }
    apiLock_ = &l;
    const int rc = proto_->update(static_cast<int>(tuner), static_cast<std::uint32_t>(common::raw(reason)),
                                  static_cast<std::uint32_t>(common::raw(ext1)), false);
    apiLock_ = nullptr;
    logUpdateFailure(static_cast<int>(tuner), rc);
    // the slave channel's DSP side (the engine latched the slave channel)
    duoUpdTuner_ = proto_->slaveTuner();
    applyDspRequests();
    return static_cast<sdrplay_api_ErrT>(rc);
}

sdrplay_api_ErrT RspDuoDevice::initSlave()
{
    std::unique_lock<std::mutex> l(eng_);
    apiLock_ = &l;
    const int rc = proto_->initSlave();
    apiLock_ = nullptr;
    (void)proto_->takeDspRequests();
    if (rc == 0 && !singleMode() && dspReady_) {
        // the slave channel joins the running stream (Q samples -> sData)
        configureChan(1);
        // one reset flag per stream: the slave's first block has reset = 0
        duo_[1].proc.cancelReset();
        syncBlockSize();
        if (slaveSink_) slaveSink_->restart();
    }
    if (rc == sdrplay_api_StartPending && ctx_.log)
        ctx_.log->log(log::Level::Error, __func__, "Tuner%d: master channel not initialised - start pending master signal",
                      proto_->slaveTuner());
    return static_cast<sdrplay_api_ErrT>(rc);
}

sdrplay_api_ErrT RspDuoDevice::uninitSlave()
{
    std::unique_lock<std::mutex> l(eng_);
    apiLock_ = &l;
    const int rc = proto_->uninitSlave();
    apiLock_ = nullptr;
    if (proto_->mode() != sdrplay_api_RspDuoMode_Dual_Tuner) duo_[1].active = false;
    return static_cast<sdrplay_api_ErrT>(rc);
}

void RspDuoDevice::slaveAttached()
{
    std::lock_guard<std::mutex> l(eng_);
    proto_->slaveAttached();
}

void RspDuoDevice::slaveDetached(bool dllDisappeared)
{
    std::lock_guard<std::mutex> l(eng_);
    proto_->slaveDetached(dllDisappeared);
}

void RspDuoDevice::masterDisappeared()
{
    std::unique_lock<std::mutex> l(eng_);
    apiLock_ = &l;
    proto_->masterDisappeared();
    apiLock_ = nullptr;
    dspReady_ = false;
}

void RspDuoDevice::swapTuner(int newTuner)
{
    std::lock_guard<std::mutex> l(eng_);
    ipc::CmdMap *m = ctx_.params;
    ipc::CmdMap *s = cfg_.slaveParams;
    if (newTuner == 1)
        proto_->swapTuner(1, m ? &m->rxChannelA : nullptr, s ? &s->rxChannelB : nullptr);
    else
        proto_->swapTuner(newTuner, s ? &s->rxChannelA : nullptr, m ? &m->rxChannelB : nullptr);
}

void RspDuoDevice::changeSampleRate(double fs)
{
    std::lock_guard<std::mutex> l(eng_);
    proto_->changeSampleRate(fs);
}

// ============================================================ Dual / Master / Slave channels
// Channel 0 = the I samples (Dual: tuner A, Master: the master tuner), channel 1 = the Q samples
// (Dual: tuner B, Master: the slave tuner).
class RspDuoDevice::DuoHooks final : public dsp::StreamHooks {
public:
    DuoHooks(RspDuoDevice &d, int c) : d_(d), c_(c) {}

    void powerEstimate(dsp::StreamProcessor &p, const dsp::BlockView &v) override
    {
        DuoChan &ch = d_.duo_[c_];
        dsp::agc::OverloadContext ctx;
        ctx.agcEnable = p.params().agcEnable;
        ctx.lastAgcGr = ch.agc.lastGr();
        dsp::agc::OverloadMsg m = dsp::agc::OverloadMsg::None;
        ch.lastPow = ch.pe.estimate(v.xi, v.xq, v.n, v.ionly, ctx, &m);
        if (m != dsp::agc::OverloadMsg::None) {
            sdrplay_api_EventParamsT ep;
            std::memset(&ep, 0, sizeof ep);
            ep.powerOverloadParams.powerOverloadChangeType =
                m == dsp::agc::OverloadMsg::Detected ? sdrplay_api_Overload_Detected : sdrplay_api_Overload_Corrected;
            d_.postEvent(sdrplay_api_PowerOverloadChange, static_cast<sdrplay_api_TunerSelectT>(d_.chanTuner(c_)), ep);
        }
    }

    void agc(dsp::StreamProcessor &p, const dsp::BlockView &) override
    {
        DuoChan &ch = d_.duo_[c_];
        if (!ch.agc.enabled() || ch.agc.waiting()) return;
        const int t = d_.chanTuner(c_);
        const sdrplay_api_RxChannelParamsT *live = d_.chanParams(c_);
        const sdrplay_api_RxChannelParamsT &lat = d_.proto_->channel(t == 2 ? 1 : 0);
        dsp::agc::AgcGainState g;
        g.gRdB = ch.agcGr;
        g.lnaState = live ? live->tunerParams.gain.LNAstate : lat.tunerParams.gain.LNAstate;
        g.minGr = common::raw(live ? live->tunerParams.gain.minGr : lat.tunerParams.gain.minGr);
        const dsp::agc::AgcDecision dec = ch.agc.evaluate(ch.lastPow, g);
        if (!dec.apply) return;
        const int rc = d_.proto_->applyAgcGain(t, dec.newGRdB);
        ++d_.stats_.agcWrites;
        const int applied = d_.proto_->appliedGr(t == 2 ? 1 : 0);
        ch.agc.applied(rc, applied, g.lnaState);
        if (applied >= 0) ch.agcGr = applied;
        if (rc == 0) p.setAgcWaitGain(true);
    }

    void dcCalPeriod(dsp::StreamProcessor &) override
    {
        DuoChan &ch = d_.duo_[c_];
        const int t = d_.chanTuner(c_);
        if (d_.proto_->channel(t == 2 ? 1 : 0).tunerParams.dcOffsetTuner.dcCal != 4) return;
        (void)d_.proto_->applyAgcGain(t, ch.agcGr);
    }

private:
    RspDuoDevice &d_;
    int c_;
};

void RspDuoDevice::setSlaveSink(StreamSink *data)
{
    std::lock_guard<std::mutex> l(eng_);
    slaveSink_ = data;
}

int RspDuoDevice::chanTuner(int c) const
{
    if (proto_->mode() == sdrplay_api_RspDuoMode_Dual_Tuner) return c == 0 ? 1 : 2;
    return c == 0 ? proto_->tuner() : proto_->slaveTuner();
}

sdrplay_api_RxChannelParamsT *RspDuoDevice::chanParams(int c) const
{
    ipc::CmdMap *m = ctx_.params;
    if (proto_->mode() == sdrplay_api_RspDuoMode_Dual_Tuner) {
        if (!m) return nullptr;
        return c == 0 ? &m->rxChannelA : &m->rxChannelB;
    }
    if (c == 0) return masterChannel();
    // slave channel in the slave map (wireChannels): master tuner 1 -> slave rxChannelB, else rxChannelA
    ipc::CmdMap *sm = cfg_.slaveParams;
    if (!sm) return nullptr;
    return proto_->tuner() == 1 ? &sm->rxChannelB : &sm->rxChannelA;
}

void RspDuoDevice::latchChan(int c)
{
    // the stream thread reads the engine's latched channel (A or B)
    const int t = chanTuner(c);
    const sdrplay_api_RxChannelParamsT &rx = proto_->channel(t == 2 ? 1 : 0);
    DuoChan &ch = duo_[c];
    dsp::ChannelParams &p = ch.proc.params();
    p.bwType = common::raw(rx.tunerParams.bwType);
    p.ifType = common::raw(rx.tunerParams.ifType);
    p.lnaState = rx.tunerParams.gain.LNAstate;
    p.dcEnable = rx.ctrlParams.dcOffset.DCenable != 0;
    p.iqEnable = rx.ctrlParams.dcOffset.IQenable != 0;
    p.decEnable = rx.ctrlParams.decimation.enable != 0;
    p.decFactor = rx.ctrlParams.decimation.decimationFactor;
    p.decWide = rx.ctrlParams.decimation.wideBandSignal;
    p.agcEnable = common::raw(rx.ctrlParams.agc.enable);
    p.adsbMode = common::raw(rx.ctrlParams.adsbMode);
    ch.agc.setParams(common::agcParamsOf(rx));
    const int applied = proto_->appliedGr(t == 2 ? 1 : 0);
    ch.agcGr = applied >= 0 ? applied : rx.tunerParams.gain.gRdB;
}

namespace {
// The RSPduo engine's down-converter choice as the DSP configuration:
// Master/Slave force LIF whatever the ifType; Dual uses the normal table.
dsp::DownConverterConfig duoDownConverter(const rsp1b::DownConv &dc, int mode, int ifType)
{
    dsp::DownConverterConfig c;
    const bool forced = rspduo::lifForced(mode);
    c.lif = forced || ifType != 0;
    c.enable = dc.enabled;
    c.path = dc.path;
    c.countShift = static_cast<std::uint32_t>(dc.countShift);
    c.countDivisor = static_cast<std::uint32_t>(dc.countDivisor);
    c.adsbCfg = dc.adsbCfg;
    c.ionly = c.lif && (!c.enable || c.path == dsp::DownConvPath::kNone);
    c.ifType = forced ? (dc.path == dsp::DownConvPath::kLif1620Decim3 ? sdrplay_api_IF_1_620 : sdrplay_api_IF_2_048)
                      : ifType;
    c.mixHz = c.ifType == 2048 ? 2048000.0 : c.ifType == 1620 ? 1620000.0 : c.ifType == 450 ? 450000.0 : 0.0;
    return c;
}
} // namespace

void RspDuoDevice::configureChan(int c)
{
    // channel init + initHardware's DSP controls for this channel; first block reset = 1
    DuoChan &ch = duo_[c];
    if (!ch.hooks) ch.hooks.reset(new DuoHooks(*this, c));
    latchChan(c);
    const int t = chanTuner(c);
    const rspduo::FsState &fs = proto_->fsState();
    ch.proc.setRealInput(c == 0 ? dsp::StreamProcessor::RealInput::I : dsp::StreamProcessor::RealInput::Q);
    ch.proc.setIqFor504(true); // RSPduo outside single-tuner mode
    ch.proc.setExternalToggles(&ch.ext);
    ch.proc.setDownConverterOverride(
        true, duoDownConverter(proto_->downConv(t == 2 ? 1 : 0), proto_->mode(), ch.proc.params().ifType));
    ch.proc.setSamplesPerPacket(static_cast<std::uint32_t>(fs.spp));
    ch.proc.setConfirmedFsHz(cur_.dev.fsFreq.fsHz);
    ch.proc.initChannel(fs.fsMHz);
    ch.proc.dcOffsetIqControl();
    ch.proc.updateDecimation();
    ch.proc.requestReset();
    ch.agc.channelInit();
    (void)ch.agc.control(dsp::agc::setPointMin(fs.spp, kHwRspDuo, proto_->mode(), ch.proc.params().ifType));
    ch.pe.reset();
    ch.active = true;
}

void RspDuoDevice::syncBlockSize()
{
    // one packets-per-block count for the device, from the I channel; the base
    // reader uses proc_'s count to size its USB reads
    const std::uint32_t n = duo_[0].proc.packetsPerBlock();
    duo_[1].proc.setPacketsPerBlock(n);
    proc_.setPacketsPerBlock(n);
}

void RspDuoDevice::configureDspAtInit(const Snapshot &s)
{
    if (singleMode()) {
        CommonRspDevice::configureDspAtInit(s);
        return;
    }
    duo_[0].active = duo_[1].active = false;
    configureChan(0);
    if (proto_->mode() == sdrplay_api_RspDuoMode_Dual_Tuner || proto_->slaveInitialised()) configureChan(1);
    syncBlockSize();
    (void)proto_->takeDspRequests(); // already applied above
    if (ctx_.sink) ctx_.sink->restart();
    if (slaveSink_) slaveSink_->restart();
    dspReady_ = true;
}

void RspDuoDevice::latchDspForUpdate(int tuner)
{
    if (singleMode()) {
        CommonRspDevice::latchDspForUpdate(tuner);
        return;
    }
    duoUpdTuner_ = tuner; // the engine latches the channel inside Update; applyDspRequests copies it
}

void RspDuoDevice::applyDspRequests()
{
    if (singleMode()) {
        CommonRspDevice::applyDspRequests();
        return;
    }
    const rsp1b::DspRequests r = proto_->takeDspRequests();
    const rspduo::FsState &fs = proto_->fsState();
    for (int c = 0; c < 2; ++c) {
        DuoChan &ch = duo_[c];
        if (!ch.active) continue;
        const int t = chanTuner(c);
        const bool mine = duoUpdTuner_ == 0 || (duoUpdTuner_ & t) != 0;
        if (r.streamRestarted) {
            ch.proc.setSamplesPerPacket(static_cast<std::uint32_t>(fs.spp));
            ch.proc.setConfirmedFsHz(cur_.dev.fsFreq.fsHz);
            ch.proc.requestReset();
            ch.proc.forceFsChanged();
            ch.agc.streamRestart();
        }
        if (!mine) continue;
        latchChan(c);
        if (r.downConverterReinit) {
            ch.proc.setDownConverterOverride(
                true, duoDownConverter(proto_->downConv(t == 2 ? 1 : 0), proto_->mode(), ch.proc.params().ifType));
            ch.proc.configureDownConverter(fs.fsMHz);
        }
        if (r.decimationChanged) ch.proc.updateDecimation();
        if (r.dcIqReset) ch.proc.dcOffsetIqControl();
        if (r.agcChanged)
            (void)ch.agc.control(dsp::agc::setPointMin(fs.spp, kHwRspDuo, proto_->mode(), ch.proc.params().ifType));
        if (r.agcUserRequest && ch.agc.enabled()) ch.agc.requestApply();
        if (r.overloadAck) ch.pe.detector().ack();
    }
    syncBlockSize();
}

bool RspDuoDevice::streamOther(std::uint32_t g, bool *result)
{
    if (singleMode()) return false;
    *result = duoStreamOnce(g);
    return true;
}

bool RspDuoDevice::duoStreamOnce(std::uint32_t g)
{
    const bool dual = proto_->mode() == sdrplay_api_RspDuoMode_Dual_Tuner;
    // output slots (up to 10 x 10 ms each, else processed and dropped)
    StreamBlock blk, blkS;
    const bool pub = ctx_.sink && ctx_.sink->acquire(blk, common::kSlotWaitMs);
    StreamSink *ss = nullptr;
    {
        std::lock_guard<std::mutex> l(eng_);
        if (!dual && duo_[1].active && proto_->slaveInitialised()) ss = slaveSink_;
    }
    const bool pubS = ss && ss->acquire(blkS, common::kSlotWaitMs);
    if (scratchBI_.empty()) {
        scratchBI_.assign(dsp::kMaxBlockSamples, 0);
        scratchBQ_.assign(dsp::kMaxBlockSamples, 0);
    }
    dsp::BlockBuffers bufs[2];
    bufs[0] = pub ? dsp::BlockBuffers{blk.xi, blk.xq, blk.xiReadable, blk.xqReadable}
                  : dsp::BlockBuffers{scratchI_.data(), scratchQ_.data(), scratchI_.size(), scratchQ_.size()};
    if (dual && pub && blk.xiB && blk.xqB)
        bufs[1] = dsp::BlockBuffers{blk.xiB, blk.xqB, blk.xiBReadable, blk.xqBReadable};
    else if (!dual && pubS)
        bufs[1] = dsp::BlockBuffers{blkS.xi, blkS.xq, blkS.xiReadable, blkS.xqReadable};
    else
        bufs[1] = dsp::BlockBuffers{scratchBI_.data(), scratchBQ_.data(), scratchBI_.size(), scratchBQ_.size()};

    dsp::BlockInfo info[2];
    bool done[2] = {false, false};
    bool processed = false;
    {
        std::lock_guard<std::mutex> l(eng_);
        const std::uint32_t nNow = duo_[0].proc.packetsPerBlock();
        if (gen_.load() != g || !dspReady_ || proto_->state() != common::kStateStreaming) {
            if (gen_.load() == g || static_cast<std::int32_t>(flushGen_.load() - g) > 0 || !dspReady_) queued_ = 0;
            ++stats_.discarded;
        } else if (queued_ < nNow) {
            // a partial block (N grew, or the read was short): read more first
        } else {
            const std::size_t use = nNow;
            // Packet fetch: one shared gain/RF/Fs toggle set for both tuners (rspduo::ToggleResult)
            for (DuoChan &ch : duo_) ch.ext = dsp::StreamProcessor::ExternalToggles{};
            bool fsConfirmed = false;
            for (std::size_t i = 0; i < use; ++i) {
                const dsp::PacketHeader h = dsp::parsePacketHeader(queueData() + i * dsp::kPacketBytes);
                rspduo::ToggleBits t;
                t.gr = h.grToggle != 0;
                t.rf = h.rfToggle != 0;
                t.fs = h.fsToggle != 0;
                const rspduo::ToggleResult res = proto_->onPacketToggles(t);
                const std::uint32_t bit = 1u << (i & 31u);
                for (int c = 0; c < 2; ++c) {
                    const int tm = chanTuner(c) == 2 ? 2 : 1; // ToggleResult masks: bit 0 A, bit 1 B
                    DuoChan &ch = duo_[c];
                    if (res.grConfirmedMask & tm) ch.ext.grConfirmed |= bit;
                    if (res.grReportMask & tm) ch.ext.grReport |= bit;
                    if ((res.rfConfirmedMask & tm) && res.rfReport) ch.ext.rf |= bit;
                    if (res.fsConfirmed && res.fsReport) ch.ext.fs |= bit;
                }
                if (res.fsConfirmed) fsConfirmed = true;
            }
            dspActive_ = true;
            for (int c = 0; c < 2; ++c) {
                DuoChan &ch = duo_[c];
                if (!ch.active) continue;
                if (const sdrplay_api_RxChannelParamsT *live = chanParams(c))
                    ch.proc.params().adsbMode = common::raw(live->ctrlParams.adsbMode); // read live per block
                const int applied = proto_->appliedGr(chanTuner(c) == 2 ? 1 : 0);
                if (applied >= 0) ch.agcGr = applied;
                const double fs = ch.proc.confirmedFsHz();
                const int spp = static_cast<int>(ch.proc.samplesPerPacket());
                ch.agc.blockStart(fs, spp, static_cast<int>(nNow));
                dsp::MemoryPacketSource src(queueData(), use);
                ch.proc.processBlock(src, bufs[c], info[c], ch.hooks.get());
                if (!ch.proc.agcWaitGain()) ch.agc.gainConfirmed();
                ch.agc.blockEnd(ch.proc.confirmedFsHz(), spp, static_cast<int>(nNow));
                if (fsConfirmed) ch.proc.setConfirmedFsHz(cur_.dev.fsFreq.fsHz);
                if (info[c].dcNotEnabled) {
                    rsp1b::ErrorRecord r; // recorded for every block while DC offset correction is off
                    r.file = __FILE__;
                    r.function = __func__;
                    r.line = __LINE__;
                    r.message = "DCoffset not enabled";
                    r.logAs = rsp1b::LogAs::None;
                    record(r);
                }
                done[c] = true;
            }
            dspActive_ = false;
            qhead_ += use;
            queued_ -= use;
            if (queued_ == 0) qhead_ = 0;
            processed = true;
            ++stats_.blocks;
            if (info[0].readError) ++stats_.readErrors;
            if (!pub) ++stats_.dropped;
        }
    }
    auto cb = [](const dsp::BlockInfo &i) {
        sdrplay_api_StreamCbParamsT p;
        p.firstSampleNum = i.firstSampleNum;
        p.grChanged = static_cast<int>(i.grChanged);
        p.rfChanged = static_cast<int>(i.rfChanged);
        p.fsChanged = static_cast<int>(i.fsChanged);
        p.numSamples = i.numSamples;
        return p;
    };
    if (pub) {
        if (processed && done[0]) {
            if (dual && done[1]) ctx_.sink->publishDual(blk, cb(info[0]), cb(info[1]), info[0].reset);
            else ctx_.sink->publish(blk, cb(info[0]), info[0].reset);
        } else {
            ctx_.sink->cancel(blk);
        }
    }
    if (pubS) {
        if (processed && done[1]) ss->publish(blkS, cb(info[1]), info[1].reset);
        else ss->cancel(blkS);
    }
    flushDspOutputs(processed && pub);
    if (processed && info[0].readError) platform::sleepMs(10); // skip one iteration, next block reset = 1
    return processed;
}

} // namespace device
} // namespace sdrplay
