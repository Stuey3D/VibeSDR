// add-hogs.cpp — a hog-heavy ISM test capture: an rtl_tcp u8 recording with wandering emitters added across the band.
//   add-hogs <in.u8> <out.u8> <rate Hz> <level × RMS> <offset Hz>...
// Each emitter is a carrier swept ±30 kHz at 0.5–0.9 Hz around its offset (from the capture centre) and keyed on and off
// at random (on 2–15 ms, off 1–10 ms): what Stuart's Pi 500 433 capture holds near 433.27 and 434.82 MHz, and what a
// host with a poor antenna for the band sees across it (the Pi 2 at 433, "OUTSIDE ANTENNA RANGE", 2026-10-31). The
// floor cannot absorb such a thing because it moves between bins, so before the hog hold (vibe_ism.h kHoldQuietS) each
// kept a channel busy for good. Its level is given as a multiple of the recording's own RMS (2 = well over the floor in one
// detector bin, and clear of the u8 quantisation step).
// For the Pi 2 thread-split bench (docs/v12/DECODER-SIMD-AUDIT.md). Deterministic (fixed seeds).
// Build: g++ -std=c++17 -O2 -o add-hogs tools/ism-bench/add-hogs.cpp
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: %s <in.u8> <out.u8> <rate Hz> <level x RMS> <offset Hz>...\n", argv[0]); return 2; }
    FILE* in = std::fopen(argv[1], "rb");
    FILE* out = std::fopen(argv[2], "wb");
    if (!in || !out) { std::perror("open"); return 1; }
    const double rate = std::atof(argv[3]);
    struct Hog { double off, sweepHz, ph = 0; bool on = true; size_t next = 0; std::mt19937 g; };
    std::vector<Hog> hogs;
    const double level = std::atof(argv[4]);
    for (int i = 5; i < argc; ++i) hogs.push_back(Hog{std::atof(argv[i]), 0.5 + 0.08 * (i - 5), 0, true, 0, std::mt19937(uint32_t(100 + i))});
    std::uniform_real_distribution<double> onS(0.002, 0.015), offS(0.001, 0.010);
    // the recording's RMS, from its first second
    std::vector<unsigned char> u(size_t(rate) * 2);
    size_t n = std::fread(u.data(), 1, u.size(), in);
    double acc = 0;
    for (size_t i = 0; i < n; ++i) { const double v = (u[i] - 127.5) / 128.0; acc += v * v; }
    const double rms = std::sqrt(acc / double(std::max<size_t>(n / 2, 1)));
    const double amp = level * rms;
    std::fprintf(stderr, "recording RMS %.4f; %zu emitters at %.4f each\n", rms, hogs.size(), amp);
    std::rewind(in);
    size_t s = 0;
    std::vector<unsigned char> buf(1 << 20);
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) >= 2) {
        for (size_t i = 0; i + 1 < n; i += 2, ++s) {
            double re = (buf[i] - 127.5) / 128.0, im = (buf[i + 1] - 127.5) / 128.0;
            for (auto& h : hogs) {
                if (s >= h.next) { h.on = !h.on; h.next = s + size_t((h.on ? onS(h.g) : offS(h.g)) * rate); }
                const double f = h.off + 30e3 * std::sin(2 * M_PI * h.sweepHz * double(s) / rate);
                h.ph = std::fmod(h.ph + 2 * M_PI * f / rate, 2 * M_PI);
                if (h.on) { re += amp * std::cos(h.ph); im += amp * std::sin(h.ph); }
            }
            auto q = [](double v) { const long k = std::lround(v * 128.0 + 127.5); return (unsigned char)(k < 0 ? 0 : k > 255 ? 255 : k); };
            buf[i] = q(re); buf[i + 1] = q(im);
        }
        std::fwrite(buf.data(), 1, n & ~size_t(1), out);
    }
    std::fclose(in); std::fclose(out);
    return 0;
}
