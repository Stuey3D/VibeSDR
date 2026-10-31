// test-same.cpp — EAS / SAME (decoders/same_decoder.h) from 47 CFR § 11.31, on synthetic audio. V12, 2026-10-31.
// ★ Silent; deterministic. Every header here is made up by this file (no recorded alert is used or kept).
//
// ★★★ WHAT IS PINNED.
//   • The § 11.31 format: a header parses field by field; every malformed one (bad ORG, letters in a location, day 367,
//     minute 60, a sender of 9, no trailing '-') is refused.
//   • The bit order: the synth sends LSB first (the null eighth bit last); the same header sent MSB first decodes NOTHING.
//   • Three bursts → one alert ("heard 3"); a burst wrecked by a click → the other two confirm it; one burst only →
//     not shown (no checksum: two readings must agree); NNNN ×3 → one end-of-message; the attention signal alone → nothing.
//   • Sensitivity in white noise (SNR in a 3 kHz band), ±1 % clock / ±20 Hz tone error; hours of noise and of random
//     AFSK bytes → 0 alerts. A fingerprint of every line, equal in a VIBE_FORCE_SCALAR build (printed).
#include "decoders/same_decoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

static const char* kHdr = "ZCZC-WXR-TOR-024031-024033+0030-3041345-KLWX/NWS-";

struct Synth {
    double fs = 48000, clk = 0, toneErr = 0; bool msbFirst = false;
    std::vector<float> x; double ph = 0;
    void tone(double f, double sec, double amp = 0.5) {
        const int n = (int)(sec * fs);
        for (int i = 0; i < n; ++i) { ph += 2 * M_PI * f / fs; x.push_back((float)(amp * std::sin(ph))); }
    }
    void silence(double sec) { x.insert(x.end(), (size_t)(sec * fs), 0.f); }
    void bytes(const std::vector<uint8_t>& b) {
        const double br = (520.0 + 5.0 / 6.0) * (1 + clk);
        double t = 0; const double bitT = 1.0 / br;
        for (uint8_t c : b)
            for (int k = 0; k < 8; ++k) {
                const int bit = msbFirst ? (c >> (7 - k)) & 1 : (c >> k) & 1;
                const double f = (bit ? 2083.0 + 1.0 / 3.0 : 1562.5) + toneErr;
                t += bitT;
                while ((double)x.size() / fs < tEnd + t) { ph += 2 * M_PI * f / fs; x.push_back((float)(0.5 * std::sin(ph))); }
            }
        tEnd += t;
    }
    double tEnd = 0;
    void burst(const std::string& s) {
        tEnd = (double)x.size() / fs;
        std::vector<uint8_t> b(16, 0xAB);
        for (char c : s) b.push_back((uint8_t)c);
        bytes(b);
    }
    void addNoise(double snrDb3k, uint32_t seed) {
        // ★ signal power 0.125 (amp 0.5); noise σ² over fs/2 so that the power in 3 kHz is 0.125 / 10^(snr/10)
        std::mt19937 r(seed); std::normal_distribution<float> g(0.f, 1.f);
        const double pn3 = 0.125 / std::pow(10.0, snrDb3k / 10.0);
        const float sig = (float)std::sqrt(pn3 * (fs / 2) / 3000.0);
        for (auto& v : x) v += sig * g(r);
    }
};
static void alertAudio(Synth& s, const std::string& h, int skipBurst = -1, int clickBurst = -1) {
    s.silence(0.5);
    for (int k = 0; k < 3; ++k) {
        if (k == skipBurst) { s.silence(1.6); }
        else {
            const size_t at = s.x.size();
            s.burst(h);
            if (k == clickBurst) for (size_t i = at + (size_t)(0.6 * s.fs); i < at + (size_t)(0.65 * s.fs) && i < s.x.size(); ++i) s.x[i] = 0;   // 50 ms dropout
        }
        s.silence(1.0);
    }
    // the attention signal: 853 + 960 Hz together, 8 s
    { const int n = (int)(8 * s.fs); double a = 0, b = 0;
      for (int i = 0; i < n; ++i) { a += 2 * M_PI * 853 / s.fs; b += 2 * M_PI * 960 / s.fs; s.x.push_back((float)(0.25 * (std::sin(a) + std::sin(b)))); } }
    s.silence(1.0);
    for (int k = 0; k < 3; ++k) { s.burst("NNNN"); s.silence(1.0); }
    s.silence(4.0);
}
struct Got { std::vector<SameDecoder::Alert> alerts; std::vector<std::string> lines; int eom = 0; };
static uint64_t g_fp = 1469598103934665603ull;
static Got run(const std::vector<float>& x, double fs = 48000) {
    SameDecoder d(fs); Got g;
    d.onAlert = [&](const SameDecoder::Alert& a) {
        g.alerts.push_back(a); g.lines.push_back(SameDecoder::line(a));
        for (char c : g.lines.back()) { g_fp ^= (uint8_t)c; g_fp *= 1099511628211ull; }
    };
    d.onEom = [&] { ++g.eom; };
    const int blk = (int)(fs * 0.02);
    for (size_t i = 0; i < x.size(); i += (size_t)blk) d.feed(x.data() + i, (int)std::min<size_t>((size_t)blk, x.size() - i));
    return g;
}

int main() {
    std::printf("test-same — EAS / SAME (47 CFR § 11.31)\n\n── the header format ──\n");
    {
        SameDecoder::Alert a;
        ok(SameDecoder::parse(kHdr, a) && a.org == "WXR" && a.event == "TOR" && a.areas.size() == 2 && a.areas[1] == "024033"
           && a.validMin == 30 && a.day == 304 && a.hour == 13 && a.minute == 45 && a.sender == "KLWX/NWS", "a header parses field by field");
        const char* bad[] = {
            "ZCZC-XYZ-TOR-024031+0030-3041345-KLWX/NWS-", "ZCZC-WXR-T0R-024031+0030-3041345-KLWX/NWS-",
            "ZCZC-WXR-TOR-02A031+0030-3041345-KLWX/NWS-", "ZCZC-WXR-TOR-024031+0030-3671345-KLWX/NWS-",
            "ZCZC-WXR-TOR-024031+0030-3041360-KLWX/NWS-", "ZCZC-WXR-TOR-024031+0060-3041345-KLWX/NWS-",
            "ZCZC-WXR-TOR-024031+0030-3041345-KLWX/NWSX-", "ZCZC-WXR-TOR-024031+0030-3041345-KLWX/NWS",
            "ZCZC-WXR-TOR-024031-0030-3041345-KLWX/NWS-", "ZCZC-WXR-TOR-024031+0030-3042445-KLWX/NWS-" };
        int refused = 0; for (auto* b : bad) refused += !SameDecoder::parse(b, a);
        ok(refused == 10, "every malformed header refused (" + std::to_string(refused) + " of 10)");
        ok(std::string(SameDecoder::eventName("RWT")) == "Required Weekly Test" && std::string(SameDecoder::stateAbbr(24)) == "MD", "names: RWT, state 24 = MD");
    }

    std::printf("\n── alerts ──\n");
    {
        Synth s; alertAudio(s, kHdr);
        const Got g = run(s.x);
        ok(g.alerts.size() == 1 && g.alerts[0].raw == kHdr && g.alerts[0].heard == 3 && g.eom == 1, "three bursts → one alert (heard 3), NNNN → one end");
        if (!g.lines.empty()) std::printf("    %s", g.lines[0].c_str());
        std::printf("    (%zu alerts, heard %d, %d ends)\n", g.alerts.size(), g.alerts.empty() ? 0 : g.alerts[0].heard, g.eom);
        Synth m; m.msbFirst = true; alertAudio(m, kHdr);
        ok(run(m.x).alerts.empty(), "the same header MSB first: nothing (the bit order is pinned)");
        Synth c; alertAudio(c, kHdr, -1, 1);
        const Got gc = run(c.x);
        if (!gc.alerts.empty()) std::printf("    drop-out: %zu alerts, heard %d\n", gc.alerts.size(), gc.alerts[0].heard);
        ok(gc.alerts.size() == 1 && gc.alerts[0].raw == kHdr && gc.alerts[0].heard >= 2, "one burst wrecked by a 50 ms drop-out: the others confirm it");
        Synth o; alertAudio(o, kHdr, 0); o.x.clear(); o.silence(0.5); o.burst(kHdr); o.silence(6);
        const Got go = run(o.x);
        ok(go.alerts.empty(), "one burst only (tuned in mid-alert): not shown — SAME has no checksum, two readings must agree");
        for (double fs : {24000.0, 12000.0}) {
            Synth r; r.fs = fs; alertAudio(r, kHdr);
            ok(run(r.x, fs).alerts.size() == 1, "at " + std::to_string((int)fs) + " Hz too");
        }
        Synth t; t.silence(0.5); { const int n = (int)(20 * t.fs); double a = 0, b = 0;
          for (int i = 0; i < n; ++i) { a += 2 * M_PI * 853 / t.fs; b += 2 * M_PI * 960 / t.fs; t.x.push_back((float)(0.25 * (std::sin(a) + std::sin(b)))); } }
        const Got gt = run(t.x);
        ok(gt.alerts.empty() && gt.eom == 0, "20 s of the attention signal alone: nothing");
    }

    std::printf("\n── sensitivity (SNR in 3 kHz; 20 alerts a point; ±1 %% clock, ±20 Hz tones) ──\n    ");
    double fifty = NAN; float minRight[2] = {9, 9};
    for (double snr = -6; snr <= 10.01; snr += 2) {
        int right = 0, wrong = 0;
        for (int k = 0; k < 20; ++k) {
            Synth s; s.clk = 0.01 * ((k % 3) - 1); s.toneErr = 20.0 * ((k % 5) - 2) / 2.0;
            alertAudio(s, kHdr); s.addNoise(snr, 100u + (uint32_t)k + (uint32_t)(snr * 13 + 100));
            const Got g = run(s.x);
            for (auto& a : g.alerts) { if (a.raw == kHdr) { ++right; minRight[a.heard > 1] = std::min(minRight[a.heard > 1], a.margin); } else { ++wrong; std::printf("\n      wrong (heard %d, margin %.2f): %s", a.heard, a.margin, a.raw.c_str()); } }
        }
        std::printf(" %+.0f dB %2d%s", snr, right, wrong ? ("(" + std::to_string(wrong) + " wrong)").c_str() : "");
        if (std::isnan(fifty) && right >= 10) fifty = snr;
        if (wrong) ok(false, "a wrong alert at " + std::to_string((int)snr) + " dB");
    }
    std::printf("\n    → 50 %% ≈ %+.0f dB SNR in 3 kHz (weakest right margin: lone %.2f, combined %.2f)\n", fifty, minRight[0], minRight[1]);
    ok(!std::isnan(fifty) && fifty <= 6, "50 % of alerts by +6 dB SNR in 3 kHz");

    std::printf("\n── nothing where there is nothing ──\n");
    {
        const double fs = 48000; const int secs = 3600 * 2;
        SameDecoder d(fs); int n = 0, e = 0;
        d.onAlert = [&](const SameDecoder::Alert&) { ++n; }; d.onEom = [&] { ++e; };
        std::mt19937 r(7); std::normal_distribution<float> g(0.f, 0.3f);
        std::vector<float> x((size_t)fs);
        const auto t0 = std::chrono::steady_clock::now();
        for (int s = 0; s < secs; ++s) { for (auto& v : x) v = g(r); d.feed(x.data(), (int)x.size()); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ok(n == 0 && e == 0, "2 hours of white noise: " + std::to_string(n) + " alerts, " + std::to_string(e) + " ends");
        std::printf("    cost (incl. the noise generator): %.3f ms per second of 48 kHz audio\n", ms / secs);
    }
    {
        // random AFSK bytes at SAME's own rate and tones (no preamble) — the bit clock locks, nothing else may
        Synth s; std::mt19937 r(11); std::vector<uint8_t> b(520 * 60 * 20 / 8);
        for (auto& v : b) v = (uint8_t)r();
        s.tEnd = 0; s.bytes(b); s.addNoise(15, 3);
        const Got g = run(s.x);
        ok(g.alerts.empty() && g.eom == 0, "20 minutes of random AFSK bytes at 520.83 bit/s: " + std::to_string(g.alerts.size()) + " alerts");
    }
    std::printf("\nfingerprint %016llx (every line above — must equal the VIBE_FORCE_SCALAR build's)\n", (unsigned long long)g_fp);
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
