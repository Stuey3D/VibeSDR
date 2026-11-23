#pragma once
/**
 * vibe_sat_gain_airspy.h — THE SATELLITE-PASS GAIN HOLD ON AN AIRSPY R2 / MINI (V12 beta92).
 *
 * ★★★ WHY IT EXISTS. The XCover (Android server, Airspy R2, SuperScanStick vertical behind an FM filter), METEOR-M2-3,
 *     2026-11-23 19:26–19:38: the LRPT decoder "kept looking like it was going to work but nothing", and the owner saw
 *     the "airspy was bouncing lots". The log has NO gain write in the whole pass — the bouncing was the Airspy's OWN
 *     hardware AGC (the R820T2's LNA and mixer AGCs, the "Free" mode with its AGC switches on), which nothing logs. The
 *     beta91 hold (vibe_sat_gain.h) ran only inside the RTL's loop, so on this radio every AGC jump under a weak OQPSK
 *     signal cost the demodulator its lock, all pass long.
 *
 * ★★★ THE SAME PROFILE AS THE RTL's — vibesatgain::Hold, not a copy — on this radio's own ladder:
 *   1. BEGIN  the owner's whole gain state is kept (mode Free / Linear / Sensitive, both remembered preset positions, the
 *             three stages, both AGC switches). The hardware AGCs go OFF: the radio goes to ONE preset position on the
 *             band's curve (Linearity unless the owner's band rule names Sensitivity — vibe_airspy_limit.h), the nearest
 *             to the gain it was at (or Airspy's own "start here", 15, when its AGC was deciding — that gain is unknown).
 *   2. MEASURE kMeasureSec of the RAW converter's headroom (the 12-bit samples before libairspy's IQ converter — the
 *             VibeSDR patch airspy_vibe_set_adc_stats; the float IQ cannot say "on the rail", see airspy.c).
 *   3. SET    ONE preset: the highest whose predicted peak stays airspyRadio().targetPk under the rail, never above the
 *             owner's band ceiling.
 *   4. HOLD   for the pass. Only a genuine clip (two consecutive seconds with more than an impulse's rail samples) cuts
 *             it, downward. An impulse second is left out of the measurement (the RTL's beta92 spark rule).
 *   5. END    (LOS, the decoder stops, the pick goes): EXACTLY the owner's state again — mode, positions, stages, AGCs.
 *   ★ If the gain is changed by anything but the hold during the pass (a listener's slider, the owner's panel, a band
 *     rule on a retune), the hold STANDS ASIDE for the rest of the pass and the end leaves that newer choice alone: a
 *     person's later decision beats the owner's earlier one.
 *   ★ A LOCKED band (the owner's figure IS the setting): the hold does nothing and says so.
 *
 * ★★ THE LADDER IS AN ESTIMATE, said plainly. A preset sets three R820T2 stages at once (libairspy's own tables, below,
 *    copied from airspy.c). Their gains in dB come from librtlsdr's published R820T stage steps (the same tuner chip) and
 *    ≈ 3.5 dB per VGA step; the 22 linearity positions come out ≈ 3.8 dB apart (14 → 93 dB, monotone). The hold's
 *    prediction uses the DIFFERENCES only, and a misjudged step is corrected by the clip cut — but the set point is only
 *    as good as this table: an on-air check (peak after the set against the predicted figure in the log) is owed.
 *
 * Pure: no clock, no radio, no lock. The shim (LocalSdrShim::airspySatTick) feeds AirspyPass with the live
 * AirspySource; vibeserver/test-sat-gain.cpp feeds it a fake one.
 */
#include "vibe_sat_gain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace vibesatgain {

/** ★ 12-bit converter, peaks read off the raw samples: −10 dBFS, as the RSP's 12-bit one (the RTL's 8 bits need −8 —
 *  it has less to lose above the noise). */
inline Radio airspyRadio() { return {"Airspy R2/Mini", 0.0, -10.0, kMeasureSec}; }

/* ── THE PRESET LADDER ─────────────────────────────────────────────────────────────────────────────────────── */
constexpr int kAspPositions = 22;
constexpr int kAspModeSensitive = 0, kAspModeLinear = 1, kAspModeFree = 2;   // AirspySource::GainMode
// libairspy's tables (airspy.c), indexed by 21 − position exactly as airspy_set_linearity_gain / _sensitivity_gain do
constexpr uint8_t kAspLinVga[kAspPositions]  = {13, 12, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 8, 7, 6, 5, 4};
constexpr uint8_t kAspLinMix[kAspPositions]  = {12, 12, 11, 9, 8, 7, 6, 6, 5, 0, 0, 1, 0, 0, 2, 2, 1, 1, 1, 1, 0, 0};
constexpr uint8_t kAspLinLna[kAspPositions]  = {14, 14, 14, 13, 12, 10, 9, 9, 8, 9, 8, 6, 5, 3, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t kAspSenVga[kAspPositions]  = {13, 12, 11, 10, 9, 8, 7, 6, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4};
constexpr uint8_t kAspSenMix[kAspPositions]  = {12, 12, 12, 12, 11, 10, 10, 9, 9, 8, 7, 4, 4, 4, 3, 2, 2, 1, 0, 0, 0, 0};
constexpr uint8_t kAspSenLna[kAspPositions]  = {14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12, 12, 9, 9, 8, 7, 6, 5, 3, 2, 1, 0};
// the R820T's stage steps, tenths of a dB per step (librtlsdr tuner_r82xx.c — the same tuner chip as the R2's R820T2)
constexpr int kAspLnaStep[16] = {0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13};
constexpr int kAspMixStep[16] = {0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8};
constexpr int kAspVgaTenthPerStep = 35;   // ★ estimate — see the header note
/** The three stages' total, tenths of a dB on this table's own scale (only differences mean anything). */
inline int aspStagesTenth(int lna, int mixer, int vga) {
    lna = std::clamp(lna, 0, 15); mixer = std::clamp(mixer, 0, 15); vga = std::clamp(vga, 0, 15);
    int g = 0;
    for (int i = 1; i <= lna; ++i) g += kAspLnaStep[i];
    for (int i = 1; i <= mixer; ++i) g += kAspMixStep[i];
    return g + vga * kAspVgaTenthPerStep;
}
/** The 22 positions of a curve as gains, ascending (position = index), tenths of a dB. */
inline std::vector<int> airspyLadder(bool sensitivity) {
    std::vector<int> g;
    g.reserve(kAspPositions);
    for (int pos = 0; pos < kAspPositions; ++pos) {
        const int v = kAspPositions - 1 - pos;
        g.push_back(sensitivity ? aspStagesTenth(kAspSenLna[v], kAspSenMix[v], kAspSenVga[v])
                                : aspStagesTenth(kAspLinLna[v], kAspLinMix[v], kAspLinVga[v]));
    }
    return g;
}
inline int nearestIdx(const std::vector<int>& g, int tenth) {
    int best = 0;
    for (int i = 1; i < (int)g.size(); ++i) if (std::abs(g[(size_t)i] - tenth) < std::abs(g[(size_t)best] - tenth)) best = i;
    return best;
}

/* ── THE OWNER'S GAIN STATE ────────────────────────────────────────────────────────────────────────────────── */
/** Everything AirspySource keeps about the gain — what a pass must give back exactly. */
struct AirspyState {
    int  mode = kAspModeLinear;
    int  sensTenth = -1, linTenth = -1;   // each curve's remembered position, tenths (−1 = never set on it)
    int  gainTenth = 150;                 // the slider's figure
    int  lna = -1, mixer = -1, vga = -1;  // Free mode's stages (−1 = never set)
    bool lnaAgc = false, mixerAgc = false;
    int  tenthOn(int m) const { return m == kAspModeSensitive ? sensTenth : m == kAspModeLinear ? linTenth : -1; }
    bool operator==(const AirspyState& o) const {
        return mode == o.mode && sensTenth == o.sensTenth && linTenth == o.linTenth && gainTenth == o.gainTenth
            && lna == o.lna && mixer == o.mixer && vga == o.vga && lnaAgc == o.lnaAgc && mixerAgc == o.mixerAgc;
    }
    bool operator!=(const AirspyState& o) const { return !(*this == o); }
    /** "linearity preset 15" · "free: LNA 8 (AGC), mixer 5 (AGC), VGA 10" */
    std::string describe() const {
        char b[128];
        if (mode != kAspModeFree) {
            const int t = tenthOn(mode);
            if (t < 0) std::snprintf(b, sizeof b, "%s (no position set)", mode == kAspModeSensitive ? "sensitivity" : "linearity");
            else std::snprintf(b, sizeof b, "%s preset %d", mode == kAspModeSensitive ? "sensitivity" : "linearity", (t + 5) / 10);
        } else {
            std::snprintf(b, sizeof b, "free: LNA %d%s, mixer %d%s, VGA %d", std::max(0, lna), lnaAgc ? " (AGC)" : "",
                          std::max(0, mixer), mixerAgc ? " (AGC)" : "", std::max(0, vga));
        }
        return b;
    }
};
/** Where a pass starts measuring on `curve`'s ladder: the owner's own position on it; else the position nearest the
 *  gain the radio is at (the other curve, or Free with both AGCs off); else 15 (an AGC was deciding: no gain known —
 *  Airspy's own "start here"). Never above the owner's ceiling. */
inline int airspyStartPos(const AirspyState& s, bool sensCurve, int ceilPos) {
    const int curveMode = sensCurve ? kAspModeSensitive : kAspModeLinear;
    const std::vector<int> mine = airspyLadder(sensCurve);
    int pos = 15;
    if (s.mode == curveMode && s.tenthOn(curveMode) >= 0) pos = (s.tenthOn(curveMode) + 5) / 10;
    else if (s.mode != kAspModeFree && s.tenthOn(s.mode) >= 0) {
        const std::vector<int> theirs = airspyLadder(s.mode == kAspModeSensitive);
        pos = nearestIdx(mine, theirs[(size_t)std::clamp((s.tenthOn(s.mode) + 5) / 10, 0, kAspPositions - 1)]);
    } else if (s.mode == kAspModeFree && !s.lnaAgc && !s.mixerAgc) pos = nearestIdx(mine, aspStagesTenth(s.lna, s.mixer, s.vga));
    return std::clamp(pos, 0, std::clamp(ceilPos, 0, kAspPositions - 1));
}

/* ── THE RAW CONVERTER, A SECOND AT A TIME ─────────────────────────────────────────────────────────────────── */
/** ★ The RTL's kAdcRailNoise (8 rail samples a second) is ~6.7 per million of what its one-in-four examines at 2.4 MS/s;
 *  the Airspy's one-in-four sees ~5 M raw samples a second at 10 MS/s IQ, so the same impulse rule is a FRACTION here. */
constexpr double kAspRailNoiseFrac = 8.0 / 1.2e6;
struct AdcTake {
    int    seconds = 0;          // whole seconds folded in since the last take (0 = nothing to judge)
    double peak = -99.0;         // the loudest NON-impulse second's peak, dBFS
    int    sparks = 0;           // seconds whose peak was an impulse on the rail (left out of `peak`)
    double clipPct = 0.0;        // the worst second's % of samples on the rail
    int    clipRun = 0;          // consecutive clipping seconds, now
};
/** Fed buffer by buffer (the radio's consumer thread, under the source's own small lock); taken by the gain thread. */
class AdcMeter {
public:
    /** One scanned buffer: the raw 12-bit min / max, how many samples sat on a rail, of how many. */
    void add(uint16_t lo, uint16_t hi, uint32_t rails, uint32_t total, double now) {
        if (now < settleUntil_) return;                         // ★ straddling our own write: not evidence
        if (!started_) { started_ = true; secAt_ = now; }
        lo_ = std::min(lo_, lo); hi_ = std::max(hi_, hi); rails_ += rails; total_ += total;
        if (now - secAt_ < 1.0) return;
        secAt_ = now;
        if (total_ == 0) return;
        const int dev = std::max((int)hi_ - 2048, 2048 - (int)lo_);
        const double pk = dev > 0 ? 20.0 * std::log10(dev / 2048.0) : -99.0;
        const double pct = 100.0 * (double)rails_ / (double)total_;
        const bool clipping = (double)rails_ > std::max(8.0, kAspRailNoiseFrac * (double)total_);
        run_ = clipping ? run_ + 1 : 0;
        ++agg_.seconds;
        if (pk > -1.0 && !clipping) ++agg_.sparks;              // ★ an impulse: its peak says nothing about the band
        else agg_.peak = std::max(agg_.peak, pk);
        agg_.clipPct = std::max(agg_.clipPct, pct);
        agg_.clipRun = run_;
        lastPeak_ = pk;
        lo_ = 4095; hi_ = 0; rails_ = total_ = 0;
    }
    AdcTake take() { AdcTake t = agg_; agg_ = AdcTake{}; agg_.clipRun = run_; return t; }
    /** A gain write: start afresh after it has settled (the clip run too — the rail it counted was the old gain's). */
    void restart(double now, double settleSec = 0.25) {
        settleUntil_ = now + settleSec; started_ = false; secAt_ = 0; lo_ = 4095; hi_ = 0; rails_ = total_ = 0; run_ = 0; agg_ = AdcTake{};
    }
    double lastPeak() const { return lastPeak_; }
private:
    double settleUntil_ = -1e300, secAt_ = 0, lastPeak_ = -99.0;
    bool started_ = false;
    uint16_t lo_ = 4095, hi_ = 0;
    uint64_t rails_ = 0, total_ = 0;
    int run_ = 0;
    AdcTake agg_;
};

/* ── THE PASS ──────────────────────────────────────────────────────────────────────────────────────────────── */
/** What the shim knows each tick. */
struct AirspyCtx {
    bool on = false;            // g_satGainOn
    int  seq = 0;               // g_satGainSeq (a new value = measure again: AOS, the IF filter changed)
    double now = 0;
    std::string what;           // "METEOR-M2 3 LRPT"
    bool limited = false;       // the owner's band rule at the dial (vibe_airspy_limit.h)
    bool sensCurve = false;     //   …its curve
    int  ceilPos = kAspPositions - 1;
    bool locked = false;
};
/**
 * The driver. `Src` is the live AirspySource or a test's fake, with:
 *   AirspyState gainState() const · void setPreset(int mode, int tenth) · void restoreGainState(const AirspyState&)
 *   AdcTake takeAdc() · void setAdcStats(bool on, double now)   (on = measure, from a fresh start after a short settle:
 *   it is called again after every write of the hold's, so nothing measured at the old gain is judged at the new one)
 * step() returns the log lines to print and says whether it wrote the gain (the shim then tells the listeners).
 */
class AirspyPass {
public:
    struct Result { std::vector<std::string> log; bool wrote = false; };
    bool active() const { return active_; }
    bool yielded() const { return yielded_; }
    int  position() const { return pos_; }
    const AirspyState& owner() const { return owner_; }
    const Hold& hold() const { return hold_; }
    /** For the per-pass LRPT summary: "Airspy linearity preset 12 (held, 0 clip cuts)". Empty = no pass. */
    std::string note() const {
        if (!active_) return {};
        char b[160];
        if (locked_) { std::snprintf(b, sizeof b, "Airspy %s (the owner's locked band figure, not held)", owner_.describe().c_str()); return b; }
        if (yielded_) { std::snprintf(b, sizeof b, "Airspy changed by hand mid-pass (hold stood aside)"); return b; }
        std::snprintf(b, sizeof b, "Airspy %s preset %d (%s, %u clip cut%s)", curveName(), pos_,
                      hold_.phase() == Phase::Measure ? "measuring" : "held", hold_.cuts(), hold_.cuts() == 1 ? "" : "s");
        return b;
    }

    template <class Src>
    Result step(Src& s, const AirspyCtx& c) {
        Result r;
        char b[480];
        if (!c.on) {
            if (!active_) return r;
            if (locked_) {
                r.log.push_back("satellite pass over \xe2\x80\x94 the Airspy stayed at the owner's locked band figure");
            } else if (yielded_) {
                std::snprintf(b, sizeof b, "satellite pass over \xe2\x80\x94 the Airspy's gain stays as it was set during the pass (%s); "
                              "the owner's %s is not put back over a later choice", s.gainState().describe().c_str(), owner_.describe().c_str());
                r.log.push_back(b);
            } else {
                const AirspyState was = s.gainState();
                if (was != owner_) { s.restoreGainState(owner_); r.wrote = true; }
                std::snprintf(b, sizeof b, "satellite pass over \xe2\x80\x94 Airspy gain back to the owner's %s%s (was %s; %u set, %u clip cut%s "
                              "during the pass)", owner_.describe().c_str(),
                              owner_.mode == kAspModeFree && (owner_.lnaAgc || owner_.mixerAgc) ? ", its AGC running again" : "",
                              was.describe().c_str(), hold_.sets(), hold_.cuts(), hold_.cuts() == 1 ? "" : "s");
                r.log.push_back(b);
            }
            s.setAdcStats(false, c.now);
            hold_.end();
            active_ = yielded_ = locked_ = false;
            return r;
        }
        if (!active_) {
            active_ = true; yielded_ = false; seen_ = c.seq;
            owner_ = s.gainState();
            if (c.limited && c.locked) {
                locked_ = true;
                std::snprintf(b, sizeof b, "satellite pass (%s): the owner has LOCKED the Airspy's gain on this band (%s) \xe2\x80\x94 "
                              "it is the setting, so the satellite hold does not move it", c.what.c_str(), owner_.describe().c_str());
                r.log.push_back(b);
                return r;
            }
            locked_ = false;
            sens_ = c.limited && c.sensCurve;
            ladder_ = airspyLadder(sens_);
            ceil_ = c.limited ? std::clamp(c.ceilPos, 0, kAspPositions - 1) : kAspPositions - 1;
            pos_ = airspyStartPos(owner_, sens_, ceil_);
            const bool needWrite = owner_.mode != curveMode() || owner_.tenthOn(curveMode()) != pos_ * 10;
            if (needWrite) { s.setPreset(curveMode(), pos_ * 10); r.wrote = true; }
            s.setAdcStats(true, c.now);
            hold_ = Hold(airspyRadio());
            hold_.begin(c.now, c.what);
            const bool agc = owner_.mode == kAspModeFree && (owner_.lnaAgc || owner_.mixerAgc);
            std::snprintf(b, sizeof b, "satellite pass (%s): Airspy %s%s \xe2\x80\x94 %s %s preset %d; measuring the converter's headroom for "
                          "%.0f s, then held (clips only)%s", c.what.c_str(), owner_.describe().c_str(),
                          agc ? " \xe2\x80\x94 hardware AGC OFF for the pass" : "", needWrite ? "now" : "stays at", curveName(), pos_,
                          hold_.radio().measureSec, c.limited ? " within the owner's band ceiling" : "");
            r.log.push_back(b);
            return r;
        }
        if (locked_ || yielded_) return r;
        // ★ changed by anything but the hold: stand aside, and leave the newer choice at the end
        const AirspyState now = s.gainState();
        // (the AGC switches are only REMEMBERED in a preset mode — the radio's AGCs are off there, applyGainMode — so the
        //  mode and the position are the whole of what the radio is doing)
        if (now.mode != curveMode() || now.tenthOn(curveMode()) != pos_ * 10) {
            yielded_ = true;
            s.setAdcStats(false, c.now);
            std::snprintf(b, sizeof b, "satellite pass (%s): the Airspy's gain was changed during the pass (now %s, the hold had %s preset %d) "
                          "\xe2\x80\x94 the hold stands aside for the rest of the pass and leaves it as set", c.what.c_str(),
                          now.describe().c_str(), curveName(), pos_);
            r.log.push_back(b);
            return r;
        }
        if (c.seq != seen_) {
            seen_ = c.seq;
            hold_.remeasure(c.now);
            s.setAdcStats(true, c.now);
            std::snprintf(b, sizeof b, "satellite pass (%s): Airspy gain held at %s preset %d \xe2\x80\x94 measuring the headroom again "
                          "(AOS, or the IF filter changed)", c.what.c_str(), curveName(), pos_);
            r.log.push_back(b);
            return r;
        }
        const AdcTake t = s.takeAdc();
        if (t.seconds == 0) return r;
        In in;
        in.now = c.now;
        in.peak = t.peak;
        in.spark = t.peak <= -98.0 && t.sparks > 0;    // every second since the last tick was an impulse
        in.clipPct = t.clipPct;
        in.clipRun = t.clipRun;
        in.gains = &ladder_; in.curIdx = pos_; in.ceilIdx = ceil_;
        const int from = pos_;
        const Out o = hold_.tick(in);
        if (o.log.empty()) return r;
        if (o.kind != Out::None && o.idx >= 0 && o.idx != pos_) {
            pos_ = o.idx;
            s.setPreset(curveMode(), pos_ * 10);
            s.setAdcStats(true, c.now);
            r.wrote = true;
        }
        if (o.kind == Out::ClipCut) {
            std::snprintf(b, sizeof b, "satellite pass (%s): Airspy on the rail (%.3f %% of samples, %d s) \xe2\x80\x94 %s preset %d -> %d "
                          "(about %.1f dB down); %s", c.what.c_str(), t.clipPct, t.clipRun, curveName(), from, pos_,
                          (ladder_[(size_t)from] - ladder_[(size_t)pos_]) / 10.0,
                          hold_.phase() == Phase::Measure ? "measuring again from there" : "still held (clips only)");
        } else {
            const Choice& ch = hold_.lastChoice();
            const double measured = ch.predictedPk - (ladder_[(size_t)pos_] - ladder_[(size_t)from]) / 10.0;
            std::snprintf(b, sizeof b, "satellite pass (%s): Airspy gain held at %s preset %d%s \xe2\x80\x94 peak %.1f dBFS at preset %d over "
                          "%.0f s, %.1f dBFS predicted (set point %.0f dBFS; decided by %s; about %+.1f dB); hardware AGC off, clips only",
                          c.what.c_str(), curveName(), pos_, pos_ == from ? " (unchanged)" : "", measured, from, hold_.radio().measureSec,
                          ch.predictedPk, hold_.radio().targetPk, ch.limit, (ladder_[(size_t)pos_] - ladder_[(size_t)from]) / 10.0);
        }
        r.log.push_back(b);
        return r;
    }

private:
    Hold hold_{airspyRadio()};
    AirspyState owner_;
    std::vector<int> ladder_;
    bool active_ = false, yielded_ = false, locked_ = false, sens_ = false;
    int  pos_ = 15, ceil_ = kAspPositions - 1, seen_ = -1;
    int  curveMode() const { return sens_ ? kAspModeSensitive : kAspModeLinear; }
    const char* curveName() const { return sens_ ? "sensitivity" : "linearity"; }
};

}  // namespace vibesatgain
