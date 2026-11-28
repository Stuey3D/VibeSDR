// bench-frontend.cpp — what the shared front end's forward FFT costs on THIS machine (b97-cpu-budget, 2026-11-28).
//
// Standalone: no radio, no server, nothing replaced. Times the channeliser's overlap-save forward transform (PFFFT, the
// library vibedsp uses — NEON on arm64, SSE on x86) at the sizes a front end could use, ordered (what ComplexFFT::forward
// does today) and unordered (PFFFT's own bin order — the reorder skipped), complex (an RSP / RTL: IQ) and real (an
// RX888: real samples, half the bins). Reports the share of ONE core each needs at 2.4, 8, 32 and 64 MS/s with the
// channeliser's 1/4 overlap (hop = 3/4 of the block) — the yardstick for "can this box carry that radio".
//   build:  c++ -O3 -std=c++17 -I android/app/src/main/cpp vibeserver/bench-frontend.cpp \
//           android/app/src/main/cpp/vibedsp/third_party/pffft/pffft.c -o bench-frontend   (add -march=native on x86)
//   run:    nice -n 19 ./bench-frontend            (seconds; one thread; silent)
#include "vibedsp/third_party/pffft/pffft.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static double timeIt(PFFFT_Setup* s, float* in, float* out, float* work, bool ordered, int reps) {
    // warm-up
    for (int i = 0; i < 20; ++i) { if (ordered) pffft_transform_ordered(s, in, out, work, PFFFT_FORWARD); else pffft_transform(s, in, out, work, PFFFT_FORWARD); }
    double best = 1e30;
    for (int round = 0; round < 5; ++round) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; ++i) {
            if (ordered) pffft_transform_ordered(s, in, out, work, PFFFT_FORWARD); else pffft_transform(s, in, out, work, PFFFT_FORWARD);
        }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
        if (us < best) best = us;
    }
    return best;
}

// ★★ THE FOUR-STEP FFT (the textbook N = N1·N2 decomposition, written here from the algorithm): a big transform done as
//    N2 transforms of N1 and N1 of N2, each small enough to live in L1/L2, with the twiddles between — how cache-aware
//    libraries keep a 32k-1M transform fast once its data no longer fits the cache. PFFFT does the small ones.
struct FourStep {
    int n1, n2, n;
    PFFFT_Setup *s1, *s2;
    float *a, *b, *w1, *w2, *tw;   // complex interleaved buffers of n, the twiddles W_n^(n2·k1)
    FourStep(int N1, int N2) : n1(N1), n2(N2), n(N1 * N2) {
        s1 = pffft_new_setup(n1, PFFFT_COMPLEX); s2 = pffft_new_setup(n2, PFFFT_COMPLEX);
        a = (float*)pffft_aligned_malloc((size_t)n * 8); b = (float*)pffft_aligned_malloc((size_t)n * 8);
        w1 = (float*)pffft_aligned_malloc((size_t)n1 * 8); w2 = (float*)pffft_aligned_malloc((size_t)n2 * 8);
        tw = (float*)pffft_aligned_malloc((size_t)n * 8);
        for (int c = 0; c < n2; ++c) for (int k1 = 0; k1 < n1; ++k1) {
            const double ph = -2.0 * M_PI * (double)c * (double)k1 / (double)n;
            tw[2 * ((size_t)c * n1 + k1)] = (float)std::cos(ph); tw[2 * ((size_t)c * n1 + k1) + 1] = (float)std::sin(ph);
        }
    }
    /** in: n complex, natural order; out: X[k], natural order. `natural` false = leave Y[k1][k2] (X[k1 + n1·k2]) —
     *  a channeliser can read that layout directly and skip the last transpose. */
    void forward(const float* in, float* out, bool natural = true) {
        const int T = 16;
        // 1. transpose rows n1 × cols n2 → [n2][n1] (each column contiguous), in tiles
        for (int r0 = 0; r0 < n1; r0 += T) for (int c0 = 0; c0 < n2; c0 += T)
            for (int r = r0; r < std::min(n1, r0 + T); ++r) for (int c = c0; c < std::min(n2, c0 + T); ++c) {
                a[2 * ((size_t)c * n1 + r)] = in[2 * ((size_t)r * n2 + c)]; a[2 * ((size_t)c * n1 + r) + 1] = in[2 * ((size_t)r * n2 + c) + 1];
            }
        // 2. n2 transforms of n1, then the twiddles
        for (int c = 0; c < n2; ++c) {
            float* row = a + 2 * (size_t)c * n1;
            pffft_transform_ordered(s1, row, row, w1, PFFFT_FORWARD);
            const float* t = tw + 2 * (size_t)c * n1;
            for (int k = 0; k < n1; ++k) {
                const float re = row[2 * k], im = row[2 * k + 1], tr = t[2 * k], ti = t[2 * k + 1];
                row[2 * k] = re * tr - im * ti; row[2 * k + 1] = re * ti + im * tr;
            }
        }
        // 3. transpose [n2][n1] → [n1][n2]
        for (int r0 = 0; r0 < n2; r0 += T) for (int c0 = 0; c0 < n1; c0 += T)
            for (int r = r0; r < std::min(n2, r0 + T); ++r) for (int c = c0; c < std::min(n1, c0 + T); ++c) {
                b[2 * ((size_t)c * n2 + r)] = a[2 * ((size_t)r * n1 + c)]; b[2 * ((size_t)c * n2 + r) + 1] = a[2 * ((size_t)r * n1 + c) + 1];
            }
        // 4. n1 transforms of n2: Y[k1][k2] = X[k1 + n1·k2]
        for (int k1 = 0; k1 < n1; ++k1) { float* row = b + 2 * (size_t)k1 * n2; pffft_transform_ordered(s2, row, row, w2, PFFFT_FORWARD); }
        if (!natural) return;
        // 5. natural order
        for (int r0 = 0; r0 < n1; r0 += T) for (int c0 = 0; c0 < n2; c0 += T)
            for (int k1 = r0; k1 < std::min(n1, r0 + T); ++k1) for (int k2 = c0; k2 < std::min(n2, c0 + T); ++k2) {
                const size_t k = (size_t)k1 + (size_t)n1 * k2;
                out[2 * k] = b[2 * ((size_t)k1 * n2 + k2)]; out[2 * k + 1] = b[2 * ((size_t)k1 * n2 + k2) + 1];
            }
    }
};

static void fourStepRows(const double* rates) {
    const int shapes[][2] = { { 128, 256 }, { 256, 256 }, { 256, 512 } };
    for (auto& sh : shapes) {
        FourStep f(sh[0], sh[1]);
        const int n = f.n;
        float* in = (float*)pffft_aligned_malloc((size_t)n * 8); float* out = (float*)pffft_aligned_malloc((size_t)n * 8);
        float* ref = (float*)pffft_aligned_malloc((size_t)n * 8); float* wk = (float*)pffft_aligned_malloc((size_t)n * 8);
        for (int i = 0; i < 2 * n; ++i) in[i] = (float)std::sin(0.00123 * (double)((long long)i * i % 7919));
        PFFFT_Setup* d = pffft_new_setup(n, PFFFT_COMPLEX);
        pffft_transform_ordered(d, in, ref, wk, PFFFT_FORWARD);
        f.forward(in, out);
        double err = 0, mag = 0;
        for (int i = 0; i < 2 * n; ++i) { err = std::max(err, (double)std::fabs(out[i] - ref[i])); mag = std::max(mag, (double)std::fabs(ref[i])); }
        const int reps = std::max(20, (int)(4e6 / n));
        for (int nat = 1; nat >= 0; --nat) {
            double best = 1e30;
            for (int round = 0; round < 5; ++round) {
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < reps; ++i) f.forward(in, out, nat == 1);
                best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps);
            }
            std::printf("%-8d %-9s %10.1f  ", n, nat ? "4-step" : "4-step-Y", best);
            for (int k = 0; k < 4; ++k) std::printf(" %7.1f%%", 100.0 * best * 1e-6 * (rates[k] / (0.75 * n)));
            std::printf("   (%dx%d, max error %.1e of peak)\n", sh[0], sh[1], err / mag);
        }
        pffft_destroy_setup(d);
    }
}

int main() {
    const int sizes[] = { 8192, 16384, 32768, 65536, 131072 };
    const double rates[] = { 2.4e6, 8e6, 32e6, 64e6 };
    std::printf("forward FFT per block (best of 5), and the share of ONE core at each input rate (hop = 3/4 block)\n");
    std::printf("%-8s %-9s %10s   %8s %8s %8s %8s\n", "size", "kind", "us/block", "2.4MS/s", "8MS/s", "32MS/s", "64MS/s");
    for (int n : sizes) {
        for (int kind = 0; kind < 3; ++kind) {   // 0 complex ordered, 1 complex unordered, 2 real ordered
            const bool real = kind == 2;
            PFFFT_Setup* s = pffft_new_setup(n, real ? PFFFT_REAL : PFFFT_COMPLEX);
            if (!s) continue;
            const size_t floats = real ? (size_t)n : (size_t)n * 2;
            float* in = (float*)pffft_aligned_malloc(floats * sizeof(float));
            float* out = (float*)pffft_aligned_malloc(floats * sizeof(float));
            float* work = (float*)pffft_aligned_malloc(floats * sizeof(float));
            for (size_t i = 0; i < floats; ++i) in[i] = (float)std::sin(0.001 * (double)i) * 0.5f;
            const int reps = std::max(20, (int)(4e6 / n));
            const double us = timeIt(s, in, out, work, kind != 1, reps);
            const double hop = 0.75 * n;   // samples consumed per block (a real block of n reals = n input samples)
            std::printf("%-8d %-9s %10.1f  ", n, kind == 0 ? "cplx-ord" : kind == 1 ? "cplx-raw" : "real-ord", us);
            for (double r : rates) std::printf(" %7.1f%%", 100.0 * us * 1e-6 * (r / hop));
            std::printf("\n");
            pffft_aligned_free(in); pffft_aligned_free(out); pffft_aligned_free(work);
            pffft_destroy_setup(s);
        }
    }
    fourStepRows(rates);
    std::printf("(an RX888 delivers REAL samples: read its row as real-ord at 64 MS/s; an RSP / RTL row as cplx at its rate)\n");
    return 0;
}
