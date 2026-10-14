// vibe_squelch_msg.h — is a `squelch` control message turning the gate ON?
//
// ★★★ THE "OFF" SENTINEL SAT INSIDE THE RANGE OF REAL THRESHOLDS (Stuart, 2026-10-14).
//     The wire was `{"type":"squelch","db":X}` with **db <= -100 meaning off** — a number chosen
//     when every radio's quiet channel read well above -100 dBFS. An Airspy HF+ on 127.875 MHz AM
//     does not: its channel level sits near -110 (needle at ~S3, spectrum floor -131), so auto
//     squelch's threshold (quiet level + 12 dB) landed at about -100 and wandered either side of it
//     as the noise moved. Every value at or below -100 read as OFF, so the gate never closed — while
//     the red line, drawn from the same number, sat exactly where it should. "It works at higher
//     signal and noise levels" is just "it works when the threshold happens to be above -100".
//     The same wall stopped a hand-set threshold: on the RSP1A on MW (signal S4, ~-100) any drag
//     into the left third of the bar (-130..-100) was OFF, and the handle snapped to the far left.
// ★★ THE CURE IS TO SAY "ON" IN WORDS, NOT IN A MAGIC NUMBER. A client that knows sends `"on"`;
//    the threshold is then whatever `db` says, however low. A client that does not (every build
//    before this one, Jr included) sends no `on`, and gets the old rule unchanged — PROTOCOL.md's
//    "defaults for silence": a parameter a client does not send behaves as before it existed.
//    hwinfo carries `"sqlOn":true` so a new client knows this server will honour a low threshold.
// ★ Header-only so vibeserver/test-squelch-msg.cpp drives the very function the server uses.
#pragma once
#include <cctype>
#include <cmath>
#include <string>

namespace vibe {

/** The legacy sentinel: without an explicit `on`, `db` at or below this is OFF. */
constexpr double kSquelchLegacyOffDb = -100.0;

/** Whether a `squelch` message (raw JSON `msg`, already-parsed threshold `db`) turns the gate on.
 *  An explicit `"on": true|false` wins; a non-finite threshold is never on (a gate comparing
 *  against NaN would read "open" by accident, never by design). */
inline bool squelchMsgOn(const std::string& msg, double db) {
    if (!std::isfinite(db)) return false;
    auto p = msg.find("\"on\"");
    if (p != std::string::npos) {
        p = msg.find(':', p);
        if (p != std::string::npos) {
            ++p;
            while (p < msg.size() && std::isspace((unsigned char)msg[p])) ++p;
            return msg.compare(p, 4, "true") == 0;
        }
    }
    return db > kSquelchLegacyOffDb;
}

}  // namespace vibe
