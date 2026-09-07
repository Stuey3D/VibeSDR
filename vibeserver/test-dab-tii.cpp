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
    }
    if (fails == 0) printf("  all passed\n"); else printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
