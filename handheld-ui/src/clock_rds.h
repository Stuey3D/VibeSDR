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
// ★★ AND CLOCK AUTO-SET (Stuart, 2026-11-08) sets it without the key: see clockAutoTick at the foot.
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
    // ── DAB FIG 0/10: mjd + utc "HH:MM:SS" (+ the long form's milliseconds) of the multiplex ──
    // ★★ THE ENGINE'S OWN STAMP (2026-11-08, ~3 s slow from DAB): timeChangedAtMs is this machine's clock at the moment
    //    the DECODER saw the value change. The report reaches us only every 0.5 s, so our own frame time was up to half a
    //    second late — and every late millisecond sets the clock that much slow. Without the stamp (an older engine), the
    //    frame time as before.
    if (dabActive()) {
        const Json* rep = dabReport(s);
        if (rep && (*rep).has("mjd") && !(*rep)["utc"].str().empty()) {
            int hh = 0, mm = 0, ss = 0;
            if (std::sscanf((*rep)["utc"].str().c_str(), "%d:%d:%d", &hh, &mm, &ss) >= 2) {
                const double mjd = (*rep)["mjd"].num(0);
                const double milli = (*rep)["utcMilli"].num(0);
                const double utc = (mjd - 40587.0) * 86400000.0 + ((hh * 60.0 + mm) * 60.0 + ss) * 1000.0 + milli;
                const double stamp = (*rep)["timeChangedAtMs"].num(0);
                const bool stamped = stamp > 0;
                const std::string key = stamped ? std::to_string((long long)stamp)
                                      : ((*rep)["mjd"].str().empty() ? std::to_string((long long)mjd) + " " + (*rep)["utc"].str()
                                                                     : (*rep)["mjd"].str() + " " + (*rep)["utc"].str());
                const std::string ens = (*rep)["ensemble"].str().empty() ? (*rep)["label"].str() : (*rep)["ensemble"].str();
                if (mjd > 50000 && clockStep(clockDab(), key, utc, stamped ? stamp : host, 2000, ens))
                    hwNoteTimeSync("DAB · " + (ens.empty() ? std::string("multiplex") : ens), utc, 0, stamped ? stamp : host);
            }
        }
    } else clockDab() = ClockSrc{};
}

/* ★★★ CLOCK AUTO-SET (Stuart, 2026-11-08: "there should be an option for the clock to autoset from any compatible source
 *  RDS/DAB then a user doesnt have to press an option button"). Every corroborated minute hwNoteTimeSync records is a
 *  candidate; it sets the clock only when the clock is further out than that kind of source can be trusted to:
 *    a time station (MSF, DCF77, WWV, WWVB, JJY)  0.2 s
 *    RDS clock time                               1.5 s  (sent once a minute, ~a second of decode path)
 *    DAB multiplex time                           4 s    (★ Stuart measured a multiplex's own clock 2 s BEHIND his Mac —
 *                                                         a broadcast's chain delay, not ours: DAB can put a clock that
 *                                                         is minutes out right, but must never drag a good one 2 s slow)
 *  and a weaker kind never overrules a stronger one set in the last 6 hours (DAB does not undo MSF). FT8 stays a key:
 *  its DT is a spread of other people's clocks. Corroboration (two agreeing boundaries) is unchanged, so one wrong
 *  transmitter minute cannot set it. OFF (MENU › HARDWARE › CLOCK AUTO-SET) puts it back to the keys alone. */
inline void clockAutoTick(App& a) {
    static double seenAt = 0, lastAt = -1e18;
    static int lastRank = 0;
    const HwTimeSync& ls = hwLastSync();
    if (ls.station.empty() || ls.at == seenAt) return;
    seenAt = ls.at;
    if (!a.set_.clockAuto || !a.eng_) return;
    const bool rds = ls.station.rfind("RDS", 0) == 0, dab = ls.station.rfind("DAB", 0) == 0;
    const int rank = dab ? 1 : rds ? 2 : 3;
    const double limit = dab ? 4000 : rds ? 1500 : 200;
    const double err = ls.utcMs + ls.rxDelayMs - ls.hostMs;   // the engine clock's error at the boundary — still it now
    if (std::fabs(err) < limit) return;
    if (rank < lastRank && hwNow() - lastAt < 6 * 3600) return;
    lastRank = rank; lastAt = hwNow();
    std::fprintf(stderr, "vibe-hhui: clock auto-set from %s, %+.0f ms\n", ls.station.c_str(), err);
    hwSetClock(a.eng_->port(), "{\"deltaMs\":" + std::to_string(std::llround(err)) + ",\"source\":" + jsonQuote("auto · " + ls.station) + "}",
               "from " + ls.station);
}

}  // namespace hh
