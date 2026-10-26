// jtty_bench — the JTTY receiver's measuring bench (scripts/test-jtty.sh builds and runs it; vibeserver/test-jtty.cpp
// holds the pass/fail checks).
//
//   jtty_bench wav <file.wav>...            recordings (mono/stereo int16 or float, 12 or 48 kHz) → every frame, every message
//   jtty_bench cpu [seconds]                CPU per second of 12 kHz audio: a busy passband (six stations) and pure noise
//   jtty_bench clip <out.cu8> <rate> <seconds> <centreHz> <dialHz>
//                                           the server tests' IQ clip: unsigned 8-bit IQ at `rate` centred on `centreHz`,
//                                           two JTTY stations (a CQ, then an exchange) at dial + 1000 / + 1500 Hz on USB
//   jtty_bench synth <out.wav> <message> [f0] [snrDb]   one transmission as a 12 kHz mono int16 WAV, in noise
//
// ★ Silent: nothing is played. Deterministic: every draw is seeded.
#include "decoders/jtty_decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace vibe::jtty;

namespace {
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double uni() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    double gauss() { double u, v, r; do { u = 2 * uni() - 1; v = 2 * uni() - 1; r = u * u + v * v; } while (r >= 1 || r == 0); return u * std::sqrt(-2 * std::log(r) / r); }
};
std::vector<uint8_t> tonesOf(const std::string& msg) {
    std::vector<uint64_t> fr; packMessage(msg, fr);
    std::vector<uint8_t> t;
    for (auto w : fr) { uint8_t x[kFrameSyms]; encodeFrame(w, x); t.insert(t.end(), x, x + kFrameSyms); }
    return t;
}
bool readWav(const std::string& path, std::vector<float>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    int fmt = 0, ch = 0, rate = 0, bits = 0; size_t p = 12, at = 0, len = 0;
    while (p + 8 <= b.size()) {
        uint32_t l; std::memcpy(&l, b.data() + p + 4, 4);
        if (!std::memcmp(b.data() + p, "fmt ", 4)) {
            uint16_t v; std::memcpy(&v, b.data() + p + 8, 2); fmt = v; std::memcpy(&v, b.data() + p + 10, 2); ch = v;
            uint32_t r; std::memcpy(&r, b.data() + p + 12, 4); rate = (int)r; std::memcpy(&v, b.data() + p + 22, 2); bits = v;
            if (fmt == 0xFFFE && l >= 26) { std::memcpy(&v, b.data() + p + 32, 2); fmt = v; }
        } else if (!std::memcmp(b.data() + p, "data", 4)) { at = p + 8; len = std::min<size_t>(l, b.size() - at); break; }
        p += 8 + l + (l & 1);
    }
    if (!at || ch < 1 || (rate != 12000 && rate != 48000)) return false;
    const int bps = bits / 8; const size_t n = len / (size_t)(bps * ch);
    std::vector<float> mono(n);
    for (size_t i = 0; i < n; ++i) {
        const char* s = b.data() + at + i * (size_t)(bps * ch);
        if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, s, 4); mono[i] = v; }
        else if (fmt == 1 && bits == 16) { int16_t v; std::memcpy(&v, s, 2); mono[i] = v / 32768.0f; }
        else return false;
    }
    out.clear();
    if (rate == 12000) out = mono;
    else for (size_t i = 0; i + 4 <= n; i += 4) out.push_back(0.25f * (mono[i] + mono[i + 1] + mono[i + 2] + mono[i + 3]));
    return true;
}

int wav(int argc, char** argv) {
    int total = 0;
    for (int a = 2; a < argc; ++a) {
        std::vector<float> x;
        if (!readWav(argv[a], x)) { std::printf("%s: not a WAV this bench reads\n", argv[a]); continue; }
        Decoder::Config c; Decoder d(c); d.setStartUtcMs(0);
        int nf = 0, nm = 0;
        d.onFrame = [&](const Frame& f) {
            ++nf;
            std::printf("  %7.2f s  %6.1f Hz  %+5.1f dB  %-6s  rank %d  agree %2d %s %s\n", f.sample / 12000.0, f.audioHz, f.snrDb,
                        qualityWord(f.quality), f.rank, f.agree, f.continuation ? "cont" : "    ", render(f.atom).c_str());
        };
        d.onMessage = [&](int id, double hz, const std::string& t, bool complete) { if (complete) { ++nm; std::printf("  MESSAGE #%d %.0f Hz: %s\n", id, hz, t.c_str()); } };
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < x.size(); i += 1200) d.feed(x.data() + i, (int)std::min<size_t>(1200, x.size() - i));
        std::vector<float> z(12000 * 6, 0.0f); d.feed(z.data(), (int)z.size());
        const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%s: %.0f s, %d frames, %d messages, %llu candidates (%.0f/s), %.0f× real time\n", argv[a], x.size() / 12000.0, nf, nm,
                    (unsigned long long)d.candidates(), d.candidates() / (x.size() / 12000.0), x.size() / 12000.0 / cpu);
        total += nf;
    }
    return 0;
}

int cpu(double seconds) {
    for (int busy = 1; busy >= 0; --busy) {
        Rng r(9);
        std::vector<float> x((size_t)(seconds * 12000), 0.0f);
        if (busy) {
            const char* msgs[] = { "CQ KA1ABC CQ", "WB9XYZ 599 101", "TU NOW JA6DEF 599 102", "THE QUICK BROWN FOX", "1D EMA", "CQ DL1XYZ CQ" };
            const double f0s[] = { 400, 650, 900, 1300, 1800, 2400 };
            for (int s = 0; s < 6; ++s) {
                const auto t = tonesOf(msgs[s]);
                for (double at = 0.2 * s; at + t.size() / kBaud + 1 < seconds; at += t.size() / kBaud + 2.3 + 0.7 * s)
                    synthesise(t.data(), (int)t.size(), f0s[s], 12000, 0.29f, x, (size_t)(at * 12000));
            }
        }
        for (auto& v : x) v += (float)r.gauss();
        Decoder::Config c; Decoder d(c);
        uint64_t frames = 0; d.onFrame = [&](const Frame&) { ++frames; };
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < x.size(); i += 1200) d.feed(x.data() + i, (int)std::min<size_t>(1200, x.size() - i));
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%-6s %-26s %.0f s: %.2f s CPU = %.2f %% of one core (%s), %llu candidates, %llu frames\n", Decoder::simdName(),
                    busy ? "six stations, −10 dB" : "white noise", seconds, s, 100.0 * s / seconds, Decoder::simdName(),
                    (unsigned long long)d.candidates(), (unsigned long long)frames);
    }
    return 0;
}

int clip(const char* out, double rate, double seconds, double centre, double dial) {
    // the two stations' complex baseband at 12 kHz (I = cos, Q = sin of one phase), then up to `rate` by linear
    // interpolation and onto their RF: USB, so RF = dial + audio
    struct St { std::string msg; double audio; double at; };
    const St st[] = { { "CQ G4ABC CQ", 1000.0, 1.0 }, { "DL1XYZ 599 042", 1500.0, 2.5 } };
    const size_t n12 = (size_t)(seconds * 12000);
    std::vector<float> I(n12, 0.0f), Q(n12, 0.0f);
    for (auto& s : st) {
        const auto t = tonesOf(s.msg);
        const double off = dial + s.audio - centre;
        synthesise(t.data(), (int)t.size(), off, 12000, 1.0f, Q, (size_t)(s.at * 12000), 0.0);
        synthesise(t.data(), (int)t.size(), off, 12000, 1.0f, I, (size_t)(s.at * 12000), M_PI / 2);
    }
    I.resize(n12); Q.resize(n12);
    const size_t n = (size_t)(seconds * rate);
    std::vector<uint8_t> b(n * 2);
    Rng rng(2026);
    auto q = [](double v) { const int x = (int)std::lround(127.5 + v * 18.0); return (uint8_t)std::max(0, std::min(255, x)); };
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i * 12000.0 / rate;
        const size_t k = (size_t)t; const double fr = t - (double)k;
        const double i0 = k < n12 ? I[k] : 0, i1 = k + 1 < n12 ? I[k + 1] : 0, q0 = k < n12 ? Q[k] : 0, q1 = k + 1 < n12 ? Q[k + 1] : 0;
        const double re = 0.12 * (i0 + fr * (i1 - i0)) + 0.707 * rng.gauss(), im = 0.12 * (q0 + fr * (q1 - q0)) + 0.707 * rng.gauss();
        b[2 * i] = q(re); b[2 * i + 1] = q(im);
    }
    FILE* f = std::fopen(out, "wb"); if (!f) { std::perror(out); return 1; }
    std::fwrite(b.data(), 1, b.size(), f); std::fclose(f);
    std::printf("%s: %.0f s at %.0f Hz centred on %.0f; JTTY on %.0f USB: \"%s\" at +%.0f Hz, \"%s\" at +%.0f Hz\n", out, seconds, rate,
                centre, dial, st[0].msg.c_str(), st[0].audio, st[1].msg.c_str(), st[1].audio);
    return 0;
}

int synth(const char* out, const std::string& msg, double f0, double snr) {
    const auto t = tonesOf(msg);
    std::vector<float> x(12000, 0.0f);
    synthesise(t.data(), (int)t.size(), f0, 12000, 1.0f, x, 6000);
    x.resize(x.size() + 12000, 0.0f);
    // ★ SNR in 2500 Hz: unit sine power ½ against noise σ² · 2500/6000
    const double sigma = std::sqrt(0.5 / (std::pow(10.0, snr / 10.0) * 2500.0 / 6000.0));
    Rng r(5);
    float peak = 0;
    for (auto& v : x) { v += (float)(sigma * r.gauss()); peak = std::max(peak, std::fabs(v)); }
    std::vector<int16_t> pcm(x.size());
    for (size_t i = 0; i < x.size(); ++i) pcm[i] = (int16_t)std::lround(x[i] / peak * 30000.0);
    FILE* f = std::fopen(out, "wb"); if (!f) { std::perror(out); return 1; }
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); }; auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + (uint32_t)pcm.size() * 2); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(1); u32(12000); u32(24000); u16(2); u16(16); std::fwrite("data", 1, 4, f); u32((uint32_t)pcm.size() * 2);
    std::fwrite(pcm.data(), 2, pcm.size(), f); std::fclose(f);
    std::printf("%s: \"%s\" (%zu frames) at %.0f Hz, %.0f dB\n", out, normalise(msg).c_str(), t.size() / kFrameSyms, f0, snr);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    const std::string m = argc > 1 ? argv[1] : "";
    if (m == "wav" && argc > 2) return wav(argc, argv);
    if (m == "cpu") return cpu(argc > 2 ? std::atof(argv[2]) : 300);
    if (m == "clip" && argc > 6) return clip(argv[2], std::atof(argv[3]), std::atof(argv[4]), std::atof(argv[5]), std::atof(argv[6]));
    if (m == "synth" && argc > 3) return synth(argv[2], argv[3], argc > 4 ? std::atof(argv[4]) : 1500, argc > 5 ? std::atof(argv[5]) : -10);
    std::fprintf(stderr, "usage: jtty_bench wav <file.wav>... | cpu [seconds] | clip <out.cu8> <rate> <seconds> <centreHz> <dialHz> | synth <out.wav> <message> [f0] [snrDb]\n");
    return 2;
}
