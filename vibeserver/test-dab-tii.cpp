// test-dab-tii.cpp — the TII pattern table, the carrier layout, and a detector run on a
// synthesised null symbol (EN 300 401 V2.2.1 clause 14.8, table 26, figure 64).
#include "vibe_dab_tii.h"
#include "vibe_dab_ofdm.h"
#include "vibe_dab_prs.h"
#include <cstdio>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)
static uint32_t seed = 777; static uint32_t rnd() { seed = seed*1664525u+1013904223u; return seed; }
static float gauss() { double s = 0; for (int i = 0; i < 12; ++i) s += (rnd() & 0xFFFF) / 65536.0; return float(s - 6.0); }

int main() {
    printf("test-dab-tii\n");
    // ── table 26, spot-checked against the printed table ────────────────────
    CHECK(tiiPattern(0)  == 0x0F, "p=0  is 0000 1111");
    CHECK(tiiPattern(11) == 0x36, "p=11 is 0011 0110 (figure 64's example)");
    CHECK(tiiPattern(34) == 0x78, "p=34 is 0111 1000");
    CHECK(tiiPattern(47) == 0xA6, "p=47 is 1010 0110");
    CHECK(tiiPattern(69) == 0xF0, "p=69 is 1111 0000");
    CHECK(tiiPatternIndex(0x36) == 11 && tiiPatternIndex(0x37) == -1, "pattern lookup and its refusal of five ones");
    // ── figure 64: c = 1, p = 11 lights -670, -622, -526, -478 in the first section, 99 in the third
    CHECK(tiiPairCarrier(0, 1, 2) == -670 && tiiPairCarrier(0, 1, 3) == -622, "first-section pairs of figure 64");
    CHECK(tiiPairCarrier(0, 1, 5) == -526 && tiiPairCarrier(0, 1, 6) == -478, "…and the other two");
    CHECK(tiiPairCarrier(2, 1, 2) == 99 && tiiPairCarrier(3, 1, 2) == 483, "third and fourth sections");

    // ── a synthesised TII null symbol: transmitter (p=11, c=1) plus a weaker (p=40, c=17) ──
    {
        const int K = 1536;
        std::vector<C32> prs{}; prs.resize(size_t(K)); prsSymbol(prs.data());
        auto build = [&](float noiseAmp) {
            std::vector<TiiC32> z(size_t(K), TiiC32{});
            auto put = [&](int p, int c, float amp) {
                const uint8_t bits = tiiPattern(p);
                for (int s = 0; s < 4; ++s) for (int b = 0; b < 8; ++b) {
                    if (!(bits & (0x80 >> b))) continue;
                    const int k = tiiPairCarrier(s, c, b);
                    const TiiC32 ph(prs[size_t(tiiCarrierPos(k))].real(), prs[size_t(tiiCarrierPos(k))].imag());
                    z[size_t(tiiCarrierPos(k))]     += amp * ph;
                    z[size_t(tiiCarrierPos(k + 1))] += amp * ph;   // the pair shares phi_k
                }
            };
            put(11, 1, 1.0f);
            put(40, 17, 0.5f);
            for (auto& v : z) v += TiiC32(noiseAmp * gauss(), noiseAmp * gauss());
            return z;
        };
        TiiDetector det;
        for (int f = 0; f < TiiDetector::kFramesPerVerdict; ++f) { auto z = build(0.25f); det.feed(z.data(), K); }
        const auto& h = det.hits();
        bool sawMain = false, sawWeak = false;
        for (const auto& x : h) { if (x.mainId == 11 && x.subId == 1) sawMain = true; if (x.mainId == 40 && x.subId == 17) sawWeak = true; }
        printf("  hits: %zu", h.size());
        for (const auto& x : h) printf("  (main %d sub %d %.1f dB)", x.mainId, x.subId, x.strength);
        printf("\n");
        CHECK(sawMain, "the strong transmitter (main 11, sub 1) is identified");
        CHECK(sawWeak, "the weaker one (main 40, sub 17) is identified too");
        CHECK(!h.empty() && h.front().mainId == 11, "hits are ordered strongest first");
        CHECK(h.size() <= 3, "no phantom transmitters on a two-transmitter null");
        // noise alone must yield nothing
        TiiDetector quiet;
        for (int f = 0; f < TiiDetector::kFramesPerVerdict; ++f) {
            std::vector<TiiC32> z{}; z.resize(size_t(K)); for (auto& v : z) v = TiiC32(0.3f * gauss(), 0.3f * gauss());
            quiet.feed(z.data(), K);
        }
        CHECK(quiet.hits().empty(), "★ noise alone identifies no transmitter");
        CHECK(quiet.shared().empty(), "★ …and offers no shared comb to resolve");
    }

    /* ── 2026-11-04: the 11D / Beds-Bucks-Herts cases (vibe_dab_tii.h, the top). A synthetic null where
     *    each transmitter may radiate the pair's second carrier with ITS OWN PRS phase (Daventry, Bow
     *    Brickhill, Sandy Heath as measured) and two may share a Sub Id with different Main Ids. ── */
    {
        const int K = 1536;
        std::vector<C32> prs{}; prs.resize(size_t(K)); prsSymbol(prs.data());
        struct Tx { int p, c; float amp; bool own; };
        auto frames = [&](const std::vector<Tx>& txs, float noiseAmp, TiiDetector& det) {
            for (int f = 0; f < TiiDetector::kFramesPerVerdict; ++f) {
                std::vector<TiiC32> z(size_t(K), TiiC32{});
                for (const Tx& t : txs) {
                    const uint8_t bits = tiiPattern(t.p);
                    for (int s = 0; s < 4; ++s) for (int b = 0; b < 8; ++b) {
                        if (!(bits & (0x80 >> b))) continue;
                        const int k = tiiPairCarrier(s, t.c, b);
                        const C32 a = prs[size_t(tiiCarrierPos(k))], a1 = prs[size_t(tiiCarrierPos(k + 1))];
                        z[size_t(tiiCarrierPos(k))]     += t.amp * TiiC32(a.real(), a.imag());
                        z[size_t(tiiCarrierPos(k + 1))] += t.amp * (t.own ? TiiC32(a1.real(), a1.imag()) : TiiC32(a.real(), a.imag()));
                    }
                }
                for (auto& v : z) v += TiiC32(noiseAmp * gauss(), noiseAmp * gauss());
                det.feed(z.data(), K);
            }
        };
        auto find = [](const std::vector<TiiHit>& h, int m, int s) { for (const auto& x : h) if (x.mainId == m && x.subId == s) return &x; return (const TiiHit*)nullptr; };

        // ★ A single transmitter on the standard convention: exactly the original answer — one clean hit,
        //   the original figure (mean of four over the odd-carrier reference), nothing shared.
        {
            TiiDetector det; frames({ {11, 1, 1.0f, false} }, 0.25f, det);
            const auto& h = det.hits();
            CHECK(h.size() == 1 && h[0].mainId == 11 && h[0].subId == 1 && h[0].from == kTiiClean, "one standard transmitter: one clean hit");
            CHECK(det.shared().empty(), "…and no shared comb");
            CHECK(det.diag().active == 4 && det.diag().comb == 1, "Diag: the comb, four blocks lit");
        }
        // ★ The 11D fault: each carrier with its own phase. The original test rejects it; the new one names it.
        {
            TiiDetector det; frames({ {23, 2, 1.0f, true}, {22, 9, 0.3f, true} }, 0.05f, det);
            const auto& h = det.hits();
            printf("  own-phase hits:"); for (const auto& x : h) printf("  (main %d sub %d %.1f dB)", x.mainId, x.subId, x.strength); printf("\n");
            CHECK(find(h, 23, 2) != nullptr, "★ own-phase transmitter (Daventry's 23/2) identified");
            CHECK(find(h, 22, 9) != nullptr, "★ …and a weaker own-phase one (Bow Brickhill's 22/9)");
            CHECK(h.size() == 2, "…and nothing else — no leakage named as a site");
        }
        // ★ A collision: Wellingborough 23/6 and Aylesbury 22/6 — one Sub Id, two Main Ids, five blocks lit.
        {
            TiiDetector det; frames({ {23, 6, 1.0f, true}, {22, 6, 0.8f, true} }, 0.05f, det);
            CHECK(det.hits().empty(), "a collision is not a clean 4-of-8 comb");
            CHECK(det.shared().size() == 1 && det.shared()[0].subId == 6
                  && __builtin_popcount(det.shared()[0].active) == 5, "★ it is handed out as a shared comb, five blocks lit");
            CHECK(det.diag().active == 5, "Diag counts the five");
            // with a directory that knows both sites (and one more on Sub 6 that is NOT lit)
            auto known = [](int m, int s) { return s == 6 && (m == 23 || m == 22 || m == 40); };
            std::vector<TiiHit> withDir = det.hits();
            tiiResolveShared(det.shared(), true, known, withDir);
            printf("  resolved:"); for (const auto& x : withDir) printf("  (main %d sub %d %.1f dB c%d)", x.mainId, x.subId, x.strength, x.from); printf("\n");
            CHECK(withDir.size() == 2 && find(withDir, 23, 6) && find(withDir, 22, 6), "★ the directory resolves BOTH sites, and only those");
            CHECK(find(withDir, 23, 6) && find(withDir, 23, 6)->from == kTiiResolved, "…marked resolved-from-collision");
            CHECK(find(withDir, 23, 6) && find(withDir, 22, 6) && find(withDir, 23, 6)->strength > find(withDir, 22, 6)->strength,
                  "…the stronger first by its weakest block");
            // without a directory: every pattern of four lit blocks, as candidates — never one confident name
            std::vector<TiiHit> noDir = det.hits();
            tiiResolveShared(det.shared(), false, [](int, int) { return false; }, noDir);
            bool allCand = !noDir.empty();
            for (const auto& x : noDir) if (x.from != kTiiCandidate || x.subId != 6) allCand = false;
            CHECK(noDir.size() == 5 && allCand, "★ no directory: the five fitting patterns, each a CANDIDATE");
            CHECK(find(noDir, 23, 6) && find(noDir, 22, 6), "…the two real ones among them");
            // a directory with codes for the ensemble, none of them on this comb: nothing invented
            std::vector<TiiHit> none = det.hits();
            tiiResolveShared(det.shared(), true, [](int m, int s) { return m == 1 && s == 2; }, none);
            CHECK(none.empty(), "a directory that holds neither code names nothing");
        }
        // ★ Cap: candidates never push a measured hit out.
        {
            std::vector<TiiShared> sh(1); sh[0].subId = 4; sh[0].active = 0xFF; sh[0].noise = 1.0f;
            for (int b = 0; b < 8; ++b) sh[0].v[b] = 100.0f;
            std::vector<TiiHit> h(1); h[0].mainId = 3; h[0].subId = 9; h[0].strength = 3.0f;
            tiiResolveShared(sh, false, [](int, int) { return false; }, h);
            CHECK(h.size() == 8 && h[0].mainId == 3 && h[0].from == kTiiClean, "capped at eight, the measured hit first");
        }
    }
    if (fails == 0) printf("  all passed\n"); else printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
