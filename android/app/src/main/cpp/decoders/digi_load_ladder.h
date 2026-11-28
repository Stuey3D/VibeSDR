// decoders/digi_load_ladder.h — BAND'S LOAD LADDER: DEPTH DROPS BEFORE WHOLE MODES OR BANDS (V12 beta94, 2026-11-25).
//
// ★★★ THE OWNER'S RULE (Stuart, 25 Nov): "what should happen in the background is if the cpu is fully loaded the depth
//     drops before dropping entire bands".
//
// ── The ladder, in the order it is climbed when Band decoding falls behind ─────────────────────────────────────────
//   depth 0  FULL           today's best: passes as the slot's clock allows, OSD, a-priori, and "Dig deeper" if on
//   depth 1  NO DIG DEEPER  the deep tier off (repeats, 8 AP hypotheses, FT8's looser second look) — the book still learns
//   depth 2  NO OSD / AP    belief propagation only (the second look stays: it is cheaper than the old single pass)
//   depth 3  ONE PASS       every slotted engine LIGHT (the governor's "light" chip; WSPR/JS8/Q65/FST4 their own light)
//   depth 4  FEWER CANDIDATES  FT8/FT4/FT2 search 70 sync candidates a slot, not 140
//   depth 5  STRICTER SYNC  … and only candidates with a sync score of 14 or more (not 10) — the MINIMUM
//   then, and only then, the existing pause order: CW, then RTTY, then wide PSK (Spotter::pauseTick); the slotted
//   modes keep their slots as long as there is anything else to give; last, background decoding's easing may pause a whole
//   Band VFO (vibe_bg_ease.h).
//   The existing knobs throughout: Ft8Decoder's passes / OSD / AP / deep tier / maxCandidates / minScore (applied per
//   slot, decoders/ft8_decoder.cpp runDecode), the engines' `loaded` (light) and the governor's light costs.
//
// ── "Behind" (the pause rule's own signals, unchanged): a slotted engine skipped a slot, the decode's backlog over
//    Spotter::kPauseBehindSec (in FULL-CPU mode: lasting 10 s; a skip only without headroom — vibe_bg_fullcpu.h), or the
//    governor shed a period (its budget exceeded). One step every kStepSec — an FT8 slot, so a step can show before the
//    next is taken.
// ── Recovery, in reverse: modes come back first (the pause rule's 30 s of calm each — RTTY, then CW), and only when
//    nothing is paused anywhere on the ladder does depth come back, one step per kCalmSec of calm.
// ── ONE LADDER PER HOST — every Band VFO of a background profile shares the runner's, so depth drops on EVERY band before
//    any band loses a mode. Thread-safe (its own lock); several spotters report into it.
// ── A LISTENER arriving (vibe_bg_ease.h): easing's first step is `forceMin` — depth to the minimum on every band —
//    before any band's extra modes go off.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace vibe {
namespace digi {

class LoadLadder {
public:
    enum Depth : int { kFull = 0, kNoDeep = 1, kNoOsdAp = 2, kOnePass = 3, kFewerCands = 4, kStrictSync = 5 };
    static constexpr int kMin = kStrictSync;
    static constexpr double kStepSec = 15.0;    ///< one FT8 slot between steps down — each step's effect can show
    static constexpr double kCalmSec = 30.0;    ///< calm this long (and nothing paused) before one step back up
    static constexpr int kFewerCandsN = 70;     ///< FT8/FT4/FT2 sync candidates at depth ≥ 4 (140 at full)
    static constexpr int kStrictMinScore = 14;  ///< …and their sync threshold at depth 5 (10 at full)

    static const char* name(int d) {
        switch (d) {
            case kFull: return "full";
            case kNoDeep: return "no Dig deeper";
            case kNoOsdAp: return "no OSD or a-priori passes";
            case kOnePass: return "one pass a slot";
            case kFewerCands: return "fewer candidates";
            case kStrictSync: return "stricter sync (the minimum)";
        }
        return "?";
    }
    /** The depth the engines decode at now (any thread). */
    int depth() const { std::lock_guard<std::mutex> lk(m_); return force_ ? kMin : std::max(depth_, floor_); }
    /** ★★ b97-cpu-budget: the BOX's budget (vibe_bg_budget.h) holds depth at least this low (0 = no hold). The ladder's own
     *  steps and calm carry on beneath it; the budget gives it back one step at a time. */
    void setFloor(int f) { std::lock_guard<std::mutex> lk(m_); floor_ = std::max(0, std::min(kMin, f)); }
    int floor() const { std::lock_guard<std::mutex> lk(m_); return floor_; }
    int learnt() const { std::lock_guard<std::mutex> lk(m_); return depth_; }
    bool atMin() const { return depth() >= kMin; }
    void setLog(std::function<void(const std::string&)> f) { std::lock_guard<std::mutex> lk(m_); log_ = std::move(f); }
    /** ★ Background easing for a listener: the minimum on every band at once (true), or the ladder's own figure back. */
    void forceMin(bool on) { std::lock_guard<std::mutex> lk(m_); force_ = on; }
    bool forced() const { std::lock_guard<std::mutex> lk(m_); return force_; }

    /** ★★ A reporter is BEHIND (`why` in words). @return true = depth is already at its minimum: the caller may shed (its
     *  pause rule goes on); false = handled here (a depth step taken now, or one taken too recently to judge). */
    bool behind(double utc, const std::string& why) {
        std::function<void(const std::string&)> lg;
        std::string line;
        {
            std::lock_guard<std::mutex> lk(m_);
            lastBehind_ = utc;
            if (force_ || depth_ >= kMin || floor_ >= kMin) { lastAtMin_ = utc; return true; }
            if (utc - stepAt_ < kStepSec) return false;
            ++depth_; stepAt_ = utc;
            line = std::string("Band: CPU behind (") + why + ") \xe2\x80\x94 decode depth reduced to " + name(depth_)
                 + " (all bands keep decoding)";
            if (depth_ >= kMin) line += "; next, CW then RTTY pause";
            lg = log_;
        }
        if (lg) lg(line);
        return false;
    }
    /** ★ A reporter's calm second: `paused` = how many modes it has paused now. Depth comes back only when every reporter
     *  has nothing paused and it has been calm (no `behind` from anyone) for kCalmSec. */
    void calm(double utc, const void* who, int paused) {
        std::function<void(const std::string&)> lg;
        std::string line;
        {
            std::lock_guard<std::mutex> lk(m_);
            paused_[who] = paused;
            int total = 0;
            for (auto& kv : paused_) total += kv.second;
            if (total > 0 || depth_ <= kFull) return;
            if (!(utc - lastBehind_ >= kCalmSec) || !(utc - stepAt_ >= kCalmSec)) return;
            --depth_; stepAt_ = utc;
            line = std::string("Band: decode has caught up \xe2\x80\x94 depth back up to ") + name(depth_);
            lg = log_;
        }
        if (lg) lg(line);
    }
    /** ★ Somebody on this ladder was behind AT minimum depth within the last 2 s — a spotter with continuous modes pauses
     *  the next of them for it (a slotted spotter has none of its own to give). */
    bool behindAtMin(double utc) const { std::lock_guard<std::mutex> lk(m_); return utc - lastAtMin_ < 2.0 && utc >= lastAtMin_; }
    /** A reporter leaves (its spotter is going). */
    void forget(const void* who) { std::lock_guard<std::mutex> lk(m_); paused_.erase(who); }

private:
    mutable std::mutex m_;
    int depth_ = kFull;
    int floor_ = 0;
    bool force_ = false;
    double stepAt_ = -1e300, lastBehind_ = -1e300, lastAtMin_ = -1e300;
    std::map<const void*, int> paused_;
    std::function<void(const std::string&)> log_;
};

} // namespace digi
} // namespace vibe
