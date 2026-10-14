// test-squelch-msg.cpp — a squelch threshold below -100 dBFS is a THRESHOLD, not "off".
//
// ★★★ The bug (Stuart, 2026-10-14): auto squelch on an Airspy HF+ at 127.875 MHz AM drew its red
//     line in the right place and never closed the gate. The channel's quiet level is about -110,
//     so quiet + 12 dB lands near -100 — and the wire's "db <= -100 means off" turned every such
//     threshold into OFF. On the RSP1A on MW, any hand-set threshold in the bottom third of the bar
//     did the same. See vibe_squelch_msg.h.
// ★ The legacy rule must survive for every client that does not send `on` (all builds before this,
//   the store Jr): PROTOCOL.md rule 4, defaults for silence.
#include "vibe_squelch_msg.h"
#include <cmath>
#include <cstdio>
#include <string>

static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

/** The server's gate, as written in onClientAudio / the shared path: closed when on && chan < db. */
static bool closed(const std::string& msg, double db, double chan) {
    return vibe::squelchMsgOn(msg, db) && chan < db;
}

int main() {
    using vibe::squelchMsgOn;
    std::printf("── squelch message: explicit on ──\n");
    // The Airspy HF+ airband case, with the numbers from the report: channel ~-110 quiet, the
    // threshold auto squelch computes ~-98..-106 depending on where the noise baseline sits.
    ok(squelchMsgOn(R"({"type":"squelch","db":-106,"on":true})", -106), "-106 with on:true is ON (was OFF)");
    ok(closed(R"({"type":"squelch","db":-106,"on":true})", -106, -110), "quiet channel at -110 under a -106 threshold: gate CLOSED");
    ok(!closed(R"({"type":"squelch","db":-106,"on":true})", -106, -95), "a -95 signal opens it");
    ok(squelchMsgOn(R"({"type":"squelch","db":-129,"on":true})", -129), "the bottom of the bar (-129) is still a threshold");
    ok(!squelchMsgOn(R"({"type":"squelch","db":-100,"on":false})", -100), "on:false is OFF");
    ok(!squelchMsgOn(R"({"type":"squelch","db":-60,"on":false})", -60), "on:false wins even over a high db");
    ok(squelchMsgOn(R"({"type":"squelch", "db": -112, "on" : true})", -112), "spacing does not matter (jsonOn's lesson)");
    ok(!squelchMsgOn(R"({"type":"squelch","db":-100,"on":true})", NAN), "a non-finite threshold is never on");

    std::printf("── squelch message: legacy (no on field) — unchanged ──\n");
    ok(!squelchMsgOn(R"({"type":"squelch","db":-100})", -100), "legacy -100 = OFF (old app, web, Jr)");
    ok(!squelchMsgOn(R"({"type":"squelch","db":-999})", -999), "legacy -999 = OFF");
    ok(!squelchMsgOn(R"({"type":"squelch","db":-110})", -110), "legacy -110 = OFF, as it always was");
    ok(squelchMsgOn(R"({"type":"squelch","db":-99})", -99), "legacy -99 = ON");
    ok(squelchMsgOn(R"({"type":"squelch","db":-60})", -60), "legacy -60 = ON");

    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
