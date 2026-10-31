// test-bench-decoders.cpp — the benchmark's decoder rows (vibe_benchmark_decoders.h, vibe_benchmark_more.h) and the
// "how many at once" recommendation that becomes the owner's default decoder limit (B6).
//
// ★ Every decoder the server has gets a row with a real, positive cost — a row that reads 0 % is a
//   decoder that never ran (the WEFAX-that-was-never-dispatched shape), and it would recommend an
//   unlimited box. SSTV must actually COMMIT to a picture (its VIS is decoded), or the row measures
//   only the listening loop. The recommendation is checked against the cases Stuart described.
// ★★ 2026-10-31 (Stuart: "all these decoders need their own lines in the initial setup benchmark too"): every row after
//    the first eight must DECODE its signal (`decoded` > 0) — a row that decoded nothing timed only the search — except
//    the filters (tone ID counts its lock; noise reduction and the notch decode nothing) and the clip rows, which read
//    "not measured" without decoders-bench.vbu8 (VIBE_BENCH_CLIP=<path> runs them).
// ★ Silent: no audio device is opened anywhere.
#include "vibe_benchmark_decoders.h"
#include "vibe_benchmark_more.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace vibe::benchdec;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

int main() {
    std::printf("── every decoder, timed on this machine ──\n");
    const int64_t clk = vibe::clockOffsetUs().load();
    const double t0 = rowClock();
    auto rows = runDecoderRows(2.0, nullptr);
    const double firstSec = rowClock() - t0;
    const char* want[] = { "dec_rtty", "dec_navtex", "dec_wefax", "dec_sstv", "dec_time", "dec_ft8" };
    for (const char* id : want) {
        const Row* r = nullptr;
        for (auto& x : rows) if (x.id == id) r = &x;
        ok(r && r->pct > 0.0 && r->pct < 400.0, std::string(id) + (r ? ": " + std::to_string(r->pct).substr(0, 5) + " % of a core — " + r->label : ": missing"));
    }
    for (auto& x : rows) if (x.id == "dec_ft8") ok(x.decoded >= 15, "the FT8 row DECODES its busy band (" + std::to_string(x.decoded) + " of 20), so it times the LDPC work, not just the search");
    // ★★ V12: Band spots' own row — it sets Band's default (off where not green), so it must measure a REAL busy band.
    { const Row* b = nullptr; for (auto& x : rows) if (x.id == "dec_ft8band") b = &x;
      ok(b && b->pct > 0.0 && b->pct < 400.0 && b->decoded >= 20, std::string("dec_ft8band: ") + (b ? std::to_string(b->pct).substr(0, 5) + " % of a core, "
         + std::to_string(b->decoded) + " spots decoded — " + b->label : "missing")); }
    // ★★ LoRa (2026-10-31): threaded since the ISM/LoRa split, so scored by its hottest thread, the engine's threads kept
    { const Row* l = nullptr; for (auto& x : rows) if (x.id == "dec_lora") l = &x;
      double proc = -1, lora = 0; int nl = 0;
      if (l) for (const auto& t : l->threads) { if (t.first == "process") proc = t.second; if (t.first.rfind("vibe-lora", 0) == 0) { lora += t.second; ++nl; } }
      ok(l && l->pct > 0.0 && proc >= 0 && l->pct <= proc + 0.5 && !l->hottest.empty(),
         std::string("dec_lora: scored by its hottest thread (") + (l ? l->hottest + " " + std::to_string(l->pct).substr(0, 5) : "missing")
         + " %), process " + std::to_string(proc).substr(0, 5) + " % in the detail");
#if defined(__linux__) || defined(__APPLE__)
      ok(nl == 3, "…and its three engine threads are in the detail (" + std::to_string(nl) + ", " + std::to_string(lora).substr(0, 5)
         + " % together — what budgetFromBench reads)");
#endif
      ok(l && l->decoded >= 1, "dec_lora decodes its default channel's uplink (" + std::to_string(l ? l->decoded : -1) + ")"); }
    for (auto& x : rows) if (x.id == "dec_sstv") ok(x.label.find("listening") == std::string::npos, "SSTV committed to a picture (its VIS decoded), so the row is the decoding cost");
    ok(vibe::clockOffsetUs().load() == clk, "the FT8 row puts the UTC correction back exactly as it found it");
    for (auto& x : rows) ok(x.ms >= 0, x.id + " reports its own wall time (" + std::to_string(x.ms) + " ms)");

    std::printf("── every decoder after the first eight ──\n");
    const char* clipPath = std::getenv("VIBE_BENCH_CLIP");
    const auto clips = loadDecoderClips(clipPath ? clipPath : "");
    std::printf("   clip: %s (%zu records)\n", clipPath ? clipPath : "none", clips.size());
    int counted = 0;
    const auto more = runMoreDecoderRows(clips, [](const std::string& l) { std::fprintf(stderr, "   … %s\n", l.c_str()); });
    runMoreDecoderRows({}, nullptr, &counted);
    ok(counted == (int)more.size() && moreDecoderSteps() == counted, "the progress bar's count (" + std::to_string(counted) + ") is the rows made (" + std::to_string(more.size()) + ")");
    double moreMs = 0;
    std::printf("   %-14s %7s %8s %7s  %s\n", "id", "% core", "decoded", "ms", "label");
    for (const auto& r : more) {
        std::printf("   %-14s %7.2f %8d %7d  %s\n", r.id.c_str(), r.pct, r.decoded, r.ms, r.label.c_str());
        moreMs += r.ms;
    }
    for (const auto& r : more) {
        const bool clipRow = r.id == "dec_cwskim" || r.id == "dec_pskskim" || r.id == "dec_ndb" || r.id == "dec_ism" || r.id == "dec_wmbus";
        const bool filter = r.id == "dec_nr" || r.id == "dec_notch";
        if (clipRow && clips.empty()) {
            ok(r.pct < 0 && r.label.find("no clip") != std::string::npos, r.id + ": no clip → not measured (pct −1, said in its label), never a guess");
            continue;
        }
        ok(r.pct > 0.0 && r.pct < 400.0, r.id + ": " + std::to_string(r.pct).substr(0, 5) + " % of a core");
        if (!filter) ok(r.decoded > 0, r.id + " decoded its signal (" + std::to_string(r.decoded) + ")");
        // ★★ whole-radio rows are scored by their HOTTEST thread (as ADS-B), the process total kept in the detail
        if (r.id == "dec_ais" || r.id == "dec_ism" || r.id == "dec_wmbus" || r.id == "dec_lora_all") {
            double proc = -1; for (const auto& t : r.threads) if (t.first == "process") proc = t.second;
            ok(!r.hottest.empty() && proc >= 0 && r.pct <= proc + 0.5, r.id + ": scored by its hottest thread (" + r.hottest + " "
               + std::to_string(r.pct).substr(0, 5) + " %), process " + std::to_string(proc).substr(0, 5) + " % kept in the detail");
        }
        ok(ruleFor(r.id).sw != nullptr, r.id + " has a defaults rule");
    }
    // every digital-spots mode registered has its own row
    for (const auto& m : vibe::digi::modes()) {
        if (m.name == "FT8" || m.name == "FT4") continue;
        std::string id = "spot_" + m.name; for (auto& c : id) c = (char)std::tolower((unsigned char)c);
        bool have = false; for (const auto& r : more) have |= r.id == id;
        ok(have, "digital-spots mode " + m.name + " has its own row");
    }
    // ★★ "every LoRa channel" (lora+all): its own row, after dec_lora's default channels — and it hears more than they do
    { const Row* a = nullptr; const Row* d = nullptr;
      for (const auto& r : more) if (r.id == "dec_lora_all") a = &r;
      for (const auto& r : rows) if (r.id == "dec_lora") d = &r;
      ok(a && a->label.rfind("Decoder: LoRa (868, every channel)", 0) == 0, "dec_lora_all is a row: " + (a ? a->label : std::string("missing")));
      ok(a && d && a->decoded > d->decoded, "every channel decodes the uplink on a channel the default set leaves out ("
         + std::to_string(a ? a->decoded : -1) + " frames against " + std::to_string(d ? d->decoded : -1) + ")"); }
    std::printf("   first eight rows: %.1f s · the rest: %.1f s · all: %.1f s\n", firstSec, moreMs / 1000.0, firstSec + moreMs / 1000.0);

    std::printf("── the defaults rule ──\n");
    ok(ruleFor("dec_dsc").primary && std::string(ruleFor("dec_dsc").sw) == "dsc", "DSC's row is its switch's main decoder: below green, \"dsc\" starts off");
    ok(!ruleFor("dec_cwskim").primary && std::string(ruleFor("dec_cwskim").sw) == "cw", "the CW skimmer never switches CW off (the one-signal decoder is green)");
    ok(!ruleFor("spot_q65").primary && ruleFor("spot_q65").slot, "a digital-spots mode is a slot, flagged but never switched off");
    ok(std::string(ruleFor("dec_lora_all").sw) == "lora+all" && !ruleFor("dec_lora_all").primary && !ruleFor("dec_lora_all").slot,
       "dec_lora_all names the owner's \"lora+all\", never switches anything off and is no slot (the readers tick it ON when green)");
    ok(!ruleFor("dec_ndb").slot && !ruleFor("dec_lora").slot && !ruleFor("dec_ft8band").slot, "whole-radio modes and Band are not the decoder limit's yardstick");
    // ★ Stuart, 2026-10-31: the first eight rows keep their old behaviour — flagged below green, never switched off.
    for (const char* id : { "dec_rtty", "dec_navtex", "dec_wefax", "dec_sstv", "dec_time", "dec_ft8", "dec_ft8band", "dec_lora" })
        ok(!ruleFor(id).primary, std::string(id) + " is never switched off by the benchmark (flagged only)");

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
