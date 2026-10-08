// ★★★ A PS IS SHOWN WHOLE, NEVER HALF OLD AND HALF NEW (2026-10-08) — RdsDecoder::psSegment (vibedsp/rds.cpp).
//
// Stuart, on Chicopee's 99.3 (WLZX "Lazer"): the display read "LAZER WLZX-FM NIR FM NNIRV M NIRVANA IIN IN BBLOO FMM
// NIRV VANAA …". The station flips its 8-character PS word by word; PS arrives two characters per group, so for most
// of a pass the buffer held the old word's tail under the new word's head — and the server sampled that buffer once a
// second and sent the mixture on. Real RDS groups, check words and all, go in at the bit level here:
//   1. a word-by-word marquee whose word changes MID-PASS → only whole words ever come out;
//   2. a fast scroller (every segment new on every pass) → every frame comes out, none frozen, none mixed;
//   3. a fixed name → it fills in as heard on first acquisition, then holds.
// ★ SILENT, pure: no radio, no network.
#include "vibedsp/vibedsp.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using vibedsp::RdsDecoder;

static int fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("   %s   %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) fails++;
}

static std::vector<std::string> got;
static void onPs(void*, uint16_t, const char* ps8) { got.emplace_back(ps8); }

static void pushBlock(RdsDecoder& d, uint16_t data, int off) {
    const uint32_t blk = ((uint32_t)data << 10) | (uint16_t)(RdsDecoder::checkword(data) ^ RdsDecoder::OFFSET[off]);
    for (int i = 25; i >= 0; --i) d.pushBit((blk >> i) & 1);
}
static const uint16_t PI = 0x7A1B;   // a US PI (WLZX's own is not needed: the name path is PI-agnostic)
static void group0A(RdsDecoder& d, int addr, const std::string& ps8) {
    const uint16_t b = (uint16_t)((0 << 12) | (0 << 11) | (10 << 5) | (1 << 3) | addr);   // 0A, PTY 10, MS, segment
    const uint16_t dd = (uint16_t)(((uint8_t)ps8[addr * 2] << 8) | (uint8_t)ps8[addr * 2 + 1]);
    pushBlock(d, PI, 0); pushBlock(d, b, 1); pushBlock(d, 0xCDCD, 2); pushBlock(d, dd, 4);
}
/** Other traffic between PS groups, as on air (2A RadioText). */
static void group2A(RdsDecoder& d) {
    pushBlock(d, PI, 0); pushBlock(d, (uint16_t)((2 << 12) | (10 << 5)), 1); pushBlock(d, 0x2020, 2); pushBlock(d, 0x2020, 4);
}
static std::string pad8(std::string s) { s.resize(8, ' '); return s; }
static std::string trimR(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back(); return s; }

/** Send `frames`, each held for `passes` passes; frame k switches to k+1 after `cutAt` segments of the last pass. */
static void run(RdsDecoder& d, const std::vector<std::string>& frames, int passes, int cutAt) {
    for (size_t k = 0; k < frames.size(); ++k) {
        for (int p = 0; p < passes; ++p)
            for (int a = 0; a < 4; ++a) {
                // the last pass of frame k: segments from cutAt on already carry frame k+1 (the encoder changed mid-pass)
                const bool next = p == passes - 1 && a >= cutAt && k + 1 < frames.size();
                group0A(d, a, pad8(next ? frames[k + 1] : frames[k]));
                group2A(d);
            }
    }
}

int main() {
    RdsDecoder::Callbacks cb; cb.ps = onPs;

    {   // ── 1. word by word, the change landing mid-pass ──
        RdsDecoder d; d.setCallbacks(cb); got.clear();
        for (int i = 0; i < 12; ++i) group2A(d);                 // acquire block sync first
        const std::vector<std::string> words = { "LAZER", "WLZX-FM", "NIRVANA", "IN", "BLOOM", "LAZER", "99.3" };
        run(d, words, 6, 2);
        std::set<std::string> ok(words.begin(), words.end());
        std::set<std::string> seen; std::vector<std::string> bad;
        bool committed = false;
        for (auto& g : got) {
            const std::string t = trimR(g);
            if (ok.count(t)) { committed = true; seen.insert(t); continue; }
            if (committed) bad.push_back(t);                     // before the first whole PS a partial fill is allowed
        }
        std::string badList; for (auto& b : bad) badList += " \"" + b + "\"";
        check(bad.empty(), "a word changed mid-pass never comes out half old, half new" + (bad.empty() ? std::string() : ":" + badList));
        check(seen.size() == ok.size(), "…and every word comes out (" + std::to_string(seen.size()) + " of " + std::to_string(ok.size()) + ")");
    }
    {   // ── 2. a fast character scroller: every frame new on every pass ──
        RdsDecoder d; d.setCallbacks(cb); got.clear();
        for (int i = 0; i < 12; ++i) group2A(d);
        const std::string s = "LAZER 99.3 PLAYING NIRVANA ";
        std::vector<std::string> frames;
        for (int i = 0; i < 20; ++i) frames.push_back((s + s).substr(i % s.size(), 8));
        run(d, frames, 1, 4);
        std::set<std::string> ok; for (auto& f : frames) ok.insert(trimR(f));
        std::set<std::string> seen; int bad = 0;
        for (auto& g : got) { const std::string t = trimR(g); if (ok.count(t)) seen.insert(t); else if (!seen.empty()) bad++; }
        check(bad == 0, "a fast scroller is never mixed (" + std::to_string(bad) + " mixtures)");
        check(seen.size() >= frames.size() - 1, "…and it does not freeze: " + std::to_string(seen.size()) + " of " + std::to_string(frames.size()) + " frames shown");
    }
    {   // ── 3. a fixed name ──
        RdsDecoder d; d.setCallbacks(cb); got.clear();
        for (int i = 0; i < 12; ++i) group2A(d);
        for (int a = 0; a < 2; ++a) { group0A(d, a, "BBC R4  "); group2A(d); }
        check(!got.empty() && trimR(got.back()) == "BBC", "first acquisition fills in as heard (\"" + (got.empty() ? std::string() : trimR(got.back())) + "\" after two segments)");
        run(d, { "BBC R4" }, 4, 4);
        check(!got.empty() && trimR(got.back()) == "BBC R4", "a fixed name settles and holds: \"" + (got.empty() ? std::string() : trimR(got.back())) + "\"");
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall good\n", fails);
    return fails ? 1 : 0;
}
