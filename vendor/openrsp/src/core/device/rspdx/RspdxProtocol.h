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

// RSPdx (hwVer 4) / RSPdx-R2 (hwVer 7) hardware protocol engine (pure,
// portable). It runs the RSP1B engine's sequences (core/device/rsp1b/Rsp1bProtocol)
// with these RSPdx differences:
//
//   writeBridgeGpio  reg8 0xEA80; rspDxHdr re-writes reg8 (bit 0x400 = HDR) at the
//                    end of every band program
//   initHardware     rspDx bias-T / notch / DAB (writes FE12) / antenna / HDR bandwidth
//   band program     DX expander XA/XB bits; in HDR only writeRf
//   programGain      28-state DX table, FE13 before FE12, XA pad; in HDR the
//                    IFGR re-write, 22-state HDR table, XA bit 0
//   programRf        HDR: no synthesiser write, Frf recorded for the DSP
//   gain values      DX / R2 tables, DX DAB table, antenna-C term, HDR group
//   Uninit           bias-T off, then XA |= 0x20
//   Update           Rsp1a bits go to the rsp1a* handlers (FE13 from the RSP1A
//                    shadow), ext1 RspDx bits with a fixed error precedence,
//                    DX LNA rule with AGC on, HDR bypasses the AGC
//
// It reuses the RSP1B engine's I/O seam (rsp1b::Io, ControlTransfer, ...), so the
// service wiring is shared.
#pragma once

#include "core/api/ApiTypes.h"
#include "core/device/common/CommonRspProtocol.h"
#include "core/device/rsp1b/Rsp1bProtocol.h"
#include "core/device/rspdx/RspdxCalc.h"

#include <cstdint>
#include <string>

namespace sdrplay {
namespace device {
namespace rspdx {

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
    // hwVer: 4 (RSPdx) or 7 (RSPdx-R2); it selects the gain-values table only.
    Protocol(std::uint8_t hwVer, sdrplay_api_DevParamsT *dev, sdrplay_api_RxChannelParamsT *rx,
             sdrplay_api_InternalRfTestT *internal, Io &io);

    // SelectDevice: device-object reset (first Init after Select pending). No USB traffic.
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
    void confirmAllPending();

    // ---- state for the service / DSP side
    std::uint8_t hwVer() const { return hwVer_; }
    bool initialised() const { return state_ != common::kStateIdle; }
    int state() const { return state_; }
    const FsState &fsState() const { return fs_; }
    const BandState &band() const { return band_; }
    // configureDownConverter result (latched): the HDR
    // configuration while the DSP is in the HDR path.
    DownConv downConv() const { return dc_; }
    bool hdrDsp() const { return dcHdr_; }        // down-converter in the HDR path (LIF via HDR)
    bool hdrActive() const { return band_.hdrActive; } // RF front end in HDR
    std::int32_t frf() const { return frf_; }
    int hdrBw() const;                             // as the stream thread reads it
    int userDecimShift() const { return decimShift_; }
    double mixHz() const { return mix_; }
    std::uint8_t shadowA() const { return xa_; }        // DX expander GPIOA
    std::uint8_t shadowB() const { return xb_; }        // DX expander GPIOB
    std::uint8_t rsp1aShadowB() const { return gB_; }   // RSP1A-style expander port B
    std::uint32_t reg8() const { return reg8_; }
    std::uint32_t r1Payload() const { return r1Payload_; }
    int appliedGr() const { return appliedGr_; }
    int totalGrValue() const { return totalGr_; }
    DspRequests takeDspRequests();
    const DspRequests &dspRequests() const { return dsp_; }

private:
    static constexpr std::uint32_t kNoSyncDeferred = 0xFFFFFFFFu;

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
    int programGainHdr(bool fromAgc);
    int writeGain();
    int applyTunerDc();
    int applyLoMode();
    int selectBand(BandState &out);
    // bwChange: Tuner_BwType update (LO code path, RF write only)
    int programBand(bool bwChange);
    int rspDxHdr();
    int readTunerTrims(std::uint32_t &v);
    void dropPendingUpdates();
    int programSyncUpdate();
    int applyPpm();
    int configureAgc();
    void updateDecimation();
    void configureDcIq();
    void configureDownConverter();
    bool hdrActiveLive() const; // hdrActive test on the latched params
    GainVals computeGainVals(int gApplied, int lnaState) const;
    void finishGr(int gApplied, int tuner); // gain values + shared copy + GainChange

    // RSPdx front end (XA = GPIOA 0x12, XB = GPIOB 0x13)
    int rspDxBiasT(bool write, bool force);
    int rspDxAntenna(bool write);
    int rspDxBroadcastNotch(bool write);
    int rspDxDabNotch();
    int rspDxHdrBw();
    // other models' handlers reached through foreign reason bits (no model check)
    int rsp1aBiasT(bool write, bool force);
    int rsp1aBroadcastNotch(bool write);
    int rsp1aDabNotch(bool write);
    int rsp1aRfTest(bool write);
    int rsp2BiasT(bool write);
    int rsp2Antenna(bool write);
    int rsp2RfNotch(bool write);
    int rsp2ExtRef(bool write);
    int rsp2AmPort();
    int rspDuoExtRef(bool write);
    int rspDuoTuner1AmNotch(int tuner, bool write);
    int rspDuoBroadcastNotch(int tuner, bool write);
    int rspDuoDabNotch(int tuner, bool write);
    int rspDuoRfTest(bool write);

    std::uint8_t hwVer_;
    Variant variant_;
    sdrplay_api_DevParamsT *devShared_;
    sdrplay_api_RxChannelParamsT *rxShared_;
    sdrplay_api_InternalRfTestT *internalShared_;
    Io &io_;

    sdrplay_api_DevParamsT dev_{};
    sdrplay_api_RxChannelParamsT ch_{};
    sdrplay_api_InternalRfTestT internal_{};

    int state_ = common::kStateIdle;
    bool bridgeOpen_ = false;
    bool fnAddrDone_ = false;
    // GPIO expander shadows: the RSP1A-style port B (request 0x49), RSP2 ports A/B, RSPduo
    // expander 0 port A and expander 1 ports A/B (foreign handlers), and the DX expander XA/XB
    std::uint8_t gB_ = 0, r2a_ = 0, r2b_ = 0, d1a_ = 0, d2a_ = 0, d2b_ = 0, xa_ = 0, xb_ = 0;
    std::uint32_t reg8_ = 0;          // last reg 8 value written

    FsState fs_{};
    BandState band_{};
    int firstLO_ = 0;
    bool loFlag_ = false;
    double mix_ = 0.0;
    double fSynth_ = 0.0;
    DownConv dc_{};
    bool dcHdr_ = false;
    std::int32_t frf_ = 0;
    std::uint32_t r1Payload_ = 0;     // 0 after SelectDevice, kept across Init
    std::uint8_t dcCalApplied_ = 0, speedUpApplied_ = 0;
    int appliedGr_ = -1;
    int totalGr_ = 0;
    int decimShift_ = 0;
    bool rfToggle_ = false, fsToggle_ = false, grToggle_ = false;
    bool rfPending_ = false, fsPending_ = false, grPending_ = false;
    std::uint32_t syncPeriod_ = 0;
    std::uint32_t syncDeferred_ = kNoSyncDeferred;
    bool grUserFlag_ = false;
    bool rfUser_ = false, fsUser_ = false; // report request for the next programRf / programSampleRate
    int dcIfType_ = 0; // ifType code of the last configureDownConverter (0 / 2048 / 1620 / 450 / -1)
    DspRequests dsp_{};
    int agcEnableState_ = 0;
    int curTuner_ = 1;
};

} // namespace rspdx
} // namespace device
} // namespace sdrplay
