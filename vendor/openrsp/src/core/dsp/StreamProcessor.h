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

// Block assembly and the per-block DSP pipeline of the stream thread
// (master channel, non-RSPduo) with the packet-header toggle processing.
//
// One StreamProcessor corresponds to one stream thread: it is created when the
// device is selected (finishSetup) and lives across Init/Uninit cycles, so its
// per-stream state (last fs, last LNA, packet counter, dcCal period) persists across
// them. The service owns transport, timing, ring slots and USB; this class is
// pure and deterministic.
//
// Per block (processBlock):
//   1. fs change (confirmed fs != last): DC c0/trackPkts, dcCal period, hooks->sampleRateChanged; DC reset
//   2. staged decimation (updateDecimation) becomes active (2 and N: at the block start, beginBlock())
//   3. N packets: header, unpack into xi/xq + p*spp, toggle confirmations -> masks 1<<p
//   4. DC (xi only for low IF, then xq = 0); 5. power/AGC hooks;
//   6. down-converter; 7. user decimation; 8. IQ correction (zero IF only)
//   9. agcWaitGain cleared if a Gr toggle was confirmed while waiting
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/dsp/DcIq.h"
#include "core/dsp/DspCommon.h"
#include "core/dsp/DownConverter.h"
#include "core/dsp/Packet.h"
#include "core/dsp/UserDecimator.h"

namespace sdrplay {
namespace dsp {

// The latched channel parameters the stream path reads (sdrplay_api_RxChannelParamsT
// fields). The service updates them whenever it latches the API parameters (every Update).
struct ChannelParams {
    int bwType = 200;            // tunerParams.bwType (sdrplay_api_BW_0_200)
    int ifType = 0;              // tunerParams.ifType
    std::uint8_t lnaState = 0;   // tunerParams.gain.LNAstate
    bool dcEnable = true;        // ctrlParams.dcOffset.DCenable (dcOffsetIqControl turns it on when IQ is on)
    bool iqEnable = true;        // ctrlParams.dcOffset.IQenable
    bool decEnable = false;      // ctrlParams.decimation.enable
    std::uint8_t decFactor = 1;  // ctrlParams.decimation.decimationFactor
    std::uint8_t decWide = 0;    // ctrlParams.decimation.wideBandSignal
    int agcEnable = 0;           // ctrlParams.agc.enable (0 off, 1..3 fixed rates, 4 CTRL_EN)
    int adsbMode = 0;            // ctrlParams.adsbMode (read live per block)
};

struct BlockInfo {
    std::uint32_t firstSampleNum = 0;
    std::uint32_t grChanged = 0;     // bit p = packet p (pre-decimation) confirmed a reported Gr change
    std::uint32_t rfChanged = 0;
    std::uint32_t fsChanged = 0;
    std::uint32_t numSamples = 0;
    std::uint32_t reset = 0;         // 1 on the first block after (re)start / read error
    std::uint32_t packets = 0;       // N used for this block
    std::uint32_t packetsRead = 0;   // packets whose read succeeded
    bool readError = false;          // the LAST packet read failed: the stream loop then skips one
                                     // iteration (10 ms sleep) and the next block has reset = 1
    bool dcNotEnabled = false;       // DC offset correction off: logged once as an error
    bool decimationConfigApplied = false; // a staged user-decimation setting took effect
    std::uint32_t gaps = 0;          // sampleNum discontinuities in this block
    bool grConfirmed = false;        // any Gr toggle confirmed in this block (reported or not)
};

// Source of 1024-byte device packets. nullptr = read error.
class PacketSource {
public:
    virtual ~PacketSource() = default;
    virtual const std::uint8_t *nextPacket() = 0;
};

// Contiguous packets in memory (tests, replay).
class MemoryPacketSource : public PacketSource {
public:
    MemoryPacketSource(const std::uint8_t *data, std::size_t count) : data_(data), count_(count) {}
    const std::uint8_t *nextPacket() override
    {
        if (pos_ >= count_) return nullptr;
        return data_ + std::size_t(kPacketBytes) * pos_++;
    }
    std::size_t position() const { return pos_; }

private:
    const std::uint8_t *data_;
    std::size_t count_;
    std::size_t pos_ = 0;
};

struct BlockView {
    std::int16_t *xi;
    std::int16_t *xq;
    std::uint32_t n;        // samples (pre-down-conversion)
    bool ionly;             // power estimator uses xi only
    std::uint32_t packets;  // N
    std::uint32_t spp;
    double fsHz;            // confirmed fs
};

class StreamProcessor;

// Gain-control hooks (power estimate, AGC, dcCal retrigger, deferred sync update).
// The AGC module (src/core/dsp/agc) implements these; all default to no-ops.
class StreamHooks {
public:
    virtual ~StreamHooks() = default;
    virtual void sampleRateChanged(StreamProcessor &, double /*fsHz*/) {}
    virtual void powerEstimate(StreamProcessor &, const BlockView &) {}
    virtual void agc(StreamProcessor &, const BlockView &) {}
    // AGC off and dcCal period reached: programGain re-runs when dcCal == 4.
    virtual void dcCalPeriod(StreamProcessor &) {}
    // The packet read re-issues a deferred programSyncUpdate once sampleNum is below 2^31. Return true
    // when it succeeded (the deferral is then cleared).
    virtual bool deferredSyncUpdate(StreamProcessor &) { return true; }
    virtual void gainConfirmed(StreamProcessor &, std::int32_t /*grValue*/) {}
    virtual void rfConfirmed(StreamProcessor &, double /*rfHz*/) {}
    virtual void fsConfirmed(StreamProcessor &, double /*fsHz*/) {}
    // Model-specific down-converter (RSPdx HDR: rspdx::HdrDecim4 / HdrDecim3) in place of the
    // configured one: convert n real samples of xi into I/Q in xi/xq and return true. The
    // output count follows the configuration ((n >> countShift) / countDivisor). false: the
    // normal down-converter runs.
    virtual bool downConvert(StreamProcessor &, std::int16_t * /*xi*/, std::int16_t * /*xq*/, std::uint32_t /*n*/,
                             bool /*reset*/)
    {
        return false;
    }
};

class StreamProcessor {
public:
    StreamProcessor();

    // ---- configuration (service side, the Init/Update control steps)
    ChannelParams &params() { return params_; }
    const ChannelParams &params() const { return params_; }
    // Channel init at Init: DSP histories, DC trackers, IQ state, active
    // decimation {0,1,0}, agcWaitGain 0. Also runs configureDownConverter(fsMHz).
    void initChannel(double fsMHz);
    // Samples per packet for the current fs.
    void setSamplesPerPacket(std::uint32_t spp) { spp_ = spp; }
    std::uint32_t samplesPerPacket() const { return spp_; }
    // The fs the DSP uses (set at Init; replaced by confirmed Fs toggles).
    void setConfirmedFsHz(double fsHz) { fsHz_ = fsHz; }
    double confirmedFsHz() const { return fsHz_; }
    void configureDownConverter(double fsMHz);   // on Init / IfType / BwType / Fs updates
    void updateDecimation();                      // Update(Ctrl_Decimation) / Init
    void dcOffsetIqControl();                    // Update(Ctrl_DCoffsetIQimbalance) / Init
    const DownConverterConfig &downConverter() const { return dcfg_; }
    std::uint32_t packetsPerBlock() const { return numPackets_; }
    void setPacketsPerBlock(std::uint32_t n) { numPackets_ = n; }   // (normally via the controls)

    // ---- stream state
    void requestReset() { reset_ = true; }     // first block after (re)start: reset = 1
    // RSPduo: a slave channel joining a running stream shares the stream's reset flag (0).
    void cancelReset() { reset_ = false; }
    void forceFsChanged() { forceFs_ = true; } // fsChanged bit on the next packet
    // Pending hardware changes and the header toggle value that confirms them.
    void armGain(std::uint32_t expectedToggle, std::int32_t grValue, bool notify, bool report);
    void armRf(std::uint32_t expectedToggle, double rfHz, bool report);
    void armFs(std::uint32_t expectedToggle, double fsHz, bool report);
    void armDeferredSync() { syncDeferred_ = true; }
    bool gainPending() const { return grPending_; }

    // ---- RSPduo Dual/Master/Slave channels. Defaults
    // leave the single-tuner pipeline untouched.
    // One tuner per ADC channel: the I (or Q) component of every packet is this channel's real
    // low-IF input (Q is copied into xi after unpacking; the LIF pipeline then zeroes xq).
    enum class RealInput : std::uint8_t { None, I, Q };
    void setRealInput(RealInput r) { realInput_ = r; }
    // 504-spp unpacker choice: IQ8 for an RSPduo not in single-tuner mode (else by ifType).
    void setIqFor504(bool v) { iqFor504_ = v; }
    // configureDownConverter of an RSPduo channel in Dual/Master/Slave mode (the model's own
    // selection, e.g. LIF forced for Master/Slave): used instead of selectDownConverter.
    void setDownConverterOverride(bool on, const DownConverterConfig &c)
    {
        dcOverride_ = on;
        dcOverrideCfg_ = c;
    }
    // Header toggles confirmed by the device for this channel, bit p = packet p of the next
    // block (the RSPduo protocol keeps one shared toggle set for both tuners). With a non-null
    // pointer processBlock() ignores its own armed state and uses these bits instead.
    struct ExternalToggles {
        std::uint32_t grConfirmed = 0; // gain update of this tuner confirmed (DC reset, AGC wait cleared)
        std::uint32_t grReport = 0;  // ... reported (grChanged)
        std::uint32_t rf = 0;        // rfChanged
        std::uint32_t fs = 0;        // fsChanged
    };
    void setExternalToggles(const ExternalToggles *t) { ext_ = t; }

    bool agcWaitGain() const { return agcWaitGain_; }
    void setAgcWaitGain(bool v) { agcWaitGain_ = v; }  // the AGC sets it after a successful programGain

    // ---- per block
    // Reads packetsPerBlock() packets from src, processes them into buffers (xi/xq hold
    // at least kMaxBlockSamples each; the read limits matter only for odd decimation
    // factors), fills info. hooks may be nullptr.
    void processBlock(PacketSource &src, const BlockBuffers &buffers, BlockInfo &info, StreamHooks *hooks = nullptr);
    // Block start of the stream thread, before its first
    // packet read: the staged decimation becomes active (step 2) and N is latched.
    // The thread then waits in the packet read, so a block that is waiting for its
    // 384-packet chunk when an Update(Ctrl_Decimation/IfType/BwType) runs completes with the
    // pre-Update N and user decimation; the down-converter config and spp are read live after
    // the wait. The next processBlock() uses this N and
    // skips step 2. Without a beginBlock() call processBlock() does both itself (DSP vectors).
    std::uint32_t beginBlock(); // idempotent while the block is open
    bool blockOpen() const { return blockOpen_; }
    void closeBlock() { blockOpen_ = false; } // block discarded (Init/Uninit, stream gone)

    // ---- state exposed for tests / diagnostics
    DcOffsetCorrector &dc() { return dc_; }
    IqImbalanceCorrector &iq() { return iq_; }
    UserDecimator &decimator() { return dec_; }
    std::uint32_t packetCounter() const { return pktCounter_; }
    void setPacketCounter(std::uint32_t v) { pktCounter_ = v; }
    std::uint32_t dcCalPeriodPackets() const { return dcCalPeriod_; }

private:
    bool applyStagedDecimation(); // step 2: staged -> active
    ChannelParams params_{};
    DownConverterConfig dcfg_{};
    DcOffsetCorrector dc_{};
    IqImbalanceCorrector iq_{};
    UserDecimator dec_{};
    DownConverter down_{};
    DecimationSetting staged_{};
    bool decStaged_ = false;
    bool blockOpen_ = false;           // beginBlock() called, processBlock() not yet
    bool blockDecApplied_ = false;     // beginBlock() applied the staged decimation
    std::uint32_t blockPackets_ = 4;   // N latched by beginBlock()

    std::uint32_t spp_ = 252;
    std::uint32_t numPackets_ = 4;
    double fsHz_ = 2000000.0;

    // per-stream state
    bool reset_ = true;
    bool iqResetPending_ = true;
    std::uint8_t lastLna_ = 0;
    double lastFs_ = 0.0;
    std::uint32_t dcCalPeriod_ = 0xFFFFFFFFu;
    std::uint32_t pktCounter_ = 0;
    std::uint32_t expectSn_ = 0;
    bool agcWaitGain_ = false;

    // toggles
    bool grPending_ = false, grNotify_ = false;
    std::uint32_t grExpected_ = 0;
    std::int32_t grValue_ = 0;
    bool grReport_ = false;
    bool rfPending_ = false, rfReport_ = false;
    std::uint32_t rfExpected_ = 0;
    double rfValue_ = 0.0;
    bool fsPending_ = false, fsReport_ = false;
    std::uint32_t fsExpected_ = 0;
    double fsNew_ = 0.0;
    bool forceFs_ = false;
    bool syncDeferred_ = false;

    // RSPduo non-single channels (defaults: single-tuner behaviour)
    RealInput realInput_ = RealInput::None;
    bool iqFor504_ = false;
    bool dcOverride_ = false;
    DownConverterConfig dcOverrideCfg_{};
    const ExternalToggles *ext_ = nullptr;
};

} // namespace dsp
} // namespace sdrplay
