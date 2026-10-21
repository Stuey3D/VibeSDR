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

#include "core/dsp/StreamProcessor.h"

#include <chrono>
#include <cstring>

#include "core/dsp/DspCommon.h"
#include "core/dsp/Kernels.h"
#include "core/dsp/StageTimes.h"

#include <sdrplay_api.h>

namespace sdrplay {
namespace dsp {

namespace {

// Times the consecutive stages of one block when on; the totals go to stages::add().
class StageClock {
public:
    explicit StageClock(bool on) : on_(on)
    {
        if (on_) t_ = now();
    }
    ~StageClock()
    {
        if (on_) stages::add(ns_);
    }
    void lap(stages::Stage s)
    {
        if (!on_) return;
        const std::uint64_t t = now();
        ns_[s] += t - t_;
        t_ = t;
    }

private:
    static std::uint64_t now()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    bool on_;
    std::uint64_t t_ = 0;
    std::uint64_t ns_[stages::kCount] = {};
};

} // namespace

StreamProcessor::StreamProcessor()
{
    dec_.init();
    iq_.reset();
}

void StreamProcessor::initChannel(double fsMHz)
{
    // channel init (DSP-relevant part)
    down_.init();
    dec_.init();
    dc_.dcI = dc_.dcQ = 0.0;
    dc_.cnt = 0;
    dc_.statsEnabled = true;
    dc_.tracking = false;
    iq_.reset();
    iq_.period = 0;
    iq_.step = kIqAccStep;
    iq_.deadBand = kIqDeadBand;
    agcWaitGain_ = false;
    decStaged_ = false;
    blockOpen_ = false;
    blockDecApplied_ = false;
    configureDownConverter(fsMHz);
}

void StreamProcessor::configureDownConverter(double fsMHz)
{
    dcfg_ = dcOverride_ ? dcOverrideCfg_ : selectDownConverter(fsMHz, params_.bwType, params_.ifType);
    numPackets_ = numActivePackets(dcfg_, params_.decEnable, params_.decFactor);
}

void StreamProcessor::updateDecimation()
{
    staged_ = dsp::updateDecimation(params_.decEnable, params_.decFactor, params_.decWide);
    numPackets_ = numActivePackets(dcfg_, params_.decEnable, params_.decFactor);
    decStaged_ = true;
}

void StreamProcessor::dcOffsetIqControl()
{
    if (params_.iqEnable) params_.dcEnable = true; // IQ correction needs DC correction
    if (params_.dcEnable) dc_.resetTrackers();
    if (params_.iqEnable) iq_.reset();
}

void StreamProcessor::armGain(std::uint32_t expectedToggle, std::int32_t grValue, bool notify, bool report)
{
    grPending_ = true;
    grExpected_ = expectedToggle;
    grValue_ = grValue;
    grNotify_ = notify;
    grReport_ = report;
}

void StreamProcessor::armRf(std::uint32_t expectedToggle, double rfHz, bool report)
{
    rfPending_ = true;
    rfExpected_ = expectedToggle;
    rfValue_ = rfHz;
    rfReport_ = report;
}

void StreamProcessor::armFs(std::uint32_t expectedToggle, double fsHz, bool report)
{
    fsPending_ = true;
    fsExpected_ = expectedToggle;
    fsNew_ = fsHz;
    fsReport_ = report;
}

bool StreamProcessor::applyStagedDecimation()
{
    if (!decStaged_) return false;
    decStaged_ = false;
    DecimationSetting a = staged_;
    a.enable = params_.decEnable; // the active copy takes the live decEnable/decFactor/decWide
    a.factor = params_.decFactor;
    a.wide = params_.decWide;
    dec_.setActive(a);
    return true;
}

std::uint32_t StreamProcessor::beginBlock()
{
    if (blockOpen_) return blockPackets_; // still waiting for this block's packets
    blockDecApplied_ = applyStagedDecimation();
    blockPackets_ = numPackets_;
    blockOpen_ = true;
    return blockPackets_;
}

void StreamProcessor::processBlock(PacketSource &src, const BlockBuffers &buffers, BlockInfo &info, StreamHooks *hooks)
{
    StreamHooks nullHooks;
    StreamHooks &hk = hooks ? *hooks : nullHooks;
    info = BlockInfo{};
    StageClock clock(stages::takeBlock());
    const bool reset = reset_;
    info.reset = reset ? 1u : 0u;

    // 1. fs change -> DC / AGC coefficients (before the packets)
    bool dcReset = false;
    const double fs = fsHz_;
    if (fs != lastFs_) {
        dcCalPeriod_ = truncToU32((60.0 * fs) / static_cast<double>(spp_));
        dc_.setSampleRate(fs);
        dcReset = true;
        lastFs_ = fs;
        hk.sampleRateChanged(*this, fs);
    }

    // 2. staged decimation (unless beginBlock() already did it at the block start)
    std::uint32_t N;
    if (blockOpen_) {
        blockOpen_ = false;
        info.decimationConfigApplied = blockDecApplied_;
        N = blockPackets_;
    } else {
        info.decimationConfigApplied = applyStagedDecimation();
        N = numPackets_;
    }

    // 3. packets
    const std::uint32_t spp = spp_;
    info.packets = N;
    bool gainConfirmed = false;
    bool agcClear = false;
    bool lastReadFailed = false;
    std::uint32_t first = 0;
    PacketFormat fmt{};
    const bool fmtOk = formatForSpp(static_cast<int>(spp), iqFor504_ || params_.ifType == sdrplay_api_IF_Zero, fmt);
    const Kernels &kern = kernels();
    for (std::uint32_t p = 0; p < N; ++p) {   // N <= kMaxPacketsPerBlock (32)
        const std::uint32_t bit = 1u << p;      // packet p in the block's change masks
        const std::uint8_t *pkt = src.nextPacket();
        if (!pkt) {
            lastReadFailed = true;
            continue;
        }
        lastReadFailed = false;
        ++info.packetsRead;
        const PacketHeader h = parsePacketHeader(pkt);
        if (fmtOk) kern.unpack[static_cast<int>(fmt)](pkt, buffers.xi + std::size_t(p) * spp, buffers.xq + std::size_t(p) * spp);
        if (fmtOk && realInput_ == RealInput::Q)
            std::memcpy(buffers.xi + std::size_t(p) * spp, buffers.xq + std::size_t(p) * spp, std::size_t(spp) * sizeof(std::int16_t));

        // packet-header toggle processing
        bool grFlag = false, grRep = false, rfFlag = false, fsFlag = false;
        if (ext_) {
            grFlag = (ext_->grConfirmed & bit) != 0; // single-tuner arms always notify
            grRep = grFlag && (ext_->grReport & bit) != 0;
            rfFlag = (ext_->rf & bit) != 0;
            fsFlag = (ext_->fs & bit) != 0;
        } else if (grPending_ && grExpected_ == h.grToggle) {
            hk.gainConfirmed(*this, grValue_);
            if (grNotify_) {
                if (grReport_) grRep = true;
                grFlag = true;
            }
            grPending_ = false;
            grNotify_ = false;
            grReport_ = false;
        }
        if (!ext_ && rfPending_ && rfExpected_ == h.rfToggle) {
            hk.rfConfirmed(*this, rfValue_);
            if (rfReport_) rfFlag = true;
            rfPending_ = false;
            rfReport_ = false;
        }
        if (!ext_ && fsPending_ && fsExpected_ == h.fsToggle) {
            fsHz_ = fsNew_;
            hk.fsConfirmed(*this, fsNew_);
            if (fsReport_) fsFlag = true;
            fsPending_ = false;
            fsReport_ = false;
        }
        if (syncDeferred_ && h.sampleNum < 0x80000000u) {
            if (hk.deferredSyncUpdate(*this)) syncDeferred_ = false;
        }

        // per-packet bookkeeping
        if (grFlag) {
            if (grRep) info.grChanged |= bit;
            gainConfirmed = true;
            if (agcWaitGain_) agcClear = true;
            const std::uint8_t lna = params_.lnaState;
            if (lna != lastLna_) {
                lastLna_ = lna;
                if (!dcfg_.lif) iqResetPending_ = true;
            }
        }
        if (rfFlag) info.rfChanged |= bit;
        if (fsFlag) info.fsChanged |= bit;
        if (forceFs_) {
            forceFs_ = false;
            info.fsChanged |= bit;
        }
        if (!reset) {
            expectSn_ += spp;
            if (expectSn_ != h.sampleNum) ++info.gaps;
        }
        ++pktCounter_;
        expectSn_ = h.sampleNum;
        if (p == 0) first = h.sampleNum;
    }
    info.grConfirmed = gainConfirmed;
    clock.lap(stages::Unpack);

    std::uint32_t n = spp * N;
    const bool lif = dcfg_.lif;

    // 4. DC offset correction (always called; low IF: xi only, then xq zeroed)
    const bool dcOk = dc_.run(buffers.xi, lif ? nullptr : buffers.xq, n, dcReset || gainConfirmed || reset,
                              params_.dcEnable, N);
    info.dcNotEnabled = !dcOk;
    if (lif) std::memset(buffers.xq, 0, std::size_t(n) * sizeof(std::int16_t));
    clock.lap(stages::DcOffset);

    // 5. power estimate / AGC (RSP1B: never in HDR mode)
    const BlockView view{buffers.xi, buffers.xq, n, dcfg_.ionly, N, spp, fsHz_};
    if (params_.agcEnable == 0) {
        if (dcCalPeriod_ <= pktCounter_) {
            hk.dcCalPeriod(*this);
            pktCounter_ = 0;
        }
        hk.powerEstimate(*this, view);
    } else if (!agcWaitGain_) {
        hk.powerEstimate(*this, view);
        hk.agc(*this, view);
        const bool agcStillOn = params_.agcEnable != 0;
        pktCounter_ = 0;
        if (!agcStillOn) hk.powerEstimate(*this, view);
    }
    clock.lap(stages::PowerAgc);

    // 6. down-converter
    if (dcfg_.enable) {
        if (!hk.downConvert(*this, buffers.xi, buffers.xq, n, reset))
            down_.run(dcfg_, buffers.xi, buffers.xq, n, reset, params_.adsbMode);
        if (!(dcfg_.adsbCfg && params_.adsbMode != sdrplay_api_ADSB_DECIMATION)) {
            n = (n >> dcfg_.countShift) / dcfg_.countDivisor;
            first = (first >> dcfg_.countShift) / dcfg_.countDivisor;
        }
    }
    clock.lap(stages::DownConvert);

    // 7. user decimation
    if (dec_.enabled()) {
        dec_.run(buffers, n, reset, dcfg_.ionly);
        const unsigned s = dec_.active().shift;
        first >>= s;
        n >>= s;
    }
    clock.lap(stages::Decimate);

    // 8. IQ imbalance (zero IF only; the reset part runs even when IQ is disabled)
    if (!lif) {
        iq_.run(buffers.xi, buffers.xq, n, iqResetPending_ || reset, params_.iqEnable);
        iqResetPending_ = false;
    }
    clock.lap(stages::IqBalance);

    // 9. Clear the AGC wait once a gain toggle has been confirmed.
    if (agcClear) agcWaitGain_ = false;

    info.firstSampleNum = first;
    info.numSamples = n;
    info.readError = lastReadFailed;
    reset_ = lastReadFailed; // read error -> skip one iteration, then reset = 1
}

} // namespace dsp
} // namespace sdrplay
