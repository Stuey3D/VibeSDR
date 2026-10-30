// ★★★ VibeAGC FOR SDRplay — ONE LOOP THAT OWNS BOTH GAIN STAGES OF AN RSP (2026-10-30, V12).
//
// Stuart, 2026-10-30: "one that we couldnt do before due to the SDRPlay API being garbage, but now we use
// OpenRSP which is more responsive … VibeAGC for SDRPlay a combined IF/RF AGC system completely controlled
// by us. The issue we had before was too many commands to the SDR kept locking up the SDRPlay API, we now
// have OpenRSP which may be a lot more resiliant".
//
// ★★ WHY NOW AND NOT ON 09-13. VibeAGC drove gRdB + the LNA from outside once before (57b87900 removed it):
//    under the OFFICIAL service, writing gain several times a second wedged it — writes no longer honoured,
//    a readback that lied, only a reboot cleared it. Two things are different now:
//      · OpenRSP. The 10-20 A/B on the Lenovo: 150 min, 6566 retunes, 2773 DAB channel changes, 395 RF
//        steps — 0 hangs, 0 API errors; the official 3.15 hung 3 times in 17 min of the same script.
//      · A COMMAND BUDGET, which the 09-12 VibeAGC never had. It wrote whenever it liked; this one writes the
//        IF at most every 250 ms (≤ 4/s), the LNA at most every 1.5 s, never more than kMaxWritesPerSec in any
//        second, and only on a change past hysteresis — and counts every write so the rate is a logged fact.
//    So it is ON BY DEFAULT ONLY UNDER OpenRSP. Under the official API the old arrangement stays (their IF
//    AGC + our RF walker) unless the owner turns this on knowingly (vibe_sdrplay_lib.h: vagcDefaultOn).
//
// ★★★ WHAT IT FIXES. The hardware IF AGC's readout comes through an EVENT PATH THAT CAN FREEZE (09-13,
//     10-16, 10-30): every RF rule that steered by it inherited the freeze, and on 10-30 the RF stage stuck
//     at FULL gain on Band II next to a +20 dB neighbour — MPX S/N 10 vs the V4's 16, RDS 100 % errors,
//     spurious carriers the V4 does not show. Here nothing is read back to steer by:
//       · the MEASURE is OUR OWN ADC PEAK (the 0.1 s windows streamCb fills — never the API's event path);
//       · the IF REDUCTION is the number WE wrote, so it cannot be stale;
//       · and gRdB, being ours, IS the front-end load meter: the ADC sees Pin + G_lna − gRdB, so with the peak
//         held at target, gRdB = Pin + G_lna − target. A high reduction means the mixer is being driven hard
//         — the IF window rule (Stuart's 30-50/52, rsp_agc_architecture_2026_09_13) restated in a number
//         that cannot freeze.
//
// ★★★ THE SPLIT RULE:
//   1. IF — the fine control. Holds the ADC peak on the profile's target with a dead band, in SMALL steps
//      (≤ 2 dB normally) that the GainStepSmoother cancels exactly (we know the dB and the packet), so an IF
//      move is inaudible (Stuart: "the IF agc must not cause audio dropouts like the SDRPlay own IF agc").
//      Attack is fast (overload is destructive and instant); decay is slow and reluctant (the LOUDEST window
//      of the last second must be under target before any gain is added back) — the asymmetry 08-05 kept.
//   2. RF — protection first. A rung of RF comes OUT when the front end is loaded: the IF reduction sits
//      above the profile's grHigh (sustained), or the IF is at its 59 rail with the ADC still hot, or the
//      ADC clips / the API's overload flag is corroborated by the samples. That is regardless of how weak
//      the wanted station is — intermod made in the LNA cannot be removed by the IF afterwards.
//   3. RF goes back IN only when the IF has room for the WHOLE rung with margin (gRdB + rung ≤ grHigh −
//      kAddMarginDb) for rfAddDwellSec, nothing has clipped meanwhile, the rung is not one that overloaded
//      recently (a ceiling that DECAYS — never a permanent limit), and it is not an immediate reversal.
//      ★ So the window is ALWAYS wider than the step (agc_window_narrower_than_step): with the learned per-
//        band ladder (rsp_lna_states_are_per_band — LEARNED from operation, never swept) and the published
//        prior below when a rung has not been seen yet.
//   4. AN RF STEP IS ONE WRITE THAT MOVES BOTH. The LNA rung and an IF counter-step that cancels as much of
//      it as the IF's range allows go in the SAME Update(Tuner_Gr) — gRdB and LNAstate are one struct. So the
//      ADC level barely moves on a rung (residual = rung − IF counter-step, usually 0-2 dB) and the smoother
//      only has the switching transient to hold. That is the best shot at "make the RF AGC move silent too".
//
// ★ Profiles by MODE (the same bandwidth-first spirit as AgcProfile, agc_band_profiles): FM broadcast, DAB,
//   AM/MW, HF SSB/CW, VHF/UHF NFM. DAB freezes the IF inside a wide band (no steps inside OFDM symbols —
//   rsp_rf_agc_walks_lna_in_dab_2026_09_15: the IF AGC's ms steps were what erased frames).
//
// Pure: no clock of its own, no radio. The shim feeds tick() and performs what it returns; the closed-loop
// proof is vibeserver/test-rsp-vibeagc.cpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

namespace vibersp {

/* ── PROFILES ─────────────────────────────────────────────────────────────────────────────────────── */
enum class VagcBand { FmBroadcast, Dab, AmMw, HfSsb, Nfm };

struct VagcProfile {
    const char* name;
    double targetPk;      // dBFS: where the IF holds the 0.1 s ADC peak (peak reads ~10 dB over an average)
    double deadDb;        // ± target: no IF write inside this (the hysteresis on the fine control)
    double maxIfStepDb;   // an ordinary IF write, so the smoother cancels it exactly and nobody hears it
    double attackPk;      // above this the IF may take a bigger step at once (and the 59 rail cuts RF)
    double grHigh;        // our gRdB above this, sustained, = the front end is loaded → one rung of RF out
    double rfCutDwellSec; // how long over grHigh before that rung comes out
    double rfAddDwellSec; // how long the IF must have had room for a whole rung before one goes back in
    bool   ifFreeze;      // DAB: the IF moves only when the peak leaves [freezeLo, freezeHi]
    double freezeLo, freezeHi;
};

/* ★ Targets: -20 dBFS peak is the IF AGC's -30 set point + the ~10 dB our peak reads over it (measured in DAB
 *   09-15), and Stuart's own DAB IF target. grHigh is the 52 trigger his window rule settled on; with the 6 dB
 *   add margin a 6 dB rung gives exactly his 40/52 resting band, and a 19 dB HF rung a 27/52 one — always wider
 *   than the step. DAB's 55 is the 09-16 suite's "IF >= 55 = too much RF". */
inline VagcProfile vagcProfile(VagcBand b) {
    switch (b) {
        case VagcBand::FmBroadcast: return {"FM broadcast", -20.0, 4.0, 2.0, -6.0, 52.0, 1.5, 8.0, false, 0, 0};
        case VagcBand::Dab:         return {"DAB",          -20.0, 6.0, 10.0, -6.0, 55.0, 1.5, 8.0, true, -34.0, -8.0};
        case VagcBand::AmMw:        return {"AM/MW",        -20.0, 5.0, 2.0, -6.0, 52.0, 2.0, 10.0, false, 0, 0};
        case VagcBand::HfSsb:       return {"HF SSB/CW",    -20.0, 5.0, 2.0, -6.0, 52.0, 2.0, 10.0, false, 0, 0};
        case VagcBand::Nfm:         return {"VHF/UHF NFM",  -20.0, 4.0, 2.0, -6.0, 52.0, 1.5, 8.0, false, 0, 0};
    }
    return {"FM broadcast", -20.0, 4.0, 2.0, -6.0, 52.0, 1.5, 8.0, false, 0, 0};
}

/** ★ By mode string and DAB flag — the shim's mode names. Wide FM is the broadcast band; everything narrow above
 *  30 MHz that is not AM is NFM-like; below 30 MHz AM is MW/SW broadcast and the rest is SSB/CW/data. */
inline VagcBand vagcBandFor(const std::string& mode, bool dab, double hz) {
    if (dab) return VagcBand::Dab;
    if (mode == "wfm" || mode == "fm" || mode == "wfm-st") return VagcBand::FmBroadcast;
    if (mode == "am" || mode == "sam" || mode == "ecss" || mode == "drm") return hz < 30e6 ? VagcBand::AmMw : VagcBand::Nfm;
    if (hz < 30e6) return VagcBand::HfSsb;
    return VagcBand::Nfm;
}

/* ── THE LNA LADDER PRIOR ────────────────────────────────────────────────────────────────────────────────
 * ★★ Used ONLY for a rung never yet seen in this band — the learned figure (SdrplaySource::lnaGainDb, from
 *    gainVals.curr + our gRdB after a write lands) replaces it the moment there is one. Reductions in dB per
 *    state, RSP1A/1B. Band II (60-250 MHz) is the ladder MEASURED in the 10-30 journal (test-rsp-fm-rail-sim);
 *    the others are SDRplay's published gain-reduction tables (API specification). A wrong prior costs one
 *    mis-sized counter-step that the smoother still measures and cancels; it cannot hunt, because the add
 *    rule demands room for the whole rung PLUS kAddMarginDb, and the rung is learned once it has been taken. */
inline double vagcPriorRed(int band, int state) {
    static const double b0[] = {0, 6, 12, 18, 37, 42, 61};                    // < 60 MHz
    static const double b1[] = {0, 6.3, 12.6, 18.9, 37, 42, 61, 66, 72, 77};  // 60-250 (Band II measured)
    static const double b2[] = {0, 6, 12, 18, 20, 26, 32, 38, 57, 62};        // 250-420
    static const double b3[] = {0, 7, 13, 19, 20, 27, 33, 39, 45, 64};        // 420-1000
    static const double b4[] = {0, 6, 12, 20, 26, 32, 38, 43, 62};            // > 1000
    const double* t; int n;
    switch (band) { case 0: t = b0; n = 7; break; case 1: t = b1; n = 10; break; case 2: t = b2; n = 10; break;
                    case 3: t = b3; n = 10; break; default: t = b4; n = 9; break; }
    if (state < 0) state = 0;
    if (state >= n) return t[n - 1] + 6.0 * (state - n + 1);   // a model with more states: assume 6 dB a rung
    return t[state];
}

/* ── WHAT THE SHIM HANDS IN, AND WHAT IT GETS BACK ──────────────────────────────────────────────────── */
struct VagcIn {
    double now = 0.0;          // seconds, monotonic
    unsigned windows = 0;      // SdrplaySource::adcWindows() — a new value = a new 0.1 s window closed
    double peak = -99.0;       // that window's peak, dBFS
    double clipPct = 0.0;      // fraction of samples at the rail
    bool   overload = false;   // SdrplaySource::overloadReal() — the API's flag, corroborated by the samples
    int    nStates = 10;       // LNA states in this band
    int    lnaFloor = 0;       // the owner's RF cap as the lowest state allowed (0 = full RF gain allowed)
    bool   rfOwned = true;     // false = the owner runs the LNA by hand; we keep to the IF
    int    band = 1;           // SdrplaySource::lnaBandId
    double learnedRed[28];     // learned LNA reduction per state in this band (gain(0) − gain(s)); NaN unknown
    VagcIn() { for (double& d : learnedRed) d = NAN; }
};

struct VagcOut {
    bool   write = false;      // perform ONE Update(Tuner_Gr) with these two
    int    lna = 0, gr = 59;
    bool   lnaChanged = false;
    double expectDb = 0.0;     // the level change the write should cause at the ADC (+ = louder); NaN if unknown
    double ifDb = 0.0;         // the IF part of it, exactly known
    char   why[200] = {0};
};

/* ── THE LOOP ──────────────────────────────────────────────────────────────────────────────────────────── */
class VibeAgcRsp {
public:
    static constexpr double kIfMinSec       = 0.25;   // ≤ 4 IF writes a second
    static constexpr double kIfUpMinSec     = 0.6;    // gain going UP (less reduction) is reluctant
    static constexpr double kLnaMinSec      = 1.5;    // ≤ 1 LNA write per 1.5 s
    static constexpr int    kMaxWritesPerSec = 5;     // hard cap over ANY one-second span — never a burst
    static constexpr double kAddMarginDb    = 6.0;    // a rung goes back only with this much IF room to spare
    static constexpr double kReverseSec     = 20.0;   // an add straight after a cut (or the reverse) waits this
    static constexpr double kCeilingSec     = 90.0;   // a rung that overloaded is refused this long (decays)
    static constexpr int    kSettleWindows  = 2;      // windows to discard after a write (one straddles it)
    static constexpr double kClipAttackDb   = 12.0;   // the IF step when the converter clips (the peak is blind)
    static constexpr double kAcqSettleSec   = 3.0, kAcqMaxSec = 40.0;   // acquisition: on target this long, or give up
    static constexpr double kAcqAddDwellSec = 2.0;    // …and a rung goes back after this much room, not 8-10 s
    static constexpr double kAcqIfStepDb    = 6.0;    // …and the IF climbs in up to 6 dB (the smoother measures those)
    static constexpr int    kGrMin = 20, kGrMax = 59;

    void setProfile(VagcBand b) {
        if (b == band_ && haveProfile_) return;
        band_ = b; p_ = vagcProfile(b); haveProfile_ = true;
        forgetJudgements();
    }
    const VagcProfile& profile() const { return p_; }
    /** The gain the radio is at (ours — call once at take-over, and after anything else wrote it). Starts ACQUIRING. */
    void adopt(int lna, int gr) { lna_ = lna; gr_ = std::clamp(gr, kGrMin, kGrMax); acquiring_ = true; acqStart_ = -1; inBandSince_ = -1; }
    /** ★ True until the level has first sat on target for kAcqSettleSec (or kAcqMaxSec has passed) after a take-over or a
     *  band change. While acquiring, gain comes UP briskly — the cold start from minimum RF on a weak band took ~100 s at
     *  the steady-state pace (measured, case B) — and everything else is unchanged: attack, budgets, hysteresis. */
    bool acquiring() const { return acquiring_; }
    /** A retune / rate change / DAB block hop: the windows in flight describe the old capture. Gains are kept. */
    void noteRetune(double now, bool bandChanged) {
        hist_.clear(); waitWindowsTo_ = 0; needFresh_ = true; retuneAt_ = now;
        if (bandChanged) { forgetJudgements(); acquiring_ = true; acqStart_ = -1; inBandSince_ = -1; }
    }
    int lna() const { return lna_; }
    int gr()  const { return gr_; }
    /* write accounting — the budget is a logged fact, not a hope */
    unsigned writes() const { return writes_; }
    unsigned ifWrites() const { return ifWrites_; }
    unsigned lnaWrites() const { return lnaWrites_; }
    int maxWritesInAnySecond() const { return maxPerSec_; }

    VagcOut tick(const VagcIn& in) {
        VagcOut o; o.lna = lna_; o.gr = gr_;
        // ── fresh measurement only ──
        if (in.windows == lastWindows_) return o;
        lastWindows_ = in.windows;
        if (needFresh_) { waitWindowsTo_ = in.windows + 1; needFresh_ = false; }
        if (in.windows < waitWindowsTo_) return o;                // straddles our write or a retune
        if (!std::isfinite(in.peak) || in.peak <= -98.0) return o;
        hist_.push_back({in.now, in.peak, in.clipPct > 0.0 || in.overload});
        while (hist_.size() > 20) hist_.pop_front();
        const bool clipping = in.clipPct > 0.0 || in.overload || in.peak > -1.0;

        // levels: attack on the last 2 windows, decay on the loudest of the last 10 (1 s)
        double atk = -99, dec = -99; int n = 0;
        for (auto it = hist_.rbegin(); it != hist_.rend(); ++it, ++n) {
            if (n < 2) atk = std::max(atk, it->pk);
            if (n < 10) dec = std::max(dec, it->pk);
        }
        const bool haveSecond = hist_.size() >= (acquiring_ ? 3u : 10u);
        if (clipping) { lastClipAt_ = in.now; }
        // ── acquisition ends once the level has sat on target for a while ──
        if (acquiring_) {
            if (acqStart_ < 0) acqStart_ = in.now;
            const bool inBand = std::fabs(atk - p_.targetPk) <= p_.deadDb;
            if (inBand) { if (inBandSince_ < 0) inBandSince_ = in.now; } else inBandSince_ = -1;
            if ((inBandSince_ >= 0 && in.now - inBandSince_ >= kAcqSettleSec) || in.now - acqStart_ >= kAcqMaxSec) acquiring_ = false;
        }
        const double addDwell = acquiring_ ? kAcqAddDwellSec : p_.rfAddDwellSec;

        // ── the front-end judgements, on OUR gRdB ──
        const bool loaded = gr_ > p_.grHigh;
        if (loaded) { if (loadedSince_ < 0) loadedSince_ = in.now; } else loadedSince_ = -1;
        const double rungUp = rungDbFor(in, lna_ - 1, lna_);     // what one more rung of RF gain adds
        const bool roomForRung = lna_ - 1 >= in.lnaFloor
                              && (gr_ + rungUp) <= p_.grHigh - kAddMarginDb
                              && dec < p_.targetPk + p_.deadDb;   // and the band is not loud right now
        if (roomForRung && !clipping) { if (roomSince_ < 0) roomSince_ = in.now; } else roomSince_ = -1;
        if (ceilingState_ >= 0 && in.now - ceilingAt_ > kCeilingSec) ceilingState_ = -1;   // ★ decays

        // ── 1. RF OUT: protection first ──
        if (in.rfOwned && lna_ + 1 < in.nStates && lnaAllowed(in.now)) {
            const bool swamped = gr_ >= kGrMax && atk > p_.attackPk;      // IF has nothing left and still hot
            // ★ clipping with less IF left than a clip-attack would take: cut the RF NOW, in the same write as the IF
            const bool clipCut = clipping && gr_ + kClipAttackDb > kGrMax;
            // ★ the reversal guard is for the OPPOSITE direction: a cut straight after an ADD waits; a second cut
            //   straight after a cut does not (that is a front end still loaded — walk on, at the LNA's own pace)
            const bool loadCut = loaded && loadedSince_ >= 0 && in.now - loadedSince_ >= p_.rfCutDwellSec
                              && !(lastLnaDir_ > 0 && in.now - lastLnaAt_ < kReverseSec);
            if (swamped || clipCut || loadCut) {
                const int to = lna_ + 1;
                const double rung = rungDbFor(in, lna_, to);   // gain lost
                // ★ counter-step: give back as much of the rung on the IF as its range allows, so the ADC
                //   barely moves — EXCEPT when we are cutting because the converter itself is hot, where the
                //   level SHOULD drop: then aim the IF at the target, not at the old level.
                int grTo;
                if (swamped || clipCut) grTo = clampGr((int)std::lround(gr_ - rung + (atk - p_.targetPk)));
                else                    grTo = clampGr((int)std::lround(gr_ - rung));
                if (swamped || clipCut) { ceilingState_ = lna_; ceilingAt_ = in.now; }   // this rung overloaded
                std::snprintf(o.why, sizeof o.why, "%s — RF gain state %d -> %d (rung %.1f dB), IF %d -> %d dB "
                              "in the same write (ADC peak %.1f dBFS)",
                              swamped ? "IF at its 59 rail and the converter still hot" :
                              clipCut ? "converter clipping with the IF nearly spent" :
                                        "IF reduction held over the front-end load line",
                              lna_, to, rung, gr_, grTo, atk);
                emit(o, in, to, grTo, -rung, -1);
                return o;
            }
        }
        // ── 2. RF IN: only with room for the whole rung, sustained, and not into a recent overload ──
        if (in.rfOwned && lna_ - 1 >= in.lnaFloor && lnaAllowed(in.now) && haveSecond
            && roomSince_ >= 0 && in.now - roomSince_ >= addDwell
            && !(ceilingState_ >= 0 && lna_ - 1 <= ceilingState_)          // ★ not back into a rung that overloaded
            && !(lastLnaDir_ < 0 && in.now - lastLnaAt_ < kReverseSec)     // ★ an add straight after a CUT waits
            && in.now - lastClipAt_ > addDwell) {
            const int to = lna_ - 1;
            const double rung = rungDbFor(in, to, lna_);
            const int grTo = clampGr((int)std::lround(gr_ + rung));
            std::snprintf(o.why, sizeof o.why, "the IF has had room for a whole rung for %.0f s — RF gain state "
                          "%d -> %d (rung %.1f dB), IF %d -> %d dB in the same write (ADC peak %.1f dBFS)",
                          in.now - roomSince_, lna_, to, rung, gr_, grTo, dec);
            emit(o, in, to, grTo, +rung, +1);
            return o;
        }
        // ── 3. IF: the fine control ──
        if (p_.ifFreeze) {
            // DAB: hold still inside the band; outside it, one write straight back to target
            const bool out = atk > p_.freezeHi || clipping || (haveSecond && dec < p_.freezeLo);
            if (!out || !ifAllowed(in.now, atk > p_.targetPk)) return o;
            const double want = (atk > p_.targetPk ? atk : dec) - p_.targetPk;
            int grTo = clampGr((int)std::lround(gr_ + std::clamp(want, -p_.maxIfStepDb, p_.maxIfStepDb)));
            if (grTo == gr_) return o;
            std::snprintf(o.why, sizeof o.why, "DAB: ADC peak %.1f dBFS left the %.0f..%.0f band — IF %d -> %d dB",
                          want > 0 ? atk : dec, p_.freezeLo, p_.freezeHi, gr_, grTo);
            emit(o, in, lna_, grTo, -(grTo - gr_), 0);
            return o;
        }
        const double eAtk = atk - p_.targetPk;
        if (eAtk > p_.deadDb || clipping) {                        // too hot: less gain, promptly
            if (!ifAllowed(in.now, true) || gr_ >= kGrMax) return o;
            const bool urgent = clipping || atk > p_.attackPk;
            const double stepMax = urgent ? 6.0 : p_.maxIfStepDb;
            // ★ a clipped peak says nothing about HOW far over it is: take kClipAttackDb at once
            const int step = clipping ? (int)kClipAttackDb
                                      : (int)std::clamp(std::ceil(eAtk - p_.deadDb * 0.5), 1.0, stepMax);
            const int grTo = clampGr(gr_ + step);
            if (grTo == gr_) return o;
            std::snprintf(o.why, sizeof o.why, "ADC peak %.1f dBFS over the %.0f target%s — IF %d -> %d dB",
                          atk, p_.targetPk, clipping ? " (clipping)" : "", gr_, grTo);
            emit(o, in, lna_, grTo, -(double)(grTo - gr_), 0);
            return o;
        }
        const double eDec = dec - p_.targetPk;
        if (haveSecond && eDec < -p_.deadDb) {                     // quiet for a whole second: more gain, gently
            if (!ifAllowed(in.now, false) || gr_ <= kGrMin) return o;
            const int step = (int)std::clamp(std::floor((-eDec - p_.deadDb * 0.5) * (acquiring_ ? 1.0 : 0.5)), 1.0,
                                             acquiring_ ? kAcqIfStepDb : p_.maxIfStepDb);
            const int grTo = clampGr(gr_ - step);
            if (grTo == gr_) return o;
            std::snprintf(o.why, sizeof o.why, "loudest window of the last second %.1f dBFS, under the %.0f target "
                          "— IF %d -> %d dB", dec, p_.targetPk, gr_, grTo);
            emit(o, in, lna_, grTo, (double)(gr_ - grTo), 0);
            return o;
        }
        return o;
    }

    /** The rung between two states in dB of gain (positive): learned where seen (the truth for this radio),
     *  else the prior, which only fills gaps. */
    static double rungDbFor(const VagcIn& in, int hiGainState, int loGainState) {
        if (hiGainState < 0) hiGainState = 0;
        auto red = [&](int s) {
            const double l = (s >= 0 && s < 28) ? in.learnedRed[s] : NAN;
            return std::isfinite(l) ? l : vagcPriorRed(in.band, s);
        };
        const double d = red(loGainState) - red(hiGainState);
        return std::isfinite(d) && d > 0.5 ? d : 6.0;
    }

private:
    struct Win { double t, pk; bool clip; };
    std::deque<Win> hist_;
    VagcBand band_ = VagcBand::FmBroadcast; VagcProfile p_ = vagcProfile(VagcBand::FmBroadcast); bool haveProfile_ = false;
    int lna_ = 0, gr_ = 59;
    unsigned lastWindows_ = 0, waitWindowsTo_ = 0; bool needFresh_ = true;
    double lastIfAt_ = -1e9, lastLnaAt_ = -1e9, lastClipAt_ = -1e9, retuneAt_ = -1e9;
    int lastLnaDir_ = 0;                                   // +1 = gain added, -1 = cut
    double loadedSince_ = -1, roomSince_ = -1;
    int ceilingState_ = -1; double ceilingAt_ = -1e9;
    bool acquiring_ = true; double acqStart_ = -1, inBandSince_ = -1;
    std::deque<double> writeTimes_;
    unsigned writes_ = 0, ifWrites_ = 0, lnaWrites_ = 0; int maxPerSec_ = 0;

    void forgetJudgements() { loadedSince_ = roomSince_ = -1; ceilingState_ = -1; lastLnaDir_ = 0; }
    static int clampGr(int g) { return std::clamp(g, kGrMin, kGrMax); }
    bool budgetOk(double now) {
        while (!writeTimes_.empty() && now - writeTimes_.front() >= 1.0) writeTimes_.pop_front();
        return (int)writeTimes_.size() < kMaxWritesPerSec;
    }
    bool ifAllowed(double now, bool down) {
        const double gap = (down || acquiring_) ? kIfMinSec : kIfUpMinSec;
        return now - lastIfAt_ >= gap && now - lastLnaAt_ >= kIfMinSec && budgetOk(now);
    }
    bool lnaAllowed(double now) { return now - lastLnaAt_ >= kLnaMinSec && now - lastIfAt_ >= kIfMinSec && budgetOk(now); }
    void emit(VagcOut& o, const VagcIn& in, int lna, int gr, double expectDb, int lnaDir) {
        o.write = true; o.lna = lna; o.gr = gr; o.lnaChanged = lna != lna_;
        o.ifDb = -(double)(gr - gr_);
        o.expectDb = o.lnaChanged ? (expectDb + o.ifDb) : o.ifDb;
        if (o.lnaChanged) { lastLnaAt_ = in.now; lastLnaDir_ = lnaDir; ++lnaWrites_; roomSince_ = -1; loadedSince_ = -1; }
        if (gr != gr_) { lastIfAt_ = in.now; ++ifWrites_; }
        if (o.lnaChanged && gr == gr_) lastIfAt_ = std::max(lastIfAt_, in.now - kIfMinSec + 0.25);
        lna_ = lna; gr_ = gr; ++writes_;
        writeTimes_.push_back(in.now);
        maxPerSec_ = std::max(maxPerSec_, (int)writeTimes_.size());
        waitWindowsTo_ = in.windows + kSettleWindows;
        hist_.clear();
    }
};

}  // namespace vibersp
