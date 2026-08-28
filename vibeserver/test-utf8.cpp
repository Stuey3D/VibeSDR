// test-utf8.cpp — the wire may never carry a byte a browser will hang up over.
//
// ★★★ WHY THIS EXISTS. RFC 6455 §8.1 says a client MUST fail the WebSocket connection when a TEXT
//     frame is not well-formed UTF-8, and browsers do. So one bad byte in one JSON message takes
//     down the spectrum/control socket and everything on it — the waterfall AND tuning — while the
//     audio socket, being a different socket, plays on perfectly. That asymmetry is what made it
//     look like a network fault for weeks.
// ★★★ AND THE BAD BYTE COMES OFF THE AIR. RDS text is EBU-coded and arrives over a noisy link; with
//     block errors at 87-93% a corrupted group put 0x81 into PTYN and it went into the JSON
//     verbatim. Intermittent, and it follows the STATION rather than the machine — seen on two
//     unrelated servers and blamed on both of their networks (2026-08-28).
//     The literal below is the frame captured off 96.1 MHz that afternoon.
#include "vibe_admin.h"
#include <cstdio>
#include <string>
static int fails = 0;
static void ok(bool c, const char* what) { printf("  %s  %s\n", c ? "ok  " : "FAIL", what); if (!c) fails++; }
static bool valid(const std::string& s) {           // the check a browser makes
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = s[i]; int n;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return false;
        if (i + n >= s.size()) return false;
        for (int k = 1; k <= n; k++) if ((((unsigned char)s[i+k]) & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}
int main() {
    using vibeadmin::utf8Clean;
    // ★ THE FRAME THAT STARTED IT — the real PTYN captured off 96.1 with 93% block errors.
    const std::string ptyn = "    \x81.DS";
    ok(!valid(ptyn), "★★★ the captured PTYN really is invalid UTF-8 (the bug reproduces)");
    ok(valid(utf8Clean(ptyn)), "★★★ and cleaning it makes a frame a browser will accept");
    ok(utf8Clean(ptyn) == "    .DS", "★★ only the corrupt byte is dropped — the text survives");
    // Text that is ALREADY valid must be untouched: station names carry real accents.
    ok(utf8Clean("BBC Radio 4") == "BBC Radio 4", "★ ASCII is untouched");
    ok(utf8Clean("Bayern 3 über") == "Bayern 3 über", "★★ a real 2-byte char survives");
    ok(utf8Clean("€100") == "€100", "★ 3-byte (euro sign) survives");
    ok(utf8Clean("\U0001F4FB radio") == "\U0001F4FB radio", "★ 4-byte (emoji) survives");
    // The ill-formed shapes a noisy link can produce.
    ok(utf8Clean(std::string("a\xC3", 2)) == "a", "★★ a lead byte truncated at the end is dropped");
    ok(utf8Clean("a\xC3\x28" "b") == "a(b", "★★ a bad continuation drops the lead, keeps the rest");
    ok(utf8Clean("a\x80\x80" "b") == "ab", "★★ stray continuation bytes are dropped");
    ok(utf8Clean("a\xC0\xAF" "b") == "ab", "★★★ an OVERLONG encoding is ill-formed and goes");
    ok(utf8Clean("a\xED\xA0\x80" "b") == "ab", "★★★ a surrogate is ill-formed and goes");
    ok(utf8Clean("a\xF5\x80\x80\x80" "b") == "ab", "★★ past U+10FFFF goes");
    // And the escaper that RDS actually travels through must be safe by construction.
    ok(valid(vibeadmin::esc(ptyn)), "★★★ esc() output is valid UTF-8 for the frame that broke it");
    ok(vibeadmin::esc("say \"hi\"\n") == "say \\\"hi\\\"\\n", "★ and it still escapes JSON properly");
    printf("\n%s\n", fails ? "FAILURES" : "all utf8 checks passed");
    return fails ? 1 : 0;
}
