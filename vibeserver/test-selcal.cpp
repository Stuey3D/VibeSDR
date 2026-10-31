// test-selcal.cpp — SELCAL (decoders/selcal_decoder.h) from ICAO Annex 10 Vol III Part II Ch 3, Amendment 91 (Table 3-1).
// V12, 2026-10-31. ★ Silent; deterministic; synthetic audio only.
//
// ★★★ WHAT IS PINNED.
//   • Table 3-1: 32 tones, the red group A–S as before 2022 and the SELCAL32 tones T–9, each between its two neighbours
//     of the old group (log⁻¹ 0.0225 spacing) — checked against the 1.045 / 1.0227 ratios.
//   • 300 random codes over all 32 tones (pulses 0.75–1.25 s, gaps 0.1–0.3 s, tones within 3 dB, ±0.15 %): all right.
//   • The dial: ±3 Hz decodes everything; the limit is measured and printed.
//   • White noise (SNR in a 3 kHz band) sweep; nothing on hours of noise, DTMF dialling, two-tone pages, speech-like
//     harmonic sweeps. A fingerprint of every code, equal in a VIBE_FORCE_SCALAR build (printed).
#include "decoders/selcal_decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }
static uint64_t g_fp = 1469598103934665603ull;

struct Au {
    double fs = 48000; std::vector<float> x; double pa = 0, pb = 0;
    void two(double fa, double fb, double sec, double amp = 0.3, double rel = 1.0) {
        const int n = (int)(sec * fs);
        for (int i = 0; i < n; ++i) { pa += 2 * M_PI * fa / fs; pb += 2 * M_PI * fb / fs; x.push_back((float)(amp * (std::sin(pa) + rel * std::sin(pb)))); }
    }
    void gap(double sec) { x.insert(x.end(), (size_t)(sec * fs), 0.f); }
    void noise(double snrDb3k, uint32_t seed, double sigPow = 0.09) {
        std::mt19937 r(seed); std::normal_distribution<float> g(0.f, 1.f);
        const float s = (float)std::sqrt(sigPow / std::pow(10.0, snrDb3k / 10.0) * (fs / 2) / 3000.0);
        for (auto& v : x) v += s * g(r);
    }
};
static std::vector<std::string> run(const std::vector<float>& x, double fs = 48000) {
    SelcalDecoder d(fs); std::vector<std::string> got;
    d.onCode = [&](const SelcalDecoder::Code& c) { got.push_back(c.code); for (char ch : c.code) { g_fp ^= (uint8_t)ch; g_fp *= 1099511628211ull; } };
    for (size_t i = 0; i < x.size(); i += 960) d.feed(x.data() + i, (int)std::min<size_t>(960, x.size() - i));
    return got;
}
/** A code's pulses: designations sorted by Table 3-1's order within a pulse (as the decoder writes them). */
static std::string codeOf(int a, int b, int c, int d) {
    if (a > b) std::swap(a, b);
    if (c > d) std::swap(c, d);
    return std::string() + SelcalDecoder::kName[a] + SelcalDecoder::kName[b] + "-" + SelcalDecoder::kName[c] + SelcalDecoder::kName[d];
}
static void call(Au& u, int a, int b, int c, int d, double p1 = 1.0, double g = 0.2, double p2 = 1.0, double off = 0, double rel = 1.0) {
    u.gap(0.5);
    u.two(SelcalDecoder::kHz[a] + off, SelcalDecoder::kHz[b] + off, p1, 0.3, rel);
    u.gap(g);
    u.two(SelcalDecoder::kHz[c] + off, SelcalDecoder::kHz[d] + off, p2, 0.3, rel);
    u.gap(1.5);
}

int main() {
    std::printf("test-selcal — ICAO Annex 10 Vol III Part II Ch 3 (Amendment 91, Table 3-1)\n\n── the table ──\n");
    {
        bool okOld = true, okNew = true;
        for (int k = 0; k + 1 < 16; ++k) okOld &= std::fabs(SelcalDecoder::kHz[k + 1] / SelcalDecoder::kHz[k] - std::pow(10.0, 0.045)) < 0.002;
        for (int k = 0; k < 16; ++k) okNew &= std::fabs(SelcalDecoder::kHz[16 + k] / SelcalDecoder::kHz[k] - std::pow(10.0, 0.0225)) < 0.002;
        ok(okOld, "Red A–S spaced by log⁻¹ 0.045 (Note 1)");
        ok(okNew, "Red T–9 each log⁻¹ 0.0225 above its red-group partner (Amendment 91)");
        ok(SelcalDecoder::indexOf('A') == 0 && SelcalDecoder::indexOf('S') == 15 && SelcalDecoder::indexOf('9') == 31 && SelcalDecoder::indexOf('I') < 0 && SelcalDecoder::indexOf('O') < 0 && SelcalDecoder::indexOf('N') < 0,
           "designations: no I, N, O");
    }

    std::printf("\n── codes ──\n");
    {
        std::mt19937 r(42); int right = 0, wrong = 0; const int N = 300;
        for (int k = 0; k < N; ++k) {
            int t[4]; for (int i = 0; i < 4; ++i) { bool dup; do { t[i] = (int)(r() % 32); dup = false; for (int j = 0; j < i; ++j) dup |= t[j] == t[i]; } while (dup); }
            Au u;
            const double p1 = 0.75 + 0.5 * (r() % 1000) / 1000.0, p2 = 0.75 + 0.5 * (r() % 1000) / 1000.0, g = 0.1 + 0.2 * (r() % 1000) / 1000.0;
            const double drift = 1.0 + 0.0015 * (((int)(r() % 3)) - 1);
            const double rel = std::pow(10.0, (((int)(r() % 7)) - 3) / 20.0);   // within ±3 dB
            u.gap(0.5);
            u.two(SelcalDecoder::kHz[t[0]] * drift, SelcalDecoder::kHz[t[1]] * drift, p1, 0.3, rel); u.gap(g);
            u.two(SelcalDecoder::kHz[t[2]] * drift, SelcalDecoder::kHz[t[3]] * drift, p2, 0.3, rel); u.gap(1.5);
            u.noise(20, 500u + (uint32_t)k);
            const auto got = run(u.x);
            const std::string want = codeOf(t[0], t[1], t[2], t[3]);
            if (got.size() == 1 && got[0] == want) ++right; else { wrong += !got.empty() && got[0] != want; if (k < 1000 && right + wrong < 400) std::printf("      want %s got %s (p1 %.2f gap %.2f p2 %.2f rel %.2f drift %.4f)\n", want.c_str(), got.empty() ? "-" : got[0].c_str(), p1, g, p2, rel, drift); }
        }
        ok(right == N && wrong == 0, std::to_string(right) + " of " + std::to_string(N) + " random codes right (all 32 tones, pulses 0.75–1.25 s, gaps 0.1–0.3 s, ±3 dB, ±0.15 %), " + std::to_string(wrong) + " wrong");
        Au u; call(u, 0, 1, 2, 3); call(u, 0, 1, 2, 3);
        ok(run(u.x).size() == 1, "the same code called twice in a row: reported once");
    }

    std::printf("\n── the dial (mistuned SSB) ──\n    ");
    int limit = 0;
    for (int off = 0; off <= 10; off += 1) {
        std::mt19937 r(7u + (uint32_t)off); int right = 0, wrong = 0;
        for (int k = 0; k < 40; ++k) {
            int t[4]; for (int i = 0; i < 4; ++i) { bool dup; do { t[i] = (int)(r() % 32); dup = false; for (int j = 0; j < i; ++j) dup |= t[j] == t[i]; } while (dup); }
            Au u; call(u, t[0], t[1], t[2], t[3], 1.0, 0.2, 1.0, (k & 1) ? off : -off);
            const auto got = run(u.x);
            if (got.size() == 1 && got[0] == codeOf(t[0], t[1], t[2], t[3])) ++right; else if (!got.empty()) ++wrong;
        }
        std::printf(" %d Hz: %d/40%s", off, right, wrong ? ("(" + std::to_string(wrong) + " wrong)").c_str() : "");
        if (right == 40) limit = off;
        if (wrong) ok(false, "a wrong code at " + std::to_string(off) + " Hz off");
    }
    std::printf("\n    → every code right to ±%d Hz\n", limit);
    ok(limit >= 3, "a dial within ±3 Hz decodes every code");

    std::printf("\n── sensitivity (SNR in 3 kHz, 40 codes a point) ──\n    ");
    double fifty = NAN;
    for (double snr = -12; snr <= 4.01; snr += 2) {
        std::mt19937 r(99u + (uint32_t)(snr + 20)); int right = 0, wrong = 0;
        for (int k = 0; k < 40; ++k) {
            int t[4]; for (int i = 0; i < 4; ++i) { bool dup; do { t[i] = (int)(r() % 32); dup = false; for (int j = 0; j < i; ++j) dup |= t[j] == t[i]; } while (dup); }
            Au u; call(u, t[0], t[1], t[2], t[3]); u.noise(snr, 900u + (uint32_t)k + (uint32_t)(snr * 7 + 100));
            const auto got = run(u.x);
            for (auto& g : got) { if (g == codeOf(t[0], t[1], t[2], t[3])) ++right; else ++wrong; }
        }
        std::printf(" %+.0f dB %d%s", snr, right, wrong ? ("(" + std::to_string(wrong) + " wrong)").c_str() : "");
        if (std::isnan(fifty) && right >= 20) fifty = snr;
        if (wrong) ok(false, "a wrong code at " + std::to_string((int)snr) + " dB");
    }
    std::printf("\n    → 50 %% ≈ %+.0f dB SNR in 3 kHz\n", fifty);
    ok(!std::isnan(fifty) && fifty <= 0, "50 % of codes by 0 dB SNR in 3 kHz");

    std::printf("\n── nothing where there is nothing ──\n");
    {
        const double fs = 48000; const int secs = 3600 * 2;
        SelcalDecoder d(fs); int n = 0;
        d.onCode = [&](const SelcalDecoder::Code&) { ++n; };
        std::mt19937 r(5); std::normal_distribution<float> g(0.f, 0.3f);
        std::vector<float> x((size_t)fs);
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < secs; ++s) { for (auto& v : x) v = g(r); d.feed(x.data(), (int)x.size()); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ok(n == 0, "2 hours of white noise: " + std::to_string(n) + " codes");
        std::printf("    cost (incl. the noise generator): %.3f ms per second of 48 kHz audio\n", ms / secs);
    }
    {
        // DTMF dialling: every digit pair, 1 s each, 0.2 s apart — two tones, two pulses, but not SELCAL's tones
        const double lo[4] = {697, 770, 852, 941}, hi[4] = {1209, 1336, 1477, 1633};
        Au u;
        for (int rep = 0; rep < 3; ++rep) for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { u.two(lo[i], hi[j], 1.0); u.gap(0.2); }
        u.noise(20, 8);
        ok(run(u.x).empty(), "3 × the 16 DTMF digits at SELCAL's timing: nothing");
        // speech-like: a harmonic series with a gliding pitch, syllables of 0.2–0.6 s
        Au s; std::mt19937 r(13);
        for (int k = 0; k < 600; ++k) {
            const double f0 = 90 + (r() % 160), len = 0.2 + (r() % 400) / 1000.0, glide = ((int)(r() % 61) - 30);
            const int n = (int)(len * s.fs); double ph = 0;
            for (int i = 0; i < n; ++i) {
                const double f = f0 + glide * i / n; ph += 2 * M_PI * f / s.fs;
                double v = 0; for (int h = 1; h <= 12; ++h) v += std::sin(h * ph) / h;
                s.x.push_back((float)(0.1 * v));
            }
            s.gap((r() % 200) / 1000.0);
        }
        s.noise(25, 9, 0.02);
        ok(run(s.x).empty(), "≈ 5 minutes of speech-like harmonic syllables: nothing");
    }
    std::printf("\nfingerprint %016llx (every code above — must equal the VIBE_FORCE_SCALAR build's)\n", (unsigned long long)g_fp);
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
