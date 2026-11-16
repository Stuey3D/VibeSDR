// rade-bench.cpp — what one RADE V1 receiver costs on THIS machine, measured, not estimated (docs/v12/FREEDV.md "Cost").
//
// A standalone tool, never part of the server: it links only librade_vibe.a (+ librade_vibe_tx.a to make signals), so it
// can be copied to a box that is running a live VibeServer and run beside it without touching the server, its radios or
// its dial. The signals are made ONCE (on the Mac) and copied with it, so every machine decodes exactly the same samples.
//
//   rade-bench make  <speech.wav> <snr dB> <out.f32>   rade_c's own transmitter: 1 s quiet, the over, its end-of-over
//                                                      frame (G4ABC), 1 s quiet — white noise at <snr> in 3 kHz throughout
//   rade-bench noise <seconds> <out.f32>               band noise only: what a receiver does while nobody transmits
//   rade-bench wav   <in.wav> <out.f32>                a recording (any multiple of 8 kHz) taken down to 8 kHz
//   rade-bench run   <in.f32> [runs]                   decode it [runs] times (default 5); per run and the MEDIAN:
//                                                      this thread's CPU time ÷ the audio's length = % of one core
//
// The figure is the receiver alone (vrade_rx_process: acquisition, demodulator, decoder network, FARGAN) on one thread —
// the server's vibe-freedv thread does the same work plus a 48 → 8 kHz and a 16 → 48 kHz resampler (a few % of it).
// VIBE_RADE_ARCH (tests only) forces a lower DNN path; the "path" line says which one ran. The "pcm" hash shows whether
// two paths or two machines made the same speech (the NEON and dot-product paths should; AVX2's FMA need not).
//
// Build (Mac):   clang++ -O2 -std=c++17 -I <prefix>/include rade-bench.cpp <prefix>/lib/librade_vibe_tx.a <prefix>/lib/librade_vibe.a
// Build (Linux): add -static (and -lpthread); the transmitter's archive goes FIRST (it takes its end-of-over bits from
//                the receiver's). -DRADE_BENCH_NO_ARCH for an archive from before runtime dispatch (no vrade_rx_arch_name).
//
// Copyright (c) 2026 VibeSDR. Licensed as VibeServer (GPL-3.0).
#include "vibe_rade.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <time.h>
#include <vector>

static double threadCpu() {
    timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}
static double wall() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
}

static std::vector<float> readWav(const char* p, int* rate) {
    std::vector<float> o;
    *rate = 0;
    FILE* f = std::fopen(p, "rb");
    if (!f) return o;
    char h[12];
    if (std::fread(h, 1, 12, f) != 12) { std::fclose(f); return o; }
    int ch = 1;
    for (;;) {
        char id[4]; uint32_t sz;
        if (std::fread(id, 1, 4, f) != 4 || std::fread(&sz, 4, 1, f) != 1) break;
        if (!std::memcmp(id, "fmt ", 4)) {
            std::vector<char> b(sz);
            if (std::fread(b.data(), 1, sz, f) != sz) break;
            uint16_t c; uint32_t r; std::memcpy(&c, &b[2], 2); std::memcpy(&r, &b[4], 4); ch = c; *rate = int(r);
        } else if (!std::memcmp(id, "data", 4)) {
            std::vector<int16_t> d(sz / 2);
            d.resize(std::fread(d.data(), 2, d.size(), f));
            for (size_t i = 0; i + size_t(ch) <= d.size(); i += size_t(ch)) o.push_back(float(d[i]) / 32768.f);
            break;
        } else {
            std::fseek(f, long(sz), SEEK_CUR);
        }
    }
    std::fclose(f);
    return o;
}
static bool writeF32(const char* p, const std::vector<float>& x) {
    FILE* f = std::fopen(p, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(x.data(), sizeof(float), x.size(), f) == x.size();
    return std::fclose(f) == 0 && ok;
}
static std::vector<float> readF32(const char* p) {
    std::vector<float> x;
    FILE* f = std::fopen(p, "rb");
    if (!f) return x;
    std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    x.resize(size_t(n) / sizeof(float));
    x.resize(std::fread(x.data(), sizeof(float), x.size(), f));
    std::fclose(f);
    return x;
}
// windowed-sinc decimation by an integer factor (a recording at 16/48 kHz → the modem's 8 kHz)
static std::vector<float> decimate(const std::vector<float>& x, int k) {
    if (k == 1) return x;
    const int N = 64 * k + 1; std::vector<double> h(static_cast<size_t>(N)); double fc = 0.45 / k, s = 0;
    for (int i = 0; i < N; ++i) {
        const double t = i - (N - 1) / 2.0, w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (N - 1));
        h[size_t(i)] = (t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t)) * w; s += h[size_t(i)];
    }
    for (auto& v : h) v /= s;
    std::vector<float> o;
    for (size_t n = 0; n + size_t(N) < x.size(); n += size_t(k)) {
        double a = 0; for (int i = 0; i < N; ++i) a += h[size_t(i)] * x[n + size_t(i)];
        o.push_back(float(a));
    }
    return o;
}

struct Result { double cpuPct, wallPct, firstSync, speechSec, snr; int syncs, frames, uw, eoo; std::string call; uint64_t hash; };

static Result decodeOnce(const std::vector<float>& m8) {
    Result R{}; R.firstSync = -1;
    vrade_rx* r = vrade_rx_open();
    if (!r) { std::fprintf(stderr, "vrade_rx_open failed\n"); std::exit(1); }
    std::vector<float> pcm(static_cast<size_t>(vrade_rx_pcm_max()));
    uint64_t h = 1469598103934665603ull;   // FNV-1a over the speech's bytes
    size_t pos = 0, npcm = 0; int was = 0;
    const double c0 = threadCpu(), w0 = wall();
    while (pos + size_t(vrade_rx_nin(r)) <= m8.size()) {
        const int nin = vrade_rx_nin(r); int info = 0;
        const int n = vrade_rx_process(r, m8.data() + pos, pcm.data(), &info);
        pos += size_t(nin);
        const unsigned char* b = reinterpret_cast<const unsigned char*>(pcm.data());
        for (size_t i = 0; i < size_t(n) * sizeof(float); ++i) { h ^= b[i]; h *= 1099511628211ull; }
        npcm += size_t(n);
        const int s = vrade_rx_sync(r);
        if (s && !was) { ++R.syncs; if (R.firstSync < 0) R.firstSync = double(pos) / 8000.0; }
        if (s) R.snr = vrade_rx_snr_db(r);
        was = s;
        if (info & VRADE_INFO_FRAME) { ++R.frames; if (info & VRADE_INFO_UWERR) ++R.uw; }
        if (info & VRADE_INFO_EOO) ++R.eoo;
    }
    const double cpu = threadCpu() - c0, el = wall() - w0, audio = double(m8.size()) / 8000.0;
    R.cpuPct = 100.0 * cpu / audio; R.wallPct = 100.0 * el / audio;
    R.speechSec = double(npcm) / 16000.0; R.call = vrade_rx_callsign(r); R.hash = h;
    vrade_rx_close(r);
    return R;
}

static int usage() {
    std::fprintf(stderr, "usage: rade-bench make <speech.wav> <snr dB> <out.f32> | noise <sec> <out.f32> | wav <in.wav> <out.f32> | run <in.f32> [runs]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string mode = argv[1];
    if (mode == "make" && argc >= 5) {
#if defined(RADE_BENCH_NO_TX)
        std::fprintf(stderr, "built without the transmitter\n"); return 2;
#else
        int rate = 0; std::vector<float> sp = readWav(argv[2], &rate);
        if (rate != 16000 || sp.empty()) { std::fprintf(stderr, "%s: need 16 kHz mono speech (got %d Hz)\n", argv[2], rate); return 1; }
        const double snr = std::atof(argv[3]);
        vrade_tx* t = vrade_tx_open();
        std::vector<float> m8(size_t(vrade_tx_frame_samples()) * (2 + sp.size() / 1920));
        const int n = vrade_tx_speech(t, sp.data(), int(sp.size()), m8.data(), nullptr);
        std::vector<float> sig(8000, 0.f);
        sig.insert(sig.end(), m8.begin(), m8.begin() + n);
        std::vector<float> e(static_cast<size_t>(vrade_tx_eoo_samples()));
        vrade_tx_eoo(t, "G4ABC", e.data(), nullptr);
        sig.insert(sig.end(), e.begin(), e.end());
        sig.insert(sig.end(), 8000, 0.f);
        vrade_tx_close(t);
        double p = 0; for (int i = 0; i < n; ++i) p += double(sig[size_t(8000 + i)]) * sig[size_t(8000 + i)];
        p /= n;
        // white noise at 8 kHz spans 4 kHz: its power in 3 kHz is σ²·3/4
        const double sigma = std::sqrt(p / std::pow(10.0, snr / 10.0) * 4.0 / 3.0), k = 0.1 / std::sqrt(p);
        std::mt19937 g(1); std::normal_distribution<float> N(0.f, 1.f);
        for (auto& v : sig) v = float((v + sigma * N(g)) * k);
        std::printf("made %.1f s: the over %.1f s at %.1f dB in 3 kHz\n", double(sig.size()) / 8000.0, n / 8000.0, snr);
        return writeF32(argv[4], sig) ? 0 : 1;
#endif
    }
    if (mode == "noise" && argc >= 4) {
        std::mt19937 g(3); std::normal_distribution<float> N(0.f, 0.1f);
        std::vector<float> m8(size_t(8000 * std::atoi(argv[2])));
        for (auto& v : m8) v = N(g);
        return writeF32(argv[3], m8) ? 0 : 1;
    }
    if (mode == "wav" && argc >= 4) {
        int rate = 0; const std::vector<float> x = readWav(argv[2], &rate);
        if (rate <= 0 || rate % 8000) { std::fprintf(stderr, "%s: rate %d is not a multiple of 8 kHz\n", argv[2], rate); return 1; }
        return writeF32(argv[3], decimate(x, rate / 8000)) ? 0 : 1;
    }
    if (mode == "run") {
        const std::vector<float> m8 = readF32(argv[2]);
        if (m8.empty()) { std::fprintf(stderr, "%s: empty or unreadable\n", argv[2]); return 1; }
        const int runs = argc > 3 ? std::max(1, std::atoi(argv[3])) : 5;
#if !defined(RADE_BENCH_NO_ARCH)
        std::printf("path: %s\n", vrade_rx_arch_name());
#else
        std::printf("path: (archive from before runtime dispatch)\n");
#endif
        std::printf("input: %s, %.1f s at 8 kHz\n", argv[2], double(m8.size()) / 8000.0);
        std::vector<double> cpus;
        Result last{};
        for (int i = 0; i < runs; ++i) {
            last = decodeOnce(m8);
            cpus.push_back(last.cpuPct);
            std::printf("  run %d: %.2f %% of a core (wall %.2f %%)\n", i + 1, last.cpuPct, last.wallPct);
            std::fflush(stdout);
        }
        std::sort(cpus.begin(), cpus.end());
        std::printf("median %.2f %% of a core (min %.2f, max %.2f) over %d runs\n", cpus[cpus.size() / 2], cpus.front(), cpus.back(), runs);
        std::printf("decode: first sync %.2f s, syncs %d, frames %d (unique-word errors %d), end-of-over %d, call '%s', snr %.1f dB, "
                    "speech %.1f s, pcm %016llx\n", last.firstSync, last.syncs, last.frames, last.uw, last.eoo, last.call.c_str(),
                    last.snr, last.speechSec, (unsigned long long)last.hash);
        return 0;
    }
    return usage();
}
