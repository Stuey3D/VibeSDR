// test-dab-receiver.cpp — the whole chain, driven by a SYNTHESISED Mode I transmission.
//
// ★★★ THIS IS THE CLOSEST THING TO A REAL SIGNAL THAT EXISTS WITHOUT A RADIO. It builds an actual
//     OFDM waveform — null symbol, phase reference, 76 symbols of DQPSK across 1536 carriers with
//     a proper cyclic prefix — and feeds it in as IQ. It exercises the sync, the offset estimator,
//     the transform, the carrier mapping and the frequency deinterleaver in the same order and
//     with the same state a live receiver would.
//
// ★★ What it does NOT prove: multipath, an AGC moving under it, a real transmitter's spectral
//    mask, or anything about the MSC audio path. AGENTS.md's rule stands — measure on the live
//    radio before believing any of it. What this buys is that the FIRST live test starts from a
//    chain whose arithmetic is already known good.
#include "vibe_dab_receiver.h"
#include <cstdio>
#include <cmath>
#include <vector>

using namespace vibedab;
static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("  FAIL: %s\n", msg); ++fails; } } while (0)
static uint32_t seed = 8080; static uint32_t rnd() { seed = seed*1664525u+1013904223u; return seed; }

/** Build one Mode I frame at the canonical rate: null, phase reference, then `data` symbols. */
static std::vector<Cplx> makeFrame(const std::vector<std::vector<C32>>& symbols) {
    const Mode& m = modeI();
    const int N = m.usefulSamples, G = m.guardSamples, K = m.carriers;
    std::vector<Cplx> out;
    out.reserve(size_t(m.frameSamples));
    for (int i = 0; i < m.nullSamples; ++i) out.push_back({0.0f, 0.0f});   // the null symbol

    for (const auto& carriers : symbols) {
        // Inverse DFT of the carrier set onto N samples.
        std::vector<Cplx> useful(size_t(N), Cplx{});
        for (int t = 0; t < N; ++t) {
            double re = 0, im = 0;
            int j = 0;
            for (int k = -K/2; k <= K/2; ++k) {
                if (k == 0) continue;
                const C32 c = carriers[size_t(j++)];
                const double a = 2.0 * M_PI * double(k) * double(t) / double(N);
                const double cs = std::cos(a), sn = std::sin(a);
                re += double(c.real()) * cs - double(c.imag()) * sn;
                im += double(c.real()) * sn + double(c.imag()) * cs;
            }
            useful[size_t(t)] = { float(re / N), float(im / N) };
        }
        for (int i = 0; i < G; ++i) out.push_back(useful[size_t(N - G + i)]);   // cyclic prefix
        for (int i = 0; i < N; ++i) out.push_back(useful[size_t(i)]);
    }
    return out;
}

int main() {
    printf("test-dab-receiver\n");
    const Mode& m = modeI();
    const int K = m.carriers;

    // ── the receiver must not lock onto noise ───────────────────────────────
    {
        DabReceiver rx;
        std::vector<Cplx> noise(size_t(m.frameSamples) + 4096);
        for (auto& c : noise) { c.re = float(int(rnd() % 200) - 100) / 100.0f;
                                c.im = float(int(rnd() % 200) - 100) / 100.0f; }
        rx.push(noise.data(), noise.size());
        CHECK(!rx.stats().locked, "flat noise does not produce a frame lock");
    }

    // ── ★★★ A SYNTHESISED TRANSMISSION, SYNCHRONISED AND MEASURED ───────────
    {
        // Symbol 0 is the phase reference; the rest carry arbitrary DQPSK.
        std::vector<std::vector<C32>> syms;
        std::vector<C32> ref(size_t(K), C32{});
        prsSymbol(ref.data());
        syms.push_back(ref);
        std::vector<C32> prev = ref;
        for (int s = 1; s < m.symbolsPerFrame; ++s) {
            std::vector<C32> cur(size_t(K), C32{});
            for (int k = 0; k < K; ++k) {
                // Differential: rotate the previous carrier by one of four phases.
                static const C32 rot[4] = { {0.7071f,0.7071f}, {-0.7071f,0.7071f},
                                            {-0.7071f,-0.7071f}, {0.7071f,-0.7071f} };
                cur[size_t(k)] = prev[size_t(k)] * rot[rnd() & 3];
            }
            syms.push_back(cur);
            prev = cur;
        }

        auto frame = makeFrame(syms);
        CHECK(frame.size() == size_t(m.frameSamples), "a synthesised frame is 196 608 samples");

        // Pad so the acquisition has a whole frame plus a null to search.
        std::vector<Cplx> air(frame);
        air.insert(air.end(), frame.begin(), frame.begin() + long(m.nullSamples) + 64);

        DabReceiver rx;
        const bool got = rx.push(air.data(), air.size());
        CHECK(got, "★ the receiver locks and decodes a synthesised Mode I frame");
        CHECK(rx.stats().locked, "and reports itself locked");
        // ★ The null is genuinely empty here, so the depth should be enormous — the honest
        //   "is this a real signal" number.
        char msg[96];
        snprintf(msg, sizeof msg, "null depth %.0f dB on a clean signal", rx.stats().nullDepthDb);
        CHECK(rx.stats().nullDepthDb > 20.0f, msg);
        snprintf(msg, sizeof msg, "no frequency offset on a clean signal (%.1f Hz)", rx.stats().freqOffsetHz);
        CHECK(std::fabs(rx.stats().freqOffsetHz) < 5.0f, msg);
    }

    // ── ★★ AND IT MUST MEASURE AN OFFSET THAT IS ACTUALLY THERE ─────────────
    {
        std::vector<std::vector<C32>> syms;
        std::vector<C32> ref(size_t(K), C32{}); prsSymbol(ref.data()); syms.push_back(ref);
        std::vector<C32> prev = ref;
        for (int s = 1; s < m.symbolsPerFrame; ++s) { syms.push_back(prev); }
        auto frame = makeFrame(syms);
        std::vector<Cplx> air(frame);
        air.insert(air.end(), frame.begin(), frame.begin() + long(m.nullSamples) + 64);
        // Apply +200 Hz. One carrier spacing is 1 kHz, so this is 0.2 spacings.
        derotate(air.data(), air.size(), -200.0 / double(kCanonicalRateHz));

        DabReceiver rx;
        rx.push(air.data(), air.size());
        char msg[96];
        snprintf(msg, sizeof msg, "a 200 Hz offset is measured as %.1f Hz", rx.stats().freqOffsetHz);
        CHECK(std::fabs(rx.stats().freqOffsetHz - 200.0f) < 25.0f, msg);
        // ★ and reported in ppm, which is what tells a user their dongle is drifting rather than
        //   the transmitter — 200 Hz at 222.064 MHz is about 0.9 ppm.
        snprintf(msg, sizeof msg, "and as %.2f ppm against 11D", rx.stats().freqOffsetPpm);
        CHECK(std::fabs(rx.stats().freqOffsetPpm - 0.9f) < 0.3f, msg);
    }

    if (fails == 0) printf("  all passed\n");
    else            printf("  %d FAILED\n", fails);
    return fails ? 1 : 0;
}
