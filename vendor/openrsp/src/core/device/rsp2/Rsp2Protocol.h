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

// RSP2 / RSP2pro (hwVer 2) hardware protocol engine (pure, portable).
//
// Same structure and contract as rsp1b::Protocol (it reuses that engine's Io
// sink, transfer and event types): it issues the control-transfer sequences the
// device needs for Init (state reset ->
// initHardware -> start), Uninit, Update (every reason bit, including other models' bits,
// which are executed on an RSP2 without a model check), InternalUpdate, Release
// and the AGC gain hand-off, plus the RSP2 front-end shadows R2A/R2B
// and the other models' shadows the foreign bits write to the
// same expander.
//
// Only the RSP2 differences from the RSP1B engine are new here: LNA
// table/validation and r1 bits, FE13-only programGain, the RSP2 GPIO block
// of the band program, the AM (Hi-Z) port r0 mode, antenna / ext-ref / bias-T /
// RF-notch handlers, Uninit bias-T via FE13, gainVals (10-float rows, no DAB
// correction) and the Init LNA limit (0:8).
//
// It never touches a device: every hardware action goes through the Io sink.
#pragma once

#include "core/api/ApiTypes.h"
#include "core/device/common/CommonRspProtocol.h"
#include "core/device/rsp1b/Rsp1bProtocol.h"
#include "core/device/rsp2/Rsp2Calc.h"

#include <cstdint>
#include <string>

namespace sdrplay {
namespace device {
namespace rsp2 {

// The engine-to-service seams are shared with the RSP1B engine.
using rsp1b::Change;
using rsp1b::ControlTransfer;
using rsp1b::DspRequests;
using rsp1b::ErrorRecord;
using rsp1b::LogAs;
using rsp1b::GainChangeEvent;
using rsp1b::Io;
using rsp1b::StreamOp;
using rsp1b::ToggleBits;
using rsp1b::FsState;
using rsp1b::DownConv;

class Protocol : public common::ProtocolBase {
public:
    Protocol(sdrplay_api_DevParamsT *dev, sdrplay_api_RxChannelParamsT *rx, sdrplay_api_InternalRfTestT *internal,
             Io &io);

    // SelectDevice: device-object reset (all shadows 0, first Init after Select
    // pending). No USB traffic.
    void select();

    int init();
    int uninit() { return uninitImpl(true); }
    int update(int tuner, std::uint32_t reason, std::uint32_t ext1);
    int internalUpdate(std::uint32_t reason);
    void release();
    int applyAgcGain(int gRdB);

    void onPacketToggles(const ToggleBits &t);
    int onPacketSampleNum(std::uint32_t sampleNum);
    void confirmAllPending();

    bool initialised() const { return state_ != common::kStateIdle; }
    int state() const { return state_; }
    const FsState &fsState() const { return fs_; }
    const BandState &band() const { return band_; }
    DownConv downConv() const;
    int userDecimShift() const { return decimShift_; }
    double mixHz() const { return mix_; }
    // front-end shadows
    const rsp1b::hw::GpioShadows &shadows() const { return gpio_; }
    std::uint8_t shadowR2A() const { return gpio_.rsp2A; }
    std::uint8_t shadowR2B() const { return gpio_.rsp2B; }
    int appliedGr() const { return appliedGr_; }
    int totalGr() const { return totalGr_; }
    DspRequests takeDspRequests();
    const DspRequests &dspRequests() const { return dsp_; }

private:
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

    int uninitImpl(bool apiCall);
    void latchDev();
    void latchChannel();
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
    int amPortSel() const;

    // RSP2 front end (R2A = GPIOA 0x12, R2B = GPIOB 0x13)
    int rsp2BiasT(bool write, bool force);
    int rsp2Antenna(bool write);
    int rsp2RfNotch(bool write);
    int rsp2ExtRef(bool write);
    int rsp2AmPort();
    // other models' handlers (run without a model check, for compatibility)
    int rsp1aBiasT(bool write);
    int rsp1aBroadcastNotch(bool write);
    int rsp1aDabNotch(bool write);
    int rsp1aRfTest(bool write);
    int rspDxBiasT(bool write);
    int rspDxAntenna(bool write);
    int rspDxBroadcastNotch(bool write);
    int rspDxDabNotch();
    int rspDuoExtRef(bool write);
    int rspDuoTuner1AmNotch(int tuner, bool write);
    int rspDuoBroadcastNotch(int tuner, bool write);
    int rspDuoDabNotch(int tuner, bool write);
    int rspDuoRfTest(bool write);

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
    int dcIfType_ = 0; // r0 IF code of the last configureDownConverter (0 / 2048 / 1620 / 450 / -1)
    DspRequests dsp_{};
    int agcEnableState_ = 0;
    int curTuner_ = 1;
};

} // namespace rsp2
} // namespace device
} // namespace sdrplay
