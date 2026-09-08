// PFFFT against the KissFFT reference, bin for bin, forward and inverse, at every size the
// engine actually uses. A faster FFT that computes something slightly different is not a
// faster FFT — it is a new bug in every mode at once.
#include "../vibedsp.h"
#include "../third_party/kissfft/kiss_fft.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace vibedsp;
static int fails = 0;
static void cmp(int N, bool inverse) {
    std::vector<cf32> in(N), got(N), ref(N);
    std::mt19937 rng(N * 7 + inverse); std::uniform_real_distribution<float> d(-1, 1);
    for (auto& x : in) x = cf32(d(rng), d(rng));
    ComplexFFT f(N, inverse);                       // pffft where it can
    f.forward(in.data(), got.data());
    kiss_fft_cfg cfg = kiss_fft_alloc(N, inverse ? 1 : 0, nullptr, nullptr);
    kiss_fft(cfg, (const kiss_fft_cpx*)in.data(), (kiss_fft_cpx*)ref.data());
    kiss_fft_free(cfg);
    double worst = 0, energy = 0;
    for (int i = 0; i < N; ++i) {
        worst  = std::max(worst, (double)std::abs(got[i] - ref[i]));
        energy = std::max(energy, (double)std::abs(ref[i]));
    }
    const double rel = energy > 0 ? worst / energy : worst;
    const bool ok = rel < 2e-5;
    if (!ok) ++fails;
    printf("  [%s] N=%-6d %-8s worst=%.3g  relative=%.2e\n",
           ok ? "PASS" : "FAIL", N, inverse ? "inverse" : "forward", worst, rel);
}
// An UNALIGNED destination — the channelizer writes channels into slices of a bigger buffer,
// and PFFFT reads through SIMD loads. This is the case that segfaults if the guard is wrong.
static void unaligned(int N) {
    std::vector<cf32> in(N), big(N + 1), ref(N);
    std::mt19937 rng(3); std::uniform_real_distribution<float> d(-1, 1);
    for (auto& x : in) x = cf32(d(rng), d(rng));
    cf32* out = big.data() + 1;                     // 8-byte offset: deliberately not 16-aligned
    ComplexFFT f(N, false);
    f.forward(in.data(), out);
    kiss_fft_cfg cfg = kiss_fft_alloc(N, 0, nullptr, nullptr);
    kiss_fft(cfg, (const kiss_fft_cpx*)in.data(), (kiss_fft_cpx*)ref.data());
    kiss_fft_free(cfg);
    double worst = 0, energy = 0;
    for (int i = 0; i < N; ++i) { worst = std::max(worst, (double)std::abs(out[i] - ref[i]));
                                  energy = std::max(energy, (double)std::abs(ref[i])); }
    const bool ok = energy > 0 && worst / energy < 2e-5;
    if (!ok) ++fails;
    printf("  [%s] N=%-6d unaligned destination  relative=%.2e\n", ok ? "PASS" : "FAIL", N, worst / energy);
}
int main() {
    printf("test-cfft-pffft\n");
    for (int N : { 4096, 8192, 16384, 32768 }) { cmp(N, false); cmp(N, true); }
    for (int N : { 64, 128, 256, 512, 1024 })  { cmp(N, false); cmp(N, true); }
    for (int N : { 16, 8 })                     cmp(N, false);   // below pffft's floor: kiss path
    unaligned(8192); unaligned(32768);
    printf(fails ? "  %d FAILED\n" : "  all passed\n", fails);
    return fails ? 1 : 0;
}
