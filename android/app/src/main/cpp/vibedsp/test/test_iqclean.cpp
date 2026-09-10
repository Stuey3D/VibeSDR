// VibeSDR — IqCleaner and ImpulseBlanker tests (host only).
//   A tone at +50 kHz with a DC offset and a 1 dB / 5° I/Q mismatch: after convergence the DC
//   must be gone (< -60 dB of the tone) and the image at -50 kHz suppressed by > 30 dB more than
//   it was. Rotation by +15 kHz must move the tone to +65 kHz to within one bin. The blanker must
//   remove a 5-sample impulse 30 dB over a noise floor and leave a steady tone untouched.
#include "vibedsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <random>
using namespace vibedsp;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static float binPowerDb(const std::vector<cf32>& x, double rate, double hz) {
    // Direct DFT at one frequency (Hann), enough for a test.
    const int n = (int)x.size();
    double re = 0, im = 0;
    for (int i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        const double th = -2 * M_PI * hz * i / rate;
        re += w * (x[i].real() * std::cos(th) - x[i].imag() * std::sin(th));
        im += w * (x[i].real() * std::sin(th) + x[i].imag() * std::cos(th));
    }
    return 10.0f * (float)std::log10((re * re + im * im) / ((double)n * n / 4) + 1e-30);
}

int main() {
    const double rate = 2400000.0;
    const int blk = 65536;
    // ── Synthetic front end: tone at +50 kHz, DC (0.05, -0.03), Q gain 0.89 (−1 dB), phase 5°.
    auto make = [&](int n, double phase0) {
        std::vector<cf32> v((size_t)n);
        const double gq = std::pow(10.0, -1.0 / 20.0), ph = 5.0 * M_PI / 180.0;
        for (int i = 0; i < n; ++i) {
            const double th = phase0 + 2 * M_PI * 50000.0 * i / rate;
            const double I = 0.3 * std::cos(th), Q = 0.3 * std::sin(th);
            // imbalance: q' = gq·(Q·cos ph + I·sin ph)
            v[(size_t)i] = cf32((float)(I + 0.05), (float)(gq * (Q * std::cos(ph) + I * std::sin(ph)) - 0.03));
        }
        return v;
    };
    IqCleaner c; c.configure(rate);
    std::vector<cf32> raw = make(blk, 0.0);
    const float dc0 = binPowerDb(raw, rate, 0.0), tone0 = binPowerDb(raw, rate, 50000.0), img0 = binPowerDb(raw, rate, -50000.0);
    std::printf("before: tone %.1f dB, DC %.1f dB, image %.1f dB\n", tone0, dc0, img0);
    // Converge: ~3 s of blocks.
    double ph = 0.0;
    for (int b = 0; b < (int)(3.0 * rate / blk); ++b) { auto v = make(blk, ph); c.process(v.data(), blk); ph += 2 * M_PI * 50000.0 * blk / rate; }
    auto v = make(blk, ph); c.process(v.data(), blk);
    const float dc1 = binPowerDb(v, rate, 0.0), tone1 = binPowerDb(v, rate, 50000.0), img1 = binPowerDb(v, rate, -50000.0);
    std::printf("after:  tone %.1f dB, DC %.1f dB, image %.1f dB  (A=%.4f B=%.4f dc=%.4f,%.4f)\n", tone1, dc1, img1, c.coefA(), c.coefB(), c.dcI(), c.dcQ());
    CHECK(tone1 > tone0 - 1.0f, "tone level changed by more than 1 dB (%.1f -> %.1f)", tone0, tone1);
    CHECK(dc1 < tone1 - 60.0f, "DC not removed: %.1f dB vs tone %.1f", dc1, tone1);
    CHECK(img1 < img0 - 30.0f, "image not suppressed by 30 dB: %.1f -> %.1f", img0, img1);
    // ── Rotation: +15 kHz moves the tone to 65 kHz.
    IqCleaner r; r.configure(rate); r.setDc(false); r.setImbalance(false); r.setRotation(15000.0);
    auto w = make(blk, 0.0); for (auto& s : w) s = cf32(s.real() - 0.05f, s.imag() + 0.03f);
    r.process(w.data(), blk);
    const float at65 = binPowerDb(w, rate, 65000.0), at50 = binPowerDb(w, rate, 50000.0);
    std::printf("rotate: 65 kHz %.1f dB, 50 kHz %.1f dB\n", at65, at50);
    CHECK(at65 > at50 + 40.0f, "rotation did not move the tone (+65k %.1f, +50k %.1f)", at65, at50);
    // Continuity across blocks: process two halves and compare to one whole.
    IqCleaner r2; r2.configure(rate); r2.setDc(false); r2.setImbalance(false); r2.setRotation(15000.0);
    auto w2 = make(blk, 0.0); for (auto& s : w2) s = cf32(s.real() - 0.05f, s.imag() + 0.03f);
    r2.process(w2.data(), 1000); r2.process(w2.data() + 1000, blk - 1000);
    float maxd = 0; for (int i = 0; i < blk; ++i) maxd = std::max(maxd, std::abs(w2[(size_t)i] - w[(size_t)i]));
    CHECK(maxd < 1e-3f, "block-split rotation differs by %.2e", maxd);
    // ── Blanker: noise floor + tone + 5-sample impulse 30 dB up.
    ImpulseBlanker nb; nb.configure(rate);
    std::mt19937 rng(1); std::normal_distribution<float> g(0.0f, 0.01f);
    std::vector<cf32> x((size_t)blk);
    for (int i = 0; i < blk; ++i) x[(size_t)i] = cf32(0.02f * std::cos(2 * M_PI * 50000.0 * i / rate) + g(rng), 0.02f * std::sin(2 * M_PI * 50000.0 * i / rate) + g(rng));
    auto clean = x;
    nb.process(x.data(), blk);                     // seeds
    float d0 = 0; for (int i = 0; i < blk; ++i) d0 = std::max(d0, std::abs(x[(size_t)i] - clean[(size_t)i]));
    CHECK(d0 == 0.0f, "seed block was modified");
    auto y = clean; for (int i = 30000; i < 30005; ++i) y[(size_t)i] = cf32(1.0f, -1.0f);
    nb.process(y.data(), blk);
    float peak = 0; for (int i = 29990; i < 30015; ++i) peak = std::max(peak, std::abs(y[(size_t)i]));
    const float r1 = nb.rate();
    std::printf("blanker: peak after %.3f, rate %.2e\n", peak, r1);
    CHECK(peak < 0.1f, "impulse not blanked: peak %.3f", peak);
    CHECK(r1 < 1e-3f, "blanked too much of a clean stream: %.2e", r1);
    auto z = clean; nb.process(z.data(), blk);
    float dz = 0; for (int i = 0; i < blk; ++i) dz = std::max(dz, std::abs(z[(size_t)i] - clean[(size_t)i]));
    CHECK(dz == 0.0f, "clean block was modified after the impulse (%.3e)", dz);
    std::printf(fails ? "%d FAILURE(S)\n" : "all iqclean tests passed\n", fails);
    return fails ? 1 : 0;
}
