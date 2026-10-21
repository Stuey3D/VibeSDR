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

// Software AGC: control, fixed modes, CTRL_EN, computeAgcParams and the
// per-block coefficient. See Agc.h.
#include "core/dsp/agc/Agc.h"

#include "core/dsp/DspCommon.h"

#include <sdrplay_api.h>

#include <cmath>
#include <cstdint>

namespace sdrplay {
namespace dsp {
namespace agc {

namespace {

// Fixed-mode loop coefficients at the reference rate (2 MHz, 252 samples per packet).
constexpr double kC0_100Hz = 0.29;
constexpr double kC0_50Hz = 0.145;
constexpr double kC0_5Hz = 0.0145;
constexpr double kCInit = 0.29;       // coefficient after channel init
constexpr double kFsRef = 2000000.0;
constexpr double kPktRef = 252.0;
constexpr int kMaxGr = MAX_BB_GR;     // maximum IF gain reduction, dB
constexpr int kHoldoffBlocks = 20;    // blocks without a decision after a failed gain update

// Smallest AGC step worth applying, from the rounded half-error v: |v|, at least 1 dB.
// truncToI32 returns INT32_MIN for an error too large for an int; that also gives 1.
std::int32_t minStepFrom(std::int32_t v)
{
    if (v == INT32_MIN) return 1;
    const std::int32_t m = v < 0 ? -v : v;
    return m < 1 ? 1 : m;
}

} // namespace

int setPointMin(int spp, int hwVer, int rspDuoMode, int ifType)
{
    if (spp == 384) return -60;
    if (spp != 504) return -72;
    if (hwVer == SDRPLAY_RSPduo_ID && rspDuoMode != sdrplay_api_RspDuoMode_Single_Tuner) return -48;
    return ifType == sdrplay_api_IF_Zero ? -48 : -72;
}

void AgcChannel::channelInit()
{
    lastEnable_ = 0;
    recalc_ = false;
    lastLna_ = -1;
    lastGr_ = -1;
    waitGain_ = false;
    holdoff_ = 0;
    coefBase_ = kCInit;
    coef_ = kCInit;
    forced_ = false;
    userReq_ = false;
    haveFs_ = false;
}

int AgcChannel::control(int minSetPoint, int *minOut)
{
    if (minOut) *minOut = minSetPoint;
    const int sp = p_.setPoint_dBfs;
    if (sp > 0 || sp < minSetPoint) return sdrplay_api_OutOfRange;   // nothing changed

    forced_ = false;
    const int en = p_.enable;
    const bool wasDisabled = lastEnable_ == kAgcDisable;
    lastEnable_ = en;
    if (en == kAgcDisable) {
        fixedAcc_ = 0.0;
        ctrlAcc_ = 0.0;
        return sdrplay_api_Success;
    }
    if (wasDisabled) forced_ = true;   // disabled -> enabled: the first evaluation always applies
    if (en != kAgcCtrlEn) {
        const double c0 = en == kAgc100Hz ? kC0_100Hz : en == kAgc50Hz ? kC0_50Hz : kC0_5Hz;
        coefBase_ = c0;
        coef_ = c0;              // raw until the stream thread rescales it (see Agc.h)
    }
    recalc_ = true;
    return sdrplay_api_Success;
}

void AgcChannel::recompute(double fsHz, int spp, int numPackets)
{
    if (p_.enable == kAgcDisable) return;
    if (p_.enable == kAgcCtrlEn) {
        computeAgcParams(fsHz, spp, numPackets);
        return;
    }
    coef_ = (static_cast<double>(spp) * ((kFsRef * coefBase_) / fsHz)) / kPktRef;
}

void AgcChannel::computeAgcParams(double fsHz, int spp, int numPackets)
{
    const double t = static_cast<double>(spp * numPackets) / fsHz;   // block length, seconds
    if (p_.attack_ms == 0) {
        attack_ = 1.0;
    } else {
        attack_ = 1.0 - std::pow(10.0, -((t * 1000.0) / static_cast<double>(p_.attack_ms)));
    }
    double d = 1.0;
    if (p_.decay_ms != 0) d = 1.0 - std::pow(10.0, -((t * 1000.0) / static_cast<double>(p_.decay_ms)));
    decay_ = d;
    const int cnt = truncToI32(static_cast<double>(p_.decay_delay_ms) / (t * 1000.0) + 0.5);
    dcnt_ = cnt;
    dreload_ = cnt;
    dthresh_ = p_.decay_threshold_dB;
    phase_ = CtrlPhase::Settled;
}

void AgcChannel::blockStart(double fsHz, int spp, int numPackets)
{
    if (haveFs_ && fsHz == lastFs_) return;
    haveFs_ = true;
    lastFs_ = fsHz;
    recalc_ = false;
    recompute(fsHz, spp, numPackets);
}

void AgcChannel::blockEnd(double fsHz, int spp, int numPackets)
{
    if (!recalc_) return;
    recalc_ = false;
    recompute(fsHz, spp, numPackets);
}

AgcDecision AgcChannel::evaluate(double powDb, const AgcGainState &g)
{
    if (p_.enable == kAgcCtrlEn) return evalCtrlEn(powDb, g);
    return evalFixed(powDb, g);
}

AgcDecision AgcChannel::makeApply(int step, const AgcGainState &g)
{
    AgcDecision d;
    d.apply = true;
    d.step = step;
    d.newGRdB = wrap32(std::int64_t(g.gRdB) + step);
    d.reportUser = userReq_;
    return d;
}

AgcDecision AgcChannel::applyUnlessAtLimit(int step, const AgcGainState &g, bool lnaSame)
{
    // Nothing to do when the LNA state is unchanged, no apply is forced and the IF gain
    // reduction already sits at the limit the step pushes towards.
    const bool atMin = g.minGr == lastGr_ && step <= 0;
    const bool atMax = lastGr_ == kMaxGr && step >= 0;
    if (!forced_ && lnaSame && (atMin || atMax)) {
        userReq_ = false;
        return {};
    }
    return makeApply(step, g);
}

bool AgcChannel::holdingOff()
{
    if (holdoff_ == 0) return false;
    return --holdoff_ != 0;
}

void AgcChannel::applied(int setGrRc, int appliedIfGr, std::uint8_t lnaState)
{
    if (setGrRc == sdrplay_api_Success) {
        waitGain_ = true;
        lastGr_ = appliedIfGr;
        lastLna_ = lnaState;
    } else if (setGrRc == sdrplay_api_GainUpdateError) {
        holdoff_ = kHoldoffBlocks;
    }
    forced_ = false;
    userReq_ = false;
}

// Fixed modes: AGC_100HZ / AGC_50HZ / AGC_5HZ
AgcDecision AgcChannel::evalFixed(double powDb, const AgcGainState &g)
{
    if (powDb == 0.0) return {};
    // First-order loop on half the setpoint error.
    const double a = fixedAcc_;
    fixedAcc_ = ((static_cast<double>(p_.setPoint_dBfs) - powDb) * 0.5 - a) * coef_ + a;
    const std::int32_t step = truncToI32(-fixedAcc_);
    const bool lnaSame = lastLna_ == g.lnaState;

    if (holdingOff()) return {};
    if (step >= -1 && step <= 1) {
        // A step of at most 1 dB is applied only to pick up an LNA change or a forced update.
        if (!forced_ && lnaSame) return {};
        return makeApply(step, g);
    }
    return applyUnlessAtLimit(step, g, lnaSame);
}

// AGC_CTRL_EN
AgcDecision AgcChannel::evalCtrlEn(double powDb, const AgcGainState &g)
{
    if (powDb == 0.0) return {};
    const double e = static_cast<double>(p_.setPoint_dBfs) - powDb;
    const bool lnaSame = lastLna_ == g.lnaState;
    std::int32_t step = 0;
    bool small = true;   // |step| below the minimum worth applying

    if (e <= -1.0 || 1.0 <= e) {
        // Smallest step worth applying: |e|/2 rounded, at least 1 dB.
        const std::int32_t minStep = minStepFrom(truncToI32(e * 0.5 + 0.5));

        bool accumulate = true;
        if (0.0 <= e) {
            // Signal below the setpoint: decay, once the decay delay has run out.
            if (phase_ != CtrlPhase::Decay) {
                if (e <= static_cast<double>(dthresh_)) {
                    accumulate = false;   // within the decay threshold: hold
                } else if (dcnt_ != 0) {
                    phase_ = CtrlPhase::DecayDelay;
                    --dcnt_;
                    accumulate = false;
                } else {
                    phase_ = CtrlPhase::Decay;
                }
            }
            if (accumulate) ctrlAcc_ = decay_ * e + ctrlAcc_;
        } else {
            phase_ = CtrlPhase::Attack;
            dcnt_ = dreload_;
            ctrlAcc_ = attack_ * e + ctrlAcc_;
        }
        if (accumulate) {
            step = truncToI32(-ctrlAcc_);
            small = step < minStep && -minStep < step;
        }
    } else {
        phase_ = CtrlPhase::Settled;
    }

    if (holdingOff()) return {};
    if (small && !forced_ && lnaSame) return {};
    ctrlAcc_ = ctrlAcc_ + static_cast<double>(step);   // keep the fractional residue
    return applyUnlessAtLimit(step, g, lnaSame);
}

GainChangeEvent makeGainChangeEvent(int appliedIfGr, int totalGr, float gainValsCurr)
{
    GainChangeEvent ev;
    ev.gRdB = static_cast<unsigned>(appliedIfGr);
    ev.lnaGRdB = static_cast<unsigned>(totalGr - appliedIfGr);
    ev.currGain = static_cast<double>(gainValsCurr);
    return ev;
}

} // namespace agc
} // namespace dsp
} // namespace sdrplay
