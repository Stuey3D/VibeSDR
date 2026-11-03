// bench-wsp-strong.cpp — what the Broadcast FM "NR" (weak-signal processing: L-R high-blend + audio high-cut) does on
// a STRONG station, measured. Saber, 2026-11-03: "the NR causes a lot of audio-glitches … Even my strong 87 MHz signal
// will have weird audio … because of the NR".
//
// A noise-free FM carrier (the strongest possible signal) carrying REAL programme, built the way a broadcaster builds
// it: 50 µs pre-emphasis, a 15 kHz audio low-pass, and then one of three processing chains —
//   clean      peak-normalised, nothing clipped (the textbook transmitter)
//   loud       audio hard-clipped 6 dB hot AFTER the 15 kHz filter (a loud, cheap chain: harmonics above 15 kHz)
//   composite  the MPX driven 1.5 dB hot into a composite clipper (common on commercial FM)
// For each, every 100 ms: the MPX S/N the NR steers by (pilot vs the 17 kHz guard band), the L-R corner and the audio
// high-cut, with NR on. Also the same carrier with real noise on the IQ, so the curve is seen doing its proper job.
//
//   bench-wsp-strong [programme.wav]   (a 48 kHz stereo float WAV; without one a synthetic programme is used)
// ★ Silent: nothing is played. Recordings stay local and are never committed.
#include "../android/app/src/main/cpp/vibedsp/vibedsp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using vibedsp::RxPipeline;
using vibedsp::cf32;

namespace {
double g_packetMs = 100.0;
constexpr double kFs = 1000000.0, kAudFs = 48000.0, kDev = 75000.0, kPilot = 19000.0;

bool readWav(const char* path, std::vector<float>& L, std::vector<float>& R) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<unsigned char> b;
    unsigned char tmp[65536]; size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, f)) > 0) b.insert(b.end(), tmp, tmp + n);
    std::fclose(f);
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    size_t p = 12; int ch = 0, bits = 0, fmt = 0; double rate = 0;
    while (p + 8 <= b.size()) {
        const uint32_t sz = b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | ((uint32_t)b[p + 7] << 24);
        if (!std::memcmp(b.data() + p, "fmt ", 4)) {
            fmt = b[p + 8] | (b[p + 9] << 8); ch = b[p + 10];
            rate = b[p + 12] | (b[p + 13] << 8) | (b[p + 14] << 16);
            bits = b[p + 22];
            if (fmt == 0xFFFE && p + 8 + 26 <= b.size()) fmt = b[p + 8 + 24] | (b[p + 8 + 25] << 8);
        } else if (!std::memcmp(b.data() + p, "data", 4)) {
            if (ch != 2 || bits != 32 || fmt != 3 || rate != kAudFs) return false;
            const size_t frames = std::min<size_t>(sz, b.size() - p - 8) / 8;
            const float* d = reinterpret_cast<const float*>(b.data() + p + 8);
            L.resize(frames); R.resize(frames);
            for (size_t i = 0; i < frames; ++i) { L[i] = d[2 * i]; R[i] = d[2 * i + 1]; }
            return true;
        }
        p += 8 + sz + (sz & 1);
    }
    return false;
}

/** A synthetic programme with real HF content: pink-ish noise bursts and a melody, different in each ear. */
void synthProgramme(std::vector<float>& L, std::vector<float>& R, double secs) {
    std::mt19937 rng(7); std::normal_distribution<float> g(0.f, 1.f);
    const size_t n = (size_t)(secs * kAudFs); L.resize(n); R.resize(n);
    float b0 = 0, b1 = 0, c0 = 0, c1 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = i / kAudFs;
        const float env = 0.5f + 0.5f * (float)std::sin(2 * M_PI * 0.7 * t);
        b0 = 0.97f * b0 + 0.03f * g(rng); b1 = 0.6f * b1 + 0.4f * g(rng);
        c0 = 0.97f * c0 + 0.03f * g(rng); c1 = 0.6f * c1 + 0.4f * g(rng);
        L[i] = env * (2.5f * b0 + 0.25f * b1) + 0.3f * (float)std::sin(2 * M_PI * 440 * t);
        R[i] = env * (2.5f * c0 + 0.25f * c1) + 0.3f * (float)std::sin(2 * M_PI * 660 * t);
    }
}

/** Windowed-sinc low-pass at 48 kHz (the broadcaster's 15 kHz brick wall, near enough). */
void lowpass15k(std::vector<float>& x) {
    const int N = 127; std::vector<float> h(N);
    const double fc = 15000.0 / kAudFs;
    double s = 0;
    for (int i = 0; i < N; ++i) {
        const double m = i - (N - 1) / 2.0;
        const double sinc = m == 0 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (N - 1)) + 0.08 * std::cos(4 * M_PI * i / (N - 1));
        h[i] = (float)(sinc * w); s += h[i];
    }
    for (auto& v : h) v = (float)(v / s);
    std::vector<float> y(x.size(), 0.f);
    for (size_t i = 0; i < x.size(); ++i) {
        double a = 0;
        for (int k = 0; k < N; ++k) { const long j = (long)i - k; if (j >= 0) a += h[k] * x[(size_t)j]; }
        y[i] = (float)a;
    }
    x.swap(y);
}

void preEmphasis(std::vector<float>& x) {
    // 50 µs: H = 1 + k·(1 − z⁻¹), k = tau·fs — within 0.1 dB of 1 + s·tau up to 15 kHz at 48 kHz (+13.6 dB there)
    const double k = 50e-6 * kAudFs;
    double x1 = 0;
    for (auto& v : x) { const double y = v + k * (v - x1); x1 = v; v = (float)y; }
}

/** echo: a second ray `echoAmp` strong, `echoUs` late, its delay swinging ±`echoWobble` µs at 0.5 Hz (a car, a tree
 *  in the wind) — multipath on a STRONG signal. adj: a neighbour `adjAmp` strong at +200 kHz (steady carrier). */
struct Chain { const char* name; bool clipAudio; bool compositeClip; double noise;
               double echoAmp = 0, echoUs = 0, echoWobble = 0, adjAmp = 0; };

struct Gen {
    std::vector<float> L, R;   // processed audio at 48 kHz, peak ±1
    const Chain* c = nullptr;
    double t = 0, phase = 0;
    size_t i48 = 0; double frac = 0;
    std::mt19937 rng{99}; std::normal_distribution<double> g{0.0, 1.0};
    std::vector<double> hist = std::vector<double>(64, 0.0); size_t hpos = 0; double adjPh = 0;
    void fill(std::vector<cf32>& out, int n) {
        out.resize((size_t)n);
        const double step = kAudFs / kFs;
        for (int k = 0; k < n; ++k) {
            const size_t a = i48 % L.size(), b = (i48 + 1) % L.size();
            const double l = L[a] + (L[b] - L[a]) * frac, r = R[a] + (R[b] - R[a]) * frac;
            frac += step; while (frac >= 1.0) { frac -= 1.0; ++i48; }
            const double sp = 2 * M_PI * 2 * kPilot * t;
            double mpx = 0.45 * (l + r) + 0.45 * (l - r) * std::sin(sp) + 0.09 * std::sin(2 * M_PI * kPilot * t);
            if (c->compositeClip) { mpx *= 1.19; mpx = std::max(-1.0, std::min(1.0, mpx)); }
            phase += 2 * M_PI * kDev * mpx / kFs;
            if (phase > M_PI * 1e6) phase -= M_PI * 2e6;
            double re = std::cos(phase), im = std::sin(phase);
            if (c->echoAmp > 0) {
                hist[hpos] = phase; hpos = (hpos + 1) % hist.size();
                const double dUs = c->echoUs + c->echoWobble * std::sin(2 * M_PI * 0.5 * t);
                const size_t d = (size_t)std::max(1.0, dUs * kFs / 1e6);
                const double ph = hist[(hpos + hist.size() - 1 - std::min(d, hist.size() - 2)) % hist.size()];
                re += c->echoAmp * std::cos(ph + 1.0); im += c->echoAmp * std::sin(ph + 1.0);
            }
            if (c->adjAmp > 0) {
                adjPh += 2 * M_PI * (200000.0 + 40000.0 * std::sin(2 * M_PI * 600 * t)) / kFs;
                re += c->adjAmp * std::cos(adjPh); im += c->adjAmp * std::sin(adjPh);
            }
            if (c->noise > 0) { re += c->noise * g(rng); im += c->noise * g(rng); }
            out[(size_t)k] = cf32{ (float)re, (float)im };
            t += 1.0 / kFs;
        }
    }
};

struct Sink { static void onAudio(void*, const float*, int, int, int) {} static void onStereo(void*, bool) {} };

void runChain(const Chain& c, const std::vector<float>& L0, const std::vector<float>& R0, double secs) {
    Gen g; g.c = &c; g.L = L0; g.R = R0;
    for (auto* ch : { &g.L, &g.R }) {
        preEmphasis(*ch); lowpass15k(*ch);
        float pk = 1e-9f; for (float v : *ch) pk = std::max(pk, std::fabs(v));
        const float gain = c.clipAudio ? 2.0f / pk : 1.0f / pk;   // 'loud': 6 dB into the clipper
        for (auto& v : *ch) { v *= gain; if (c.clipAudio) v = std::max(-1.0f, std::min(1.0f, v)); }
    }
    RxPipeline rx; Sink s; RxPipeline::Callbacks cb{}; cb.ctx = &s; cb.audio = &Sink::onAudio; cb.stereo = &Sink::onStereo;
    rx.start(kFs, 1024, 10.0, 48000, cb);
    rx.setTune(0.0, RxPipeline::Mode::WFM, 250000.0);
    rx.setWeakSignalProc(true);
    std::vector<cf32> buf;
    std::vector<float> snr, lmr, cut;
    // ★ The radio's packet size: what one feed() — one pipeline call — carries. An RTL hands over ~16-32 ms, an RSP or
    //   an Airspy far less; every smoother that counts CALLS instead of time runs faster on the small ones.
    const int pkt = std::max(256, (int)(g_packetMs * kFs / 1000.0));
    const int block = 100000;   // one reading every 100 ms
    for (int k = 0; k < (int)(secs * 10); ++k) {
        g.fill(buf, block);
        for (int o = 0; o < block; o += pkt) rx.feed(buf.data() + o, std::min(pkt, block - o));
        if (k >= 30) { snr.push_back(rx.blendSnrDb()); lmr.push_back(rx.lmrHiCutHz()); cut.push_back(rx.audioHiCutHz()); }   // after 3 s
    }
    auto pct = [](std::vector<float> v, double q) { std::sort(v.begin(), v.end()); return v.empty() ? 0.f : v[(size_t)(q * (v.size() - 1))]; };
    int narrowed = 0, moves = 0; float lastBand = -1;
    for (size_t i = 0; i < lmr.size(); ++i) {
        if (lmr[i] < 14000.f || cut[i] < 14000.f) ++narrowed;
        const float band = std::round(std::min(lmr[i], cut[i]) / 1000.f);
        if (lastBand >= 0 && band != lastBand) ++moves;
        lastBand = band;
    }
    std::printf("  %-10s noise %.2f | MPX S/N  min %5.1f  p10 %5.1f  med %5.1f  max %5.1f dB | L-R corner min %6.0f med %6.0f Hz"
                " | high-cut min %6.0f Hz | acting %3.0f%% of the time, corner moved %d times in %zu s\n",
                c.name, c.noise, pct(snr, 0), pct(snr, 0.1), pct(snr, 0.5), pct(snr, 1), pct(lmr, 0), pct(lmr, 0.5), pct(cut, 0),
                100.0 * narrowed / std::max<size_t>(1, lmr.size()), moves, lmr.size() / 10);
}
}  // namespace

int main(int argc, char** argv) {
    if (const char* e = std::getenv("PACKET_MS")) g_packetMs = std::atof(e);
    std::printf("radio packet: %.2f ms per pipeline call\n", g_packetMs);
    std::vector<float> L, R;
    double secs = 30;
    if (argc > 1 && readWav(argv[1], L, R)) {
        const size_t want = (size_t)(secs * kAudFs);
        if (L.size() > want + (size_t)(10 * kAudFs)) { L.erase(L.begin(), L.begin() + (long)(10 * kAudFs)); R.erase(R.begin(), R.begin() + (long)(10 * kAudFs)); }
        if (L.size() > want) { L.resize(want); R.resize(want); }
        std::printf("programme: %s (%.0f s)\n", argv[1], L.size() / kAudFs);
    } else {
        synthProgramme(L, R, secs);
        std::printf("programme: synthetic (%.0f s)\n", secs);
    }
    const Chain chains[] = {
        { "clean", false, false, 0.0 }, { "loud", true, false, 0.0 }, { "composite", false, true, 0.0 },
        { "clean", false, false, 0.30 }, { "clean", false, false, 0.60 }, { "clean", false, false, 0.90 },
        { "echo-10%", false, false, 0.0, 0.10, 8.0, 3.0 }, { "echo-30%", false, false, 0.0, 0.30, 8.0, 3.0 },
        { "echo-50%", false, false, 0.0, 0.50, 12.0, 4.0 },
        { "adj-0dB", false, false, 0.0, 0, 0, 0, 1.0 }, { "adj+6dB", false, false, 0.0, 0, 0, 0, 2.0 },
    };
    for (const auto& c : chains) runChain(c, L, R, secs);
    return 0;
}
