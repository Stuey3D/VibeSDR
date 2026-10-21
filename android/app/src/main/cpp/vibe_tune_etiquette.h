// vibe_tune_etiquette.h — "Ask before tuning": who may move a shared dial, and what an answer in the chat does.
//
// ★★★ WHY (Stuart, 2026-10-21, from the Discord group): owners of shared-tuner servers are fed up with people
//     joining and tuning without asking, despite the "SHARED TUNER · ASK TO TUNE" banner. This is the OPT-IN lock
//     that makes the ask happen. Design: docs/TUNING-ETIQUETTE.md.
//
// ★★★ WHY THIS IS ITS OWN HEADER — the same reason as vibe_session_turns.h. Inside the shim the rule could only be
//     exercised by three live browsers and a stopwatch; here it runs on a synthetic clock in test-tune-etiquette.cpp
//     in milliseconds. The shim builds a Room from what it knows (sessions, turn starts, the admin, the limit) and
//     asks; nothing in here touches a socket or a lock.
//
// ★★★ THE SERVER OWNS ALL OF IT (shared-dial contract): ranks, the lock, both timers. Clients are told the result
//     in the `dial` message and have no vote.
#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace vibeask {

/** ★ No answer in this long and the asker may tune. Stuart's figure. */
constexpr double kAskWindowS = 15.0;
/** ★★★ A "NO" HOLDS FOR AT MOST THIS LONG, then the asker may tune without asking again — so nobody can keep a
 *  shared server to themselves by saying no for ever. Five minutes is long enough to finish listening to
 *  something, short enough that a newcomer is not driven away. THE ONE NUMBER TO CHANGE. */
constexpr double kNoHoldS = 5.0 * 60.0;
/** ★ A grant outlives a reconnect: the same hold the user numbers use (the shim's handleHoldSec, 2 minutes). */
constexpr double kGoneForgetS = 120.0;

/** One session in the room, as the shim sees it. */
struct Person {
    std::string session;
    bool   admin = false;      ///< the signed-in admin — never locked, outranks everybody
    double since = 0;          ///< when their turn began (the session limit's own clock; lower = here longer)
    bool   overLimit = false;  ///< their session time limit has run out — drops to the BOTTOM of the order
    int    handle = 0;         ///< their user number, a stable tie-break
};

struct Room {
    bool inForce = false;          ///< switch on, shared dial, more than one listener
    std::vector<Person> people;    ///< everybody present, any order
};

enum class Reply { None, Yes, No };

/** ★★ THE ASK. "Can I tune?" and "Anyone using this?" — both already canned, so an OLD client can ask too. */
inline bool isAsk(const std::string& id) { return id == "ask_tune" || id == "anyone_using"; }

/** ★★ ANSWERS CARRY A MEANING BY ID — never parsed from text. */
inline Reply replyMeaning(const std::string& id) {
    if (id == "go_ahead" || id == "yes_go_ahead" || id == "all_yours" || id == "decode_done") return Reply::Yes;
    if (id == "please_hold" || id == "yes_hold" || id == "mid_decode" || id == "decoding_10min"
        || id == "listening_few_min") return Reply::No;
    return Reply::None;
}

/** ★★★ THE COMMAND STRUCTURE. Admin first; then longest-connected; a listener past their time limit drops to the
 *  bottom ("lucky to still be connected"), still longest-first among themselves. Sessions, highest rank first. */
inline std::vector<std::string> rankOrder(std::vector<Person> people) {
    std::stable_sort(people.begin(), people.end(), [](const Person& a, const Person& b) {
        const int ta = a.admin ? 0 : (a.overLimit ? 2 : 1), tb = b.admin ? 0 : (b.overLimit ? 2 : 1);
        if (ta != tb) return ta < tb;
        if (a.since != b.since) return a.since < b.since;
        if (a.handle != b.handle) return a.handle < b.handle;
        return a.session < b.session;
    });
    std::vector<std::string> out;
    out.reserve(people.size());
    for (auto& p : people) out.push_back(p.session);
    return out;
}

/** What one listener is told (the `dial` message's ask fields). */
struct View {
    bool inForce = false;   ///< askFirst
    bool locked = false;    ///< tuneLocked
    std::string state;      ///< "" | "open" (asked, waiting) | "no" (told no)
    int secs = 0;           ///< seconds left in `state`
    bool operator==(const View& o) const { return inForce == o.inForce && locked == o.locked && state == o.state; }
    bool operator!=(const View& o) const { return !(*this == o); }
};

class Book {
public:
    struct Req {
        bool   granted = false;
        bool   open = false;       ///< an ask inside its 15 s window
        double deadline = 0;       ///< when the window closes
        std::string bestBy;        ///< the highest-ranked replier so far (open window), or the NO's sayer
        Reply  best = Reply::None;
        double noUntil = 0;        ///< a standing NO ends here
        double lastSeen = 0;
    };
    std::map<std::string, Req> reqs;
    double askWindowS = kAskWindowS, noHoldS = kNoHoldS;

    /** Position in the order, 0 = highest. -1 = not here. */
    static int rankOf(const std::vector<std::string>& order, const std::string& s) {
        for (size_t i = 0; i < order.size(); i++) if (order[i] == s) return (int)i;
        return -1;
    }
    static const Person* find(const Room& r, const std::string& s) {
        for (auto& p : r.people) if (p.session == s) return &p;
        return nullptr;
    }

    /** ★★★ MAY THIS SESSION MOVE THE DIAL? The ONE answer every path gets. */
    bool locked(const Room& r, const std::string& s) const {
        if (!r.inForce) return false;
        const Person* p = find(r, s);
        if (p && p->admin) return false;                       // ★ admins are never locked
        auto it = reqs.find(s);
        if (it != reqs.end() && it->second.granted) return false;
        // ★ Nobody ranked above you is here: you are the one being protected, not the one who asks.
        const auto order = rankOrder(r.people);
        const int me = rankOf(order, s);
        if (me == 0) return false;
        return true;                                           // ★ unknown session (me == -1) is locked too
    }

    /** A chat line. @return true if anybody's view changed. */
    bool onSaid(const Room& r, const std::string& from, const std::string& id, double now) {
        if (!r.inForce || from.empty()) return false;
        if (isAsk(id)) return ask(r, from, now);
        const Reply m = replyMeaning(id);
        if (m == Reply::None) return false;
        return reply(r, from, m, now);
    }

    /** ★ An ask opens the 15 s window — only for somebody locked, and not while one is open or a NO stands
     *  (asking again does not restart the clock or buy a second "no"). */
    bool ask(const Room& r, const std::string& s, double now) {
        if (!locked(r, s)) return false;
        Req& q = reqs[s];
        q.lastSeen = now;
        if (q.open || q.noUntil > now) return false;
        q.open = true; q.deadline = now + askWindowS; q.best = Reply::None; q.bestBy.clear(); q.noUntil = 0;
        return true;
    }

    /** ★★★ A REPLY COUNTS ONLY FROM ABOVE THE ASKER, AND THE HIGHEST-RANKED REPLY WINS. */
    bool reply(const Room& r, const std::string& from, Reply m, double now) {
        const auto order = rankOrder(r.people);
        const int fr = rankOf(order, from);
        if (fr < 0) return false;
        bool changed = false;
        for (auto& [asker, q] : reqs) {
            const int ar = rankOf(order, asker);
            if (ar < 0 || fr >= ar) continue;                  // not above the asker: does not count
            const bool standingNo = !q.open && q.noUntil > now;
            if (!q.open && !standingNo) continue;              // nothing asked of this listener
            const int br = q.bestBy.empty() ? -1 : rankOf(order, q.bestBy);
            const bool outranks = br < 0 || fr < br || from == q.bestBy;
            if (!outranks) continue;
            if (standingNo) {
                // ★ After the window only a YES matters — from above the NO's sayer. A second NO buys no time.
                if (m != Reply::Yes) continue;
                q.noUntil = 0; q.granted = true; q.best = Reply::Yes; q.bestBy = from;
                changed = true;
                continue;
            }
            q.best = m; q.bestBy = from;
            if (m == Reply::Yes) { if (!q.granted) changed = true; q.granted = true; q.noUntil = 0; }
            else                 { if (q.granted || q.noUntil <= now) changed = true; q.granted = false; q.noUntil = now + noHoldS; }
        }
        return changed;
    }

    /** The clock: windows close, NOs lapse, the absent are forgotten. @return true if anybody's view changed. */
    bool tick(const Room& r, double now) {
        bool changed = false;
        const auto order = rankOrder(r.people);
        for (auto it = reqs.begin(); it != reqs.end(); ) {
            Req& q = it->second;
            const int ar = rankOf(order, it->first);
            if (ar >= 0) q.lastSeen = now;
            else if (now - q.lastSeen > kGoneForgetS) { it = reqs.erase(it); continue; }
            if (q.open && now >= q.deadline) {
                q.open = false;
                if (q.best != Reply::No) q.granted = true;     // ★ a YES, or nobody answered
                changed = true;
            }
            if (!q.open && q.noUntil > 0) {
                // ★★ A NO ENDS: on time, or when its sayer has gone or is no longer above the asker.
                const int nr = rankOf(order, q.bestBy);
                if (now >= q.noUntil || nr < 0 || (ar >= 0 && nr >= ar)) {
                    q.noUntil = 0; q.granted = true; changed = true;
                }
            }
            ++it;
        }
        return changed;
    }

    View view(const Room& r, const std::string& s, double now) const {
        View v;
        v.inForce = r.inForce;
        if (!r.inForce) return v;
        v.locked = locked(r, s);
        auto it = reqs.find(s);
        if (v.locked && it != reqs.end()) {
            const Req& q = it->second;
            if (q.open)                { v.state = "open"; v.secs = (int)(q.deadline - now + 0.999); }
            else if (q.noUntil > now)  { v.state = "no";   v.secs = (int)(q.noUntil - now + 0.999); }
            if (v.secs < 0) v.secs = 0;
        }
        return v;
    }
};

}  // namespace vibeask
