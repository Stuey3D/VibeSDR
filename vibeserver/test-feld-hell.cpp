// test-feld-hell.cpp — FELD HELL drawn (decoders/feld_hell.h), V12 2026-10-31. Silent; synthetic audio only, plus (when
// VIBE_TEXTMODE_SAMPLES names the folder of 8 kHz float32 reference recordings) the public recordings drawn. Deterministic.
//   • the test transmitter's text drawn back: the upper copy of each column against the font, ≥ 90 % of the pixels;
//   • with the sender's clock 300 ppm off, at 48 and 12 kHz in, and at 0 dB (SNR in 3 kHz over the keyed-on time); weaker, informational;
//   • 10 min of white noise: nothing drawn; the tone found within 10 Hz.
#include "decoders/feld_hell.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>
using namespace vibe;
using FH = FeldHellDecoder;

static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }
static uint64_t g_fp = 1469598103934665603ull;

struct Drawn { std::map<uint32_t, std::vector<uint8_t>> rows; float hz = 0; };
static Drawn draw(const std::vector<float>& x, double fs) {
    FH d(fs); Drawn r;
    d.onLine = [&](uint32_t ln, uint32_t w, const uint8_t* px) { r.rows[ln].assign(px, px + w); for (uint32_t i = 0; i < w; ++i) { g_fp ^= px[i]; g_fp *= 1099511628211ull; } };
    const int blk = (int)(fs / 50);
    for (size_t i = 0; i < x.size(); i += blk) d.feed(x.data() + i, (int)std::min<size_t>(blk, x.size() - i));
    r.hz = d.hz();
    return r;
}
static std::vector<float> withNoise(std::vector<float> x, double fs, double snr, uint32_t seed, double padS) {
    double p = 0; size_t n = 0; for (float v : x) if (v != 0.f) { p += v * v; ++n; } p /= n;
    std::vector<float> pad((size_t)std::lround(fs * padS), 0.f);
    x.insert(x.begin(), pad.begin(), pad.end()); x.insert(x.end(), pad.begin(), pad.end());
    const float s = (float)std::sqrt(p / std::pow(10.0, snr / 10.0) * (fs / 2) / 3000.0);
    std::mt19937 r(seed); std::normal_distribution<float> g(0.f, 1.f);
    for (auto& v : x) v += s * g(r);
    return x;
}
/** the expected upper copy: the transmitter's own elements, column by column (14 half-elements, top row first) */
static std::vector<std::vector<int>> expected(const std::string& text, double fs) {
    // the transmitter's keying, read back at the element centres (fs / 122.5 a element), bottom first
    auto clean = feldHellSynth(text, fs, 1000);
    const double perEl = fs / 122.5; std::vector<int> el;
    for (double c = perEl / 2; c < clean.size(); c += perEl) {
        double e = 0; const int a = (int)(c - perEl / 4), b = (int)(c + perEl / 4);
        for (int k = std::max(0, a); k < std::min((int)clean.size(), b); ++k) e += clean[k] * clean[k];
        el.push_back(e / (b - a) > 0.2);
    }
    std::vector<std::vector<int>> cols;
    for (size_t i = 0; i + 7 <= el.size(); i += 7) {
        std::vector<int> c(FH::kRows);
        for (int k = 0; k < 7; ++k) { c[FH::kRows - 1 - 2 * k] = el[i + k]; c[FH::kRows - 2 - 2 * k] = el[i + k]; }
        cols.push_back(c);
    }
    return cols;
}
/** the best agreement of the drawn upper copies with the expected columns, over a few columns' start offset */
static double agreement(const Drawn& d, const std::vector<std::vector<int>>& want, int chars) {
    std::vector<std::vector<int>> got;
    const uint32_t strips = d.rows.empty() ? 0 : d.rows.rbegin()->first / (2 * FH::kRows) + 1;
    for (uint32_t s = 0; s < strips; ++s)
        for (int x = 0; x < FH::kStripCols; ++x) {
            std::vector<int> c(FH::kRows); bool any = false;
            for (int h = 0; h < FH::kRows; ++h) { auto it = d.rows.find(s * 2 * FH::kRows + h); if (it != d.rows.end()) { c[h] = it->second[x] < 128; any = true; } }
            if (any) got.push_back(c);
        }
    const size_t n = std::min<size_t>(want.size(), (size_t)chars * 7);
    double best = 0;
    for (int off = -8; off <= 8; ++off) {
        size_t same = 0, tot = 0;
        for (size_t i = 0; i < n; ++i) {
            const long j = (long)i + off; if (j < 0 || j >= (long)got.size()) { tot += FH::kRows; continue; }
            for (int h = 0; h < FH::kRows; ++h) { same += got[j][h] == want[i][h]; ++tot; }
        }
        if (tot) best = std::max(best, (double)same / tot);
    }
    return best;
}

int main() {
    std::printf("test-feld-hell — FELD HELL, drawn\n");
    const std::string msg = "CQ CQ DE G4XYZ G4XYZ FELD HELL TEST K";
    const int chars = (int)msg.size();
    // ★ pads of whole columns (2 s = 35): the receiver's columns then start where the sender's do. Half a column out
    //   the copies show each letter split (bottom above top) — whole only across the pair, as Hell is read.
    std::printf("\n── the text drawn back (upper copy against the transmitter's own elements) ──\n");
    struct C { double fs, snr, ppm, hz; const char* what; double need; };
    const C cases[] = {
        {48000, 10, 0, 1000, "+10 dB, 48 kHz", 0.95}, {12000, 10, 0, 1500, "+10 dB, 12 kHz in", 0.95},
        {48000, 10, 300, 800, "+10 dB, the sender 300 ppm fast", 0.90}, {48000, 0, 0, 1200, "0 dB", 0.90},
    };
    for (const C& c : cases) {
        auto x = withNoise(feldHellSynth(msg, c.fs, c.hz, c.ppm), c.fs, c.snr, 11, 2.0);
        Drawn d = draw(x, c.fs);
        const double a = agreement(d, expected(msg, c.fs), chars);
        char w[160]; std::snprintf(w, sizeof w, "%s: %.1f %% of the pixels, tone %.0f Hz (sent %.0f)", c.what, 100 * a, d.hz, c.hz);
        ok(a >= c.need && std::fabs(d.hz - c.hz) < 10, w);
    }
    std::printf("\n── how weak (informational) ──\n");
    for (double snr : {-3.0, -6.0, -9.0, -12.0}) {
        auto x = withNoise(feldHellSynth(msg, 48000, 1000), 48000, snr, 12, 2.0);
        std::printf("  %+.0f dB: %.1f %%\n", snr, 100 * agreement(draw(x, 48000), expected(msg, 48000), chars));
    }
    std::printf("\n── nothing where there is nothing ──\n");
    {
        FH d(48000); int lines = 0; d.onLine = [&](uint32_t, uint32_t, const uint8_t*) { ++lines; };
        std::mt19937 r(5); std::normal_distribution<float> g(0.f, 0.3f); std::vector<float> b(960);
        const int mins = std::getenv("VIBE_HELL_NOISE_MIN") ? std::atoi(std::getenv("VIBE_HELL_NOISE_MIN")) : 10;
        for (long i = 0; i < (long)mins * 60 * 50; ++i) { for (auto& v : b) v = g(r); d.feed(b.data(), 960); }
        ok(lines == 0 && !d.signal(), std::to_string(mins) + " min of white noise: " + std::to_string(lines) + " lines drawn");
    }
    if (const char* dir = std::getenv("VIBE_TEXTMODE_SAMPLES")) {
        std::printf("\n── reference recordings (%s): drawn (read them with tools/hell-bench/hell_file) ──\n", dir);
        for (const char* f : {"Hellschreiber.f32", "Wikipedia-Slogan_Hellschreiber.f32", "FeldHell.f32"}) {
            FILE* fp = std::fopen((std::string(dir) + "/" + f).c_str(), "rb");
            if (!fp) { std::printf("  (absent: %s)\n", f); continue; }
            std::vector<float> x; float b[4096]; size_t n; while ((n = std::fread(b, 4, 4096, fp)) > 0) x.insert(x.end(), b, b + n);
            std::fclose(fp);
            Drawn d = draw(x, 8000);
            size_t ink = 0, px = 0; for (auto& kv : d.rows) for (uint8_t v : kv.second) { ink += v < 128; ++px; }
            char w[160]; std::snprintf(w, sizeof w, "%s: %zu rows, %.0f %% ink, tone %.0f Hz", f, d.rows.size(), px ? 100.0 * ink / px : 0.0, d.hz);
            ok(d.rows.size() >= 2 * FH::kRows && px && ink * 100 / px >= 5 && ink * 100 / px <= 50, w);
        }
    }
    std::printf("\noutput fingerprint %016llx\n", (unsigned long long)g_fp);
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
