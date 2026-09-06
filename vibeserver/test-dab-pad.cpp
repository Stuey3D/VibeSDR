// test-dab-pad.cpp — the dynamic label: segment assembly, the toggle, and the CRC that guards it.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "vibe_dab_pad.h"

using namespace vibedab;
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("  FAIL: %s\n", m); ++fails; } } while (0)

/** Build one DLS data group the way a transmitter would: prefix, segment, CRC over both. */
static std::vector<uint8_t> group(const std::string& seg, bool first, bool last, bool toggle) {
    std::vector<uint8_t> g;
    g.push_back(uint8_t((toggle ? 0x80 : 0) | (first ? 0x40 : 0) | (last ? 0x20 : 0)
                        | uint8_t(seg.size() - 1)));
    g.push_back(0x00);                                  // charset 0, EBU Latin
    g.insert(g.end(), seg.begin(), seg.end());
    const uint16_t c = dlsCrc16(g.data(), g.size());
    g.push_back(uint8_t(c >> 8)); g.push_back(uint8_t(c & 0xFF));
    return g;
}

int main() {
    printf("test-dab-pad\n");

    // ── sub-field lengths, straight from table 25 ───────────────────────────
    CHECK(xpadSubFieldLen(0) == 4 && xpadSubFieldLen(3) == 12 && xpadSubFieldLen(7) == 48,
          "contents indicator length index maps to 4/12/48");

    // ── one whole label in a single segment ─────────────────────────────────
    {
        DlsAssembler a;
        auto g = group("Now Playing", true, true, false);
        a.pushGroup(g.data(), g.size());
        CHECK(a.label().valid, "a single-segment label completes");
        CHECK(a.label().text == "Now Playing", "and reads back exactly");
        CHECK(a.crcOk() == 1 && a.crcFail() == 0, "its CRC passed");
    }

    // ── a label split across segments, which is the normal case on air ──────
    {
        DlsAssembler a;
        auto g1 = group("Talking Heads", true,  false, false);
        auto g2 = group(" - Road to No", false, false, false);
        auto g3 = group("where",         false, true,  false);
        a.pushGroup(g1.data(), g1.size());
        CHECK(!a.label().valid, "★ nothing is shown until the LAST segment arrives");
        a.pushGroup(g2.data(), g2.size());
        a.pushGroup(g3.data(), g3.size());
        CHECK(a.label().text == "Talking Heads - Road to Nowhere", "segments join in order");
        CHECK(a.label().changes == 1, "and count as one label, not three");
    }

    /* ── ★★★ THE CRC IS THE POINT: A DAMAGED SEGMENT MUST NOT REACH THE SCREEN ──────────
     *  Audio can be concealed and mud is tolerable; a track name is READ, and half a corrupted
     *  one looks like a fault in the radio rather than in the air. */
    {
        DlsAssembler a;
        auto g = group("Clean Text Here", true, true, false);
        g[5] ^= 0x40;                                    // one bit flip in the segment
        a.pushGroup(g.data(), g.size());
        CHECK(!a.label().valid, "★ a segment failing CRC is DROPPED, not displayed");
        CHECK(a.crcFail() == 1, "and counted, so the error rate stays visible");
    }

    /* ── ★★★ THE TOGGLE IS WHAT MAKES "THE TRACK CHANGED" DIFFERENT FROM "SENT AGAIN" ──
     *  Stations repeat the same label continuously. Without the toggle a listener's "now playing"
     *  would appear to change on every repetition, and a change counter would be meaningless. */
    {
        DlsAssembler a;
        auto g1 = group("Track One", true, true, false);
        a.pushGroup(g1.data(), g1.size());
        a.pushGroup(g1.data(), g1.size());
        a.pushGroup(g1.data(), g1.size());
        CHECK(a.label().changes == 1, "★ the same label repeated is ONE change, not three");
        auto g2 = group("Track Two", true, true, true);   // toggle flipped
        a.pushGroup(g2.data(), g2.size());
        CHECK(a.label().text == "Track Two" && a.label().changes == 2,
              "a new label with the toggle flipped is a second change");
    }

    /* ── joining mid-label, which is what happens on every tune ──────────────────────── */
    {
        DlsAssembler a;
        auto mid = group("ddle of a label", false, false, false);
        auto end = group(" ends here",      false, true,  false);
        a.pushGroup(mid.data(), mid.size());
        a.pushGroup(end.data(), end.size());
        CHECK(!a.label().valid,
              "★ a label joined after its First segment is discarded, not shown truncated");
    }

    /* ── the clear-display command ───────────────────────────────────────────────────── */
    {
        DlsAssembler a;
        auto g = group("Something", true, true, false);
        a.pushGroup(g.data(), g.size());
        std::vector<uint8_t> cmd{ 0x10 | 0x01, 0x00 };    // command, field 2 = 1 = clear
        const uint16_t c = dlsCrc16(cmd.data(), cmd.size());
        cmd.push_back(uint8_t(c >> 8)); cmd.push_back(uint8_t(c & 0xFF));
        a.pushGroup(cmd.data(), cmd.size());
        CHECK(a.label().text.empty(),
              "★ 'clear display' blanks the label — the last track must not sit under the next programme");
    }

    // ── the PAD walker: no X-PAD means nothing happens, and must not read off the end ──
    {
        PadReader r;
        const uint8_t noX[2] = { 0x00, 0x00 };            // X-PAD indicator 0
        r.feed(noX, sizeof noX);
        CHECK(r.framesSeen() == 1 && r.dlsBytes() == 0, "a frame with no X-PAD yields nothing");
        r.feed(nullptr, 0);
        r.feed(noX, 1);
        CHECK(r.framesSeen() == 1, "★ a short or absent PAD is ignored, not walked off the end");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
