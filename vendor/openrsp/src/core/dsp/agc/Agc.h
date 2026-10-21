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

// Software IF AGC of the service (pure, portable, one instance per tuner channel).
//
//   control()         Update(Ctrl_Agc) and initHardware
//   fixed modes       AGC_100HZ / AGC_50HZ / AGC_5HZ
//   CTRL_EN mode      AGC_CTRL_EN
//   computeAgcParams
//   coefficient       stream thread, top of a block
//
// Arithmetic is double precision with a fixed operation order (build with
// -ffp-contract=off); pow is the C library's. Integer conversions
// truncate toward zero and give INT32_MIN for NaN / out of range, on every architecture.
//
// How the stream layer drives one channel per block:
//
//   blockStart(fs, spp, N)                 // fs change since the last block -> recompute
//   ... unpack, DC correction ...
//   if (!enabled())       power only (PowerEstimator) ; [dcCal==4 retrigger]
//   else if (!waiting()) {
//       pow = PowerEstimator::estimate(...)
//       AgcDecision d = evaluate(pow, gain)
//       if (d.apply) { rc = applyAgcGain(d.newGRdB);   // the engine's programGain, AGC caller
//                      applied(rc, appliedIfGr, lnaState) }   // MUST follow an apply decision
//   }
//   ... down-convert, decimate, IQ ...
//   if (gain toggle confirmed in this block) gainConfirmed()
//   blockEnd(fs, spp, N)                   // deferred Ctrl_Agc recompute (see below)
//
// Coefficient timing: configureAgc stores the RAW coefficient (c0 = 0.29 /
// 0.145 / 0.0145) and only flags a recompute; the stream thread applies the
// (2e6/fs)*(spp/252) scaling (or computeAgcParams for CTRL_EN) at the top of the next block. The
// update normally lands while the stream thread is blocked reading the first packet of a
// block, i.e. after that block's check, so the first AGC evaluation after any
// Update(Ctrl_Agc) uses the unscaled c0. blockEnd() reproduces that ordering
// deterministically for updates applied between blocks.
#pragma once

#include <cstdint>

namespace sdrplay {
namespace dsp {
namespace agc {

// sdrplay_api_AgcControlT
enum AgcEnable : int {
    kAgcDisable = 0,
    kAgc100Hz = 1,
    kAgc50Hz = 2,
    kAgc5Hz = 3,
    kAgcCtrlEn = 4,
};

// sdrplay_api_AgcT as the service holds it.
struct AgcParams {
    int enable = kAgc50Hz;
    int setPoint_dBfs = -60;
    std::uint16_t attack_ms = 0;
    std::uint16_t decay_ms = 0;
    std::uint16_t decay_delay_ms = 0;
    std::uint16_t decay_threshold_dB = 0;
    int syncUpdate = 0;
};

// Setpoint lower limit of configureAgc (upper limit 0):
//   spp 384 -> -60; spp 504 -> zero-IF -48 / low-IF -72 (RSPduo: -48 unless in single-tuner
//   mode); any other spp -> -72. rspDuoMode = devParams rspDuoMode.
int setPointMin(int spp, int hwVer, int rspDuoMode, int ifType);

// Error text recorded when configureAgc rejects the setpoint (sdrplay_api_OutOfRange), as
// sdrplay_api_GetLastError reports it.
constexpr const char *kAgcControlErrFmt =
    "Tuner%d: setPoint_dBfs out of range for current sampling freq[%d:0], %d";

// What the AGC reads from the tuner channel at evaluation time.
struct AgcGainState {
    int gRdB = 0;              // tunerParams.gain.gRdB: the AGC adds its step to THIS value
    std::uint8_t lnaState = 0; // tunerParams.gain.LNAstate
    int minGr = 20;            // tunerParams.gain.minGr
};

// CTRL_EN loop phase.
enum class CtrlPhase : int {
    Settled = 0,    // within 1 dB of the setpoint (also after computeAgcParams)
    Attack = 1,     // signal above the setpoint
    Decay = 2,      // signal below the setpoint, decay delay over
    DecayDelay = 3, // signal below the setpoint, decay delay still running
};

struct AgcDecision {
    bool apply = false;      // program the gain from the AGC now
    int step = 0;            // dB added to gRdB
    int newGRdB = 0;         // gRdB + step (programGain clamps to [minGr, 59] and writes it back)
    bool reportUser = false; // report the change to the user: a user LNA/gain request is pending
};

class AgcChannel {
public:
    // Channel init (AGC part): coefBase_ = coef_ = 0.29, lastEnable 0, lastLna/lastGr -1,
    // not waiting, holdoff 0, flags cleared. The integrators (fixedAcc_/ctrlAcc_) are NOT touched there.
    void channelInit();

    // The service's copy of ctrlParams.agc (made before configureAgc runs; the stream thread
    // always reads the live copy, even when configureAgc rejected the setpoint).
    void setParams(const AgcParams &p) { p_ = p; }
    const AgcParams &params() const { return p_; }
    bool enabled() const { return p_.enable != kAgcDisable; }

    // configureAgc. Returns sdrplay_api_Success, or sdrplay_api_OutOfRange when setPoint is outside
    // [minSetPoint, 0] (nothing else is changed then; *minOut receives the limit for the error
    // text). initHardware ignores the result.
    int control(int minSetPoint, int *minOut = nullptr);

    // Update(Tuner_Gr) (or an implied Gr update) while the AGC is on: the next evaluation
    // applies unconditionally and its programGain reports to the user.
    void requestApply() { forced_ = true; userReq_ = true; }

    // Top of a block: a sample-rate change (also the first block after a (re)start) re-derives
    // the coefficient (fixed modes) or runs computeAgcParams (CTRL_EN) when the AGC is enabled.
    void blockStart(double fsHz, int spp, int numPackets);
    // End of a block: the recompute requested by configureAgc (see the header comment).
    void blockEnd(double fsHz, int spp, int numPackets);
    // Forget the last fs (stream restart): the next blockStart recomputes.
    void streamRestart() { haveFs_ = false; }

    bool waiting() const { return waitGain_; }   // agcWaitGain
    void gainConfirmed() { waitGain_ = false; }

    // One AGC evaluation (only when enabled() && !waiting()).
    AgcDecision evaluate(double powDb, const AgcGainState &g);
    // Result of the programGain that followed an apply decision: sdrplay_api_Success ->
    // wait for the toggle; sdrplay_api_GainUpdateError -> 20-block hold-off. Clears the
    // forced/user flags.
    void applied(int setGrRc, int appliedIfGr, std::uint8_t lnaState);

    // Introspection (tests, logging, overload context).
    int lastGr() const { return lastGr_; }
    int lastLna() const { return lastLna_; }
    int holdoff() const { return holdoff_; }
    double coeff() const { return coef_; }
    double coeffBase() const { return coefBase_; }
    double accFixed() const { return fixedAcc_; }
    double accCtrl() const { return ctrlAcc_; }
    CtrlPhase ctrlPhase() const { return phase_; }
    double attackCoeff() const { return attack_; }
    double decayCoeff() const { return decay_; }
    int decayCount() const { return dcnt_; }
    int decayReload() const { return dreload_; }
    bool forced() const { return forced_; }
    bool recomputePending() const { return recalc_; }

    // Test hook: load a given fixed-mode integrator and coefficient to check single
    // evaluations bit-exactly.
    void debugSetFixedState(double acc, double coeff) { fixedAcc_ = acc; coef_ = coeff; }
    struct CtrlState {
        double acc = 0.0, attack = 0.0, decay = 0.0;
        int dcnt = 0, reload = 0;
        std::uint32_t thresh = 0;
        CtrlPhase phase = CtrlPhase::Settled;
        int holdoff = 0;
        bool forced = false, userReq = false;
        int lastLna = -1, lastGr = -1;
    };
    void debugSetCtrlState(const CtrlState &c)
    {
        ctrlAcc_ = c.acc;
        attack_ = c.attack;
        decay_ = c.decay;
        dcnt_ = c.dcnt;
        dreload_ = c.reload;
        dthresh_ = c.thresh;
        phase_ = c.phase;
        holdoff_ = c.holdoff;
        forced_ = c.forced;
        userReq_ = c.userReq;
        lastLna_ = c.lastLna;
        lastGr_ = c.lastGr;
    }

private:
    void recompute(double fsHz, int spp, int numPackets);
    void computeAgcParams(double fsHz, int spp, int numPackets);
    AgcDecision evalFixed(double powDb, const AgcGainState &g);
    AgcDecision evalCtrlEn(double powDb, const AgcGainState &g);
    AgcDecision makeApply(int step, const AgcGainState &g);
    AgcDecision applyUnlessAtLimit(int step, const AgcGainState &g, bool lnaSame);
    bool holdingOff(); // counts the hold-off down; true while it has not run out

    AgcParams p_;
    int lastEnable_ = 0;
    bool recalc_ = false;
    int lastLna_ = -1;         // LNA state of the last applied gain (-1: none yet)
    int lastGr_ = -1;
    double fixedAcc_ = 0.0;    // fixed-mode integrator
    double ctrlAcc_ = 0.0;     // CTRL_EN integrator
    double coefBase_ = 0.29;   // unscaled coefficient
    double coef_ = 0.29;       // coefficient in use
    double attack_ = 0.0;
    double decay_ = 0.0;
    int dcnt_ = 0;
    int dreload_ = 0;
    std::uint32_t dthresh_ = 0;
    CtrlPhase phase_ = CtrlPhase::Settled;
    bool waitGain_ = false;    // gain applied, waiting for its confirmation
    int holdoff_ = 0;
    bool forced_ = false;      // the next evaluation applies unconditionally
    bool userReq_ = false;     // the next programGain reports to the user
    double lastFs_ = 0.0;
    bool haveFs_ = false;
};

// GainChange event (eventId 0) sent by programGain right after programming, before confirmation:
// gRdB = IF gain reduction actually programmed, lnaGRdB = total - IF,
// currGain = (double)gainVals.curr.
struct GainChangeEvent {
    unsigned gRdB = 0;
    unsigned lnaGRdB = 0;
    double currGain = 0.0;
};
GainChangeEvent makeGainChangeEvent(int appliedIfGr, int totalGr, float gainValsCurr);

} // namespace agc
} // namespace dsp
} // namespace sdrplay
