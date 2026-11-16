// vibe_dab_own.h — V12 DAB OWN STATION: who hears which station, and whether the feature is on (docs/v12/DAB-OWN-STATION.md).
//
// ★★★ STUART'S FINAL RULE (2026-11-15). The benchmark measures N, the stations this box decodes at once.
//     N >= the radio's user limit → each listener may pick their own station inside the multiplex: no slots, no asking,
//     no eviction (stations in use can never exceed listeners, and listeners never exceed the limit).
//     N <  the limit → OFF: one station for everyone, exactly as before. The setup page says how to turn it on (lower the
//     limit to N); the owner chooses, and can always force it on or off (-1 rule / 0 off / 1 on).
// ★★ PURE: no sockets, no threads, no DabService — the shim does what Roster says (open / close a station, retune the
//    radio's own). test-dab-own holds every rule here.
// ★ A listener is a SESSION (user_session_id): their spectrum and audio sockets go together.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace vibedab {
namespace own {

/** The figure the rule needs from the benchmark: N = how many stations at once.
 *  @param perStationPct the dec_dab_station row (one more station, % of a core; its hottest thread + its FAAD2)
 *  @param dabTotalPct   the dab row's threads summed (front end + the radio's own station)
 *  ★ 70 % of every core is the green budget; one core's worth stays for network, spectrum and the audio clock. One core
 *    → 1. Capped at 16 (vibedab::DabService::kMaxStations — memory floor; past that the uplink decides anyway).
 *  @return 0 = not measured (the row did not run): the rule then reads OFF. */
inline int nFromBench(double perStationPct, double dabTotalPct, int cores) {
    if (!(perStationPct > 0) || !(dabTotalPct > 0) || cores < 1) return 0;
    const double spare = 70.0 * cores - dabTotalPct - 70.0;
    const int extra = spare > 0 ? int(std::floor(spare / perStationPct)) : 0;
    return std::min(16, 1 + extra);
}

/** On or off: the owner's `setting` (-1 the rule, 0 off, 1 on), the measured N (0 = not measured) and the user limit. */
inline bool effectiveOn(int setting, int n, int limit) {
    if (setting == 0) return false;
    if (setting == 1) return true;
    return n > 0 && n >= std::max(1, limit);
}

/** ★ How many stations may be open at once (besides the radio's own): the rule's N − 1, or — forced on above N — the
 *  user limit, since a station per listener is the most there can ever be. Never above the hard cap. */
inline int stationCap(int setting, int n, int limit) {
    if (!effectiveOn(setting, n, limit)) return 0;
    const int most = std::max(n, setting == 1 ? limit : 0);
    return std::max(0, std::min(16, most) - 1);
}

/** The setup page's sentence under the switch (the page draws the same words — ONE RULE, TWO READERS: vibe_setup_page.h
 *  dabOwnText mirrors this, test-dab-own holds both to these strings). */
inline std::string setupText(int setting, int n, int limit) {
    char b[320];
    if (n <= 0) {
        std::snprintf(b, sizeof b, "Not measured yet: run the benchmark to see how many listeners this box can give their own station.");
        return b;
    }
    const bool on = effectiveOn(setting, n, limit);
    if (setting == -1 && on)
        std::snprintf(b, sizeof b, "On: this box can give up to %d listeners their own DAB station, and the listener limit is %d.", n, limit);
    else if (setting == -1)
        std::snprintf(b, sizeof b, "Off: this box can give up to %d listeners their own DAB station. Lower the listener limit to %d to turn it on.", n, n);
    else if (setting == 1 && n < limit)
        std::snprintf(b, sizeof b, "On by your choice: this box measured up to %d stations at once, below the listener limit of %d. Watch the health chip.", n, limit);
    else if (setting == 1)
        std::snprintf(b, sizeof b, "On by your choice: this box can give up to %d listeners their own DAB station.", n);
    else
        std::snprintf(b, sizeof b, "Off by your choice: everyone hears the same station. This box could give up to %d listeners their own.", n);
    return b;
}

/** What the shim must do for a listener's pick. */
struct Action {
    enum Kind { None, JoinPrimary, JoinStation, RetunePrimary, OpenStation, SwitchAll } kind = None;
    uint32_t sid = 0;          ///< the service to join / open / retune to
    uint32_t closeSid = 0;     ///< a station the pick emptied (close it first), 0 = none
};

/** ★ Who is on which station. primary = the radio's own DabService (g_dab); every other sid is a station. */
class Roster {
public:
    /** Listener `sess` picks `sid`; the radio's own plays `primarySid`; `cap` = stations that may be open (stationCap).
     *  Rules A–E (docs §3): A the primary's → join it; B a playing station → join; C nobody else on the primary → the
     *  primary retunes (a lone listener never opens a station — the old path exactly); D open one; E none left → switch
     *  everyone, as before the feature. The roster is updated to what the shim will make true. */
    Action pick(const std::string& sess, uint32_t sid, uint32_t primarySid, int cap) {
        Action a; a.sid = sid;
        want_[sess] = sid;
        pending_.erase(sess);
        const uint32_t mine = on_.count(sess) ? on_[sess] : 0;   // 0 = on the primary
        if (sid == 0) return a;
        if (mine == sid) { a.kind = Action::JoinStation; return a; }
        if (sid == primarySid) { a.closeSid = leaveStation_(sess); on_.erase(sess); a.kind = Action::JoinPrimary; return a; }
        if (listenersOn_(sid) > 0) { a.closeSid = leaveStation_(sess); on_[sess] = sid; a.kind = Action::JoinStation; return a; }
        // ★ X's old station is released before anything is counted: a listener alone on a station frees it here.
        const uint32_t freed = leaveStation_(sess);
        on_.erase(sess);
        a.closeSid = freed;
        if (primaryCount_(sess) == 0) {
            a.kind = Action::RetunePrimary;
            return a;
        }
        if (openStations_() < cap) { on_[sess] = sid; a.kind = Action::OpenStation; return a; }
        // ★ E: no station can be opened. Everyone on the primary follows (whole-radio, as before); station listeners keep theirs.
        a.kind = Action::SwitchAll;
        return a;
    }
    /** The listener's sockets are all gone. Returns a station that emptied (close it), or 0. Their choice is remembered
     *  for `graceSecs` (a reconnect re-adopts it — see adopt). */
    uint32_t leave(const std::string& sess, double now) {
        const uint32_t c = leaveStation_(sess);
        const auto w = want_.find(sess);
        if (w != want_.end() && w->second) gone_[sess] = { w->second, now };
        on_.erase(sess); want_.erase(sess); pending_.erase(sess); present_.erase(sess);
        return c;
    }
    /** A session's sockets arrived (or re-arrived). Returns the sid it should resume (its station within the grace
     *  period), or 0 — the shim then calls pick() for it, which is the server's move, not the client's re-assert. */
    uint32_t arrive(const std::string& sess, double now, double graceSecs = 120.0) {
        present_[sess] = true;
        auto g = gone_.find(sess);
        if (g == gone_.end()) return 0;
        const uint32_t sid = now - g->second.at <= graceSecs ? g->second.sid : 0;
        gone_.erase(g);
        return sid;
    }
    bool present(const std::string& sess) const { return present_.count(sess) != 0; }
    void forget(double now, double graceSecs = 120.0) {
        for (auto it = gone_.begin(); it != gone_.end();) it = now - it->second.at > graceSecs ? gone_.erase(it) : std::next(it);
    }
    /** ★ The multiplex changed (whole-radio): every station is gone, everyone hears the primary. Each listener who had their
     *  own station is PENDING: when the new list arrives, if their SId is on it, they get it back (rule 1, per listener). */
    void blockChanged() {
        for (const auto& kv : on_) if (kv.second) pending_[kv.first] = kv.second;
        on_.clear();
    }
    /** The pending listeners and the sid each wants back. The shim calls pick() for each whose sid is on the new ensemble,
     *  and drop() for those whose sid is not there once the list is complete. */
    const std::map<std::string, uint32_t>& pending() const { return pending_; }
    void dropPending(const std::string& sess) { pending_.erase(sess); }
    /** Everyone back on the primary, no memory (the feature switched off, DAB's end). */
    void clear() { on_.clear(); pending_.clear(); }
    /** The station `sess` hears — 0 = the radio's own. */
    uint32_t stationOf(const std::string& sess) const { auto it = on_.find(sess); return it == on_.end() ? 0 : it->second; }
    /** Every station in use (not the primary). */
    std::vector<uint32_t> stations() const {
        std::vector<uint32_t> v;
        for (const auto& kv : on_) if (kv.second && std::find(v.begin(), v.end(), kv.second) == v.end()) v.push_back(kv.second);
        return v;
    }
    /** The `playing` list: {sid, listeners} for the primary (if anyone hears it) and every station. `present` = sessions
     *  with sockets now (the primary's count is everyone present not on a station). */
    std::vector<std::pair<uint32_t, int>> playing(uint32_t primarySid, const std::vector<std::string>& presentSessions) const {
        std::map<uint32_t, int> n;
        for (const auto& s : presentSessions) {
            const uint32_t st = stationOf(s);
            ++n[st ? st : primarySid];
        }
        std::vector<std::pair<uint32_t, int>> v;
        for (const auto& kv : n) if (kv.first) v.push_back(kv);
        return v;
    }
private:
    struct Gone { uint32_t sid; double at; };
    std::map<std::string, uint32_t> on_;       ///< sessions on a STATION (absent = the primary)
    std::map<std::string, uint32_t> want_;     ///< every listener's last pick
    std::map<std::string, uint32_t> pending_;  ///< after a block change: the station each wants back
    std::map<std::string, Gone> gone_;         ///< left within the grace period
    std::map<std::string, bool> present_;
    int listenersOn_(uint32_t sid) const { int n = 0; for (const auto& kv : on_) if (kv.second == sid) ++n; return n; }
    int openStations_() const { return int(stations().size()); }
    int primaryCount_(const std::string& except) const {
        int n = 0;
        for (const auto& kv : present_) if (kv.first != except && !on_.count(kv.first)) ++n;
        return n;
    }
    /** Take `sess` off its station; returns that station's sid if nobody is left on it. */
    uint32_t leaveStation_(const std::string& sess) {
        auto it = on_.find(sess);
        if (it == on_.end()) return 0;
        const uint32_t sid = it->second;
        on_.erase(it);
        return listenersOn_(sid) == 0 ? sid : 0;
    }
};

}  // namespace own
}  // namespace vibedab
