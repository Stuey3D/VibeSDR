// vibe_afc.h — AUTOMATIC FREQUENCY CORRECTION ("auto-PPM", V12, 2026-11-12). The radio's own clock error, LEARNED from
// signals whose frequency is exact, and corrected for the whole radio so every mode lands where it should as it drifts.
// Docs: docs/v12/AUTO-PPM.md. Tests: vibeserver/test-afc.cpp (this header alone), scripts/test-server-afc.mjs (end to end).
//
// ★★★ WHY. A tester's USRP B210 (TCXO, no GPSDO) read "Frequency offset 781 Hz (4.02 ppm)" on DAB 7D, and a DAB lock then
//     broke suddenly — warm-up drift. An RTL in a battery handheld (the R36S) warms and drifts the same way. A DAB
//     multiplex is a GPS-locked SFN: its carriers are exact, and the DAB receiver already measures how far off they land.
//     That figure IS the radio's clock error; nothing used it.
//
// ★★★ ONE INTERFACE FOR EVERY REFERENCE (Reference below). Phase 1 feeds it from DAB (primary) and DRM (corroborating
//     only). Phase 2 slots in without touching the estimator: atomic time-station carriers (WWV/WWVH 5/10/15 MHz — long
//     averaging, the ionosphere adds about ±1 Hz of Doppler), LTE PSS/SSS, GSM FCCH. A source says WHERE it measured
//     (rfHz), HOW FAR OFF the reference landed there (offsetHz, + = above), how much it trusts itself, and which policy
//     applies to it (kind). ✗ NEVER FM OR RDS: a broadcaster's carrier is not a reference, and the pilot and RDS are
//     baseband — they say nothing about the radio's RF clock.
//
// ★★★ WHAT IS LEARNED IS ABSOLUTE, NOT AN ERROR SIGNAL. Each reading is turned into "the clock is ε ppm fast" using the
//     correction that was in force when it was taken: ε = applied − residual. A reading taken before a correction and one
//     taken after it therefore agree, so the window of readings survives every step, and the loop is DEADBEAT — the
//     correction is set to the estimate, not nudged by an error. With the settling blank (no readings while a change
//     is still moving through the pipeline) there is nothing to oscillate: a step of the correction moves the residual
//     by exactly minus that step, and the estimate stays put. test-afc.cpp proves it on a simulated radio with latency.
//
// ★★ ONE BAD READING NEVER MOVES IT. Median of a window, a spread gate (no decision while the readings disagree), an
//    outlier gate around the median, a step cap (0.5 ppm) and a minimum interval (30 s) between steps once acquired.
//    A run of outliers that agree among themselves IS the truth having moved (another radio on the same serial, a big
//    temperature change) — the window is replaced by them, never by a single one.
//
// ★ Sign convention, librtlsdr's: +ppm = the radio's crystal runs FAST, so its LO sits high and a signal lands LOW
//   (offset −ε·f). Hardware ppm takes the same sign; the digital remainder shifts the IQ UP by ρ·f.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "vibe_dab_scan_store.h"   // vibedabscan::writeAtomic — temp + fsync + rename + directory sync

namespace vibeafc {

/** How a source may be used. */
enum class Kind {
    Primary,        ///< exact and identifiable (DAB: a GPS-locked SFN) — may acquire on its own
    Corroborating,  ///< good but not always exact (DRM: most transmitters are within a hertz, not all) — counted only when it
                    ///< AGREES with what primaries already established; never acquires
    Averaged,       ///< exact at source but smeared on the way (an HF time station: ionospheric Doppler ±1 Hz) — phase 2;
                    ///< the SOURCE averages minutes of carrier into one Reference before offering it, then it is a primary
};

/** ONE reading, from any reference. */
struct Reference {
    std::string source;     ///< in words, for the owner: "DAB 7D", "DRM 3965 kHz", "WWV 10 MHz"
    double rfHz = 0;        ///< the RF frequency the reading was taken at
    double offsetHz = 0;    ///< where the reference landed relative to where it should be (+ = above), WITH the correction in force
    double confidence = 1;  ///< 0..1 — the source's own view (reserved for weighting; the gates already passed)
    double at = 0;          ///< monotonic seconds when the reading's span ENDED
    Kind kind = Kind::Primary;
};

/** The reading as a residual clock error in ppm (+ = the signal landed high = the radio is SLOW by that much). */
inline double residualPpm(const Reference& r) { return r.rfHz > 0 ? r.offsetHz / r.rfHz * 1e6 : 0.0; }
/** The radio's absolute clock error implied by a reading, given the correction that was in force. */
inline double absolutePpm(const Reference& r, double appliedPpm) { return appliedPpm - residualPpm(r); }

struct Config {
    int    minSamples   = 10;     ///< readings before the first decision (DAB: one per 2 s housekeeping tick = 20 s of clean lock)
    size_t window       = 60;     ///< readings kept (2 minutes of DAB)
    double maxSpreadPpm = 0.25;   ///< no decision while the window's median absolute deviation is wider than this
    double outlierPpm   = 1.5;    ///< a reading this far from the median is an outlier (once the window has minSamples)
    int    regroupAfter = 30;     ///< …and this many outliers in a row that AGREE among themselves replace the window
    double agreePpm     = 1.0;    ///< a corroborating reading must sit this close to the primaries' median
    double minIntervalS = 30;     ///< between steps once acquired
    double maxStepPpm   = 0.5;    ///< largest step once acquired (the first acquisition is not capped)
    double deadbandPpm  = 0.05;   ///< smaller differences are left alone (no busywork, no dither)
};

inline double medianOf(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(m), v.end());
    double hi = v[m];
    if (v.size() % 2) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + long(m));
    return 0.5 * (lo + hi);
}
inline double madOf(const std::vector<double>& v, double med) {
    std::vector<double> d; d.reserve(v.size());
    for (double x : v) d.push_back(std::fabs(x - med));
    return medianOf(std::move(d));
}

/** The estimator: readings in, a correction out. Pure — no clock, no radio, no thread; the caller holds any lock. */
class Estimator {
public:
    enum class Verdict { Accepted, Outlier, Disagrees, Settling, Regrouped };

    explicit Estimator(Config c = {}) : c_(c) {}
    const Config& config() const { return c_; }

    /** No reading whose span ended before `t` counts — a correction change is still moving through the pipeline. */
    void blankUntil(double t) { blank_ = std::max(blank_, t); }

    /** Offer one reading, taken with `appliedPpm` in force. */
    Verdict offer(const Reference& r, double appliedPpm) {
        if (r.at < blank_) { ++settling_; return Verdict::Settling; }
        const double x = absolutePpm(r, appliedPpm);
        const bool primary = r.kind != Kind::Corroborating;
        if (!primary) {
            // ★ DRM (and kin) only ever CONFIRM: no primaries yet, or it disagrees with them = not counted
            if (primaries_ < c_.minSamples || std::fabs(x - median()) > c_.agreePpm) { ++disagree_; return Verdict::Disagrees; }
        } else if (int(win_.size()) >= c_.minSamples && std::fabs(x - median()) > c_.outlierPpm) {
            ++outliers_;
            run_.push_back(x);
            if (int(run_.size()) >= c_.regroupAfter) {
                const double m = medianOf(run_);
                if (madOf(run_, m) <= c_.maxSpreadPpm) {
                    // ★ the truth moved: a whole run agrees with itself and not with the window
                    win_.clear(); primaries_ = 0;
                    for (double v : run_) push_(v, true);
                    run_.clear(); ++regroups_;
                    lastSource_ = r.source; lastAt_ = r.at;
                    return Verdict::Regrouped;
                }
                run_.erase(run_.begin());
            }
            return Verdict::Outlier;
        }
        run_.clear();
        push_(x, primary);
        lastSource_ = r.source; lastAt_ = r.at;
        ++accepted_;
        return Verdict::Accepted;
    }

    /** The correction to put in force now, if it should move. `acquired` = a learned value is already in force (the
     *  first acquisition is applied whole; after that, steps are capped and spaced). */
    bool decide(double now, double appliedPpm, bool acquired, double& next) const {
        if (primaries_ < c_.minSamples) return false;
        const double med = median();
        if (spread() > c_.maxSpreadPpm) return false;
        if (!acquired) { next = med; return true; }
        if (now - lastChange_ < c_.minIntervalS) return false;
        const double d = med - appliedPpm;
        if (std::fabs(d) < c_.deadbandPpm) return false;
        next = appliedPpm + std::max(-c_.maxStepPpm, std::min(c_.maxStepPpm, d));
        return true;
    }
    /** The caller put a decision in force at `now`. */
    void noteChanged(double now) { lastChange_ = now; }

    double median() const { std::vector<double> v; for (const auto& e : win_) v.push_back(e.v); return medianOf(std::move(v)); }
    double spread() const { std::vector<double> v; for (const auto& e : win_) v.push_back(e.v); return madOf(v, medianOf(v)); }
    size_t count() const { return win_.size(); }
    int primaries() const { return primaries_; }
    const std::string& lastSource() const { return lastSource_; }
    double lastAt() const { return lastAt_; }
    int outliers() const { return outliers_; }
    int settling() const { return settling_; }
    int disagreements() const { return disagree_; }
    int regroups() const { return regroups_; }
    void clear() { *this = Estimator(c_); }

private:
    struct E { double v; bool primary; };
    void push_(double v, bool primary) {
        win_.push_back({v, primary});
        if (primary) ++primaries_;
        while (win_.size() > c_.window) { if (win_.front().primary) --primaries_; win_.pop_front(); }
    }
    Config c_;
    std::deque<E> win_;
    std::vector<double> run_;
    int primaries_ = 0, outliers_ = 0, settling_ = 0, disagree_ = 0, regroups_ = 0, accepted_ = 0;
    double blank_ = -1e300, lastChange_ = -1e300, lastAt_ = 0;
    std::string lastSource_;
};

/* ── THE DAB READING'S GATES ──────────────────────────────────────────────────────────────────────────────────────────
 *  Only a STRONG, CLEAN, SETTLED lock: locked through the whole span, the FIC decoding (FIB rate ≥ 0.95 — the FIC is the
 *  best-protected thing on the air, so a lower rate is a marginal lock), the MER clear of the noise, no re-acquisition in
 *  the span or for kStableS before it, enough frames averaged, and a residual a real radio could have. */
struct DabRead {
    bool locked = false;          ///< locked on every frame of the span
    int frames = 0;               ///< frames averaged
    double offsetHz = 0;          ///< their mean measured offset (vibedab::DabStats::freqOffsetHz)
    double blockHz = 0;           ///< the block's centre
    float merDb = 0, fibRate = 0;
    bool disturbed = false;       ///< a re-acquisition, a block change or an unlocked frame inside the span
};
struct DabGate {
    double minMerDb = 8.0, minFib = 0.95, stableS = 10.0, maxAbsPpm = 100.0;
    int minFrames = 8;
};
/** nullptr = usable; else why not, in words (the admin's diagnostics). `stableFor` = seconds the lock has been undisturbed. */
inline const char* dabWhyNot(const DabRead& r, double stableFor, const DabGate& g = {}) {
    if (!r.locked) return "not locked";
    if (r.disturbed) return "lock disturbed";
    if (r.frames < g.minFrames) return "too few frames";
    if (r.fibRate < g.minFib) return "FIC not clean";
    if (r.merDb < g.minMerDb) return "MER too low";
    if (stableFor < g.stableS) return "lock not yet settled";
    if (r.blockHz <= 0 || std::fabs(r.offsetHz / r.blockHz * 1e6) > g.maxAbsPpm) return "offset not plausible";
    return nullptr;
}

/* ── DRM'S GATES (corroborating) ──────────────────────────────────────────────────────────────────────────────────────
 *  The DRM receiver measures the signal's offset from the DIAL, so the dial must be ON the station's nominal frequency:
 *  a whole kilohertz (the broadcast grid), and the residual small enough that it is the clock and not a listener tuned
 *  off-channel. Its MER clear of the noise. Never acquires (Kind::Corroborating). */
struct DrmGate { double minMerDb = 12.0, maxAbsHz = 150.0; };
inline const char* drmWhyNot(bool locked, double dialHz, double offsetHz, double merDb, const DrmGate& g = {}) {
    if (!locked) return "not locked";
    if (dialHz < 1e6) return "below 1 MHz";   // ★ 1 Hz of transmitter error is 1 ppm here — too coarse
    const double k = dialHz / 1000.0;
    if (std::fabs(k - std::round(k)) > 1e-6) return "dial not on a whole kHz";
    if (merDb < g.minMerDb) return "MER too low";
    if (std::fabs(offsetHz) > g.maxAbsHz) return "offset too large (tuned off the channel?)";
    return nullptr;
}

/* ── HOW A CORRECTION IS SPLIT BETWEEN THE RADIO AND THE IQ ───────────────────────────────────────────────────────────
 *  A radio with an integer ppm control (RTL, rtl_tcp, RSP) takes WHOLE ppm in hardware — that also corrects its sample
 *  clock and keeps the capture's edges where they were — and the remainder is a digital shift. A hardware write retunes
 *  (a brief disturbance), so it moves only on the first acquisition or when the remainder would pass kHwHysteresis;
 *  everything between is the digital shift, which glides. A radio without one (Airspy, HackRF, USRP, HF+) is all
 *  digital. */
constexpr double kHwHysteresis = 1.5;
inline int hwPpmFor(double total, int curHw, bool first, bool hwCapable) {
    if (!hwCapable) return curHw;
    if (!first && std::fabs(total - curHw) <= kHwHysteresis) return curHw;
    const double r = std::round(total);
    return int(std::max(-500.0, std::min(500.0, r)));
}

/** The digital shift glides: kSlewPpmPerS at most (0.5 ppm in 5 s — 100 Hz at 200 MHz over 50 DAB frames, ~2 Hz a
 *  frame, nothing a DQPSK decision notices; 7 Hz at 14 MHz, nothing an SSB ear notices). */
constexpr double kSlewPpmPerS = 0.1;
inline double slewToward(double cur, double target, double dtS) {
    const double m = kSlewPpmPerS * std::max(0.0, dtS);
    return cur + std::max(-m, std::min(m, target - cur));
}

/* ── THE STORE, PER RADIO ─────────────────────────────────────────────────────────────────────────────────────────────
 *  One small text file per radio serial (the caller names it, as vibe_dab_scan_store.h's), written atomically.
 *    VIBE-AFC 1
 *    ppm <learned, 4 decimals> at <unix seconds> hw <integer in the radio when saved>
 *    on <0|1>                          (the handheld only — it has no setup page; servers keep the switch in their config)
 *    src <the reference, in words>
 *  A file past 4 KB or not starting with the magic is damage and is not read. */
struct Saved {
    bool have = false;      ///< a learned value is present
    double ppm = 0;
    int64_t at = 0;
    int hw = 0;
    int on = -1;            ///< -1 = not stored (a server)
    std::string source;
};
inline std::string serialise(const Saved& s) {
    std::string o = "VIBE-AFC 1\n";
    char b[128];
    if (s.have) {
        std::snprintf(b, sizeof b, "ppm %.4f at %lld hw %d\n", s.ppm, (long long)s.at, s.hw);
        o += b;
        std::string src;
        for (char c : s.source) if (c != '\n' && c != '\r' && c != '\0' && src.size() < 64) src += c;
        if (!src.empty()) o += "src " + src + "\n";
    }
    if (s.on >= 0) o += std::string("on ") + (s.on ? "1" : "0") + "\n";
    return o;
}
inline bool parse(const std::string& t, Saved& out) {
    out = Saved{};
    if (t.size() > 4096 || t.compare(0, 11, "VIBE-AFC 1\n") != 0) return false;
    size_t p = 11;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(p, e - p);
        p = e + 1;
        if (line.rfind("ppm ", 0) == 0) {
            double v = 0; long long at = 0; int hw = 0;
            if (std::sscanf(line.c_str(), "ppm %lf at %lld hw %d", &v, &at, &hw) >= 2 && std::isfinite(v) && std::fabs(v) <= 500) {
                out.have = true; out.ppm = v; out.at = at; out.hw = hw;
            }
        } else if (line.rfind("src ", 0) == 0) out.source = line.substr(4, 64);
        else if (line.rfind("on ", 0) == 0) out.on = line.size() > 3 && line[3] == '1' ? 1 : 0;
    }
    return true;
}
inline bool load(const std::string& path, Saved& out) {
    out = Saved{};
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4097];
    const size_t n = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    if (n > 4096) return false;
    return parse(std::string(buf, n), out);
}
inline bool save(const std::string& path, const Saved& s, std::string* err = nullptr) {
    return vibedabscan::writeAtomic(path, serialise(s), err);
}

/** "auto-corrected +4.0 ppm from DAB 7D, 2 min ago" — the owner's line (setup page, admin page, the handheld). */
inline std::string describe(const Saved& s, int64_t nowUnix) {
    if (!s.have) return "not learned yet";
    char b[160];
    const long long ago = std::max<long long>(0, (long long)(nowUnix - s.at));
    char when[48];
    if (ago < 90) std::snprintf(when, sizeof when, "just now");
    else if (ago < 5400) std::snprintf(when, sizeof when, "%lld min ago", ago / 60);
    else if (ago < 172800) std::snprintf(when, sizeof when, "%lld h ago", ago / 3600);
    else std::snprintf(when, sizeof when, "%lld days ago", ago / 86400);
    std::snprintf(b, sizeof b, "auto-corrected %+.1f ppm%s%s, %s", s.ppm, s.source.empty() ? "" : " from ", s.source.c_str(), when);
    return b;
}

}  // namespace vibeafc
