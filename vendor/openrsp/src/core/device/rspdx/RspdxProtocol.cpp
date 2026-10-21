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

#include "core/device/rspdx/RspdxProtocol.h"

#include "core/device/Models.h"
#include "core/device/rspduo/RspDuoCalc.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sdrplay {
namespace device {
namespace rspdx {

using common::kInitOk;
using common::kInitParams;
using common::kInitHardware;
using common::kInitChannelState;
using common::kInitStart;
using common::kStateIdle;
using common::kStateInitialised;
using common::kStateStopped;
using common::kStateStreaming;
using rsp1b::kReqFnAddr;
using rsp1b::kReqGpioDual0;
using rsp1b::kReqGpioDual1;
using rsp1b::kReqGpioExt;
using rsp1b::kReqReadReg;
using rsp1b::kReqSetInterface;
using rsp1b::kReqStart;
using rsp1b::kReqStop;
using rsp1b::kReqWriteReg;
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

constexpr std::uint32_t kBridgeGpioDx = 0x00EA80; // RSPdx bridge GPIO configuration (register 8)
constexpr std::uint32_t kReg8Hdr = 0x400;          // register 8: HDR front end selected

// DX front-end expander, port A (XA, kExpGpioA)
constexpr unsigned kXaHdrAtt = 0x01;      // HDR extra attenuator off
constexpr unsigned kXaBiasTOff = kDxBiasTOff;
constexpr unsigned kXaAntenna = kDxAntennaMask; // antenna select: A = 10, B = 01, C = 00
constexpr unsigned kXaAntennaA = kDxAntennaA;
constexpr unsigned kXaAntennaB = kDxAntennaB;
constexpr unsigned kXaNotchOff = kDxBroadcastNotchOff; // MW/FM broadcast notch off
constexpr unsigned kXaPad = 0x20;         // front-end pad
constexpr unsigned kXaDabNotchOff = kDxDabNotchOff;
constexpr unsigned kXaBand = 0x80;        // band path select (feBandA)
// port B (XB, kExpGpioB)
constexpr unsigned kXbAtt = 0x3C;         // step-attenuator code, bits 5:2
} // namespace

Protocol::Protocol(std::uint8_t hwVer, sdrplay_api_DevParamsT *dev, sdrplay_api_RxChannelParamsT *rx,
                   sdrplay_api_InternalRfTestT *internal, Io &io)
    : hwVer_(hwVer), variant_(hwVer == kHwRspDxR2 ? Variant::DxR2 : Variant::Dx), devShared_(dev), rxShared_(rx),
      internalShared_(internal), io_(io)
{
}

// ------------------------------------------------------------------ primitives (as RSP1B)
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
    // register 8 values are remembered before the transfer
    if (regByte == kRegBridgeGpio) reg8_ = value & 0xFFFFFFu;
    const std::uint32_t w = (value & 0xFFFFFFu) << 8 | regByte;
    return ctl(0x40, kReqWriteReg, static_cast<std::uint16_t>(w & 0xFFFF), static_cast<std::uint16_t>(w >> 16), 0);
}

int Protocol::gpioWrite(std::uint8_t reg, std::uint8_t val)
{
    return ctl(0x40, kReqGpioExt, static_cast<std::uint16_t>(reg << 8 | val), 0x0040, 0);
}

int Protocol::gpioDualWrite(int devId, std::uint8_t reg, std::uint8_t val)
{
    return ctl(0x40, devId == 0 ? kReqGpioDual0 : kReqGpioDual1, static_cast<std::uint16_t>(reg << 8 | val), 0x0040, 0);
}

int Protocol::setInterface(std::uint8_t alt) { return ctl(0x01, kReqSetInterface, alt, 0, 0); }
int Protocol::startRx() { return ctl(0x40, kReqStart, 0, 0, 0); }
int Protocol::stopRx() { return ctl(0x40, kReqStop, 0, 0, 0); }

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

void Protocol::latchChannel()
{
    if (rxShared_) ch_ = *rxShared_;
}

DspRequests Protocol::takeDspRequests()
{
    DspRequests r = dsp_;
    dsp_ = DspRequests{};
    return r;
}

int Protocol::hdrBw() const { return raw(ch_.rspDxTunerParams.hdrBw); }

bool Protocol::hdrActiveLive() const
{
    return rspdx::hdrActive(dev_.rspDxParams.hdrEnable, ch_.tunerParams.rfFreq.rfHz);
}

// ------------------------------------------------------------------ select
void Protocol::select()
{
    // Fresh device state: every shadow 0, first Init after Select pending.
    state_ = kStateIdle;
    bridgeOpen_ = false;
    fnAddrDone_ = false;
    gB_ = r2a_ = r2b_ = d1a_ = d2a_ = d2b_ = xa_ = xb_ = 0;
    reg8_ = 0;
    fs_ = FsState{};
    band_ = BandState{};
    firstLO_ = 0;
    loFlag_ = false;
    mix_ = 0.0;
    fSynth_ = 0.0;
    dc_ = DownConv{};
    dcHdr_ = false;
    frf_ = 0;
    r1Payload_ = 0;
    dcCalApplied_ = speedUpApplied_ = 0;
    appliedGr_ = -1;
    totalGr_ = 0;
    decimShift_ = 0;
    rfToggle_ = fsToggle_ = grToggle_ = false;
    rfPending_ = fsPending_ = grPending_ = false;
    syncPeriod_ = 0;
    syncDeferred_ = kNoSyncDeferred;
    rfUser_ = fsUser_ = false;
    dcIfType_ = 0;
    grUserFlag_ = false;
    dsp_ = DspRequests{};
}

// ------------------------------------------------------------------ limits
namespace {
struct LimitError {
    const char *fn;
    int line;
    std::string msg;
};

bool validateParams(const sdrplay_api_DevParamsT &d, const sdrplay_api_RxChannelParamsT &c, int tuner, LimitError &e)
{
    // Device / tuner parameter limits: the RSP1B list with the RSPdx
    // LNAstate limit (0:27).
    const char *fn = __func__;
    auto rng = [&](int line, const char *what, long long lo, long long hi, long long v) {
        e = {fn, line, common::limitMsg(what, lo, hi, v)};
        return false;
    };
    auto trng = [&](int line, const char *what, long long lo, long long hi, long long v) {
        e = {fn, line, common::tunerLimitMsg(tuner, what, lo, hi, v)};
        return false;
    };
    if (d.ppm < -1000.0 || d.ppm > 1000.0) {
        e = {fn, __LINE__, fmt("ppm out of range (%f:%f) = %f", -1000.0, 1000.0, d.ppm)};
        return false;
    }
    if (!(d.fsFreq.fsHz >= 2000000.0 && d.fsFreq.fsHz <= 10000000.0)) {
        e = {fn, __LINE__, fmt("fsFreq fsHz out of range (%f:%f) = %f", 2000000.0, 10000000.0, d.fsFreq.fsHz)};
        return false;
    }
    if (d.fsFreq.syncUpdate > 1) return rng(__LINE__, "fsFreq syncUpdate", 0, 1, d.fsFreq.syncUpdate);
    if (d.fsFreq.reCal > 1) return rng(__LINE__, "fsFreq reCal", 0, 1, d.fsFreq.reCal);
    if (raw(d.mode) < 0 || raw(d.mode) > 1) return rng(__LINE__, "mode", 0, 1, raw(d.mode));
    if (d.rsp1aParams.rfDabNotchEnable > 1)
        return rng(__LINE__, "rsp1a rfDabNotchEnable", 0, 1, d.rsp1aParams.rfDabNotchEnable);
    if (d.rsp1aParams.rfNotchEnable > 1) return rng(__LINE__, "rsp1a rfNotchEnable", 0, 1, d.rsp1aParams.rfNotchEnable);
    if (d.rsp2Params.extRefOutputEn > 1) return rng(__LINE__, "rsp2 extRefOutputEn", 0, 1, d.rsp2Params.extRefOutputEn);
    if (raw(d.rspDuoParams.extRefOutputEn) < 0 || raw(d.rspDuoParams.extRefOutputEn) > 1)
        return rng(__LINE__, "rspDuo extRefOutputEn", 0, 1, raw(d.rspDuoParams.extRefOutputEn));
    if (d.rspDxParams.hdrEnable > 1) return rng(__LINE__, "rspDx hdrEnable", 0, 1, d.rspDxParams.hdrEnable);
    if (d.rspDxParams.biasTEnable > 1) return rng(__LINE__, "rspDx biasTEnable", 0, 1, d.rspDxParams.biasTEnable);
    if (d.rspDxParams.rfDabNotchEnable > 1)
        return rng(__LINE__, "rspDx rfDabNotchEnable", 0, 1, d.rspDxParams.rfDabNotchEnable);
    if (d.rspDxParams.rfNotchEnable > 1) return rng(__LINE__, "rspDx rfNotchEnable", 0, 1, d.rspDxParams.rfNotchEnable);
    if (raw(d.rspDxParams.antennaSel) < 0 || raw(d.rspDxParams.antennaSel) > 2)
        return rng(__LINE__, "rspDx antennaSel", 0, 2, raw(d.rspDxParams.antennaSel));

    const sdrplay_api_TunerParamsT &t = c.tunerParams;
    const sdrplay_api_ControlParamsT &k = c.ctrlParams;
    const int bw = raw(t.bwType);
    if (bw < 0 || bw > 8000) return trng(__LINE__, "bwType", 0, 8000, bw);
    const int ift = raw(t.ifType);
    if (ift < -1 || ift > 2048) return trng(__LINE__, "ifType", -1, 2048, ift);
    if (raw(t.loMode) < 0 || raw(t.loMode) > 4) return trng(__LINE__, "loMode", 0, 4, raw(t.loMode));
    const int minGr = raw(t.gain.minGr);
    if (minGr < 0 || minGr > 20) return trng(__LINE__, "gain minGr", 0, 20, minGr);
    if (!(t.gain.gRdB < 60 && minGr <= t.gain.gRdB)) return trng(__LINE__, "gain gRdB", minGr, 59, t.gain.gRdB);
    if (t.gain.syncUpdate > 1) return trng(__LINE__, "gain syncUpdate", 0, 1, t.gain.syncUpdate);
    if (t.gain.LNAstate > 27) return trng(__LINE__, "gain LNAstate", 0, 27, t.gain.LNAstate); // RSPdx
    if (t.rfFreq.rfHz < 0.0 || t.rfFreq.rfHz > 2000000000.0) {
        e = {fn, __LINE__,
             fmt("Tuner%d: rfFreq rfHz out of range (%f:%f) = %f", tuner, 0.0, 2000000000.0, t.rfFreq.rfHz)};
        return false;
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
    if (c.rspDuoTunerParams.rfNotchEnable > 1)
        return trng(__LINE__, "rspDuo rfNotchEnable", 0, 1, c.rspDuoTunerParams.rfNotchEnable);
    if (c.rspDuoTunerParams.rfDabNotchEnable > 1)
        return trng(__LINE__, "rspDuo rfDabNotchEnable", 0, 1, c.rspDuoTunerParams.rfDabNotchEnable);
    if (raw(c.rspDxTunerParams.hdrBw) < 0 || raw(c.rspDxTunerParams.hdrBw) > 3)
        return trng(__LINE__, "rspDx hdrBw", 0, 3, raw(c.rspDxTunerParams.hdrBw));
    return true;
}
} // namespace

// ------------------------------------------------------------------ Init
int Protocol::init()
{
    curTuner_ = 1;
    initFailStage_ = kInitOk;
    if (state_ != kStateIdle) {
        err(OPENRSP_HERE, "Not in uninitialised state");
        return sdrplay_api_AlreadyInitialised;
    }
    sdrplay_api_DevParamsT d{};
    sdrplay_api_RxChannelParamsT c{};
    if (devShared_) d = *devShared_;
    if (rxShared_) c = *rxShared_;
    LimitError le;
    if (!validateParams(d, c, curTuner_, le)) {
        err(__FILE__, le.fn, le.line, le.msg);
        initFailStage_ = kInitParams;
        return sdrplay_api_OutOfRange;
    }
    latchDev();
    latchChannel();
    if (internalShared_) internal_ = *internalShared_;

    // initial fs state
    FsState st;
    if (rsp1b::fsState(dev_.fsFreq.fsHz, raw(ch_.tunerParams.ifType) != 0, raw(dev_.mode) == 1, st) != 0) {
        err(OPENRSP_HERE, fmt("fsMHz out of range %.3f", dev_.fsFreq.fsHz / 1e6));
        initFailStage_ = kInitChannelState;
        return sdrplay_api_OutOfRange;
    }
    fs_ = st;
    rfToggle_ = rfPending_ = false;
    fsToggle_ = fsPending_ = false;
    grToggle_ = grPending_ = false;
    syncPeriod_ = 0;
    syncDeferred_ = kNoSyncDeferred;
    // initial shadows
    gB_ = 0xFF;
    r2a_ = 0x00;
    r2b_ = 0x00;
    d1a_ = 0x9F;
    d2a_ = 0xFF;
    d2b_ = 0xFF;
    xa_ = 0xFF;
    xb_ = 0xFF;
    dsp_ = DspRequests{};
    // channel init: Frf = 875000, then configureDownConverter,
    // applied GR -1, firstLO 120 MHz, dcCal/speedUp 3/0; r1Payload_ is NOT reset.
    frf_ = kHdrDefaultFrf;
    configureDownConverter();
    appliedGr_ = -1;
    firstLO_ = 120000000;
    loFlag_ = false;
    dcCalApplied_ = 3;
    speedUpApplied_ = 0;
    agcEnableState_ = 0;
    if (applyLoMode() == 0) {
        BandState b;
        if (selectBand(b) == 0) {
            band_ = b;
            firstLO_ = b.b.firstLO;
            if (b.b.loFlag) loFlag_ = true;
        }
    }

    const int r = initHardware();
    if (r != 0) {
        initFailStage_ = kInitHardware;
        return r;
    }
    state_ = kStateInitialised;
    const int rs = start(false);
    if (rs != 0) initFailStage_ = kInitStart;
    return rs;
}

int Protocol::writeBridgeGpio()
{
    if (const int rc = setRegister(kRegBridgeGpio, kBridgeGpioDx); rc != 0) {
        errVerbose(OPENRSP_HERE, fmt("register 0x08 write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::initHardware()
{
    if (writeBridgeGpio() != 0) return sdrplay_api_HwError;
    if (const int rc = bridgeInit(); rc != 0) {
        errVerbose(OPENRSP_HERE, fmt("Tuner%d: bridge init returned error %d", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    // results ignored; only the DAB-notch handler writes (FE12 with the previous antenna bits).
    rspDxBiasT(false, false);
    rspDxBroadcastNotch(false);
    rspDxDabNotch();
    rspDxAntenna(false);
    rspDxHdrBw();
    if (const int rc = tunerWrite(kTunerReg5Init); rc != 0) {
        errVerbose(OPENRSP_HERE, fmt("register 0x09 write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    if (setRegister(kRegPacketFormat, fs_.reg7) != 0) return sdrplay_api_HwError; // rate hardware update
    applyTunerDc();
    applyLoMode();
    configureDcIq();
    updateDecimation();
    configureAgc();
    rfPending_ = false;
    int r = programBand(false);
    if (r != 0) return r;
    grPending_ = false;
    r = programGain(false);
    if (r != 0) return r;
    const std::uint8_t savedReCal = dev_.fsFreq.reCal;
    dev_.fsFreq.reCal = 1;
    r = writeRate();
    dev_.fsFreq.reCal = savedReCal;
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

int Protocol::bridgeInit()
{
    // Open the bridge: alt setting, then the first-Init-after-Select sequence or a trim clear
    bridgeOpen_ = true;
    int r = setInterface(fs_.alt);
    if (r == 0) {
        if (!fnAddrDone_) {
            fnAddrDone_ = true;
            std::uint8_t v = 0;
            r = ctl(0xC0, kReqFnAddr, 0, 0, 1, &v);
            if (r == 0 && (v & 3) == 2) r = io_.stream(StreamOp::ResetDevice);
            if (r == 0) r = ctl(0xC0, kReqFnAddr, 0, 0, 1, &v);
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

int Protocol::start(bool toggles)
{
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
    fsPending_ = rfPending_ = grPending_ = false;
    io_.disarmed(Change::Gr);
    io_.disarmed(Change::Rf);
    io_.disarmed(Change::Fs);
    return 0;
}

void Protocol::stopStream()
{
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
    if (state_ == kStateIdle) return sdrplay_api_NotInitialised;
    if (state_ == kStateStreaming) stopStream();
    if (apiCall) io_.overloadCorrected(curTuner_);
    rspDxBiasT(true, true);
    xa_ = static_cast<std::uint8_t>(xa_ | kXaPad);
    gpioWrite(kExpGpioA, xa_);
    bridgeUninit();
    state_ = kStateIdle;
    return 0;
}

void Protocol::bridgeUninit()
{
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

void Protocol::release()
{
    if (state_ != kStateIdle) uninitImpl(false);
    if (bridgeOpen_) {
        setInterface(0);
        bridgeOpen_ = false;
    }
}

// ------------------------------------------------------------------ tuner helpers
int Protocol::readTunerTrims(std::uint32_t &v)
{
    if (tunerWrite(kTunerReadbackStart) != 0) return 1;
    io_.sleepMs(1);
    if (tunerWrite(kTunerReadbackEnd) != 0) return 1;
    io_.sleepMs(1);
    std::uint8_t b[4] = {0, 0, 0, 0};
    if (ctl(0xC0, kReqReadReg, 0, 0x0010, 4, b) != 0) return 1;
    v = static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8 |
        static_cast<std::uint32_t>(b[2]) << 16 | static_cast<std::uint32_t>(b[3]) << 24;
    return 0;
}

int Protocol::applyTunerDc()
{
    const auto &dc = ch_.tunerParams.dcOffsetTuner;
    if (dc.dcCal > 5) {
        err(OPENRSP_HERE, fmt("Tuner%d: dcCal out of range[%d:%d] %d", curTuner_, 0, 5, dc.dcCal));
        return sdrplay_api_OutOfRange;
    }
    if (dc.speedUp > 1) {
        err(OPENRSP_HERE, fmt("Tuner%d: speedUp out of range[%d:%d] %d", curTuner_, 0, 1, dc.speedUp));
        return sdrplay_api_OutOfRange;
    }
    if (dc.trackTime < 0 || dc.trackTime > 63) {
        err(OPENRSP_HERE, fmt("Tuner%d: trackTime out of range[%d:%d] %d", curTuner_, 0, 63, dc.trackTime));
        return sdrplay_api_OutOfRange;
    }
    if (dc.refreshRateTime < 0 || dc.refreshRateTime > 4095) {
        err(OPENRSP_HERE,
            fmt("Tuner%d: refreshRateTime out of range[%d:%d] %d", curTuner_, 0, 4095, dc.refreshRateTime));
        return sdrplay_api_OutOfRange;
    }
    dcCalApplied_ = dc.dcCal;
    speedUpApplied_ = dc.speedUp;
    if (const int rc = tunerWrite(tunerDcWord(dc.refreshRateTime, dc.trackTime)); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: register write failed 0x%08X", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::applyLoMode()
{
    const int r = rsp1b::loModeFirstLo(raw(ch_.tunerParams.loMode), firstLO_);
    if (r != 0)
        err(OPENRSP_HERE, fmt("Tuner%d: invalid mode %d", curTuner_, raw(ch_.tunerParams.loMode)));
    return r;
}

int Protocol::selectBand(BandState &out)
{
    const int r = rspdx::computeBand(ch_.tunerParams.rfFreq.rfHz, raw(ch_.tunerParams.loMode), firstLO_,
                                     dev_.rspDxParams.hdrEnable, out);
    if (r != 0)
        errVerbose(OPENRSP_HERE,
            fmt("Tuner%d: rfMHz out of range %.3f", curTuner_, ch_.tunerParams.rfFreq.rfHz / 1e6));
    return r;
}

void Protocol::configureDownConverter()
{
    // hdrActive is evaluated live (hdrActiveLive()),
    // so a Dev_Fs update after a hdrEnable change switches only the DSP.
    if (hdrActiveLive()) {
        dcHdr_ = true;
        mix_ = 0.0; // no mix frequency in HDR
        dcIfType_ = -1; // no IF code in r0 (HDR r0 040e10)
        dc_ = hdrDownConverter(fs_.fsMHz);
    } else {
        dcHdr_ = false;
        {
            const int ift = raw(ch_.tunerParams.ifType);
            dcIfType_ = ift == 0 ? 0 : (ift == 2048 || ift == 1620 || ift == 450) ? ift : -1;
        }
        mix_ = rsp1b::mixFreq(dev_.fsFreq.fsHz, raw(ch_.tunerParams.bwType), raw(ch_.tunerParams.ifType));
        dc_ = rsp1b::downConverter(dev_.fsFreq.fsHz, raw(ch_.tunerParams.bwType), raw(ch_.tunerParams.ifType));
    }
    dsp_.downConverterReinit = true;
}

int Protocol::rspDxHdr()
{
    // register 8 HDR bit set / cleared, register 8 always written.
    // A failed register 8 write fails the call.
    if (hdrActiveLive()) {
        if (const int rc = setRegister(kRegBridgeGpio, reg8_ | kReg8Hdr); rc != 0) {
            err(OPENRSP_HERE, fmt("GPIO bit set failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
    } else {
        if (const int rc = setRegister(kRegBridgeGpio, reg8_ & ~kReg8Hdr); rc != 0) {
            err(OPENRSP_HERE, fmt("GPIO bit clear failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
    }
    return 0;
}

int Protocol::programBand(bool bwChange)
{
    const int bw = raw(ch_.tunerParams.bwType);
    const int ift = dcIfType_; // r0 IF code from the down-converter state
    std::uint32_t w0 = rsp1b::reg0Word(band_.b, bw, ift, bwChange && loFlag_);
    // HDR band program: r0 carries no IF code (bits 13:12 = 00), e.g. 040e10
    // for ifType 0 / bwType 200 on the RSPdx and RSPdx-R2; the non-HDR program keeps 043e10.
    // The IF code is cleared for every ifType/bwType combination in HDR.
    if (band_.hdrActive) w0 &= ~0x3000u;
    if (const int rc = tunerWrite(w0); rc != 0) {
        errVerbose(OPENRSP_HERE, fmt("Tuner%d: register 0x09 write failed 0x%08X", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    xa_ = static_cast<std::uint8_t>((xa_ & ~kXaBand) | feBandA(band_));
    xb_ = static_cast<std::uint8_t>((xb_ & kXbAtt) | feBandB(band_));
    gpioWrite(kExpGpioA, xa_); // results ignored
    gpioWrite(kExpGpioB, xb_);
    gpioWrite(kExpIodirA, 0x00); // every pin an output
    gpioWrite(kExpIodirB, 0x00);
    if (rspDxHdr() != 0) {
        errVerbose(OPENRSP_HERE, fmt("Tuner%d: HDR enable failed", curTuner_));
        return sdrplay_api_HwError;
    }
    if (bwChange || band_.hdrActive) return writeRf(); // HDR: no trims, no LO sequence
    bool heldRfUser = false; // the LO-flag sequence reports its second synth write only
    if (loFlag_) {
        heldRfUser = rfUser_;
        rfUser_ = false;
    }
    int r = writeRf();
    if (r != 0) {
        rfUser_ = false;
        return r;
    }
    if (!loFlag_) {
        // trim bypass: r13 / r14 with every trim 0
        tunerWrite(kTunerReg13Clear);
        tunerWrite(kTunerReg14Clear);
        return 0;
    }
    loFlag_ = false;
    std::uint32_t v = 0;
    readTunerTrims(v);
    tunerWrite(rsp1b::reg0Word(band_.b, bw, ift, true));
    rfUser_ = heldRfUser;
    r = writeRf();
    if (r != 0) return r;
    std::uint32_t r13 = 0, r14 = 0;
    rsp1b::bypassTrim(rsp1b::readbackTrims(v), 1, r13, r14);
    tunerWrite(r13);
    tunerWrite(r14);
    return 0;
}

// ------------------------------------------------------------------ programRf / programSampleRate
GainVals Protocol::computeGainVals(int gApplied, int lnaState) const
{
    if (band_.hdrActive) return hdrGainVals(variant_, frf_, dc_.enabled, lnaState);
    return gainValues(variant_, rsp1b::gainFreqMHz(fSynth_, mix_, band_.b.lif1Hz), raw(ch_.tunerParams.bwType),
                      raw(ch_.tunerParams.ifType), dc_.enabled, raw(ch_.tunerParams.gain.minGr), gApplied, lnaState,
                      (xa_ & kXaDabNotchOff) == 0, raw(dev_.rspDxParams.antennaSel) == sdrplay_api_RspDx_ANTENNA_C);
}

int Protocol::programRf()
{
    if (!band_.hdrActive) {
        if (rfPending_) {
            errWarning(OPENRSP_HERE, fmt("Tuner%d: previous Rf update not received (or timed out)", curTuner_));
            return sdrplay_api_RfUpdateError;
        }
        // non-HDR: rf must lie in the CURRENT band's range (the RSPduo band table)
        const double rf = ch_.tunerParams.rfFreq.rfHz;
        if (!rspduo::rfInBandRange(rf, band_.b.band)) {
            err(OPENRSP_HERE, fmt("Tuner%d: rfHz out of range %.1fHz (abs=1)", curTuner_, rf));
            return sdrplay_api_OutOfRange;
        }
        rsp1b::TunerPll p;
        const int r = rsp1b::tunerPll(ch_.tunerParams.rfFreq.rfHz, dev_.ppm, mix_, band_.b, p);
        // fSynth is stored before the INT/FRAC checks and kept when they fail
        fSynth_ = p.fSynthHz;
        if (r != 0) {
            if (p.fail == rsp1b::kPllFracOutOfRange)
                errWarning(OPENRSP_HERE,
                    fmt("Tuner%d: detected FRAC out of range - returning without programming tuner", curTuner_));
            else
                errWarning(OPENRSP_HERE,
                    fmt("Tuner%d: detected INT out of range - returning without programming tuner", curTuner_));
            return sdrplay_api_OutOfRange;
        }
        const std::uint8_t sync = ch_.tunerParams.rfFreq.syncUpdate;
        if (const int rc = tunerWrite(p.r3, sync); rc != 0) {
            errWarning(OPENRSP_HERE, fmt("Tuner%d: AFC register write failed 0x%08X", curTuner_, rc));
            return sdrplay_api_HwError;
        }
        if (const int rc = tunerWrite(p.r2, sync); rc != 0) {
            errWarning(OPENRSP_HERE, fmt("Tuner%d: RF register write failed 0x%08X", curTuner_, rc));
            return sdrplay_api_HwError;
        }
        rfToggle_ = !rfToggle_;
        rfPending_ = true;
        const bool report = rfUser_; // report request taken, then cleared
        rfUser_ = false;
        io_.armed(Change::Rf, rfToggle_, report);
    }
    // HDR and non-HDR: Frf for the DSP
    frf_ = hdrFrf(ch_.tunerParams.rfFreq.rfHz);
    if (appliedGr_ != -1) {
        const GainVals gv = computeGainVals(appliedGr_, ch_.tunerParams.gain.LNAstate);
        if (rxShared_) rxShared_->tunerParams.gain.gainVals = {gv.curr, gv.max, gv.min};
        ch_.tunerParams.gain.gainVals = {gv.curr, gv.max, gv.min};
        GainChangeEvent e;
        e.tuner = curTuner_;
        e.gRdB = static_cast<unsigned>(appliedGr_);
        e.lnaGRdB = static_cast<unsigned>(totalGr_ - appliedGr_);
        e.currGain = static_cast<double>(gv.curr);
        io_.gainChange(e);
    }
    return 0;
}

int Protocol::writeRf()
{
    for (int i = 0; i < 16; ++i) {
        if (!rfPending_) {
            programRf();
            return 0;
        }
        io_.sleepMs(25);
    }
    dev_.resetFlags.resetRfUpdate = 1;
    dropPendingUpdates();
    return programRf();
}

int Protocol::programSampleRate()
{
    if (fsPending_) {
        errWarning(OPENRSP_HERE, "Previous Fs update not recieved (or timed out)");
        return sdrplay_api_FsUpdateError;
    }
    rsp1b::AdcPll p;
    const double fsHz = dev_.fsFreq.fsHz;
    if (!(2000000.0 <= fsHz && fsHz <= 10000000.0)) {
        err(OPENRSP_HERE, fmt("fsHz out of range %.6fHz ", fsHz));
        return sdrplay_api_OutOfRange;
    }
    if (rsp1b::adcPll(fsHz, dev_.ppm, fs_.D, fs_.V, dev_.fsFreq.reCal, p) != 0) {
        errWarning(OPENRSP_HERE, "requested invalid IntVal");
        return sdrplay_api_OutOfRange;
    }
    const std::uint8_t sync = static_cast<std::uint8_t>((dev_.fsFreq.syncUpdate & 7) << 5);
    if (setRegister(static_cast<std::uint8_t>(sync | kRegAdcFrac), p.reg4) != 0) return sdrplay_api_HwError;
    if (setRegister(static_cast<std::uint8_t>(sync | kRegAdcPll), p.reg3) != 0) return sdrplay_api_HwError;
    fsPending_ = true;
    fsToggle_ = !fsToggle_;
    const bool report = fsUser_; // report request taken, then cleared
    fsUser_ = false;
    io_.armed(Change::Fs, fsToggle_, report);
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
    dropPendingUpdates();
    return programSampleRate();
}

// ------------------------------------------------------------------ programGain
void Protocol::finishGr(int gApplied, int tuner)
{
    // gain values, copy back to the shared params, GainChange
    auto &g = ch_.tunerParams.gain;
    const GainVals gv = computeGainVals(gApplied, g.LNAstate);
    g.gainVals = {gv.curr, gv.max, gv.min};
    if (rxShared_) {
        auto &s = rxShared_->tunerParams.gain;
        s.gRdB = g.gRdB;
        if (raw(ch_.ctrlParams.agc.enable) == 0) s.LNAstate = g.LNAstate;
        s.minGr = g.minGr;
        s.syncUpdate = g.syncUpdate;
        s.gainVals = g.gainVals;
    }
    GainChangeEvent ev;
    ev.tuner = tuner;
    ev.gRdB = static_cast<unsigned>(gApplied);
    ev.lnaGRdB = static_cast<unsigned>(totalGr_ - gApplied);
    ev.currGain = static_cast<double>(gv.curr);
    io_.gainChange(ev);
}

int Protocol::programGain(bool fromAgc)
{
    if (band_.hdrActive) return programGainHdr(fromAgc);
    auto &g = ch_.tunerParams.gain;
    const int gr = g.gRdB;
    const int lna = g.LNAstate;
    const int minGr = raw(g.minGr);
    if (!fromAgc && !setGrValid(gr, minGr, band_.b.band, band_.b.dxSub, lna)) {
        err(OPENRSP_HERE, fmt("Tuner%d: parameter out of range gRdB=%d LNAstate=%d)", curTuner_, gr, lna));
        return sdrplay_api_OutOfRange;
    }
    if (grPending_) {
        errWarning(OPENRSP_HERE, fmt("Tuner%d: previous gain update not received (or timed out)", curTuner_));
        return sdrplay_api_GainUpdateError;
    }
    const int gApplied = gr < minGr ? minGr : (gr > 59 ? 59 : gr);
    const int lnaC = lna > 27 ? 27 : lna;
    g.gRdB = gApplied;
    if (raw(ch_.ctrlParams.agc.enable) == 0) g.LNAstate = static_cast<unsigned char>(lnaC);
    const LnaEntryView e = lnaEntry(band_.row, lnaC);
    appliedGr_ = gApplied;
    r1Payload_ = gainBits(band_.b.band, gApplied, e);
    const std::uint32_t r1 = static_cast<std::uint32_t>(dcCalApplied_) << 14 |
                             static_cast<std::uint32_t>(speedUpApplied_) << 17 | r1Payload_ << 4 | 1u;
    if (const int rc = tunerWrite(r1, g.syncUpdate); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: register write failed 0x%08X", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    // expander: XA bit 5 pad, XB[5:2] step attenuator; FE13 before FE12
    const FeAtt fa = attBits(band_.b.band, band_.b.dxSub, e.attDb);
    xb_ = static_cast<std::uint8_t>((xb_ & ~kXbAtt) | fa.code << 2);
    xa_ = static_cast<std::uint8_t>((xa_ & ~kXaPad) | (fa.pad ? kXaPad : 0));
    if (const int rc = gpioWrite(kExpGpioB, xb_); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: GPIO write failed 0x%08X", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    if (const int rc = gpioWrite(kExpGpioA, xa_); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: GPIO write failed 0x%08X", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    totalGr_ = totalGr(band_, gApplied, e);
    grToggle_ = !grToggle_;
    grPending_ = true;
    io_.armed(Change::Gr, grToggle_, grUserFlag_);
    grUserFlag_ = false;
    finishGr(gApplied, curTuner_);
    return 0;
}

int Protocol::programGainHdr(bool fromAgc)
{
    // Gain programming in HDR: gRdB is neither validated nor used
    auto &g = ch_.tunerParams.gain;
    const int lna = g.LNAstate;
    if (lna > kHdrMaxLna && !fromAgc) {
        err(OPENRSP_HERE,
            fmt("Tuner%d: parameter out of range (LNAstate=%d) numStatesDx=%d", curTuner_, lna, kHdrMaxLna + 1));
        return sdrplay_api_OutOfRange;
    }
    if (grPending_) {
        err(OPENRSP_HERE, fmt("Tuner%d: previous gain update not received (or timed out)", curTuner_));
        return sdrplay_api_GainUpdateError;
    }
    // IFGR re-write: the r1 payload of the last non-HDR programGain
    const std::uint32_t r1 = static_cast<std::uint32_t>(dcCalApplied_) << 14 |
                             static_cast<std::uint32_t>(speedUpApplied_) << 17 | r1Payload_ << 4 | 1u;
    if (const int rc = tunerWrite(r1, g.syncUpdate); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: register write failed 0x%08X", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    grToggle_ = !grToggle_;
    grPending_ = true;
    io_.armed(Change::Gr, grToggle_, grUserFlag_);
    grUserFlag_ = false;
    appliedGr_ = 0;
    const int lnaC = lna > kHdrMaxLna ? kHdrMaxLna : lna;
    if (raw(ch_.ctrlParams.agc.enable) == 0) g.LNAstate = static_cast<unsigned char>(lnaC);
    const int att = hdrLnaEntry(lnaC).attDb;
    const HdrAtt ha = hdrAttBits(att);
    xb_ = static_cast<std::uint8_t>((xb_ & ~kXbAtt) | ha.code << 2);
    xa_ = static_cast<std::uint8_t>((xa_ & ~(kXaPad | kXaHdrAtt)) | kXaPad | (ha.a0 ? kXaHdrAtt : 0));
    if (const int rc = gpioWrite(kExpGpioB, xb_); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: GPIO write failed 0x%08X", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    if (const int rc = gpioWrite(kExpGpioA, xa_); rc != 0) {
        err(OPENRSP_HERE, fmt("Tuner%d: GPIO write failed 0x%08X", curTuner_, rc));
        return sdrplay_api_HwError;
    }
    totalGr_ = att;
    finishGr(0, curTuner_);
    return 0;
}

int Protocol::writeGain()
{
    for (int i = 0; i < 16; ++i) {
        if (!grPending_) return programGain(false);
        io_.sleepMs(25);
    }
    dev_.resetFlags.resetGainUpdate = 1;
    dropPendingUpdates();
    return programGain(false);
}

int Protocol::applyAgcGain(int gRdB)
{
    if (state_ == kStateIdle) return sdrplay_api_NotInitialised;
    if (rxShared_) {
        ch_.tunerParams.gain.minGr = rxShared_->tunerParams.gain.minGr;
        ch_.tunerParams.gain.LNAstate = rxShared_->tunerParams.gain.LNAstate;
        ch_.ctrlParams.agc.enable = rxShared_->ctrlParams.agc.enable;
    }
    const int prevGr = ch_.tunerParams.gain.gRdB;
    ch_.tunerParams.gain.gRdB = gRdB;
    const int r = programGain(true);
    // gain update still pending: nothing was applied or written
    if (r == sdrplay_api_GainUpdateError) ch_.tunerParams.gain.gRdB = prevGr;
    return r;
}

void Protocol::dropPendingUpdates()
{
    if (dev_.resetFlags.resetGainUpdate) {
        dev_.resetFlags.resetGainUpdate = 0;
        grPending_ = false;
        grToggle_ = !grToggle_;
        io_.disarmed(Change::Gr);
    }
    if (dev_.resetFlags.resetRfUpdate) {
        dev_.resetFlags.resetRfUpdate = 0;
        rfPending_ = false;
        rfToggle_ = !rfToggle_;
        io_.disarmed(Change::Rf);
    }
    if (dev_.resetFlags.resetFsUpdate) {
        dev_.resetFlags.resetFsUpdate = 0;
        fsPending_ = false;
        fsToggle_ = !fsToggle_;
        io_.disarmed(Change::Fs);
    }
}

void Protocol::onPacketToggles(const ToggleBits &t)
{
    if (grPending_ && t.gr == grToggle_) grPending_ = false;
    if (rfPending_ && t.rf == rfToggle_) rfPending_ = false;
    if (fsPending_ && t.fs == fsToggle_) fsPending_ = false;
}

int Protocol::onPacketSampleNum(std::uint32_t sampleNum)
{
    // a deferred sync update is written once the sample number has wrapped (top bit clear again)
    if (syncDeferred_ == kNoSyncDeferred || sampleNum >= 0x80000000u) return 0;
    const int r = programSyncUpdate();
    if (r == 0) syncDeferred_ = kNoSyncDeferred;
    return r;
}

void Protocol::confirmAllPending() { grPending_ = rfPending_ = fsPending_ = false; }

int Protocol::programSyncUpdate()
{
    // latched down-converter (in HDR: x3 at fs 6 MHz, else << 4)
    std::uint32_t period = dev_.syncUpdate.period;
    std::uint32_t sample = dev_.syncUpdate.sampleNum;
    if (dc_.enabled) {
        period = (period << dc_.countShift) * static_cast<std::uint32_t>(dc_.countDivisor);
        sample = (sample << dc_.countShift) * static_cast<std::uint32_t>(dc_.countDivisor);
    }
    if (ch_.ctrlParams.decimation.enable) {
        period <<= decimShift_;
        sample <<= decimShift_;
    }
    if (syncPeriod_ != period) {
        syncPeriod_ = period;
        if (const int rc = setRegister(kRegSyncPeriod, period | 0x800000u); rc != 0) {
            err(OPENRSP_HERE, fmt("Tuner%d: register write failed 0x%08X", curTuner_, rc));
            return sdrplay_api_HwError;
        }
    }
    if (fs_.wrapThr < sample) {
        // beyond the wrap threshold: step whole periods until the 32-bit sample number wraps below it
        if (syncPeriod_ == 0) syncPeriod_ = 1024;
        std::uint32_t s = sample;
        while (fs_.wrapThr < s) s += syncPeriod_;
        syncDeferred_ = s;
        io_.syncDeferred();
        return 0;
    }
    if (setRegister(kRegSyncSampleLo, sample & 0xFFFF) != 0) return sdrplay_api_HwError;
    if (setRegister(kRegSyncSampleHi, sample >> 16) != 0) return sdrplay_api_HwError;
    return 0;
}

int Protocol::applyPpm()
{
    int r = writeRf();
    if (r != 0) return r;
    dev_.fsFreq.reCal = 1;
    r = writeRate();
    dev_.fsFreq.reCal = 0;
    return r;
}

int Protocol::configureAgc()
{
    // setpoint limit from the packet format
    int lim = -72;
    if (fs_.spp == 384) lim = -60;
    else if (fs_.spp == 504) lim = raw(ch_.tunerParams.ifType) == 0 ? -48 : -72;
    const int sp = ch_.ctrlParams.agc.setPoint_dBfs;
    if (sp > 0 || sp < lim) {
        err(OPENRSP_HERE,
            fmt("Tuner%d: setPoint_dBfs out of range for current sampling freq[%d:0], %d", curTuner_, lim, sp));
        return sdrplay_api_OutOfRange;
    }
    const int en = raw(ch_.ctrlParams.agc.enable);
    dsp_.agcForced = false;
    if (agcEnableState_ == 0 && en != 0) dsp_.agcForced = true;
    agcEnableState_ = en;
    dsp_.agcChanged = true;
    return 0;
}

void Protocol::updateDecimation()
{
    const auto &d = ch_.ctrlParams.decimation;
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
    decimShift_ = s;
    dsp_.decimationChanged = true;
}

void Protocol::configureDcIq()
{
    if (ch_.ctrlParams.dcOffset.IQenable) ch_.ctrlParams.dcOffset.DCenable = ch_.ctrlParams.dcOffset.IQenable;
    dsp_.dcIqReset = true;
}

// ------------------------------------------------------------------ RSPdx front-end handlers
int Protocol::rspDxBiasT(bool write, bool force)
{
    // bias-T: XA bit 1, 0 = on
    std::uint8_t b = static_cast<std::uint8_t>(xa_ & ~kXaBiasTOff);
    if (dev_.rspDxParams.biasTEnable != 1 || force) b |= kXaBiasTOff;
    xa_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDxAntenna(bool write)
{
    // antenna select: XA[3:2] A = 10, B = 01, C = 00 (other values -> A)
    std::uint8_t b = static_cast<std::uint8_t>(xa_ & ~kXaAntenna);
    const int sel = raw(dev_.rspDxParams.antennaSel);
    if (sel == sdrplay_api_RspDx_ANTENNA_B) b |= kXaAntennaB;
    else if (sel != sdrplay_api_RspDx_ANTENNA_C) b |= kXaAntennaA;
    xa_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDxBroadcastNotch(bool write)
{
    // broadcast notch: XA bit 4, 0 = on
    std::uint8_t b = static_cast<std::uint8_t>(xa_ & ~kXaNotchOff);
    if (dev_.rspDxParams.rfNotchEnable != 1) b |= kXaNotchOff;
    xa_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDxDabNotch()
{
    // DAB notch: XA bit 6, 0 = on; always writes
    std::uint8_t b = static_cast<std::uint8_t>(xa_ & ~kXaDabNotchOff);
    if (dev_.rspDxParams.rfDabNotchEnable != 1) b |= kXaDabNotchOff;
    xa_ = b;
    if (const int rc = gpioWrite(kExpGpioA, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDxHdrBw()
{
    // HDR bandwidth: an out-of-range value is replaced by BW_1_700 without an error; returns 0
    if (raw(ch_.rspDxTunerParams.hdrBw) < 0 || raw(ch_.rspDxTunerParams.hdrBw) > 3)
        ch_.rspDxTunerParams.hdrBw = sdrplay_api_RspDx_HDRMODE_BW_1_700;
    return 0;
}

// ------------------------------------------------------------------ foreign handlers
int Protocol::rsp1aBiasT(bool write, bool force)
{
    // RSP1A bias-T: the RSP1A-style port B through kReqGpioExt
    const std::uint8_t b = withBits(gB_, kRsp1aBiasTOff, ch_.rsp1aTunerParams.biasTEnable != 1 || force);
    gB_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp1aBroadcastNotch(bool write)
{
    const std::uint8_t b = withBits(gB_, kRsp1aBroadcastNotchOff, dev_.rsp1aParams.rfNotchEnable != 1);
    gB_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp1aDabNotch(bool write)
{
    const std::uint8_t b = withBits(gB_, kRsp1aDabNotchOff, dev_.rsp1aParams.rfDabNotchEnable != 1);
    gB_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp1aRfTest(bool write)
{
    std::uint8_t b = withBits(gB_, kRsp1aRfTestOff, internal_.rsp1aRfTestEn != 1);
    b = withBits(b, kRsp1aRfTestSrcStability, internal_.rsp1aRfTestSrcStability != 0);
    gB_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2BiasT(bool write)
{
    const std::uint8_t b = withBits(r2b_, kRsp2BiasTOff, ch_.rsp2TunerParams.biasTEnable != 1);
    r2b_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2Antenna(bool write)
{
    int sel = raw(ch_.rsp2TunerParams.antennaSel);
    if (sel != sdrplay_api_Rsp2_ANTENNA_A && sel != sdrplay_api_Rsp2_ANTENNA_B) {
        ch_.rsp2TunerParams.antennaSel = static_cast<sdrplay_api_Rsp2_AntennaSelectT>(kRsp2AntennaNone);
        sel = kRsp2AntennaNone;
    }
    const std::uint8_t b = withField(r2a_, kRsp2AntennaMask, static_cast<std::uint8_t>(sel));
    r2a_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2RfNotch(bool write)
{
    const std::uint8_t b = withField(r2b_, kRsp2NotchMask,
                                     ch_.rsp2TunerParams.rfNotchEnable == 0 ? kRsp2NotchOffBits : kRsp2NotchOnBits);
    r2b_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioB, b); rc != 0) {
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
    const std::uint8_t b = withBits(r2a_, kRsp2ExtRefOff, en == 0);
    r2a_ = b;
    if (!write) return 0;
    if (const int rc = gpioWrite(kExpGpioA, b); rc != 0) {
        err(OPENRSP_HERE, fmt("GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rsp2AmPort()
{
    const int port = raw(ch_.rsp2TunerParams.amPortSel);
    if (port == sdrplay_api_Rsp2_AMPORT_2 || port == sdrplay_api_Rsp2_AMPORT_1) return 0;
    err(OPENRSP_HERE, fmt("port out of range[0:1] %d", port));
    return sdrplay_api_OutOfRange;
}

int Protocol::rspDuoExtRef(bool write)
{
    const std::uint8_t b = withBits(d1a_, kDuoExtRefOff, dev_.rspDuoParams.extRefOutputEn != 1);
    d1a_ = b;
    if (!write) return 0;
    if (const int rc = gpioDualWrite(0, kExpGpioA, b); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDuoTuner1AmNotch(int tuner, bool write)
{
    if (tuner == sdrplay_api_Tuner_A) {
        const std::uint8_t b =
            withBits(d2a_, kDuoTuner1AmNotchOff, ch_.rspDuoTunerParams.tuner1AmNotchEnable != 1);
        d2a_ = b;
        if (!write) return 0;
        if (const int rc = gpioDualWrite(1, kExpGpioA, b); rc != 0) {
            err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
            return sdrplay_api_HwError;
        }
        return 0;
    }
    err(OPENRSP_HERE, fmt("wrong tuner %d", tuner));
    return sdrplay_api_OutOfRange;
}

int Protocol::rspDuoBroadcastNotch(int tuner, bool write)
{
    if (tuner & sdrplay_api_Tuner_A)
        d2a_ = withBits(d2a_, kDuoBroadcastNotchAOff, ch_.rspDuoTunerParams.rfNotchEnable != 1);
    if (tuner & sdrplay_api_Tuner_B)
        d2a_ = withBits(d2a_, kDuoBroadcastNotchBOff, ch_.rspDuoTunerParams.rfNotchEnable != 1);
    if (!write) return 0;
    if (const int rc = gpioDualWrite(1, kExpGpioA, d2a_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDuoDabNotch(int tuner, bool write)
{
    if (tuner & sdrplay_api_Tuner_A)
        d2a_ = withBits(d2a_, kDuoDabNotchAOff, ch_.rspDuoTunerParams.rfDabNotchEnable != 1);
    if (tuner & sdrplay_api_Tuner_B)
        d2b_ = withBits(d2b_, kDuoDabNotchBOff, ch_.rspDuoTunerParams.rfDabNotchEnable != 1);
    if (!write) return 0;
    if (const int rc = gpioDualWrite(1, kExpGpioA, d2a_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    if (const int rc = gpioDualWrite(1, kExpGpioB, d2b_); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

int Protocol::rspDuoRfTest(bool write)
{
    const std::uint8_t b = withBits(d2a_, kDuoRfTestOff, internal_.rspDuoRfTestEn != 1);
    d2a_ = b;
    if (!write) return 0;
    if (const int rc = gpioDualWrite(1, kExpGpioA, b); rc != 0) {
        err(OPENRSP_HERE, fmt("dual GPIO write failed 0x%08X", rc));
        return sdrplay_api_HwError;
    }
    return 0;
}

// ------------------------------------------------------------------ InternalUpdate
int Protocol::internalUpdate(std::uint32_t reason)
{
    if (reason == 0) return 0;
    if (internalShared_) internal_ = *internalShared_;
    if (reason & sdrplay_api_InternalUpdate_Rsp1aRfTest) return rsp1aRfTest(true);
    if (reason & sdrplay_api_InternalUpdate_RspDuoRfTest) return rspDuoRfTest(true);
    return 0;
}

// ------------------------------------------------------------------ Update
int Protocol::update(int tuner, std::uint32_t reason, std::uint32_t ext1)
{
    if (state_ == kStateIdle) return sdrplay_api_NotInitialised;
    updStep_ = nullptr;
    curTuner_ = tuner;
    latchChannel();
    if (reason == 0 && ext1 == 0) return 0;
    latchDev();
    int r = 0;
    // Tuner_Gr requested, or implied by one of the steps below
    bool gainNeeded = (reason & sdrplay_api_Update_Tuner_Gr) != 0;

    if (reason & sdrplay_api_Update_Dev_Fs) {
        FsState st;
        r = rsp1b::fsState(dev_.fsFreq.fsHz, raw(ch_.tunerParams.ifType) != 0, raw(dev_.mode) == 1, st);
        if (r != 0) {
            err(OPENRSP_HERE, fmt("fsMHz out of range %.3f", dev_.fsFreq.fsHz / 1e6));
            updStep_ = "sample-rate state update";
            return r;
        }
        const double oldMix = mix_;
        if (st.spp == fs_.spp && st.D == fs_.D && st.V == fs_.V) {
            fs_.wrapThr = st.wrapThr;
            fs_.fsMHz = st.fsMHz;
            configureDownConverter();
            fsUser_ = true;
            r = step("sample rate", writeRate());
            if (r != 0) {
                fsUser_ = false;
                return r;
            }
        } else {
            stopStream();
            setRegister(kRegAdcPll, kAdcPllFsChange);
            fs_ = st;
            setInterface(fs_.alt);
            configureDownConverter();
            fsUser_ = true;
            if (setRegister(kRegPacketFormat, fs_.reg7) != 0) {
                fsUser_ = false;
                return step("sample-rate hardware update", sdrplay_api_HwError);
            }
            r = step("sample rate", writeRate());
            if (r != 0) {
                fsUser_ = false;
                return r;
            }
            gainNeeded = true;
            dsp_.streamRestarted = true;
            start(true);
        }
        // a new mix frequency reprograms the tuner IF unless another step reprograms the RF anyway
        const std::uint32_t rfProgrammed = sdrplay_api_Update_Rsp2_AmPortSelect | sdrplay_api_Update_Tuner_LoMode |
                                           sdrplay_api_Update_RspDuo_AmPortSelect;
        if (mix_ != oldMix && (reason & rfProgrammed) == 0) reason |= sdrplay_api_Update_Tuner_IfType;
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
        dropPendingUpdates();
        if (devShared_) {
            devShared_->resetFlags.resetGainUpdate = 0;
            devShared_->resetFlags.resetRfUpdate = 0;
            devShared_->resetFlags.resetFsUpdate = 0;
        }
    }
    // Rsp1a_* bits: the rsp1a* handlers (FE13 from the RSP1A shadow)
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
        r = step("RF frequency hardware update", programBand(false));
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
        r = step("external reference control", rspDuoExtRef(true));
        if (r != 0) return r;
    }
    if (ext1 & sdrplay_api_Update_RspDx_HdrEnable) reason |= sdrplay_api_Update_Tuner_Frf;
    {
        // the value of the last handler called wins; HdrBw (always 0) discards errors
        int dxr = 0;
        if (ext1 & sdrplay_api_Update_RspDx_BiasTControl) dxr = rspDxBiasT(true, false);
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
            gainNeeded = true;
            dxr = rspDxHdrBw();
        }
        if (dxr != 0) {
            // rspDxHdrBw always returns 0, so a failure is the last expander handler called
            updStep_ = (ext1 & sdrplay_api_Update_RspDx_RfDabNotchControl) ? "DAB notch"
                     : (ext1 & sdrplay_api_Update_RspDx_RfNotchControl)    ? "broadcast notch"
                     : (ext1 & sdrplay_api_Update_RspDx_AntennaControl)    ? "antenna control"
                                                                            : "bias-T control";
            return dxr;
        }
    }
    if (reason & sdrplay_api_Update_Tuner_GrLimits) {
        const int minGr = raw(ch_.tunerParams.gain.minGr);
        if (raw(ch_.ctrlParams.agc.enable) != 0 && minGr == 0) {
            errWarning(OPENRSP_HERE, fmt("Tuner%d: called while AGC enabled", curTuner_));
            return step("gain limits", sdrplay_api_NotEnabled);
        }
        if (minGr != 0 && minGr != 20) {
            err(OPENRSP_HERE,
                fmt("Tuner%d: parameter out of range[%d or %d] (minGr=%d)", curTuner_, 0, 20, minGr));
            return step("gain limits", sdrplay_api_OutOfRange);
        }
    }
    if (reason & sdrplay_api_Update_Tuner_Frf) {
        configureDownConverter();
        BandState nb;
        r = step("RF frequency state update", selectBand(nb));
        if (r != 0) return r;
        if (sameBand(nb, band_)) {
            rfUser_ = true;
            r = step("RF programming", writeRf());
            if (r != 0) {
                rfUser_ = false;
                return r;
            }
        } else {
            band_ = nb;
            firstLO_ = nb.b.firstLO;
            if (nb.b.loFlag) loFlag_ = true;
            gainNeeded = true;
            rfUser_ = true;
            r = step("RF frequency hardware update", programBand(false));
            if (r != 0) {
                rfUser_ = false;
                return r;
            }
        }
    }
    if (reason & sdrplay_api_Update_Tuner_BwType) {
        configureDownConverter();
        BandState nb;
        r = step("RF frequency state update", selectBand(nb));
        if (r != 0) return r;
        band_ = nb;
        firstLO_ = nb.b.firstLO;
        if (nb.b.loFlag) loFlag_ = true;
        r = step("RF frequency hardware update", programBand(true));
        if (r != 0) return r;
        gainNeeded = true;
    }
    if (reason & sdrplay_api_Update_Tuner_IfType) {
        configureDownConverter();
        BandState nb;
        r = step("RF frequency state update", selectBand(nb));
        if (r != 0) return r;
        band_ = nb;
        firstLO_ = nb.b.firstLO;
        if (nb.b.loFlag) loFlag_ = true;
        r = step("RF frequency hardware update", programBand(false));
        if (r != 0) return r;
        gainNeeded = true;
    }
    if (reason & sdrplay_api_Update_Tuner_DcOffset) {
        r = step("tuner DC offset", applyTunerDc());
        if (r != 0) return r;
    }
    if (reason & sdrplay_api_Update_Tuner_LoMode) {
        r = step("LO mode", applyLoMode());
        if (r != 0) return r;
        BandState nb;
        r = step("RF frequency state update", selectBand(nb));
        if (r != 0) return r;
        band_ = nb;
        firstLO_ = nb.b.firstLO;
        if (nb.b.loFlag) loFlag_ = true;
        r = step("RF frequency hardware update", programBand(false));
        if (r != 0) return r;
    }
    if (reason & sdrplay_api_Update_Ctrl_DCoffsetIQimbalance) configureDcIq();
    if (reason & sdrplay_api_Update_Ctrl_Decimation) updateDecimation();
    if (reason & sdrplay_api_Update_Ctrl_Agc) {
        r = step("AGC control", configureAgc());
        if (r != 0) return r;
    }
    if (reason & sdrplay_api_Update_Ctrl_OverloadMsgAck) dsp_.overloadAck = true;
    if (reason & sdrplay_api_Update_RspDuo_BiasTControl) {
        // log only
        if (tuner != sdrplay_api_Tuner_B) return step(common::kStepDuoBiasTWrongTuner, sdrplay_api_InvalidParam);
    }
    if (reason & sdrplay_api_Update_RspDuo_AmPortSelect) {
        const int port = raw(ch_.rspDuoTunerParams.tuner1AmPortSel);
        if (tuner == sdrplay_api_Tuner_B || port < 0 || port > 1) {
            // RSPduo AM port select (model-independent)
            err(OPENRSP_HERE, fmt("port out of range[0:1] %d or wrong tuner %d", port, tuner));
            return step("AM port select", sdrplay_api_OutOfRange);
        }
        return step("RF frequency hardware update", programBand(false));
    }
    if (reason & sdrplay_api_Update_RspDuo_Tuner1AmNotchControl) {
        r = step("tuner 1 AM notch", rspDuoTuner1AmNotch(tuner, true));
        if (r != 0) return r;
    }
    if (reason & sdrplay_api_Update_RspDuo_RfNotchControl) {
        r = step("broadcast notch", rspDuoBroadcastNotch(tuner, true));
        if (r != 0) return r;
    }
    if (reason & sdrplay_api_Update_RspDuo_RfDabNotchControl) {
        r = step("DAB notch", rspDuoDabNotch(tuner, true));
        if (r != 0) return r;
        gainNeeded = true;
    }
    if (ext1 & sdrplay_api_Update_RspDuo_ResetSlaveFlags) {
        if (tuner != sdrplay_api_Tuner_Neither) return step("slave update flags reset", sdrplay_api_InvalidMode);
        if (rxShared_) {
            rxShared_->rspDuoTunerParams.resetSlaveFlags.resetGainUpdate = 0;
            rxShared_->rspDuoTunerParams.resetSlaveFlags.resetRfUpdate = 0;
        }
    }
    if (!gainNeeded) return 0;

    // final Gr: AGC off or HDR -> writeGain directly
    if (raw(ch_.ctrlParams.agc.enable) == 0 || band_.hdrActive) {
        grUserFlag_ = true;
        r = step(common::kStepProgramGr, writeGain());
        if (r != 0) grUserFlag_ = false;
        return r;
    }
    const int lna = ch_.tunerParams.gain.LNAstate;
    if (!lnaValid(band_.b.band, band_.b.dxSub, lna)) {
        err(OPENRSP_HERE, fmt("Tuner%d: LNAstate out of range LNAstate=%d", curTuner_, lna));
        return sdrplay_api_OutOfRange;
    }
    if (ch_.tunerParams.gain.syncUpdate >= 2) {
        err(OPENRSP_HERE,
            fmt("Tuner%d: Gain syncUpdate out of range (%d:%d) = %d", curTuner_, 0, 1, ch_.tunerParams.gain.syncUpdate));
        return sdrplay_api_OutOfRange;
    }
    dsp_.agcForced = true;
    dsp_.agcUserRequest = true;
    return 0;
}

} // namespace rspdx
} // namespace device
} // namespace sdrplay
