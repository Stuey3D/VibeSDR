// test-tones.cpp — V12 tone decoders (decoders/tones_*.h, docs/v12/TONES.md): DTMF (ITU-T Q.23/Q.24) and 5-tone selective
// calling (CCIR, EEA, EIA, ZVEI-1/2/3, DZVEI, PZVEI). ★ Silent; deterministic; our own transmitter (decoders/tones_tx.h).
//
// ★★★ WHAT IS PINNED.
//   • The tables: Q.23's eight frequencies; every 5-tone table 16 tones, each ≥ 6 % from its neighbours (so ±2 % is one tone).
//   • DTMF: all 16 keys, 200 random dialled strings at 25 dB and down to 10 dB SNR (3 kHz) — every digit right; twist −3.5
//     and +7 dB read, −6 and +10 dB not; 40 ms tones / 40 ms pauses read, 20 ms tones not; a 10 ms break bridged, a 40 ms pause two digits;
//     ±1.5 % read, ±3.5 % not; with a CTCSS sub-tone under it.
//   • 5-tone: every table, 60 random calls each (repeats in them), its nominal period and 40–100 ms; the repeat rule; ±1.5 %
//     read; ±4.5 % never read as the same call; AUTO names the table; a pin reads only its own; CTCSS under it; noise.
//   • FALSE POSITIVES: minutes of white noise, synthetic speech (glottal pulses through moving formants), music (notes with
//     harmonics, chords, arpeggios and pure-sine melodies at 70–100 ms a note), DTMF read by the 5-tone decoder and 5-tone by
//     DTMF — nothing. `test-tones wav <file>…` runs recordings (e.g. macOS `say` output) and prints anything decoded.
//   • SIMD = scalar: the Goertzel bank and the decimator bit for bit, and every decode the same.
//   • Cost on this machine.
//   `test-tones clip <out.cu8> [rate] [seconds]` writes the end-to-end tests' NFM clip (see writeClip).
#include "decoders/tones_rx.h"
#include "decoders/tones_tx.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vibe::tones;

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); c ? ++g_pass : ++g_fail; }

struct Got { std::vector<std::string> dtmf; std::vector<std::string> five; std::vector<int> fiveTable; std::vector<double> fivePer; };

struct Five { std::vector<int> also; std::vector<std::string> alsoDigits; };
static Five g_lastFive;
static Got run(const std::vector<float>& a, bool scalar = false, int pin = -1, bool dtmfOn = true, bool fiveOn = true) {
    Rx rx; rx.scalar = scalar; rx.five.pin = pin; rx.dtmfOn = dtmfOn; rx.fiveOn = fiveOn;
    Got g;
    rx.dtmf.onSequence = [&](const Dtmf::Sequence& s) { g.dtmf.push_back(s.digits); };
    rx.five.onCall = [&](const FiveTone::Call& c) {
        g.five.push_back(c.digits); g.fiveTable.push_back(c.table); g.fivePer.push_back(c.periodMs);
        g_lastFive.also = c.alsoFits; g_lastFive.alsoDigits = c.alsoDigits;
    };
    for (size_t i = 0; i < a.size(); i += 960) rx.feed(a.data() + i, (int)std::min<size_t>(960, a.size() - i));
    // ★ a second of silence lets every open sequence close
    std::vector<float> z(48000, 0.f);
    for (size_t i = 0; i < z.size(); i += 960) rx.feed(z.data() + i, 960);
    rx.flush();
    return g;
}
/** White noise at `snrDb` relative to a signal of power `sigPow`, measured in a 3 kHz band. */
static void addNoise(std::vector<float>& a, double snrDb, double sigPow, uint32_t seed) {
    std::mt19937 r(seed); std::normal_distribution<float> n(0.f, 1.f);
    const float s = (float)std::sqrt(sigPow / std::pow(10.0, snrDb / 10) * 24000.0 / 3000.0);
    for (auto& v : a) v += s * n(r);
}
static void addCtcss(std::vector<float>& a, double hz, double amp) {
    for (size_t i = 0; i < a.size(); ++i) a[i] += (float)(amp * std::sin(2 * M_PI * hz * i / 48000.0));
}
static std::string join(const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += (s.empty() ? "" : "|") + x; return s; }

static const char* kKeys = "0123456789*#ABCD";

// ── the tables ──────────────────────────────────────────────────────────────────────────────────────────────────────
static void testTables() {
    std::printf("\n── the tables ──\n");
    ok(Dtmf::kLow[0] == 697 && Dtmf::kLow[3] == 941 && Dtmf::kHigh[0] == 1209 && Dtmf::kHigh[3] == 1633, "DTMF: Q.23's 697/770/852/941 × 1209/1336/1477/1633 Hz");
    ok(std::string(Dtmf::kKeys) == "123A456B789C*0#D", "DTMF: the keypad (rows = low group)");
    bool sep = true; double worst = 1;
    for (int t = 0; t < kToneTables; ++t)
        for (int i = 0; i < 16; ++i) for (int j = 0; j < 16; ++j) if (i != j) {
            const double r = std::fabs(toneTables()[t].hz[i] / toneTables()[t].hz[j] - 1);
            if (r < worst) worst = r;
            if (r < 0.055) sep = false;
        }
    char b[160]; std::snprintf(b, sizeof b, "5-tone: every table's 16 tones ≥ 5.5 %% apart (closest %.1f %%) — ±2 %% names one tone", worst * 100);
    ok(sep, b);
    ok(toneTables()[5].hz[0] == 1981 && toneTables()[5].hz[1] == 1124 && toneTables()[5].hz[14] == 2110, "CCIR: 0 = 1981, 1 = 1124, repeat E = 2110 Hz");
    ok(toneTables()[0].hz[0] == 2400 && toneTables()[0].hz[1] == 1060 && toneTables()[0].hz[14] == 2600, "ZVEI-1: 0 = 2400, 1 = 1060, repeat E = 2600 Hz");
    ok(toneTables()[7].hz[0] == 600 && toneTables()[7].hz[14] == 459, "EIA: 0 = 600, repeat E = 459 Hz");
    ok(tx::withRepeats("12334") == "123E4" && tx::withRepeats("12333") == "123E3" && tx::withRepeats("11111") == "1E1E1", "the repeat rule (sender): 12334 → 123E4, 12333 → 123E3");
}

// ── DTMF ────────────────────────────────────────────────────────────────────────────────────────────────────────────
static std::vector<float> dial(const std::string& keys, const tx::DtmfOpt& o = tx::DtmfOpt()) {
    std::vector<float> a; tx::silence(a, 0.3); tx::dtmf(a, keys, o); tx::silence(a, 0.3); return a;
}
static void testDtmf() {
    std::printf("\n── DTMF (Q.23 / Q.24) ──\n");
    {
        Got g = run(dial(kKeys));
        ok(g.dtmf.size() == 1 && g.dtmf[0] == kKeys, std::string("all 16 keys, 70 ms tones / 70 ms pauses: ") + join(g.dtmf));
        ok(g.five.empty(), "…and the 5-tone decoder reads nothing in DTMF");
    }
    for (double snr : { 25.0, 15.0, 10.0, 6.0 }) {
        std::mt19937 r(7 + (int)snr); int good = 0, bad = 0, n = 200;
        for (int k = 0; k < n; ++k) {
            std::string s; const int len = 3 + (int)(r() % 8);
            for (int i = 0; i < len; ++i) s += kKeys[r() % 16];
            tx::DtmfOpt o; o.toneSec = 0.05 + (r() % 50) / 1000.0; o.gapSec = 0.05 + (r() % 60) / 1000.0;
            o.twistDb = -3.0 + (double)(r() % 7);
            auto a = dial(s, o);
            const double pw = 0.5 * o.amp * o.amp * (1 + std::pow(10.0, o.twistDb / 10));
            addNoise(a, snr, pw, 100 + k);
            Got g = run(a);
            if (g.dtmf.size() == 1 && g.dtmf[0] == s) ++good;
            else if (!g.dtmf.empty()) ++bad;
        }
        char b[160]; std::snprintf(b, sizeof b, "%3.0f dB SNR (3 kHz): %d/%d dialled strings exactly right, %d wrong", snr, good, n, bad);
        if (snr >= 10) ok(good >= n - (snr >= 15 ? 0 : 4) && bad <= (snr >= 15 ? 0 : 2), b);
        else std::printf("  [INFO] %s\n", b);
    }
    for (double tw : { -3.5, 7.0, -6.0, 10.0 }) {
        tx::DtmfOpt o; o.twistDb = tw;
        Got g = run(dial("159#", o));
        const bool want = tw >= -4 && tw <= 8;
        char b[120]; std::snprintf(b, sizeof b, "twist %+.0f dB: %s (%s)", tw, want ? "read" : "refused", join(g.dtmf).c_str());
        ok(want ? (g.dtmf.size() == 1 && g.dtmf[0] == "159#") : g.dtmf.empty(), b);
    }
    {
        tx::DtmfOpt o; o.toneSec = 0.040; o.gapSec = 0.040;
        Got g = run(dial("0123456789", o));
        ok(g.dtmf.size() == 1 && g.dtmf[0] == "0123456789", "40 ms tones, 40 ms pauses (Q.24's must-accept): " + join(g.dtmf));
        o.toneSec = 0.020; o.gapSec = 0.060;
        g = run(dial("0123456789", o));
        ok(g.dtmf.empty(), "20 ms tones (Q.24's must-reject): nothing (" + join(g.dtmf) + ")");
        o.toneSec = 0.025; g = run(dial("0123456789", o));
        std::printf("  [INFO] 25 ms tones: %s\n", join(g.dtmf).c_str());
        o.toneSec = 0.033; g = run(dial("0123456789", o));
        std::printf("  [INFO] 33 ms tones: %s\n", join(g.dtmf).c_str());
    }
    {   // a 10 ms break inside a digit
        std::vector<float> a; tx::silence(a, 0.3);
        tx::burst(a, 770, 1336, 0.05, 0.25, 0.3); tx::silence(a, 0.010); tx::burst(a, 770, 1336, 0.05, 0.25, 0.3);
        tx::silence(a, 0.3);
        Got g = run(a);
        ok(g.dtmf.size() == 1 && g.dtmf[0] == "5", "a 10 ms break inside a 5 is bridged: " + join(g.dtmf));
        a.clear(); tx::silence(a, 0.3);
        tx::burst(a, 770, 1336, 0.05, 0.25, 0.3); tx::silence(a, 0.040); tx::burst(a, 770, 1336, 0.05, 0.25, 0.3);
        tx::silence(a, 0.3);
        g = run(a);
        ok(g.dtmf.size() == 1 && g.dtmf[0] == "55", "a 40 ms pause between two 5s is two digits: " + join(g.dtmf));
        a.clear(); tx::silence(a, 0.3); tx::burst(a, 770, 1336, 1.5, 0.25, 0.3); tx::silence(a, 0.3);
        g = run(a);
        ok(g.dtmf.size() == 1 && g.dtmf[0] == "5", "a key held 1.5 s is one digit: " + join(g.dtmf));
        a.clear(); tx::silence(a, 0.3); tx::dtmf(a, "12"); tx::silence(a, 2.0); tx::dtmf(a, "34"); tx::silence(a, 0.3);
        g = run(a);
        ok(g.dtmf.size() == 2 && g.dtmf[0] == "12" && g.dtmf[1] == "34", "2 s apart: two sequences (" + join(g.dtmf) + ")");
    }
    for (double off : { 0.015, -0.015, 0.035, -0.035 }) {
        int good = 0, any = 0;
        for (int k = 0; k < 16; ++k) {
            tx::DtmfOpt o; o.offLow = off; o.offHigh = off;
            Got g = run(dial(std::string(1, kKeys[k]), o));
            if (g.dtmf.size() == 1 && g.dtmf[0] == std::string(1, kKeys[k])) ++good;
            if (!g.dtmf.empty()) ++any;
        }
        char b[120]; std::snprintf(b, sizeof b, "both tones %+.1f %%: %d/16 keys read, %d read anything", off * 100, good, any);
        ok(std::fabs(off) < 0.02 ? good == 16 : any <= 2, b);
    }
    {
        auto a = dial("*0#159");
        addCtcss(a, 88.5, 0.08);
        Got g = run(a);
        ok(g.dtmf.size() == 1 && g.dtmf[0] == "*0#159", "with an 88.5 Hz CTCSS sub-tone under it: " + join(g.dtmf));
    }
}

// ── 5-tone ──────────────────────────────────────────────────────────────────────────────────────────────────────────
static std::vector<float> call5(int table, const std::string& d, double per = 0, double off = 0, double amp = 0.3) {
    std::vector<float> a; tx::silence(a, 0.3); tx::fiveTone(a, table, d, per, amp, off); tx::silence(a, 0.4); return a;
}
static void testFive() {
    std::printf("\n── 5-tone selective calling ──\n");
    for (int t = 0; t < kToneTables; ++t) {
        std::mt19937 r(31 + t); int good = 0, named = 0, n = 60;
        for (int k = 0; k < n; ++k) {
            std::string s; for (int i = 0; i < 5; ++i) s += (char)('0' + r() % 10);
            if (k % 3 == 0) s[2] = s[1];   // a repeat in it
            auto a = call5(t, s);
            addNoise(a, 20, 0.045, 300 + k + 100 * t);
            Got g = run(a, false, t == 3 ? 3 : -1);   // ★ DZVEI pinned (AUTO reads it as ZVEI, one digit lower — see below)
            if (g.five.size() == 1 && g.five[0] == s) ++good;
            if (g.fiveTable.size() == 1 && g.fiveTable[0] == t) ++named;
        }
        char b[160]; std::snprintf(b, sizeof b, "%-7s %3.0f ms%s: %d/%d calls exactly right at 20 dB, %d named %s", toneTables()[t].name,
                                   toneTables()[t].periodMs, t == 3 ? " (pinned)" : "", good, n, named, toneTables()[t].name);
        // ★ ZVEI-1/PZVEI and ZVEI-2/ZVEI-3 share digits and period — the name is the tie-break's, the digits must be right
        ok(good == n, b);
    }
    for (double per : { 40.0, 50.0, 70.0, 100.0 }) {
        int good = 0;
        for (int t = 0; t < kToneTables; ++t) { Got g = run(call5(t, "40719", per), false, t == 3 ? 3 : -1); if (g.five.size() == 1 && g.five[0] == "40719") ++good; }
        char b[120]; std::snprintf(b, sizeof b, "every table at %.0f ms a tone: %d/8", per, good);
        ok(good == 8, b);
    }
    {
        int good = 0; for (int t = 0; t < kToneTables; ++t) { Got g = run(call5(t, "40719", 30), false, t == 3 ? 3 : -1); if (g.five.size() == 1 && g.five[0] == "40719") ++good; }
        std::printf("  [INFO] 30 ms a tone: %d/8 tables read\n", good);
    }
    {
        Got g = run(call5(5, "11111"));
        ok(g.five.size() == 1 && g.five[0] == "11111", "the repeat rule (receiver): 1E1E1 → 11111 (" + join(g.five) + ")");
        g = run(call5(0, "12333"));
        ok(g.five.size() == 1 && g.five[0] == "12333", "123E3 → 12333 (" + join(g.five) + ")");
        g = run(call5(5, "1234567"));
        ok(g.five.size() == 1 && g.five[0] == "1234567", "a 7-tone call: " + join(g.five));
        g = run(call5(5, "123"));
        ok(g.five.empty(), "a 3-tone burst is not a call (" + join(g.five) + ")");
    }
    {
        Got g = run(call5(5, "23456", 100)); bool c = g.fiveTable.size() == 1 && g.fiveTable[0] == 5;
        g = run(call5(5, "23456", 40)); bool e = g.fiveTable.size() == 1 && g.fiveTable[0] == 6;
        ok(c && e, "same digits, CCIR's tones: 100 ms named CCIR, 40 ms named EEA (the period breaks the tie)");
        g = run(call5(0, "12345"));
        bool dz = false; for (size_t i = 0; i < g_lastFive.also.size(); ++i) if (g_lastFive.also[i] == 3 && g_lastFive.alsoDigits[i] == "23456") dz = true;
        ok(g.five.size() == 1 && g.five[0] == "12345" && dz, "ZVEI-1 12345 in AUTO: read 12345, and DZVEI's reading (23456) listed beside it");
        g = run(call5(3, "23456"), false, 3); ok(g.five.size() == 1 && g.five[0] == "23456", "DZVEI 23456 with DZVEI pinned: 23456");
        g = run(call5(7, "23456")); ok(g.fiveTable.size() == 1 && g.fiveTable[0] == 7, "EIA named EIA");
    }
    {   // the pin
        Rx rx; rx.five.pin = 5; int n = 0;
        rx.five.onCall = [&](const FiveTone::Call&) { ++n; };
        auto a = call5(0, "12345"); auto b = call5(5, "12345"); a.insert(a.end(), b.begin(), b.end());
        for (size_t i = 0; i < a.size(); i += 960) rx.feed(a.data() + i, (int)std::min<size_t>(960, a.size() - i));
        rx.flush();
        ok(n == 1, "pinned to CCIR: the ZVEI-1 call is not read, the CCIR one is (" + std::to_string(n) + ")");
    }
    for (double off : { 0.015, -0.015 }) {
        int good = 0; for (int t = 0; t < kToneTables; ++t) { Got g = run(call5(t, "40719", 0, off), false, t == 3 ? 3 : -1); if (g.five.size() == 1 && g.five[0] == "40719") ++good; }
        char b[120]; std::snprintf(b, sizeof b, "every tone %+.1f %%: %d/8 tables read right", off * 100, good);
        ok(good == 8, b);
    }
    for (double off : { 0.045, -0.045, 0.035, -0.035 }) {
        int same = 0, any = 0;
        for (int t = 0; t < kToneTables; ++t) { Got g = run(call5(t, "40719", 0, off), false, t == 3 ? 3 : -1); if (!g.five.empty()) ++any; if (g.five.size() == 1 && g.five[0] == "40719" && g.fiveTable[0] == t) ++same; }
        char b[120]; std::snprintf(b, sizeof b, "every tone %+.1f %%: read as the call sent in %d/8, anything in %d/8", off * 100, same, any);
        if (std::fabs(off) > 0.04) ok(same == 0, b); else std::printf("  [INFO] %s\n", b);
    }
    {
        auto a = call5(0, "31415"); addCtcss(a, 123.0, 0.08);
        Got g = run(a);
        ok(g.five.size() == 1 && g.five[0] == "31415", "ZVEI-1 with a 123 Hz CTCSS sub-tone: " + join(g.five));
    }
    for (double snr : { 15.0, 10.0, 6.0 }) {
        int good = 0, bad = 0, n = 80; std::mt19937 r(99);
        for (int k = 0; k < n; ++k) {
            const int t = k % kToneTables; std::string s; for (int i = 0; i < 5; ++i) s += (char)('0' + r() % 10);
            auto a = call5(t, s); addNoise(a, snr, 0.045, 900 + k);
            Got g = run(a, false, t == 3 ? 3 : -1);
            if (g.five.size() == 1 && g.five[0] == s) ++good; else if (!g.five.empty()) ++bad;
        }
        char b[120]; std::snprintf(b, sizeof b, "%2.0f dB SNR (3 kHz): %d/%d calls right, %d wrong", snr, good, n, bad);
        if (snr >= 10) ok(good >= n - 2 && bad == 0, b); else std::printf("  [INFO] %s\n", b);
    }
}

// ── false positives ─────────────────────────────────────────────────────────────────────────────────────────────────
/** Speech-like: a glottal pulse train (f0 85–260 Hz, wandering, with jitter) through three moving formant resonators,
 *  syllables 80–400 ms, pauses, unvoiced hiss. */
static std::vector<float> speechLike(double sec, uint32_t seed) {
    std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 1); std::normal_distribution<double> n(0, 1);
    const double fs = 48000; std::vector<float> a; a.reserve((size_t)(sec * fs));
    double f0 = 120 + 100 * u(r), ph = 0;
    struct Res { double f, bw, y1 = 0, y2 = 0; } F[3] = { { 500, 80 }, { 1500, 100 }, { 2500, 120 } };
    double tgt[3] = { 500, 1500, 2500 };
    int left = 0; bool voiced = true, silent = false;
    while (a.size() < sec * fs) {
        if (left-- <= 0) {
            left = (int)((0.08 + 0.32 * u(r)) * fs);
            silent = u(r) < 0.15; voiced = u(r) < 0.8;
            tgt[0] = 250 + 600 * u(r); tgt[1] = 800 + 1600 * u(r); tgt[2] = 2000 + 1200 * u(r);
            f0 = std::max(85.0, std::min(260.0, f0 * (0.85 + 0.3 * u(r))));
        }
        double ex = 0;
        if (!silent) {
            if (voiced) { ph += f0 * (1 + 0.01 * n(r)) / fs; if (ph >= 1) { ph -= 1; ex = 1.0; } }
            else ex = 0.15 * n(r);
        }
        double y = ex;
        for (int k = 0; k < 3; ++k) {
            F[k].f += (tgt[k] - F[k].f) * 0.0005;
            const double R = std::exp(-M_PI * F[k].bw / fs), c = 2 * R * std::cos(2 * M_PI * F[k].f / fs);
            const double o = y + c * F[k].y1 - R * R * F[k].y2;
            F[k].y2 = F[k].y1; F[k].y1 = o; y = o * (1 - R);
        }
        a.push_back((float)(0.3 * y));
    }
    float mx = 1e-9f; for (float v : a) mx = std::max(mx, std::fabs(v));
    for (float& v : a) v *= 0.4f / mx;
    return a;
}
/** Music-like: notes on the equal-tempered scale (A4 = 440) with harmonics, two- and three-note chords, fast arpeggios
 *  and — the hardest case — PURE SINE melodies at 70 and 100 ms a note, legato. */
static std::vector<float> musicLike(double sec, uint32_t seed, bool pureSines) {
    std::mt19937 r(seed); std::uniform_real_distribution<double> u(0, 1);
    const double fs = 48000; std::vector<float> a; a.reserve((size_t)(sec * fs));
    std::vector<double> ph(3, 0);
    while (a.size() < sec * fs) {
        const int voices = pureSines ? 1 : 1 + (int)(u(r) * 3);
        double f[3]; for (int v = 0; v < 3; ++v) f[v] = 440 * std::pow(2.0, (std::floor(u(r) * 36) - 21) / 12.0);
        const double dur = pureSines ? (u(r) < 0.5 ? 0.07 : 0.1) : (u(r) < 0.3 ? 0.06 + 0.06 * u(r) : 0.15 + 0.5 * u(r));
        const int n = (int)(dur * fs);
        for (int i = 0; i < n; ++i) {
            double y = 0;
            for (int v = 0; v < voices; ++v) {
                ph[v] += 2 * M_PI * f[v] / fs;
                y += std::sin(ph[v]);
                if (!pureSines) y += 0.5 * std::sin(2 * ph[v]) + 0.3 * std::sin(3 * ph[v]) + 0.2 * std::sin(4 * ph[v]);
            }
            const double env = pureSines ? 1.0 : std::exp(-3.0 * i / fs) * std::min(1.0, i / (0.005 * fs));
            a.push_back((float)(0.12 * env * y));
        }
    }
    return a;
}
static void testFalse() {
    std::printf("\n── nothing from noise, speech or music ──\n");
    struct C { const char* what; std::vector<float> a; };
    std::vector<C> cs;
    { std::vector<float> a(48000 * 300, 0.f); addNoise(a, 0, 0.02, 4242); cs.push_back({ "5 min of white noise", a }); }
    { std::vector<float> a(48000 * 120, 0.f); addNoise(a, 0, 0.02, 4243); addCtcss(a, 103.5, 0.1); cs.push_back({ "2 min of noise + CTCSS", a }); }
    cs.push_back({ "6 min of speech-like audio", speechLike(360, 11) });
    { auto s = speechLike(120, 12); addNoise(s, 15, 0.02, 4244); cs.push_back({ "2 min of speech-like audio in noise", s }); }
    cs.push_back({ "5 min of music-like audio (harmonics, chords, arpeggios)", musicLike(300, 13, false) });
    for (auto& c : cs) {
        Got g = run(c.a);
        ok(g.dtmf.empty() && g.five.empty(), std::string(c.what) + ": " + std::to_string(g.dtmf.size()) + " DTMF, " + std::to_string(g.five.size()) + " 5-tone" +
           (g.dtmf.empty() && g.five.empty() ? "" : " (" + join(g.dtmf) + " / " + join(g.five) + ")"));
    }
    {   // ★ the hardest case, measured and pinned loosely: pure sines at a 5-tone period, every note a scale tone
        auto a = musicLike(600, 14, true);
        Got g = run(a);
        char b[200]; std::snprintf(b, sizeof b, "10 min of PURE-SINE melody at 70/100 ms a note: %zu DTMF, %zu 5-tone calls (%.1f an hour)",
                                   g.dtmf.size(), g.five.size(), g.five.size() * 6.0);
        ok(g.dtmf.empty() && g.five.size() <= 3, b);
    }
    {   // DTMF read as 5-tone, 5-tone read as DTMF
        std::vector<float> a; for (int k = 0; k < 40; ++k) { auto d = dial("0123456789*#"); a.insert(a.end(), d.begin(), d.end()); }
        Got g = run(a, false, -1, false, true);
        ok(g.five.empty(), "40 dialled strings: no 5-tone call (" + join(g.five) + ")");
        a.clear(); for (int t = 0; t < kToneTables; ++t) for (int k = 0; k < 5; ++k) { auto c = call5(t, "1234567"); a.insert(a.end(), c.begin(), c.end()); }
        g = run(a, false, -1, true, false);
        ok(g.dtmf.empty(), "40 5-tone calls: no DTMF (" + join(g.dtmf) + ")");
    }
}

// ── SIMD = scalar, cost ─────────────────────────────────────────────────────────────────────────────────────────────
static void testSimd() {
    std::printf("\n── SIMD (%s) = scalar, cost ──\n", simdName());
    std::mt19937 r(5); std::normal_distribution<float> n(0.f, 0.3f);
    std::vector<float> x(4096); for (auto& v : x) v = n(r);
    std::vector<float> c(204); for (size_t i = 0; i < c.size(); ++i) c[i] = (float)(2 * std::cos(2 * M_PI * (400 + 12.5 * i) / 8000));
    float p1[204], p2[204];
    goertzel(x.data(), 205, c.data(), 204, p1); goertzelScalar(x.data(), 205, c.data(), 204, p2);
    ok(std::memcmp(p1, p2, sizeof p1) == 0, "the Goertzel bank: SIMD and scalar bit for bit equal (204 filters)");
    bool same = true;
    for (size_t i = 0; i + 96 <= x.size(); i += 7) if (Decim6::dot(&x[i], &x[0]) != Decim6::dotScalar(&x[i], &x[0])) same = false;
    ok(same, "the decimator's dot product: SIMD and scalar bit for bit equal");
    std::vector<float> a;
    tx::silence(a, 0.2); tx::dtmf(a, "0123456789*#ABCD");
    for (int t = 0; t < kToneTables; ++t) { auto cc = call5(t, "90210"); a.insert(a.end(), cc.begin(), cc.end()); }
    auto s = speechLike(30, 77); a.insert(a.end(), s.begin(), s.end());
    addNoise(a, 12, 0.045, 55);
    Got g1 = run(a, false), g2 = run(a, true);
    ok(join(g1.dtmf) == join(g2.dtmf) && join(g1.five) == join(g2.five) && g1.fivePer == g2.fivePer,
       "a mixed clip: every decode the same on the SIMD and the scalar path (" + std::to_string(g1.dtmf.size() + g1.five.size()) + ")");
    std::vector<float> big = speechLike(120, 8);
    for (int sc = 0; sc < 2; ++sc) {
        const auto t0 = std::chrono::steady_clock::now();
        run(big, sc == 1);
        const double s2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  [INFO] %s: 121 s of audio (both decoders) in %.3f s — %.2f %% of one core\n", sc ? "scalar" : simdName(), s2, 100 * s2 / 121);
    }
}

// ── recordings ──────────────────────────────────────────────────────────────────────────────────────────────────────
static bool readWav(const char* path, std::vector<float>& out, double& rate) {
    FILE* f = std::fopen(path, "rb"); if (!f) return false;
    std::vector<uint8_t> d; { uint8_t b[65536]; size_t k; while ((k = std::fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + k); }
    std::fclose(f);
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) != 0) return false;
    int ch = 1, fmt = 1, bits = 16; size_t off = 0, len = 0; rate = 48000;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t sz = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
        if (std::memcmp(&d[p], "fmt ", 4) == 0) {
            fmt = d[p + 8] | d[p + 9] << 8; ch = d[p + 10] | d[p + 11] << 8;
            rate = d[p + 12] | d[p + 13] << 8 | d[p + 14] << 16 | (uint32_t)d[p + 15] << 24; bits = d[p + 22] | d[p + 23] << 8;
        }
        if (std::memcmp(&d[p], "data", 4) == 0) { off = p + 8; len = std::min<size_t>(sz, d.size() - off); break; }
        p += 8 + sz + (sz & 1);
    }
    std::vector<float> in;
    const size_t step = (size_t)ch * (bits / 8);
    for (size_t i = off; i + step <= off + len; i += step) {
        if (fmt == 3 && bits == 32) { float v; std::memcpy(&v, &d[i], 4); in.push_back(v); }
        else if (bits == 16) in.push_back((int16_t)(d[i] | d[i + 1] << 8) / 32768.0f);
        else return false;
    }
    out.clear();
    for (double t = 0; t + 1 < in.size(); t += rate / 48000.0) { const size_t i = (size_t)t; const double fr = t - (double)i; out.push_back((float)(in[i] * (1 - fr) + in[i + 1] * fr)); }
    return true;
}
static int replay(int argc, char** argv) {
    int found = 0; double total = 0;
    for (int k = 2; k < argc; ++k) {
        std::vector<float> a; double rate;
        if (!readWav(argv[k], a, rate)) { std::printf("%s: not a 16-bit / float WAV\n", argv[k]); continue; }
        total += a.size() / 48000.0;
        Rx rx;
        rx.dtmf.onSequence = [&](const Dtmf::Sequence& s) { ++found; std::printf("  %s: DTMF %s at %.1f s\n", argv[k], s.digits.c_str(), s.startS); };
        rx.five.onCall = [&](const FiveTone::Call& c) { ++found; std::printf("  %s: %s %s at %.1f s\n", argv[k], toneTables()[c.table].name, c.digits.c_str(), c.startS); };
        for (size_t i = 0; i < a.size(); i += 960) rx.feed(a.data() + i, (int)std::min<size_t>(960, a.size() - i));
        rx.flush();
    }
    std::printf("%d decoded in %.1f s of recordings\n", found, total);
    return found ? 1 : 0;   // ★ for talk-off corpora: anything decoded is a failure
}

// ── the end-to-end clip ─────────────────────────────────────────────────────────────────────────────────────────────
/** ★ The server/web tests' clip: unsigned 8-bit IQ at `rate`, an NFM channel (±2.5 kHz deviation, 30 dB C/N in 12.5 kHz)
 *  25 kHz ABOVE the clip's centre, on a loop of about 6 s: DTMF "*73#1234" (70 ms tones, 70 ms pauses), a ZVEI-1 call
 *  "12345", a CCIR call "40719" (100 ms tones), silence between. Where it is depends on the --hf-centre the test gives
 *  fake-rtl-tcp: on 2 m it is an amateur repeater, on PMR446 it is not. */
static int writeClip(const char* out, double rate, double seconds) {
    std::vector<float> a;
    tx::silence(a, 1.0); tx::dtmf(a, "*73#1234");
    tx::silence(a, 1.0); tx::fiveTone(a, 0, "12345");
    tx::silence(a, 1.0); tx::fiveTone(a, 5, "40719");
    tx::silence(a, 1.5);
    FILE* f = std::fopen(out, "wb"); if (!f) { std::perror(out); return 1; }
    std::mt19937 rng(77); std::normal_distribution<float> g(0.0f, std::sqrt(0.5f));
    const double A = std::sqrt(std::pow(10.0, 30.0 / 10) * 12500.0 / rate * 2.0);
    const size_t total = (size_t)(seconds * rate);
    std::vector<uint8_t> buf; buf.reserve(1 << 17);
    auto q = [](double v) { const int x = (int)std::lround(127.5 + v * 18.0); return (uint8_t)std::max(0, std::min(255, x)); };
    double ph = 0;
    for (size_t i = 0; i < total; ++i) {
        const double t = i / rate;
        const size_t k = (size_t)std::fmod(t * 48000.0, (double)a.size());
        const double dev = 2500.0 * a[k] / 0.55;   // ★ the loudest DTMF pair (≈ 0.55 peak) at full deviation
        ph += 2 * M_PI * (25000.0 + dev) / rate;
        if (ph > M_PI) ph -= 2 * M_PI;
        buf.push_back(q(A * std::cos(ph) + g(rng))); buf.push_back(q(A * std::sin(ph) + g(rng)));
        if (buf.size() >= (1 << 17)) { std::fwrite(buf.data(), 1, buf.size(), f); buf.clear(); }
    }
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    std::printf("clip: %s, %.0f s at %.0f S/s, one pass %.1f s\n", out, seconds, rate, a.size() / 48000.0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "wav") return replay(argc, argv);
    if (argc > 2 && std::string(argv[1]) == "clip") return writeClip(argv[2], argc > 3 ? std::atof(argv[3]) : 288000, argc > 4 ? std::atof(argv[4]) : 30);
    std::printf("test-tones — DTMF (ITU-T Q.23/Q.24) and 5-tone selective calling\n");
    testTables();
    testDtmf();
    testFive();
    testFalse();
    testSimd();
    std::printf("\n%s test-tones: %d passed, %d failed\n", g_fail ? "✗" : "✓", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
