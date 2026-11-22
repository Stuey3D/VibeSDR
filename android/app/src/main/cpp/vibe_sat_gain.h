#pragma once
/**
 * vibe_sat_gain.h — THE SATELLITE-PASS GAIN PROFILE (V12 beta91, docs/v12/LRPT-LIVE-PASS.md "SATELLITE GAIN").
 *
 * ★★★ WHY IT EXISTS. Stuart's Pi 500, RTL-SDR Blog V4, METEOR-M2 4 on 2026-11-22: during a live LRPT decode the RTL's
 *     VibeAGC stepped the gain 9 times in 10 minutes — "converter hot" cuts with the peak nowhere near the rail, "still
 *     spraying, going down again", "still chasing what the front end is making", and "clean for 8 s — gain back up"
 *     climbs in between. At 14:28:20.5 UTC one cut took 5.6 dB ("that 1.4 dB cut took 4.3 dB off the neighbourhood —
 *     still spraying, going down again … gain 32.8 dB"), the satellite sank into the noise and the decode stopped.
 *     Every one of those heuristics was written for a BROADCAST listener, where the wanted signal is strong and the harm
 *     is intermodulation round it. A satellite is the opposite: a weak signal that the demodulator is tracking, and
 *     EVERY gain step is a level jolt it has to ride out. What a pass wants is one good fixed gain, decided once.
 *
 * ★★★ THE PROFILE, in three phases (Hold below; the shim feeds it and performs what it returns):
 *   1. MEASURE — when a satellite picture / telemetry decoder starts on a pick (and again at AOS if the pick was armed
 *      before the rise): kMeasureSec of the converter's own headroom at the gain the radio is at. Nothing moves, except
 *      that a converter ALREADY ON THE RAIL is shed at once (a clipped peak measures nothing) and the window restarts.
 *   2. SET — ONE write: the highest gain whose predicted peak stays the radio's margin below the rail (chooseGain), or —
 *      where the radio's own noise floor is known — no higher than lifts the band's floor kFloorLiftDb above it (more
 *      gain past that buys no signal-to-noise, only less headroom). Never above the owner's ceiling.
 *   3. HOLD — for the rest of the pass. No climb, no spur chasing, no "converter hot". ONLY genuine clipping (the
 *      radio's own hard-overload test: two consecutive seconds on the rail) steps it down, sized as the ordinary loop
 *      sizes an overload (vibeagc::grossOverloadShedDb).
 *   The shim ends it when the decoder stops, the pick goes, or at LOS; the ordinary automatic gain resumes from where
 *   the pass left it (it re-learns the channel — nothing judged before the pass is judged after it).
 *
 * ★★ TRADE-OFFS, said plainly:
 *   · A strong terrestrial signal that appears mid-pass (a pager, a nearby transmitter keying up) can still force a
 *     step down — that is the one move the hold allows, because a railing converter destroys the satellite too.
 *   · The measurement window costs kMeasureSec at the start (the gain the radio was at stands meanwhile).
 *   · A fixed gain cannot follow a band that gets louder or quieter during the pass: it trades tracking for stillness.
 *     The margin is the price of that — the RTL's set point is 8 dB under the rail, against VibeAGC's adaptive −6 dBFS.
 *   · The prediction is linear (+1 dB of gain = +1 dB of peak), which holds below compression — exactly where the
 *     margin keeps it.
 *
 * Pure: no clock of its own, no radio — vibeserver/test-sat-gain.cpp drives it.
 */
#include "vibe_agc_rules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vibesatgain {

/* ── THE RADIO'S NUMBERS ─────────────────────────────────────────────────────────────────────────────────── */
/** ★ Per radio, because the three do not share a converter: the RTL's is 8 bits with nothing to spare, the RSP's is
 *  12-14 bits and its peaks are read on 0.1 s windows. railDbfs = where the converter clips (0 dBFS on both: the
 *  peak is measured as a fraction of full scale); targetPk = where the pass's peak is SET. */
struct Radio {
    const char* name;
    double railDbfs;      // the converter's full scale
    double targetPk;      // the pass's operating point: the measured peak is put here
    double measureSec;    // the headroom window
};
constexpr double kMeasureSec   = 4.0;
constexpr double kFloorLiftDb  = 10.0;   // the band's floor this far over the radio's own = the radio is no longer the limit
constexpr int    kClipRunSec   = 2;      // ★ the radios' own hard-overload test: two consecutive seconds on the rail
inline Radio rtlRadio() { return {"RTL-SDR", 0.0, -8.0, kMeasureSec}; }   // ★ 8 dB under the rail: VibeAGC aims −6 adaptively
inline Radio rspRadio() { return {"SDRplay", 0.0, -10.0, kMeasureSec}; }  // ★ its loop aims −20; a fixed pass can sit higher

/* ── THE SETTER ──────────────────────────────────────────────────────────────────────────────────────────── */
struct Choice {
    int    idx = 0;                 // into the ascending gain table
    double predictedPk = -99.0;     // the peak this gain should give, dBFS
    double floorOverOwn = NAN;      // the band's floor over the radio's own at this gain (NaN = own floor unknown)
    const char* limit = "headroom"; // what decided it: headroom · floor · ceiling · bottom
};

/** The best fixed gain for the pass, from what was measured at `curIdx`.
 *  @param gains    the radio's gain table, ascending, tenths of a dB (the RTL's tuner list; any monotone list)
 *  @param ceilIdx  the owner's ceiling (the highest index allowed)
 *  @param peakMax  the loudest peak of the window, dBFS
 *  @param floorDb  the band's noise floor at curIdx (any dB scale — only differences are used)
 *  @param ownFloorDb the radio's own floor on that same scale at curIdx, or NaN when not known
 *  ★ Headroom first and always: the floor rule may only make the choice LOWER (it never buys gain past the margin). */
inline Choice chooseGain(const std::vector<int>& gains, int curIdx, int ceilIdx, double peakMax, double floorDb,
                         double ownFloorDb, const Radio& r) {
    Choice c;
    const int n = (int)gains.size();
    if (n == 0) return c;
    ceilIdx = std::clamp(ceilIdx, 0, n - 1);
    curIdx = std::clamp(curIdx, 0, n - 1);
    auto add = [&](int i) { return (gains[(size_t)i] - gains[(size_t)curIdx]) / 10.0; };
    int best = -1;
    for (int i = 0; i <= ceilIdx; ++i)
        if (peakMax + add(i) <= r.targetPk) best = i;
    if (best < 0) { best = 0; c.limit = "bottom"; }                  // even the bottom of the table is over: as low as it goes
    else if (best == ceilIdx && ceilIdx < n - 1) c.limit = "ceiling";
    else if (best == n - 1) c.limit = "ceiling";
    if (std::isfinite(ownFloorDb) && std::isfinite(floorDb)) {
        // ★ the lowest gain that already lifts the band's floor kFloorLiftDb over the radio's own
        for (int i = 0; i < best; ++i)
            if (floorDb + add(i) - ownFloorDb >= kFloorLiftDb) { best = i; c.limit = "floor"; break; }
        c.floorOverOwn = floorDb + add(best) - ownFloorDb;
    }
    c.idx = best;
    c.predictedPk = peakMax + add(best);
    return c;
}

/** ★ A genuine clip, shed as the ordinary loop sheds one: at least one rung, the peak's excess over the operating
 *  point, and at least vibeagc::kGrossShedDb when the rail fraction is gross (the peak has stopped being a
 *  measurement). @return the new index (≤ curIdx - 1 unless already at the bottom). */
inline int clipShedIdx(const std::vector<int>& gains, int curIdx, double peak, double clipPct, const Radio& r) {
    if (gains.empty()) return 0;
    curIdx = std::clamp(curIdx, 0, (int)gains.size() - 1);
    if (curIdx == 0) return 0;
    const double shed = vibeagc::grossOverloadShedDb(std::max(0.0, peak - r.targetPk), clipPct);
    int to = curIdx - 1;
    for (int i = curIdx - 1; i >= 0; --i) {
        to = i;
        if ((gains[(size_t)curIdx] - gains[(size_t)i]) / 10.0 >= shed) break;
    }
    return to;
}

/* ── THE PASS ────────────────────────────────────────────────────────────────────────────────────────────── */
enum class Phase { Off, Measure, Hold };
inline const char* phaseName(Phase p) { return p == Phase::Measure ? "measuring" : p == Phase::Hold ? "holding" : "off"; }

struct In {
    double now = 0;                       // seconds, monotonic
    double peak = -99.0;                  // the converter's peak since the last tick, dBFS (fresh: after any write settled)
    double clipPct = 0.0;                 // % of samples on the rail
    int    clipRun = 0;                   // consecutive seconds on the rail (the radio's own hard-overload counter)
    double floorDb = NAN;                 // the band's floor now
    double ownFloorDb = NAN;              // the radio's own floor on the same scale, NaN = unknown
    const std::vector<int>* gains = nullptr;
    int    curIdx = 0, ceilIdx = 0;
};
struct Out {
    enum Kind { None, Set, ClipCut } kind = None;
    int idx = -1;
    std::string log;
};

class Hold {
public:
    explicit Hold(Radio r = rtlRadio()) : r_(r) {}
    const Radio& radio() const { return r_; }
    Phase phase() const { return ph_; }
    bool  active() const { return ph_ != Phase::Off; }
    unsigned sets() const { return sets_; }
    unsigned cuts() const { return cuts_; }
    /** A satellite decoder started (or the hold is re-armed at AOS): measure from here. */
    void begin(double now, const std::string& what) { what_ = what; ph_ = Phase::Measure; startWindow(now); }
    /** AOS on a pick that was armed before the rise: the band is the same, the satellite is now in it — measure again. */
    void remeasure(double now) { if (ph_ != Phase::Off) { ph_ = Phase::Measure; startWindow(now); } }
    /** The decoder stopped / the pick went / LOS: the ordinary automatic gain resumes. */
    void end() { ph_ = Phase::Off; }
    const std::string& what() const { return what_; }

    Out tick(const In& in) {
        Out o;
        if (ph_ == Phase::Off || !in.gains || in.gains->empty()) return o;
        const std::vector<int>& g = *in.gains;
        const int cur = std::clamp(in.curIdx, 0, (int)g.size() - 1);
        const double gainDb = g[(size_t)cur] / 10.0;
        char b[400];
        // ── genuine clipping: the one move either phase allows ──
        if (in.clipRun >= kClipRunSec) {
            const int to = clipShedIdx(g, cur, in.peak, in.clipPct, r_);
            if (to < cur) {
                std::snprintf(b, sizeof b, "satellite pass (%s): the converter is on the rail (%.3f %% of samples, %d s) — "
                              "down %.1f dB to %.1f dB; %s", what_.c_str(), in.clipPct, in.clipRun,
                              (g[(size_t)cur] - g[(size_t)to]) / 10.0, g[(size_t)to] / 10.0,
                              ph_ == Phase::Measure ? "measuring again from there" : "still held (clips only)");
                o.kind = Out::ClipCut; o.idx = to; o.log = b; ++cuts_;
                if (ph_ == Phase::Measure) startWindow(in.now);
                return o;
            }
        }
        if (ph_ == Phase::Hold) return o;     // ★★★ THE HOLD: nothing else moves it — no climb, no spur chasing
        // ── measuring ──
        peakMax_ = std::max(peakMax_, in.peak);
        if (std::isfinite(in.floorDb)) { floorSum_ += in.floorDb; ++floorN_; }
        ++n_;
        if (in.now - t0_ < r_.measureSec || n_ < 2) return o;
        const double floorAvg = floorN_ ? floorSum_ / floorN_ : NAN;
        const Choice c = chooseGain(g, cur, in.ceilIdx, peakMax_, floorAvg, in.ownFloorDb, r_);
        ph_ = Phase::Hold;
        char fl[96];
        if (std::isfinite(c.floorOverOwn)) std::snprintf(fl, sizeof fl, ", floor %.1f dB above the radio's own", c.floorOverOwn);
        else std::snprintf(fl, sizeof fl, ", floor %.1f dB (the radio's own floor is not known here — headroom decides)",
                           std::isfinite(floorAvg) ? floorAvg : -99.0);
        std::snprintf(b, sizeof b, "satellite pass (%s): gain %s %.1f dB — peak %.1f dBFS over %.0f s at %.1f dB, %.1f dB "
                      "predicted (set point %.0f dBFS, %.0f dB under the rail; decided by %s)%s; held for the pass "
                      "(clips only)", what_.c_str(), c.idx == cur ? "stays at" : "set to", g[(size_t)c.idx] / 10.0,
                      peakMax_, in.now - t0_, gainDb, c.predictedPk, r_.targetPk, r_.railDbfs - r_.targetPk, c.limit, fl);
        o.log = b; ++sets_;
        if (c.idx != cur) { o.kind = Out::Set; o.idx = c.idx; }
        return o;
    }

private:
    Radio r_;
    Phase ph_ = Phase::Off;
    std::string what_;
    double t0_ = 0, peakMax_ = -99.0, floorSum_ = 0; int floorN_ = 0, n_ = 0;
    unsigned sets_ = 0, cuts_ = 0;
    void startWindow(double now) { t0_ = now; peakMax_ = -99.0; floorSum_ = 0; floorN_ = 0; n_ = 0; }
};

/* ── WHEN A PASS IS ON ───────────────────────────────────────────────────────────────────────────────────── */
/** ★★ The shim's housekeeping asks this every tick: is a satellite decoder running (`running`, with `key` naming
 *  which — a different pick is a different pass), and is the satellite above the horizon (`up`; `knowsSky` false when
 *  there is no orbit to ask, e.g. LRPT started by hand with no pick). It answers with the one event the gain loop
 *  needs:
 *    Begin — a pass starts: measure and set (a decoder started, a new pick, or AOS after an earlier LOS)
 *    Aos   — the satellite rose on a pick that was armed before the rise: measure again (the hold stays)
 *    End   — the decoder stopped, the pick went, or LOS: the ordinary automatic gain resumes
 *  ★ Armed before the rise = held from the start: the band is measured and set while waiting, and again at AOS. After
 *    LOS the radio is the ordinary radio until the next AOS on the same pick. */
class Watch {
public:
    enum Ev { None, Begin, Aos, End };
    Ev step(bool running, const std::string& key, bool knowsSky, bool up) {
        if (!running) { const bool was = on_; reset(); return was ? End : None; }
        if (key != key_) { reset(); key_ = key; }
        Ev ev = None;
        if (knowsSky) {
            const bool aos = haveSky_ && up && !wasUp_;
            const bool los = haveSky_ && !up && wasUp_;
            haveSky_ = true; wasUp_ = up;
            if (los) { losSeen_ = true; if (on_) { on_ = false; return End; } }
            if (aos) { losSeen_ = false; if (on_) return Aos; on_ = true; return Begin; }
        }
        if (!on_ && !losSeen_) { on_ = true; ev = Begin; }
        return ev;
    }
    bool on() const { return on_; }
private:
    bool on_ = false, losSeen_ = false, wasUp_ = false, haveSky_ = false;
    std::string key_;
    void reset() { on_ = losSeen_ = wasUp_ = haveSky_ = false; key_.clear(); }
};

/* ── THE RSP: TWO STAGES ─────────────────────────────────────────────────────────────────────────────────── */
/** ★ An RSP has no gain table: an LNA state (0 = most RF gain) and an IF reduction gRdB (20-59, less = more gain).
 *  The pass's IF is expressed as a gain table for Hold (index i = gain i dB = reduction 59 − i, from the owner's IF
 *  floor up), and the RF stage is placed by VibeAGC's OWN rule (vibe_rsp_vibeagc.h rule 3): one more rung of RF only
 *  when the IF has room to take the whole rung back with kAddMarginDb to spare — and one rung out when the IF cannot
 *  shed enough on its own. One rung per decision, exactly as that loop moves. */
struct RspPlace { int lna, gr; bool lnaChanged; };
constexpr int kRspGrMin = 20, kRspGrMax = 59;
constexpr double kRspGrHigh = 52.0, kRspAddMarginDb = 6.0;
inline std::vector<int> rspIfTable(int grFloor) {
    std::vector<int> t;
    for (int gr = kRspGrMax; gr >= std::max(kRspGrMin, grFloor); --gr) t.push_back((kRspGrMax - gr) * 10);
    return t;
}
inline int rspIdxOfGr(int gr) { return kRspGrMax - std::clamp(gr, kRspGrMin, kRspGrMax); }
inline int rspGrOfIdx(int idx) { return kRspGrMax - idx; }
/** Where the RF stage goes once the IF has been chosen (grWant may be outside 20..59: the IF alone could not reach).
 *  rungUp = dB one more rung of RF adds; rungDown = dB one rung out removes. */
inline RspPlace rspPlace(int lna, int grWant, int grFloor, int lnaFloor, int nStates, bool rfOwned, double rungUp, double rungDown) {
    RspPlace p{lna, grWant, false};
    grFloor = std::clamp(grFloor, kRspGrMin, kRspGrMax);
    if (rfOwned && grWant > kRspGrMax && lna + 1 < nStates) {             // the IF cannot shed it all: one rung out
        p.lna = lna + 1; p.gr = (int)std::lround(grWant - rungDown); p.lnaChanged = true;
    } else if (rfOwned && lna - 1 >= lnaFloor && grWant + rungUp <= kRspGrHigh - kRspAddMarginDb) {   // room for a rung in
        p.lna = lna - 1; p.gr = (int)std::lround(grWant + rungUp); p.lnaChanged = true;
    }
    p.gr = std::clamp(p.gr, grFloor, kRspGrMax);
    return p;
}

}  // namespace vibesatgain
