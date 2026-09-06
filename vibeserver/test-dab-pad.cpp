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

    /* ── ★★★ THE X-PAD FIELD IS TRANSMITTED BYTE-REVERSED (EN 300 401 7.4.2.0) ─────────────
     *  Logical order: [CI list][sub-field 1][sub-field 2]…; on the wire the whole field is
     *  reversed, so the list is adjacent to F-PAD and every sub-field's bytes are backwards. The
     *  previous reader copied sub-fields forwards in wire order and no label ever assembled. */
    auto wire = [](const std::vector<uint8_t>& logical, bool ciFlag, int xInd, size_t junkBefore) {
        std::vector<uint8_t> w(junkBefore, 0xA5);          // audio bytes before the field
        for (size_t i = logical.size(); i > 0; --i) w.push_back(logical[i - 1]);
        w.push_back(uint8_t(xInd << 4));                   // F-PAD byte L-1: type 00, X-PAD ind
        w.push_back(uint8_t(ciFlag ? 0x02 : 0x00));        // F-PAD byte L: CI flag
        return w;
    };
    {
        PadReader r;
        auto g = group("Hello World!", true, true, false);            // 2 + 12 + 2 = 16 bytes
        CHECK(g.size() == 16, "test group is one 16-byte sub-field");
        std::vector<uint8_t> logical{ uint8_t((4 << 5) | kXpadDlsStart), uint8_t(kXpadEnd) };  // CI len idx 4 = 16, end marker
        logical.insert(logical.end(), g.begin(), g.end());
        auto w = wire(logical, true, 2, 37);
        r.feed(w.data(), w.size());
        CHECK(r.xIndCount(2) == 1, "variable X-PAD recognised");
        CHECK(r.dls().crcOk() == 1 && r.dls().label().text == "Hello World!",
              "★ a variable X-PAD field read in logical order yields the label");
    }
    // ── a group spanning two frames, the second with NO indicator list (CI flag 0) ──────
    {
        PadReader r;
        auto g = group("Talking Heads", true, true, false);           // 2 + 13 + 2 = 17 bytes
        std::vector<uint8_t> l1{ uint8_t((2 << 5) | kXpadDlsStart), uint8_t(kXpadEnd) };  // len idx 2 = 8
        l1.insert(l1.end(), g.begin(), g.begin() + 8);
        auto w1 = wire(l1, true, 2, 20);
        r.feed(w1.data(), w1.size());
        CHECK(!r.dls().label().valid, "half a group shows nothing");
        // no CI list: one sub-field, the same length as the previous FIELD (10 bytes here)
        std::vector<uint8_t> l2(g.begin() + 8, g.end());              // the remaining 9 bytes
        l2.push_back(0x00);                                           // padding to 10
        auto w2 = wire(l2, false, 2, 20);
        r.feed(w2.data(), w2.size());
        CHECK(r.dls().label().text == "Talking Heads", "★ a CI-less continuation completes the group");
    }
    // ── short X-PAD: 4 bytes, CI + 3 data, then CI-less 4-byte continuations ───────────
    {
        PadReader r;
        auto g = group("Short", true, true, false);                   // 2 + 5 + 2 = 9 bytes
        std::vector<uint8_t> l1{ uint8_t(kXpadDlsStart) }; l1.insert(l1.end(), g.begin(), g.begin() + 3);
        auto w1 = wire(l1, true, 1, 5);  r.feed(w1.data(), w1.size());
        std::vector<uint8_t> l2(g.begin() + 3, g.begin() + 7);
        auto w2 = wire(l2, false, 1, 5); r.feed(w2.data(), w2.size());
        std::vector<uint8_t> l3(g.begin() + 7, g.end()); l3.push_back(0); l3.push_back(0);
        auto w3 = wire(l3, false, 1, 5); r.feed(w3.data(), w3.size());
        CHECK(r.xIndCount(1) == 3, "short X-PAD recognised three times");
        CHECK(r.dls().label().text == "Short", "★ short X-PAD with CI-less continuations assembles");
    }
    // ── DAB+: the PAD is a data stream element at the start of the access unit ──────────
    {
        PadReader r;
        auto g = group("DAB+ Label", true, true, false);              // 14 bytes
        std::vector<uint8_t> logical{ uint8_t((4 << 5) | kXpadDlsStart), uint8_t(kXpadEnd) };
        logical.insert(logical.end(), g.begin(), g.end());
        logical.push_back(0); logical.push_back(0);                   // padding to the 16-byte sub-field
        auto pad = wire(logical, true, 2, 0);                         // exact field + F-PAD
        std::vector<uint8_t> au{ uint8_t(4 << 5), uint8_t(pad.size()) };
        au.insert(au.end(), pad.begin(), pad.end());
        au.insert(au.end(), { 0x21, 0x00, 0x49, 0x90 });              // whatever AAC follows
        r.feedAccessUnit(au.data(), au.size());
        CHECK(r.dls().label().text == "DAB+ Label", "★ the DSE at the start of a DAB+ AU yields the label");
        const uint8_t plain[4] = { 0x21, 0x00, 0x49, 0x90 };          // an AU with no DSE
        r.feedAccessUnit(plain, 4);
        CHECK(r.xIndCount(0) == 1 && r.dseBad() == 0, "an AU without a DSE is 'no PAD', not an error");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
