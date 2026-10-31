// tools/textmode-bench/tm_clip.cpp — the end-to-end clip for scripts/test-web-textmodes.mjs (V12, 2026-10-31): unsigned
// 8-bit IQ at 960 kS/s, 20 s, for vibeserver/fake-rtl-tcp.mjs --hf FILE --hf-centre (dial + 3500). Upper-sideband audio as
// a USB dial hears it: OLIVIA 16/500 at 1500 Hz and MFSK16 at 800 Hz, each sending its own text, made by the test
// transmitter (decoders/textmodes.h), turned analytic with one FFT and moved up. Synthetic — no recording, no server.
//   tm_clip OUT.cu8
#include "decoders/textmodes.h"
#include "vibedsp/third_party/pffft/pffft.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <cstdlib>
#include <vector>
using namespace vibe;
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const double fs = 48000, R = 960000, sec = 20, off = -3500;
    const int N = 1 << 20;   // ≥ 20 s at 48 kHz
    std::vector<float> a((size_t)(fs * sec), 0.f);
    auto o = textModeSynth(TextModeDecoder::Family::Olivia, "olivia-16-500", "CQ DE G4XYZ OLIVIA K", fs, 1500);
    auto m = textModeSynth(TextModeDecoder::Family::Mfsk, "mfsk16", "CQ CQ DE G4XYZ MFSK TEST PSE K", fs, 800);
    for (size_t i = 0; i < o.size() && i + 24000 < a.size(); ++i) a[i + 24000] += 0.3f * o[i];
    for (size_t i = 0; i < m.size() && i + 24000 < a.size(); ++i) a[i + 24000] += 0.3f * m[i];
    if (const char* d = std::getenv("VIBE_TM_DUMP")) { FILE* f = std::fopen(d, "wb"); if (f) { std::fwrite(a.data(), 4, a.size(), f); std::fclose(f); } }
    // analytic signal: FFT, zero the negative half, double the positive, inverse
    PFFFT_Setup* st = pffft_new_setup(N, PFFFT_COMPLEX);
    float* buf = (float*)pffft_aligned_malloc(sizeof(float) * 2 * N); float* out = (float*)pffft_aligned_malloc(sizeof(float) * 2 * N); float* wk = (float*)pffft_aligned_malloc(sizeof(float) * 2 * N);
    for (int i = 0; i < N; ++i) { buf[2 * i] = i < (int)a.size() ? a[i] : 0.f; buf[2 * i + 1] = 0.f; }
    pffft_transform_ordered(st, buf, out, wk, PFFFT_FORWARD);
    for (int k = 0; k < N; ++k) { const float g = k == 0 ? 1.f : k < N / 2 ? 2.f : 0.f; out[2 * k] *= g / N; out[2 * k + 1] *= g / N; }
    pffft_transform_ordered(st, out, buf, wk, PFFFT_BACKWARD);
    const int up = (int)(R / fs); const size_t n = (size_t)(R * sec);
    std::vector<uint8_t> iq(n * 2);
    std::mt19937 rng(9); std::normal_distribution<float> g(0.f, 1.f);
    auto q8 = [](double v) { const long u = std::lround(127.5 + 127.0 * v); return (uint8_t)std::max(0L, std::min(255L, u)); };
    for (size_t i = 0; i < n; ++i) {
        const size_t j = i / up; const double f = (double)(i % up) / up;
        const size_t j1 = std::min(j + 1, a.size() - 1);
        std::complex<double> z(buf[2 * j] * (1 - f) + buf[2 * j1] * f, buf[2 * j + 1] * (1 - f) + buf[2 * j1 + 1] * f);
        const double ph = 2 * M_PI * off * i / R;
        z *= std::complex<double>(std::cos(ph), std::sin(ph));
        iq[2 * i] = q8(z.real() + 0.01 * g(rng)); iq[2 * i + 1] = q8(z.imag() + 0.01 * g(rng));
    }
    FILE* fp = std::fopen(argv[1], "wb"); if (!fp) return 1;
    std::fwrite(iq.data(), 1, iq.size(), fp); std::fclose(fp);
    std::printf("wrote %s\n", argv[1]);
    pffft_destroy_setup(st); pffft_aligned_free(buf); pffft_aligned_free(out); pffft_aligned_free(wk);
    return 0;
}
