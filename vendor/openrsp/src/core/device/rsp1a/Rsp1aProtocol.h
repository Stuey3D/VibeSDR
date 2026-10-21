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

// RSP1A (hwVer 255) / RSP1 (hwVer 1) hardware protocol engine (pure, portable).
//
// These two models use the RSP1B engine's sequences (core/device/rsp1b/Rsp1bProtocol.*)
// with these model differences:
//   - bridge GPIO configuration: RSP1A W08 = 0xEE80 (Init only); RSP1 W08 = 0xF280 | sub<<8
//     at Init AND inside every band program, right after r0
//   - front end: RSP1A expander bit map (A 7:5 attenuator, A 4:0 band code; B bit 1 pad,
//     B 0/2/4 DAB notch / broadcast notch / bias-T, B 3/5 RF test);
//     RSP1 has no expander path in initHardware, band program, programGain or Uninit
//   - LNA tables, validation and clamps, gainVals tables/groups/DAB predicate
//   - Update Rsp1a_BiasTControl / Rsp1a_RfNotchControl / Rsp1a_RfDabNotchControl and
//     InternalUpdate_Rsp1aRfTest use the rsp1a* functions on both
//     models (the RSP1 receives FE13 writes too)
// Everything else (bridge, ADC PLL, tuner PLL, band state, sync update, DSP requests,
// other models' reason bits, failure paths) sends the same control-transfer sequence as
// the RSP1B engine.
//
// It never touches a device: every hardware action goes through the Io sink (the
// RSP1B Io interface, shared).
//
// Threading: as the RSP1B engine (not internally locked; callers hold the engine mutex).
#pragma once

#include "core/api/ApiTypes.h"
#include "core/device/common/CommonRspProtocol.h"
#include "core/device/rsp1a/Rsp1aCalc.h"
#include "core/device/rsp1b/Rsp1bProtocol.h"

#include <cstdint>
#include <string>

namespace sdrplay {
namespace device {
namespace rsp1a {

// Shared with the RSP1B engine (same wire primitives and sink).
using rsp1b::Change;
using rsp1b::ControlTransfer;
using rsp1b::DspRequests;
using rsp1b::ErrorRecord;
using rsp1b::LogAs;
using rsp1b::GainChangeEvent;
using rsp1b::Io;
using rsp1b::StreamOp;
using rsp1b::ToggleBits;
using rsp1b::kReqFnAddr;
using rsp1b::kReqGpioDual0;
using rsp1b::kReqGpioDual1;
using rsp1b::kReqGpioExt;
using rsp1b::kReqReadReg;
using rsp1b::kReqSetInterface;
using rsp1b::kReqStart;
using rsp1b::kReqStop;
using rsp1b::kReqWriteReg;

class Protocol : public common::ProtocolBase {
public:
    // hwVer: 255 (RSP1A) or 1 (RSP1); any other value is treated as the RSP1A.
    Protocol(std::uint8_t hwVer, sdrplay_api_DevParamsT *dev, sdrplay_api_RxChannelParamsT *rx,
             sdrplay_api_InternalRfTestT *internal, Io &io);

    Model model() const { return model_; }

    // SelectDevice: device-object reset (all shadows 0, first Init after Select
    // pending). No USB traffic.
    void select();

    int init();
    int uninit() { return uninitImpl(true); }
    int update(int tuner, std::uint32_t reason, std::uint32_t ext1);
    int internalUpdate(std::uint32_t reason);
    void release();

    // AGC hand-off: programGain(gRdB) without the API validation (configureAgc path).
    int applyAgcGain(int gRdB);

    void onPacketToggles(const ToggleBits &t);
    int onPacketSampleNum(std::uint32_t sampleNum);
    void confirmAllPending(); // test helper

    // ---- state for the service / DSP side
    bool initialised() const { return state_ != common::kStateIdle; }
    int state() const { return state_; }
    const FsState &fsState() const { return fs_; }
    const BandState &band() const { return band_; }
    DownConv downConv() const;
    int userDecimShift() const { return decimShift_; }
    double mixHz() const { return mix_; }
    std::uint8_t shadowA() const { return gpio_.a; }
    std::uint8_t shadowB() const { return gpio_.b; }
    const rsp1b::hw::GpioShadows &shadows() const { return gpio_; }
    int appliedGr() const { return appliedGr_; }
    int totalGr() const { return totalGr_; }
    bool grPending() const { return grPending_; }
    DspRequests takeDspRequests();
    const DspRequests &dspRequests() const { return dsp_; }

private:
    // primitives
    int ctl(std::uint8_t bmRT, std::uint8_t req, std::uint16_t wValue, std::uint16_t wIndex, std::uint16_t wLength,
            std::uint8_t *in = nullptr);
    int setRegister(std::uint8_t regByte, std::uint32_t value);
    int tunerWrite(std::uint32_t word, std::uint8_t syncFlag = 0)
    {
        return setRegister(static_cast<std::uint8_t>((syncFlag & 7) << 5 | rsp1b::hw::kRegTuner), word);
    }
    int gpioWrite(std::uint8_t reg, std::uint8_t val);
    int gpioDualWrite(int devId, std::uint8_t reg, std::uint8_t val);
    int setInterface(std::uint8_t alt);
    int startRx();
    int stopRx();
    void err(const char *file, const char *fn, int line, std::string msg, LogAs as = LogAs::Error);
    void errWarning(const char *file, const char *fn, int line, std::string msg)
    {
        err(file, fn, line, std::move(msg), LogAs::Warning);
    }
    void errVerbose(const char *file, const char *fn, int line, std::string msg)
    {
        err(file, fn, line, std::move(msg), LogAs::Verbose);
    }
    void errQuiet(const char *file, const char *fn, int line, std::string msg)
    {
        err(file, fn, line, std::move(msg), LogAs::None);
    }

    // engine routines
    int uninitImpl(bool apiCall);
    void latchDev();
    void latchChannel();
    int writeBridgeGpio();
    int bridgeInit();
    void bridgeUninit();
    int initHardware();
    int start(bool toggles);
    void stopStream();
    int resyncToggles();
    int programSampleRate();
    int writeRate();
    int programRf();
    int writeRf();
    int programGain(bool fromAgc);
    int writeGain();
    int applyTunerDc();
    int applyLoMode();
    int selectBand(BandState &out);
    // Band program; bwUpdate as in rsp1b::Protocol::programBand.
    int programBand(bool bwUpdate);
    int readTunerTrims(std::uint32_t &v);
    void dropPendingUpdates();
    int programSyncUpdate();
    int applyPpm();
    int configureAgc();
    void updateDecimation();
    void configureDcIq();
    void configureDownConverter();
    void computeGainVals(int gApplied, int lnaState, GainVals &gv) const;

    // RSP1A front end (shadow B = GPIOB 0x13); also run on the RSP1 (no model check)
    int rsp1aBiasT(bool write, bool forceOff);
    int rsp1aDabNotch(bool write);
    int rsp1aBroadcastNotch(bool write);
    int rsp1aRfTest(bool write);
    // other models' handlers (run for every model, no model check)
    int rsp2BiasT(bool write);
    int rsp2Antenna(bool write);
    int rsp2RfNotch(bool write);
    int rsp2ExtRef(bool write);
    int rsp2AmPort();
    int rspDxBiasT(bool write);
    int rspDxAntenna(bool write);
    int rspDxBroadcastNotch(bool write);
    int rspDxDabNotch();
    int rspDuoExtRef(bool write);
    int rspDuoTuner1AmNotch(int tuner, bool write);
    int rspDuoBroadcastNotch(int tuner, bool write);
    int rspDuoDabNotch(int tuner, bool write);
    int rspDuoRfTest(bool write);

    Model model_;
    sdrplay_api_DevParamsT *devShared_;
    sdrplay_api_RxChannelParamsT *rxShared_;
    sdrplay_api_InternalRfTestT *internalShared_;
    Io &io_;

    sdrplay_api_DevParamsT dev_{};
    sdrplay_api_RxChannelParamsT ch_{};
    sdrplay_api_InternalRfTestT internal_{};

    common::ProtocolState state_ = common::kStateIdle;
    bool bridgeOpen_ = false;
    bool fnAddrDone_ = false;
    rsp1b::hw::GpioShadows gpio_{};

    FsState fs_{};
    BandState band_{};
    int firstLO_ = 0;
    bool loFlag_ = false;
    double mix_ = 0.0;
    double fSynth_ = 0.0;
    std::uint8_t dcCalApplied_ = 0, speedUpApplied_ = 0;
    int appliedGr_ = -1;
    int totalGr_ = 0;
    int decimShift_ = 0;
    bool rfToggle_ = false, fsToggle_ = false, grToggle_ = false;
    bool rfPending_ = false, fsPending_ = false, grPending_ = false;
    std::uint32_t syncPeriod_ = 0;
    static constexpr std::uint32_t kNoSyncDeferred = 0xFFFFFFFFu;
    std::uint32_t syncDeferred_ = kNoSyncDeferred; // sample number of a deferred sync update
    bool grUserFlag_ = false;
    bool rfUser_ = false, fsUser_ = false; // report request for the next programRf / programSampleRate
    int dcIfType_ = 0; // down-converter IF of the last configureDownConverter as an ifType code (0 / 2048 / 1620 / 450 / -1)
    DspRequests dsp_{};
    int agcEnableState_ = 0;
    int curTuner_ = 1;
};

} // namespace rsp1a
} // namespace device
} // namespace sdrplay
