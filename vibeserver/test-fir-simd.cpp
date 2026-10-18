// ★★★ THE FIR KERNELS AGAINST THE ARITHMETIC THEY STAND FOR — sample for sample, every path.
//
// Why this exists (2026-10-18): 21fe0cf7 gave FirDecimator/RealFir an x86-only kernel (taps
// pre-shifted into 2/4 banks so every SSE load is aligned) and b0287e14 changed both loops to step
// from kept output to kept output. A field report of "distortion on the stereo" (BBC Radio 1, RSP1A,
// widest WFM filter, the Lenovo i5) pointed straight at them, and NOTHING in the suite compared the
// x86 path with the plain convolution: the DSP suites assert behaviours (stereo locks, separation
// above N dB), which a kernel that is merely a little wrong can pass. The Mac (arm64) never even
// compiles the SSE path.
//
// So this compares each filter's output with a double-precision direct convolution of the same
// stream — the definition, not another implementation — over:
//   • tap counts either side of every bank boundary (Lp rounding to 8 complex / 16 real) and of the
//     8 KB bank cap (where x86 falls back to the plain kernel);
//   • decimations 1..8, the odd/prime ones the channel cascade picks at RSP rates, and 101 (narrow SSB);
//   • random block sizes (so the kept index lands on every bank phase, and phase_ carries across
//     calls at every offset), including blocks shorter than the decimation and of length 1;
//   • the REAL filters the WFM/NFM/AM chains design at the RSP and RTL rates (designLowpass);
//   • retune() mid-stream (history and phase must carry) and reset().
// Output COUNT must match exactly (a dropped or extra sample is a timing fault), and every sample
// within float rounding of the reference. Run on x86 (amd64/i386 containers) this checks the SSE
// banks; on arm64 the NEON kernel — the same test, the same bar.
#include "vibedsp.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using vibedsp::cf32;
using vibedsp::FirDecimator;
using vibedsp::RealFir;

static int failures = 0, checks = 0;
static void ok(bool cond, const std::string& what) {
    checks++;
    if (cond) return;
    failures++;
    std::printf("   FAIL %s\n", what.c_str());
}

namespace {

// ★ Float sums of K products: the error grows with K and with the size of the terms, so the bar is
//   relative to Σ|tap·x| for that output. 2e-6 is ~16 float ulps — a real kernel fault (a wrong
//   bank, a tap off by one, a lost sample) is orders of magnitude bigger.
constexpr double kTol = 2e-6;

struct Shape { int K, D; std::vector<float> taps; std::string name; };

/** Feed `x` (complex) through `fd` in random blocks; compare with the direct convolution.
 *  retuneAt >= 0: swap to taps2 at that input index (same K), as AdaptiveIf/SSB do live. */
double runComplex(const Shape& s, const std::vector<cf32>& x, std::mt19937& rng, int maxBlock,
                  long retuneAt, const std::vector<float>* taps2, std::string& why) {
    FirDecimator fd(s.taps, s.D);
    std::vector<cf32> y(x.size() + 8);
    std::vector<long> kept;            // input index each output belongs to (for the reference)
    std::vector<int>  tapSet;          // which taps produced it
    int on = 0; long pos = 0; bool swapped = false;
    while (pos < (long)x.size()) {
        long n = std::min<long>((long)x.size() - pos, 1 + (long)(rng() % (unsigned)maxBlock));
        if (retuneAt >= 0 && !swapped && pos + n > retuneAt) {
            n = retuneAt - pos;
            if (n == 0) { fd.retune(*taps2); swapped = true; continue; }
        }
        const int before = on;
        on += fd.process(x.data() + pos, (int)n, y.data() + on);
        // kept outputs are the inputs at D-1, 2D-1, … counted over the whole stream
        long first = ((pos + 1 + s.D - 1) / s.D) * s.D - 1;
        if (first < pos) first += s.D;
        for (long i = first; i < pos + n; i += s.D) { kept.push_back(i); tapSet.push_back(swapped ? 1 : 0); }
        if ((long)kept.size() != on) { why = "count " + std::to_string(on) + " vs " + std::to_string(kept.size()) +
                                             " after block at " + std::to_string(pos) + " (+" + std::to_string(on - before) + ")";
                                       return 1e9; }
        pos += n;
    }
    double worst = 0;
    for (int j = 0; j < on; ++j) {
        const std::vector<float>& t = tapSet[(size_t)j] ? *taps2 : s.taps;
        const long i = kept[(size_t)j];
        double re = 0, im = 0, mag = 1e-30;
        for (int k = 0; k < s.K && i - k >= 0; ++k) {
            re += (double)t[(size_t)k] * x[(size_t)(i - k)].real();
            im += (double)t[(size_t)k] * x[(size_t)(i - k)].imag();
            mag += std::fabs((double)t[(size_t)k]) * (std::fabs(x[(size_t)(i - k)].real()) + std::fabs(x[(size_t)(i - k)].imag()));
        }
        const double e = std::max(std::fabs(re - y[(size_t)j].real()), std::fabs(im - y[(size_t)j].imag())) / mag;
        if (e > worst) { worst = e; if (e > kTol) why = "output " + std::to_string(j) + " (input " + std::to_string(i) + ")"; }
    }
    return worst;
}

double runReal(const Shape& s, const std::vector<float>& x, std::mt19937& rng, int maxBlock,
               long retuneAt, const std::vector<float>* taps2, std::string& why) {
    RealFir rf(s.taps, s.D);
    std::vector<float> y(x.size() + 8);
    std::vector<long> kept; std::vector<int> tapSet;
    int on = 0; long pos = 0; bool swapped = false;
    while (pos < (long)x.size()) {
        long n = std::min<long>((long)x.size() - pos, 1 + (long)(rng() % (unsigned)maxBlock));
        if (retuneAt >= 0 && !swapped && pos + n > retuneAt) {
            n = retuneAt - pos;
            if (n == 0) { rf.retune(*taps2); swapped = true; continue; }
        }
        on += rf.process(x.data() + pos, (int)n, y.data() + on);
        long first = ((pos + 1 + s.D - 1) / s.D) * s.D - 1;
        if (first < pos) first += s.D;
        for (long i = first; i < pos + n; i += s.D) { kept.push_back(i); tapSet.push_back(swapped ? 1 : 0); }
        if ((long)kept.size() != on) { why = "count mismatch at block " + std::to_string(pos); return 1e9; }
        pos += n;
    }
    double worst = 0;
    for (int j = 0; j < on; ++j) {
        const std::vector<float>& t = tapSet[(size_t)j] ? *taps2 : s.taps;
        const long i = kept[(size_t)j];
        double r = 0, mag = 1e-30;
        for (int k = 0; k < s.K && i - k >= 0; ++k) {
            r += (double)t[(size_t)k] * x[(size_t)(i - k)];
            mag += std::fabs((double)t[(size_t)k] * x[(size_t)(i - k)]);
        }
        const double e = std::fabs(r - y[(size_t)j]) / mag;
        if (e > worst) { worst = e; if (e > kTol) why = "output " + std::to_string(j) + " (input " + std::to_string(i) + ")"; }
    }
    return worst;
}

}  // namespace

int main() {
    std::mt19937 rng(20261018);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
#if defined(__SSE2__) || defined(__x86_64__)
    std::printf("\nFIR kernels vs direct convolution — x86 (SSE2 aligned tap banks)\n");
#elif defined(__ARM_NEON) || defined(__aarch64__)
    std::printf("\nFIR kernels vs direct convolution — ARM (NEON)\n");
#else
    std::printf("\nFIR kernels vs direct convolution — scalar\n");
#endif

    std::vector<Shape> shapes;
    // ── random taps at every bank boundary and either side of the 8 KB cap ──
    const int Ks[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 23, 24, 25, 31, 32, 33, 45, 47, 48, 49, 63, 64, 65,
                      101, 127, 128, 129, 137, 275, 490, 491, 492, 493, 494, 505, 506, 507, 508, 509, 510, 511, 700};
    const int Ds[] = {1, 2, 3, 4, 5, 6, 7, 8, 13, 101};
    for (int K : Ks) for (int D : Ds) {
        Shape s{K, D, std::vector<float>((size_t)K), "rand K=" + std::to_string(K) + " D=" + std::to_string(D)};
        for (auto& t : s.taps) t = u(rng) / std::sqrt((float)K);
        shapes.push_back(std::move(s));
    }
    // ── the real designs the chains build at RSP and RTL rates (channel cascade + audio low-pass) ──
    for (double fs : {2.0e6, 2.048e6, 2.4e6, 2.5e6, 3.0e6, 6.0e6, 8.0e6, 10.0e6}) {
        for (int D : {2, 3, 4, 5, 6, 7, 8}) {
            const double cut = 0.5 / D * 0.8;
            Shape s{0, D, vibedsp::designLowpass(cut, cut * 0.5, false), ""};
            s.K = (int)s.taps.size(); s.name = "design fs=" + std::to_string((int)fs) + " D=" + std::to_string(D);
            shapes.push_back(std::move(s));
        }
        for (double ch : {fs / 8, fs / 12, fs / 16}) {   // the deep 15 kHz audio filter at plausible channel rates
            if (ch < 64000) continue;
            const double cut = 15000.0 / ch;
            Shape s{0, std::max(1, (int)(ch / 64000)), vibedsp::designLowpass(cut, cut * 0.4, true), ""};
            s.K = (int)s.taps.size(); s.name = "audio15k ch=" + std::to_string((int)ch);
            shapes.push_back(std::move(s));
        }
    }

    double worstC = 0, worstR = 0; int failC = 0, failR = 0;
    for (const auto& s : shapes) {
        const int len = 6000 + (int)(rng() % 3000);
        std::vector<cf32> xc((size_t)len); for (auto& v : xc) v = cf32(u(rng), u(rng));
        std::vector<float> xr((size_t)len); for (auto& v : xr) v = u(rng);
        std::vector<float> taps2 = s.taps; for (auto& t : taps2) t *= 0.5f + 0.5f * u(rng);
        for (int pass = 0; pass < 3; ++pass) {
            const int maxBlock = pass == 0 ? 3 : pass == 1 ? 97 : 4096;    // tiny, odd, server-sized blocks
            const long rt = pass == 2 ? len / 2 + (long)(rng() % 7) : -1;   // last pass retunes mid-stream
            std::string why;
            const double ec = runComplex(s, xc, rng, maxBlock, rt, rt >= 0 ? &taps2 : nullptr, why);
            if (ec > kTol && failC++ < 10) ok(false, "complex " + s.name + " block≤" + std::to_string(maxBlock) +
                                                      ": err " + std::to_string(ec) + " " + why);
            worstC = std::max(worstC, ec);
            why.clear();
            const double er = runReal(s, xr, rng, maxBlock, rt, rt >= 0 ? &taps2 : nullptr, why);
            if (er > kTol && failR++ < 10) ok(false, "real " + s.name + " block≤" + std::to_string(maxBlock) +
                                                   ": err " + std::to_string(er) + " " + why);
            worstR = std::max(worstR, er);
        }
    }
    std::printf("   .. %zu shapes x 3 block patterns: worst relative error complex %.3g, real %.3g (bar %.0e)\n",
                shapes.size(), worstC, worstR, kTol);
    ok(failC == 0, "★★★ FirDecimator matches the direct convolution, sample for sample, every shape");
    ok(failR == 0, "★★★ RealFir matches the direct convolution, sample for sample, every shape");
    if (!failC && !failR) std::printf("   ok   FirDecimator + RealFir = direct convolution (counts exact, values within %.0e)\n", kTol);

    // ── reset(): the next block must be exactly what a fresh filter makes ──
    {
        Shape s = shapes[shapes.size() / 2];
        std::vector<cf32> x(5000); for (auto& v : x) v = cf32(u(rng), u(rng));
        FirDecimator a(s.taps, s.D), b(s.taps, s.D);
        std::vector<cf32> ya(6000), yb(6000);
        a.process(x.data(), 1234, ya.data()); a.reset();
        const int na = a.process(x.data(), 5000, ya.data()), nb = b.process(x.data(), 5000, yb.data());
        bool same = na == nb;
        for (int i = 0; same && i < na; ++i) same = ya[(size_t)i] == yb[(size_t)i];
        ok(same, "FirDecimator::reset() leaves a filter identical to a fresh one");
        if (same) std::printf("   ok   FirDecimator::reset() = a fresh filter\n");
    }

    std::printf("\n%s%d checks\n", failures ? "FAILURES — " : "", checks);
    if (failures) std::printf("%d FAILED\n", failures);
    return failures ? 1 : 0;
}
