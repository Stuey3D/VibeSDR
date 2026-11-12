// test-dab-last-service.cpp — the station chosen when a multiplex is picked without one (vibe_dab_last_service.h).
//
// ★★★ Stuart, 2026-11-12: "last on MUX is good, but with one extra rule … in the case where a station is on multiple
//     multiplexes then switch to that one instead" (Nation 80s on 10C and 10D). The rule's ORDER and the store's
//     bounds, on fixtures. The decoder end to end — the bench clip through DabService — is test-dab-station-choice.
#include "vibe_dab_last_service.h"
#include <cstdio>
#include <string>
#include <unistd.h>

using namespace vibedablast;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("  FAIL: %s\n", msg); ++fails; } } while (0)

int main() {
    const std::vector<Candidate> d10 = { {0xC0D1, "Absolute 80s"}, {0xC0D2, "Nation 80s"}, {0xC0D3, "talkSPORT"} };
    const char* why = nullptr;
    // 1. the same station by SId
    CHECK(chooseService(d10, 0xC0D2, "Nation 80s", 0xC0D3, &why) == 0xC0D2, "the same SId wins over the last-on-mux");
    // 1b. the same programme service under another SId — by an exact, unique label
    CHECK(chooseService(d10, 0xAAAA, "  NATION   80s ", 0xC0D3) == 0xC0D2, "the same label (case/spaces aside) wins next");
    const std::vector<Candidate> two = { {1, "Heart"}, {2, "Heart"}, {3, "Smooth"} };
    CHECK(chooseService(two, 0xAAAA, "Heart", 3) == 3, "two services share the label: no label match (conservative)");
    CHECK(chooseService(d10, 0xAAAA, "Nation", 0xC0D3) == 0xC0D3, "a partial label is not the same station");
    CHECK(chooseService({ {1, "BB"}, {2, "X"} }, 0xAAAA, "bb", 2) == 2, "a two-letter label proves nothing");
    // 2. the last one played on this multiplex
    CHECK(chooseService(d10, 0xBBBB, "Radio X", 0xC0D3, &why) == 0xC0D3, "the last one played here");
    CHECK(chooseService(d10, 0xBBBB, "Radio X", 0xDEAD) == 0xC0D1, "a remembered SId no longer listed is skipped");
    // 3. the first in the list
    CHECK(chooseService(d10, 0, "", 0) == 0xC0D1, "otherwise the first in the clients' order");
    CHECK(chooseService({}, 0xC0D2, "Nation 80s", 0xC0D3) == 0, "no list yet: no choice (keep waiting, silent)");

    // ── The store ──
    Store s;
    CHECK(s.remember(0xC1B2, "10D", 0xC0D3), "remember a new multiplex");
    CHECK(!s.remember(0xC1B2, "10D", 0xC0D3), "the same again is no change (no save)");
    CHECK(s.lookup(0xC1B2, "10D") == 0xC0D3, "lookup by EId");
    CHECK(s.lookup(0xC1B2, "") == 0xC0D3, "…without the block");
    CHECK(s.lookup(0x9999, "10D") == 0xC0D3, "an unknown EId falls back to the block");
    CHECK(s.lookup(0x9999, "11A") == 0, "nothing for a block never played");
    s.remember(0xC1B2, "10D", 0xC0D1);
    CHECK(s.lookup(0xC1B2, "10D") == 0xC0D1, "a later choice replaces it");
    for (uint32_t i = 0; i < kMaxEntries + 20; ++i) s.remember(0x1000 + i, "5A", 0x2000 + i);
    CHECK(s.entries().size() == kMaxEntries, "bounded");
    CHECK(s.lookup(0xC1B2, "") == 0, "the oldest use went first");
    CHECK(s.lookup(0x1000 + kMaxEntries + 19, "") == 0x2000 + kMaxEntries + 19, "the newest stays");
    // round trip, and a damaged file is not read
    char tmpl[] = "/tmp/vibe-dab-last-XXXXXX";
    const int fd = mkstemp(tmpl); if (fd >= 0) close(fd);
    CHECK(s.save(tmpl), "saved");
    Store t; CHECK(t.load(tmpl), "loaded");
    CHECK(t.serialise() == s.serialise(), "round trip is exact");
    t.remember(0x7777, "12B", 0x8888);
    CHECK(t.lookup(0x7777, "12B") == 0x8888 && t.entries().size() == kMaxEntries, "a loaded store keeps its bound and its order");
    Store u; CHECK(!u.parse("garbage\nL 1 2 3 4\n") && u.entries().empty(), "not ours: not read");
    // ★★ The last BLOCK (vibe_dab_landing.h rule 1) rides in the same file as a "B" line.
    { Store b; CHECK(b.lastBlock().empty(), "no last block to begin with");
      CHECK(b.setLastBlock("10D") && !b.setLastBlock("10D"), "set once; the same again is no change");
      CHECK(!b.setLastBlock("") && !b.setLastBlock("TOOLONG"), "empty or impossible names are refused");
      b.remember(0xC1B2, "10D", 0xC0D3);
      Store c; CHECK(c.parse(b.serialise()) && c.lastBlock() == "10D" && c.lookup(0xC1B2, "") == 0xC0D3, "block and stations round trip");
      Store o; CHECK(o.parse("VIBE-DAB-LAST 1\nL 1 C1B2 10D C0D3\n") && o.lastBlock().empty(), "an older file: no block, entries kept");
      c.clear(); CHECK(c.lastBlock().empty(), "clear() forgets the block too (Start+Select's reset)"); }
    CHECK(!u.parse(std::string("VIBE-DAB-LAST 1\n") + std::string(kMaxFileBytes, 'x')), "too big: not read");
    unlink(tmpl);

    std::printf(fails ? "dab last service: %d FAILED\n" : "dab last service: all passed\n", fails);
    return fails ? 1 : 0;
}
