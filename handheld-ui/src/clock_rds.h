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
//   station. Time stations and FT8 are the precise sources (~0.2 s); FT8 auto-sets too (clockAutoFt8).
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
 *    FT8 (clockAutoFt8 below)                     0.2 s
 *    RDS clock time                               1.5 s  (sent once a minute, ~a second of decode path)
 *    DAB multiplex time                           4 s    (★ Stuart's slow-mo video: a multiplex's own clock 1.3 s BEHIND
 *                                                         his Mac — a broadcast's chain delay, not ours: DAB can put a
 *                                                         clock that is minutes out right, but must never drag a good one
 *                                                         slow)
 *  and a weaker kind never overrules a stronger one set in the last 6 hours (DAB does not undo MSF or FT8).
 *  Corroboration (two agreeing boundaries) is unchanged, so one wrong transmitter minute cannot set it. OFF (MENU ›
 *  HARDWARE › CLOCK AUTO-SET) puts it back to the keys alone. */
struct ClockAutoRank { int rank = 0; double at = -1e18; };
inline ClockAutoRank& clockAutoRank() { static ClockAutoRank r; return r; }
inline bool clockAutoMay(int rank) {
    ClockAutoRank& r = clockAutoRank();
    if (rank < r.rank && hwNow() - r.at < 6 * 3600) return false;
    r.rank = rank; r.at = hwNow();
    return true;
}

/* ★★★ FT8 SETS THE CLOCK TOO (Stuart, 2026-11-08: "auto setting via FT8 would be good … the clock will adjust after 3
 *  cycles and until that point decodes may decode poorly. Then when the clock has set a system clock synchronised to
 *  FT8 signals"). DAB leaves the clock ~1.3 s slow, and FT8's decoder looks only ~1.6 s early — no margin left for a
 *  station whose own clock is fast. FT8's DT is the clock error AS FT8 SEES IT, so it is the right judge for FT8.
 *  ★ THREE CYCLES: the last three complete 15 s cycles, each with ≥ 2 decodes, whose medians agree within 0.3 s (one
 *    station with a bad clock, or one odd cycle, cannot move it). The clock moves by minus the median of all their DTs
 *    — a positive DT = signals late against our clock = our clock is FAST.
 *  ★ Only when the median is 0.2 s or more out (DT is sent to 0.1 s); under that the clock is called synchronised.
 *  ★ Ranks with a time station: both are good to ~0.2 s, and the limits keep them from undoing each other.
 *  ★ After a set every DT so far, and the cycle in flight, was measured against the old clock: all forgotten, and the
 *    next 25 s ignored (the rest of the slot being cut, plus its decode). */
inline void clockAutoFt8(App& a) {
    HwFt8Clock& k = hwFt8Clock();
    const long long cur = hwUtcMsNow() / 15000;
    while (!k.cycles.empty() && cur - k.cycles.front().slot > 8) k.cycles.pop_front();   // 2 minutes
    std::vector<const HwFt8Clock::Cycle*> done;
    for (auto& cy : k.cycles) if (cy.slot < cur && cy.dts.size() >= 2) done.push_back(&cy);
    if (done.empty()) { k.agreeing = 0; return; }
    if (done.back()->slot == k.evalSlot) return;                       // judged already — wait for the next cycle
    k.evalSlot = done.back()->slot;
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        const size_t n = v.size();
        return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    };
    // how many of the newest cycles agree with the newest (the box's progress)
    const size_t from = done.size() > 3 ? done.size() - 3 : 0;
    std::vector<double> meds, all;
    for (size_t i = from; i < done.size(); ++i) { meds.push_back(median(done[i]->dts)); all.insert(all.end(), done[i]->dts.begin(), done[i]->dts.end()); }
    int agree = 1;
    for (size_t i = meds.size() - 1; i-- > 0;) { if (std::fabs(meds[i] - meds.back()) <= 0.3) ++agree; else break; }
    k.agreeing = agree;
    if (meds.size() < 3) return;
    if (*std::max_element(meds.begin(), meds.end()) - *std::min_element(meds.begin(), meds.end()) > 0.3) return;
    const double m = median(all);
    if (std::fabs(m) < 0.2) { k.syncedAt = hwNow(); return; }
    if (!a.set_.clockAuto || !a.eng_ || !clockAutoMay(3)) return;
    const long long delta = std::llround(-m * 1000);
    std::fprintf(stderr, "vibe-hhui: clock auto-set from FT8, %+lld ms (%zu decodes, cycle medians %+.2f %+.2f %+.2f)\n",
                 delta, all.size(), meds[0], meds[1], meds[2]);
    hwSetClock(a.eng_->port(), "{\"deltaMs\":" + std::to_string(delta) + ",\"source\":\"auto · FT8 (" + std::to_string(all.size()) + " decodes)\"}",
               "to FT8 (" + std::to_string(all.size()) + " decodes)");
    hwFt8ClockForget(25);
    k.syncedAt = k.setAt = hwNow(); k.setMs = (double)delta;
}

inline void clockAutoTick(App& a) {
    clockAutoFt8(a);
    static double seenAt = 0;
    const HwTimeSync& ls = hwLastSync();
    if (ls.station.empty() || ls.at == seenAt) return;
    seenAt = ls.at;
    if (!a.set_.clockAuto || !a.eng_) return;
    const bool rds = ls.station.rfind("RDS", 0) == 0, dab = ls.station.rfind("DAB", 0) == 0;
    const int rank = dab ? 1 : rds ? 2 : 3;
    const double limit = dab ? 4000 : rds ? 1500 : 200;
    const double err = ls.utcMs + ls.rxDelayMs - ls.hostMs;   // the engine clock's error at the boundary — still it now
    if (std::fabs(err) < limit) return;
    if (!clockAutoMay(rank)) return;
    std::fprintf(stderr, "vibe-hhui: clock auto-set from %s, %+.0f ms\n", ls.station.c_str(), err);
    hwSetClock(a.eng_->port(), "{\"deltaMs\":" + std::to_string(std::llround(err)) + ",\"source\":" + jsonQuote("auto · " + ls.station) + "}",
               "from " + ls.station);
    hwFt8ClockForget(25);
}

}  // namespace hh
