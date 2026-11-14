// test-dab-ddc.cpp — the wide-rate DAB front end (vibe_dab_ddc.h) must hand the decoder the block it would have got
// from a radio tuned to it at 2.048 MS/s: flat across the ensemble, nothing folded in, the exact rate, and the same
// output however the input is chopped up.
//
// For every capture rate a radio offers (2.048 … 56 MS/s — the USRP's 40/50/56 added 2026-11-14) and block offsets that fit:
//   1. PASSBAND — tones across the ensemble (±768 kHz of the block centre) within ±0.1 dB and above 70 dB SNDR
//      (the old 2.4 path's own test asks 1 dB / 40 dB; it measures 0.25 dB / 75 dB);
//   2. ALIASES — a tone anywhere that would fold INTO the ensemble at 2.048 (|f| ≥ 1.248 MHz from the block centre, up
//      to the capture's edge) comes out at least 75 dB down, so neighbouring multiplexes and the wide noise floor stay out;
//   3. RATE — exactly 2.048 output samples per microsecond of input;
//   4. CHUNKING — any chop of the input gives the identical output (the buffering is right);
//   5. SIMD == SCALAR — the vector dot product matches the scalar reference to float rounding.
// Then the COST: ns per input sample and % of one core for real time, per rate (the number the report states).
#include "vibe_dab_ddc.h"
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>
using namespace vibedab;

static bool g_ok = true;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); g_ok = false; } } while (0)

static std::vector<float> toneIn(double fHz, double fs, size_t n, double amp = 1.0) {
    std::vector<float> v(2 * n);
    for (size_t i = 0; i < n; ++i) { const double ph = 2 * M_PI * fHz * double(i) / fs; v[2 * i] = float(amp * std::cos(ph)); v[2 * i + 1] = float(amp * std::sin(ph)); }
    return v;
}
/** Power (dB) of the output that lands INSIDE the ensemble (|f| <= 768 kHz) — Hann-windowed FFTs, averaged. This is
 *  what an out-of-block signal must not reach: energy in the guard band (768 kHz … 1.024 MHz) is outside every carrier. */
static double ensemblePowerDb(const std::vector<float>& out, size_t skip) {
    const int N = 4096; const size_t m = out.size() / 2;
    if (m < skip + N) return -999;
    std::vector<std::complex<double>> a(N); double acc = 0; int frames = 0;
    for (size_t s0 = skip; s0 + N <= m; s0 += N, ++frames) {
        double wsum = 0;
        for (int i = 0; i < N; ++i) { const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / N); wsum += w * w; a[size_t(i)] = { out[2 * (s0 + i)] * w, out[2 * (s0 + i) + 1] * w }; }
        for (int i = 1, j = 0; i < N; ++i) { int bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(a[size_t(i)], a[size_t(j)]); }
        for (int len = 2; len <= N; len <<= 1) {
            const std::complex<double> wl = std::polar(1.0, -2 * M_PI / len);
            for (int i = 0; i < N; i += len) { std::complex<double> w = 1; for (int j = 0; j < len / 2; ++j) { const auto u = a[size_t(i + j)], v = a[size_t(i + j + len / 2)] * w; a[size_t(i + j)] = u + v; a[size_t(i + j + len / 2)] = u - v; w *= wl; } }
        }
        double p = 0;
        for (int i = 0; i < N; ++i) { const double f = (i < N / 2 ? i : i - N) * DabDdc::kOutHz / N; if (std::fabs(f) <= 768e3) p += std::norm(a[size_t(i)]); }
        acc += p / (wsum * N);
    }
    return 10 * std::log10(std::max(acc / frames, 1e-30));
}
static double g_outRate = DabDdc::kOutHz;
/** Correlate the output with a tone at `fOut` (Hz, at 2.048): gain (dB) and SNDR (dB). */
static void measure(const std::vector<float>& out, double fOut, size_t skip, double& gainDb, double& sndrDb, double& totDb) {
    const size_t m = out.size() / 2;
    double sr = 0, si = 0, tot = 0;
    for (size_t i = skip; i < m; ++i) {
        const double ph = 2 * M_PI * fOut * double(i) / g_outRate, c = std::cos(ph), s = std::sin(ph);
        sr += out[2 * i] * c + out[2 * i + 1] * s; si += out[2 * i + 1] * c - out[2 * i] * s;
        tot += double(out[2 * i]) * out[2 * i] + double(out[2 * i + 1]) * out[2 * i + 1];
    }
    const double nn = double(m - skip), amp = std::sqrt(sr * sr + si * si) / nn, sig = amp * amp, noise = tot / nn - sig;
    gainDb = 20 * std::log10(std::max(amp, 1e-12));
    sndrDb = 10 * std::log10(sig / std::max(noise, 1e-20));
    totDb = 10 * std::log10(std::max(tot / nn, 1e-20));
}

int main() {
    const double rates[] = { 2048000, 2400000, 2500000, 3000000, 3200000, 4000000, 5000000, 6000000, 8000000, 10000000,
                             12500000, 16000000, 20000000, 25000000, 28000000, 30720000, 32000000, 40000000, 50000000, 56000000 };
    std::mt19937 rng(7);
    std::printf("rate MS/s  shift MHz   hb  mid MS/s   L/M        taps(hb..|rat)   pass dB [min,max]  SNDR min  alias worst\n");
    for (double fs : rates) {
        std::vector<double> shifts = { 0.0 };
        for (double s : { 2.5e6, -1.5e6, 0.9e6, -7.0e6, 11.0e6 }) if (std::fabs(s) + 1.3e6 < fs / 2) shifts.push_back(s);
        if (fs == 2400000) shifts.push_back(0.15e6);   // ★ a 2.4 capture off the block by a little (the DC-offset case)
        for (double sh : shifts) {
            DabDdc d;
            if (!d.configure(fs, sh)) { CHECK(false, "%.3f MS/s shift %.2f MHz refused", fs / 1e6, sh / 1e6); continue; }
            g_outRate = d.outRateActual();
            const size_t n = size_t(fs * 0.06);   // 60 ms
            double gMin = 99, gMax = -99, sMin = 999;
            for (double f : { -768e3, -600e3, -384e3, -100e3, 0.0, 1e3, 250e3, 500e3, 700e3, 768e3 }) {
                d.reset();
                const auto in = toneIn(sh + f, fs, n);
                std::vector<float> out; d.process(in.data(), n, out);
                double g, s, t; measure(out, f, 4000, g, s, t);
                gMin = std::min(gMin, g); gMax = std::max(gMax, g); sMin = std::min(sMin, s);
                // ★ 3. the rate: exactly 2.048 per µs of input, give or take the filters' start-up
                const double want = double(n) * DabDdc::kOutHz / fs;
                CHECK(std::fabs(double(out.size() / 2) - want) < 2.0 + d.ratTaps(), "%.3f MS/s: %zu out for %.1f expected", fs / 1e6, out.size() / 2, want);
            }
            CHECK(gMin > -0.1 && gMax < 0.1, "%.3f MS/s shift %.2f: passband %.3f..%.3f dB", fs / 1e6, sh / 1e6, gMin, gMax);
            CHECK(sMin > 70, "%.3f MS/s shift %.2f: SNDR %.1f dB", fs / 1e6, sh / 1e6, sMin);
            // ★ 2. aliases: every tone that would fold into the ensemble, out to the capture's edge
            double worst = -999, worstF = 0;
            for (double f = 1.248e6; f < fs; f += (f < 4.5e6 ? 61e3 : 397e3)) {
                for (int sgn : { 1, -1 }) {
                    const double fin = sh + sgn * f;
                    if (std::fabs(fin) > fs / 2 * 0.999) continue;
                    d.reset();
                    const auto in = toneIn(fin, fs, size_t(fs * 0.02));
                    std::vector<float> out; d.process(in.data(), in.size() / 2, out);
                    const double t = ensemblePowerDb(out, 2000);
                    if (t > worst) { worst = t; worstF = sgn * f; }
                }
            }
            CHECK(worst < -75, "%.3f MS/s shift %.2f: an out-of-block tone at %+.3f MHz reached the ensemble at %.1f dB", fs / 1e6, sh / 1e6, worstF / 1e6, worst);
            char taps[64] = "";
            int p = 0; for (int i = 0; i < d.halfBands(); ++i) p += std::snprintf(taps + p, sizeof taps - size_t(p), "%d ", d.halfBandTaps(i));
            std::snprintf(taps + p, sizeof taps - size_t(p), "| %d", d.ratTaps());
            std::printf("%8.3f  %8.2f   %2d  %7.3f   %4d/%-5d  %-16s  [%6.3f,%6.3f]   %6.1f   %7.1f\n", fs / 1e6, sh / 1e6, d.halfBands(),
                        d.midRate() / 1e6, d.ratL(), d.ratM(), taps, gMin, gMax, sMin, worst);
        }
    }
    // ★ 4. chunking: random chops give the identical output
    {
        const double fs = 8000000;
        DabDdc a, b; a.configure(fs, 2.5e6); b.configure(fs, 2.5e6);
        std::normal_distribution<float> g(0, 0.3f);
        std::vector<float> in(2 * 400000); for (auto& v : in) v = g(rng);
        std::vector<float> oa, ob; a.process(in.data(), in.size() / 2, oa);
        size_t i = 0; std::uniform_int_distribution<int> len(1, 20000);
        while (i < in.size() / 2) { const size_t k = std::min(size_t(len(rng)), in.size() / 2 - i); b.process(in.data() + 2 * i, k, ob); i += k; }
        CHECK(oa.size() == ob.size(), "chunking: %zu vs %zu samples", oa.size(), ob.size());
        size_t diff = 0; for (size_t k = 0; k < std::min(oa.size(), ob.size()); ++k) if (oa[k] != ob[k]) ++diff;
        CHECK(diff == 0, "chunking: %zu samples differ", diff);
        std::printf("chunking: %zu samples, %zu differ\n", oa.size() / 2, diff);
    }
    // ★ 5. the vector dot product against the scalar reference
    {
        std::uniform_real_distribution<float> u(-1, 1);
        double worst = 0;
        for (int t = 0; t < 200; ++t) {
            const int n4 = 1 + t % 40;
            std::vector<float> x(size_t(4 * n4)), h(size_t(4 * n4)); for (auto& v : x) v = u(rng); for (auto& v : h) v = u(rng);
            float r1, i1, r2, i2; ddc::dot4(x.data(), h.data(), n4, r1, i1); ddc::dot4Scalar(x.data(), h.data(), n4, r2, i2);
            worst = std::max(worst, double(std::fabs(r1 - r2) + std::fabs(i1 - i2)));
        }
        CHECK(worst < 1e-5, "SIMD dot differs from scalar by %g", worst);
        std::printf("SIMD vs scalar dot: worst difference %.2g\n", worst);
    }
    // ★ The COST, per input sample, and as % of one core in real time
    std::printf("\ncost (this machine): rate MS/s  ns/sample  %%core real time\n");
    for (double fs : { 2400000.0, 2500000.0, 4000000.0, 8000000.0, 10000000.0, 16000000.0, 20000000.0, 30720000.0, 56000000.0 }) {
        DabDdc d; d.configure(fs, fs >= 8e6 ? 2.5e6 : 0.0);
        std::vector<float> in(2 * size_t(fs * 0.005)); std::normal_distribution<float> g(0, 0.3f); for (auto& v : in) v = g(rng);
        std::vector<float> out; out.reserve(2 * 20000);
        const int reps = int(2.0e8 / fs) + 20;   // ~1 s of real time worth of 5 ms blocks
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r) { out.clear(); d.process(in.data(), in.size() / 2, out); }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double samples = double(reps) * double(in.size() / 2);
        std::printf("                     %7.3f   %7.2f    %6.2f\n", fs / 1e6, 1e9 * s / samples, 100.0 * s / (samples / fs));
    }
    std::printf("%s\n", g_ok ? "PASS" : "FAIL");
    return g_ok ? 0 : 1;
}
