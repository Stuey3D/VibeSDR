// test-dab-ofdm.cpp — OFDM offset estimation, de-rotation, DQPSK demapping, carrier mapping.
#include "vibe_dab_ofdm.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)

static uint32_t seed = 99;
static float rnd() { seed = seed * 1664525u + 1013904223u; return float(int32_t(seed) >> 8) / 8388608.0f; }

/** Build `symbols` OFDM symbols with a proper cyclic prefix, optionally with a frequency offset
 *  applied, so the estimator has something real to find. */
static std::vector<Cplx> makeSymbols(const Mode& m, int symbols, double fracOffset) {
    const size_t U = size_t(m.usefulSamples), G = size_t(m.guardSamples), S = U + G;
    std::vector<Cplx> v(S * size_t(symbols));
    for (int s = 0; s < symbols; ++s) {
        std::vector<Cplx> useful(U);
        for (size_t i = 0; i < U; ++i) { useful[i].re = rnd(); useful[i].im = rnd(); }
        Cplx* p = v.data() + size_t(s) * S;
        for (size_t i = 0; i < G; ++i) p[i] = useful[U - G + i];   // prefix = tail of the useful part
        for (size_t i = 0; i < U; ++i) p[G + i] = useful[i];
    }
    if (fracOffset != 0.0) {
        // A frequency offset of `fracOffset` carrier spacings = fracOffset/U cycles per sample.
        derotate(v.data(), v.size(), -fracOffset / double(U));
    }
    return v;
}

int main() {
    printf("test-dab-ofdm\n");
    const Mode& m = modeI();

    // ── the cyclic-prefix estimator finds the offset it was given ────────────
    for (double want : { 0.0, 0.25, -0.3, 0.45, -0.45 }) {
        auto sig = makeSymbols(m, 10, want);
        const float got = fractionalOffset(sig.data(), sig.size(), m, 8);
        char msg[96];
        snprintf(msg, sizeof msg, "fractional offset %.2f recovered (got %.3f)", want, got);
        CHECK(std::fabs(got - float(want)) < 0.02f, msg);
    }

    // ★ and it reports in Hz for the panel: half a spacing is 500 Hz in Mode I.
    CHECK(std::fabs(offsetHz(0.5f, m) - 500.0f) < 0.01f, "0.5 spacings = 500 Hz in Mode I");

    // ── de-rotation is the inverse of the offset ─────────────────────────────
    {
        auto clean  = makeSymbols(m, 4, 0.0);
        auto offset = clean;
        derotate(offset.data(), offset.size(), 0.3 / double(m.usefulSamples));
        derotate(offset.data(), offset.size(), -0.3 / double(m.usefulSamples));
        double worst = 0;
        for (size_t i = 0; i < clean.size(); ++i)
            worst = std::max(worst, double(std::hypot(clean[i].re - offset[i].re,
                                                      clean[i].im - offset[i].im)));
        CHECK(worst < 1e-3, "rotate then unrotate returns the original");
    }

    // ── DQPSK: all four symbols, and Gray adjacency ──────────────────────────
    {
        const C32 prev(1.0f, 0.0f);
        // +45 deg => both soft bits positive; +135 => (-,+); etc.
        struct { double deg; int s0; int s1; } cases[] = {
            {  45,  1,  1 }, { 135, -1,  1 }, { 225, -1, -1 }, { 315,  1, -1 },
        };
        for (auto& c : cases) {
            const double r = c.deg * M_PI / 180.0;
            SoftBits b = dqpskSoft(C32(float(std::cos(r)), float(std::sin(r))), prev);
            char msg[80];
            snprintf(msg, sizeof msg, "%.0f deg -> signs (%d,%d)", c.deg, c.s0, c.s1);
            CHECK((b.b0 > 0) == (c.s0 > 0) && (b.b1 > 0) == (c.s1 > 0), msg);
        }
        // ★★ A NOISY POINT NEAR A BOUNDARY MUST BE SOFT, NOT CONFIDENT. That is the whole value of
        //    soft decisions to the Viterbi decoder downstream.
        SoftBits edge = dqpskSoft(C32(1.0f, 0.02f), prev);   // barely above the axis
        CHECK(std::abs(int(edge.b1)) < 30, "a point near the boundary yields a WEAK bit");
        SoftBits sure = dqpskSoft(C32(0.707f, 0.707f), prev);
        CHECK(std::abs(int(sure.b0)) > 80 && std::abs(int(sure.b1)) > 80, "a clean point is confident");
        // Absolute phase must not matter — that is why DAB is differential.
        SoftBits a = dqpskSoft(C32(0.707f, 0.707f), C32(1.0f, 0.0f));
        SoftBits b = dqpskSoft(C32(-0.707f, 0.707f), C32(0.0f, 1.0f));   // both rotated 90 deg
        CHECK(a.b0 == b.b0 && a.b1 == b.b1, "a constant phase rotation cancels — differential");
    }

    // ── carrier <-> bin mapping ──────────────────────────────────────────────
    {
        CHECK(binForCarrier(1, 2048) == 1, "+1 is bin 1");
        CHECK(binForCarrier(-1, 2048) == 2047, "-1 is the top bin");
        CHECK(binForCarrier(-768, 2048) == 1280, "-768 wraps correctly");
        std::vector<C32> spec(2048);
        for (int i = 0; i < 2048; ++i) spec[i] = C32(float(i), 0.0f);
        std::vector<C32> out(1536);
        carriersFromFft(spec.data(), 2048, 1536, out.data());
        // ★ DC must be skipped: the first carrier is -768 and the middle jumps -1 -> +1.
        CHECK(out[0] == C32(1280.0f, 0.0f), "first carrier is -768");
        CHECK(out[767] == C32(2047.0f, 0.0f), "last negative carrier is -1");
        CHECK(out[768] == C32(1.0f, 0.0f), "first positive carrier is +1, DC skipped");
        CHECK(out[1535] == C32(768.0f, 0.0f), "last carrier is +768");
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
