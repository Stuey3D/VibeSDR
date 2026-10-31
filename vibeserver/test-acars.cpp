// test-acars.cpp — V12 VHF ACARS (vibe_avi_front.h, vibe_acars.h), docs/v12/AVIATION.md.
//
//  1. The BCS and parity against a REAL block off the air (131.725 MHz, 2026-10-31, 9V-SWK): its BCS leaves zero.
//  2. The parser: mode, registration, label, block id, message number, flight, text; uplink vs downlink; ETB.
//  3. The SIMD kernel (envMix) against a double-precision reference — NEON / SSE2 / scalar each meet the same bar, and
//     the digest line ("digest …") must be identical between a NEON, a VIBE_FORCE_SCALAR and an SSE2 build.
//  4. The whole receiver on synthetic captures at 2.4 and 2.048 MS/s: AM-MSK blocks on the three European channels at once,
//     a carrier offset each, over a range of C/N0 — the block rate per C/N0, with and without repair.
//  5. Noise only: ten minutes of capture, ZERO blocks.
//  6. CPU: real time per capture second.
//  `test-acars --cu8 <file.u8> <rate> <hwCentreHz> [chanHz…]` decodes a REAL capture (ACARS_DEBUG=1: every BCS failure).
#include "vibe_acars.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using vibeacars::cf32;
static uint64_t g_digest = 1469598103934665603ull;
static void dig(const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) g_digest = (g_digest ^ b[i]) * 1099511628211ull; }

// ── a block's bytes, as an aircraft would send them ────────────────────────────────────────────────────────────────
static std::vector<uint8_t> makeBlock(char mode, const std::string& reg7, char ack, const std::string& label, char blk,
                                      const std::string& text, bool etb = false) {
    std::vector<uint8_t> b;
    b.push_back(uint8_t(mode));
    for (char c : reg7) b.push_back(uint8_t(c));
    b.push_back(uint8_t(ack));
    for (char c : label) b.push_back(uint8_t(c));
    b.push_back(uint8_t(blk));
    if (!text.empty()) { b.push_back(vibeacars::STX); for (char c : text) b.push_back(uint8_t(c)); }
    b.push_back(etb ? vibeacars::ETB : vibeacars::ETX);
    for (auto& v : b) v = vibeacars::withParity(v);
    const uint16_t c = vibeacars::bcs(b.data(), b.size());
    b.push_back(uint8_t(c)); b.push_back(uint8_t(c >> 8));
    b.push_back(vibeacars::withParity(vibeacars::DEL));
    return b;
}
/** Bits on the air (LSB first): pre-key ones, '+' '*' SYN SYN SOH, the block. */
static std::vector<int> airBits(const std::vector<uint8_t>& blk, int prekeyBits = 128) {
    std::vector<int> bits(size_t(prekeyBits), 1);
    const uint8_t s[5] = { vibeacars::withParity('+'), vibeacars::withParity('*'), vibeacars::withParity(vibeacars::SYN),
                           vibeacars::withParity(vibeacars::SYN), vibeacars::withParity(vibeacars::SOH) };
    for (uint8_t v : s) for (int i = 0; i < 8; ++i) bits.push_back((v >> i) & 1);
    for (uint8_t v : blk) for (int i = 0; i < 8; ++i) bits.push_back((v >> i) & 1);
    for (int i = 0; i < 8; ++i) bits.push_back(1);   // a trailing character of ones, as the carrier drops
    return bits;
}

/** One AM-MSK burst added into a capture at `fs`: offset `offHz` from the capture centre, carrier amplitude `A`, start
 *  sample `s0`, modulation index `m`. The tone of bit k is 2400 Hz when it equals bit k−1, 1200 Hz when it differs. */
static void addBurst(std::vector<cf32>& cap, double fs, double offHz, double A, size_t s0, const std::vector<int>& bits,
                     double m = 0.6, double ph0 = 0.3) {
    const double sp = fs / 2400.0;
    double ph = ph0, cph = 1.1;
    int prev = 1;
    const size_t n = size_t(bits.size() * sp);
    for (size_t i = 0; i < n && s0 + i < cap.size(); ++i) {
        const size_t k = size_t(double(i) / sp);
        const int b = bits[std::min(k, bits.size() - 1)];
        const int pb = k ? bits[k - 1] : prev;
        const double f = (b == pb) ? 2400.0 : 1200.0;
        ph += 2 * M_PI * f / fs;
        const double env = A * (1.0 + m * std::cos(ph));
        cph += 2 * M_PI * offHz / fs;
        cap[s0 + i] += cf32(float(env * std::cos(cph)), float(env * std::sin(cph)));
    }
}

struct Res { int good = 0, rep = 0, wrong = 0; };

/** A synthetic run: `nb` blocks per channel on the three EU channels, carrier C/N0 `cn0` dB-Hz. */
static Res synth(double fs, double cn0, int nb, uint32_t seed, bool repair = true, bool print = false) {
    const double hw = 131.675e6;
    const std::vector<double> ch = { 131.525e6, 131.725e6, 131.825e6 };
    const double noiseSigma = 0.02;                 // per real component
    const double N0 = 2 * noiseSigma * noiseSigma / fs;   // complex noise density (per Hz)
    const double A = std::sqrt(std::pow(10.0, cn0 / 10) * N0);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, float(noiseSigma));
    const double blockS = 0.75;
    const size_t total = size_t(fs * (0.2 + blockS * nb));
    std::vector<cf32> cap(total);
    for (auto& v : cap) v = cf32(g(rng), g(rng));
    std::vector<std::vector<uint8_t>> sent;
    const char* regs[] = { ".G-EUPT", ".9V-SWK", "..A7BAP", ".N476MC", ".EI-DEO", ".D-AIZZ" };
    for (int i = 0; i < nb; ++i)
        for (size_t c = 0; c < ch.size(); ++c) {
            char txt[160];
            std::snprintf(txt, sizeof txt, "M%02dABA%04d/POS N51%03d W001%03d,%05d,%03d TEST BLOCK %d CH %zu", i % 100,
                          int(rng() % 10000), int(rng() % 1000), int(rng() % 1000), int(rng() % 40000), int(rng() % 999), i, c);
            auto blk = makeBlock('2', regs[(i + c) % 6], 0x15, "H1", char('0' + i % 10), txt);
            sent.push_back(blk);
            const double off = (ch[c] - hw) + (double(int(rng() % 801)) - 400.0);
            addBurst(cap, fs, off, A, size_t(fs * (0.1 + blockS * i + 0.05 * c)), airBits(blk), 0.5 + 0.4 * (rng() % 100) / 100.0,
                     (rng() % 1000) * 0.00628);
        }
    vibeacars::Receiver rx(fs, hw, ch);
    Res r;
    std::vector<std::string> got;
    vibeacars::tuning().repair = repair;
    const size_t step = 65536;
    for (size_t o = 0; o < total; o += step)
        rx.feed(cap.data() + o, std::min(step, total - o), [&](const vibeacars::Block& b) {
            std::string s(b.raw.begin(), b.raw.end());
            bool match = false;
            for (auto& v : sent) { std::string t; for (size_t j = 0; j + 3 < v.size(); ++j) t += char(v[j] & 0x7F); if (t == s) { match = true; break; } }
            if (match) { ++r.good; if (b.repaired) ++r.rep; } else ++r.wrong;
            dig(s.data(), s.size()); dig(&b.repaired, sizeof b.repaired);
            if (print) std::printf("      %s %s %s\n", b.reg.c_str(), b.flight.c_str(), b.text.c_str());
        });
    vibeacars::tuning().repair = true;
    return r;
}

static int cu8(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "usage: test-acars --cu8 file rate hwCentreHz [chanHz...]\n"); return 2; }
    const double rate = std::atof(argv[3]), hw = std::atof(argv[4]);
    std::vector<double> ch;
    for (int i = 5; i < argc; ++i) ch.push_back(std::atof(argv[i]));
    if (ch.empty()) ch = { 131.525e6, 131.725e6, 131.825e6 };
    FILE* f = std::fopen(argv[2], "rb");
    if (!f) { std::perror(argv[2]); return 2; }
    vibeacars::Receiver rx(rate, hw, ch);
    std::vector<uint8_t> buf(1 << 18);
    std::vector<cf32> z(1 << 17);
    int n = 0, rep = 0;
    size_t samples = 0;
    std::vector<std::vector<double>> goodAt(ch.size());
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const size_t r = std::fread(buf.data(), 2, 1 << 17, f);
        if (!r) break;
        vibeavi::u8ToCf(buf.data(), r, z.data());
        samples += r;
        rx.feed(z.data(), r, [&](const vibeacars::Block& b) {
            ++n; rep += b.repaired > 0;
            goodAt[size_t(b.channel)].push_back(b.t);
            std::string t = b.text;
            for (auto& c : t) if (c == '\r' || c == '\n') c = ' ';
            std::printf("%7.2f %.3f %s%s rep%d lvl %5.1f snr %4.1f mode %c %-7s lbl %-2s blk %c msg %-4s flt %-6s | %s\n", b.t,
                        b.hz / 1e6, b.downlink ? "DN" : "UP", b.more ? "+" : " ", b.repaired, b.levelDb, b.snrDb, b.mode,
                        b.reg.c_str(), b.label.c_str(), b.blockId, b.msgNo.c_str(), b.flight.c_str(), t.c_str());
        });
    }
    std::fclose(f);
    const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    for (size_t c = 0; c < rx.channels(); ++c) {
        const auto& s = rx.demod(c).stats();
        std::printf("ch %.3f: syncs %llu, blocks %llu (repaired %llu), BCS failures %llu, clock duplicates %llu\n", ch[c] / 1e6,
                    (unsigned long long)s.syncs, (unsigned long long)s.good, (unsigned long long)s.repaired,
                    (unsigned long long)s.bcsFail, (unsigned long long)s.dupes);
    }
    // ★ bursts that reached a sync word but gave no block (no clock got it through the BCS)
    int lost = 0;
    for (size_t c = 0; c < rx.channels(); ++c) {
        double last = -9;
        for (double t : rx.demod(c).failTimes()) {
            if (t - last < 0.25) { last = t; continue; }
            last = t;
            bool ok = false;
            for (double g : goodAt[c]) if (std::fabs(g - t) < 1.2) ok = true;
            if (!ok) { ++lost; if (std::getenv("ACARS_LOST")) std::printf("   lost: ch %.3f t=%.2f\n", ch[c] / 1e6, t); }
        }
    }
    std::printf("bursts synced but not decoded: %d\n", lost);
    std::printf("blocks %d (repaired %d) in %.0f s of capture; %.2f s here = %.2f %% of a core\n", n, rep, samples / rate, el,
                100.0 * el / (samples / rate));
    return 0;
}

int main(int argc, char** argv) {
    // ★ experiment switches: ACARS_TUNE="two=0 W=8 ph=8 rep=0"
    if (const char* t = std::getenv("ACARS_TUNE")) {
        auto& T = vibeacars::tuning();
        std::string s(t);
        auto val = [&](const char* k, int& v) { const size_t p = s.find(k); if (p != std::string::npos) v = std::atoi(s.c_str() + p + std::strlen(k)); };
        int two = T.twoSided, rep = T.repair, coh = T.coherentAm;
        val("two=", two); val("W=", T.W); val("ph=", T.phases); val("rep=", rep); val("coh=", coh); val("A=", T.cohA);
        T.twoSided = two; T.repair = rep; T.coherentAm = coh;
    }
    if (argc > 1 && !std::strcmp(argv[1], "--cu8")) return cu8(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "--sens")) {   // the sensitivity table alone, 30 blocks a point
        for (double fs : { 2.4e6, 2.048e6 }) for (double cn0 = 42; cn0 <= 51; cn0 += 1.5) {
            Res r = synth(fs, cn0, 10, uint32_t(cn0 * 10 + fs / 1e5));
            std::printf("%.3f MS/s %4.1f dB-Hz: %2d of 30 (%d repaired) wrong %d\n", fs / 1e6, cn0, r.good, r.rep, r.wrong);
        }
        return 0;
    }
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");

    std::printf("1. the BCS and parity on a real block (9V-SWK, 131.725 MHz)\n");
    {
        // "2.9V-SWKM_<DEL>5<STX>S23ASQ0321<ETX>" + BCS 37 64, received 2026-10-31 22:08:56 UTC
        const char* body = "2.9V-SWKM_\x7f" "5\x02S23ASQ0321\x03";
        std::vector<uint8_t> b;
        for (const char* p = body; *p; ++p) b.push_back(vibeacars::withParity(uint8_t(*p)));
        b.push_back(0x37); b.push_back(0x64);
        CHECK(vibeacars::bcs(b.data(), b.size()) == 0, "real block's BCS residue %04x", vibeacars::bcs(b.data(), b.size()));
        b[5] ^= 0x04;
        CHECK(vibeacars::bcs(b.data(), b.size()) != 0, "a flipped bit must fail");
        CHECK(vibeacars::withParity('+') == 0xAB && vibeacars::withParity('*') == 0x2A && vibeacars::withParity(0x16) == 0x16, "parity");
    }

    std::printf("2. the parser\n");
    {
        auto blk = makeBlock('2', ".9V-SWK", 0x15, "H1", '7', "D21FSQ0321#DFB5 -02 876");
        std::vector<uint8_t> b; for (size_t i = 0; i + 3 < blk.size(); ++i) b.push_back(blk[i] & 0x7F);
        vibeacars::Block o;
        CHECK(vibeacars::parse(b, o), "parse");
        CHECK(o.reg == "9V-SWK" && o.label == "H1" && o.blockId == '7' && o.downlink && o.msgNo == "D21F" && o.flight == "SQ0321" &&
              o.text == "#DFB5 -02 876" && !o.more, "fields: '%s' '%s' '%c' '%s' '%s' '%s'", o.reg.c_str(), o.label.c_str(), o.blockId,
              o.msgNo.c_str(), o.flight.c_str(), o.text.c_str());
        auto up = makeBlock('2', ".G-EUPT", 0x15, "SQ", 'A', "02XAEGLLL51286W00038", true);
        b.clear(); for (size_t i = 0; i + 3 < up.size(); ++i) b.push_back(up[i] & 0x7F);
        CHECK(vibeacars::parse(b, o) && !o.downlink && o.more && o.text == "02XAEGLLL51286W00038" && o.flight.empty(), "uplink");
        auto noText = makeBlock('2', ".A7-BAP", 'E', "_\x7f", '3', "");
        b.clear(); for (size_t i = 0; i + 3 < noText.size(); ++i) b.push_back(noText[i] & 0x7F);
        CHECK(vibeacars::parse(b, o) && o.text.empty() && o.reg == "A7-BAP", "no text");
    }

    std::printf("3. the SIMD kernel against a double-precision reference\n");
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> u(-1, 1);
        for (int n : { 1, 3, 4, 5, 17, 64, 333, 1000 }) {
            std::vector<cf32> z((size_t)n), o((size_t)n);
            std::vector<float> lr((size_t)n), li((size_t)n);
            for (int i = 0; i < n; ++i) { z[size_t(i)] = cf32(u(rng), u(rng)); lr[size_t(i)] = u(rng); li[size_t(i)] = u(rng); }
            float sum = 0;
            vibeacars::envMix(z.data(), n, 0.3f, lr.data(), li.data(), o.data(), &sum);
            double err = 0, ssum = 0;
            for (int i = 0; i < n; ++i) {
                const double e = std::hypot(double(z[size_t(i)].real()), double(z[size_t(i)].imag()));
                ssum += e;
                err = std::max(err, std::fabs((e - 0.3) * lr[size_t(i)] - o[size_t(i)].real()));
                err = std::max(err, std::fabs((e - 0.3) * li[size_t(i)] - o[size_t(i)].imag()));
            }
            CHECK(err < 2e-6 && std::fabs(ssum - sum) < 1e-4 * n, "envMix n=%d err %.2g sum %.6f vs %.6f", n, err, sum, ssum);
        }
    }

    std::printf("4. the receiver on synthetic captures (3 channels at once; carrier ±400 Hz; m 0.5–0.9)\n");
    {
        const double rates[2] = { 2.4e6, 2.048e6 };
        for (double fs : rates) {
            std::printf("   %.3f MS/s   C/N0 dB-Hz : blocks decoded of %d (repaired) | no repair\n", fs / 1e6, quick ? 9 : 30);
            const int nb = quick ? 3 : 10;
            for (double cn0 : { 42.0, 45.0, 48.0, 51.0, 54.0, 60.0 }) {
                Res r = synth(fs, cn0, nb, uint32_t(cn0 * 10 + fs / 1e5));
                Res r0 = synth(fs, cn0, nb, uint32_t(cn0 * 10 + fs / 1e5), false);
                std::printf("              %4.0f        : %3d (%2d)  wrong %d | %3d\n", cn0, r.good, r.rep, r.wrong, r0.good);
                CHECK(r.wrong == 0, "a wrong block at %.0f dB-Hz", cn0);
                if (cn0 >= 54) CHECK(r.good == 3 * nb, "all blocks at %.0f dB-Hz (%d of %d)", cn0, r.good, 3 * nb);
            }
        }
    }

    std::printf("5. noise only: %s of capture, zero blocks\n", quick ? "1 min" : "10 min");
    {
        const double fs = 2.4e6;
        vibeacars::Receiver rx(fs, 131.675e6, { 131.525e6, 131.725e6, 131.825e6 });
        std::mt19937 rng(99);
        std::normal_distribution<float> g(0.f, 0.05f);
        std::vector<cf32> blk(1 << 16);
        int got = 0;
        const size_t total = size_t(fs * (quick ? 60 : 600));
        for (size_t o = 0; o < total; o += blk.size()) {
            for (auto& v : blk) v = cf32(g(rng), g(rng));
            rx.feed(blk.data(), blk.size(), [&](const vibeacars::Block&) { ++got; });
        }
        uint64_t syncs = 0; for (size_t c = 0; c < 3; ++c) syncs += rx.demod(c).stats().syncs;
        std::printf("   %d blocks (sync words matched: %llu)\n", got, (unsigned long long)syncs);
        CHECK(got == 0, "noise produced %d blocks", got);
    }

    std::printf("6. CPU: three channels at 2.4 MS/s\n");
    {
        const double fs = 2.4e6;
        vibeacars::Receiver rx(fs, 131.675e6, { 131.525e6, 131.725e6, 131.825e6 });
        std::vector<cf32> blk(1 << 16);
        std::mt19937 rng(5); std::normal_distribution<float> g(0.f, 0.05f);
        for (auto& v : blk) v = cf32(g(rng), g(rng));
        const int reps = int(fs * 20 / blk.size());
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < reps; ++i) rx.feed(blk.data(), blk.size(), [&](const vibeacars::Block&) {});
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   %.3f s for 20 s of capture = %.2f %% of a core here (front end: %d half-band stages, FFT %d, %d-bin channels at %.0f S/s)\n",
                    el, 100 * el / 20, rx.front().stages(), rx.front().fftSize(), rx.front().chanBins(), rx.front().chanRate());
    }
    std::printf("digest %016llx\n", (unsigned long long)g_digest);
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
