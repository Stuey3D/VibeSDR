// test-bench-decoders.cpp — the benchmark's decoder rows (vibe_benchmark_decoders.h) and the
// "how many at once" recommendation that becomes the owner's default decoder limit (B6).
//
// ★ Every decoder the server has gets a row with a real, positive cost — a row that reads 0 % is a
//   decoder that never ran (the WEFAX-that-was-never-dispatched shape), and it would recommend an
//   unlimited box. SSTV must actually COMMIT to a picture (its VIS is decoded), or the row measures
//   only the listening loop. The recommendation is checked against the cases Stuart described.
#include "vibe_benchmark_decoders.h"
#include <cstdio>
#include <string>

using namespace vibe::benchdec;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

int main() {
    std::printf("── every decoder, timed on this machine ──\n");
    const int64_t clk = vibe::clockOffsetUs().load();
    const auto rows = runDecoderRows(2.0, nullptr);
    const char* want[] = { "dec_rtty", "dec_navtex", "dec_wefax", "dec_sstv", "dec_time", "dec_ft8" };
    for (const char* id : want) {
        const Row* r = nullptr;
        for (auto& x : rows) if (x.id == id) r = &x;
        ok(r && r->pct > 0.0 && r->pct < 400.0, std::string(id) + (r ? ": " + std::to_string(r->pct).substr(0, 5) + " % of a core — " + r->label : ": missing"));
    }
    for (auto& x : rows) if (x.id == "dec_ft8") ok(x.decoded >= 15, "the FT8 row DECODES its busy band (" + std::to_string(x.decoded) + " of 20), so it times the LDPC work, not just the search");
    for (auto& x : rows) if (x.id == "dec_sstv") ok(x.label.find("listening") == std::string::npos, "SSTV committed to a picture (its VIS decoded), so the row is the decoding cost");
    ok(vibe::clockOffsetUs().load() == clk, "the FT8 row puts the UTC correction back exactly as it found it");

    std::printf("── how many at once ──\n");
    // Stuart's case: a box that carries ten listeners with room for about four decoders more.
    ok(recommendDecoders(4, 6.0, 10, 37.0) == 4, "4 cores, 10 listeners at 6 %, FT8 at 37 %: 4 decoders");
    ok(recommendDecoders(4, 6.0, 0, 37.0) == 5, "...and 5 with nobody listening");
    ok(recommendDecoders(2, 30.0, 10, 50.0) == 1, "an overloaded box still allows ONE, at the lowest priority");
    ok(recommendDecoders(16, 1.0, 10, 2.0) == 20, "a big machine is capped at 20 — past that the link decides");
    ok(recommendDecoders(4, 6.0, 10, 0.0) == 1, "an unmeasured decoder recommends the floor, never 'unlimited'");
    std::printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
