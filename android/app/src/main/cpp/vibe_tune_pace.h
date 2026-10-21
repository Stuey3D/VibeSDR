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
// ★★★ DAB BLOCK STEPS TOO (Stuart, 2026-10-21: "if I connect at 12b and rapidly tune down to 7D hammering
//     the multiplex button multiple times a second … dont tune the SDR just hold it and tune on final
//     move"). A block change is a whole re-acquire (retune, AGC settle, FIC lock, decoder restart), far
//     dearer than a VFO move, and an app from before the clients coalesced (or anybody's script) sends
//     one per press. So `{"type":"dab","on":1,"channel":N}` is held the same way, LATEST WINS, with its
//     own rule because "idle = pace 0" would still apply every press that arrives 150 ms apart:
//       - a LONE block request goes at once, as before — no delay is ever added to a single press;
//       - one arriving within kDabBlockGapMs of the last applied block waits for that gap;
//       - each newer request inside a hold pushes it to kDabBurstTrailMs past itself (the burst is
//         still going), but never past kDabBurstCapMs from the hold's start (the dial must not lag
//         for ever behind a held-down key).
//     So a hammered burst costs at most TWO re-acquires — its first press, which nobody can know is
//     the start of a burst, and the block it ended on — however many presses it had; a burst already
//     queued in the socket costs ONE. A newer `dab` that names its own block (a pick, with a service)
//     or leaves DAB REPLACES a held step rather than applying it first.
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
/** ★ DAB block steps — see the header. 400 < the clients' own 450 ms settle, so a client that already
 *  coalesces (one send per burst, bursts >= 450 ms apart) is never held by the server at all. */
constexpr int kDabBlockGapMs   = 400;
constexpr int kDabBurstTrailMs = 350;
constexpr int kDabBurstCapMs   = 1500;

/** The value of a top-level `"key":` in a flat JSON message, as raw text (no parse — the server's own
 *  reader is text too). Empty when absent. A string value comes back WITH its quotes. */
inline std::string rawValue(const std::string& msg, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    const auto p = msg.find(k);
    if (p == std::string::npos) return std::string();
    auto c = msg.find(':', p + k.size());
    if (c == std::string::npos) return std::string();
    ++c; while (c < msg.size() && (msg[c] == ' ' || msg[c] == '\t')) ++c;
    auto e = c;
    if (e < msg.size() && msg[e] == '"') { e = msg.find('"', e + 1); e = (e == std::string::npos) ? msg.size() : e + 1; }
    else while (e < msg.size() && msg[e] != ',' && msg[e] != '}' && msg[e] != ' ') ++e;
    return msg.substr(c, e - c);
}
/** Is this message of `"type":"<t>"`? Exact — "tuneStep" is not "tune". */
inline bool typeIs(const std::string& msg, const char* t) {
    return rawValue(msg, "type") == std::string("\"") + t + "\"";
}
/** `"on":1` (or true) — DAB's way in. Absent or anything else is OFF, as the server reads it. */
inline bool dabOn(const std::string& msg) {
    const std::string v = rawValue(msg, "on");
    return v == "1" || v == "true" || v == "1.0";
}
/** ★ A DAB BLOCK STEP: on, a block, and NO service — the shape every client's next/previous multiplex
 *  sends. Keyed with the block's value cut out, so a run of steps shares one key. A request with a
 *  service is a PICK (bookmark, list) and is never held; nor is an entry with no block. */
inline std::string dabBlockKey(const std::string& msg) {
    if (!typeIs(msg, "dab") || !dabOn(msg) || msg.find("\"sid\"") != std::string::npos) return std::string();
    const auto p = msg.find("\"channel\"");
    if (p == std::string::npos) return std::string();
    const std::string v = rawValue(msg, "channel");
    if (v.empty()) return std::string();
    const auto at = msg.find(v, p + 9);
    return "dab|" + msg.substr(0, at) + msg.substr(at + v.size());
}
inline bool isDabKey(const std::string& k) { return k.compare(0, 4, "dab|") == 0; }
/** A `dab` message that makes a held block step pointless: leaving DAB, or naming a block of its own. */
inline bool dabSupersedesStep(const std::string& msg) {
    return typeIs(msg, "dab") && (!dabOn(msg) || msg.find("\"channel\"") != std::string::npos);
}

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
    if (typeIs(msg, "dab")) return dabBlockKey(msg);
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
    bool heldDab = false;   ///< what is held is a DAB block step
    bool takenDab = false;  ///< what take() last handed out was one (so applied() can time the gap)
    int64_t holdStart = 0;  ///< when the current hold began — the DAB burst cap counts from here
    int64_t lastDabApplied = INT64_MIN / 2;

    bool holding() const { return !held.empty(); }
    /** ms until the held tune is due (0 = now), or -1 when nothing is held. */
    int waitMs(int64_t now) const {
        if (!holding()) return -1;
        return (int)std::max<int64_t>(0, std::min<int64_t>(due - now, 60000));
    }
    std::string take() { std::string m; m.swap(held); key.clear(); takenDab = heldDab; heldDab = false; return m; }
    void applied(int64_t now) { lastApplied = now; if (takenDab) { lastDabApplied = now; takenDab = false; } }

    /** Offer a text message. Returns a held tune that must be applied FIRST ("" when none), and sets
     *  `passThrough` when `msg` itself is not held — the caller then applies it straight after.
     *  `pace` is only called for a tune that starts a new hold, so an idle reader reads no load. */
    template <class PaceFn>
    std::string offer(const std::string& msg, int64_t now, PaceFn pace, bool& passThrough) {
        const std::string k = coalesceKey(msg);
        std::string first;
        if (k.empty()) {                       // not mergeable: keep its ORDER behind the held tune
            passThrough = true;
            if (heldDab) {
                // ★ A keep-alive changes nothing a block step could be ordered against: the hold stands.
                if (typeIs(msg, "ping")) return first;
                // ★ Leaving DAB, or a pick of a block: the held step is stale — replaced, never applied.
                if (dabSupersedesStep(msg)) { (void)take(); takenDab = false; ++merged; return first; }
            }
            if (holding()) first = take();
            return first;
        }
        passThrough = false;
        if (holding() && k != key) {                  // a different shape — the older one still lands,
            first = take(); lastApplied = now;        //   and the new one waits its pace behind it
            if (takenDab) lastDabApplied = now;
        }
        const bool dab = isDabKey(k);
        if (holding()) {
            ++merged;
            if (dab) due = std::max(due, std::min(holdStart + (int64_t)kDabBurstCapMs, now + (int64_t)kDabBurstTrailMs));
        } else {
            due = std::max(now, lastApplied + (int64_t)pace());
            if (dab) due = std::max(due, lastDabApplied + (int64_t)kDabBlockGapMs);
            holdStart = now;
        }
        held = msg; key = k; heldDab = dab;
        return first;
    }
};

}  // namespace vibetune
