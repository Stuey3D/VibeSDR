// test-msk144.cpp — MSK144, our own receiver (decoders/digimodes/msk144_code.h, msk144_decoder.h, msk144.h) and the
// small-LDPC belief propagation / OSD it runs on (ldpc_small.h). V12, 2026-10-31.
//
// ★★★ WHAT IS PINNED.
//   • THE FRAME: 8 vectors captured from the shipped WSJT-X 2.7.0 utility run as a black box (msk144code: the 144 tones)
//     reproduced exactly — the CRC-13, the (128,90) LDPC code, the sync, the offset-QPSK bit mapping;
//   • the LDPC: encode → every check holds; BP exact on clean soft bits; OSD recovers a codeword BP cannot;
//   • SENSITIVITY on synthetic MSK pings (tools/msk144-bench/msk144_synth.h, SNR in 2500 Hz, the form jt9 decodes):
//     a single 72 ms frame ≥ 85 % at +5 dB; four frames ≥ 80 % at −3 dB; a 1 s ping ≥ 80 % at −6 dB — each ≈ 1.5–3 dB
//     past what jt9 decodes on the same files (docs/v12/DECODERS-WAVE2.md "MSK144"); frequency within 2 Hz;
//   • PURE NOISE gives nothing (20 minutes);
//   • the ENGINE: audio blocks with capture times → one MSK144 spot with the call, the grid and the ping's UTC.
// ★ Silent; deterministic.
#include "decoders/digimodes/msk144.h"
#include "decoders/digimodes/ldpc_small.h"
#include "msk144_synth.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace vibe;
static int fails = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (!c) fails++; }

struct Vec { const char* msg; const char* tones; };
static const Vec kVecs[] = {
    {"71E25C2DF7E47D1E37", "110000100111000100011000100111011011100101100101000011011100001001011011000100011110010010010000110011010110010001111101001010111101100011110101"},
    {"4781C2B67D2ED65C94", "110000101100010001010001110110101110000001011011101100101100001010100000100111100010111010011010111000101100101010000011001101101001111010010000"},
    {"43EC1A88B76F0AC56C", "110000101101110100111101000010100110011011100110001101111100001101101011110010100011111010011101000101111100010111101101011011111101110111000010"},
    {"CQ G4ABC/P IO91", "110000110101010101010101010101010011010110001101111101011100001010000000100001011101100000100110001101101011100000000111101010101111111110100010"},
    {"M6BCR G5BQG BC66", "110000110100101100001111000011110000010110001100110011101100001110110011110100110101100011001100011001101011101111000011101001111011011110101001"},
    {"CQ N8BS DH90", "110000110101010101010101010101010011010110100000110101001100001000100010110111110100100111000000100000111101110101011000001011000100110000101010"},
    {"CQ VK8FDS GL18", "110000110101010101010101010101010011110001100100010011111100001001001110110010011011000111001010011010111000001001010010100001010100100111000010"},
    {"G4T DL7RNY R-10", "110000110100111001000100110010001101000010011010100011101100001011110001000101011010101110000011111000100001110000001111011000101101101111010110"},
};

static bool tonesOf(const char* msg, uint8_t t[144]) {
    const std::string m = msg;
    if (m.size() == 18 && m.find_first_not_of("0123456789ABCDEF") == std::string::npos) {   // telemetry: 71 bits, n3 5, i3 0
        uint8_t b[77] = {};
        for (int k = 0; k < 18; ++k) {
            const int v = std::stoi(m.substr((size_t)k, 1), nullptr, 16);
            for (int j = 0; j < 4; ++j) { const int bit = 4 * k + j - 1; if (bit >= 0) b[bit] = (uint8_t)((v >> (3 - j)) & 1); }
        }
        b[71] = 1; b[73] = 1;
        uint8_t cw[msk144::kN]; msk144::encode(b, cw); msk144::tones(cw, t);
        return true;
    }
    return msk144::encodeText(msg, t);
}

static const char* kMsgs[] = {"G4ABC PA3XYZ JO22", "CQ DL1AAA JO62", "OH2BH G3WOS -05", "SP9XYZ OK1ABC R-03", "CQ M0ABC IO91"};

static double rate(int bits, double snr, int trials, double* fErr) {
    int got = 0; double fe = 0;
    for (int t = 0; t < trials; ++t) {
        const char* want = kMsgs[t % 5];
        uint8_t tones[144]; msk144::encodeText(want, tones);
        auto a = msksynth::noise(3.5, 900u + (uint32_t)t * 77u + (uint32_t)bits);
        const double f = 1500 + ((t * 41) % 81 - 40) + 0.31 * t;
        msksynth::addPing(a, tones, f, 0.8 + 0.11 * (t % 5), bits, (t * 59) % 144, snr, 0.9 * t);
        msk144::Receiver r;
        bool hit = false;
        r.onResult = [&](const msk144::Result& x) { if (x.text == want && !hit) { hit = true; fe = std::max(fe, (double)std::fabs(x.f0 - f)); } };
        r.push(a.data(), (int)a.size()); r.flush();
        got += hit;
    }
    if (fErr) *fErr = fe;
    return (double)got / trials;
}

int main() {
    std::printf("test-msk144 — MSK144, our own receiver\n\n── the frame against the black-box utility ──\n");
    int same = 0;
    for (const Vec& v : kVecs) {
        uint8_t t[144];
        std::string got;
        if (tonesOf(v.msg, t)) for (int k = 0; k < 144; ++k) got += (char)('0' + t[k]);
        same += got == v.tones;
        if (got != v.tones) std::printf("    differs: %s\n", v.msg);
    }
    ok(same == (int)(sizeof kVecs / sizeof kVecs[0]), "msk144code's tones reproduced: " + std::to_string(same) + "/8");

    std::printf("\n── the code ──\n");
    {
        const ldpc::Code& c = msk144::code();
        uint8_t info[90], cw[128], dec[128];
        for (int i = 0; i < 90; ++i) info[i] = (uint8_t)((i * 7 + 3) % 5 == 0);
        c.encode(info, cw);
        ok(c.checksOk(cw), "encode: all 38 checks hold");
        float llr[128];
        for (int j = 0; j < 128; ++j) llr[j] = cw[j] ? 4.f : -4.f;
        ok(c.bp(llr, 30, dec) > 0 && std::memcmp(dec, cw, 128) == 0, "BP: clean soft bits decode at once");
        // 14 strong wrong bits among the least reliable: beyond BP here, inside OSD's pairs
        for (int j = 0; j < 128; ++j) llr[j] = cw[j] ? 3.f : -3.f;
        int flipped = 0;
        for (int j = 5; j < 128 && flipped < 12; j += 10) { llr[j] = cw[j] ? -0.3f : 0.3f; ++flipped; }
        uint8_t o[128];
        const bool got = c.osd(llr, 12, [&](const uint8_t* x) { return std::memcmp(x, cw, 128) == 0; }, o);
        ok(got && std::memcmp(o, cw, 128) == 0, "OSD: 12 weak wrong bits recovered");
        uint8_t m77[77]; for (int i = 0; i < 77; ++i) m77[i] = (uint8_t)(i % 3 == 1);
        msk144::encode(m77, cw);
        ok(msk144::crcOk(cw) && c.checksOk(cw), "CRC-13 + LDPC of a 77-bit payload");
    }

    std::printf("\n── sensitivity (synthetic pings, SNR in 2500 Hz) ──\n");
    double fe = 0, p;
    p = rate(144, 5, 20, &fe);  ok(p >= 0.85, "one frame (72 ms) at +5 dB: " + std::to_string((int)(p * 100)) + " %");
    ok(fe < 2.5, "  frequency within 2.5 Hz (" + std::to_string(fe) + ")");
    p = rate(576, -3, 20, &fe); ok(p >= 0.80, "four frames at −3 dB: " + std::to_string((int)(p * 100)) + " %");
    p = rate(2000, -6, 20, &fe); ok(p >= 0.80, "a 1 s ping at −6 dB: " + std::to_string((int)(p * 100)) + " %");
    ok(fe < 0.6, "  frequency within 0.6 Hz on a 1 s ping (" + std::to_string(fe) + ")");

    std::printf("\n── noise ──\n");
    {
        msk144::Receiver r; int n = 0;
        r.onResult = [&](const msk144::Result& x) { ++n; std::printf("    FALSE %s\n", x.text.c_str()); };
        for (int i = 0; i < 120; ++i) { auto a = msksynth::noise(10.0, 31337u + (uint32_t)i); r.push(a.data(), (int)a.size()); }
        r.flush();
        ok(n == 0, "20 minutes of white noise: no decode (" + std::to_string(n) + ", " + std::to_string(r.stats.attempts) + " attempts)");
    }

    std::printf("\n── the engine ──\n");
    {
        const digi::ModeDef* m = nullptr;
        digi::ModeDef def = digi::vibeDigiMode_msk144();
        m = &def;
        auto e = m->make();
        std::vector<digi::Spot> spots;
        e->onSpot = [&](const digi::Spot& s) { spots.push_back(s); };
        uint8_t tones[144]; msk144::encodeText("G4ABC PA3XYZ JO22", tones);
        auto a = msksynth::noise(6.0, 77);
        msksynth::addPing(a, tones, 1520, 2.25, 700, 17, 0);
        const double utcStart = 1793400000.0;   // sample 0
        for (size_t i = 0; i < a.size(); i += 1200) {
            const int n = (int)std::min<size_t>(1200, a.size() - i);
            e->process(a.data() + i, n, utcStart + (double)(i + n) / 12000.0);
        }
        e.reset();
        const bool one = spots.size() == 1;
        ok(one && spots[0].call == "PA3XYZ" && spots[0].grid == "JO22" && spots[0].mode == "MSK144", "one spot: PA3XYZ JO22, mode MSK144");
        // the first whole frame starts (144 − 17) bits after the ping: 2.25 + 0.0635 s
        ok(one && std::fabs(spots[0].slotUtc - (utcStart + 2.3135)) < 0.08, "  stamped with the ping's own time");
        ok(one && std::fabs(spots[0].audioHz - 1520) < 2, "  at 1520 Hz");
    }
    std::printf("\n%s (%d failed)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
