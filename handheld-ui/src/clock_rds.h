// clock_rds.h — SET CLOCK FROM RDS / DAB (Stuart, 2026-11-07: "yes build it in please" — for radios that cannot hear
// a time station, a strong FM station's RDS clock time or a DAB multiplex's date and time).
//
// ★★ THE BOUNDARY, NOT THE VALUE. Both carry a time that holds still between updates — RDS 4A says the minute that just
//    began (sent once, at the start of each minute; the engine keeps the last one), DAB FIG 0/10 the ensemble's time
//    (the engine keeps the last one). So the moment that counts is when the value CHANGES: that instant is the
//    boundary the new value names. The first value seen after tuning is stale by an unknown amount and is never used.
// ★★ CORROBORATED, as the web's time-station rule: an offer only after TWO consecutive changes that agree with each
//    other and with the elapsed time here (RDS: the next minute ~60 s later; DAB: the step in its clock matches ours to
//    within 2 s). Some transmitters send a wrong time — this is a button the person presses, never automatic.
// ★ Precision ~ a second (the report rate and the decode path): far better than a drifting clock, worse than a time
//   station — which is why time stations and FT8 keep their own keys.
// Feeds hwNoteTimeSync (hw_state.h) — the R36S HARDWARE clock rows draw SET CLOCK FROM <source> from it.
#pragma once
#include <chrono>

namespace hh {

struct ClockSrc {
    std::string lastKey;          // the value last seen (RDS: "ct"; DAB: "mjd hh:mm:ss")
    double lastUtcMs = 0, lastHostMs = 0;
    bool haveChange = false;      // a change has been seen (the boundary of lastUtcMs is known)
    std::string where;            // the station / multiplex it came from (a retune starts again)
};
inline ClockSrc& clockRds() { static ClockSrc c; return c; }
inline ClockSrc& clockDab() { static ClockSrc c; return c; }

inline double wallMs() {
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

/** A new value at `hostMs`: true when it is the second agreeing boundary in a row. */
inline bool clockStep(ClockSrc& c, const std::string& key, double utcMs, double hostMs, double toleranceMs, const std::string& where) {
    if (where != c.where) { c = ClockSrc{}; c.where = where; c.lastKey = key; return false; }   // a new source: start again
    if (key == c.lastKey) return false;
    const bool first = c.lastKey.empty();
    c.lastKey = key;
    if (first) return false;                                   // the value we tuned into: stale by an unknown amount
    bool agree = false;
    if (c.haveChange) {
        const double dUtc = utcMs - c.lastUtcMs, dHost = hostMs - c.lastHostMs;
        agree = dUtc > 0 && std::fabs(dUtc - dHost) <= toleranceMs;
    }
    c.lastUtcMs = utcMs; c.lastHostMs = hostMs; c.haveChange = true;
    return agree;
}

/** Once a frame: watch the RDS and DAB clocks; offer the clock when one has corroborated itself. */
inline void clockTick(App& a, const RadioState& s) {
    (void)a;
    const double host = wallMs();
    // ── RDS 4A (FM): ct = UTC hour × 60 + minute; the date is ours (the minute nearest now) ──
    const Json& r = s.msg("rds");
    if (s.mode == "wfm" && r.has("ct") && r["ct"].num(-1) >= 0) {
        const int ct = (int)r["ct"].num(-1);
        const double dayMs = std::floor(host / 86400000.0) * 86400000.0;
        double utc = dayMs + ct * 60000.0;
        if (utc - host > 43200000.0) utc -= 86400000.0;        // across midnight: the day nearest now
        if (host - utc > 43200000.0) utc += 86400000.0;
        const std::string ps = s.rdsPs.empty() ? std::string("FM") : s.rdsPs;
        char where[64]; std::snprintf(where, sizeof where, "%.0f", s.vfoHz);
        if (clockStep(clockRds(), std::to_string(ct), utc, host, 3000, where)) {
            std::string name = ps;
            while (!name.empty() && name.back() == ' ') name.pop_back();
            hwNoteTimeSync("RDS · " + name, utc, 0, host);
        }
    }
    // ── DAB FIG 0/10: mjd + utc "HH:MM:SS" of the multiplex ──
    if (dabActive()) {
        const Json* rep = dabReport(s);
        if (rep && (*rep).has("mjd") && !(*rep)["utc"].str().empty()) {
            int hh = 0, mm = 0, ss = 0;
            if (std::sscanf((*rep)["utc"].str().c_str(), "%d:%d:%d", &hh, &mm, &ss) >= 2) {
                const double mjd = (*rep)["mjd"].num(0);
                const double utc = (mjd - 40587.0) * 86400000.0 + ((hh * 60.0 + mm) * 60.0 + ss) * 1000.0;
                const std::string key = (*rep)["mjd"].str().empty() ? std::to_string((long long)mjd) + " " + (*rep)["utc"].str()
                                                                     : (*rep)["mjd"].str() + " " + (*rep)["utc"].str();
                const std::string ens = (*rep)["ensemble"].str().empty() ? (*rep)["label"].str() : (*rep)["ensemble"].str();
                if (mjd > 50000 && clockStep(clockDab(), key, utc, host, 2000, ens))
                    hwNoteTimeSync("DAB · " + (ens.empty() ? std::string("multiplex") : ens), utc, 0, host);
            }
        }
    } else clockDab() = ClockSrc{};
}

}  // namespace hh
