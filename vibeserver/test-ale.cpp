// test-ale.cpp — 2G ALE (decoders/ale_decoder.h), V12 2026-11-19. Silent; synthetic audio from our own transmitter,
// plus (when VIBE_TEXTMODE_SAMPLES names the reference folder) the public sigidwiki recording "2G_ALEaudio" (8 kHz
// float32). Deterministic.
#include "decoders/ale_decoder.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
using namespace vibe;

static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

static std::vector<float> withNoise(std::vector<float> x, double fs, double snrDb3k, uint32_t seed, double padS = 2) {
    double p = 0; size_t n = 0;
    for (float v : x) if (v != 0.f) { p += v * v; ++n; }
    p = n ? p / n : 1;
    std::vector<float> pad((size_t)(fs * padS), 0.f);
    x.insert(x.begin(), pad.begin(), pad.end()); x.insert(x.end(), pad.begin(), pad.end());
    const float sig = (float)std::sqrt(p / std::pow(10.0, snrDb3k / 10.0) * (fs / 2) / 3000.0);
    std::mt19937 r(seed); std::normal_distribution<float> g(0.f, 1.f);
    for (auto& v : x) v += sig * g(r);
    return x;
}
struct Got { std::vector<std::string> pub, priv; int words = 0; bool everOn = false; };
static Got run(const std::vector<float>& x, double fs) {
    AleDecoder d(fs); Got g;
    d.clock = [] { return std::string("12:34:56"); };
    d.onLine = [&](const std::string& a, const std::string& b) { g.pub.push_back(a); g.priv.push_back(b); };
    d.onWord = [&](const AleDecoder::Word&) { ++g.words; };
    d.onState = [&](int s) { if (s) g.everOn = true; };
    const int blk = (int)(fs / 50);
    for (size_t i = 0; i < x.size(); i += blk) d.feed(x.data() + i, (int)std::min<size_t>(blk, x.size() - i));
    std::vector<float> z((size_t)(fs * 2), 0.f); d.feed(z.data(), (int)z.size());
    return g;
}
static std::string all(const std::vector<std::string>& v) { std::string s; for (auto& l : v) s += l; return s; }

int main() {
    std::printf("test-ale — 2G ALE (MIL-STD-188-141 Appendix A)\n");
    const double fs = 48000;

    std::printf("\n── the extended Golay (24,12) code ──\n");
    {
        int good = 0, det = 0; std::mt19937 r(1);
        for (uint32_t d = 0; d < 4096; ++d) {
            const uint32_t c = aleGolayEncode(d);
            // every error of weight ≤ 3 at random places is corrected; weight 4 is never mis-corrected to weight ≤ 3 silently
            uint32_t e = 0; const int wt = d % 4; while (__builtin_popcount(e) < wt) e |= 1u << (r() % 24);
            uint32_t out; const int k = aleGolayDecode(c ^ e, out);
            good += k == wt && out == d;
            uint32_t e4 = 0; while (__builtin_popcount(e4) < 4) e4 |= 1u << (r() % 24);
            det += aleGolayDecode(c ^ e4, out) < 0;
        }
        ok(good == 4096, "4096 data words, 0–3 random errors each: all corrected (" + std::to_string(good) + ")");
        ok(det == 4096, "…and 4 errors: detected, never 'corrected' (" + std::to_string(det) + ")");
        int minw = 24; for (uint32_t d = 1; d < 4096; ++d) minw = std::min(minw, __builtin_popcount(aleGolayEncode(d)));
        ok(minw == 8, "the code's minimum distance is 8 (" + std::to_string(minw) + ")");
    }

    std::printf("\n── soundings, calls, a handshake, an AMD message (our transmitter, +10 dB in 3 kHz, 48 kHz in) ──\n");
    {
        auto x = withNoise(aleSynth(aleCallWords("", "G4XYZ", 4), fs), fs, 10, 11);
        Got g = run(x, fs);
        ok(all(g.pub) == "[ALE] 12:34:56  sound  G4XYZ\n", "a sounding: \"" + all(g.pub) + "\"");
        auto y = withNoise(aleSynth(aleCallWords("", "K1ABC", 3, true), fs), fs, 10, 12);
        ok(all(run(y, fs).pub) == "[ALE] 12:34:56  sound  K1ABC  (TWAS: not taking calls)\n", "a TWAS sounding: " + all(run(y, fs).pub));
        // a 15-character address in five words, called by a 6-character one
        auto z = withNoise(aleSynth(aleCallWords("HFLNETWORKSTN15", "W1AW", 6), fs), fs, 10, 13);
        ok(all(run(z, fs).pub) == "[ALE] 12:34:56  call  W1AW \xe2\x86\x92 HFLNETWORKSTN15\n", "a call, long address: " + all(run(z, fs).pub));
        // the handshake: call, response, acknowledgement — on one decoder, a second apart
        std::vector<float> hs;
        for (auto w : {aleCallWords("W1XYZ", "K1ABC", 5), aleCallWords("K1ABC", "W1XYZ", 1), aleCallWords("W1XYZ", "K1ABC", 1)}) {
            auto a = aleSynth(w, fs); hs.insert(hs.end(), a.begin(), a.end()); hs.resize(hs.size() + (size_t)fs, 0.f);
        }
        Got h = run(withNoise(hs, fs, 10, 14), fs);
        ok(h.pub.size() == 3 && h.pub[0].find("call  K1ABC \xe2\x86\x92 W1XYZ") != std::string::npos
           && h.pub[1].find("response  W1XYZ \xe2\x86\x92 K1ABC") != std::string::npos
           && h.pub[2].find("ack  K1ABC \xe2\x86\x92 W1XYZ  (linked)") != std::string::npos, "call → response → ack (linked): " + all(h.pub));
        // AMD: the text only in the private line
        auto m = withNoise(aleSynth(aleCallWords("W1XYZ", "K1ABC", 3, false, "QSY 7102 USB PSE"), fs), fs, 10, 15);
        Got a = run(m, fs);
        ok(a.pub.size() == 1 && a.pub[0].find("AMD message (16 characters)") != std::string::npos && a.pub[0].find("QSY") == std::string::npos,
           "AMD: the public line has its length, not its text: " + all(a.pub));
        ok(a.priv.size() == 1 && a.priv[0].find("AMD: QSY 7102 USB PSE") != std::string::npos, "AMD: the private line has the text: " + all(a.priv));
    }

    std::printf("\n── the dial a little off, 12 kHz input (an UberSDR recording's rate) ──\n");
    for (double off : {-90.0, -45.0, 30.0, 70.0, 95.0}) {
        auto x = withNoise(aleSynth(aleCallWords("", "G4XYZ", 4), fs, off), fs, 10, 20 + (int)off);
        ok(all(run(x, fs).pub) == "[ALE] 12:34:56  sound  G4XYZ\n", "audio offset " + std::to_string((int)off) + " Hz");
    }
    {
        auto x = withNoise(aleSynth(aleCallWords("W1XYZ", "K1ABC", 4), 12000), 12000, 10, 30);
        ok(all(run(x, 12000).pub).find("call  K1ABC \xe2\x86\x92 W1XYZ") != std::string::npos, "12 kHz input");
    }

    std::printf("\n── weak signals: a 4-word sounding, 20 trials a point (SNR in 3 kHz) ──\n");
    for (double snr : {0.0, -3.0, -6.0, -9.0, -12.0}) {
        int whole = 0, wrong = 0;
        for (int t = 0; t < 20; ++t) {
            Got g = run(withNoise(aleSynth(aleCallWords("", "G4XYZ", 4), fs), fs, snr, 100 + t), fs);
            const std::string s = all(g.pub);
            if (s == "[ALE] 12:34:56  sound  G4XYZ\n") ++whole; else if (!s.empty() && s.find("G4XYZ") == std::string::npos) { ++wrong; std::printf("      wrong: %s", s.c_str()); }
            else if (std::getenv("VIBE_ALE_SHOW")) std::printf("      not whole (%d words): %s|\n", g.words, s.c_str());
        }
        std::printf("    %+.0f dB: %d/20 whole, %d wrong\n", snr, whole, wrong);
        if (snr >= 0) ok(whole >= 19, "0 dB: ≥ 19/20 (" + std::to_string(whole) + ")");
        ok(wrong == 0, std::to_string((int)snr) + " dB: never a wrong address");
    }

    std::printf("\n── other signals (60 s each, +10 dB): RTTY 45.45 / 170, an 8-tone MFSK at the wrong rate, a carrier ──\n");
    {
        std::vector<float> rtty((size_t)(fs * 60)), mf((size_t)(fs * 60)), car((size_t)(fs * 60));
        std::mt19937 r(5); double ph = 0; int bit = 0, tone = 0; const int spb = (int)(fs / 45.45), spt = (int)(fs / 100);
        for (size_t i = 0; i < rtty.size(); ++i) { if (i % spb == 0) bit = r() & 1; ph += 2 * M_PI * (bit ? 1585 : 1415) / fs; rtty[i] = (float)std::sin(ph); }
        for (size_t i = 0; i < mf.size(); ++i) { if (i % spt == 0) tone = r() % 8; ph += 2 * M_PI * (750 + 250 * tone) / fs; mf[i] = (float)std::sin(ph); }
        for (size_t i = 0; i < car.size(); ++i) car[i] = (float)std::sin(2 * M_PI * 1250 * i / fs);
        ok(run(withNoise(rtty, fs, 10, 41), fs).words == 0, "RTTY: no word");
        ok(run(withNoise(mf, fs, 10, 42), fs).words == 0, "ALE's tones at 100 Bd, random: no word");
        ok(run(withNoise(car, fs, 10, 43), fs).words == 0, "a carrier on an ALE tone: no word");
    }

    const int noiseMin = std::getenv("VIBE_ALE_NOISE_MIN") ? std::atoi(std::getenv("VIBE_ALE_NOISE_MIN")) : 10;
    std::printf("\n── white noise, %d min ──\n", noiseMin);
    {
        AleDecoder d(fs); int words = 0, lines = 0;
        d.onWord = [&](const AleDecoder::Word&) { ++words; };
        d.onLine = [&](const std::string&, const std::string&) { ++lines; };
        std::mt19937 r(7); std::normal_distribution<float> g(0.f, 0.3f); std::vector<float> b(960);
        for (long i = 0; i < (long)noiseMin * 60 * 50; ++i) { for (auto& v : b) v = g(r); d.feed(b.data(), 960); }
        ok(words == 0 && lines == 0, std::to_string(words) + " words, " + std::to_string(lines) + " lines");
    }

    if (const char* dir = std::getenv("VIBE_TEXTMODE_SAMPLES")) {
        std::printf("\n── the public reference recording (%s/2G_ALEaudio.f32) ──\n", dir);
        FILE* fp = std::fopen((std::string(dir) + "/2G_ALEaudio.f32").c_str(), "rb");
        if (!fp) std::printf("  (absent)\n");
        else {
            std::vector<float> x; float b[4096]; size_t n; while ((n = std::fread(b, 4, 4096, fp)) > 0) x.insert(x.end(), b, b + n);
            std::fclose(fp);
            Got g = run(x, 8000);
            const std::string s = all(g.pub);
            ok(s.find("sound  SHAEENQ2") != std::string::npos && s.find("USMANQ7") != std::string::npos, "two soundings by SHAEENQ2 and a call to USMANQ7: " + s);
        }
    }
    std::printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
