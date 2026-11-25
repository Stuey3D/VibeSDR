// vibe_bg_ease.h — BACKGROUND DECODING EASES OFF FOR LISTENERS, ON PURPOSE, BEFORE IT HURTS (V12, 2026-11-24).
//
// ★★★ WHY (Stuart, 24 Nov, listening on the Pi 500's RSP1B with nine background VFOs carrying on in range): the health
//     card said "Background decoding: TOO HEAVY" while the receiver felt fine — "we need to throttle back the background
//     decoding when users are on before it gets to this stage". Beta92 already made background decoding GIVE WAY (its
//     threads at nice 19, the DSP thread never waiting for its queue) and that costs it a little IQ now and then (~0.4 s
//     a minute, bridged). That is the design working, not a fault — "the sdr feels responsive enough, just the warning".
//     So this file does two things and nothing more:
//       1. it judges whether background decoding is costing anybody anything, from what the machine MEASURES: the
//          listener's own IQ queue (late = the DSP thread behind; dropped = harm), and background decoding's own drops
//          per minute beyond the small, bridged give-way;
//       2. only when one of those says so, it sheds background work ONE STEP AT A TIME, in a deliberate order, waits to
//          see the effect, and puts it back in reverse when the box has been calm for a while (never flapping).
//     Work that costs nobody anything is never paused.
//
// ★★ THE ORDER (cheapest loss first; distress last):
//     0  a Band skimmer's EXTRA chips (CW / RTTY / wide PSK — the band's IQ chips), the band FURTHEST from the listener
//        first. FT8/FT4/WSPR on that band carry on: the core of the PSK Reporter feed (the all-band skimmer goal).
//     1  picture decoders — WEFAX stations (several charts in one slot), WEFAX, SSTV. An idle one before one mid-chart.
//     2  other text decoders — RTTY weather, amateur RTTY, SITOR-A, APRS — the owner's lowest row first.
//     3  whole skimmer VFOs — Band skimmers (furthest band first), FT8, CW / PSK / RTTY skimmers, NDB.
//     4  SAFETY — DSC and NAVTEX. LAST, and ONLY for the listener's own harm: never to stop background decoding's own
//        drops (pausing a distress watch so that the distress watch drops less input would be self-defeating).
//
// ★ Pure: no threads, no clocks, no files — every input is a number handed in (test-bg-ease.cpp drives it directly).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

namespace vibe {
namespace bg {
namespace ease {

enum class Kind { Chips, Pause, Depth };
/* ★★★ beta94 (Stuart: "the depth drops before dropping entire bands"): tier −1 comes before everything — every Band VFO's
 *  decode depth to the minimum (digi::LoadLadder::forceMin), all bands and all their modes still decoding. */
enum Tier : int { kTierDepth = -1, kTierChips = 0, kTierPictures = 1, kTierText = 2, kTierSkimmers = 3, kTierSafety = 4 };

/** ★ Which tier a VFO's decoder pauses in (its id from vibe_bgdecode_config.h). Unknown ids are text (tier 2). */
inline int tierOf(const std::string& dec) {
    if (dec == "dsc" || dec == "dsc-vhf" || dec == "navtex") return kTierSafety;
    if (dec == "wefax" || dec == "wefax-stations" || dec == "sstv") return kTierPictures;
    if (dec == "spots-band" || dec == "ft8" || dec == "ndb-skim" || dec.rfind("cw-skim", 0) == 0 || dec.rfind("psk-skim", 0) == 0
        || dec.rfind("rtty-skim", 0) == 0) return kTierSkimmers;
    return kTierText;
}

/** One step background decoding could take now. */
struct Candidate {
    int vfo = 0;             ///< the VFO's index in its profile
    Kind kind = Kind::Pause;
    int tier = kTierText;
    double distHz = 0;       ///< from the listener's dial to where this VFO listens
    int rank = 1;            ///< its row (1 = the owner's top priority)
    bool busy = false;       ///< part-way through a picture: passed over while an idle one in its tier is left
};

/** ★★ THE NEXT STEP: the lowest tier first. Inside a tier: an idle one before one mid-picture; for chips and skimmers the
 *  one FURTHEST from the listener (then the lowest row); for the rest the owner's lowest row first (his order IS his
 *  priority). Safety only when `allowSafety`. @return an index into `c`, or -1 = nothing left to take. */
inline int pickNext(const std::vector<Candidate>& c, bool allowSafety) {
    int best = -1;
    auto better = [&](const Candidate& a, const Candidate& b) {   // a before b?
        if (a.tier != b.tier) return a.tier < b.tier;
        if (a.busy != b.busy) return !a.busy;
        if ((a.tier == kTierChips || a.tier == kTierSkimmers) && std::fabs(a.distHz - b.distHz) > 1.0) return a.distHz > b.distHz;
        if (a.rank != b.rank) return a.rank > b.rank;
        return a.vfo > b.vfo;
    };
    for (size_t i = 0; i < c.size(); i++) {
        if (c[i].tier == kTierSafety && !allowSafety) continue;
        if (best < 0 || better(c[i], c[(size_t)best])) best = (int)i;
    }
    return best;
}

/** What the machine measured since the last tick (about every 2 s). */
struct Signals {
    int64_t  nowMs = 0;
    bool     listenerOn = false;
    double   bgDroppedSec = 0;       ///< monotonic: seconds of capture background decoding dropped (its queue + busy)
    uint64_t listenerIqDrops = 0;    ///< monotonic: the LISTENER's IQ buffers thrown away (the DSP thread behind)
    double   listenerLag = -1;       ///< the listener's IQ queue at its fullest since the last tick, 0..1 of its cap (-1 = unknown)
    double   machineBusyPct = -1;    ///< the whole machine, 0..100 (-1 = unknown: Android shows only this process)
    int      cores = 0;
};

enum class Act { Hold, Shed, ShedSafety, Restore, RestoreAll };

/** ★★★ THE THRESHOLDS, with their reasons (BG-EASE.md has the long form).
 *   kShedBgSecPerMin 1.5 — the Pi 500 gave way 0.3–0.5 s a minute with a listener on and nobody noticed (bridged); five
 *                          times the gap a WSPR/FT8 slot can bridge is still under 3 % of the capture. Above it, slots go.
 *   kCalmBgSecPerMin 0.5 — a step comes back only at the level that was proven harmless. The 3× gap is the hysteresis.
 *   kLagFrac 0.6         — the listener's IQ queue 60 % full (150 ms of a 250 ms queue) twice in 10 s: the DSP thread is
 *                          losing the race; one more stall is a dropped buffer (an audible gap). Acted on in 10 s.
 *   kObserveMs 30 s      — at least this long after a step before judging background decoding's own drop rate again.
 *   kCalmMs 3 min        — calm this long (no listener harm, background drops at the proven level) before one step back;
 *                          doubled (to 30 min at most) each time a restored step had to be shed again within 10 min. */
struct Limits {
    double  shedBgSecPerMin = 1.5;
    double  calmBgSecPerMin = 0.5;
    double  lagFrac = 0.6;
    int     lagTicks = 2;
    int64_t lagWindowMs = 10000;
    int64_t harmSettleMs = 10000;
    int64_t observeMs = 30000;
    int64_t calmMs = 180000;
    int64_t maxCalmMs = 1800000;
    int64_t flapMs = 600000;
    double  restoreBusyMax = 85.0;   ///< the machine busier than this: nothing comes back yet
};

class Governor {
public:
    explicit Governor(Limits l = Limits()) : L_(l), wait_(l.calmMs) {}

    /** ★ One tick. `taken` = steps in effect now; `canShed` = a non-safety step is left; `canShedSafety` = a safety one is;
     *  `nextCostPct` = what the step that would come back next cost when it was taken (% of one core; -1 unknown). */
    Act tick(const Signals& s, int taken, bool canShed, bool canShedSafety, double nextCostPct = -1) {
        why_.clear();
        exhausted_ = false; rate_ = -1;
        if (!s.listenerOn) {
            const bool was = on_;
            on_ = false; h_.clear(); harm_ = false;
            if (taken > 0) { why_ = was ? "the listener left" : "nobody is listening"; last_ = s.nowMs; return Act::RestoreAll; }
            return Act::Hold;
        }
        if (!on_) { on_ = true; last_ = s.nowMs; h_.clear(); harmAt_ = INT64_MIN / 2; }   // ★ an arrival changes the load: judge afresh
        if (!h_.empty() && (s.bgDroppedSec + 1e-9 < h_.back().bg || s.listenerIqDrops < h_.back().drops)) h_.clear();   // restarted
        h_.push_back({ s.nowMs, s.bgDroppedSec, s.listenerIqDrops, s.listenerLag });
        while (!h_.empty() && s.nowMs - h_.front().t > 120000) h_.pop_front();
        while (h_.size() > 256) h_.pop_front();

        // ── the listener's own harm: a dropped buffer, or the queue well past half full twice in 10 s ──
        harm_ = false;
        int lagged = 0;
        for (auto& e : h_) {
            if (s.nowMs - e.t > L_.lagWindowMs) continue;
            if (e.lag >= L_.lagFrac) ++lagged;
        }
        for (auto& e : h_) if (s.nowMs - e.t <= L_.lagWindowMs) { if (s.listenerIqDrops > e.drops) harm_ = true; break; }
        if (lagged >= L_.lagTicks) harm_ = true;
        if (harm_) harmAt_ = s.nowMs;

        // ── background decoding's own drop rate, judged only on what happened since the last step ──
        const int64_t from = std::max(last_, s.nowMs - 60000);
        const E* base = nullptr;
        for (auto& e : h_) if (e.t >= from) { base = &e; break; }
        if (base && s.nowMs - base->t >= L_.observeMs)
            rate_ = (s.bgDroppedSec - base->bg) * 60000.0 / (double)(s.nowMs - base->t);
        const bool over = rate_ >= L_.shedBgSecPerMin;
        const bool calm = rate_ >= 0 && rate_ <= L_.calmBgSecPerMin;

        if (harm_) {
            if (s.nowMs - last_ < L_.harmSettleMs) return Act::Hold;   // ★ the last step has not had time to show yet
            char b[160];
            std::snprintf(b, sizeof b, "the listener's own receiver was falling behind (%s)",
                          lagged >= L_.lagTicks ? "its IQ queue past 60 % full" : "IQ dropped");
            why_ = b;
            if (canShed) { noteShed(s.nowMs); return Act::Shed; }
            if (canShedSafety) { noteShed(s.nowMs); return Act::ShedSafety; }
            exhausted_ = true;
            return Act::Hold;
        }
        if (over) {
            char b[160];
            std::snprintf(b, sizeof b, "background decoding dropped %.1f s of IQ a minute (more than the %.1f s it may give way)",
                          rate_, L_.shedBgSecPerMin);
            why_ = b;
            if (canShed) { noteShed(s.nowMs); return Act::Shed; }
            exhausted_ = true;   // ★ only safety left — never paused for background decoding's own drops
            return Act::Hold;
        }
        if (taken > 0 && calm && s.nowMs - harmAt_ >= wait_ && s.nowMs - last_ >= wait_ && fits(s, nextCostPct)) {
            char b[160];
            std::snprintf(b, sizeof b, "calm for %d min (%.1f s of IQ a minute given way, the listener keeping up)",
                          (int)(wait_ / 60000), rate_);
            why_ = b;
            last_ = s.nowMs; restoredAt_ = s.nowMs;
            return Act::Restore;
        }
        return Act::Hold;
    }

    /** ★ Listener on, background decoding dropping more than it may, and nothing left to ease but safety (or nothing at
     *  all) — the one case that is still "TOO HEAVY" with a listener on. */
    bool exhausted() const { return exhausted_; }
    bool listenerHarm() const { return harm_; }
    double rate() const { return rate_; }
    int64_t calmWaitMs() const { return wait_; }
    const std::string& why() const { return why_; }
    void reset() { *this = Governor(L_); }

private:
    struct E { int64_t t; double bg; uint64_t drops; double lag; };
    bool fits(const Signals& s, double cost) const {
        if (s.machineBusyPct < 0) return true;                       // ★ unknown (Android): calm is the only judge
        if (s.machineBusyPct > L_.restoreBusyMax) return false;
        if (cost < 0 || s.cores <= 0) return true;
        const double idleCorePct = (100.0 - s.machineBusyPct) * (double)s.cores;   // % of one core, free
        return cost * 2.0 <= idleCorePct;                            // ★ twice its cost free, or it waits
    }
    void noteShed(int64_t now) {
        if (now - shedAt_ > L_.maxCalmMs) wait_ = L_.calmMs;         // ★ quiet for half an hour: forgive old flaps
        if (restoredAt_ > INT64_MIN / 2 && now - restoredAt_ < L_.flapMs) wait_ = std::min(L_.maxCalmMs, wait_ * 2);
        restoredAt_ = INT64_MIN / 2;
        shedAt_ = now; last_ = now;
    }
    Limits L_;
    std::deque<E> h_;
    bool on_ = false, harm_ = false, exhausted_ = false;
    int64_t last_ = 0, harmAt_ = INT64_MIN / 2, shedAt_ = INT64_MIN / 2, restoredAt_ = INT64_MIN / 2;
    int64_t wait_;
    double rate_ = -1;
    std::string why_;
};

/** ★★★ beta94 — WHEN BACKGROUND DECODING BELONGS ON THE HEALTH PILL (the Lenovo, Stuart 11-25). The pill said "Background
 *  decoding: TOO HEAVY" on an 8-core i5 at load 0.05 while he listened to an ORBCOMM pass at 137.66 MHz — "not even
 *  background decoding right now either, as I'm tuned away from the background decoders". Two faults:
 *    1. ANY drop of background decoding's own IQ named it, for a minute. With nobody listening the drops are never
 *       excused, and they are steady: the DSP thread never waits for background decoding's queue (beta92), which costs
 *       ~0.5 s of IQ a minute on an idle box (the Lenovo's log, every minute: 0.5 s, beside "decode 0.00 s behind the
 *       capture" and "IQ blocks dropped 0"; the Pi 500 0.3–0.5 s). That loses background decoding a little — it costs a
 *       listener nothing.
 *    2. It STOPPED (the listener's dial left its bands: "no background VFO is inside the radio's window now") and the name
 *       stayed up for the next minute (vibehealth::BehindWatch holds a name a minute after its last drop, whether or not
 *       the decoder is still there) — the first thing the listener saw was TOO HEAVY for work that had just ended.
 *  ★★ Stuart's rule: "the pill is to inform users why they might be having a sluggish connection, lagging, broken audio
 *     etc." — "background decoding doesn't need to be shown to users, only when the CPU is struggling. If it's idle
 *     because the SDR is in use, the admin menu is enough." So background decoding is on the pill ONLY when it is running
 *     with something decoding, a listener is on and the LISTENER's own receiver is falling behind (Governor::listenerHarm —
 *     their IQ dropped, or their queue past 60 % twice in 10 s: the DSP thread short of CPU). The easing sheds a step
 *     every 10 s meanwhile; the line goes when the harm does. Everything else — its own drops, paused, idle, tuned away, a satellite pass — is the admin
 *     page's. Shown at once; held 30 s after the last degraded tick so one calm tick does not blink it; gone AT ONCE when
 *     it stops, nothing decodes or the listener leaves. Pure: the shim hands in the runner's state every health tick. */
class PillWatch {
public:
    /** @return true = put "the server's CPU is overloaded" on the pill now */
    bool tick(int64_t nowMs, bool running, int decoding, bool listenerOn, bool degrading) {
        if (!running || decoding <= 0 || !listenerOn) { lastMs_ = INT64_MIN / 2; return false; }
        if (degrading) lastMs_ = nowMs;
        return nowMs - lastMs_ < kHoldMs;
    }
    static constexpr int64_t kHoldMs = 30000;
private:
    int64_t lastMs_ = INT64_MIN / 2;
};

}  // namespace ease
}  // namespace bg
}  // namespace vibe
