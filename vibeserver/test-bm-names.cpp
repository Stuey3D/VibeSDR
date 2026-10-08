// ★★★ A STATION WHOSE NAME ROTATES STILL GETS A NAME (2026-10-08) — vibe_bm_names.h.
//
// Chicopee (US) 7 of 8 learned bookmarks and Kiko's TV box (Brazil) 3 of 4 sat at "PI: XXXX" for ever: the server names
// a station when its PS settles, and a marquee never settles. Two rules:
//   1. US: the call letters the PI encodes — checked against the PIs on Chicopee's own list.
//   2. Anywhere: the segments a rotation keeps coming back to — the station's name, not the song playing.
// ★ SILENT, pure: no radio, no network.
#include "vibe_bm_names.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) fails++;
}

int main() {
    using vibe::rbdsCallsign;
    // ── 1. US call letters, from Chicopee's list ──
    check(rbdsCallsign(0x87A6) == "WTIC", "87A6 → WTIC (96.5 Hartford) — " + rbdsCallsign(0x87A6));
    check(rbdsCallsign(0x672C) == "WHAI", "672C → WHAI (98.3 Greenfield) — " + rbdsCallsign(0x672C));
    check(rbdsCallsign(0x5660) == "WAQY", "5660 → WAQY (102.1 Springfield) — " + rbdsCallsign(0x5660));
    check(rbdsCallsign(0x1000) == "KAAA" && rbdsCallsign(0x54A7) == "KZZZ", "the K range runs KAAA … KZZZ");
    check(rbdsCallsign(0x54A8) == "WAAA" && rbdsCallsign(0x994F) == "WZZZ", "the W range runs WAAA … WZZZ (0x994F)");
    check(rbdsCallsign(0xD388).empty() && rbdsCallsign(0x0022).empty() && rbdsCallsign(0x9950).empty(),
          "outside the call-letter ranges (D388 on Chicopee, Kiko's 0022, 3-letter calls) → no name, never a made-up one");
    check(rbdsCallsign(0xA6AC) == rbdsCallsign(0x60AC), "a PI 0xAxyz is the compressed form of 0xx0yz");

    // ── 2. a rotation ──
    {   // Kiko's 94.5: "UMUARAMA" / "MASSA" alternating every 2 s for 10 minutes → both
        vibe::RotationNamer r; std::string n;
        for (long long t = 0; t < 600; t += 2) n = r.feed((t / 2) % 2 ? "MASSA" : "UMUARAMA", 1'000'000 + t);
        check(n == "UMUARAMA MASSA", "a two-part marquee for 10 min → \"" + n + "\"");
    }
    {   // not before it has proved itself: 3 minutes is not enough
        vibe::RotationNamer r; std::string n;
        for (long long t = 0; t < 180; t += 2) n = r.feed((t / 2) % 2 ? "MASSA" : "UMUARAMA", 1'000'000 + t);
        check(n.empty(), "after only 3 minutes: no name yet (a song lasts that long)");
    }
    {   // station name + song text: a different song every 4 minutes for 12 minutes → only the name
        vibe::RotationNamer r; std::string n;
        const char* songs[3][2] = { {"JUSTIN", "BIEBER"}, {"ANITTA", "ENVOLVER"}, {"LUAN", "SANTANA"} };
        for (long long t = 0; t < 720; t += 2) {
            const int song = (int)(t / 240), ph = (int)(t / 2) % 3;
            n = r.feed(ph == 0 ? "MASSA FM" : songs[song][ph - 1], 2'000'000 + t);
        }
        check(n == "MASSA FM", "name + song titles changing every 4 min → only the name: \"" + n + "\"");
    }
    {   // a garbled copy on a weak signal does not join the name
        vibe::RotationNamer r; std::string n;
        for (long long t = 0; t < 720; t += 2) n = r.feed((t / 2) % 7 == 0 ? "LUX FN" : "LUX FM", 3'000'000 + t);
        check(n == "LUX FM", "a one-character garble recurring too → only the true segment: \"" + n + "\"");
    }
    {   // a character-scroller ("RADIO MA", "ADIO MAS" …) makes many segments: capped, no crash, no runaway name
        vibe::RotationNamer r; std::string n; const std::string s = "RADIO MASSA FM UMUARAMA ";
        for (long long t = 0; t < 900; t += 1) { const int o = (int)(t % s.size()); std::string seg = (s + s).substr(o, 8); n = r.feed(seg, 4'000'000 + t); }
        check((int)r.segs.size() <= vibe::RotationNamer::MAX_SEGS, "a character scroller keeps at most MAX_SEGS segments");
        check(n.empty(), "…and yields NO name — window fragments are not a station name: \"" + n + "\"");
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
