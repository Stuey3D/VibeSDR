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
            /* ★ ONE FIGURE, TWO PATHS (2026-11-04, "if our SNR figures are wrong because we measure them wrong
             *  please fix them"): the same transmitter found by the original test (standard phase) and by
             *  the new path (own phase) reads the same dB — both over the quartile noise. Before, the
             *  original's odd-carrier reference held the TII itself and read ~11 dB low on 12B. */
            TiiDetector own; frames({ {11, 1, 1.0f, true} }, 0.25f, own);
            const auto& h2 = own.hits();
            printf("  same site, two paths: %.1f dB (original test)  %.1f dB (new path)\n", h.empty() ? 0.f : h[0].strength, h2.empty() ? 0.f : h2[0].strength);
            CHECK(!h.empty() && h2.size() == 1 && std::fabs(h[0].strength - h2[0].strength) < 1.0f, "★ the original test and the new path agree on the dB");
            // amplitude 1 per carrier against noise 0.25 per component (power 0.125): the true figure is 9.0 dB
            CHECK(!h.empty() && std::fabs(h[0].strength - 9.03f) < 1.0f, "…and it is the true carrier-to-noise figure, 9 dB");
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
        /* ★★★ "≈ Scalloway, 541 mi" (Stuart, 2026-11-13): a clean Daventry 23/2 and one or two blocks barely lit
         *  beside it. The old resolver named every directory code on Sub 2 that fitted the lit blocks. */
        {
            const uint8_t dav = tiiPattern(23);
            int xb = -1, yb = -1;                       // two blocks Daventry does not use
            for (int b = 0; b < 8; ++b) if (!(dav & (0x80 >> b))) { if (xb < 0) xb = b; else if (yb < 0) yb = b; }
            auto mk = [&](float vx, float vy) {
                TiiShared sc; sc.subId = 2; sc.noise = 1.0f; sc.sigma = 0.25f; sc.alongside = true;
                sc.active = uint8_t(dav | (0x80 >> xb) | (0x80 >> yb));
                for (int b = 0; b < 8; ++b) sc.v[b] = (dav & (0x80 >> b)) ? 2000.0f : 0.0f;
                sc.v[xb] = vx; sc.v[yb] = vy;
                return std::vector<TiiShared>{sc};
            };
            auto clean = [] { std::vector<TiiHit> h(1); h[0].mainId = 23; h[0].subId = 2; h[0].strength = 33.0f; return h; };
            auto everySub2 = [](int, int s) { return s == 2; };          // the UK: Sub 2 on dozens of sites
            // 1.0 and 1.9 dB blocks (1.26 and 1.55 x the noise): not a transmitter
            { auto h = clean(); tiiResolveShared(mk(1.26f, 1.55f), true, everySub2, h);
              CHECK(h.size() == 1 && h[0].mainId == 23, "★ blocks 1–2 dB over the noise beside Daventry name NO extra site"); }
            // one real 10 dB block: ONE site explains it — the nearest of those that fit, not all of them
            { auto h = clean();
              auto km = [&](int m, int) { return m == 59 ? 228.0 : m == 45 ? 870.0 : 40.0 + m; };
              tiiResolveShared(mk(10.0f, 0.0f), true, everySub2, km, h);
              int extra = 0; const TiiHit* x = nullptr;
              for (const auto& t : h) if (!(t.mainId == 23 && t.subId == 2)) { ++extra; x = &t; }
              printf("  Daventry + one 10 dB block: %d extra", extra); if (x) printf(" (main %d, c%d)", x->mainId, x->from); printf("\n");
              int fits = 0, nearest = -1; double nk = 1e9;
              for (int p = 0; p < 70; ++p) { const uint8_t bits = tiiPattern(p);
                  if (p != 23 && (bits & uint8_t(dav | (0x80 >> xb))) == bits) { ++fits; if (km(p, 2) < nk) { nk = km(p, 2); nearest = p; } } }
              CHECK(fits > 1, "…(several codes fit those five blocks)");
              CHECK(extra == 1 && x && x->mainId == nearest && x->from == kTiiResolved, "★ one extra block = ONE site, the nearest that fits, resolved"); }
            // the same with no position to choose by: a tie, so candidates, never a confident name
            { auto h = clean(); tiiResolveShared(mk(10.0f, 0.0f), true, everySub2, h);
              bool allCand = h.size() > 1; for (size_t i = 1; i < h.size(); ++i) if (h[i].from != kTiiCandidate) allCand = false;
              CHECK(h[0].mainId == 23 && h[0].from == kTiiClean && allCand, "★ equally near (no position): candidates, not resolved"); }
        }
        // ★ The Wellingborough/Aylesbury collision with distances: still both (the only cover of five blocks).
        {
            TiiDetector det; frames({ {23, 6, 1.0f, true}, {22, 6, 0.8f, true} }, 0.05f, det);
            std::vector<TiiHit> h = det.hits();
            tiiResolveShared(det.shared(), true, [](int m, int s) { return s == 6 && (m == 23 || m == 22 || m == 40); },
                             [](int m, int) { return m == 22 ? 60.0 : 20.0; }, h);
            CHECK(h.size() == 2 && find(h, 23, 6) && find(h, 22, 6) && find(h, 22, 6)->from == kTiiResolved,
                  "★ a real two-site collision still resolves both, with distances given");
        }
        /* ★★★ "that list is constantly growing" (7D, 2026-11-13): a code is reported only once it is in 2 of the last 3
         *  verdicts — a one-verdict ghost never reaches the panels, which remember every code they are shown. */
        {
            auto H = [](int m, int s, float db) { TiiHit h; h.mainId = m; h.subId = s; h.strength = db; return h; };
            TiiConfirm cf;
            cf.verdict(1, {H(1, 5, 32.0f), H(44, 12, 4.0f)});
            CHECK(cf.confirmed().empty(), "the first verdict alone names nothing");
            cf.verdict(2, {H(1, 5, 32.5f), H(44, 13, 4.1f)});
            CHECK(cf.confirmed().size() == 1 && cf.confirmed()[0].mainId == 1 && cf.confirmed()[0].subId == 5,
                  "★ the real site (in both verdicts) is named; 2C/0C and 2C/0D (one verdict each) are not");
            CHECK(std::fabs(cf.confirmed()[0].strength - 32.5f) < 0.01f, "…at the newest verdict's strength");
            cf.verdict(3, {H(1, 5, 32.6f), H(44, 14, 4.0f), H(44, 12, 4.2f)});
            bool ghost = false; for (const auto& h : cf.confirmed()) if (h.mainId == 44 && h.subId != 12) ghost = true;
            CHECK(!ghost && cf.confirmed().size() == 2, "a ghost that came back once (2C/0C in verdicts 1 and 3) counts; one-offs never do");
            cf.verdict(4, {H(1, 5, 32.0f)});
            cf.verdict(5, {H(1, 5, 32.0f)});
            CHECK(cf.confirmed().size() == 1, "a code no longer in two of the last three verdicts drops out");
            // a weak site missing one verdict in three stays on the panel throughout
            TiiConfirm fl;
            bool steady = true;
            for (unsigned n = 1; n <= 9; ++n) {
                fl.verdict(n, n % 3 ? std::vector<TiiHit>{H(22, 9, 6.0f)} : std::vector<TiiHit>{});
                if (n >= 2 && fl.confirmed().size() != 1) steady = false;
            }
            CHECK(steady, "a weak site in two verdicts of every three stays named through the one it misses (no flicker)");
            // a new block (the count starts again) forgets the old block's history
            cf.verdict(1, {H(1, 5, 30.0f)});
            CHECK(cf.confirmed().empty(), "a new block starts the history again");
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
