// decoders/digi_governor.h — BAND MODE'S CPU GOVERNOR (V12, 2026-10-27): which of a band's modes decode this period, and
// how deep. Pure decisions and a small budget tracker — the spotter (digi_spotter.cpp) asks, the tests drive it directly.
//
// ★★★ WHY (Stuart, 2026-10-27): "we want to make the band digital spots mode pull everything it can down, but we also have
//     to be mindful of CPU running that many decoders at once." A busy 20 m now has seven modes on it (FT8, FT4, WSPR,
//     JS8, FT2, JTTY, PSK) and four more engines are on their way (Q65, FST4, FST4W, MSK144). Everything at full depth
//     is ≈ 22 FT8 engines' worth — nothing on a Mac, most of a Pi 2's spare core.
//
// ── The three levers, in the order they are pulled ─────────────────────────────────────────────────────────────────
//  1. ACTIVITY (decoders/digi_activity.h): a mode whose window holds nothing this period is IDLE — its audio is not
//     decoded at all. Free, and it is where most of the saving is: on a quiet band almost everything is idle.
//  2. DEPTH: a mode that decodes runs LIGHT (its engine told it is `loaded`: one pass — no subtraction passes, no OSD,
//     no AP, no coherent stage) before anything is dropped. ModeDef::lightCost() is that price.
//  3. SHEDDING: when even the light set is over budget, modes are dropped LOWEST PRIORITY FIRST (ModeDef::priority) —
//     but greedily: a cheap mode below a shed expensive one still runs if it fits (PSK at 0.6 after JTTY at 12).
//  The OWNER's word beats all three (Setup → Modes and decoders → Digital spots: Band): ALWAYS = no gate and never shed;
//  OFF = never run. The benchmark only sets the budget — it never hides a mode (Stuart's rule).
//
// ── The priority (lower first) and why ─────────────────────────────────────────────────────────────────────────────
//   FT8 10 · FT4 20 · WSPR 30 · JS8 40 · FT2 50 · JTTY 60 · PSK 70 · anything else 100 (Q65, FST4, FST4W, MSK144).
//   FT8 is most of all digital traffic on every band (PSKReporter's hourly active-receiver modes, 2026-10-26: FT8 6666,
//   FT4 597, JS8 198, JTTY 180); FT4 next. WSPR third although it costs 5 FT8s: every WSPR spot is a calibrated path
//   (the propagation reference people run Band for), it decodes once in two minutes, and on a quiet window it is idle,
//   so it costs only where there is something to hear. JS8 and FT2 are cheap and steady. JTTY is the costliest engine
//   (≈ 12 FT8s, continuous) and is heard on a few dials — it goes before them. PSK last: spots only (callsigns heard
//   identifying, a few an hour), cheap, so it still fits after JTTY is shed. New engines take 100 until measured.
//
// ── The budget ─────────────────────────────────────────────────────────────────────────────────────────────────────
//   In FT8 units (one FT8 engine at full depth). From the benchmark's Band row (vibe_benchmark_decoders.h dec_ft8band —
//   the busiest band, every mode, one pass, measured on this machine): units = kBudgetPct ÷ (that row's % per light
//   unit). Then LIVE: a period in which any engine skipped a slot (it was still decoding the last — the health pill's
//   "too heavy") takes the budget down to 75 % of what was in use; calm periods give 10 % back, up to the benchmark's
//   figure. No benchmark: unlimited until the first skip says otherwise. The server saying it is loaded (dspCpu, the
//   demod queue, the CPU rung) makes every mode light at once.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace vibe {
namespace digi {

/** What a mode is doing this period — the spots box header's chips and the admin line. */
enum class RunState : uint8_t { Off, Idle, Shed, Light, Full };
inline const char* stateWord(RunState s) {
    switch (s) {
        case RunState::Off:   return "off";     // the owner switched it off for Band
        case RunState::Idle:  return "idle";    // nothing in its window this period: not decoded
        case RunState::Shed:  return "shed";    // something there, over budget: not decoded (counted, the health pill)
        case RunState::Light: return "light";   // decoding, one pass
        case RunState::Full:  return "active";  // decoding at full depth
    }
    return "idle";
}
/** The owner's word for one mode in Band (blocked-modes tokens "spotsband-<mode>" / "spotsband+<mode>"). */
enum class Override : int8_t { Off = -1, Auto = 0, Always = 1 };

struct GovIn {
    int    priority = 100;
    double cost = 1.0, light = 1.0;   ///< FT8 units, full and light
    bool   wants = false;             ///< its window holds something (or it is Always)
    /** Already decoding this period. It does NOT reserve its place: the decision is by priority alone, and a committed
     *  run the answer sheds is stopped by the caller where that is free (a free-running mode) or at its next period
     *  (a slotted one — its slot's audio has gone in). So a low-priority mode decoding now never keeps a higher one out. */
    bool   committed = false;
    Override ov = Override::Auto;
};

/** ★★ THE DECISION. `units` = the budget in FT8 units (∞ = none); `loaded` = the server says it is loaded (all light).
 *  Pure: the same inputs give the same states. */
inline std::vector<RunState> govern(const std::vector<GovIn>& in, double units, bool loaded) {
    std::vector<RunState> out(in.size(), RunState::Idle);
    std::vector<size_t> order;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i].ov == Override::Off) { out[i] = RunState::Off; continue; }
        if (!in[i].wants && in[i].ov != Override::Always) { out[i] = RunState::Idle; continue; }
        order.push_back(i);
    }
    // ★ What must run comes first (the owner's Always), then by priority; stable.
    auto must = [&](size_t i) { return in[i].ov == Override::Always; };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (must(a) != must(b)) return must(a);
        return in[a].priority < in[b].priority;
    });
    double left = std::isfinite(units) ? units : std::numeric_limits<double>::infinity();
    // 1. LIGHT for everything that fits, greedily in that order; the rest shed.
    for (size_t i : order) {
        const double c = in[i].light;
        if (must(i) || c <= left + 1e-9) { out[i] = RunState::Light; left -= c; }
        else out[i] = RunState::Shed;
    }
    // 2. FULL depth back, in priority order, while it fits (never while loaded).
    if (!loaded) {
        std::vector<size_t> up(order);
        std::stable_sort(up.begin(), up.end(), [&](size_t a, size_t b) { return in[a].priority < in[b].priority; });
        for (size_t i : up) {
            if (out[i] != RunState::Light) continue;
            const double extra = std::max(0.0, in[i].cost - in[i].light);
            if (extra <= left + 1e-9) { out[i] = RunState::Full; left -= extra; }
        }
    }
    return out;
}

/** ★ The budget, tracked live (see the file note). Driven by the spotter once per `kCheckSec`. */
class BandBudget {
public:
    static constexpr double kBudgetPct = 70.0;   ///< one core's green rung (the benchmark's own "green" line)
    static constexpr double kCheckSec  = 15.0;   ///< how often skips are looked at (FT8's slot)
    /** `capUnits`: what the benchmark says this machine affords (∞ = no benchmark). */
    void setCap(double capUnits) {
        cap_ = capUnits > 0 ? capUnits : std::numeric_limits<double>::infinity();
        if (!(units_ < cap_)) units_ = cap_;
    }
    double units() const { return units_; }
    double cap() const { return cap_; }
    /** Called every block: `skipped` = every engine's skipped slots so far (monotonic), `inUse` = the units decoding now. */
    void tick(double nowSec, uint64_t skipped, double inUse) {
        if (!(last_ > 0)) { last_ = nowSec; skipped_ = skipped; return; }
        peakUse_ = std::max(peakUse_, inUse);
        if (nowSec - last_ < kCheckSec) return;
        if (skipped > skipped_) {
            // ★ too heavy: down to 75 % of what was decoding when it happened (never below one light FT8)
            units_ = std::max(0.6, std::min(units_, peakUse_) * 0.75);
            ++cuts_;
        } else if (units_ < cap_) {
            units_ = std::isfinite(cap_) ? std::min(cap_, units_ * 1.1) : units_ * 1.1;
            if (!std::isfinite(cap_) && units_ > 64.0) units_ = cap_;   // ★ far past any band's demand: unlimited again
        }
        skipped_ = skipped; last_ = nowSec; peakUse_ = inUse;
    }
    unsigned cuts() const { return cuts_; }
private:
    double cap_ = std::numeric_limits<double>::infinity();
    double units_ = std::numeric_limits<double>::infinity();
    double last_ = 0, peakUse_ = 0;
    uint64_t skipped_ = 0;
    unsigned cuts_ = 0;
};

/** ★ The benchmark's figure → a budget: the Band row measured `bandPct` % of one core for a band whose modes' light costs
 *  sum to `bandLightUnits` (one pass each, as the row runs them). NaN / ≤ 0 = no benchmark: unlimited. */
inline double budgetUnitsFromBench(double bandPct, double bandLightUnits) {
    if (!(bandPct > 0) || !(bandLightUnits > 0)) return std::numeric_limits<double>::infinity();
    return BandBudget::kBudgetPct / (bandPct / bandLightUnits);
}

} // namespace digi
} // namespace vibe
