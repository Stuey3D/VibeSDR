// vibe_sat_pass_sched.h — WHICH UNATTENDED SATELLITE WORK RUNS NOW (V12 beta94, Stuart 11-25): a Meteor pass (or another
// picked pass), ORBCOMM sky-plot monitoring, or BOTH — and where the capture sits so that both fit. Pure: the shim's
// satPassTick asks it every 2 s (test-sat-pass-sched).
//
// ★★★ STUART: "can we do background monitoring of orbcomm, i like seeing the antenna plot filled up. I'd say for satellite
//     passes group meteor and orbcomm then allow the user to choose a priority" — then "in theory orbcomm and meteor share
//     the same capture window so on wide enough sample rates you can do both together", and "only drop on something like
//     the hf+ that cant fit". So:
//   · BOTH RUN TOGETHER whenever one capture holds the pass's downlink (+ its Doppler) AND the ORBCOMM band, with the
//     tuner's DC clear of the pass's signal (fitCapture). Meteor's 137.1 / 137.9 MHz and ORBCOMM's 137.2–137.8 MHz need
//     ~0.8 MHz: an RTL at 2.4 MS/s, an RSP at 2 MS/s+ fit; an Airspy HF+ (≤ 912 kS/s, less usable) does not.
//   · THE PRIORITY decides only when they cannot fit: Meteor first (the default) = the pass runs and ORBCOMM pauses until
//     LOS; ORBCOMM first = ORBCOMM is never interrupted for a pass (strictly: whichever the owner put first wins whenever
//     both are due, so ORBCOMM first also ends a pass that started while no ORBCOMM bird was up).
//   · NEVER DROPPED FOR CPU: the ORBCOMM half is the light sky-plot watcher (vibe_orbcomm_sky.h, ~0.1 % of a core at
//     2.4 MS/s), so a busy box keeps both; decoders under load go light as they always do (the background-load rule).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vibe {
namespace satsched {

/** A span of RF the capture must hold. `dcOk`: the tuner's DC may sit inside it (ORBCOMM — the sky-plot watcher blanks
 *  ±2 kHz round DC; never a pass's own signal). */
struct Region { double lo = 0, hi = 0; bool dcOk = false; };

constexpr double kOrbLoHz = 137.19e6, kOrbHiHz = 137.81e6;   ///< the ORBCOMM channels in use + Doppler (vibe_orbcomm_sky.h)
constexpr double kDcGuardHz = 10000;                         ///< the tuner's DC this far from a signal

inline Region orbcommRegion() { Region r; r.lo = kOrbLoHz; r.hi = kOrbHiHz; r.dcOk = true; return r; }

struct Fit {
    bool ok = false;
    double centreHz = 0;    ///< where the tuner goes
    std::string why;        ///< "" when ok; else why not, in words
};
/** A tuner centre whose usable span (`usableHz`, the capture minus its edges) holds every region, its DC kDcGuardHz clear of
 *  every region that is not dcOk — the candidate nearest the regions' middle (the edges are where a capture is worst). */
inline Fit fitCapture(double usableHz, const std::vector<Region>& rs) {
    Fit f;
    if (rs.empty() || !(usableHz > 0)) { f.why = "nothing to hold"; return f; }
    double lo = 1e18, hi = -1e18;
    for (auto& r : rs) { lo = std::min(lo, r.lo); hi = std::max(hi, r.hi); }
    const double half = usableHz / 2;
    if (hi - lo > usableHz) {
        char b[160];
        std::snprintf(b, sizeof b, "this radio's capture is too narrow for both (%.0f kHz usable, %.0f kHz needed)", usableHz / 1e3, (hi - lo) / 1e3);
        f.why = b;
        return f;
    }
    auto clear = [&](double c) {
        for (auto& r : rs) if (!r.dcOk && c > r.lo - kDcGuardHz && c < r.hi + kDcGuardHz) return false;
        return true;
    };
    auto holds = [&](double c) { return c - half <= lo + 1 && c + half >= hi - 1; };
    std::vector<double> cand;
    const double mid = 0.5 * (lo + hi);
    cand.push_back(mid);
    for (auto& r : rs) { cand.push_back(r.lo - kDcGuardHz - 1000); cand.push_back(r.hi + kDcGuardHz + 1000); }
    // gaps between the pass's signals: their middles
    std::vector<Region> s = rs;
    std::sort(s.begin(), s.end(), [](const Region& a, const Region& b) { return a.lo < b.lo; });
    for (size_t i = 0; i + 1 < s.size(); i++) if (s[i + 1].lo > s[i].hi) cand.push_back(0.5 * (s[i].hi + s[i + 1].lo));
    // inside a dcOk region: as near the middle as the guards allow
    for (auto& r : rs) if (r.dcOk) { cand.push_back(std::max(r.lo, std::min(r.hi, mid))); }
    // ★ the DC outside even a dcOk region when the capture allows (a 4 kHz hole in the sky plot is still a hole)
    auto inDcOk = [&](double c) { for (auto& r : rs) if (r.dcOk && c > r.lo - kDcGuardHz && c < r.hi + kDcGuardHz) return true; return false; };
    double best = 0, bd = 1e18;
    for (double c : cand) {
        if (!holds(c) || !clear(c)) continue;
        const double score = std::fabs(c - mid) + (inDcOk(c) ? 1e12 : 0.0);
        if (score < bd) { bd = score; best = c; }
    }
    if (bd >= 1e17) { f.why = "no tuner centre holds both with its DC clear of the pass's signal"; return f; }
    f.ok = true; f.centreHz = std::round(best);
    return f;
}

/** The span one capture needs for `pass` and the ORBCOMM band together (Hz). */
inline double bothNeedHz(const Region& pass) {
    const Region o = orbcommRegion();
    return std::max(pass.hi, o.hi) - std::min(pass.lo, o.lo);
}
/** ★ The pages' rule of thumb (the setup page and the app's server screen say "both run together" or "the priority
 *  decides"): the lowest sample rate whose flat part (`flatFrac` — vibe_band_slice.h kBandFlatFrac) holds `needHz`. The
 *  server's own decision (fitCapture on the radio's real usable span) is what runs; this is what the settings promise. */
inline double bothMinRate(double needHz, double flatFrac) { return std::ceil(needHz / flatFrac / 1000.0) * 1000.0; }
/** ORBCOMM alone: a centre that holds the band (its DC outside it where the capture allows), else the band's middle — a
 *  capture narrower than the band (an HF+ at 768 kS/s: ~614 kHz usable) still watches most of it. */
inline double orbcommAloneCentre(double usableHz) {
    const Fit f = fitCapture(usableHz, { orbcommRegion() });
    return f.ok ? f.centreHz : std::round(0.5 * (kOrbLoHz + kOrbHiHz));
}
/** "1 h 12 min" · "14 min" · "under a minute". */
inline std::string dur(double sec) {
    if (sec < 60) return "under a minute";
    const long m = long(sec / 60);
    char b[48];
    if (m >= 60) std::snprintf(b, sizeof b, "%ld h %ld min", m / 60, m % 60);
    else std::snprintf(b, sizeof b, "%ld min", m);
    return b;
}

struct In {
    bool meteorOn = true, orbcommOn = false, orbcommFirst = false;
    bool anyPasses = true;          ///< Meteor on, or a downlink picked — something a pass could be
    std::string blocked;            ///< satPassWhyNot's answer ("" = the radio may be used)
    bool listener = false;          ///< somebody is here: the radio is theirs
    // the best pass due now (a Meteor pass when meteorOn, or another picked downlink), if any
    bool passUp = false;
    std::string passName;           ///< "METEOR-M2 4 LRPT"
    Region passRegion;              ///< its signal + Doppler
    double nextPassAos = 0;         ///< the next such pass's AOS (0 = none known) — the "no Meteor pass for …" figure
    // ORBCOMM
    bool orbcommUp = false;         ///< at least one ORBCOMM bird above the horizon
    std::string orbcommReachWhy;    ///< "" = this radio tunes 137.2–137.8 MHz
    double usableHz = 0;            ///< the capture's usable span now
    bool loaded = false;            ///< ★ the box is busy — NOT a reason to drop either (decoders go light instead); here so the tests say so
    double now = 0;
};
struct Out {
    bool runPass = false, runOrbcomm = false;
    bool placeCentre = false; double centreHz = 0;   ///< put the tuner here (both together, or ORBCOMM alone)
    bool both = false;                               ///< both run together on one capture
    std::string state;                               ///< the admin line ("ORBCOMM monitoring (sky plot) — no Meteor pass for 1 h 12 min")
    std::string orbcommWhyNot;                       ///< ORBCOMM is wanted but not running: why
};

inline Out decide(const In& in) {
    Out o;
    if (!in.blocked.empty()) { o.state = in.blocked; return o; }
    if (in.listener) { o.state = "a listener has the radio"; return o; }
    const bool wantOrb = in.orbcommOn && in.orbcommUp && in.orbcommReachWhy.empty();
    if (in.orbcommOn && !in.orbcommReachWhy.empty()) o.orbcommWhyNot = in.orbcommReachWhy;
    else if (in.orbcommOn && !in.orbcommUp) o.orbcommWhyNot = "no ORBCOMM satellite above the horizon";
    const std::string what = in.meteorOn ? "Meteor" : "picked";
    if (in.passUp && wantOrb) {
        const Fit f = fitCapture(in.usableHz, { in.passRegion, orbcommRegion() });
        if (f.ok) {
            o.runPass = o.runOrbcomm = o.both = true; o.placeCentre = true; o.centreHz = f.centreHz;
            o.state = in.passName + " pass + ORBCOMM monitoring (sky plot) together \xe2\x80\x94 one capture holds both";
            return o;
        }
        if (in.orbcommFirst) {
            o.runOrbcomm = true;
            o.placeCentre = true; o.centreHz = orbcommAloneCentre(in.usableHz);
            o.state = "ORBCOMM monitoring (sky plot) \xe2\x80\x94 " + in.passName + " pass not taken (ORBCOMM first; " + f.why + ")";
            return o;
        }
        o.runPass = true;
        o.state = in.passName + " pass \xe2\x80\x94 ORBCOMM paused (priority; " + f.why + ")";
        o.orbcommWhyNot = "paused for the " + in.passName + " pass (" + f.why + ")";
        return o;
    }
    if (in.passUp) { o.runPass = true; o.state = in.passName + " pass"; return o; }
    if (wantOrb) {
        o.runOrbcomm = true;
        o.placeCentre = true; o.centreHz = orbcommAloneCentre(in.usableHz);
        o.state = "ORBCOMM monitoring (sky plot)";
        if (!in.anyPasses) o.state += " \xe2\x80\x94 no passes set up";
        else if (in.nextPassAos > in.now) o.state += " \xe2\x80\x94 no " + what + " pass for " + dur(in.nextPassAos - in.now);
        else o.state += " \xe2\x80\x94 no " + what + " pass due";
        return o;
    }
    o.state = in.nextPassAos > in.now ? "waiting \xe2\x80\x94 the next pass in " + dur(in.nextPassAos - in.now) : "waiting for a pass";
    if (!o.orbcommWhyNot.empty()) o.state += "; ORBCOMM: " + o.orbcommWhyNot;
    return o;
}

}  // namespace satsched
}  // namespace vibe
