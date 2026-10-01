// vibe_session_turns.h — whose turn it is on a time-limited receiver, and what happens after it.
//
// ★★★ WHY THIS IS ITS OWN HEADER. The turn book used to live inside the shim's Impl, where nothing
//     but a live radio and a two-minute wait could exercise it — and the fault it had (below) took
//     a listener on somebody else's server to find. Pulled out, the whole rule runs on a synthetic
//     clock in test-session-turns.cpp in milliseconds.
//
// ★★★ THE FAULT (B10, Stuart on Kiko's server, 2026-10-01). A single-user receiver, 15-minute turns.
//     His turn ended and the TIME UP card said "You can try again in about 2 minutes". After the
//     two-minute cooldown he was let in — and ended again on the very next tick, because a listener
//     back inside the turn break RESUMES their turn, and his had no time left. That second ending
//     put him back on cooldown, and every attempt refreshed the turn's `seen`, so the fresh turn the
//     card mentioned never arrived while he kept trying. Nobody else wanted the radio the whole time.
//
// ★★★ THE RULE NOW: A SPENT TURN IS BORROWED TIME, NOT A LOCKED DOOR. Once the limit has ended a
//     turn, that turn is marked SPENT. A listener who comes back with a spent turn (after the
//     cooldown, which is unchanged and is what gives somebody else the first chance) is admitted
//     like anyone else when there is room — and is then held to the SOFT rule whatever the owner
//     chose: they keep the radio until somebody else actually wants it, then get the usual notice
//     and go. That is "you keep it until somebody else wants it", and it is what the limit is FOR:
//     sharing a scarce radio. Refusing somebody a FREE radio that nobody is waiting for shares
//     nothing with anyone.
//  ★★ WHAT KEEPS THE LIMIT REAL: the cooldown (an address whose turn ended is refused outright for a
//     while, which is the window in which somebody else gets in), the queue (a waiter ends borrowed
//     time after a short notice), and the turn break (a FULL guaranteed turn only comes back after
//     the listener has been away kTurnBreakS). Reloading still buys nothing — the turn is keyed on
//     the browser/address, not the page.
#pragma once
#include <map>
#include <string>

namespace vibeturn {

/** ★ Away this long and the next arrival is a NEW turn, like a new listener. Fixed, not the limit:
 *  see the 2026-09-19 note in the shim (an hour-limit server charged a listener for an hour they
 *  were away). This is the number the server reports to clients as `fresh`. */
constexpr double kTurnBreakS = 15.0 * 60.0;

struct Turn {
    double started = 0;   ///< when the turn began, moved forward by any time spent away
    double seen    = 0;   ///< the last moment the listener was here
    bool   spent   = false;   ///< ★ the limit has already ended this turn once — see the header note
};

class Book {
public:
    std::map<std::string, Turn> turns;
    double breakS = kTurnBreakS;

    /** The start time to use for a listener arriving under `key`. Continues the turn of somebody
     *  back within the break (WITHOUT the time they were away); otherwise begins a new one. */
    double startFor(const std::string& key, double now) {
        // ★ Prune while we are here: this map would otherwise grow for the life of the process,
        //   one entry per visitor ever seen, on a server whose whole point is strangers.
        for (auto it = turns.begin(); it != turns.end(); ) {
            if (now - it->second.seen > breakS && it->first != key) it = turns.erase(it);
            else ++it;
        }
        if (key.empty()) return now;
        auto it = turns.find(key);
        if (it != turns.end() && (now - it->second.seen) <= breakS) {
            // ★ Same person, back within the break: their turn continues, WITHOUT the time they were
            //   away. A live listener is touched every tick, so a second socket of the same visit
            //   sees ~0 here.
            it->second.started += now - it->second.seen;
            it->second.seen = now;
            return it->second.started;
        }
        turns[key] = Turn{ now, now, false };   // ★ a NEW turn is never spent
        return now;
    }
    /** Keep a live listener's turn from lapsing while they are still here. */
    void touch(const std::string& key, double now) {
        if (key.empty()) return;
        auto it = turns.find(key);
        if (it != turns.end()) it->second.seen = now;
    }
    /** The limit has just ended this turn. Called at EVERY place the limit ends a session. */
    void markSpent(const std::string& key, double now) {
        if (key.empty()) return;
        Turn& t = turns[key];
        if (t.started <= 0) t.started = now;
        t.seen = now;
        t.spent = true;
    }
    /** ★ Is this listener on BORROWED time — back after the limit already ended their turn? */
    bool spent(const std::string& key, double now) const {
        if (key.empty()) return false;
        auto it = turns.find(key);
        return it != turns.end() && it->second.spent && (now - it->second.seen) <= breakS;
    }
};

/** ★★ Is a listener who is past their limit kept until somebody else wants the radio? Always on a
 *  SOFT server; on a HARD one only when they are on borrowed time (their turn was already ended
 *  once, and they came back to a free radio). A hard limit still ends a FIRST turn on the dot. */
inline bool keptUntilWanted(bool softMode, bool borrowed) { return softMode || borrowed; }

/** What to do about ONE listener who is past their limit. */
enum class OverAct {
    End,          ///< end the session now (hard limit, or the notice has run out with somebody waiting)
    Keep,         ///< nothing: nobody wants the radio
    StartNotice,  ///< somebody is waiting — start the handover notice and tell the listener
    Wait,         ///< the notice is running
    Withdraw,     ///< the notice ran out but the waiter has gone: keep the radio, withdraw the notice
};
/** @param kept      keptUntilWanted(...) for this listener
 *  @param contended somebody is waiting for this radio (and, on a multi-user radio, it is full)
 *  @param noticeAt  when a running notice expires, 0 = none running */
inline OverAct overLimit(bool kept, bool contended, double noticeAt, double now) {
    if (!kept) return OverAct::End;
    if (noticeAt <= 0) return contended ? OverAct::StartNotice : OverAct::Keep;
    if (now < noticeAt) return OverAct::Wait;
    return contended ? OverAct::End : OverAct::Withdraw;
}

}  // namespace vibeturn
