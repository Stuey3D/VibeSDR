// tools/alert-bench/alert_clip.cpp — the end-to-end clip for scripts/test-web-alerts.mjs (V12, 2026-10-31): unsigned 8-bit
// IQ at 960 kS/s, 20 s, for vibeserver/fake-rtl-tcp.mjs --hf FILE --hf-centre (dial + 3500). Upper-sideband audio as the
// listener's USB dial hears it (every tone a complex exponential at dial + f): one EAS / SAME header three times (47 CFR
// § 11.31 — 16 × 0xAB, ASCII LSB first, mark 2083.3 / space 1562.5 Hz, 520.83 bit/s), one SELCAL call (BH-DM, two
// pulses of two tones, ICAO Annex 10 Vol III Table 3-1), then NNNN three times. Synthetic — no recording, no server.
//   alert_clip OUT.cu8
#include "decoders/selcal_decoder.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

static const double R = 960000, kSec = 20, kOff = -3500;   // the clip's centre is 3.5 kHz above the dial
struct Clip {
    std::vector<std::complex<float>> z;
    double t0 = 0;
    Clip() : z((size_t)(R * kSec)) {}
    /** A tone (audio f Hz on the USB dial) from t for sec, phase continuous across calls with the same `ph`. */
    void tone(double& ph, double f, double t, double sec, float amp) {
        const size_t a = (size_t)(t * R), b = std::min(z.size(), (size_t)((t + sec) * R));
        const double w = 2 * M_PI * (f + kOff) / R;
        for (size_t i = a; i < b; ++i) { z[i] += amp * std::complex<float>((float)std::cos(ph), (float)std::sin(ph)); ph += w; }
    }
    /** One SAME burst at t; returns its end. */
    double burst(const std::string& s, double t) {
        std::vector<uint8_t> b(16, 0xAB);
        for (char c : s) b.push_back((uint8_t)c);
        const double bitT = 1.0 / (520.0 + 5.0 / 6.0);
        double ph = 0;
        for (uint8_t c : b) for (int k = 0; k < 8; ++k) {
            tone(ph, (c >> k) & 1 ? 2083.0 + 1.0 / 3.0 : 1562.5, t, bitT, 0.25f);
            t += bitT;
        }
        return t;
    }
};

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: alert_clip OUT.cu8\n"); return 2; }
    Clip c;
    double t = 0.5;
    for (int k = 0; k < 3; ++k) t = c.burst("ZCZC-WXR-TOR-024031-024033+0030-3041345-KLWX/NWS-", t) + 1.0;
    // SELCAL BH-DM: B + H, then D + M (Table 3-1 indices), 1.0 s each, 0.2 s apart
    using vibe::SelcalDecoder;
    const int ia = SelcalDecoder::indexOf('B'), ib = SelcalDecoder::indexOf('H'), ic = SelcalDecoder::indexOf('D'), id = SelcalDecoder::indexOf('M');
    t += 1.0;
    { double p = 0, q = 0; c.tone(p, SelcalDecoder::kHz[ia], t, 1.0, 0.15f); c.tone(q, SelcalDecoder::kHz[ib], t, 1.0, 0.15f); }
    t += 1.2;
    { double p = 0, q = 0; c.tone(p, SelcalDecoder::kHz[ic], t, 1.0, 0.15f); c.tone(q, SelcalDecoder::kHz[id], t, 1.0, 0.15f); }
    t += 2.5;
    for (int k = 0; k < 3; ++k) t = c.burst("NNNN", t) + 1.0;
    std::mt19937 rng(31); std::normal_distribution<float> g(0.f, 1.f);
    std::vector<uint8_t> out(c.z.size() * 2);
    auto q8 = [](float v) { const long u = std::lround(127.5 + 127.0 * v); return (uint8_t)std::max(0L, std::min(255L, u)); };
    for (size_t i = 0; i < c.z.size(); ++i) {
        const std::complex<float> v = c.z[i] + std::complex<float>(0.01f * g(rng), 0.01f * g(rng));
        out[i * 2] = q8(v.real()); out[i * 2 + 1] = q8(v.imag());
    }
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(out.data(), 1, out.size(), f); std::fclose(f);
    std::printf("wrote %s (%.1f s used of %.0f s at 960 kS/s)\n", argv[1], t, kSec);
    return 0;
}
