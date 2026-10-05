// vibe_tune_pace.h — tunes that arrive faster than the server can apply them: the NEWEST wins.
//
// ★★★ WHY (Stuart, 2026-10-05): "if the server's CPU is reporting that it is struggling we need to
//     slow down the amount of tune commands so that we don't overload it, I had a bit of a nightmare
//     setting up the pi2 in the garage last night probably due to network connection and the server
//     CPU getting bogged down." Every socket's reader applied every tune it was sent, IN ORDER, one
//     after another — so a drum spin on a slow link arrived as a clump and the box worked through all
//     of them (re-centre, IF filter, AGC reset, channel retune) to reach a position the user had
//     already left. On the Pi 2 the WFM chain is one thread behind a 4-slot queue, and a full queue
//     BLOCKS the DSP thread, so a backlog of retunes there is felt by everybody listening.
// ★★ ON THE SERVER, so it covers every client that will ever connect — old apps that cannot be
//    patched, Jr, the web page, rtl_tcp bridges. The clients pace too (src/services/tunePace.ts), but
//    the server must not depend on client good behaviour (the shared-dial contract's own rule).
// ★★ LATEST WINS, AND THE LAST ONE ALWAYS LANDS. A tune is HELD, never dropped: a newer one of the
//    same shape replaces it, and whatever is held when the window closes — or when any other message
//    arrives, or the socket goes — is applied. So the radio always ends where the user stopped.
// ★ Idle server = pace 0: only tunes ALREADY WAITING in the socket are merged (the backlog), and a
//   lone tune is applied the instant it arrives, exactly as before. Nothing changes on a healthy box.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace vibetune {

/** What the server knows about its own load at the moment a tune arrives. */
struct Load {
    double dspPct      = 0;     ///< dspCpu — the DSP chain's share of real time (>100 = cannot keep up)
    int    cpuLevel    = 0;     ///< the health pill's CPU rung: 0 OK, 1 WARM, 2 HIGH (>=75 %), 3 CRIT (>=90 %)
    bool   throttled   = false; ///< the snail: fully loaded AND below the all-core clock
    double backlogMs   = 0;     ///< IQ queued ahead of the DSP, in ms of signal
    bool   demodBlocked = false;///< the DSP thread blocked on a FULL demod queue in the last few seconds
};

/** ★ The two rungs. 150 ms is about one tune per drum detent on a fast spin — the dial still moves
 *  visibly, and a retune (AGC settle alone is longer) has finished before the next one starts. 350 ms
 *  is "the pipeline is already not keeping up": a tune then costs a gap in everybody's audio, so the
 *  fewest that still follow the user. Below a second, because a dial that lags a second feels broken. */
constexpr int kPaceLoadedMs = 150;
constexpr int kPaceChokedMs = 350;

/** ★★ The minimum gap between two applied tunes on one socket. Pure, so the thresholds are tested.
 *  - CHOKED (350): the DSP is past real time (dspCpu >= 95 — at 100 the backlog grows until something
 *    drops), OR its demod queue has been FULL recently (the Pi 2's step from 52 to 103 % is exactly
 *    this — see "demodWaits" in /vibeserver.json), OR a quarter-second of IQ is already queued, OR the
 *    whole machine reads CRITICAL.
 *  - LOADED (150): dspCpu >= 70, the CPU rung HIGH (>= 75 %), or the snail is out — slow but coping.
 *  - otherwise 0. */
inline int paceMs(const Load& l) {
    if (l.dspPct >= 95.0 || l.demodBlocked || l.backlogMs >= 250.0 || l.cpuLevel >= 3) return kPaceChokedMs;
    if (l.dspPct >= 70.0 || l.cpuLevel >= 2 || l.throttled) return kPaceLoadedMs;
    return 0;
}

/** ★ Two tunes may be merged ONLY when they differ in nothing but the frequency — the same mode, the
 *  same passband, the same DAB service. So the key is the message with the frequency's VALUE cut out.
 *  A mode change, a bandwidth-only tune (no frequency) and anything that is not a tune return "" and
 *  are never merged: a newer frequency must not swallow an older mode switch.
 *  ★ Text, not a JSON parse: every client writes `"frequency":<number>` and the server's own reader
 *    (jsonNum) reads it the same way. Whitespace after the colon is tolerated. */
inline std::string coalesceKey(const std::string& msg) {
    const auto t = msg.find("\"type\"");
    if (t == std::string::npos) return std::string();
    auto c = msg.find(':', t + 6);
    if (c == std::string::npos) return std::string();
    ++c; while (c < msg.size() && (msg[c] == ' ' || msg[c] == '\t')) ++c;
    if (msg.compare(c, 6, "\"tune\"") != 0) return std::string();
    const auto f = msg.find("\"frequency\"");
    if (f == std::string::npos) return std::string();
    auto v = msg.find(':', f + 11);
    if (v == std::string::npos) return std::string();
    ++v; while (v < msg.size() && (msg[v] == ' ' || msg[v] == '\t')) ++v;
    auto e = v;
    while (e < msg.size() && (std::isdigit((unsigned char)msg[e]) || msg[e] == '.' || msg[e] == '-'
                              || msg[e] == '+' || msg[e] == 'e' || msg[e] == 'E')) ++e;
    if (e == v) return std::string();
    return msg.substr(0, v) + msg.substr(e);
}

/** ★★ ONE SOCKET'S HELD TUNE. The reader owns one; nothing else touches it, so no lock.
 *  The reader's loop is:
 *    wait = holding ? waitMs(now) : 5000            — wake for the hold, or for the usual quiet slice
 *    on a quiet slice with a tune due:   apply(take()), then applied(now)
 *    on a text frame:                    offer() it; apply what offer() hands back, in order
 *    on close:                           apply(take()) if holding — the last tune is never lost
 *  ★ Paced from the END of the previous apply, not its start: on a box that takes 200 ms to retune,
 *    "150 ms between starts" would be no pacing at all. */
struct TuneHold {
    std::string held, key;
    int64_t due = 0;
    int64_t lastApplied = INT64_MIN / 2;
    uint64_t merged = 0;    ///< tunes replaced by a newer one before they were applied

    bool holding() const { return !held.empty(); }
    /** ms until the held tune is due (0 = now), or -1 when nothing is held. */
    int waitMs(int64_t now) const {
        if (!holding()) return -1;
        return (int)std::max<int64_t>(0, std::min<int64_t>(due - now, 60000));
    }
    std::string take() { std::string m; m.swap(held); key.clear(); return m; }
    void applied(int64_t now) { lastApplied = now; }

    /** Offer a text message. Returns a held tune that must be applied FIRST ("" when none), and sets
     *  `passThrough` when `msg` itself is not held — the caller then applies it straight after.
     *  `pace` is only called for a tune that starts a new hold, so an idle reader reads no load. */
    template <class PaceFn>
    std::string offer(const std::string& msg, int64_t now, PaceFn pace, bool& passThrough) {
        const std::string k = coalesceKey(msg);
        std::string first;
        if (k.empty()) {                       // not mergeable: keep its ORDER behind the held tune
            passThrough = true;
            if (holding()) first = take();
            return first;
        }
        passThrough = false;
        if (holding() && k != key) {                  // a different shape — the older one still lands,
            first = take(); lastApplied = now;        //   and the new one waits its pace behind it
        }
        if (holding()) ++merged;
        else due = std::max(now, lastApplied + (int64_t)pace());
        held = msg; key = k;
        return first;
    }
};

}  // namespace vibetune
