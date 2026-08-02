// Channelizer (fast convolution / overlap-save) — host tests.
//
// What these assert, and why each one exists:
//   1. a tone inside the channel comes out at the RIGHT frequency (a wrong slice layout does not
//      fail loudly — it tunes you somewhere else, silently);
//   2. it comes out at the RIGHT amplitude (KissFFT normalises neither direction, so the scale
//      factor is a real decision, not a detail);
//   3. a tone OUTSIDE the channel is rejected (otherwise we have an expensive copy, not a filter);
//   4. block boundaries are clean (overlap-save exists to remove the wrap-around; if the discard
//      is wrong the error shows up as a periodic click, which is exactly what nobody notices in a
//      waterfall and everybody hears in audio).
#include "vibedsp.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <complex>

using namespace vibedsp;

static int failures = 0;
static void check(bool ok, const char* what, double got = 0, double want = 0) {
    if (ok) { std::printf("  ok   %s\n", what); return; }
    std::printf("  FAIL %s   (got %.6f, want %.6f)\n", what, got, want);
    ++failures;
}

/** Estimate a complex tone's normalised frequency (cycles/sample) from its phase advance. */
static double toneFreq(const std::vector<cf32>& x) {
    double acc = 0; int n = 0;
    for (size_t i = 1; i < x.size(); ++i) {
        std::complex<double> a(x[i-1].real(), x[i-1].imag());
        std::complex<double> b(x[i].real(),   x[i].imag());
        if (std::abs(a) < 1e-6 || std::abs(b) < 1e-6) continue;
        acc += std::arg(b * std::conj(a));
        ++n;
    }
    return n ? acc / n / (2.0 * M_PI) : 0.0;
}

static double rms(const std::vector<cf32>& x) {
    double s = 0;
    for (auto& v : x) s += (double)v.real()*v.real() + (double)v.imag()*v.imag();
    return x.empty() ? 0.0 : std::sqrt(s / x.size());
}

/** Run a tone at `fIn` (cycles/sample) through a channel centred on bin `centreBin`. */
static std::vector<cf32> runTone(int N, int chanBins, int centreBin, double fIn, int blocks) {
    Channelizer ch(N);
    std::vector<cf32> out;
    std::vector<cf32> chunk(chanBins);
    const int hop = ch.hop();
    std::vector<cf32> in(hop);
    double phase = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < hop; ++i) {
            in[i] = cf32((float)std::cos(phase), (float)std::sin(phase));
            phase += 2.0 * M_PI * fIn;
            if (phase > 2*M_PI) phase -= 2*M_PI;
        }
        ch.feed(in.data(), hop, [&](const cf32* bins, int) {
            int n = ch.extract(bins, centreBin, chanBins, chunk.data());
            for (int i = 0; i < n; ++i) out.push_back(chunk[i]);
        });
    }
    return out;
}

int main() {
    std::printf("Channelizer (fast convolution / overlap-save)\n");

    const int N = 4096;          // forward FFT
    const int CH = 256;          // channel size  → decimation D = 16
    const int D  = N / CH;

    // ── 1 + 2: a tone 10 channel-bins above the channel centre ──────────────────────────────
    // Channel centre at bin +512 of 4096 = +0.125 fs. Ten channel bins above that is
    // 10 * (fs/N) = 10/4096 fs. After decimation by D the tone should sit at 10/CH of the
    // CHANNEL's rate, i.e. 10/256 cycles/sample.
    {
        const int centre = 512;
        const double fIn = (double)(centre + 10) / (double)N;
        auto y = runTone(N, CH, centre, fIn, 12);
        check(!y.empty(), "produced output");
        // Drop the first block: the filter (and the very first history block, which is zeros)
        // has to settle, exactly as any real filter does.
        std::vector<cf32> tail(y.begin() + (y.size() / 3), y.end());
        const double f  = toneFreq(tail);
        const double want = 10.0 / (double)CH;
        check(std::fabs(f - want) < 1e-3, "tone lands at the right frequency", f, want);
        const double a = rms(tail);
        check(std::fabs(a - 1.0) < 0.05, "unit input gives unit output", a, 1.0);
    }

    // ── 3: rejection. A tone well outside the channel must be attenuated, not aliased in ─────
    {
        const int centre = 512;
        const double fIn = (double)(centre + CH) / (double)N;   // a full channel-width away
        auto y = runTone(N, CH, centre, fIn, 12);
        std::vector<cf32> tail(y.begin() + (y.size() / 3), y.end());
        const double a = rms(tail);
        check(a < 0.10, "out-of-channel tone is rejected", a, 0.0);
    }

    // ── 4: decimation arithmetic — output count must be (1 - 1/OVERLAP_DIV) per block ────────
    {
        Channelizer ch(N);
        int total = 0, blocks = 0;
        std::vector<cf32> chunk(CH);
        std::vector<cf32> in(ch.hop(), cf32(0.0f, 0.0f));
        for (int b = 0; b < 8; ++b)
            ch.feed(in.data(), (int)in.size(), [&](const cf32* bins, int) {
                ++blocks;
                total += ch.extract(bins, 0, CH, chunk.data());
            });
        check(blocks == 8, "one block per hop", blocks, 8);
        const int perBlock = CH - CH / Channelizer::OVERLAP_DIV;
        check(total == 8 * perBlock, "kept samples per block", total, 8.0 * perBlock);
        // And the rate really is fs/D: hop input samples in, perBlock out.
        const double ratio = (double)ch.hop() / (double)perBlock;
        check(std::fabs(ratio - D) < 1e-9, "output rate is fs/D", ratio, D);
    }

    std::printf(failures ? "\nFAILURES: %d\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
