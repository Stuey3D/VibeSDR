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
// ★★★ ON BY DEFAULT, AND THEREFORE CAREFUL (Stuart, 2026-11-12: "I would prefer it to be on by default so it plays to our
//     plug and play ethos … We need to tread carefully though as this has the potential to fuck up a users radio in
//     subtle ways and a user might not be able to diagnose it"). The Controller below is where the care lives:
//       1. PLAUSIBILITY CAPS per driver (capFor) — a reading outside is rejected and logged, never applied; a step after
//          acquisition is at most 0.5 ppm; the first acquisition glides, never jumps.
//       2. THE 1 kHz CARRIER AMBIGUITY — a wrong DAB integer carrier shift is exactly 1 kHz (≈5 ppm in Band III). Only a
//          CLEAN decode at that offset is a reading (FIC passing, the MSC's error rate low — dabWhyNot), and the first
//          acquisition needs agreement OVER TIME (acquireSpanS of clean readings) or ACROSS BLOCKS; readings from
//          different references that disagree stop every decision (a wrong shift is a different ppm on another block).
//       3. SELF-CHECK AND AUTO-REVERT — after each change the reference must not have got worse (its residual not grown,
//          its MER not fallen); worse = revert at once; worse twice = SUSPENDED for this radio, back to the manual ppm,
//          and said so everywhere until the owner resets it.
//       4. ALWAYS VISIBLE, ALWAYS UNDOABLE — describe() is the one line every surface shows; Reset and Turn off return
//          to the manual ppm EXACTLY, because:
//       5./6. THE RADIO'S OWN PPM IS NEVER TOUCHED. The owner's manual ppm stays in the radio as they set it; the learned
//          correction is a DIGITAL SHIFT ON TOP of it, which glides (0.1 ppm/s) so no listener ever sees the dial jump,
//          and which is simply zero when the correction is reset, off or not yet confident.
//       ★ Why not the integer part in the RTL's tuner, as first briefed: a hardware write retunes (a visible, audible
//         disturbance under every listener), it would REPLACE the owner's manual value in the radio, and Reset could
//         then only be as exact as the hardware write-back. All-digital costs the sample clock's own error (a few ppm
//         of audio-rate drift that every server already lives with today) and moves the DC spike off centre by ε·f.
//
// ★ Sign convention, librtlsdr's: +ppm = the radio's crystal runs FAST, so its LO sits high and a signal lands LOW
//   (offset −ε·f). The digital correction of ε ppm shifts the IQ UP by ε·f (f = the radio's tuned frequency).
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
    size_t window       = 90;     ///< readings kept (3 minutes of DAB — longer than acquireSpanS)
    double maxSpreadPpm = 0.25;   ///< no decision while the window's median absolute deviation is wider than this
    double outlierPpm   = 1.5;    ///< a reading this far from the median is an outlier (once the window has minSamples)
    int    regroupAfter = 30;     ///< …and this many outliers in a row that AGREE among themselves replace the window
    double agreePpm     = 1.0;    ///< a corroborating reading must sit this close to the primaries' median
    double minIntervalS = 30;     ///< between steps once acquired
    double maxStepPpm   = 0.5;    ///< largest step once acquired (the first acquisition is not capped)
    double deadbandPpm  = 0.05;   ///< smaller differences are left alone (no busywork, no dither)
    double acquireSpanS = 120;    ///< the FIRST acquisition needs clean readings spanning this long (agreement over time)…
    int    minPerSource = 3;      ///< …or this many from each of two references that agree (agreement across blocks)
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
                    for (double v : run_) push_(v, true, r.at, r.source);
                    run_.clear(); ++regroups_;
                    lastSource_ = r.source; lastAt_ = r.at;
                    return Verdict::Regrouped;
                }
                run_.erase(run_.begin());
            }
            return Verdict::Outlier;
        }
        run_.clear();
        push_(x, primary, r.at, r.source);
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
        if (sourcesDisagree()) return false;   // ★ rule 2: references that disagree stop everything
        if (!acquired) {
            // ★ rule 2: agreement over time, or across two references
            const double span = win_.back().at - win_.front().at;
            if (span < c_.acquireSpanS && agreeingSources() < 2) return false;
            next = med; return true;
        }
        if (now - lastChange_ < c_.minIntervalS) return false;
        const double d = med - appliedPpm;
        if (std::fabs(d) < c_.deadbandPpm) return false;
        next = appliedPpm + std::max(-c_.maxStepPpm, std::min(c_.maxStepPpm, d));
        return true;
    }
    /** The caller put a decision in force at `now`. */
    void noteChanged(double now) { lastChange_ = now; }

    /** Two references (blocks, stations) with minPerSource readings each whose medians differ by more than agreePpm. A
     *  wrong DAB carrier shift is 1 kHz — 5.7 ppm at 174 MHz, 4.2 at 240 — so it cannot agree with another block. */
    bool sourcesDisagree() const {
        const auto m = perSource_();
        for (size_t i = 0; i < m.size(); ++i)
            for (size_t j = i + 1; j < m.size(); ++j)
                if (std::fabs(m[i].second - m[j].second) > c_.agreePpm) return true;
        return false;
    }
    int agreeingSources() const { return sourcesDisagree() ? 0 : int(perSource_().size()); }
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
    struct E { double v; bool primary; double at; std::string src; };
    std::vector<std::pair<std::string, double>> perSource_() const {
        std::vector<std::pair<std::string, double>> out;
        std::vector<std::string> names;
        for (const auto& e : win_) if (e.primary && std::find(names.begin(), names.end(), e.src) == names.end()) names.push_back(e.src);
        for (const auto& n : names) {
            std::vector<double> v;
            for (const auto& e : win_) if (e.primary && e.src == n) v.push_back(e.v);
            if (int(v.size()) >= c_.minPerSource) out.push_back({n, medianOf(std::move(v))});
        }
        return out;
    }
    void push_(double v, bool primary, double at = 0, const std::string& src = std::string()) {
        win_.push_back({v, primary, at, src});
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
 *  best-protected thing on the air, and its CRC cannot pass with every carrier in the wrong bin, so this is what rules
 *  out a wrong integer carrier shift), the MSC's own error rate low where a service is being decoded, the MER clear of
 *  the noise, no re-acquisition in the span or for stableS before it, enough frames averaged, and a residual a real
 *  radio could have. */
struct DabRead {
    bool locked = false;          ///< locked on every frame of the span
    int frames = 0;               ///< frames averaged
    double offsetHz = 0;          ///< their mean measured offset (vibedab::DabStats::freqOffsetHz)
    double blockHz = 0;           ///< the block's centre
    float merDb = 0, fibRate = 0;
    double mscBer = 0;            ///< the MSC's pre-Viterbi error rate (0 = no service decoded)
    bool disturbed = false;       ///< a re-acquisition, a block change or an unlocked frame inside the span
};
struct DabGate {
    double minMerDb = 8.0, minFib = 0.95, stableS = 10.0, maxAbsPpm = 200.0, maxMscBer = 0.08;
    int minFrames = 8;
};
/** nullptr = usable; else why not, in words (the admin's diagnostics). `stableFor` = seconds the lock has been undisturbed. */
inline const char* dabWhyNot(const DabRead& r, double stableFor, const DabGate& g = {}) {
    if (!r.locked) return "not locked";
    if (r.disturbed) return "lock disturbed";
    if (r.frames < g.minFrames) return "too few frames";
    if (r.fibRate < g.minFib) return "FIC not clean";
    if (r.mscBer > g.maxMscBer) return "audio frames not clean";
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

/* ── RULE 1: THE PLAUSIBILITY CAP, PER DRIVER ─────────────────────────────────────────────────────────────────────────
 *  The largest clock error the radio could REALLY have, from what it is built with. A reading that implies more is
 *  rejected and logged, never applied:
 *    rtl       100 — a crystal dongle without a TCXO is tens of ppm out (50+ measured on the cheapest); the Blog V3/V4
 *                    have a 1 ppm TCXO, but a dongle is a dongle until it says otherwise
 *    hackrf     30 — a 20 ppm crystal unless the TCXO option is fitted
 *    sdrplay, airspy, airspyhf, usrp  10 — TCXO radios, 0.5–2 ppm by specification; the B210 that started this
 *                    measured 4.02, so 10 leaves room for an aged one and none for a 1 kHz DAB slip (≈5 ppm) on top
 *    anything else (SpyServer, an unknown front end) 100. */
inline double capFor(const std::string& driver) {
    if (driver == "rtl" || driver == "rtlsdr") return 100;
    if (driver == "hackrf") return 30;
    if (driver == "sdrplay" || driver == "airspy" || driver == "airspyhf" || driver == "usrp") return 10;
    return 100;
}

/** The digital correction glides: kSlewPpmPerS at most (0.5 ppm in 5 s — 100 Hz at 200 MHz over 50 DAB frames, ~2 Hz a
 *  frame, nothing a DQPSK decision notices; 1.4 Hz a second at 14 MHz, nothing an SSB ear notices). The FIRST acquisition
 *  glides too — 4 ppm takes 40 s — so no listener on a shared dial ever sees it jump (rule 5). */
constexpr double kSlewPpmPerS = 0.1;
inline double slewToward(double cur, double target, double dtS) {
    const double m = kSlewPpmPerS * std::max(0.0, dtS);
    return cur + std::max(-m, std::min(m, target - cur));
}

/* ── THE STORE, PER RADIO ─────────────────────────────────────────────────────────────────────────────────────────────
 *  One small text file per radio serial (the caller names it, as vibe_dab_scan_store.h's), written atomically.
 *    VIBE-AFC 1
 *    ppm <learned, 4 decimals> at <unix seconds> hw <the manual ppm it was learned on top of>
 *    src <the reference, in words>
 *    reverts <n> suspended <0|1>        (rule 3 — survives a restart, so a suspended radio stays suspended)
 *    on <0|1>                          (the handheld only — it has no setup page; servers keep the switch in their config)
 *  A file past 4 KB or not starting with the magic is damage and is not read.
 *  ★ The learned value is the CORRECTION ON TOP OF THE MANUAL PPM (hw), not the clock's absolute error: if the owner
 *    later changes the manual ppm, the radio has been re-trimmed under it and the saved figure no longer applies — the
 *    caller discards it (Controller::restore). */
struct Saved {
    bool have = false;      ///< a learned value is present
    double ppm = 0;         ///< the digital correction on top of the manual ppm
    int64_t at = 0;
    int hw = 0;             ///< the manual ppm in the radio when it was learned
    int on = -1;            ///< -1 = not stored (a server)
    int reverts = 0;
    bool suspended = false;
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
    if (s.reverts || s.suspended) { std::snprintf(b, sizeof b, "reverts %d suspended %d\n", s.reverts, s.suspended ? 1 : 0); o += b; }
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
            if (std::sscanf(line.c_str(), "ppm %lf at %lld hw %d", &v, &at, &hw) >= 2 && std::isfinite(v) && std::fabs(v) <= 100) {
                out.have = true; out.ppm = v; out.at = at; out.hw = hw;
            }
        } else if (line.rfind("src ", 0) == 0) out.source = line.substr(4, 64);
        else if (line.rfind("reverts ", 0) == 0) {
            int n = 0, su = 0;
            if (std::sscanf(line.c_str(), "reverts %d suspended %d", &n, &su) == 2) { out.reverts = std::max(0, std::min(99, n)); out.suspended = su != 0; }
        }
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

inline std::string jsonEsc(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o;
}

/** "Auto-corrected +4.0 ppm from DAB 7D, 2 min ago" — the owner's line (setup page, admin page, the app, the handheld). */
inline std::string agoText(int64_t secs) {
    char when[48];
    if (secs < 90) std::snprintf(when, sizeof when, "just now");
    else if (secs < 5400) std::snprintf(when, sizeof when, "%lld min ago", (long long)(secs / 60));
    else if (secs < 172800) std::snprintf(when, sizeof when, "%lld h ago", (long long)(secs / 3600));
    else std::snprintf(when, sizeof when, "%lld days ago", (long long)(secs / 86400));
    return when;
}

/* ── THE CONTROLLER: the estimator, the caps, the self-check, revert and suspend (rules 1–3, 6) ───────────────────────
 *  Pure, like the estimator: the caller supplies time, the readings and the correction actually in force (the glide's
 *  position), and carries out the Action it is given. The correction it manages is the DIGITAL one ON TOP OF THE
 *  MANUAL PPM; 0 = the radio exactly as the owner set it. */
struct Action {
    enum Kind { None, Apply, Revert, Suspend } kind = None;
    double from = 0, to = 0;   ///< the correction before and after (ppm on top of the manual)
    std::string why;           ///< one line for the server log
};
class Controller {
public:
    explicit Controller(double capPpm = 100, Config c = {}) : est_(c), cap_(capPpm) {}

    /** The owner's manual ppm, already in the radio — the cap is on the radio's TOTAL error, manual + learned. */
    void setManual(int ppm) { manual_ = ppm; }
    void setCap(double c) { cap_ = c; }
    /** The owner changed the manual ppm: the radio was re-trimmed under the learned value, so it no longer applies.
     *  A suspension stands (only Reset lifts it). */
    void forgetLearned() { target_ = 0; acquired_ = false; verify_ = Verify{}; est_.clear(); source_.clear(); learnedAt_ = confirmedAt_ = 0; }
    int manual() const { return manual_; }
    // ── the switch and the owner's buttons ──
    bool on() const { return on_; }
    void setOn(bool v) { if (on_ == v) return; on_ = v; if (!v) { target_ = 0; verify_ = Verify{}; est_.clear(); } }
    /** Reset: forget everything learned and any suspension — back to the manual ppm exactly. */
    void reset() { target_ = 0; acquired_ = false; verify_ = Verify{}; est_.clear(); reverts_ = 0; suspended_ = false;
                   source_.clear(); learnedAt_ = 0; confirmedAt_ = 0; }
    /** At start: the saved state, if it was learned on top of the SAME manual ppm (else the radio was re-trimmed). */
    void restore(const Saved& s, int manualPpm) {
        reverts_ = s.reverts; suspended_ = s.suspended;
        manual_ = manualPpm;
        if (s.have && s.hw == manualPpm && std::fabs(manualPpm + s.ppm) <= cap_ && !suspended_) {
            target_ = s.ppm; acquired_ = true; source_ = s.source; learnedAt_ = confirmedAt_ = s.at;
        }
    }
    Saved saved(int manualPpm) const {
        Saved s; s.have = acquired_; s.ppm = target_; s.at = learnedAt_; s.hw = manualPpm; s.source = source_;
        s.reverts = reverts_; s.suspended = suspended_; return s;
    }

    /** What should be in force: 0 unless on, learned and not suspended. */
    double target() const { return on_ && !suspended_ && acquired_ ? target_ : 0.0; }
    bool acquired() const { return acquired_; }
    bool suspended() const { return suspended_; }
    int reverts() const { return reverts_; }
    bool verifying() const { return verify_.active; }
    double cap() const { return cap_; }
    const std::string& source() const { return source_; }
    int64_t learnedAt() const { return learnedAt_; }
    int64_t confirmedAt() const { return confirmedAt_; }
    const Estimator& estimator() const { return est_; }
    void blankUntil(double t) { est_.blankUntil(t); verify_.notBefore = std::max(verify_.notBefore, t); }
    const std::string& lastWhyNot() const { return why_; }
    void noteWhyNot(const char* w) { why_ = w ? w : ""; }

    /** One reading. `inForce` = the correction applied when it was taken (the glide's position); `merDb` and `block`
     *  for the self-check (MER compared only on the same block); `now` monotonic, `unixNow` for the owner's line. */
    Action offer(const Reference& r, float merDb, int block, double inForce, double now, int64_t unixNow) {
        Action a;
        if (!on_ || suspended_) return a;
        const double resid = residualPpm(r);
        const double x = absolutePpm(r, inForce);
        // ── rule 1: outside the cap = rejected, never applied ──
        if (std::fabs(manual_ + x) > cap_) {
            ++capRejects_;
            char b[200];
            std::snprintf(b, sizeof b, "rejected a reading of %+.2f ppm from %s — beyond this radio's %.0f ppm limit", manual_ + x, r.source.c_str(), cap_);
            why_ = b; a.why = b;
            return a;
        }
        // ── rule 3: a change in force is checked before anything else moves ──
        if (verify_.active) {
            if (r.at < verify_.notBefore) return a;
            verify_.resid += std::fabs(resid); ++verify_.n;
            if (block == verify_.block) { verify_.mer += merDb; ++verify_.merN; }
            if (verify_.n < kVerifyReadings) return a;
            const double newResid = verify_.resid / verify_.n;
            const bool residWorse = newResid > verify_.oldResid + 0.1 && newResid > 0.2;
            const bool merWorse = verify_.merN >= 2 && verify_.oldMerN > 0
                                  && verify_.mer / verify_.merN < verify_.oldMer - 2.0f;
            if (!residWorse && !merWorse) {
                verify_ = Verify{}; confirmedAt_ = unixNow;   // ★ confirmed: carry on
            } else {
                ++reverts_;
                char b[240];
                std::snprintf(b, sizeof b, "%s after the change to %+.2f ppm (offset %.2f → %.2f ppm, MER %.1f → %.1f dB)",
                              residWorse ? "the reference got worse" : "the MER fell", target_, verify_.oldResid, newResid,
                              verify_.oldMer, verify_.merN ? verify_.mer / verify_.merN : 0.0f);
                a.from = target_;
                est_.clear();
                verify_ = Verify{};
                if (reverts_ >= 2) {
                    suspended_ = true; target_ = 0; acquired_ = false; source_.clear();
                    a.kind = Action::Suspend; a.to = 0;
                    a.why = std::string("suspended: ") + b + " — the second time; back to the manual setting until the owner resets it";
                } else {
                    target_ = prev_; acquired_ = prevAcquired_;
                    a.kind = Action::Revert; a.to = target_;
                    a.why = std::string("reverted: ") + b;
                }
                why_ = a.why;
                return a;
            }
        }
        const auto v = est_.offer(r, inForce);
        if (v == Estimator::Verdict::Accepted || v == Estimator::Verdict::Regrouped) {
            lastResid_[lastN_ % kVerifyReadings] = std::fabs(resid); lastMer_[lastN_ % kVerifyReadings] = merDb;
            lastBlock_[lastN_ % kVerifyReadings] = block; ++lastN_;
            if (acquired_ && std::fabs(x - target_) < 0.25) confirmedAt_ = unixNow;
        } else if (v == Estimator::Verdict::Outlier) {
            why_ = "an outlying reading was set aside";
        }
        double next;
        if (!est_.decide(now, target_, acquired_, next)) {
            if (est_.sourcesDisagree()) why_ = "references disagree \xe2\x80\x94 nothing applied";
            return a;
        }
        if (std::fabs(manual_ + next) > cap_) { why_ = "the estimate is beyond this radio's limit \xe2\x80\x94 nothing applied"; return a; }
        // ── apply, and arm the self-check against the readings just before it ──
        prev_ = target_; prevAcquired_ = acquired_;
        a.kind = Action::Apply; a.from = target_; a.to = next;
        target_ = next; acquired_ = true; source_ = r.source; learnedAt_ = confirmedAt_ = unixNow;
        est_.noteChanged(now);
        verify_ = Verify{}; verify_.active = true; verify_.block = block;
        const int k = std::min(lastN_, kVerifyReadings);
        for (int i = 0; i < k; ++i) {
            verify_.oldResid += lastResid_[i];
            if (lastBlock_[i] == block) { verify_.oldMer += lastMer_[i]; ++verify_.oldMerN; }
        }
        if (k) verify_.oldResid /= k;
        if (verify_.oldMerN) verify_.oldMer /= verify_.oldMerN;
        char b[200];
        std::snprintf(b, sizeof b, "%+.2f \xe2\x86\x92 %+.2f ppm from %s (MER %.1f dB, offset %+.0f Hz)", a.from, a.to, r.source.c_str(), merDb, r.offsetHz);
        a.why = b;
        why_.clear();
        return a;
    }
    /** The caller's glide (and pipeline) will have the new value in force by `t`: no reading before it counts. */
    void settleUntil(double t) { blankUntil(t); }

    /** The owner's line (rule 4). */
    std::string describe(int64_t unixNow) const {
        if (!on_) return "Automatic frequency correction is off.";
        if (suspended_) return "Automatic frequency correction is suspended: it made reception worse twice, so this radio "
                               "is back on its manual setting. Reset to try again.";
        if (!acquired_) return "Learning \xe2\x80\x94 nothing applied yet (it waits for a clean DAB lock).";
        char b[200];
        std::snprintf(b, sizeof b, "Auto-corrected %+.1f ppm%s%s, %s", target_, source_.empty() ? "" : " from ", source_.c_str(),
                      agoText(std::max<int64_t>(0, unixNow - confirmedAt_)).c_str());
        return b;
    }
    int capRejects() const { return capRejects_; }

private:
    static constexpr int kVerifyReadings = 5;
    struct Verify { bool active = false; double notBefore = -1e300; int block = -1; double oldResid = 0; float oldMer = 0;
                    int oldMerN = 0; double resid = 0; float mer = 0; int n = 0, merN = 0; };
    Estimator est_;
    double cap_;
    bool on_ = true, acquired_ = false, suspended_ = false, prevAcquired_ = false;
    double target_ = 0, prev_ = 0;
    int reverts_ = 0, capRejects_ = 0, manual_ = 0;
    std::string source_, why_;
    int64_t learnedAt_ = 0, confirmedAt_ = 0;
    Verify verify_;
    double lastResid_[kVerifyReadings] = {}; float lastMer_[kVerifyReadings] = {}; int lastBlock_[kVerifyReadings] = {};
    int lastN_ = 0;
};

}  // namespace vibeafc
