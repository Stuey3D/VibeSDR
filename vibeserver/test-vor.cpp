// test-vor.cpp — V12 VOR bearing and ILS deviation (decoders/nav_rx.h, docs/v12/VOR-ILS.md). ★ Silent; deterministic; our own
// transmitter (decoders/nav_tx.h).
//
// ★★★ WHAT IS PINNED.
//   • Bearings: 36 radials 0–350° plus odd ones, at four channel rates (25 / 31.25 / 37.5 / 48 kHz), within 0.5° at 40 dB and
//     1.5° at 20 dB; a DVOR is the same signal; ±2 kHz off the dial; the transmitter's 30 Hz 0.1 % fast.
//   • ILS: localiser DDM −0.155 … +0.155 within 0.005, the side (90 Hz dominant = fly right); glideslope 40 % + 40 %.
//   • The ident: LON, BIG, OTR, MAY at 6–10 WPM read from the keying, in noise; a localiser's I-code.
//   • FALSE POSITIVES: minutes of noise, plain AM speech, an ILS (no bearing), a VOR (no ILS) — none; a VOR with voice on it
//     keeps its bearing.
//   • SIMD = scalar bit for bit (the decimated streams' checksum and every bearing).
//   • Cost on this machine.
//   `test-vor clip <out.cu8> <rate> <seconds> <offsetHz> <bearing>` writes the end-to-end clip (unsigned 8-bit IQ, rtl_tcp
//   style): a VOR "LON" `offsetHz` above the clip's centre.
#include "decoders/nav_rx.h"
#include "decoders/nav_tx.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace vibe::nav;

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); c ? ++g_pass : ++g_fail; }
static double angDiff(double a, double b) { double d = std::fmod(a - b + 540.0, 360.0) - 180.0; return d; }

struct Got {
    std::vector<Report> reps;
    std::vector<std::string> words;
    double checksum = 0;
    Report lastVor, lastIls;
    int vorN = 0, ilsN = 0;
};
static Got run(const std::vector<std::complex<float>>& x, double rate, bool scalar = false) {
    NavRx rx(rate, scalar);
    Got g;
    rx.onSecond = [&](const Report& r) {
        g.reps.push_back(r);
        if (r.kind == Report::Vor) { g.lastVor = r; ++g.vorN; }
        if (r.kind == Report::Ils) { g.lastIls = r; ++g.ilsN; }
    };
    rx.keyer.onWord = [&](const std::string& w, double) { g.words.push_back(w); };
    const int blk = (int)(rate / 50);
    for (size_t i = 0; i < x.size(); i += (size_t)blk) rx.feed(x.data() + i, (int)std::min<size_t>((size_t)blk, x.size() - i));
    g.checksum = rx.checksum();
    return g;
}

static void testBearings() {
    std::printf("\n── VOR bearings ──\n");
    const double rates[4] = { 25000, 31250, 37500, 48000 };
    for (double rate : rates) {
        double worst = 0; int missing = 0;
        for (int k = 0; k < 40; ++k) {
            tx::VorOpt o; o.rate = rate; o.bearing = k < 36 ? k * 10.0 : (k - 36) * 89.3 + 1.7; o.seed = (uint32_t)k + 11;
            const Got g = run(tx::vor(6, o), rate);
            if (!g.vorN) { ++missing; continue; }
            worst = std::max(worst, std::fabs(angDiff(g.lastVor.bearing, o.bearing)));
        }
        char b[200]; std::snprintf(b, sizeof b, "%.2f kHz: 40 radials at 40 dB, every one read within 0.5° (worst %.3f°, %d not read)", rate / 1e3, worst, missing);
        ok(missing == 0 && worst <= 0.5, b);
    }
    {
        double worst = 0; int missing = 0;
        for (int k = 0; k < 24; ++k) {
            tx::VorOpt o; o.bearing = k * 15.0 + 3; o.snrDb = 20; o.seed = 100 + k;
            const Got g = run(tx::vor(8, o), o.rate);
            if (!g.vorN) { ++missing; continue; }
            worst = std::max(worst, std::fabs(angDiff(g.lastVor.bearing, o.bearing)));
        }
        char b[200]; std::snprintf(b, sizeof b, "20 dB (25 kHz): 24 radials within 1.5° (worst %.2f°, %d not read)", worst, missing);
        ok(missing == 0 && worst <= 1.5, b);
    }
    {
        tx::VorOpt o; o.bearing = 247.5; o.offsetHz = 2000;
        Got g = run(tx::vor(6, o), o.rate);
        ok(g.vorN && std::fabs(angDiff(g.lastVor.bearing, 247.5)) < 0.5, "2 kHz off the dial: 247.5° read");
        o.offsetHz = -2000; g = run(tx::vor(6, o), o.rate);
        ok(g.vorN && std::fabs(angDiff(g.lastVor.bearing, 247.5)) < 0.5, "−2 kHz off the dial: 247.5° read");
        o.offsetHz = 0; o.ref30 = 30.03; g = run(tx::vor(6, o), o.rate);
        ok(g.vorN && std::fabs(angDiff(g.lastVor.bearing, 247.5)) < 0.5, "the station's 30 Hz 0.1 % fast: 247.5° read");
        o.ref30 = 30; o.voiceDepth = 0.3; g = run(tx::vor(10, o), o.rate);
        char b[160]; std::snprintf(b, sizeof b, "voice on the VOR (30 %%): the bearing holds (%s)", g.vorN ? std::to_string(g.lastVor.bearing).c_str() : "none");
        ok(g.vorN && std::fabs(angDiff(g.lastVor.bearing, 247.5)) < 1.0, b);
    }
    {
        tx::VorOpt o; o.bearing = 123; o.snrDb = 5; o.seed = 77;
        const Got g = run(tx::vor(10, o), o.rate);
        char b[160]; std::snprintf(b, sizeof b, "5 dB: a bearing only if right (%d reports%s)", g.vorN,
                                   g.vorN ? (", " + std::to_string(g.lastVor.bearing)).c_str() : "");
        ok(!g.vorN || std::fabs(angDiff(g.lastVor.bearing, 123)) < 4, b);
    }
}

static void testIls() {
    std::printf("\n── ILS ──\n");
    double worst = 0; int missing = 0;
    for (int k = -8; k <= 8; ++k) {
        tx::IlsOpt o; const double ddm = k * 0.155 / 8; o.m90 = 0.2 + ddm / 2; o.m150 = 0.2 - ddm / 2; o.seed = 300 + k + 8;
        const Got g = run(tx::ils(6, o), o.rate);
        if (!g.ilsN) { ++missing; continue; }
        worst = std::max(worst, std::fabs(g.lastIls.ddm - ddm));
    }
    char b[200]; std::snprintf(b, sizeof b, "localiser: DDM −0.155…+0.155 in 17 steps within 0.005 (worst %.4f, %d not read)", worst, missing);
    ok(missing == 0 && worst <= 0.005, b);
    {
        tx::IlsOpt o; o.m90 = 0.25; o.m150 = 0.15;
        const Got g = run(tx::ils(6, o), o.rate);
        ok(g.ilsN && g.lastIls.ddm > 0.09 && g.lastIls.ddm < 0.11 && g.vorN == 0, "90 Hz dominant: DDM +0.10 (left of course: fly right), never a bearing");
    }
    {
        tx::IlsOpt o; o.m90 = 0.35; o.m150 = 0.45; o.identDepth = 0;
        const Got g = run(tx::ils(6, o), o.rate);
        ok(g.ilsN && std::fabs(g.lastIls.ddm + 0.10) < 0.005 && std::fabs(g.lastIls.sdm - 0.80) < 0.01, "glideslope 35 % + 45 %: DDM −0.10, SDM 0.80");
    }
    {
        tx::IlsOpt o; o.snrDb = 15;
        const Got g = run(tx::ils(8, o), o.rate);
        ok(g.ilsN && std::fabs(g.lastIls.ddm) < 0.01, "localiser at 15 dB: on course (DDM within 0.01)");
    }
}

static void testIdent() {
    std::printf("\n── the ident ──\n");
    const char* ids[] = { "LON", "BIG", "OTR", "MAY", "TNT", "MOT" };
    const double wpm[] = { 7, 6, 8, 10, 7, 9 };
    for (int i = 0; i < 6; ++i) {
        tx::VorOpt o; o.ident = ids[i]; o.wpm = wpm[i]; o.snrDb = 25; o.seed = 500 + i;
        const Got g = run(tx::vor(45, o), o.rate);
        std::map<std::string, int> c; for (auto& w : g.words) ++c[w];
        std::string all; for (auto& w : g.words) all += w + " ";
        char b[240]; std::snprintf(b, sizeof b, "%s at %.0f WPM: read in ≥ 3 of 4 repeats (%s)", ids[i], wpm[i], all.c_str());
        ok(c[ids[i]] >= 3, b);
    }
    {
        tx::IlsOpt o; o.ident = "ILL"; o.snrDb = 25;
        const Got g = run(tx::ils(40, o), o.rate);
        std::map<std::string, int> c; for (auto& w : g.words) ++c[w];
        ok(c["ILL"] >= 3, "a localiser's ident ILL read");
    }
}

static void testFalse() {
    std::printf("\n── false positives ──\n");
    int vor = 0, ils = 0;
    for (int s = 0; s < 5; ++s) {
        std::vector<std::complex<float>> x((size_t)(60 * 37500), std::complex<float>(0, 0));
        tx::addNoise(x, 0, 37500, 900 + s);   // noise alone, unit power in 25 kHz
        const Got g = run(x, 37500);
        vor += g.vorN; ils += g.ilsN;
    }
    ok(vor == 0 && ils == 0, "5 min of noise: no bearing, no ILS (" + std::to_string(vor) + ", " + std::to_string(ils) + ")");
    vor = ils = 0;
    for (int s = 0; s < 5; ++s) {
        const Got g = run(tx::amSpeech(60, 37500, 30, 910 + s), 37500);
        vor += g.vorN; ils += g.ilsN;
    }
    ok(vor == 0 && ils == 0, "5 min of airband AM speech: no bearing, no ILS (" + std::to_string(vor) + ", " + std::to_string(ils) + ")");
    {
        // a carrier only, and a carrier with a 9960 Hz tone but no FM (half a VOR)
        std::vector<std::complex<float>> x((size_t)(30 * 37500), std::complex<float>(1, 0));
        tx::addNoise(x, 30, 37500, 920);
        Got g = run(x, 37500);
        ok(g.vorN == 0 && g.ilsN == 0, "a plain carrier: nothing");
        tx::VorOpt o; o.devHz = 0; g = run(tx::vor(30, o), o.rate);
        ok(g.vorN == 0, "9960 Hz without its FM: no bearing");
        o.devHz = 480; o.varDepth = 0.0; g = run(tx::vor(30, o), o.rate);
        ok(g.vorN == 0, "no variable 30 Hz: no bearing");
    }
    {
        tx::VorOpt o; const Got g = run(tx::vor(20, o), o.rate);
        ok(g.ilsN == 0, "a VOR is never read as an ILS");
    }
}

static void testSimd() {
    std::printf("\n── SIMD = scalar (%s) ──\n", simdName());
    tx::VorOpt o; o.bearing = 311.1; o.snrDb = 25; o.voiceDepth = 0.1;
    const auto x = tx::vor(12, o);
    const Got a = run(x, o.rate, false), b = run(x, o.rate, true);
    bool same = a.reps.size() == b.reps.size();
    for (size_t i = 0; same && i < a.reps.size(); ++i)
        same = a.reps[i].kind == b.reps[i].kind && (std::isnan(a.reps[i].bearing) ? std::isnan(b.reps[i].bearing) : a.reps[i].bearing == b.reps[i].bearing);
    ok(a.checksum == b.checksum && same, "the envelope + decimator bit for bit, every report the same");
    std::vector<float> env1(1003), env2(1003);
    std::vector<float> iq(2006); for (size_t i = 0; i < iq.size(); ++i) iq[i] = (float)std::sin(i * 0.37) * 3.1f;
    envelope(iq.data(), 1003, env1.data()); envelopeScalar(iq.data(), 1003, env2.data());
    ok(std::memcmp(env1.data(), env2.data(), env1.size() * sizeof(float)) == 0, "the envelope kernel bit for bit");
}

static void testPairs() {
    std::printf("\n── the band plan ──\n");
    ok(channelKind(113.60e6) == 1 && channelKind(109.20e6) == 1 && channelKind(108.60e6) == 1, "113.60, 109.20, 108.60: VOR");
    ok(channelKind(109.50e6) == 2 && channelKind(110.95e6) == 2 && channelKind(108.10e6) == 2, "109.50, 110.95, 108.10: localiser");
    ok(channelKind(332.60e6) == 3 && channelKind(121.5e6) == 0, "332.60 glideslope; 121.5 neither");
    ok(pairedKhz(109.50e6) == 332600 && pairedKhz(108.10e6) == 334700 && pairedKhz(111.95e6) == 330950 && pairedKhz(332.60e6) == 109500,
       "LOC 109.50 ↔ GS 332.60, 108.10 ↔ 334.70, 111.95 ↔ 330.95");
    ok(channelKind(113.62e6) == 1 && channelHz(113.62e6) == 113600000, "a dial 20 kHz off snaps to its channel");
}

static void testCost() {
    std::printf("\n── cost ──\n");
    tx::VorOpt o; const auto x = tx::vor(60, o);
    NavRx rx(o.rate);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < x.size(); i += 750) rx.feed(x.data() + i, 750);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char b[160]; std::snprintf(b, sizeof b, "60 s of VOR at 37.5 kHz in %.3f s (%.2f %% of one core, %s)", s, s / 60 * 100, simdName());
    ok(s < 6.0, b);
}

/** The end-to-end clip: a VOR `offsetHz` above the capture's centre, cu8. */
static int writeClip(const char* out, double rate, double sec, double offsetHz, double bearing) {
    tx::VorOpt o; o.rate = rate; o.bearing = bearing; o.offsetHz = offsetHz; o.snrDb = 35; o.identEvery = 8;
    const auto x = tx::vor(sec, o);
    FILE* f = std::fopen(out, "wb");
    if (!f) return 1;
    std::vector<uint8_t> b(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        auto q = [](float v) { int s = (int)std::lround(127.5 + v * 40.0); return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s); };
        b[2 * i] = q(x[i].real()); b[2 * i + 1] = q(x[i].imag());
    }
    std::fwrite(b.data(), 1, b.size(), f); std::fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 7 && !std::strcmp(argv[1], "clip")) return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]), std::atof(argv[5]), std::atof(argv[6]));
    std::printf("test-vor — VOR / ILS (%s)\n", simdName());
    testPairs();
    testBearings();
    testIls();
    testIdent();
    testFalse();
    testSimd();
    testCost();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
