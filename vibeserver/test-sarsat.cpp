// test-sarsat.cpp — V12 COSPAS-SARSAT 406 MHz beacons (decoders/sarsat_*.h, docs/v12/SARSAT.md). ★ Silent; deterministic; our
// own transmitter (decoders/sarsat_tx.h) and the specification's own worked examples (C/S T.001 Annex B).
//
// ★★★ WHAT IS PINNED.
//   • T.001 Annex B: the B1 short message's BCH-1 (001011001010101001001) and 15 Hex ID ADCD0 08004 40401; B2's BCH-2
//     (0001 0101 0001) for its user-location PDF-2.
//   • The BCH decoders: every 1-, 2- and 3-error pattern of BCH-1 on random words corrected (sampled), 2 of BCH-2; 4 errors
//     never "corrected" to the sent word.
//   • The protocols: standard location EPIRB-MMSI (position to 4 s), serial user-location ELT (4 min), aviation user
//     (registration), national location PLB (2 min + 4 s), test user — fields and positions round trip.
//   • The modem: bursts at 25 / 48 / 64 kHz channel rates, ±3 kHz off the dial, 396 / 404 bit/s, ±1.0 / 1.2 rad, either
//     sense; C/N0 down to 34 dB-Hz; the self-test sync labelled.
//   • FALSE POSITIVES: an hour of noise, a steady carrier, a keyed carrier, a CW carrier keyed like a beacon but unmodulated,
//     PSK-like random bursts of the right length — nothing.
//   • SIMD = scalar bit for bit; cost.
//   `test-sarsat clip <out.cu8> <rate> <offsetHz>` writes the end-to-end clip.
#include "decoders/sarsat_rx.h"
#include "decoders/sarsat_tx.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vibe::sarsat;

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const std::string& w) { std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); c ? ++g_pass : ++g_fail; }

struct Got { std::vector<Burst> b; Rx* rx = nullptr; int lines = 0, steady = 0, attempts = 0, refused = 0; double checksum = 0; std::string why; double sync = 0; };
static Got run(const std::vector<std::complex<float>>& x, double rate, bool scalar = false) {
    Rx rx(rate, scalar);
    Got g;
    rx.onBurst = [&](const Burst& b) { g.b.push_back(b); };
    const int blk = (int)(rate / 50);
    for (size_t i = 0; i < x.size(); i += (size_t)blk) rx.feed(x.data() + i, (int)std::min<size_t>((size_t)blk, x.size() - i));
    // a second of silence (noise-free zeros) lets the last burst be worked
    std::vector<std::complex<float>> z((size_t)rate, std::complex<float>(0, 0));
    for (size_t i = 0; i < z.size(); i += (size_t)blk) rx.feed(z.data() + i, blk);
    g.lines = rx.lines; g.steady = rx.steady; g.attempts = rx.attempts; g.refused = rx.refused; g.checksum = rx.checksum(); g.why = rx.why; g.sync = rx.lastSync;
    return g;
}
static std::string bitsStr(const Bits& b, int from, int to) { std::string s; for (int i = from; i <= to; ++i) s += (char)('0' + b[(size_t)i]); return s; }

static void testAnnexB() {
    std::printf("\n── C/S T.001 Annex B ──\n");
    Bits b = tx::annexB();
    ok(bitsStr(b, 86, 106) == "001011001010101001001", "B1: the 21-bit BCH of the sample message is 001011001010101001001 (" + bitsStr(b, 86, 106) + ")");
    ok(bch1().syndrome(b) == 0, "B1: its syndrome is zero");
    const Message m = parse(b, false);
    ok(m.hexId == "ADCD0080044040" "1", "B1: 15 Hex ID ADCD0 08004 40401 (" + m.hexId + ")");
    ok(m.protocol == "Serial user" && m.beaconType == "EPIRB (float-free)" && m.homing121, "B1: serial user, float-free EPIRB, 121.5 MHz homing (" + m.protocol + ", " + m.beaconType + ")");
    ok(m.hexMsg == "56E680400220200965525" "0", "B1: bits 25–112 = 56E68 04002 20200 96552 50 (" + m.hexMsg + ")");
    // B2: user-location PDF-2 10 0101 0111 0000 0000 0001 0111 → BCH-2 0001 0101 0001
    Bits c{};
    const char* p2 = "10010101110000000000010111";
    for (int i = 0; i < 26; ++i) c[(size_t)(107 + i)] = (uint8_t)(p2[i] - '0');
    bch2().encode(c);
    ok(bitsStr(c, 133, 144) == "000101010001", "B2: the 12-bit BCH of the sample PDF-2 is 0001 0101 0001 (" + bitsStr(c, 133, 144) + ")");
    // B2's position, read back: 43°32′ N, 1°28′ E
    Bits d = c; d[25] = 1; d[26] = 1; setField(d, 27, 36, 227); setField(d, 37, 39, 3); setField(d, 40, 42, 0);
    const Message md = parse(d, false);
    ok(md.hasPosition && std::fabs(md.lat - (43 + 32 / 60.0)) < 1e-9 && std::fabs(md.lon - (1 + 28 / 60.0)) < 1e-9, "B2: 43°32′ N, 001°28′ E");
}

static void testBch() {
    std::printf("\n── the BCH decoders ──\n");
    std::mt19937 r(5);
    int fixed[4] = { 0, 0, 0, 0 }, tried[4] = { 0, 0, 0, 0 }, wrong4 = 0;
    for (int it = 0; it < 4000; ++it) {
        Bits b{}; b[25] = r() & 1; for (int i = 26; i <= 85; ++i) b[(size_t)i] = r() & 1; bch1().encode(b);
        const int ne = it % 4 + 1;
        Bits e = b; std::vector<int> pos;
        while ((int)pos.size() < ne) { const int p = 25 + (int)(r() % 82); if (std::find(pos.begin(), pos.end(), p) == pos.end()) pos.push_back(p); }
        for (int p : pos) e[(size_t)p] ^= 1;
        ++tried[ne - 1];
        const int got = bch1().correct(e);
        bool same = true; for (int i = 25; i <= 106; ++i) if (e[(size_t)i] != b[(size_t)i]) same = false;
        if (ne <= 3 && got == ne && same) ++fixed[ne - 1];
        if (ne == 4 && same) ++wrong4;
    }
    char t[200]; std::snprintf(t, sizeof t, "BCH-1: 1/2/3 errors corrected %d/%d, %d/%d, %d/%d; 4 errors never back to the sent word (%d)",
                               fixed[0], tried[0], fixed[1], tried[1], fixed[2], tried[2], wrong4);
    ok(fixed[0] == tried[0] && fixed[1] == tried[1] && fixed[2] == tried[2] && wrong4 == 0, t);
    int f2 = 0, t2 = 0, w3 = 0;
    for (int it = 0; it < 3000; ++it) {
        Bits b{}; for (int i = 107; i <= 132; ++i) b[(size_t)i] = r() & 1; bch2().encode(b);
        const int ne = it % 3 + 1;
        Bits e = b; std::vector<int> pos;
        while ((int)pos.size() < ne) { const int p = 107 + (int)(r() % 38); if (std::find(pos.begin(), pos.end(), p) == pos.end()) pos.push_back(p); }
        for (int p : pos) e[(size_t)p] ^= 1;
        const int got = bch2().correct(e);
        bool same = true; for (int i = 107; i <= 144; ++i) if (e[(size_t)i] != b[(size_t)i]) same = false;
        if (ne <= 2) { ++t2; if (got == ne && same) ++f2; } else if (same) ++w3;
    }
    std::snprintf(t, sizeof t, "BCH-2: 1–2 errors corrected %d/%d; 3 errors never back to the sent word (%d)", f2, t2, w3);
    ok(f2 == t2 && w3 == 0, t);
}

static void testProtocols() {
    std::printf("\n── the protocols ──\n");
    {
        const Bits b = tx::standardLocationEpirb(232, 123456, 50.7123, -1.3456);
        const Message m = parse(b, false);
        char t[260]; std::snprintf(t, sizeof t, "standard location EPIRB-MMSI: %s · %s · %.5f, %.5f (%s) — 4 s resolution", m.identity.c_str(), m.beaconType.c_str(), m.lat, m.lon, m.posRes.c_str());
        ok(m.identity.rfind("MMSI 232123456", 0) == 0 && m.beaconType == "EPIRB" && m.hasPosition && std::fabs(m.lat - 50.7123) < 0.0012 && std::fabs(m.lon + 1.3456) < 0.0012 && m.posRes == "4 s", t);
        ok(m.hexId.size() == 15 && !m.testProtocol, "its 15 Hex ID has the default position (" + m.hexId + ")");
        const Message mt = parse(tx::standardLocationEpirb(232, 123456, 50.7, -1.3, true), true);
        ok(mt.selfTest, "the self-test frame sync is labelled a test");
    }
    {
        const Message m = parse(tx::serialUserLocation(227, 98765, -33.9, 151.2), false);
        char t[260]; std::snprintf(t, sizeof t, "serial user-location ELT: %s · %s · %.3f, %.3f (%s)", m.identity.c_str(), m.protocol.c_str(), m.lat, m.lon, m.posRes.c_str());
        ok(m.protocol == "Serial user-location" && m.beaconType == "ELT" && m.identity.rfind("serial 98765", 0) == 0 && std::fabs(m.lat + 33.9) < 0.04 && std::fabs(m.lon - 151.2) < 0.04, t);
    }
    {
        const Message m = parse(tx::aviationUser(232, "G-ABCD"), false);
        ok(m.protocol == "Aviation user" && m.identity.rfind("registration G-ABCD", 0) == 0 && !m.hasPosition && m.beaconType == "ELT", "aviation user, short: " + m.identity);
    }
    {
        const Message m = parse(tx::nationalLocationPlb(232, 4242, 51.5007, -0.1246), false);
        char t[260]; std::snprintf(t, sizeof t, "national location PLB: %s · %.5f, %.5f (%s)", m.identity.c_str(), m.lat, m.lon, m.posRes.c_str());
        ok(m.beaconType == "PLB" && m.identity == "national serial 4242" && std::fabs(m.lat - 51.5007) < 0.0012 && std::fabs(m.lon + 0.1246) < 0.0012, t);
    }
    {
        const Message m = parse(tx::testUser(232), false);
        ok(m.testProtocol && m.protocol == "Test user", "test user protocol labelled a test");
    }
}

static void testModem() {
    std::printf("\n── the modem ──\n");
    const Bits msgs[3] = { tx::standardLocationEpirb(232, 123456, 50.7123, -1.3456), tx::aviationUser(232, "G-ABCD"), tx::serialUserLocation(227, 98765, -33.9, 151.2) };
    const double rates[3] = { 25000, 48000, 64000 };
    int good = 0, n = 0;
    std::string fails;
    for (int ri = 0; ri < 3; ++ri)
        for (int k = 0; k < 12; ++k) {
            tx::Opt o; o.rate = rates[ri]; o.seed = (uint32_t)(ri * 100 + k);
            o.offsetHz = -9000 + 1500 * (k % 13);   // ★ ±9 kHz: one dial watches every channel in use o.bitRate = k % 3 == 0 ? 396 : k % 3 == 1 ? 404 : 400;
            o.index = k % 4 == 0 ? 1.0 : k % 4 == 1 ? 1.2 : 1.1; o.invert = k % 5 == 0; o.cn0 = 45;
            const Bits& b = msgs[k % 3];
            const Got g = run(tx::burst(&b, o), o.rate);
            ++n;
            const std::string want = parse(b, false).hexMsg;
            if (g.b.size() == 1 && g.b[0].msg.hexMsg == want && std::fabs(g.b[0].offsetHz - o.offsetHz) < 5) ++good;
            else { char t[120]; std::snprintf(t, sizeof t, " [%.0f kHz k=%d: %zu bursts, %d attempts, %d refused %s %.2f]", rates[ri] / 1e3, k, g.b.size(), g.attempts, g.refused, g.why.c_str(), g.sync); fails += t; }
        }
    ok(good == n, "36 bursts (3 rates, −9…+7.5 kHz, 396/404 bit/s, ±1.0/1.2 rad, either sense) at 45 dB-Hz: every one exactly (" + std::to_string(good) + "/" + std::to_string(n) + ")" + fails);
    for (double cn0 : { 40.0, 37.0, 34.0 }) {
        int okN = 0, wrong = 0;
        for (int k = 0; k < 20; ++k) {
            tx::Opt o; o.cn0 = cn0; o.seed = 1000 + k + (uint32_t)cn0; o.offsetHz = 700;
            const Bits& b = msgs[k % 3];
            const Got g = run(tx::burst(&b, o), o.rate);
            for (const auto& x : g.b) { if (x.msg.hexMsg == parse(b, false).hexMsg) ++okN; else ++wrong; }
        }
        char t[160]; std::snprintf(t, sizeof t, "%.0f dB-Hz: %d of 20 decoded, %d wrong", cn0, okN, wrong);
        ok(wrong == 0 && okN >= (cn0 >= 40 ? 20 : cn0 >= 37 ? 18 : 15), t);
    }
    {
        const Bits b = tx::standardLocationEpirb(232, 123456, 50.7, -1.3, true);
        tx::Opt o; const Got g = run(tx::burst(&b, o), o.rate);
        ok(g.b.size() == 1 && g.b[0].msg.selfTest, "a self-test burst: decoded and labelled");
    }
    {   // a train of three bursts 5 s apart (the first 120 s of an activation)
        const Bits b = tx::aviationUser(232, "G-ABCD");
        std::vector<std::complex<float>> x;
        for (int k = 0; k < 3; ++k) { tx::Opt o; o.seed = 40 + k; o.lead = 0.5; o.tail = 5.0 - 0.5 - 0.44; auto y = tx::burst(&b, o); x.insert(x.end(), y.begin(), y.end()); }
        const Got g = run(x, 48000);
        ok(g.b.size() == 3, "three bursts 5 s apart: three decodes (" + std::to_string(g.b.size()) + ")");
    }
}

static void testFalse() {
    std::printf("\n── false positives ──\n");
    int bursts = 0, attempts = 0;
    for (int s = 0; s < 6; ++s) {
        tx::Opt o; o.lead = 600; o.tail = 0; o.seed = 7000 + s; o.cn0 = 40;   // 10 min of noise at the level a beacon's noise has
        const Got g = run(tx::burst(nullptr, o), o.rate);
        bursts += (int)g.b.size(); attempts += g.attempts;
    }
    ok(bursts == 0, "an hour of noise: no beacon (" + std::to_string(attempts) + " lines looked at)");
    {
        // a steady carrier, a carrier keyed on/off every 0.5 s, and an unmodulated 520 ms carrier every 5 s
        const double fs = 48000; std::vector<std::complex<float>> x((size_t)(120 * fs));
        for (size_t i = 0; i < x.size(); ++i) {
            const double t = i / fs;
            std::complex<float> v = std::polar(1.0f, (float)(2 * M_PI * 1200 * t));
            if (t > 40) v = (std::fmod(t, 1.0) < 0.5) ? std::polar(1.0f, (float)(2 * M_PI * -800 * t)) : std::complex<float>(0, 0);
            if (t > 80) v = (std::fmod(t, 5.0) < 0.52) ? std::polar(1.0f, (float)(2 * M_PI * 300 * t)) : std::complex<float>(0, 0);
            x[i] = v;
        }
        std::mt19937 r(9); std::normal_distribution<float> n(0.f, 1.f);
        const float s = (float)std::sqrt(std::pow(10.0, -45 / 10.0) * fs / 2);
        for (auto& v : x) v += std::complex<float>(s * n(r), s * n(r));
        const Got g = run(x, fs);
        ok(g.b.empty(), "a steady carrier, a keyed carrier, unmodulated 520 ms carriers: no beacon (" + std::to_string(g.attempts) + " looked at, " + std::to_string(g.steady) + " steady)");
    }
    {
        // bursts shaped like a beacon — 160 ms carrier then 360 ms of random biphase PM (random bits, right sync) — whose
        // BCH is wrong: 200 of them
        int got = 0; std::mt19937 r(11);
        for (int k = 0; k < 200; ++k) {
            Bits b{}; for (int i = 25; i <= 144; ++i) b[(size_t)i] = r() & 1;
            for (int i = 1; i <= 15; ++i) b[(size_t)i] = 1;
            setField(b, 16, 24, 0x02F);
            if (bch1().syndrome(b) == 0) continue;
            tx::Opt o; o.seed = 20000 + k;
            const Got g = run(tx::burst(&b, o), o.rate);
            got += (int)g.b.size();
        }
        ok(got <= 1, "200 beacon-shaped bursts of random bits: " + std::to_string(got) + " taken as beacons (the BCH's own rate, refused unless clean)");
    }
}

static void testSimd() {
    std::printf("\n── SIMD = scalar (%s) ──\n", vibe::nav::simdName());
    const Bits b = tx::standardLocationEpirb(232, 123456, 50.7123, -1.3456);
    tx::Opt o; o.cn0 = 38; o.seed = 77;
    const auto x = tx::burst(&b, o);
    const Got a = run(x, o.rate, false), c = run(x, o.rate, true);
    ok(a.checksum == c.checksum && a.b.size() == c.b.size() && (a.b.empty() || a.b[0].msg.hexMsg == c.b[0].msg.hexMsg), "the decimator bit for bit, the same decode");
}

static void testCost() {
    std::printf("\n── cost ──\n");
    tx::Opt o; o.lead = 60; o.tail = 0;
    const Bits b = tx::aviationUser(232, "G-ABCD");
    auto x = tx::burst(nullptr, o);
    for (int k = 0; k < 10; ++k) { tx::Opt q; q.seed = k; auto y = tx::burst(&b, q); x.insert(x.end(), y.begin(), y.end()); }
    Rx rx(o.rate);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < x.size(); i += 960) rx.feed(x.data() + i, (int)std::min<size_t>(960, x.size() - i));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double sec = x.size() / o.rate;
    char t[200]; std::snprintf(t, sizeof t, "%.0f s at 48 kHz with 10 bursts in %.3f s (%.2f %% of one core)", sec, s, s / sec * 100);
    ok(s < sec / 10, t);
}

static int writeClip(const char* out, double rate, double offsetHz) {
    // 30 s: three bursts (long standard location, short aviation, self-test), 9 s apart, cu8
    const Bits m[3] = { tx::standardLocationEpirb(232, 123456, 50.7123, -1.3456), tx::aviationUser(232, "G-ABCD"), tx::standardLocationEpirb(232, 654321, 51.0, -2.0, true) };
    std::vector<std::complex<float>> x;
    for (int k = 0; k < 3; ++k) { tx::Opt o; o.rate = rate; o.offsetHz = offsetHz; o.lead = 2.0; o.tail = 7.0; o.cn0 = 70; o.seed = 90 + k; auto y = tx::burst(&m[k], o); x.insert(x.end(), y.begin(), y.end()); }
    FILE* f = std::fopen(out, "wb"); if (!f) return 1;
    std::vector<uint8_t> b(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        auto q = [](float v) { int s = (int)std::lround(127.5 + v * 30.0); return (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s); };
        b[2 * i] = q(x[i].real()); b[2 * i + 1] = q(x[i].imag());
    }
    std::fwrite(b.data(), 1, b.size(), f); std::fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && !std::strcmp(argv[1], "clip")) return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    std::printf("test-sarsat — COSPAS-SARSAT 406 MHz (%s)\n", vibe::nav::simdName());
    testAnnexB();
    testBch();
    testProtocols();
    testModem();
    testFalse();
    testSimd();
    testCost();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
