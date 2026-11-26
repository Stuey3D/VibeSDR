// vibe_rate_guard.h — when a whole-radio mode BOOSTS the sample rate (ADS-B to 2.4/2.56 MS/s, DAB to 2.048, ISM to
// 2.56, the datalink to 8, under the owner's switches) and the radio falls over there, notice, recover at the radio's
// own rate, and — only when it is a pattern, not a one-off — hold the boosts one rung lower for a while (Stuart,
// 2026-10-24: "if the SDR crashes when being run in 3.2 MS/s mode can we detect that and restart the SDR at a safe
// rate"; REDESIGNED for beta95, 2026-11-25: "that latch is too aggressive").
//
// ★★★ TWO WAYS TO FALL OVER, TWO KINDS OF EVIDENCE.
//     STALLED — the process lives, the samples stop (no IQ for 3 s, the stream ends). The watchdog sees it, leaves the
//               mode (the radio back at its base rate) and reports it here: recordStall.
//     CRASHED — the process dies. Nothing runs after an abort, so the only evidence is a file that was ALREADY there:
//               <base>.marker, written immediately BEFORE the radio is asked for the boosted rate and removed once it
//               is back at its base rate or the process stops cleanly. Found at the next start: boot().
//
// ★★★ THE beta95 RULES (Stuart, 2026-11-25 — the Sony's adb install of Lite 121 read as "SDR crashed at 2.56 MS/s"):
//   (a) AN UPDATE OR A DELIBERATE STOP IS NOT A CRASH. The marker carries the build that wrote it (setBuild: the
//       version AND the compile stamp, so a rebuilt package of the same version counts too). A marker from another
//       build — or from a build before beta95, which wrote none — means the binary was replaced under the running
//       server (adb/Play install, apt upgrade): forgotten, not counted. A clean stop takes the marker up (unmark).
//   (b) ONE UNEXPLAINED DEATH = NOTED, the same rate allowed again. Only a SECOND death at that rate within 24 h latches.
//   (c) THE LATCH IS ONE RUNG DOWN, never further than it must: the boosts are capped BELOW the rate it died at
//       (2.56 → 2.4), and a death above the ADS-B floor (2.4 MS/s) never caps below the floor — ADS-B stays.
//   ★★★ (c') 2.4 MS/s IS THE FLOOR, FULL STOP (Stuart, 2026-11-26: "2.4msps is the safe rate dont want to limit further
//       as you will loose modes"). A latch NEVER caps below 2.4. A radio that stops twice AT 2.4 (or at DAB's 2.048) in
//       24 h is not capped at all: the owner is told (`floor`, say()) — it is the radio, its USB power, cable or hub,
//       not the rate — and every mode keeps its rate. (beta95 capped such a radio at 2.048, which took ADS-B away.)
//   (d) IT CLEARS ITSELF: 24 h after it latched, the higher rate is tried again ONCE (probation). Another death there
//       during the next 24 h latches again for good (until the owner clears it); 24 h clean on probation forgets it.
//   (e) THE OWNER SEES IT (say(), the setup page and the app's server screen) and can clear it at any time.
// ★★★ AND IT NEVER WRITES THE OWNER'S CONFIG. The guard is an OVERRIDE held here (allows()); the owner's switches
//     (dabRateBoost, adsb256, ism256, avdata+8m) keep what the owner set, so clearing — or the self-clear — gives back
//     exactly what they had. (Before beta95 the latch persisted dabRateBoost:false, and Clear left ADS-B missing.)
// ★ Pure, no radio — vibeserver/test-adsb-capture.cpp holds it to known answers. Empty base = memory only.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace vibe {

/** ★ The ADS-B floor: a latch above it never caps below it (rule c). */
static constexpr double kRateGuardAdsbFloorHz = 2.4e6;
/** ★ How long a death counts towards the second (rule b), how long a latch holds before the one retry (rule d), and
 *  how long that retry must run clean to be forgotten. */
static constexpr long long kRateGuardWindowSec = 24LL * 3600;

/** ★ (c') May a stop at `hz` step the boosts down at all? Only from above the 2.4 MS/s floor. */
inline bool rateGuardCanStep(double hz) { return hz > kRateGuardAdsbFloorHz + 1000.0; }
/** The rates a boost can land on, in order — the rung below a death is the highest of these under it, never below the
 *  2.4 floor; 0 for a stop at or below the floor, which never steps down (c'). */
inline double rateGuardRungBelow(double hz) {
    if (!rateGuardCanStep(hz)) return 0;
    static const double kRungs[] = {2.048e6, 2.4e6, 2.56e6, 2.88e6, 3.0e6, 3.2e6, 4.0e6, 5.0e6, 6.0e6, 8.0e6, 10.0e6};
    double best = 0;
    for (double r : kRungs) if (r < hz - 1000.0) best = r;
    if (best < kRateGuardAdsbFloorHz) best = kRateGuardAdsbFloorHz;
    return best;
}

/** One stop at a boosted rate. */
struct RateDeath {
    std::string kind;          ///< "stalled" | "crashed"
    long long   at = 0;        ///< epoch seconds
    double      rate = 0;      ///< Hz the radio was at (or going to)
    std::string mode;          ///< "ADS-B" | "DAB" | "ISM" | "AVDATA" | ""
};

/** What the guard knows — the clients read it as hwinfo.rateGuard / GET /vibeserver/rate-guard. */
struct RateFault {
    // ── the latest stop (shown while noted, latched or on probation) ──
    std::string kind;          ///< "" | "stalled" | "crashed"
    long long   at = 0;
    double      rate = 0;
    std::string mode;
    // ── the latch ──
    bool        latched = false;
    double      ceiling = 0;   ///< while latched: no boost above this (0 = no boost at all)
    long long   latchedAt = 0;
    bool        retried = false;     ///< the one self-clear retry has been spent (rule d)
    bool        probation = false;   ///< the latch lifted after 24 h: trying the higher rate again
    long long   probationAt = 0;
    int         noted = 0;           ///< stops in the last 24 h that have not latched (0 or 1)
    bool        legacy = false;      ///< loaded from a pre-beta95 record (see LocalSdrShim::setRateGuardBase)
    // ── (c') stopped twice in 24 h AT the floor (2.4) or below: told to the owner, never capped ──
    bool        floor = false;
    double      floorRate = 0;
    long long   floorAt = 0;
    std::string floorMode;

    bool any() const { return !kind.empty() || floor; }
    static std::string rateText(double hz) {
        if (hz <= 0) return "a boosted rate";
        char b[32]; std::snprintf(b, sizeof b, "%.3f", hz / 1e6);
        std::string s = b;
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s + " MS/s";
    }
    /** The line beside the rate switch. The clients add the local time from `at`. */
    std::string say() const {
        if (!any()) return "";
        const std::string where = rateText(rate) + (mode.empty() ? "" : " during " + mode);
        const std::string what = kind == "stalled" ? "SDR stopped responding at " : "SDR crashed at ";
        static const char* D = " \xe2\x80\x94 ";
        // ★ (c') the floor note: nothing is held back, the owner is told it is the radio
        const std::string floorWhy = floor
            ? "The SDR stopped twice in 24 hours at " + rateText(floorRate) + (floorMode.empty() ? "" : " during " + floorMode)
              + D + "kept at that rate, because a lower one would lose modes. A radio that stops at "
              + rateText(floorRate) + " usually needs a better USB power supply, cable or hub."
            : "";
        if (floor && !latched && !probation) return floorWhy;
        const std::string tail = floor ? " " + floorWhy : "";
        if (latched) {
            const std::string cap = ceiling >= 2.0e6
                ? "modes now raise the rate only to " + rateText(ceiling)
                  + (ceiling >= kRateGuardAdsbFloorHz - 1000.0 ? " (ADS-B still runs)" : "")
                : "no mode raises the rate (the radio stays at its own)";
            if (retried)
                return what + where + D + "again after the 24-hour retry, so " + cap + " until you clear this." + tail;
            return what + where + " twice in 24 hours" + D + cap + ". It tries " + rateText(rate)
                   + " again by itself after 24 hours, or clear this to try now." + tail;
        }
        if (probation)
            return "Trying " + rateText(rate) + " again after 24 hours at a lower rate (it stopped there twice before)." + tail;
        if (!rateGuardCanStep(rate))   // (c') at the floor nothing is ever held lower
            return what + where + D + "noted once; it will use that rate again (" + rateText(kRateGuardAdsbFloorHz)
                   + " is never held lower, or modes would be lost).";
        return what + where + D + "noted once; it will use that rate again, and only a second stop within 24 hours "
               "holds it a step lower.";
    }
    std::string json() const {
        char b[420];
        std::snprintf(b, sizeof b, "\",\"at\":%lld,\"rate\":%.0f,\"latched\":%s,\"ceiling\":%.0f,\"latchedAt\":%lld,"
                      "\"retried\":%s,\"probation\":%s,\"probationAt\":%lld,\"noted\":%d,\"retryAt\":%lld,"
                      "\"floor\":%s,\"floorRate\":%.0f,\"floorAt\":%lld,\"floorMode\":\"%s\",\"mode\":\"",
                      at, rate, latched ? "true" : "false", ceiling, latchedAt, retried ? "true" : "false",
                      probation ? "true" : "false", probationAt, noted,
                      latched && !retried ? latchedAt + kRateGuardWindowSec : 0LL,
                      floor ? "true" : "false", floorRate, floorAt, esc(floorMode).c_str());
        // ★ "blocked" = latched: what the pre-beta95 readers looked for (they no longer grey the switch on it)
        return "{\"v\":2,\"fault\":\"" + esc(kind) + b + esc(mode) + "\",\"blocked\":" + (latched ? "true" : "false")
             + ",\"say\":\"" + esc(say()) + "\"}";
    }
    static std::string esc(const std::string& in) {
        std::string o;
        for (char c : in) { if (c == '"' || c == '\\') o += '\\'; if (c == '\n' || c == '\r') continue; o += c; }
        return o;
    }
    static double num(const std::string& s, const char* k) {
        const std::string key = std::string("\"") + k + "\":";
        const size_t p = s.find(key);
        return p == std::string::npos ? 0 : std::strtod(s.c_str() + p + key.size(), nullptr);
    }
    static bool flag(const std::string& s, const char* k) {
        const std::string key = std::string("\"") + k + "\":true";
        return s.find(key) != std::string::npos;
    }
    static std::string str(const std::string& s, const char* k) {
        const std::string key = std::string("\"") + k + "\":\"";
        const size_t p = s.find(key);
        if (p == std::string::npos) return "";
        std::string o;
        for (size_t i = p + key.size(); i < s.size() && s[i] != '"'; ++i) { if (s[i] == '\\' && i + 1 < s.size()) ++i; o += s[i]; }
        return o;
    }
};

class RateGuard {
public:
    /** `build`: this binary's identity (version + compile stamp) — written into the marker, compared at boot (rule a). */
    void setBase(const std::string& base, const std::string& build = "") {
        std::lock_guard<std::mutex> lk(m_); base_ = base; if (!build.empty()) build_ = clean(build); loadLocked();
    }
    void setBuild(const std::string& build) { std::lock_guard<std::mutex> lk(m_); build_ = clean(build); }

    enum class Boot { None, Explained, Noted, Latched };
    /** At start, once per process: a marker still present means the last run stopped at the rate it names. Removes it,
     *  then: written by ANOTHER build (or none) = the binary was replaced, Explained; else a death, Noted or Latched. */
    Boot boot(long long nowEpoch, std::string* why = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        if (booted_ || base_.empty()) return Boot::None;
        booted_ = true;
        refreshLocked(nowEpoch);
        FILE* f = std::fopen((base_ + ".marker").c_str(), "rb");
        if (!f) return Boot::None;
        char b[320] = {0};
        const size_t n = std::fread(b, 1, sizeof b - 1, f);
        std::fclose(f);
        b[n] = 0;
        std::remove((base_ + ".marker").c_str());
        // "entering <hz> Hz for <mode> at <epoch> build <id>"
        RateDeath x; x.kind = "crashed";
        if (const char* p = std::strstr(b, "entering ")) x.rate = std::strtod(p + 9, nullptr);
        if (const char* p = std::strstr(b, " for ")) {
            const char* e = std::strstr(p + 5, " at ");
            if (!e) e = p + 5 + std::strcspn(p + 5, "\r\n");
            if (e - (p + 5) <= 16) x.mode.assign(p + 5, size_t(e - (p + 5)));
        }
        if (const char* p = std::strstr(b, " at ")) x.at = std::atoll(p + 4);
        if (x.at <= 0) x.at = nowEpoch;
        std::string mb;
        if (const char* p = std::strstr(b, " build ")) { mb.assign(p + 7); mb = clean(mb); }
        if (mb.empty() || (!build_.empty() && mb != build_)) {
            if (why) *why = mb.empty() ? "written by a build before beta95 (the server was updated)"
                                       : "written by build " + mb + " (the server was updated since)";
            return Boot::Explained;
        }
        return deathLocked(x, nowEpoch) ? Boot::Latched : Boot::Noted;
    }
    /** Immediately BEFORE asking the radio for `rateHz` on behalf of `mode`. */
    void mark(double rateHz, const std::string& mode, long long nowEpoch) {
        std::lock_guard<std::mutex> lk(m_);
        markRate_ = rateHz; markMode_ = mode; marked_ = true;
        if (base_.empty()) return;
        if (FILE* f = std::fopen((base_ + ".marker").c_str(), "wb")) {
            std::fprintf(f, "entering %.0f Hz for %s at %lld build %s\n", rateHz, mode.c_str(), nowEpoch,
                         build_.empty() ? "?" : build_.c_str());
            std::fflush(f);
            std::fclose(f);
        }
    }
    /** The radio has settled at `nowRateHz`: a rate other than the one marked means the boost is over. */
    void settled(double nowRateHz) {
        std::lock_guard<std::mutex> lk(m_);
        if (marked_ && std::fabs(nowRateHz - markRate_) > 1000) unmarkLocked();
    }
    /** A clean stop (or a deliberate one: the app's Stop, onDestroy, SIGTERM from systemd/apt). */
    void unmark() { std::lock_guard<std::mutex> lk(m_); unmarkLocked(); }
    bool boosted(double* rate = nullptr, std::string* mode = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        if (rate) *rate = markRate_;
        if (mode) *mode = markMode_;
        return marked_;
    }
    bool markerPresent() {
        std::lock_guard<std::mutex> lk(m_);
        if (base_.empty()) return false;
        FILE* f = std::fopen((base_ + ".marker").c_str(), "rb");
        if (f) std::fclose(f);
        return f != nullptr;
    }
    /** A stall seen live. Returns true when it latched. */
    bool recordStall(double rateHz, const std::string& mode, long long at) {
        std::lock_guard<std::mutex> lk(m_);
        RateDeath x; x.kind = "stalled"; x.rate = rateHz; x.mode = mode; x.at = at;
        refreshLocked(at);
        return deathLocked(x, at);
    }
    /** The owner's Clear: everything forgotten, the boosts as the owner's switches say. */
    void clear() { std::lock_guard<std::mutex> lk(m_); f_ = RateFault{}; deaths_.clear(); saveLocked(); }
    /** ★ THE OVERRIDE: may a mode boost the radio to `rateHz` now? */
    bool allows(double rateHz, long long nowEpoch) {
        std::lock_guard<std::mutex> lk(m_);
        refreshLocked(nowEpoch);
        return !f_.latched || rateHz <= f_.ceiling + 1000.0;
    }
    bool latched(long long nowEpoch) { std::lock_guard<std::mutex> lk(m_); refreshLocked(nowEpoch); return f_.latched; }
    RateFault fault() { std::lock_guard<std::mutex> lk(m_); return f_; }
    RateFault fault(long long nowEpoch) { std::lock_guard<std::mutex> lk(m_); refreshLocked(nowEpoch); return f_; }
    /** ★ With this binary's `build` (the marker's stamp, rule a) — the end-to-end test plants markers with it. */
    std::string json(long long nowEpoch) {
        std::lock_guard<std::mutex> lk(m_); refreshLocked(nowEpoch);
        std::string j = f_.json();
        j.insert(j.size() - 1, ",\"build\":\"" + RateFault::esc(build_) + "\"");
        return j;
    }
    /** ★ A pre-beta95 record was found at setBase (it had switched the owner's setting off): true ONCE. */
    bool takeLegacy(std::string* mode = nullptr) {
        std::lock_guard<std::mutex> lk(m_);
        if (!legacy_) return false;
        legacy_ = false;
        if (mode) *mode = legacyMode_;
        return true;
    }

private:
    static std::string clean(const std::string& s) {
        std::string o;
        for (char c : s) if ((unsigned char)c > 0x20 && c != '"' && c != '\\' && o.size() < 64) o += c;
        return o;
    }
    /** Time moves the latch on (rule d). A clock that went backwards moves nothing. */
    void refreshLocked(long long now) {
        bool changed = false;
        if (f_.latched && !f_.retried && now >= f_.latchedAt + kRateGuardWindowSec) {
            f_.latched = false; f_.probation = true; f_.probationAt = now; f_.retried = true; changed = true;
        }
        if (f_.probation && now >= f_.probationAt + kRateGuardWindowSec) {
            RateFault keep; keep.floor = f_.floor; keep.floorRate = f_.floorRate; keep.floorAt = f_.floorAt; keep.floorMode = f_.floorMode;
            f_ = keep; deaths_.clear(); changed = true;
        }
        if (f_.floor && now >= f_.floorAt + kRateGuardWindowSec) {
            f_.floor = false; f_.floorRate = 0; f_.floorAt = 0; f_.floorMode.clear(); changed = true;
        }
        // stops that have aged out of the window are forgotten
        const size_t before = deaths_.size();
        for (size_t i = 0; i < deaths_.size();)
            if (now >= deaths_[i].at + kRateGuardWindowSec) deaths_.erase(deaths_.begin() + long(i)); else ++i;
        if (deaths_.size() != before) changed = true;
        if (!f_.latched && !f_.probation) {
            const int n = int(deaths_.size());
            if (n != f_.noted) { f_.noted = n; changed = true; }
            if (n == 0 && f_.any()) { f_.kind.clear(); f_.mode.clear(); f_.at = 0; f_.rate = 0; changed = true; }
        }
        if (changed) saveLocked();
    }
    /** One stop at a boosted rate. Returns true when it latched (rule b, or a failed retry — rule d). */
    bool deathLocked(const RateDeath& x, long long now) {
        if (!rateGuardCanStep(x.rate)) return floorDeathLocked(x, now);
        f_.kind = x.kind; f_.at = x.at; f_.rate = x.rate; f_.mode = x.mode;
        if (f_.mode.size() > 16) f_.mode.resize(16);
        bool latch = false;
        if (f_.latched) {
            // already held: a stop at the capped rate itself steps down once more (never below the floor — (c'))
            if (x.rate <= f_.ceiling + 1000.0) { f_.ceiling = rateGuardRungBelow(x.rate); f_.latchedAt = now; }
            saveLocked();
            return true;
        }
        if (f_.probation) {
            latch = true;                 // ★ the one retry failed: held until the owner clears it
            f_.probation = false; f_.retried = true;
        } else {
            int same = 1;
            for (const auto& d : deaths_) if (std::fabs(d.rate - x.rate) <= 1000.0 && x.at - d.at < kRateGuardWindowSec) ++same;
            latch = same >= 2;
        }
        if (latch) {
            f_.latched = true; f_.ceiling = rateGuardRungBelow(x.rate); f_.latchedAt = now; f_.noted = 0;
            deaths_.clear();
        } else {
            deaths_.push_back(x);
            while (deaths_.size() > 4) deaths_.erase(deaths_.begin());
            f_.noted = int(deaths_.size());
        }
        saveLocked();
        return latch;
    }
    /** ★★★ (c') A stop AT the 2.4 floor or below (ADS-B at 2.4, DAB at 2.048): never a latch, never a cap. The first is
     *  noted like any other; a second at that rate within 24 h raises the owner's note (`floor`) and nothing else.
     *  While a latch or a probation stands, its own line stays and the floor note is added to it. Returns false. */
    bool floorDeathLocked(const RateDeath& x, long long now) {
        int same = 1;
        for (const auto& d : deaths_) if (std::fabs(d.rate - x.rate) <= 1000.0 && x.at - d.at < kRateGuardWindowSec) ++same;
        deaths_.push_back(x);
        while (deaths_.size() > 4) deaths_.erase(deaths_.begin());
        if (!f_.latched && !f_.probation) {
            f_.kind = x.kind; f_.at = x.at; f_.rate = x.rate; f_.mode = x.mode;
            if (f_.mode.size() > 16) f_.mode.resize(16);
            f_.noted = int(deaths_.size());
        }
        if (same >= 2) {
            f_.floor = true; f_.floorRate = x.rate; f_.floorAt = now; f_.floorMode = x.mode.substr(0, 16);
        }
        saveLocked();
        return false;
    }
    void unmarkLocked() {
        if (!marked_) return;            // ★ cheap on every rate change that was never a boost
        marked_ = false; markRate_ = 0; markMode_.clear();
        if (!base_.empty()) std::remove((base_ + ".marker").c_str());
    }
    void loadLocked() {
        f_ = RateFault{}; deaths_.clear();
        if (base_.empty()) return;
        FILE* f = std::fopen((base_ + ".json").c_str(), "rb");
        if (!f) return;
        char b[2048]; const size_t n = std::fread(b, 1, sizeof b - 1, f); b[n] = 0;
        std::fclose(f);
        const std::string s = b;
        if (s.find("\"fault\":") == std::string::npos) return;
        f_.kind = RateFault::str(s, "fault"); f_.mode = RateFault::str(s, "mode");
        if (!f_.kind.empty() && f_.kind != "stalled" && f_.kind != "crashed") f_.kind = "crashed";
        if (f_.mode.size() > 16) f_.mode.resize(16);
        f_.at = (long long)RateFault::num(s, "at"); f_.rate = RateFault::num(s, "rate");
        if (s.find("\"v\":2") == std::string::npos) {
            /* ★★ A PRE-beta95 RECORD: it latched on ONE stop (perhaps an update, as on the Sony) and switched the owner's
             *  setting off. Under the new rules one stop is a note — kept as one, and the caller is told once so it can
             *  give the owner's switch back (takeLegacy). */
            if (f_.any()) {
                legacy_ = true; legacyMode_ = f_.mode;
                RateDeath d; d.kind = f_.kind; d.at = f_.at; d.rate = f_.rate; d.mode = f_.mode;
                deaths_.push_back(d); f_.noted = 1;
                saveLocked();
            }
            return;
        }
        f_.latched = RateFault::flag(s, "latched");
        f_.ceiling = RateFault::num(s, "ceiling");
        f_.latchedAt = (long long)RateFault::num(s, "latchedAt");
        f_.retried = RateFault::flag(s, "retried");
        f_.probation = RateFault::flag(s, "probation");
        f_.probationAt = (long long)RateFault::num(s, "probationAt");
        f_.floor = RateFault::flag(s, "floor");
        f_.floorRate = RateFault::num(s, "floorRate");
        f_.floorAt = (long long)RateFault::num(s, "floorAt");
        f_.floorMode = RateFault::str(s, "floorMode").substr(0, 16);
        // the noted stops ("d<i>":"<kind>|<at>|<rate>|<mode>")
        for (int i = 0; i < 4; ++i) {
            const std::string k = "d" + std::to_string(i);
            const std::string v = RateFault::str(s, k.c_str());
            if (v.empty()) break;
            RateDeath d;
            const size_t a = v.find('|'), c = a == std::string::npos ? a : v.find('|', a + 1),
                         e = c == std::string::npos ? c : v.find('|', c + 1);
            if (e == std::string::npos) continue;
            d.kind = v.substr(0, a); d.at = std::atoll(v.c_str() + a + 1); d.rate = std::strtod(v.c_str() + c + 1, nullptr);
            d.mode = v.substr(e + 1, 16);
            deaths_.push_back(d);
        }
        f_.noted = f_.latched || f_.probation ? 0 : int(deaths_.size());
    }
    void saveLocked() {
        if (base_.empty()) return;
        std::string j = f_.json();
        std::string extra;
        for (size_t i = 0; i < deaths_.size() && i < 4; ++i) {
            char b[160];
            std::snprintf(b, sizeof b, ",\"d%zu\":\"%s|%lld|%.0f|%s\"", i, RateFault::esc(deaths_[i].kind).c_str(),
                          deaths_[i].at, deaths_[i].rate, RateFault::esc(deaths_[i].mode).c_str());
            extra += b;
        }
        j.insert(j.size() - 1, extra);
        const std::string path = base_ + ".json", tmp = path + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            const bool ok = std::fwrite(j.data(), 1, j.size(), f) == j.size();
            std::fclose(f);
            if (ok) std::rename(tmp.c_str(), path.c_str()); else std::remove(tmp.c_str());
        }
    }
    std::mutex m_;
    std::string base_, build_;
    RateFault f_;
    std::vector<RateDeath> deaths_;
    bool booted_ = false, marked_ = false, legacy_ = false;
    std::string legacyMode_;
    double markRate_ = 0;
    std::string markMode_;
};

} // namespace vibe
