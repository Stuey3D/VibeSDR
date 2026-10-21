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

// RSPduo hardware protocol engine (pure, portable): the service's
// control-transfer sequences for every RSPduo mode - Single tuner
// (A or B), Dual tuner, Master and Slave - including the slave-side Init /
// Uninit / Update, the master's SwapTuner (cmd 10) and ChangeSampleRate
// (cmd 11), the two MCP23017-like GPIO expanders (kReqGpioDual0/1), the
// tuner SPI chip-select, the shared single-toggle header
// bookkeeping (two-slot gain queue) and the GainChange / RspDuoModeChange /
// PowerOverloadChange events.
//
// It follows the RSP1B engine (core/device/rsp1b/Rsp1bProtocol.h) and reuses
// its transport-neutral types (ControlTransfer, Io, ToggleBits, ...). Every
// hardware action goes through the Io sink; reads (FNADDR, tuner trims,
// first-block toggles) are inputs from it. Nothing here touches a device.
//
// Threading: not internally locked (the service holds its API and gain mutexes around
// every entry point).
#pragma once

#include "core/api/ApiTypes.h"
#include "core/device/common/CommonRspProtocol.h"
#include "core/device/rsp1b/Rsp1bProtocol.h"
#include "core/device/rspduo/RspDuoCalc.h"

#include <cstdint>
#include <string>

namespace sdrplay {
namespace device {
namespace rspduo {

using rsp1b::Change;
using rsp1b::ControlTransfer;
using rsp1b::DspRequests;
using rsp1b::ErrorRecord;
using rsp1b::LogAs;
using rsp1b::GainChangeEvent;
using rsp1b::StreamOp;
using rsp1b::ToggleBits;

class Io : public rsp1b::Io {
public:
    // Event (RspDuoModeChange, tuner, {type}), type an sdrplay_api_RspDuoModeCbEventIdT.
    // The service routes it to sEvent when tuner == the slave tuner, else to mEvent.
    virtual void modeChange(int type, int tuner) { (void)type; (void)tuner; }
};

struct Config {
    sdrplay_api_DevParamsT *dev = nullptr;          // master map devParams; NULL for a slave map
    sdrplay_api_RxChannelParamsT *rxA = nullptr;    // channel wiring
    sdrplay_api_RxChannelParamsT *rxB = nullptr;
    sdrplay_api_InternalRfTestT *internal = nullptr;
    int mode = sdrplay_api_RspDuoMode_Single_Tuner; // DeviceT.rspDuoMode at master select
    int tuner = 1;                                  // DeviceT.tuner
    double rspDuoSampleFreq = 0.0;                  // DeviceT.rspDuoSampleFreq
};

// Gain confirmations produced by one packet header.
struct ToggleResult {
    int grConfirmedMask = 0;   // tuners whose gain update was confirmed (bit 0 A, bit 1 B)
    int grReportMask = 0;      // ... with grChanged reported
    bool grNotify = false;     // AGC wait flag for the confirmed slot
    int grValue = 0;
    int rfConfirmedMask = 0;
    bool rfReport = false;
    bool fsConfirmed = false;
    bool fsReport = false;
};

class Protocol : public common::ProtocolBase {
public:
    Protocol(const Config &cfg, Io &io);

    // ---- configuration (service side)
    // Channel wiring: finishSetup / cmd 10 SwapTuner rewire the shared param sources.
    void setChannelSources(sdrplay_api_RxChannelParamsT *rxA, sdrplay_api_RxChannelParamsT *rxB);
    void setDevSource(sdrplay_api_DevParamsT *dev) { devShared_ = dev; }
    // cmd 10 SwapTuner: tuner = t and the wiring (t == 1: chA <- master rxChannelA,
    // chB <- slave rxChannelB; else chA <- slave rxChannelA, chB <- master rxChannelB).
    // The slave tuner is NOT updated.
    void swapTuner(int newTuner, sdrplay_api_RxChannelParamsT *rxA, sdrplay_api_RxChannelParamsT *rxB);
    // cmd 11 ChangeSampleRate: rspDuoSampleFreq = fs, fs8192 = (fs == 8.192e6).
    void changeSampleRate(double fs);

    int mode() const { return mode_; }
    int tuner() const { return tuner_; }
    int slaveTuner() const { return slaveTuner_; }
    bool fs8192() const { return fs8192_; }
    double rspDuoSampleFreq() const { return rspDuoSampleFreq_; }

    // SelectDevice (master/single/dual open): device-object reset. No USB traffic.
    void select();

    // ---- master / single / dual API (sdrplay_api_ErrT values)
    int init();
    int uninit() { return uninitImpl(true); }
    // fromMaster = false for the slave's cmd 12 (device-level bits skipped).
    int update(int tuner, std::uint32_t reason, std::uint32_t ext1, bool fromMaster = true);
    int internalUpdate(std::uint32_t reason);
    void release();

    // ---- slave API
    // Slave SelectDevice of an entry held by a master: SlaveAttached event to the master tuner.
    void slaveAttached();
    int initSlave();       // cmd 8: sdrplay_api_StartPending while the master is not streaming
    int uninitSlave();     // cmd 9; also the locked slave Uninit
    // Slave ReleaseDevice / heartbeat loss with the master alive: (the service resets the
    // slave map to defaults) and SlaveDetached [+ SlaveDllDisappeared] to the master.
    void slaveDetached(bool dllDisappeared);
    // Master heartbeat lost with the slave alive: Uninit + MasterDllDisappeared.
    void masterDisappeared();
    bool slaveInitialised() const { return slaveInit_; }
    bool slaveStartPending() const { return slaveStartPending_; }
    bool stopPending() const { return stopPending_; }

    // ---- AGC hand-off (programGain from the DSP thread, no API validation)
    int applyAgcGain(int tuner, int gRdB);

    // ---- stream-thread hooks
    ToggleResult onPacketToggles(const ToggleBits &t);
    void confirmAllPending();

    // ---- state for the service / DSP / tests
    bool initialised() const { return state_ != common::kStateIdle; }
    int state() const { return state_; }
    const FsState &fsState() const { return fs_; }
    const BandState &band(int ch) const { return ch_[ch & 1].band; }
    DownConv downConv(int ch) const;
    int userDecimShift(int ch) const { return ch_[ch & 1].decimShift; }
    double mixHz(int ch) const { return ch_[ch & 1].mix; }
    double fSynthHz(int ch) const { return ch_[ch & 1].fSynth; }
    int appliedGr(int ch) const { return ch_[ch & 1].appliedGr; }
    int totalGr(int ch) const { return ch_[ch & 1].totalGr; }
    const sdrplay_api_RxChannelParamsT &channel(int ch) const { return ch_[ch & 1].p; }
    std::uint8_t d1a() const { return d1a_; }
    std::uint8_t d1b() const { return d1b_; }
    std::uint8_t d2a() const { return d2a_; }
    std::uint8_t d2b() const { return d2b_; }
    int tunerSelCache() const { return tunerSel_; }
    bool dualFlag() const { return dual_; }
    int grSlot1() const { return grSlot1_; }
    int grSlot2() const { return grSlot2_; }
    int rfSlot() const { return rfSlot_; }
    bool rfPending() const { return rfPending_; }
    bool fsPending() const { return fsPending_; }
    DspRequests takeDspRequests();
    const DspRequests &dspRequests() const { return dsp_; }

private:

    struct Chan {
        sdrplay_api_RxChannelParamsT p{};   // latched params
        BandState band{};
        int firstLO = 0;
        bool loFlag = false;
        double mix = 0.0;                   // down-converter mix frequency
        double fSynth = 0.0;
        std::uint8_t dcCalApplied = 0, speedUpApplied = 0;
        int appliedGr = -1;                 // -1: no gain applied yet
        int totalGr = 0;
        int decimShift = 0;
        int agcEnableState = 0;
    };

    static int chIdx(int tuner) { return tuner == 2 ? 1 : 0; }
    sdrplay_api_RxChannelParamsT *shared(int tuner) const { return tuner == 2 ? rxShared_[1] : rxShared_[0]; }

    // primitives
    int ctl(std::uint8_t bmRT, std::uint8_t req, std::uint16_t wValue, std::uint16_t wIndex, std::uint16_t wLength,
            std::uint8_t *in = nullptr);
    int setRegister(std::uint8_t regByte, std::uint32_t value);
    int tunerWrite(std::uint32_t word, std::uint8_t syncFlag = 0)
    {
        return setRegister(static_cast<std::uint8_t>((syncFlag & 7) << 5 | rsp1b::hw::kRegTuner), word);
    }
    int gpioWrite(std::uint8_t reg, std::uint8_t val); // request 0x49 (other models' handlers)
    int dualWrite(int expander, std::uint8_t reg, std::uint8_t val);
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

    // Init / Update / Uninit building blocks
    int uninitImpl(bool apiCall);
    int validateDevParams();
    int validateTuner(int tuner);
    void latchDev();
    void latchChannel(int idx, const sdrplay_api_RxChannelParamsT *src);
    void channelInit(int idx);          // down-converter / gain state reset
    int initChannelState(int tuner, bool master);
    int bridgeInit();
    void bridgeUninit();
    int initHardware(int tuner, bool master);
    int start(bool toggles);
    void stopStream();
    int resyncToggles();
    int tunerSelect(int tuner);
    void expanderInit(int tuner);       // GPIO expander initial writes
    void channelConfig(int cfg);        // I/Q channel config (shadow only)
    int programSampleRate();
    int writeRate();
    int programRf(int tuner);
    int writeRf(int tuner);
    int programGain(int tuner, bool fromAgc, bool notify);
    // Slot 1 is free, or (Dual mode) slot 2 can queue a second update
    bool gainSlotFree() const
    {
        return grSlot1_ == 0 || (mode_ == sdrplay_api_RspDuoMode_Dual_Tuner && grSlot2_ == 0);
    }
    int writeGain(int tuner);
    int applyTunerDc(int tuner);
    int applyLoMode(int tuner);
    int selectBand(int tuner, BandState &out, int &firstLO);
    // bwChange: Tuner_BwType update (LO code path, RF write only)
    int programBand(int tuner, bool bwChange);
    int readTunerTrims(std::uint32_t &v);
    int dropPendingUpdates(int tuner);
    int dropSlavePendingUpdates(int tuner);
    int programSyncUpdate();
    int applyPpm();
    int configureAgc(int tuner);
    void updateDecimation(int tuner);
    void configureDcIq(int tuner);
    void configureDownConverter(int tuner);
    int refreshGainVals(int tuner);
    void writeGainBack(int tuner);      // channel gain struct -> shared

    // RSPduo front end
    int extRef(bool write);
    int rfTest(bool write);
    int biasT(int tuner, bool write, bool forceOff);
    int tuner1AmNotch(int tuner, bool write);
    int broadcastNotch(int tuner, bool write);
    int dabNotch(int tuner, bool write);
    int amPortSelect(int tuner);
    // other models' handlers that Update runs on an RSPduo (request 0x49)
    int rsp1aBiasT(bool write, bool forceOff);
    int rsp1aBroadcastNotch(bool write);
    int rsp1aDabNotch(bool write);
    int rsp2BiasT(bool write);
    int rsp2Antenna(bool write);
    int rsp2RfNotch(bool write);
    int rsp2ExtRef(bool write);
    int rsp2AmPort();
    int rspDxBiasT(bool write);
    int rspDxAntenna(bool write);
    int rspDxBroadcastNotch(bool write);
    int rspDxDabNotch();

    // shared parameter sources
    sdrplay_api_DevParamsT *devShared_;
    sdrplay_api_RxChannelParamsT *rxShared_[2];
    sdrplay_api_InternalRfTestT *internalShared_;
    Io &io_;

    // device identity
    int mode_;
    int tuner_;
    int slaveTuner_;
    double rspDuoSampleFreq_;
    bool fs8192_;

    // latched copies
    sdrplay_api_DevParamsT dev_{};
    sdrplay_api_InternalRfTestT internal_{};
    Chan ch_[2];

    int state_ = common::kStateIdle;
    bool dual_ = false;
    bool bridgeOpen_ = false;
    bool fnAddrDone_ = false;
    int tunerSel_ = 0;                // tuner the chip select currently points at
    // expander shadows: B (RSP1A-style port B, kReqGpioExt), R2A, R2B (RSP2), D1A, D1B, D2A, D2B
    // (RSPduo expanders 0 and 1), DX (RSPdx port A)
    std::uint8_t gB_ = 0, r2a_ = 0, r2b_ = 0, d1a_ = 0, d1b_ = 0, d2a_ = 0, d2b_ = 0, dx_ = 0;

    FsState fs_{};
    // toggles / pending
    bool rfToggle_ = false, rfPending_ = false;
    int rfSlot_ = 0;
    bool rfReportReq_ = false;
    bool rfReport_ = false;
    bool fsToggle_ = false, fsPending_ = false;
    bool fsReportReq_ = false;
    bool fsReport_ = false;
    bool grToggle_ = false;
    int grValue_ = 0;
    int grSlot1_ = 0, grSlot2_ = 0;   // tuner masks
    int grRep1_ = 0, grRep2_ = 0;
    bool grNote1_ = false, grNote2_ = false;
    bool grUserFlag_ = false;
    std::uint32_t syncPeriod_ = 0;

    // master/slave
    bool slaveInit_ = false;
    bool slaveStartPending_ = false;
    bool stopPending_ = false;

    DspRequests dsp_{};
};

} // namespace rspduo
} // namespace device
} // namespace sdrplay
