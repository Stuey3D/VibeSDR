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

#include "core/device/rspduo/RspDuoProtocol.h"

#include "core/device/rspduo/RspDuoTables.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sdrplay {
namespace device {
namespace rspduo {

using common::kInitOk;
using common::kInitParams;
using common::kInitHardware;
using common::kInitChannelState;
using common::kInitStart;
using common::kStateIdle;
using common::kStateInitialised;
using common::kStateStopped;
using common::kStateStreaming;
using namespace rsp1b::hw;

namespace {

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
std::string fmt(const char *f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

template <typename E> int raw(const E &e)
{
    static_assert(sizeof(E) == 4, "API enums are 32-bit");
    std::int32_t v;
    std::memcpy(&v, &e, sizeof v);
    return v;
}

// Tuner standby block (Uninit / uninitSlave): r0 0x7300, r1 0x1400, r2, r3 0, r4 0, r5, r6.
const std::uint32_t kStandby[7] = {0x073000, 0x014001, 0x201982, 0x000003, 0x000004, 0x289C05, 0x200016};

constexpr std::uint32_t kBridgeGpioDuo = 0x00EA80; // RSPduo bridge GPIO configuration (register 8)
constexpr std::uint8_t kDuoBiasTOff = 0x02;        // expander 1 GPIOA bit 1: tuner 2 bias-T off

// tunerParams.gain (gRdB, LNAstate, syncUpdate, minGr, gainVals), copied as a whole.
void copyGainStruct(sdrplay_api_RxChannelParamsT &dst, const sdrplay_api_RxChannelParamsT &src)
{
    std::memcpy(&dst.tunerParams.gain, &src.tunerParams.gain, sizeof dst.tunerParams.gain);
}
} // namespace

Protocol::Protocol(const Config &cfg, Io &io)
    : devShared_(cfg.dev), rxShared_{cfg.rxA, cfg.rxB}, internalShared_(cfg.internal), io_(io), mode_(cfg.mode),
      tuner_(cfg.tuner), slaveTuner_(cfg.mode == sdrplay_api_RspDuoMode_Master ? (cfg.tuner == 2 ? 1 : 2) : 0),
      rspDuoSampleFreq_(cfg.rspDuoSampleFreq),
      // Default params: fs8192 is set for the master map in Dual tuner or Master mode (and
      // every slave map) when the sample rate is 8.192 MHz.
      fs8192_((cfg.mode == sdrplay_api_RspDuoMode_Dual_Tuner || cfg.mode == sdrplay_api_RspDuoMode_Master ||
               cfg.mode == sdrplay_api_RspDuoMode_Slave) &&
              cfg.rspDuoSampleFreq == 8192000.0)
{
    for (Chan &c : ch_) c.appliedGr = -1;
}

void Protocol::setChannelSources(sdrplay_api_RxChannelParamsT *rxA, sdrplay_api_RxChannelParamsT *rxB)
{
    rxShared_[0] = rxA;
    rxShared_[1] = rxB;
}

void Protocol::swapTuner(int newTuner, sdrplay_api_RxChannelParamsT *rxA, sdrplay_api_RxChannelParamsT *rxB)
{
    tuner_ = newTuner;
    setChannelSources(rxA, rxB);
}

void Protocol::changeSampleRate(double fs)
{
    rspDuoSampleFreq_ = fs;
    fs8192_ = fs == 8192000.0;
}

// ------------------------------------------------------------------ primitives
int Protocol::ctl(std::uint8_t bmRT, std::uint8_t req, std::uint16_t wValue, std::uint16_t wIndex,
                  std::uint16_t wLength, std::uint8_t *in)
{
    ControlTransfer t;
    t.bmRequestType = bmRT;
    t.bRequest = req;
    t.wValue = wValue;
    t.wIndex = wIndex;
    t.wLength = wLength;
    const int r = io_.control(t);
    if (in && t.isIn())
        for (unsigned i = 0; i < wLength && i < t.data.size(); ++i) in[i] = t.data[i];
    return r == 0 ? 0 : 1;
}

int Protocol::setRegister(std::uint8_t regByte, std::uint32_t value)
{
    const std::uint32_t w = (value & 0xFFFFFFu) << 8 | regByte;
    return ctl(0x40, rsp1b::kReqWriteReg, static_cast<std::uint16_t>(w & 0xFFFF), static_cast<std::uint16_t>(w >> 16),
               0);
}

int Protocol::gpioWrite(std::uint8_t reg, std::uint8_t val)
{
    return ctl(0x40, rsp1b::kReqGpioExt, static_cast<std::uint16_t>(reg << 8 | val), 0x0040, 0);
}

int Protocol::dualWrite(int expander, std::uint8_t reg, std::uint8_t val)
{
    // expander 0 through kReqGpioDual0, expander 1 through kReqGpioDual1; wValue = reg << 8 | val
    return ctl(0x40, expander == 0 ? rsp1b::kReqGpioDual0 : rsp1b::kReqGpioDual1,
               static_cast<std::uint16_t>(reg << 8 | val), 0x0040, 0);
}

int Protocol::setInterface(std::uint8_t alt) { return ctl(0x01, rsp1b::kReqSetInterface, alt, 0, 0); }
int Protocol::startRx() { return ctl(0x40, rsp1b::kReqStart, 0, 0, 0); }
int Protocol::stopRx() { return ctl(0x40, rsp1b::kReqStop, 0, 0, 0); }

void Protocol::err(const char *file, const char *fn, int line, std::string msg, LogAs as)
{
    ErrorRecord r;
    r.file = file;
    r.function = fn;
    r.line = line;
    r.message = std::move(msg);
    r.logAs = as;
    io_.error(r);
}

void Protocol::latchDev()
{
    if (devShared_) dev_ = *devShared_;
}

void Protocol::latchChannel(int idx, const sdrplay_api_RxChannelParamsT *src)
{
    if (src) ch_[idx & 1].p = *src;
}

DspRequests Protocol::takeDspRequests()
{
    DspRequests r = dsp_;
    dsp_ = DspRequests{};
    return r;
}

DownConv Protocol::downConv(int idx) const
{
    const Chan &c = ch_[idx & 1];
    return downConverter(dev_.fsFreq.fsHz, raw(c.p.tunerParams.bwType), raw(c.p.tunerParams.ifType), mode_, fs8192_);
}

// ------------------------------------------------------------------ select
void Protocol::select()
{
    // reset all state
    state_ = kStateIdle;
    bridgeOpen_ = false;
    fnAddrDone_ = false;
    dual_ = false;
    tunerSel_ = 0;
    gB_ = r2a_ = r2b_ = d1a_ = d1b_ = d2a_ = d2b_ = dx_ = 0;
    fs_ = FsState{};
    for (Chan &c : ch_) {
        c = Chan{};
        c.appliedGr = -1;
    }
    rfToggle_ = rfPending_ = rfReportReq_ = rfReport_ = false;
    rfSlot_ = 0;
    fsToggle_ = fsPending_ = fsReportReq_ = fsReport_ = false;
    grToggle_ = false;
    grValue_ = grSlot1_ = grSlot2_ = grRep1_ = grRep2_ = 0;
    grNote1_ = grNote2_ = grUserFlag_ = false;
    syncPeriod_ = 0;
    slaveInit_ = slaveStartPending_ = stopPending_ = false;
    dsp_ = DspRequests{};
}

// ------------------------------------------------------------------ limits
int Protocol::validateDevParams()
{
    // reads the shared devParams
    if (!devShared_) return 0;
    const sdrplay_api_DevParamsT &d = *devShared_;
    const char *fn = __func__;
    auto rng = [&](int line, const char *what, long long lo, long long hi, long long v) {
        err(__FILE__, fn, line, common::limitMsg(what, lo, hi, v));
        return sdrplay_api_OutOfRange;
    };
    if (d.ppm < -1000.0 || d.ppm > 1000.0) {
        err(__FILE__, fn, __LINE__, fmt("ppm out of range (%f:%f) = %f", -1000.0, 1000.0, d.ppm));
        return sdrplay_api_OutOfRange;
    }
    const double fs = d.fsFreq.fsHz;
    if (!(fs >= 2000000.0 && fs <= 10000000.0)) {
        err(__FILE__, fn, __LINE__, fmt("fsFreq fsHz out of range (%f:%f) = %f", 2000000.0, 10000000.0, fs));
        return sdrplay_api_OutOfRange;
    }
    if (mode_ == sdrplay_api_RspDuoMode_Master && fs != 6000000.0 && fs != 8000000.0 && fs != 8192000.0) {
        err(__FILE__, fn, __LINE__,
            fmt("fsFreq fsHz out of range for master/slave mode(%.1f or %.1f or %.1f) = %.1f", 6000000.0, 8000000.0,
                8192000.0, fs));
        return sdrplay_api_OutOfRange;
    }
    if (d.fsFreq.syncUpdate > 1) return rng(__LINE__, "fsFreq syncUpdate", 0, 1, d.fsFreq.syncUpdate);
    if (d.fsFreq.reCal > 1) return rng(__LINE__, "fsFreq reCal", 0, 1, d.fsFreq.reCal);
    if (raw(d.mode) < 0 || raw(d.mode) > 1) return rng(__LINE__, "mode", 0, 1, raw(d.mode));
    if (d.rsp1aParams.rfDabNotchEnable > 1) return rng(__LINE__, "rsp1a rfDabNotchEnable", 0, 1, d.rsp1aParams.rfDabNotchEnable);
    if (d.rsp1aParams.rfNotchEnable > 1) return rng(__LINE__, "rsp1a rfNotchEnable", 0, 1, d.rsp1aParams.rfNotchEnable);
    if (d.rsp2Params.extRefOutputEn > 1) return rng(__LINE__, "rsp2 extRefOutputEn", 0, 1, d.rsp2Params.extRefOutputEn);
    if (raw(d.rspDuoParams.extRefOutputEn) < 0 || raw(d.rspDuoParams.extRefOutputEn) > 1)
        return rng(__LINE__, "rspDuo extRefOutputEn", 0, 1, raw(d.rspDuoParams.extRefOutputEn));
    if (d.rspDxParams.hdrEnable > 1) return rng(__LINE__, "rspDx hdrEnable", 0, 1, d.rspDxParams.hdrEnable);
    if (d.rspDxParams.biasTEnable > 1) return rng(__LINE__, "rspDx biasTEnable", 0, 1, d.rspDxParams.biasTEnable);
    if (d.rspDxParams.rfDabNotchEnable > 1) return rng(__LINE__, "rspDx rfDabNotchEnable", 0, 1, d.rspDxParams.rfDabNotchEnable);
    if (d.rspDxParams.rfNotchEnable > 1) return rng(__LINE__, "rspDx rfNotchEnable", 0, 1, d.rspDxParams.rfNotchEnable);
    if (raw(d.rspDxParams.antennaSel) < 0 || raw(d.rspDxParams.antennaSel) > 2)
        return rng(__LINE__, "rspDx antennaSel", 0, 2, raw(d.rspDxParams.antennaSel));
    return 0;
}

int Protocol::validateTuner(int tuner)
{
    // Checks the shared channel of tuner 2, else of tuner 1 (Dual tuner, T = 3, checks rxA).
    const sdrplay_api_RxChannelParamsT *src = shared(tuner);
    if (!src) return 0;
    const sdrplay_api_RxChannelParamsT &c = *src;
    const char *fn = __func__;
    auto trng = [&](int line, const char *what, long long lo, long long hi, long long v) {
        err(__FILE__, fn, line, common::tunerLimitMsg(tuner, what, lo, hi, v));
        return sdrplay_api_OutOfRange;
    };
    const sdrplay_api_TunerParamsT &t = c.tunerParams;
    const sdrplay_api_ControlParamsT &k = c.ctrlParams;
    const int bw = raw(t.bwType);
    if (bw < 0 || bw > 8000) return trng(__LINE__, "bwType", 0, 8000, bw);
    const int ift = raw(t.ifType);
    if (ift < -1 || ift > 2048) return trng(__LINE__, "ifType", -1, 2048, ift);
    // Master mode adds no ifType rule here; its sample rate is limited by validateDevParams.
    if (raw(t.loMode) < 0 || raw(t.loMode) > 4) return trng(__LINE__, "loMode", 0, 4, raw(t.loMode));
    const int minGr = raw(t.gain.minGr);
    if (minGr < 0 || minGr > 20) return trng(__LINE__, "gain minGr", 0, 20, minGr);
    if (!(t.gain.gRdB < 60 && minGr <= t.gain.gRdB)) return trng(__LINE__, "gain gRdB", minGr, 59, t.gain.gRdB);
    if (t.gain.syncUpdate > 1) return trng(__LINE__, "gain syncUpdate", 0, 1, t.gain.syncUpdate);
    if (t.gain.LNAstate > 9) return trng(__LINE__, "gain LNAstate", 0, 9, t.gain.LNAstate);
    if (t.rfFreq.rfHz < 0.0 || t.rfFreq.rfHz > 2000000000.0) {
        err(__FILE__, fn, __LINE__, fmt("Tuner%d: rfFreq rfHz out of range (%f:%f) = %f", tuner, 0.0, 2000000000.0, t.rfFreq.rfHz));
        return sdrplay_api_OutOfRange;
    }
    if (t.rfFreq.syncUpdate > 1) return trng(__LINE__, "rfFreq syncUpdate", 0, 1, t.rfFreq.syncUpdate);
    if (t.dcOffsetTuner.dcCal > 5) return trng(__LINE__, "dcOffsetTuner dcCal", 0, 5, t.dcOffsetTuner.dcCal);
    if (t.dcOffsetTuner.speedUp > 1) return trng(__LINE__, "dcOffsetTuner speedUp", 0, 1, t.dcOffsetTuner.speedUp);
    if (t.dcOffsetTuner.trackTime < 0 || t.dcOffsetTuner.trackTime > 63)
        return trng(__LINE__, "dcOffsetTuner trackTime", 0, 63, t.dcOffsetTuner.trackTime);
    if (t.dcOffsetTuner.refreshRateTime < 0 || t.dcOffsetTuner.refreshRateTime > 4095)
        return trng(__LINE__, "dcOffsetTuner refreshRateTime", 0, 4095, t.dcOffsetTuner.refreshRateTime);
    if (k.dcOffset.DCenable > 1) return trng(__LINE__, "dcOffset DCenable", 0, 1, k.dcOffset.DCenable);
    if (k.dcOffset.IQenable > 1) return trng(__LINE__, "dcOffset IQenable", 0, 1, k.dcOffset.IQenable);
    if (k.decimation.decimationFactor < 1 || k.decimation.decimationFactor > 32)
        return trng(__LINE__, "decimation decimationFactor", 1, 32, k.decimation.decimationFactor);
    if (k.decimation.wideBandSignal > 2) return trng(__LINE__, "decimation wideBandSignal", 0, 2, k.decimation.wideBandSignal);
    // accepts every sdrplay_api_AgcControlT value; the message keeps the logged range 0..1
    if (raw(k.agc.enable) < sdrplay_api_AGC_DISABLE || raw(k.agc.enable) > sdrplay_api_AGC_CTRL_EN)
        return trng(__LINE__, "agc enable", 0, 1, raw(k.agc.enable));
    if (k.agc.syncUpdate < 0 || k.agc.syncUpdate > 1) return trng(__LINE__, "agc syncUpdate", 0, 1, k.agc.syncUpdate);
    if (k.agc.setPoint_dBfs < -72 || k.agc.setPoint_dBfs > 0)
        return trng(__LINE__, "agc setPoint_dBfs", -72, 0, k.agc.setPoint_dBfs);
    if (c.rsp1aTunerParams.biasTEnable > 1) return trng(__LINE__, "rsp1a biasTEnable", 0, 1, c.rsp1aTunerParams.biasTEnable);
    if (c.rsp2TunerParams.biasTEnable > 1) return trng(__LINE__, "rsp2 biasTEnable", 0, 1, c.rsp2TunerParams.biasTEnable);
    if (c.rsp2TunerParams.rfNotchEnable > 1) return trng(__LINE__, "rsp2 rfNotchEnable", 0, 1, c.rsp2TunerParams.rfNotchEnable);
    if (raw(c.rsp2TunerParams.amPortSel) < 0 || raw(c.rsp2TunerParams.amPortSel) > 1)
        return trng(__LINE__, "rsp2 amPortSel", 0, 1, raw(c.rsp2TunerParams.amPortSel));
    if (raw(c.rsp2TunerParams.antennaSel) < 5 || raw(c.rsp2TunerParams.antennaSel) > 6)
        return trng(__LINE__, "rsp2 antennaSel", 5, 6, raw(c.rsp2TunerParams.antennaSel));
    if (c.rspDuoTunerParams.biasTEnable > 1) return trng(__LINE__, "rspDuo biasTEnable", 0, 1, c.rspDuoTunerParams.biasTEnable);
    if (raw(c.rspDuoTunerParams.tuner1AmPortSel) < 0 || raw(c.rspDuoTunerParams.tuner1AmPortSel) > 1)
        return trng(__LINE__, "rspDuo tuner1AmPortSel", 0, 1, raw(c.rspDuoTunerParams.tuner1AmPortSel));
    if (c.rspDuoTunerParams.tuner1AmNotchEnable > 1)
        return trng(__LINE__, "rspDuo tuner1AmNotchEnable", 0, 1, c.rspDuoTunerParams.tuner1AmNotchEnable);
    if (c.rspDuoTunerParams.rfNotchEnable > 1) return trng(__LINE__, "rspDuo rfNotchEnable", 0, 1, c.rspDuoTunerParams.rfNotchEnable);
    if (c.rspDuoTunerParams.rfDabNotchEnable > 1)
        return trng(__LINE__, "rspDuo rfDabNotchEnable", 0, 1, c.rspDuoTunerParams.rfDabNotchEnable);
    if (raw(c.rspDxTunerParams.hdrBw) < 0 || raw(c.rspDxTunerParams.hdrBw) > 3)
        return trng(__LINE__, "rspDx hdrBw", 0, 3, raw(c.rspDxTunerParams.hdrBw));
    return 0;
}

// ------------------------------------------------------------------ Init
int Protocol::init()
{
    const int T = tuner_;
    initFailStage_ = kInitOk;
    dual_ = T == 3; // set before the state check
    if (state_ != kStateIdle) {
        err(OPENRSP_HERE, "Not in uninitialised state");
        return sdrplay_api_AlreadyInitialised;
    }
    int r = validateDevParams();
    if (r == 0 && (T & 3) != 0) r = validateTuner(T);
    if (r != 0) {
        initFailStage_ = kInitParams;
        return r;
    }
    latchDev();
    latchChannel(0, rxShared_[0]);
    latchChannel(1, rxShared_[1]);
    if (internalShared_) internal_ = *internalShared_;
    r = initChannelState(T, true);
    if (r != 0) {
        initFailStage_ = kInitChannelState;
        return r;
    }
    if (mode_ == sdrplay_api_RspDuoMode_Dual_Tuner) {
        ch_[1] = ch_[0];
        if (rxShared_[0] && rxShared_[1]) *rxShared_[1] = *rxShared_[0];
    }
    r = initHardware(T, true);
    if (r != 0) {
        initFailStage_ = kInitHardware;
        return r;
    }
    state_ = kStateInitialised;
    r = start(false);
    if (r != 0) {
        initFailStage_ = kInitStart;
        return r;
    }
    if (slaveStartPending_) {
        io_.modeChange(sdrplay_api_MasterInitialised, slaveTuner_);
        slaveStartPending_ = false;
    }
    return 0;
}

int Protocol::initChannelState(int tuner, bool master)
{
    if (master) {
        const Chan &c = ch_[chIdx(tuner)];
        FsState st;
        if (rspduo::fsState(dev_.fsFreq.fsHz, raw(c.p.tunerParams.ifType), mode_, raw(dev_.mode) == 1, st) != 0) {
            err(OPENRSP_HERE, fmt("fsMHz out of range %.3f", dev_.fsFreq.fsHz / 1e6));
            return sdrplay_api_OutOfRange;
        }
        fs_ = st;
        rfToggle_ = rfPending_ = false;
        fsToggle_ = fsPending_ = false;
        grToggle_ = false;
        grSlot1_ = grSlot2_ = 0;
        syncPeriod_ = 0;
        // shadow defaults: B R2A R2B D1A D1B D2A D2B = ff 00 00 9f ff ff ff; DX = ff
        gB_ = 0xFF; r2a_ = 0x00; r2b_ = 0x00;
        d1a_ = 0x9F; d1b_ = 0xFF; d2a_ = 0xFF; d2b_ = 0xFF;
        dx_ = 0xFF;
        dsp_ = DspRequests{};
    }
    channelInit(tuner == 1 || tuner == 3 ? tuner : 2);
    return 0;
}

void Protocol::channelInit(int tuner)
{
    // channel init: down-converter state, applied GR -1, firstLO 120 MHz, LOflag 0, dcCal/speedUp
    // applied 3/0, AGC enable state 0; then applyLoMode and (if it succeeds) the band state.
    Chan &c = ch_[chIdx(tuner)];
    configureDownConverter(tuner);
    c.appliedGr = -1;
    c.firstLO = 120000000;
    c.loFlag = false;
    c.dcCalApplied = 3;
    c.speedUpApplied = 0;
    c.agcEnableState = 0;
    if (applyLoMode(tuner) == 0) {
        BandState b;
        int flo = c.firstLO;
        if (selectBand(tuner, b, flo) == 0) {
            c.band = b;
            c.firstLO = flo;
            if (b.loFlag) c.loFlag = true;
        }
    }
}

int Protocol::initHardware(int T, bool master)
{
    if (master) {
        if (const int rc = setRegister(kRegBridgeGpio, kBridgeGpioDuo); rc != 0) {
            errVerbose(OPENRSP_HERE, fmt("register 0x08 write failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
        expanderInit(T);
        if (const int rc = bridgeInit(); rc != 0) {
            errVerbose(OPENRSP_HERE, fmt("Tuner%d: bridge init returned error %d", T, rc));
            return sdrplay_api_HwError;
        }
        extRef(false);
        rfTest(false);
        tunerSelect(T);
        if (const int rc = tunerWrite(kTunerReg5Init); rc != 0) {
            errVerbose(OPENRSP_HERE, fmt("register 0x09 write failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
        // rate hardware update (result ignored by initHardware)
        if (const int rc = setRegister(kRegPacketFormat, fs_.reg7); rc != 0)
            errVerbose(OPENRSP_HERE, fmt("register 0x07 write failed 0x%08X", rc));
        applyTunerDc(T);
        applyLoMode(T);
        configureDcIq(T);
        updateDecimation(T);
        configureAgc(T);
        if (mode_ == sdrplay_api_RspDuoMode_Single_Tuner) {
            if (tuner_ == 1) channelConfig(0);
            else if (tuner_ == 2) channelConfig(1);
            else channelConfig(3);
        } else if (mode_ == sdrplay_api_RspDuoMode_Dual_Tuner ||
                   (mode_ == sdrplay_api_RspDuoMode_Master && tuner_ == 1)) {
            channelConfig(2);
        } else {
            channelConfig(3);
        }
        tuner1AmNotch(T, false);
    } else {
        tunerSelect(T);
        if (const int rc = tunerWrite(kTunerReg14Clear); rc != 0) {
            errVerbose(OPENRSP_HERE, fmt("register 0x09 write failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
        if (const int rc = tunerWrite(kTunerReg5Init); rc != 0) {
            errVerbose(OPENRSP_HERE, fmt("register 0x09 write failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
        applyTunerDc(T);
        applyLoMode(T);
        configureDcIq(T);
        updateDecimation(T);
        configureAgc(T);
    }

    auto gain = [&](int t) {
        if (master) {
            grSlot1_ = 0; // slot 2 untouched
            return programGain(t, false, true);
        }
        return writeGain(t);
    };
    int r = 0;
    if (T == 1 || T == 3) {
        broadcastNotch(T, false);
        dabNotch(T, false);
        if (T != 3) {
            amPortSelect(T); // result ignored
        } else {
            biasT(3, false, false);
        }
        rfPending_ = false;
        r = programBand(T, false);
        if (r != 0) return r;
        if (T == 3) ch_[1] = ch_[0];
        r = gain(1);
        if (r != 0) return r;
        if (T == 3) {
            r = gain(2);
            if (r != 0) return r;
        }
    } else if (T == 2) {
        broadcastNotch(2, false);
        dabNotch(2, false);
        biasT(2, false, false);
        rfPending_ = false;
        r = programBand(2, false);
        if (r != 0) return r;
        r = gain(2);
        if (r != 0) return r;
    }
    if (!master && state_ == kStateStreaming) return 0;

    dev_.fsFreq.reCal = 1;
    r = writeRate();
    dev_.fsFreq.reCal = 0;
    if (r != 0) return r;
    const bool bulk = raw(dev_.mode) == 1;
    if (bulk) {
        stopRx();
        io_.stream(StreamOp::Flush);
    }
    if (io_.stream(StreamOp::Init) != 0) return sdrplay_api_HwError;
    if (startRx() != 0) return sdrplay_api_HwError;
    if (resyncToggles() != 0) return sdrplay_api_HwError;
    if (bulk) {
        io_.stream(StreamOp::Uninit);
        stopRx();
        io_.stream(StreamOp::Flush);
        return 0;
    }
    stopRx();
    io_.stream(StreamOp::Uninit);
    return 0;
}

void Protocol::expanderInit(int T)
{
    // expander init: every result ignored
    dualWrite(0, kExpGpioA, d1a_);
    dualWrite(0, kExpGpioB, d1b_);
    tunerSelect(T);
    dualWrite(0, kExpIodirA, 0x00); // every pin an output
    dualWrite(0, kExpIodirB, 0x00);
    dualWrite(1, kExpGpioA, d2a_);
    dualWrite(1, kExpGpioB, d2b_);
    dualWrite(1, kExpIodirA, 0x00);
    dualWrite(1, kExpIodirB, 0x00);
}

void Protocol::channelConfig(int cfg)
{
    // Channel config (cfg, no register write): ICtrl = cfg in {0, 2}, QCtrl = cfg in {0, 3}
    d1a_ = static_cast<std::uint8_t>((d1a_ & 0x7F) | ((cfg == 0 || cfg == 2) ? 0x80 : 0));
    d1b_ = static_cast<std::uint8_t>((d1b_ & 0xFE) | ((cfg == 3 || cfg == 0) ? 1 : 0));
}

int Protocol::tunerSelect(int T)
{
    // tunerSelect: D1B 7:6 chip select, written only when T changes
    if (tunerSel_ == T) return 0;
    std::uint8_t b = static_cast<std::uint8_t>(d1b_ & 0x3F);
    if (T == 1) b |= 0x40;
    else if (T == 2) b |= 0xC0;
    else if (T == 3) b |= 0x80;
    d1b_ = b;
    if (const int rc = dualWrite(0, kExpGpioB, b); rc != 0) {
        errVerbose(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    tunerSel_ = T;
    return 0;
}

int Protocol::bridgeInit()
{
    // Open the bridge: alt setting, then the first-Init-after-Select sequence or a trim clear
    bridgeOpen_ = true;
    int r = setInterface(fs_.alt);
    if (r == 0) {
        if (!fnAddrDone_) {
            fnAddrDone_ = true;
            std::uint8_t v = 0;
            r = ctl(0xC0, rsp1b::kReqFnAddr, 0, 0, 1, &v);
            if (r == 0 && (v & 3) == 2) r = io_.stream(StreamOp::ResetDevice);
            if (r == 0) r = ctl(0xC0, rsp1b::kReqFnAddr, 0, 0, 1, &v);
            if (r == 0) r = setRegister(kRegBridge5, kBridge5Open);
            if (r == 0) r = tunerWrite(kTunerReg14Clear);
            if (r == 0) r = setRegister(kRegBridge0, kBridge0FirstInit);
            if (r == 0) r = setRegister(kRegBridge2, kBridge2FirstInit);
        } else {
            r = tunerWrite(kTunerReg14Clear);
        }
        if (r == 0) return 0;
    }
    bridgeUninit();
    return r;
}

void Protocol::bridgeUninit()
{
    // bridge uninit: trim readback of the selected tuner
    std::uint32_t v = 0;
    int r = readTunerTrims(v);
    if (r == 0) {
        std::uint32_t r13 = 0, r14 = 0;
        rsp1b::bypassTrim(rsp1b::readbackTrims(v), 0, r13, r14);
        r = tunerWrite(r13);
        if (r == 0) r = tunerWrite(r14);
        if (r == 0) r = tunerWrite(kTunerReg0Uninit);
        if (r == 0) r = tunerWrite(kTunerReg14Uninit);
        if (r == 0) r = setRegister(kRegAdcPll, kAdcPllUninit);
        if (r == 0) r = setRegister(kRegBridge5, kBridge5Closed);
    }
    setInterface(0);
    bridgeOpen_ = false;
}

int Protocol::start(bool toggles)
{
    // start streaming
    if (const int rc = io_.stream(StreamOp::Init); rc != 0) {
        err(OPENRSP_HERE, fmt("stream setup failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    if (const int rc = startRx(); rc != 0) {
        err(OPENRSP_HERE, fmt("receive start failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    if (toggles) {
        const int r = resyncToggles();
        if (r != 0) err(OPENRSP_HERE, fmt("toggle-state update failed %d", r));
    }
    state_ = kStateStreaming;
    return 0;
}

int Protocol::resyncToggles()
{
    ToggleBits t;
    const int r = io_.readFirstBlock(t);
    if (r != 0) {
        errVerbose(OPENRSP_HERE, fmt("stream read failed err=%d", r));
        return r;
    }
    fsToggle_ = t.fs;
    rfToggle_ = t.rf;
    grToggle_ = t.gr;
    fsPending_ = rfPending_ = false;
    grSlot1_ = grSlot2_ = 0;
    io_.disarmed(Change::Gr);
    io_.disarmed(Change::Rf);
    io_.disarmed(Change::Fs);
    return 0;
}

void Protocol::stopStream()
{
    // stop the stream
    if (raw(dev_.mode) != 1) {
        stopRx();
        io_.stream(StreamOp::Uninit);
    } else {
        io_.stream(StreamOp::Uninit);
        stopRx();
        io_.stream(StreamOp::Flush);
    }
    state_ = kStateStopped;
}

// ------------------------------------------------------------------ Uninit / release
int Protocol::uninitImpl(bool apiCall)
{
    if (slaveInit_) {
        stopPending_ = true;
        return sdrplay_api_StopPending; // the slave must uninitialise first
    }
    if (state_ == kStateIdle) return sdrplay_api_NotInitialised;
    if (state_ == kStateStreaming) stopStream();
    dual_ = false;
    if (apiCall) io_.overloadCorrected(tuner_);
    biasT(2, true, true);
    tunerSelect(tuner_);
    if (tuner_ == 1 || tuner_ == 3) {
        d2a_ = static_cast<std::uint8_t>(d2a_ | kDuoPadA);
        dualWrite(1, kExpGpioA, d2a_);
    } else if (tuner_ == 2) {
        d2b_ = static_cast<std::uint8_t>(d2b_ | kDuoPadB);
        dualWrite(1, kExpGpioB, d2b_);
    }
    for (std::uint32_t w : kStandby) tunerWrite(w);
    dualWrite(0, kExpIodirA, 0xDF); // every pin an input except expander 0 GPIOA bit 5
    dualWrite(0, kExpIodirB, 0xFF);
    dualWrite(1, kExpIodirA, 0xFF);
    dualWrite(1, kExpIodirB, 0xFF);
    bridgeUninit();
    state_ = kStateIdle;
    tunerSel_ = 0;
    return 0;
}

void Protocol::release()
{
    if (state_ != kStateIdle) uninitImpl(false);
    if (bridgeOpen_ && !slaveInit_) {
        setInterface(0);
        bridgeOpen_ = false;
    }
}

void Protocol::masterDisappeared()
{
    // watchdog thread, master gone with the slave alive: Uninit, MasterDllDisappeared
    if (state_ != kStateIdle) uninitImpl(false);
    io_.modeChange(sdrplay_api_MasterDllDisappeared, slaveTuner_);
}

void Protocol::slaveAttached() { io_.modeChange(sdrplay_api_SlaveAttached, tuner_); }

void Protocol::slaveDetached(bool dllDisappeared)
{
    io_.modeChange(sdrplay_api_SlaveDetached, tuner_);
    if (dllDisappeared) io_.modeChange(sdrplay_api_SlaveDllDisappeared, tuner_);
}

// ------------------------------------------------------------------ slave
int Protocol::initSlave()
{
    // slave Init
    const int T = slaveTuner_;
    if (state_ != kStateStreaming) {
        slaveStartPending_ = true; // "master channel not initialised - start pending master signal"
        return sdrplay_api_StartPending;
    }
    int r = (T & 3) != 0 ? validateTuner(T) : 0;
    if (r != 0) return r;
    if (T == 1) latchChannel(0, rxShared_[0]);
    else if (T == 2) latchChannel(1, rxShared_[1]);
    initChannelState(T, false);
    r = initHardware(T, false);
    if (r != 0) return r;
    if (state_ == kStateStreaming) {
        // wait (gain mutex released) until no gain update is pending - unbounded
        while (grSlot1_ != 0) io_.sleepMs(25);
    }
    io_.modeChange(sdrplay_api_SlaveInitialised, tuner_);
    slaveInit_ = true;
    return 0;
}

int Protocol::uninitSlave()
{
    tunerSelect(slaveTuner_);
    if (slaveTuner_ == 1) {
        d2a_ = static_cast<std::uint8_t>(d2a_ | kDuoPadA);
        dualWrite(1, kExpGpioA, d2a_);
    } else if (slaveTuner_ == 2) {
        d2b_ = static_cast<std::uint8_t>(d2b_ | kDuoPadB);
        dualWrite(1, kExpGpioB, d2b_);
    }
    for (std::uint32_t w : kStandby) tunerWrite(w);
    tunerSel_ = 0;
    slaveInit_ = false;
    io_.modeChange(sdrplay_api_SlaveUninitialised, tuner_);
    io_.overloadCorrected(slaveTuner_);
    stopPending_ = false;
    return 0;
}

// ------------------------------------------------------------------ tuner helpers
int Protocol::readTunerTrims(std::uint32_t &v)
{
    if (tunerWrite(kTunerReadbackStart) != 0) return 1;
    io_.sleepMs(1);
    if (tunerWrite(kTunerReadbackEnd) != 0) return 1;
    io_.sleepMs(1);
    std::uint8_t b[4] = {0, 0, 0, 0};
    if (ctl(0xC0, rsp1b::kReqReadReg, 0, 0x0010, 4, b) != 0) return 1;
    v = static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8 |
        static_cast<std::uint32_t>(b[2]) << 16 | static_cast<std::uint32_t>(b[3]) << 24;
    return 0;
}

int Protocol::applyTunerDc(int T)
{
    Chan &c = ch_[chIdx(T)];
    const auto &dc = c.p.tunerParams.dcOffsetTuner;
    if (dc.dcCal > 5) {
        err(OPENRSP_HERE, fmt("Tuner%d: dcCal out of range[%d:%d] %d", T, 0, 5, dc.dcCal));
        return sdrplay_api_OutOfRange;
    }
    if (dc.speedUp > 1) {
        err(OPENRSP_HERE, fmt("Tuner%d: speedUp out of range[%d:%d] %d", T, 0, 1, dc.speedUp));
        return sdrplay_api_OutOfRange;
    }
    if (dc.trackTime < 0 || dc.trackTime > 63) {
        err(OPENRSP_HERE, fmt("Tuner%d: trackTime out of range[%d:%d] %d", T, 0, 63, dc.trackTime));
        return sdrplay_api_OutOfRange;
    }
    if (dc.refreshRateTime < 0 || dc.refreshRateTime > 4095) {
        err(OPENRSP_HERE, fmt("Tuner%d: refreshRateTime out of range[%d:%d] %d", T, 0, 4095, dc.refreshRateTime));
        return sdrplay_api_OutOfRange;
    }
    c.dcCalApplied = dc.dcCal;
    c.speedUpApplied = dc.speedUp;
    tunerSelect(T);
    if (const int rc = tunerWrite(tunerDcWord(dc.refreshRateTime, dc.trackTime)); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: register write failed 0x%08X", T, rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::applyLoMode(int T)
{
    Chan &c = ch_[chIdx(T)];
    const int r = rsp1b::loModeFirstLo(raw(c.p.tunerParams.loMode), c.firstLO);
    if (r != 0) err(OPENRSP_HERE, fmt("Tuner%d: invalid mode %d", T, raw(c.p.tunerParams.loMode)));
    return r;
}

int Protocol::selectBand(int T, BandState &out, int &firstLO)
{
    const Chan &c = ch_[chIdx(T)];
    const int r = rsp1b::computeBand(c.p.tunerParams.rfFreq.rfHz, raw(c.p.tunerParams.loMode), firstLO, out);
    if (r != 0) {
        errVerbose(OPENRSP_HERE,
            fmt("Tuner%d: rfMHz out of range %.3f", T, c.p.tunerParams.rfFreq.rfHz / 1e6));
        return r;
    }
    out.lnaRow = lnaRowOfBand(out.band); // hwVer 3 row; no 50-60 MHz override
    firstLO = out.firstLO;
    return 0;
}

void Protocol::configureDownConverter(int T)
{
    Chan &c = ch_[chIdx(T)];
    c.mix = rspduo::mixFreq(dev_.fsFreq.fsHz, raw(c.p.tunerParams.bwType), raw(c.p.tunerParams.ifType), mode_, fs8192_);
    dsp_.downConverterReinit = true;
}

int Protocol::programBand(int T, bool bwChange)
{
    Chan &c = ch_[chIdx(T)];
    const BandState &st = c.band;
    const int bw = raw(c.p.tunerParams.bwType);
    const int ifR0 = r0IfType(dev_.fsFreq.fsHz, raw(c.p.tunerParams.ifType), mode_, fs8192_);
    // HiZ base: tuner 1 and chA's tuner1AmPortSel == 1
    const bool hiz = T == 1 && raw(ch_[0].p.rspDuoTunerParams.tuner1AmPortSel) == 1;
    const std::uint32_t w0 = reg0Word(st, bw, ifR0, bwChange && c.loFlag, hiz);
    tunerSelect(T);
    if (const int rc = tunerWrite(w0); rc != 0) {
        errVerbose(OPENRSP_HERE, fmt("Tuner%d: register 0x09 write failed 0x%08X", T, rc));
        return sdrplay_api_HwError;
    }
    const std::uint8_t code = bandCode(st);
    if (T & 2) d1b_ = static_cast<std::uint8_t>((d1b_ & 0xC1) | static_cast<std::uint8_t>(code << 1));
    if (T & 1) {
        d1a_ = static_cast<std::uint8_t>((d1a_ & 0xE0) | code);
        d2a_ = static_cast<std::uint8_t>((d2a_ & 0xFE) | (st.duoSub != 1 ? 1 : 0));
    }
    dualWrite(0, kExpGpioA, d1a_);
    dualWrite(0, kExpGpioB, d1b_);
    dualWrite(1, kExpGpioA, d2a_);
    dualWrite(1, kExpGpioB, d2b_);

    if (bwChange) return writeRf(T);
    const bool lo = c.loFlag;
    bool report = false;
    if (lo) {
        report = rfReportReq_;
        rfReportReq_ = false;
    }
    int r = writeRf(T);
    if (r != 0) {
        rfReportReq_ = false;
        return r;
    }
    if (!lo) {
        // tuner trim bypass (all 0) to the selected tuner
        tunerWrite(kTunerReg13Clear);
        tunerWrite(kTunerReg14Clear);
        return 0;
    }
    for (int t = 1; t <= 2; ++t) {
        if (!(T & t)) continue;
        c.loFlag = false;
        std::uint32_t w = w0 & 0xFFF1FFFFu;
        if (c.firstLO == 144000000) w |= 0x20000;
        tunerSelect(t); // the trim readback, r0, the RF words and the trims all go to tuner t
        std::uint32_t v = 0;
        readTunerTrims(v); // result ignored
        tunerWrite(w);
        rfReportReq_ = report;
        r = writeRf(t);
        if (r != 0) return r;
        std::uint32_t r13 = 0, r14 = 0;
        rsp1b::bypassTrim(rsp1b::readbackTrims(v), 1, r13, r14);
        tunerWrite(r13);
        tunerWrite(r14);
    }
    tunerSelect(T);
    return 0;
}

// ------------------------------------------------------------------ programRf / programSampleRate / programGain
int Protocol::programRf(int T)
{
    if (rfPending_) {
        errWarning(OPENRSP_HERE, fmt("Tuner%d: previous Rf update not received (or timed out)", T));
        return sdrplay_api_RfUpdateError;
    }
    Chan &c = ch_[chIdx(T)];
    const double rf = c.p.tunerParams.rfFreq.rfHz;
    const bool hiz = T == 1 && c.band.band == 0 && raw(c.p.rspDuoTunerParams.tuner1AmPortSel) == 1;
    if (hiz) {
        if (!rfInHizRange(rf)) {
            err(OPENRSP_HERE, fmt("Tuner%d: rfHz out of range for AM port 1 %.1fHz", 1, rf));
            return sdrplay_api_OutOfRange;
        }
    } else if (!rfInBandRange(rf, c.band.band)) {
        err(OPENRSP_HERE, fmt("Tuner%d: rfHz out of range %.1fHz (abs=1)", T, rf));
        return sdrplay_api_OutOfRange;
    }
    TunerPll p;
    double fsynth = 0.0;
    const int pr = tunerPll(rf, dev_.ppm, c.mix, c.band, fsynth, p);
    c.fSynth = fsynth;
    if (!hiz && T == 3) ch_[1].fSynth = fsynth; // mirrored into the other channel
    if (pr == kPllIntOutOfRange) {
        errWarning(OPENRSP_HERE, fmt("Tuner%d: detected INT out of range - returning without programming tuner", T));
        return sdrplay_api_OutOfRange;
    }
    if (pr == kPllFracOutOfRange) {
        errWarning(OPENRSP_HERE, fmt("Tuner%d: detected FRAC out of range - returning without programming tuner", T));
        return sdrplay_api_OutOfRange;
    }
    const std::uint8_t sync = c.p.tunerParams.rfFreq.syncUpdate;
    tunerSelect(T);
    if (const int rc = tunerWrite(p.r3, sync); rc != 0) {
        errWarning(OPENRSP_HERE, fmt("Tuner%d: AFC register write failed 0x%08X", T, rc));
        return sdrplay_api_HwError;
    }
    if (const int rc = tunerWrite(p.r2, sync); rc != 0) {
        errWarning(OPENRSP_HERE, fmt("Tuner%d: RF register write failed 0x%08X", T, rc));
        return sdrplay_api_HwError;
    }
    rfSlot_ = T;
    rfToggle_ = !rfToggle_;
    rfPending_ = true;
    rfReport_ = rfReportReq_;
    rfReportReq_ = false;
    io_.armed(Change::Rf, rfToggle_, rfReport_); // Single: the device reports from this flag
    if (c.appliedGr != -1) {
        refreshGainVals(T);
        writeGainBack(T);
        GainChangeEvent e;
        e.tuner = T;
        e.gRdB = static_cast<unsigned>(c.appliedGr);
        e.lnaGRdB = static_cast<unsigned>(c.totalGr - c.appliedGr);
        e.currGain = static_cast<double>(c.p.tunerParams.gain.gainVals.curr);
        io_.gainChange(e);
    }
    return 0;
}

int Protocol::writeRf(int T)
{
    for (int i = 0; i < 16; ++i) {
        if (!rfPending_) {
            programRf(T);
            return 0;
        }
        io_.sleepMs(25);
    }
    if (mode_ == sdrplay_api_RspDuoMode_Master && T != tuner_) {
        if (T == slaveTuner_) {
            ch_[chIdx(T)].p.rspDuoTunerParams.resetSlaveFlags.resetRfUpdate = 1;
            dropSlavePendingUpdates(T);
        }
    } else {
        dev_.resetFlags.resetRfUpdate = 1;
        dropPendingUpdates(T);
    }
    return programRf(T);
}

int Protocol::programSampleRate()
{
    if (fsPending_) {
        errWarning(OPENRSP_HERE, "Previous Fs update not recieved (or timed out)");
        return sdrplay_api_FsUpdateError;
    }
    const double fsHz = dev_.fsFreq.fsHz;
    if (!(2000000.0 <= fsHz && fsHz <= 10000000.0)) {
        err(OPENRSP_HERE, fmt("fsHz out of range %.6fHz ", fsHz));
        return sdrplay_api_OutOfRange;
    }
    rsp1b::AdcPll p;
    if (rsp1b::adcPll(fsHz, dev_.ppm, fs_.D, fs_.V, dev_.fsFreq.reCal, p) != 0) {
        errWarning(OPENRSP_HERE, "requested invalid IntVal");
        return sdrplay_api_OutOfRange;
    }
    const std::uint8_t sync = static_cast<std::uint8_t>((dev_.fsFreq.syncUpdate & 7) << 5);
    if (const int rc = setRegister(static_cast<std::uint8_t>(sync | kRegAdcFrac), p.reg4); rc != 0) {
        err(OPENRSP_HERE, fmt("register write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    if (const int rc = setRegister(static_cast<std::uint8_t>(sync | kRegAdcPll), p.reg3); rc != 0) {
        err(OPENRSP_HERE, fmt("register write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    fsPending_ = true;
    fsReport_ = fsReportReq_;
    fsReportReq_ = false; // the report request is consumed by this programSampleRate
    fsToggle_ = !fsToggle_;
    io_.armed(Change::Fs, fsToggle_, fsReport_); // Single: the device reports from this flag
    return 0;
}

int Protocol::writeRate()
{
    for (int i = 0; i < 16; ++i) {
        if (!fsPending_) {
            programSampleRate();
            return 0;
        }
        io_.sleepMs(25);
    }
    dev_.resetFlags.resetFsUpdate = 1;
    dropPendingUpdates(0);
    return programSampleRate();
}

int Protocol::refreshGainVals(int T)
{
    Chan &c = ch_[chIdx(T)];
    const auto &tp = c.p.tunerParams;
    const bool hiz = hizGainVals(c.band, T, raw(c.p.rspDuoTunerParams.tuner1AmPortSel));
    const bool dab = ((T & 1) && (d2a_ & kDuoDabNotchAOff) == 0) || ((T & 2) && (d2b_ & kDuoDabNotchBOff) == 0);
    GainVals gv;
    const int r = gainValues(rsp1b::gainFreqMHz(c.fSynth, c.mix, c.band.lif1Hz), T, hiz, raw(tp.bwType), raw(tp.ifType),
                             downConv(chIdx(T)).enabled, raw(tp.gain.minGr), c.appliedGr, tp.gain.LNAstate, dab, gv);
    if (r != 0) return r;
    c.p.tunerParams.gain.gainVals.curr = gv.curr;
    c.p.tunerParams.gain.gainVals.max = gv.max;
    c.p.tunerParams.gain.gainVals.min = gv.min;
    return 0;
}

void Protocol::writeGainBack(int T)
{
    sdrplay_api_RxChannelParamsT *s = shared(T);
    if (s) copyGainStruct(*s, ch_[chIdx(T)].p);
}

int Protocol::programGain(int T, bool fromAgc, bool notify)
{
    Chan &c = ch_[chIdx(T)];
    auto &g = c.p.tunerParams.gain;
    const int gr = g.gRdB;
    const int lna = g.LNAstate;
    const int minGr = raw(g.minGr);
    const int am = raw(c.p.rspDuoTunerParams.tuner1AmPortSel);
    if (!fromAgc && !grValid(gr, minGr, lna, c.band.band, T, am)) {
        err(OPENRSP_HERE, fmt("Tuner%d: parameter out of range (absolute gRdB=%d LNAstate=%d)", T, gr, lna));
        // ... followed by a LOG-only line with the limits used (LOG level Error, no record; e.g.
        // "minGr=20 maxGr=59 band=0 AmPortSel=0 numStates=10 numStatesAMPort=5
        // numStatesAM=7 numStatesLBand=9")
        ErrorRecord d;
        d.file = __FILE__;
        d.function = __func__;
        d.line = __LINE__;
        d.logOnly = true;
        d.message = fmt("Tuner%d: minGr=%d maxGr=%d band=%d AmPortSel=%d numStates=%d numStatesAMPort=%d numStatesAM=%d "
                        "numStatesLBand=%d",
                        T, minGr, 59, c.band.band, am, 10, 5, 7, 9);
        io_.error(d);
        return sdrplay_api_OutOfRange;
    }
    if (!gainSlotFree()) {
        errWarning(OPENRSP_HERE, fmt("Tuner%d: previous gain update not received (or timed out)", T));
        return sdrplay_api_GainUpdateError;
    }
    const int gApplied = gr < minGr ? minGr : (gr > 59 ? 59 : gr);
    const int lnaC = lna > 9 ? 9 : lna;
    g.gRdB = gApplied;
    if (raw(c.p.ctrlParams.agc.enable) == 0) g.LNAstate = static_cast<unsigned char>(lnaC);
    const GainWord w = gainWord(c.band, T, am, gApplied, lnaC);
    c.appliedGr = gApplied;
    tunerSelect(T);
    const std::uint32_t r1 = static_cast<std::uint32_t>(c.dcCalApplied) << 14 |
                             static_cast<std::uint32_t>(c.speedUpApplied) << 17 | w.g << 4 | 1u;
    if (const int rc = tunerWrite(r1, g.syncUpdate); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: register write failed 0x%08X", T, rc));
        return sdrplay_api_HwError;
    }
    const int att = w.attDb;
    const std::uint8_t step = attStep(att);
    // Pad: always on in band 0; otherwise follows the table attDb, except in Dual mode (off).
    const bool pad = c.band.band == 0 || (!dual_ && attPad(att));
    if (T & 1) {
        d2b_ = withField(d2b_, kDuoAttStepAMask, static_cast<std::uint8_t>(step << 2));
        d2a_ = withBits(d2a_, kDuoPadA, pad);
        if (const int rc = dualWrite(1, kExpGpioA, d2a_); rc != 0) {
            err(OPENRSP_HERE, fmt("Tuner%d: GPIO write failed 0x%08X", T, rc));
            return sdrplay_api_HwError;
        }
        // Band codes from 60 MHz up (any of code bits 4:2 set): code bit 1 is set when the
        // table attenuation is above 18 dB.
        if (d1a_ & kDuoBandAHighBits) d1a_ = withBits(d1a_, kDuoBandAAttPath, att > 18);
        if (const int rc = dualWrite(0, kExpGpioA, d1a_); rc != 0) {
            err(OPENRSP_HERE, fmt("Tuner%d: dual GPIO write failed 0x%08X", T, rc));
            return sdrplay_api_HwError;
        }
        if (const int rc = dualWrite(1, kExpGpioB, d2b_); rc != 0) {
            err(OPENRSP_HERE, fmt("Tuner%d: dual GPIO write failed 0x%08X", T, rc));
            return sdrplay_api_HwError;
        }
    }
    if (T & 2) {
        d2b_ = withBits(d2b_, kDuoPadB, pad);
        if (const int rc = dualWrite(1, kExpGpioB, d2b_); rc != 0) {
            err(OPENRSP_HERE, fmt("Tuner%d: dual GPIO write failed 0x%08X", T, rc));
            return sdrplay_api_HwError;
        }
        if (d1b_ & kDuoBandBHighBits) d1b_ = withBits(d1b_, kDuoBandBAttPath, att > 18); // as for tuner 1
        if (const int rc = dualWrite(0, kExpGpioB, d1b_); rc != 0) {
            err(OPENRSP_HERE, fmt("Tuner%d: dual GPIO write failed 0x%08X", T, rc));
            return sdrplay_api_HwError;
        }
        d2b_ = withField(d2b_, kDuoAttStepBMask, static_cast<std::uint8_t>(step << 5));
        if (const int rc = dualWrite(1, kExpGpioB, d2b_); rc != 0) {
            err(OPENRSP_HERE, fmt("Tuner%d: dual GPIO write failed 0x%08X", T, rc));
            return sdrplay_api_HwError;
        }
    }
    c.totalGr = w.total;
    grValue_ = gApplied;
    grToggle_ = !grToggle_;
    const bool userGr = grUserFlag_;
    if (grSlot1_ == 0) {
        grSlot1_ = T;
        if (grUserFlag_) {
            grRep1_ = T;
            grUserFlag_ = false;
            grNote1_ = notify;
        }
    } else {
        grSlot2_ = T;
        if (grUserFlag_) {
            grRep2_ = T;
            grUserFlag_ = false;
            grNote2_ = notify;
        }
    }
    io_.armed(Change::Gr, grToggle_, userGr);
    refreshGainVals(T); // on failure the gainVals keep their last values
    sdrplay_api_RxChannelParamsT *s = shared(T);
    if (s) {
        auto &sg = s->tunerParams.gain;
        sg.gRdB = g.gRdB;
        if (raw(c.p.ctrlParams.agc.enable) == 0) sg.LNAstate = g.LNAstate;
        sg.minGr = g.minGr;
        sg.syncUpdate = g.syncUpdate;
        sg.gainVals = g.gainVals;
    }
    GainChangeEvent ev;
    ev.tuner = T;
    ev.gRdB = static_cast<unsigned>(gApplied);
    ev.lnaGRdB = static_cast<unsigned>(c.totalGr - gApplied);
    ev.currGain = static_cast<double>(g.gainVals.curr);
    io_.gainChange(ev);
    return 0;
}

int Protocol::writeGain(int T)
{
    // Waits (16 x 25 ms polls) only while no gain slot is free
    for (int i = 0; i < 16; ++i) {
        if (gainSlotFree()) return programGain(T, false, true);
        io_.sleepMs(25);
    }
    if (mode_ == sdrplay_api_RspDuoMode_Master && T != tuner_) {
        if (T == slaveTuner_) {
            ch_[chIdx(T)].p.rspDuoTunerParams.resetSlaveFlags.resetGainUpdate = 1;
            dropSlavePendingUpdates(T);
        }
    } else {
        dev_.resetFlags.resetGainUpdate = 1;
        dropPendingUpdates(T);
    }
    return programGain(T, false, true);
}

int Protocol::applyAgcGain(int T, int gRdB)
{
    if (state_ == kStateIdle) return sdrplay_api_NotInitialised;
    Chan &c = ch_[chIdx(T)];
    if (const sdrplay_api_RxChannelParamsT *s = shared(T)) {
        c.p.tunerParams.gain.minGr = s->tunerParams.gain.minGr;
        c.p.tunerParams.gain.LNAstate = s->tunerParams.gain.LNAstate;
        c.p.ctrlParams.agc.enable = s->ctrlParams.agc.enable;
    }
    const int prevGr = c.p.tunerParams.gain.gRdB;
    c.p.tunerParams.gain.gRdB = gRdB;
    const int r = programGain(T, true, true);
    // gain update still pending: nothing was applied or written
    if (r == sdrplay_api_GainUpdateError) c.p.tunerParams.gain.gRdB = prevGr;
    return r;
}

int Protocol::dropPendingUpdates(int T)
{
    // Drop each pending change whose reset flag is set (RSPduo master rules)
    auto grReset = [&] {
        grSlot1_ = grSlot2_ = 0;
        grRep1_ = grRep2_ = 0;
        grNote1_ = grNote2_ = false;
        grToggle_ = !grToggle_;
        io_.disarmed(Change::Gr);
    };
    auto rfReset = [&] {
        dev_.resetFlags.resetRfUpdate = 0;
        rfPending_ = false;
        rfReport_ = false;
        rfToggle_ = !rfToggle_;
        io_.disarmed(Change::Rf);
    };
    bool doRf = false;
    if (dev_.resetFlags.resetGainUpdate) {
        dev_.resetFlags.resetGainUpdate = 0;
        if (mode_ != sdrplay_api_RspDuoMode_Master) {
            grReset();
            doRf = dev_.resetFlags.resetRfUpdate != 0;
        } else if (tuner_ == T) {
            if (grSlot1_ == T) {
                grReset();
                doRf = dev_.resetFlags.resetRfUpdate != 0 && rfSlot_ == T;
            } else if (dev_.resetFlags.resetRfUpdate) {
                doRf = rfSlot_ == T;
            }
        }
    } else if (dev_.resetFlags.resetRfUpdate) {
        if (mode_ != sdrplay_api_RspDuoMode_Master) doRf = true;
        else doRf = T == tuner_ && rfSlot_ == T;
    }
    if (doRf) rfReset();
    if (dev_.resetFlags.resetFsUpdate) {
        dev_.resetFlags.resetFsUpdate = 0;
        fsPending_ = false;
        fsReport_ = false;
        fsToggle_ = !fsToggle_;
        io_.disarmed(Change::Fs);
    }
    return 0;
}

int Protocol::dropSlavePendingUpdates(int T)
{
    if (slaveTuner_ != T) return sdrplay_api_InvalidMode;
    auto &f = ch_[chIdx(T)].p.rspDuoTunerParams.resetSlaveFlags;
    if (f.resetGainUpdate) {
        f.resetGainUpdate = 0;
        if (grSlot1_ == T) {
            grSlot1_ = grSlot2_ = 0;
            grRep1_ = grRep2_ = 0;
            grNote1_ = grNote2_ = false;
            grToggle_ = !grToggle_;
            io_.disarmed(Change::Gr);
        }
    }
    if (f.resetRfUpdate) {
        f.resetRfUpdate = 0;
        if (rfSlot_ == T) {
            rfPending_ = false;
            rfReport_ = false;
            rfToggle_ = !rfToggle_;
            io_.disarmed(Change::Rf);
        }
    }
    return 0;
}

ToggleResult Protocol::onPacketToggles(const ToggleBits &t)
{
    // Packet fetch: one gain/RF/Fs toggle set shared by both tuners
    ToggleResult res;
    if (grSlot1_ != 0 && t.gr == grToggle_) {
        res.grConfirmedMask = grSlot1_ & 3;
        res.grReportMask = grRep1_ & grSlot1_ & 3;
        res.grNotify = grNote1_;
        res.grValue = grValue_;
        if (mode_ == sdrplay_api_RspDuoMode_Dual_Tuner) {
            grSlot1_ = grSlot2_;
            grSlot2_ = 0;
            grRep1_ = grRep2_;
            grRep2_ = 0;
            grNote1_ = grNote2_;
            grNote2_ = false;
        } else {
            grSlot1_ = grSlot2_ = 0;
            grRep1_ = grRep2_ = 0;
            grNote1_ = grNote2_ = false;
        }
    }
    if (rfPending_ && t.rf == rfToggle_) {
        res.rfConfirmedMask = rfSlot_ & 3;
        res.rfReport = rfReport_;
        rfPending_ = false;
        rfReport_ = false;
    }
    if (fsPending_ && t.fs == fsToggle_) {
        res.fsConfirmed = true;
        res.fsReport = fsReport_;
        fsPending_ = false;
        fsReport_ = false;
    }
    return res;
}

void Protocol::confirmAllPending()
{
    grSlot1_ = grSlot2_ = 0;
    grRep1_ = grRep2_ = 0;
    grNote1_ = grNote2_ = false;
    rfPending_ = fsPending_ = false;
}

int Protocol::programSyncUpdate()
{
    // Sample-number synchronised updates for the master tuner's channel (tuner 2 -> chB)
    const int idx = tuner_ == 2 ? 1 : 0;
    std::uint32_t period = dev_.syncUpdate.period;
    std::uint32_t sample = dev_.syncUpdate.sampleNum;
    const DownConv dc = downConv(idx);
    if (dc.enabled) {
        period = (period << dc.countShift) * static_cast<std::uint32_t>(dc.countDivisor);
        sample = (sample << dc.countShift) * static_cast<std::uint32_t>(dc.countDivisor);
    }
    if (ch_[idx].p.ctrlParams.decimation.enable) {
        period <<= ch_[idx].decimShift;
        sample <<= ch_[idx].decimShift;
    }
    if (syncPeriod_ != period) {
        syncPeriod_ = period;
        if (const int rc = setRegister(kRegSyncPeriod, period | 0x800000u); rc != 0) {
            err(OPENRSP_HERE, fmt("Tuner%d: register write failed 0x%08X", tuner_, rc));
            return sdrplay_api_HwError;
        }
    }
    if (fs_.wrapThr < sample) {
        // beyond the wrap threshold: the sample number is not written and the stream thread
        // is told a sync update is deferred; a zero period is remembered as 1024
        if (syncPeriod_ == 0) syncPeriod_ = 1024;
        io_.syncDeferred();
        return 0;
    }
    if (setRegister(kRegSyncSampleLo, sample & 0xFFFF) != 0) return sdrplay_api_HwError;
    if (setRegister(kRegSyncSampleHi, sample >> 16) != 0) return sdrplay_api_HwError;
    return 0;
}

int Protocol::applyPpm()
{
    int r;
    if (mode_ == sdrplay_api_RspDuoMode_Single_Tuner) {
        r = writeRf(tuner_);
    } else if (mode_ == sdrplay_api_RspDuoMode_Dual_Tuner) {
        r = writeRf(3);
    } else {
        r = writeRf(tuner_);
        if (r == 0 && slaveInit_) r = writeRf(slaveTuner_);
    }
    if (r != 0) return r;
    dev_.fsFreq.reCal = 1;
    r = writeRate();
    dev_.fsFreq.reCal = 0;
    return r;
}

int Protocol::configureAgc(int T)
{
    // every channel whose bit is set in T
    for (int i = 0; i < 2; ++i) {
        if (!(T & (1 << i))) continue;
        Chan &c = ch_[i];
        int lim = -60;
        if (fs_.spp != 384) {
            lim = -72;
            if (fs_.spp == 504) {
                if (mode_ != sdrplay_api_RspDuoMode_Single_Tuner) lim = -48;
                else lim = raw(c.p.tunerParams.ifType) == 0 ? -48 : -72;
            }
        }
        const int sp = c.p.ctrlParams.agc.setPoint_dBfs;
        if (sp > 0 || sp < lim) {
            err(OPENRSP_HERE,
                fmt("Tuner%d: setPoint_dBfs out of range for current sampling freq[%d:0], %d", 1 << i, lim, sp));
            return sdrplay_api_OutOfRange;
        }
        const int en = raw(c.p.ctrlParams.agc.enable);
        dsp_.agcForced = false;
        if (c.agcEnableState == 0 && en != 0) dsp_.agcForced = true;
        c.agcEnableState = en;
        dsp_.agcChanged = true;
    }
    return 0;
}

void Protocol::updateDecimation(int T)
{
    for (int i = 0; i < 2; ++i) {
        if (!(T & (1 << i))) continue;
        const auto &d = ch_[i].p.ctrlParams.decimation;
        int s = 0;
        if (d.enable) {
            switch (d.decimationFactor) {
            case 2: s = 1; break;
            case 4: s = 2; break;
            case 8: s = 3; break;
            case 16: s = 4; break;
            case 32: s = 5; break;
            default: s = 0; break;
            }
        }
        ch_[i].decimShift = s;
    }
    dsp_.decimationChanged = true;
}

void Protocol::configureDcIq(int T)
{
    for (int i = 0; i < 2; ++i) {
        if (!(T & (1 << i))) continue;
        auto &dc = ch_[i].p.ctrlParams.dcOffset;
        if (dc.IQenable) dc.DCenable = dc.IQenable;
    }
    dsp_.dcIqReset = true;
}

// ------------------------------------------------------------------ RSPduo front end
int Protocol::extRef(bool write)
{
    d1a_ = withBits(d1a_, kDuoExtRefOff, raw(dev_.rspDuoParams.extRefOutputEn) != 1);
    if (!write) return 0;
    if (const int rc = dualWrite(0, kExpGpioA, d1a_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rfTest(bool write)
{
    d2a_ = withBits(d2a_, kDuoRfTestOff, internal_.rspDuoRfTestEn != 1);
    if (!write) return 0;
    if (const int rc = dualWrite(1, kExpGpioA, d2a_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::biasT(int T, bool write, bool forceOff)
{
    const Chan &c = ch_[chIdx(T)];
    d2a_ = withBits(d2a_, kDuoBiasTOff, c.p.rspDuoTunerParams.biasTEnable != 1 || forceOff);
    if (!write) return 0;
    if (const int rc = dualWrite(1, kExpGpioA, d2a_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::tuner1AmNotch(int T, bool write)
{
    if (T == 1) {
        d2a_ = withBits(d2a_, kDuoTuner1AmNotchOff, ch_[0].p.rspDuoTunerParams.tuner1AmNotchEnable != 1);
        if (!write) return 0;
        if (const int rc = dualWrite(1, kExpGpioA, d2a_); rc != 0) {
            err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
        return 0;
    }
    err(OPENRSP_HERE, fmt("wrong tuner %d", T));
    return sdrplay_api_OutOfRange;
}

int Protocol::broadcastNotch(int T, bool write)
{
    const Chan &cx = ch_[chIdx(T)];
    if (T & 1) d2a_ = withBits(d2a_, kDuoBroadcastNotchAOff, ch_[0].p.rspDuoTunerParams.rfNotchEnable != 1);
    if (T & 2) d2a_ = withBits(d2a_, kDuoBroadcastNotchBOff, cx.p.rspDuoTunerParams.rfNotchEnable != 1);
    if (!write) return 0;
    if (const int rc = dualWrite(1, kExpGpioA, d2a_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::dabNotch(int T, bool write)
{
    const Chan &cx = ch_[chIdx(T)];
    if (T & 1) d2a_ = withBits(d2a_, kDuoDabNotchAOff, ch_[0].p.rspDuoTunerParams.rfDabNotchEnable != 1);
    if (T & 2) d2b_ = withBits(d2b_, kDuoDabNotchBOff, cx.p.rspDuoTunerParams.rfDabNotchEnable != 1);
    if (!write) return 0;
    if (const int rc = dualWrite(1, kExpGpioA, d2a_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    if (const int rc = dualWrite(1, kExpGpioB, d2b_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::amPortSelect(int T)
{
    const int sel = raw(ch_[0].p.rspDuoTunerParams.tuner1AmPortSel);
    if (T != 2 && (sel == sdrplay_api_RspDuo_AMPORT_2 || sel == sdrplay_api_RspDuo_AMPORT_1)) return 0;
    err(OPENRSP_HERE, fmt("port out of range[0:1] %d or wrong tuner %d", sel, T));
    return sdrplay_api_OutOfRange;
}

// ------------------------------------------------------------------ other models' handlers (kReqGpioExt)
int Protocol::rsp1aBiasT(bool write, bool forceOff)
{
    // RSP1A-style bias-T control from chA's rsp1aTunerParams.biasTEnable
    gB_ = withBits(gB_, kRsp1aBiasTOff, ch_[0].p.rsp1aTunerParams.biasTEnable != 1 || forceOff);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, gB_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp1aDabNotch(bool write)
{
    gB_ = withBits(gB_, kRsp1aDabNotchOff, dev_.rsp1aParams.rfDabNotchEnable != 1);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, gB_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp1aBroadcastNotch(bool write)
{
    gB_ = withBits(gB_, kRsp1aBroadcastNotchOff, dev_.rsp1aParams.rfNotchEnable != 1);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, gB_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2BiasT(bool write)
{
    r2b_ = withBits(r2b_, kRsp2BiasTOff, ch_[0].p.rsp2TunerParams.biasTEnable != 1);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, r2b_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2Antenna(bool write)
{
    int sel = raw(ch_[0].p.rsp2TunerParams.antennaSel);
    if (sel != sdrplay_api_Rsp2_ANTENNA_A && sel != sdrplay_api_Rsp2_ANTENNA_B) {
        ch_[0].p.rsp2TunerParams.antennaSel = static_cast<sdrplay_api_Rsp2_AntennaSelectT>(kRsp2AntennaNone);
        sel = kRsp2AntennaNone;
    }
    r2a_ = withField(r2a_, kRsp2AntennaMask, static_cast<std::uint8_t>(sel));
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, r2a_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2RfNotch(bool write)
{
    r2b_ = withField(r2b_, kRsp2NotchMask,
                     ch_[0].p.rsp2TunerParams.rfNotchEnable == 0 ? kRsp2NotchOffBits : kRsp2NotchOnBits);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, r2b_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2ExtRef(bool write)
{
    const unsigned en = dev_.rsp2Params.extRefOutputEn;
    if (en >= 2) {
        err(OPENRSP_HERE, fmt("extRefOutputEn out of range[0:1] %d", en));
        return sdrplay_api_OutOfRange;
    }
    r2a_ = withBits(r2a_, kRsp2ExtRefOff, en == 0);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, r2a_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2AmPort()
{
    const int port = raw(ch_[0].p.rsp2TunerParams.amPortSel);
    if (port == sdrplay_api_Rsp2_AMPORT_2 || port == sdrplay_api_Rsp2_AMPORT_1) return 0;
    err(OPENRSP_HERE, fmt("port out of range[0:1] %d", port));
    return sdrplay_api_OutOfRange;
}

int Protocol::rspDxBiasT(bool write)
{
    dx_ = withBits(dx_, kDxBiasTOff, dev_.rspDxParams.biasTEnable != 1);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, dx_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDxAntenna(bool write)
{
    const int sel = raw(dev_.rspDxParams.antennaSel);
    const std::uint8_t ant = sel == sdrplay_api_RspDx_ANTENNA_B   ? kDxAntennaB
                             : sel == sdrplay_api_RspDx_ANTENNA_C ? 0
                                                                  : kDxAntennaA;
    dx_ = withField(dx_, kDxAntennaMask, ant);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, dx_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDxBroadcastNotch(bool write)
{
    dx_ = withBits(dx_, kDxBroadcastNotchOff, dev_.rspDxParams.rfNotchEnable != 1);
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, dx_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDxDabNotch()
{
    dx_ = withBits(dx_, kDxDabNotchOff, dev_.rspDxParams.rfDabNotchEnable != 1);
    if (const int rc = gpioWrite(kExpGpioA, dx_); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

// ------------------------------------------------------------------ InternalUpdate
int Protocol::internalUpdate(std::uint32_t reason)
{
    if (reason == 0) return 0;
    if (internalShared_) internal_ = *internalShared_;
    if (reason & sdrplay_api_InternalUpdate_Rsp1aRfTest) {
        // rsp1aRfTest on an RSPduo: the RSP1A-style port B through kReqGpioExt
        gB_ = withBits(gB_, kRsp1aRfTestOff, internal_.rsp1aRfTestEn != 1);
        gB_ = withBits(gB_, kRsp1aRfTestSrcStability, internal_.rsp1aRfTestSrcStability != 0);
        if (const int rc = gpioWrite(kExpGpioB, gB_); rc != 0) {
            err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
        return 0;
    }
    if (reason & sdrplay_api_InternalUpdate_RspDuoRfTest) return rfTest(true);
    return 0;
}

// ------------------------------------------------------------------ Update
int Protocol::update(int T, std::uint32_t reason, std::uint32_t ext1, bool fromMaster)
{
    if (state_ == kStateIdle) return sdrplay_api_NotInitialised; // "Wrong state: Uninitialised"
    updStep_ = nullptr;
    const int idx = chIdx(T);
    if (T == 3 && rxShared_[0] && rxShared_[1]) *rxShared_[1] = *rxShared_[0];
    latchChannel(idx, shared(T));
    if (reason == 0 && ext1 == 0) return 0;
    Chan &c = ch_[idx];
    Chan &cb = ch_[1];
    int r = 0;
    // Tuner_Gr requested, or implied by one of the steps below
    bool gainNeeded = (reason & sdrplay_api_Update_Tuner_Gr) != 0;

    if (fromMaster) {
        latchDev();
        if (reason & sdrplay_api_Update_Dev_Fs) {
            if (mode_ == sdrplay_api_RspDuoMode_Master && !fs8192_) {
                err(OPENRSP_HERE, fmt("Tuner%d: can't update Fs in RspDuo Dual Mode", T));
                return sdrplay_api_InvalidParam;
            }
            FsState st;
            r = rspduo::fsState(dev_.fsFreq.fsHz, raw(c.p.tunerParams.ifType), mode_, raw(dev_.mode) == 1, st);
            if (r != 0) {
                err(OPENRSP_HERE, fmt("fsMHz out of range %.3f", dev_.fsFreq.fsHz / 1e6));
                return step("sample-rate state update", r);
            }
            const double oldMix = c.mix;
            if (st.spp == fs_.spp && st.D == fs_.D && st.V == fs_.V) {
                fs_.wrapThr = st.wrapThr;
                fs_.fsMHz = st.fsMHz;
                configureDownConverter(T);
                fsReportReq_ = true;
                r = step("sample rate", writeRate());
                if (r != 0) {
                    fsReportReq_ = false;
                    return r;
                }
            } else {
                stopStream();
                setRegister(kRegAdcPll, kAdcPllFsChange);
                fs_ = st;
                setInterface(fs_.alt);
                configureDownConverter(T);
                fsReportReq_ = true;
                if (const int rc = setRegister(kRegPacketFormat, fs_.reg7); rc != 0) {
                    errVerbose(OPENRSP_HERE, fmt("register 0x07 write failed 0x%08X", rc));
                    fsReportReq_ = false;
                    return step("sample-rate hardware update", sdrplay_api_HwError);
                }
                r = step("sample rate", writeRate());
                if (r != 0) {
                    fsReportReq_ = false;
                    return r;
                }
                gainNeeded = true;
                dsp_.streamRestarted = true;
                start(true);
            }
            fsReportReq_ = false;
            // a new mix frequency reprograms the tuner IF unless another step reprograms the RF anyway
            const std::uint32_t rfProgrammed = sdrplay_api_Update_Rsp2_AmPortSelect | sdrplay_api_Update_Tuner_LoMode |
                                               sdrplay_api_Update_RspDuo_AmPortSelect;
            if (c.mix != oldMix && (reason & rfProgrammed) == 0) reason |= sdrplay_api_Update_Tuner_IfType;
        }
        if (reason & sdrplay_api_Update_Dev_Ppm) {
            r = step("frequency correction", applyPpm());
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_Dev_SyncUpdate) {
            r = step("synchronous update", programSyncUpdate());
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_Dev_ResetFlags) {
            dropPendingUpdates(T);
            if (devShared_) {
                devShared_->resetFlags.resetGainUpdate = 0;
                devShared_->resetFlags.resetRfUpdate = 0;
                devShared_->resetFlags.resetFsUpdate = 0;
            }
        }
        if (reason & sdrplay_api_Update_Rsp1a_BiasTControl) {
            r = step("bias-T control", rsp1aBiasT(true, false));
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_Rsp1a_RfNotchControl) {
            r = step("broadcast notch", rsp1aBroadcastNotch(true));
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_Rsp1a_RfDabNotchControl) {
            r = step("DAB notch", rsp1aDabNotch(true));
            if (r != 0) return r;
            gainNeeded = true;
        }
        if (reason & sdrplay_api_Update_Rsp2_BiasTControl) {
            r = step("bias-T control", rsp2BiasT(true));
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_Rsp2_AmPortSelect) {
            r = step("AM port select", rsp2AmPort());
            if (r != 0) return r;
            gainNeeded = true;
            r = step("RF frequency hardware update", programBand(T, false));
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_Rsp2_AntennaControl) {
            r = step("antenna control", rsp2Antenna(true));
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_Rsp2_RfNotchControl) {
            r = step("RF notch", rsp2RfNotch(true));
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_Rsp2_ExtRefControl) {
            r = step("external reference control", rsp2ExtRef(true));
            if (r != 0) return r;
        }
        if (reason & sdrplay_api_Update_RspDuo_ExtRefControl) {
            r = step("external reference control", extRef(true));
            if (r != 0) return r;
        }
        if (ext1 & sdrplay_api_Update_RspDx_HdrEnable) reason |= sdrplay_api_Update_Tuner_Frf;
        // RspDx bits: the result of the last handler called is returned
        int dxr = 0;
        if (ext1 & sdrplay_api_Update_RspDx_BiasTControl) dxr = rspDxBiasT(true);
        if (ext1 & sdrplay_api_Update_RspDx_AntennaControl) {
            dxr = rspDxAntenna(true);
            gainNeeded = true;
        }
        if (ext1 & sdrplay_api_Update_RspDx_RfNotchControl) dxr = rspDxBroadcastNotch(true);
        if (ext1 & sdrplay_api_Update_RspDx_RfDabNotchControl) {
            dxr = rspDxDabNotch();
            gainNeeded = true;
        }
        if (ext1 & sdrplay_api_Update_RspDx_HdrBw) {
            gainNeeded = true; // RSPdx HDR bandwidth: no traffic on an RSPduo
        } else if (dxr != 0) {
            updStep_ = (ext1 & sdrplay_api_Update_RspDx_RfDabNotchControl) ? "DAB notch"
                     : (ext1 & sdrplay_api_Update_RspDx_RfNotchControl)    ? "broadcast notch"
                     : (ext1 & sdrplay_api_Update_RspDx_AntennaControl)    ? "antenna control"
                                                                            : "bias-T control";
            return dxr;
        }
    }

    if (reason & sdrplay_api_Update_Tuner_GrLimits) {
        const int minGr = raw(c.p.tunerParams.gain.minGr);
        if (raw(c.p.ctrlParams.agc.enable) != 0 && minGr == 0) {
            errWarning(OPENRSP_HERE, fmt("Tuner%d: called while AGC enabled", T));
            return step("gain limits", sdrplay_api_NotEnabled);
        }
        if (minGr != 0 && minGr != 20) {
            err(OPENRSP_HERE, fmt("Tuner%d: parameter out of range[%d or %d] (minGr=%d)", T, 0, 20, minGr));
            return step("gain limits", sdrplay_api_OutOfRange);
        }
    }
    if (reason & sdrplay_api_Update_Tuner_Frf) {
        configureDownConverter(T);
        BandState nb;
        int flo = c.firstLO;
        r = step("RF frequency state update", selectBand(T, nb, flo));
        if (r != 0) return r;
        if (rsp1b::sameBand(nb, c.band) && flo == c.firstLO) {
            rfReportReq_ = true;
            r = step("RF programming", writeRf(T));
            if (r != 0) {
                rfReportReq_ = false;
                return r;
            }
        } else {
            c.firstLO = flo;
            if (nb.loFlag) c.loFlag = true;
            c.band = nb;
            gainNeeded = true;
            rfReportReq_ = true;
            r = step("RF frequency hardware update", programBand(T, false));
            if (r != 0) {
                rfReportReq_ = false;
                return r;
            }
        }
        if (T == 3) cb.p.tunerParams.rfFreq = ch_[0].p.tunerParams.rfFreq;
    }
    auto newBand = [&]() {
        configureDownConverter(T);
        BandState nb;
        int flo = c.firstLO;
        const int rr = selectBand(T, nb, flo);
        if (rr != 0) return rr;
        c.band = nb;
        c.firstLO = flo;
        if (nb.loFlag) c.loFlag = true;
        return 0;
    };
    if (reason & sdrplay_api_Update_Tuner_BwType) {
        r = step("RF frequency state update", newBand());
        if (r != 0) return r;
        r = step("RF frequency hardware update", programBand(T, true));
        if (r != 0) return r;
        gainNeeded = true;
        if (T == 3) cb.p.tunerParams.bwType = ch_[0].p.tunerParams.bwType;
    }
    if (reason & sdrplay_api_Update_Tuner_IfType) {
        r = step("RF frequency state update", newBand());
        if (r != 0) return r;
        r = step("RF frequency hardware update", programBand(T, false));
        if (r != 0) return r;
        gainNeeded = true;
        if (T == 3) cb.p.tunerParams.ifType = ch_[0].p.tunerParams.ifType;
    }
    if (reason & sdrplay_api_Update_Tuner_DcOffset) {
        r = step("tuner DC offset", applyTunerDc(T));
        if (r != 0) return r;
        if (T == 3) cb.p.tunerParams.dcOffsetTuner = ch_[0].p.tunerParams.dcOffsetTuner;
    }
    if (reason & sdrplay_api_Update_Tuner_LoMode) {
        r = step("LO mode", applyLoMode(T));
        if (r != 0) return r;
        BandState nb;
        int flo = c.firstLO;
        r = step("RF frequency state update", selectBand(T, nb, flo));
        if (r != 0) return r;
        c.band = nb;
        c.firstLO = flo;
        if (nb.loFlag) c.loFlag = true;
        r = step("RF frequency hardware update", programBand(T, false));
        if (r != 0) return r;
        if (T == 3) cb.p.tunerParams.loMode = ch_[0].p.tunerParams.loMode;
    }
    if (reason & sdrplay_api_Update_Ctrl_DCoffsetIQimbalance) {
        if (T == 3) cb.p.ctrlParams.dcOffset = ch_[0].p.ctrlParams.dcOffset;
        configureDcIq(T);
    }
    if (reason & sdrplay_api_Update_Ctrl_Decimation) {
        if (T == 3) cb.p.ctrlParams.decimation = ch_[0].p.ctrlParams.decimation;
        updateDecimation(T);
    }
    if (reason & sdrplay_api_Update_Ctrl_Agc) {
        if (T == 3) cb.p.ctrlParams.agc = ch_[0].p.ctrlParams.agc;
        r = step("AGC control", configureAgc(T));
        if (r != 0) return r;
    }
    if (reason & sdrplay_api_Update_Ctrl_AdsbMode) {
        if (T == 3) cb.p.ctrlParams.adsbMode = ch_[0].p.ctrlParams.adsbMode;
        // ADS-B update: log only
    }
    if (reason & sdrplay_api_Update_Ctrl_OverloadMsgAck) dsp_.overloadAck = true;
    if (reason & sdrplay_api_Update_RspDuo_BiasTControl) {
        if (T != 2) return step(common::kStepDuoBiasTWrongTuner, sdrplay_api_InvalidParam); // LOG only
        r = step("bias-T control", biasT(2, true, false));
        if (r != 0) return r;
    }
    if (reason & sdrplay_api_Update_RspDuo_AmPortSelect) { // full RF program, then return
        r = step("AM port select", amPortSelect(T));
        if (r != 0) return r;
        return step("RF frequency hardware update", programBand(T, false));
    }
    if (reason & sdrplay_api_Update_RspDuo_Tuner1AmNotchControl) {
        r = step("tuner 1 AM notch", tuner1AmNotch(T, true));
        if (r != 0) return r;
        // both tuners (T == 3): channel B takes channel A's rfNotchEnable; its tuner1AmNotchEnable is
        // left unchanged
        if (T == 3) cb.p.rspDuoTunerParams.rfNotchEnable = ch_[0].p.rspDuoTunerParams.rfNotchEnable;
    }
    if (reason & sdrplay_api_Update_RspDuo_RfNotchControl) {
        r = step("broadcast notch", broadcastNotch(T, true));
        if (r != 0) return r;
    }
    if (reason & sdrplay_api_Update_RspDuo_RfDabNotchControl) {
        r = step("DAB notch", dabNotch(T, true));
        if (r != 0) return r;
        if (T == 3) cb.p.rspDuoTunerParams.rfDabNotchEnable = ch_[0].p.rspDuoTunerParams.rfDabNotchEnable;
        gainNeeded = true;
    }
    if (ext1 & sdrplay_api_Update_RspDuo_ResetSlaveFlags) {
        r = step("slave update flags reset", dropSlavePendingUpdates(T));
        if (r != 0) return r;
        if (sdrplay_api_RxChannelParamsT *s = shared(T)) {
            s->rspDuoTunerParams.resetSlaveFlags.resetGainUpdate = 0;
            s->rspDuoTunerParams.resetSlaveFlags.resetRfUpdate = 0;
        }
    }
    if (!gainNeeded) return 0;

    // requested or implied Tuner_Gr
    if (raw(c.p.ctrlParams.agc.enable) == 0) {
        grUserFlag_ = true;
        r = step(common::kStepProgramGr, writeGain(T));
        if (r != 0) {
            grUserFlag_ = false;
            return r;
        }
        if (T == 3) copyGainStruct(cb.p, ch_[0].p);
        return 0;
    }
    const int lna = c.p.tunerParams.gain.LNAstate;
    if (!agcLnaValid(lna, c.band.band, T, raw(c.p.rspDuoTunerParams.tuner1AmPortSel))) {
        err(OPENRSP_HERE, fmt("Tuner%d: LNAstate out of range LNAstate=%d", T, lna));
        return sdrplay_api_OutOfRange;
    }
    if (T == 3) cb.p.tunerParams.gain.LNAstate = ch_[0].p.tunerParams.gain.LNAstate;
    if (c.p.tunerParams.gain.syncUpdate >= 2) {
        err(OPENRSP_HERE,
            fmt("Tuner%d: Gain syncUpdate out of range (%d:%d) = %d", T, 0, 1, c.p.tunerParams.gain.syncUpdate));
        return sdrplay_api_OutOfRange;
    }
    dsp_.agcForced = true;
    dsp_.agcUserRequest = true;
    return 0;
}

} // namespace rspduo
} // namespace device
} // namespace sdrplay
