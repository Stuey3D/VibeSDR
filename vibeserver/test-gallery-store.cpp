// test-gallery-store.cpp — THE GALLERY'S STORE at any size (vibe_gallery.h, 2026-11-01).
//
// ★★★ WHY: the Lenovo's footer said "5000 items on this server · 9.7 MB" whatever the filter — the 5,000-item cap was
//     throwing NAVTEX history away long before the owner's 100 MB. Stuart: "we dont want a hard limit of 5000 items, we
//     want the limit to be the assigned space". What this pins:
//    · PER-KIND COUNTS: gallery.json's "kinds" — each kind's count and bytes — and the filter's total agree with what
//      was written; usedBytes counts the INDEX inside the space.
//    · THE SIZE LIMIT IS THE LIMIT: thousands of items kept under a big limit (no 5,000 cap); under a small one the
//      OLDEST go first, the space (items + index) holds, and each kind keeps its newest kKindFloor (fair eviction).
//    · THE BACKSTOP: lowered for the test, it holds the count and is logged — and it is not the size rule.
//    · APPEND-ONLY: replace / retag append one line (the card where it stands, or moved to the newest); removals append
//      {"x":…}; compaction rewrites the index with only live lines; a SECOND Store on the same folder (another radio's
//      process) sees exactly the same list throughout; a half line left by a crash is skipped, not merged.
#include "vibe_gallery.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& what) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", what.c_str()); if (!c) fails++; }
static std::string tmpDir(const char* tag) {
    char t[256]; std::snprintf(t, sizeof t, "%s/galstore-%s-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp", tag);
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static void rmTree(const std::string& d) { std::string c = "rm -rf '" + d + "'"; if (std::system(c.c_str())) {} }
static long num(const std::string& j, const std::string& k) {
    const size_t p = j.find("\"" + k + "\":"); return p == std::string::npos ? -1L : std::atol(j.c_str() + p + k.size() + 3);
}
/** "n" or "b" of one kind in "kinds". */
static long kindNum(const std::string& j, const std::string& kind, const char* f) {
    const size_t p = j.find("\"" + kind + "\":{\"n\":"); if (p == std::string::npos) return -1;
    const size_t q = j.find(std::string("\"") + f + "\":", p); return std::atol(j.c_str() + q + 4);
}
static size_t countOf(const std::string& s, const std::string& n) { size_t c = 0; for (size_t at = 0; (at = s.find(n, at)) != std::string::npos; at += n.size()) c++; return c; }
static uint64_t fileSize(const std::string& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0; }
static gallery::Meta meta(const char* kind, int i) {
    gallery::Meta m; m.kind = kind; m.origin = "background"; m.freqHz = 518000; m.mode = "usb"; m.decoder = kind;
    m.utc = 1792700000 + i; m.title = std::string(kind) + " " + std::to_string(i); return m;
}

int main() {
    std::printf("test-gallery-store — the size limit governs, per-kind counts, append-only index\n");

    // ── 1. per-kind counts ─────────────────────────────────────────────────────────────────────────────────────────
    {
        std::printf("── per-kind counts and bytes ──\n");
        gallery::Store s; const std::string d = tmpDir("kinds"); s.setDir(d);
        for (int i = 0; i < 30; i++) s.add(meta("navtex", i), "txt", std::string(100, 'n'));
        for (int i = 0; i < 10; i++) s.add(meta("wefax", 100 + i), "png", std::string(1000, 'w'));
        for (int i = 0; i < 5; i++) s.add(meta("ft8", 200 + i), "txt", std::string(10, 'f'));
        const std::string all = s.listJson("", 25, 0), nav = s.listJson("navtex", 25, 0);
        ok(kindNum(all, "navtex", "n") == 30 && kindNum(all, "navtex", "b") == 3000, "NAVTEX: 30 items, 3,000 bytes");
        ok(kindNum(all, "wefax", "n") == 10 && kindNum(all, "wefax", "b") == 10000 && kindNum(all, "ft8", "n") == 5 && kindNum(all, "ft8", "b") == 50,
           "WEFAX 10 / 10,000 bytes, FT8 5 / 50 bytes");
        ok(num(all, "count") == 45 && num(all, "total") == 45 && num(nav, "total") == 30 && num(nav, "count") == 45,
           "the filter's total is its kind's count; count stays the whole gallery");
        const uint64_t idx = fileSize(d + "/index.jsonl");
        ok(num(all, "usedBytes") == (long)(13050 + idx) && num(all, "indexBytes") == (long)idx && s.usedBytes() == 13050 + idx,
           "★ usedBytes = the items + the index (" + std::to_string((unsigned long long)idx) + " bytes of index)");
        ok(num(s.listJson("sstv", 25, 0), "total") == 0 && s.listJson("sstv", 25, 0).find("\"items\":[]") != std::string::npos, "a kind with nothing: total 0");
        ok(all.find("\"re\":") == std::string::npos, "no bookkeeping fields served");
        rmTree(d);
    }

    // ── 2. the size limit governs — no 5,000 cap ───────────────────────────────────────────────────────────────────
    {
        std::printf("── the size limit governs: thousands of small items kept ──\n");
        gallery::Store s; const std::string d = tmpDir("many"); s.setDir(d);   // default 100 MB
        for (int i = 0; i < 6000; i++) s.add(meta("navtex", i), "txt", std::string(200, 'x'));
        ok(s.itemCount() == 6000, "★ 6,000 NAVTEX texts in 100 MB: all 6,000 kept (" + std::to_string(s.itemCount()) + ")");
        const std::string p1 = s.listJson("navtex", 25, 0);
        ok(num(p1, "total") == 6000 && p1.find("\"navtex 5999\"") != std::string::npos && s.listJson("navtex", 25, 5975).find("\"navtex 0\"") != std::string::npos,
           "paged: the newest on page 1, the very first on the last page");
        rmTree(d);
    }

    // ── 3. size-limit eviction order, fair between kinds, space includes the index ──────────────────────────────────
    {
        std::printf("── over the size: the oldest first, each kind keeps its newest %zu ──\n", gallery::Store::kKindFloor);
        gallery::Store s; const std::string d = tmpDir("order"); s.setDir(d); s.setLimitBytes(300 * 1024);
        gallery::Store other; other.setDir(d); other.setLimitBytes(300 * 1024);   // ★ another radio's process
        for (int i = 0; i < 60; i++) s.add(meta("wefax", i), "png", std::string(1000, 'w'));
        for (int i = 0; i < 1500; i++) s.add(meta("navtex", 1000 + i), "txt", std::string(1000, 'n'));
        const std::string j = s.listJson("", 1000, 0);
        ok(s.usedBytes() <= 300 * 1024, "★ the space holds, index included: " + std::to_string((unsigned long long)s.usedBytes()) + " of 307,200");
        ok(kindNum(j, "wefax", "n") == (long)gallery::Store::kKindFloor, "★ a NAVTEX flood leaves WEFAX its newest " + std::to_string(kindNum(j, "wefax", "n")));
        const std::string w = s.listJson("wefax", 100, 0);
        ok(w.find("\"wefax 59\"") != std::string::npos && w.find("\"wefax 10\"") != std::string::npos && w.find("\"wefax 9\"") == std::string::npos,
           "…the NEWEST 50 (10–59); 0–9, the oldest, went");
        const std::string n = s.listJson("navtex", 1000, 0);
        const long kept = num(n, "total");
        ok(kept > 100 && n.find("\"navtex 2499\"") != std::string::npos && n.find("\"navtex " + std::to_string(2500 - kept) + "\"") != std::string::npos
           && n.find("\"navtex " + std::to_string(2499 - kept) + "\"") == std::string::npos,
           "NAVTEX: the newest " + std::to_string(kept) + " kept, everything older gone (oldest first, no gaps)");
        ok(s.compactions() > 0 && fileSize(d + "/index.jsonl") < 200 * 1024, "★ compacted: the removals did not grow the index without end ("
           + std::to_string(s.compactions()) + " compactions, index " + std::to_string((unsigned long long)fileSize(d + "/index.jsonl")) + " bytes)");
        ok(other.listJson("", 1000, 0) == j, "★ another process's Store on the same folder lists exactly the same");
        // a lone wefax burst after the flood: every kind at its floor + something new → still under the space
        for (int i = 0; i < 20; i++) s.add(meta("sstv", 5000 + i), "png", std::string(20000, 's'));
        ok(s.usedBytes() <= 300 * 1024 && s.itemCount() > 0, "big pictures arriving: still inside the space (" + std::to_string((unsigned long long)s.usedBytes()) + ")");
        rmTree(d);
    }

    // ── 4. the backstop ────────────────────────────────────────────────────────────────────────────────────────────
    {
        std::printf("── the backstop (lowered for the test; 1,000,000 in use) ──\n");
        ok(gallery::Store::kMaxItems >= 1000000, "★ the backstop is 1,000,000 — normal use (100 MB ≈ 50,000 NAVTEX) never reaches it");
        gallery::Store s; const std::string d = tmpDir("cap"); s.setDir(d); s.setMaxItems(300);
        for (int i = 0; i < 450; i++) s.add(meta("navtex", i), "txt", "x");
        const std::string j = s.listJson("", 5, 0);
        ok(s.itemCount() == 300 && j.find("\"navtex 449\"") != std::string::npos && s.listJson("", 1, 299).find("\"navtex 150\"") != std::string::npos,
           "past the backstop: 300 kept, the newest; the oldest went (logged above)");
        rmTree(d);
    }

    // ── 5. append-only replace / retag, the restart, a crash's half line ──────────────────────────────────────────
    {
        std::printf("── replace and retag append; the list as before ──\n");
        gallery::Store s; const std::string d = tmpDir("re"); s.setDir(d);
        gallery::Store other; other.setDir(d);
        gallery::Meta card = meta("ft8", 0); card.slot = "last10"; card.title = "card v0";
        const std::string c = s.create(card, "txt", "v0");
        for (int i = 1; i <= 5; i++) s.add(meta("navtex", i), "txt", "n");
        const std::string log = s.add(meta("ism", 9), "txt", "log");
        for (int i = 1; i <= 3; i++) { card.title = "card v" + std::to_string(i); ok(s.replace(c, card, "v" + std::to_string(i), true), "card rewritten (v" + std::to_string(i) + ")"); }
        std::string j = s.listJson("", 10, 0);
        ok(j.find("\"card v3\"") < j.find("\"ism 9\"") && countOf(j, "\"card v") == 1 && num(j, "count") == 7, "★ the card once, as v3, moved to the top");
        ok(countOf(s.indexText(), "\"s\":\"last10\"") == 1, "one card in the (live) index, however often it is rewritten");
        gallery::Meta lm = meta("ism", 9); lm.origin = "both"; lm.title = "ism 9 both";
        ok(s.retag(log, lm), "retagged");
        j = s.listJson("", 10, 0);
        ok(j.find("\"ism 9 both\"") != std::string::npos && j.find("\"ism 9 both\"") > j.find("\"card v3\"") && j.find("\"ism 9 both\"") < j.find("\"navtex 5\""),
           "★ retag keeps its place in the list");
        ok(s.field(log, "o") == "both" && s.field(c, "title") == "card v3", "field() reads the current line");
        std::string p, ct; ok(s.resolve(c, p, ct) && !s.resolve("20991231-000000-ft8-0k0-0000.txt", p, ct), "resolve: listed yes, unknown no");
        ok(other.listJson("", 10, 0) == j, "another process's Store: the same list");
        { gallery::Store fresh; fresh.setDir(d); ok(fresh.adoptSlot(card) == c, "★ after a restart, the card is found again (adoptSlot)"); }
        // a crash left half a line
        { FILE* f = std::fopen((d + "/index.jsonl").c_str(), "a"); std::fputs("{\"f\":\"half", f); std::fclose(f); }
        const std::string after = s.add(meta("navtex", 99), "txt", "n");
        ok(s.listJson("", 3, 0).find("\"navtex 99\"") != std::string::npos && s.itemCount() == 8, "★ a crash's half line is skipped, the next item still lands");
        ok(other.itemCount() == 8, "…and another process agrees");
        // the owner empties the folder
        rmTree(d); ::mkdir(d.c_str(), 0755);
        ok(s.itemCount() == 0 && s.listJson("", 5, 0).find("\"items\":[]") != std::string::npos, "the folder emptied: nothing listed");
        rmTree(d);
    }

    std::printf("%s (%d failed)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails ? 1 : 0;
}
