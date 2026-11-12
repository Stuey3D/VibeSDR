// test-ism.cpp — V12 ISM decoder (vibe_ism*.h): every protocol on its published worked examples where the source
// gives one, then end to end — bursts ENCODED FROM THE SAME PUBLISHED SPECS, modulated into noisy IQ at the rates the
// radios run, through the real front end (detector → channel → slicer) and the decoders. Then the front end's
// robustness (drift ±150 kHz, SNR, several transmitters at once, pure noise = nothing), the PRIVACY rule on the JSON,
// and a CPU row. docs/v12/ISM-DECODER.md §Tests.
// ★ Silent: no audio anywhere. ★ The encoders below are written from the specs, independently of the decoders' code
//   paths (bits → runs), so a decoder that misreads a spec fails here.
#include "vibe_ism_service.h"
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace vibeism;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)
static bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ── a run builder: +mark / −space µs, merging like signs ─────────────────────────────────────────────────────────────
struct Tx {
    Runs r;
    void add(double us) { const int d = int(std::lround(us)); if (!d) return; if (!r.empty() && ((r.back() > 0) == (d > 0))) r.back() += d; else r.push_back(d); }
    void mark(double us) { add(us); }
    void space(double us) { add(-us); }
};
static std::vector<uint8_t> hexBytes(const char* h) {
    std::vector<uint8_t> o; for (size_t i = 0; h[i] && h[i + 1]; i += 2) { while (h[i] == ' ') ++i; unsigned v; std::sscanf(h + i, "%2x", &v); o.push_back(uint8_t(v)); }
    return o;
}
static Bits bytesToBits(const std::vector<uint8_t>& b) { Bits o; for (uint8_t x : b) for (int i = 7; i >= 0; --i) o.push_back((x >> i) & 1); return o; }

// ── IQ synthesis ─────────────────────────────────────────────────────────────────────────────────────────────────────
struct Sig { Runs runs; bool fsk = false; double offHz = 0, amp = 0.3, devHz = 35e3, startS = 0.05; };
static std::vector<cf> synth(const std::vector<Sig>& sigs, double rate, double lenS, double noise, uint32_t seed) {
    std::vector<cf> x(size_t(rate * lenS));
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0, float(noise));
    for (auto& v : x) v = cf(g(rng), g(rng));
    for (const Sig& s : sigs) {
        double t = s.startS, ph = 0;
        for (int d : s.runs) {
            const size_t a = size_t(t * rate);
            t += std::abs(d) * 1e-6;
            const size_t e = std::min(x.size(), size_t(t * rate));
            const double f = s.fsk ? s.offHz + (d > 0 ? s.devHz : -s.devHz) : s.offHz;
            for (size_t i = a; i < e; ++i) {
                ph += 2 * M_PI * f / rate;
                if (s.fsk || d > 0) x[i] += cf(float(s.amp * std::cos(ph)), float(s.amp * std::sin(ph)));
            }
        }
    }
    return x;
}
static std::vector<Reading> runFront(const std::vector<cf>& x, double rate, std::vector<Burst>* bursts = nullptr, double chanRate = 0) {
    FrontEnd fe(rate);
    fe.setDcGuardHz(15e3);
    if (chanRate > 0) { fe.setChanRate(chanRate); fe.setKeepBaseband(true); }   // ★ the 868 band's wider channel (V12 wM-Bus)
    std::vector<Reading> out;
    auto on = [&](const Burst& b) { if (bursts) bursts->push_back(b); Service::decodeBurst(b, 433.92e6, out); };
    for (size_t i = 0; i < x.size(); i += 16384) fe.feed(x.data() + i, std::min<size_t>(16384, x.size() - i), on);
    fe.flush(on);
    return out;
}

// ── ENCODERS (from the published specs) ──────────────────────────────────────────────────────────────────────────────
// Oregon: OOK Manchester, 1024 Hz, bit 1 = on then off; pulses trimmed at their end.
static void manchesterTx(Tx& tx, const Bits& bits, double half, double trim) {
    std::vector<uint8_t> lv;
    for (uint8_t b : bits) { lv.push_back(b ? 1 : 0); lv.push_back(b ? 0 : 1); }
    for (size_t i = 0; i < lv.size();) {
        size_t j = i; while (j < lv.size() && lv[j] == lv[i]) ++j;
        const double d = (j - i) * half;
        if (lv[i]) { tx.mark(d - trim); tx.space(trim); } else tx.space(d);
        i = j;
    }
}
static std::vector<int> oregonNibbles(const char* hex) { std::vector<int> n; for (const char* p = hex; *p; ++p) { const char c = *p; n.push_back(c <= '9' ? c - '0' : c - 'A' + 10); } return n; }
static Bits oregonBits(const std::vector<int>& nb, int preOnes) {
    Bits b(size_t(preOnes), 1);
    b.push_back(0); b.push_back(1); b.push_back(0); b.push_back(1);
    for (int n : nb) for (int i = 0; i < 4; ++i) b.push_back((n >> i) & 1);
    for (int i = 0; i < 8; ++i) b.push_back(0);   // post-amble
    return b;
}
static std::vector<int> withChecksum(std::vector<int> nb, int len) {
    nb.resize(size_t(len - 2), 0);
    int sum = 0; for (int x : nb) sum += x;
    nb.push_back(sum & 0xF); nb.push_back((sum >> 4) & 0xF);
    return nb;
}
static Runs oregonV3(const std::vector<int>& nb) { Tx tx; manchesterTx(tx, oregonBits(nb, 24), 488.28, 138); return tx.r; }
static Runs oregonV21(const std::vector<int>& nb) {
    const Bits o = oregonBits(nb, 16);
    Bits t; for (uint8_t b : o) { t.push_back(b ^ 1); t.push_back(b); }
    Tx tx; manchesterTx(tx, t, 488.28, 96); tx.space(10900); manchesterTx(tx, t, 488.28, 96);
    return tx.r;
}
static void pwmBits(Tx& tx, const Bits& b, double one, double zero, double gap) { for (uint8_t x : b) { tx.mark(x ? one : zero); tx.space(gap); } }
static Runs wh1080(const char* hex) { Tx tx; Bits b(8, 1); Bits d = bytesToBits(hexBytes(hex)); b.insert(b.end(), d.begin(), d.end()); pwmBits(tx, b, 500, 1500, 1000); return tx.r; }
static Runs wh2(uint16_t id, double t, int h) {
    const int raw = (t < 0 ? 0x800 : 0) | int(std::lround(std::fabs(t) * 10));
    std::vector<uint8_t> by = { uint8_t(id >> 4), uint8_t(((id & 0xF) << 4) | (raw >> 8)), uint8_t(raw & 0xFF), uint8_t(h) };
    by.push_back(crc8(by.data(), 4, 0x31, 0));
    Tx tx; Bits b(8, 1); Bits d = bytesToBits(by); b.insert(b.end(), d.begin(), d.end()); pwmBits(tx, b, 500, 1500, 1000); return tx.r;
}
static Runs nexusTx(int id, int ch, double t, int h, bool batt) {
    int traw = int(std::lround(t * 10)) & 0xFFF;
    Bits w;
    auto put = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) w.push_back((v >> i) & 1); };
    put(uint32_t(id), 8); put(batt ? 1 : 0, 1); put(0, 1); put(uint32_t(ch - 1), 2); put(uint32_t(traw), 12); put(0xF, 4); put(uint32_t(h), 8);
    Tx tx;
    for (int rep = 0; rep < 12; ++rep) { tx.mark(500); tx.space(4000); for (uint8_t b : w) { tx.mark(500); tx.space(b ? 2000 : 1000); } }
    tx.mark(500);
    return tx.r;
}
static Runs nrzFsk(const Bits& b, double bitUs) { Tx tx; for (uint8_t x : b) { if (x) tx.mark(bitUs); else tx.space(bitUs); } return tx.r; }
static Runs lacrosseTx(int id, double t, int h, bool weak) {
    const int v = int(std::lround((t + 40.0) * 10));
    std::vector<uint8_t> by = { uint8_t(0x90 | (id >> 2)), uint8_t(((id & 3) << 6) | (v / 100)), uint8_t(((v / 10) % 10) << 4 | (v % 10)),
                                uint8_t((weak ? 0x80 : 0) | h) };
    by.push_back(crc8(by.data(), 4, 0x31, 0));
    std::vector<uint8_t> f = {0xAA, 0xAA, 0xAA, 0x2D, 0xD4}; f.insert(f.end(), by.begin(), by.end()); f.push_back(0);
    return nrzFsk(bytesToBits(f), 1e6 / 17241.0);
}
static Runs acuritePwm(const std::vector<uint8_t>& by) {
    Tx tx;
    for (int rep = 0; rep < 3; ++rep) {
        tx.space(2250);
        for (int i = 0; i < 4; ++i) { tx.mark(600); tx.space(600); }
        for (uint8_t x : bytesToBits(by)) { if (x) { tx.mark(400); tx.space(225); } else { tx.mark(250); tx.space(400); } }
    }
    return tx.r;
}
static Runs acurite782(const char* hex) {
    Tx tx;
    for (int rep = 0; rep < 8; ++rep) { tx.mark(540); tx.space(9000); for (uint8_t x : bytesToBits(hexBytes(hex))) { tx.mark(540); tx.space(x ? 4100 : 2100); } }
    tx.mark(540);
    return tx.r;
}
static Runs currentCostTx(const char* hex8) {
    Bits b = bytesToBits({0x55, 0x55, 0x55, 0x2D, 0xD4});
    for (uint8_t x : bytesToBits(hexBytes(hex8))) { b.push_back(x ? 1 : 0); b.push_back(x ? 0 : 1); }   // 10 = 1, 01 = 0
    for (int i = 0; i < 8; ++i) b.push_back(i & 1);
    return nrzFsk(b, 1e6 / 3918.0);
}
static Runs keeloqTx(uint32_t hop, uint32_t serial, unsigned buttons /*S0..S3 bits 0..3*/, bool vlow, double te = 400, int words = 2) {
    Bits bits;   // LSb first: 32 hop, 28 serial, S3 S0 S1 S2, VLOW, RPT
    for (int i = 0; i < 32; ++i) bits.push_back((hop >> i) & 1);
    for (int i = 0; i < 28; ++i) bits.push_back((serial >> i) & 1);
    bits.push_back((buttons >> 3) & 1); bits.push_back(buttons & 1); bits.push_back((buttons >> 1) & 1); bits.push_back((buttons >> 2) & 1);
    bits.push_back(vlow); bits.push_back(0);
    Tx tx;
    for (int w = 0; w < words; ++w) {
        for (int p = 0; p < 12; ++p) { tx.mark(te); tx.space(p == 11 ? 10 * te : te); }
        for (size_t i = 0; i < bits.size(); ++i) {
            if (bits[i]) { tx.mark(te); tx.space(2 * te); } else { tx.mark(2 * te); tx.space(te); }
        }
        tx.space(39 * te);
    }
    return tx.r;
}
static Runs fixedCodeTx(uint32_t code24, double T = 350, int reps = 6) {
    Tx tx;
    for (int r = 0; r < reps; ++r) {
        tx.mark(T); tx.space(31 * T);
        for (int i = 23; i >= 0; --i) { if ((code24 >> i) & 1) { tx.mark(3 * T); tx.space(T); } else { tx.mark(T); tx.space(3 * T); } }
    }
    tx.mark(T);
    return tx.r;
}
static Runs tpmsBTx(uint32_t id, int pRaw, int tC) {
    std::vector<uint8_t> by = { 0x5A, uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id), uint8_t(pRaw), uint8_t(tC + 50) };
    by.push_back(crc8(by.data() + 1, 6, 0x07, 0));
    Bits b; for (int i = 0; i < 16; ++i) b.push_back(1);      // a run-in we chose (the paper gives none)
    b.push_back(1); b.push_back(0); b.push_back(1);           // 3 LEARN bits
    Bits d = bytesToBits(by); b.insert(b.end(), d.begin(), d.end());
    Tx tx; manchesterTx(tx, b, 120, 0); tx.space(500);
    Runs one = tx.r;
    Tx all; for (int k = 0; k < 2; ++k) { for (int x : one) all.add(x); all.space(3000); }
    return all.r;
}
static Runs tpmsATx(uint32_t id, int p, int tC, double half = 52) {
    std::vector<uint8_t> by = { uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id), uint8_t(p), uint8_t(tC + 40), 0x12, 0x34 };
    const uint16_t c = crc16(by.data(), 8, 0x1021, 0xFFFF); by.push_back(uint8_t(c >> 8)); by.push_back(uint8_t(c));
    Bits bits = {1, 0}; Bits d = bytesToBits(by); bits.insert(bits.end(), d.begin(), d.end());
    // differential Manchester (a change at a bit's start = 0, a change in every middle), after 8 alternating halves
    std::vector<uint8_t> lv;
    for (int i = 0; i < 8; ++i) lv.push_back(uint8_t(i & 1));
    uint8_t cur = lv.back();
    for (uint8_t b : bits) {
        uint8_t first = b ? cur : uint8_t(cur ^ 1);
        lv.push_back(first); lv.push_back(uint8_t(first ^ 1)); cur = uint8_t(first ^ 1);
    }
    Tx tx; for (uint8_t l : lv) { if (l) tx.mark(half); else tx.space(half); }
    return tx.r;
}

static const Reading* findModel(const std::vector<Reading>& v, const std::string& modelPart) {
    for (const auto& r : v) if (r.model.find(modelPart) != std::string::npos) return &r;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ --write-clip <out.u8> <radio centre Hz> <rate>: a few seconds of rtl_tcp u8 IQ holding an Oregon THGR810, a WH1080,
 *  a TPMS (twice, so it confirms) and a fixed-code remote at known 433 MHz frequencies — the replay for
 *  scripts/test-server-ism.mjs (fake-rtl-tcp.mjs --ism). */
static int writeClip(const char* out, double centre, double rate) {
    std::vector<Sig> s;
    Sig a{oregonV3(withChecksum(oregonNibbles("F824" "2" "6B" "0" "481" "0" "26" "0"), 17))}; a.offHz = 433.92e6 - centre; a.startS = 0.3; s.push_back(a);
    Sig b{wh1080("a1820d5a0306004e08d3")}; b.offHz = 433.85e6 - centre; b.startS = 0.9; s.push_back(b);
    Sig c{tpmsBTx(0x11223344, 88, 18)}; c.offHz = 433.95e6 - centre; c.startS = 1.5; s.push_back(c);
    Sig c2 = c; c2.startS = 2.1; s.push_back(c2);
    Sig d{fixedCodeTx(0xA5C3E8)}; d.offHz = 434.00e6 - centre; d.startS = 2.6; s.push_back(d);
    const auto x = synth(s, rate, 3.4, 0.02, 77);
    FILE* f = std::fopen(out, "wb");
    if (!f) return 1;
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i]     = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].real() * 127.5f + 127.5f))));
        u[2 * i + 1] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].imag() * 127.5f + 127.5f))));
    }
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    printf("wrote %zu samples (%.1f s) centred on %.3f MHz\n", x.size(), x.size() / rate, centre / 1e6);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ★ V12 2026-11-11: THE HOME FAMILIES (vibe_ism_meters.h, vibe_ism_security.h, vibe_ism_remotes.h) — each ENCODED FROM
//  ITS FRAME FACTS (as cited above each decoder), modulated into noisy IQ, through the real front end and the decoders.
//  The encoders build bits from the facts; they share no code path with the decoders but the CRC routines.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static std::string factOf(const Reading& r, const char* k) { for (const auto& kv : r.facts) if (kv.first == k) return kv.second; return ""; }
static Bits hb(uint64_t v, int n) { Bits o; for (int i = n - 1; i >= 0; --i) o.push_back((v >> i) & 1); return o; }
static void cat(Bits& a, const Bits& b) { a.insert(a.end(), b.begin(), b.end()); }
static Bits manch01(const Bits& d) { Bits o; for (uint8_t x : d) { o.push_back(x ? 0 : 1); o.push_back(x ? 1 : 0); } return o; }   // 01 = 1
struct HomeCase { const char* name; Sig s; const char* model; const char* fact; const char* want; bool secret; };
static void homeRun(std::vector<HomeCase>& cs, double rate = 2.4e6) {
    for (auto& c : cs) {
        if (c.s.offHz == 0) c.s.offHz = 120e3;
        std::vector<Burst> bs;
        const auto got = runFront(synth({c.s}, rate, 1.2, 0.02, 7), rate, &bs);
        const Reading* r = findModel(got, c.model);
        if (!r && std::getenv("ISM_HOME_DIAG"))
            for (const auto& b : bs) { printf("     burst %.3f s %+.0f Hz dev %.0f ook %zu fsk %zu:", b.tSec, b.offHz + b.carrierHz, b.fskDevHz, b.ook.size(), b.fsk.size());
                                       for (size_t i = 0; i < b.fsk.size() && i < 80; ++i) printf(" %d", b.fsk[i]); printf("\n");
                                       if (const char* us = std::getenv("ISM_HOME_DIAG_US")) for (const Bits& row : kit::nrzRowsAny(b, std::atof(us), 160, 16)) { printf("      "); for (auto x : row) printf("%d", x); printf("\n"); } }
        CHECK(r != nullptr, "%s: not decoded (%zu readings%s%s)", c.name, got.size(), got.empty() ? "" : ", first: ", got.empty() ? "" : got[0].model.c_str());
        if (!r) continue;
        if (c.fact) CHECK(factOf(*r, c.fact) == c.want, "%s: %s = %s, want %s", c.name, c.fact, factOf(*r, c.fact).c_str(), c.want);
        CHECK(r->secret == c.secret && (!c.secret || r->pubId.empty()), "%s: secret %d, pubId '%s'", c.name, r->secret, r->pubId.c_str());
        printf("   %-26s → %s\n", c.name, readable(*r, 1).c_str());
    }
}
/** NRZ chips → runs with the edges at their true times (Tx rounds each chip to a whole µs: 57 chips of 20.3 µs would
 *  come out a chip short). */
static Runs fskNrz(const Bits& chips, double us) {
    Runs r; long prev = 0;
    for (size_t i = 0; i < chips.size(); ++i) {
        if (i + 1 < chips.size() && chips[i + 1] == chips[i]) continue;
        const long edge = std::lround(double(i + 1) * us);
        const int d = int(edge - prev); prev = edge;
        r.push_back(chips[i] ? d : -d);
    }
    return r;
}
static void testHome() {
    printf(" home families (meters, alarm sensors, remotes)\n");
    std::vector<HomeCase> cs;
    // Watchman Sonic: 1 ms chips, 111000, 64 Manchester bits (01 = 1), the last chip twice more
    {
        std::vector<uint8_t> by = {0x28, 0xC7, 0x4E, 0x0F, 0x80, uint8_t(17 << 2 | 0), 92};
        by.push_back(kit::crc8r(by.data(), 7, 0x31, 0));
        Bits c = hb(0x55, 8); cat(c, hb(0x38, 6)); cat(c, manch01(bytesToBits(by)));
        const uint8_t last = by[7] & 1; c.push_back(last); c.push_back(last); cat(c, hb(last ? 0 : 0xFF, 8));
        Sig s{fskNrz(c, 1000)}; s.fsk = true; s.devHz = 30e3;
        cs.push_back({"oil: Watchman Sonic", s, "Watchman Sonic", "depth_cm", "92", false});
    }
    // Apollo Smart: 500 µs chips, …5558, 64 Manchester bits, CRC-8 reflected over all 8 = 0
    {
        std::vector<uint8_t> by = {0x12, 0x5D, 0x82, 0xAB, 0x46, 0xB0, 48};
        by.push_back(kit::crc8r(by.data(), 7, 0x31, 0));
        Bits c = hb(0x5555, 16); cat(c, hb(0x5558, 16)); cat(c, manch01(bytesToBits(by))); cat(c, hb(0xF0, 8));
        Sig s{fskNrz(c, 500)}; s.fsk = true; s.devHz = 30e3;
        cs.push_back({"oil: Apollo Smart", s, "Apollo Ultrasonic Smart", "depth_cm", "48", false});
    }
    // Apollo Standard: 500 µs chips, 0x555D (last two chips = the first pair), 32 Manchester bits, ends in a violation
    {
        std::vector<uint8_t> by = {0x60, 0xFB, 0x00, 135};
        Bits m = manch01(bytesToBits(by));
        Bits c = hb(0x5555, 16); cat(c, hb((by[0] & 0x80 ? 0x555D : 0x5562) >> 2, 14)); cat(c, m);   // its last 2 chips = the 1st pair
        const uint8_t l = m.back(); for (int i = 0; i < 4; ++i) c.push_back(l); cat(c, hb(l ? 0 : 0xFF, 8));
        Tx tx; for (int rep = 0; rep < 2; ++rep) { for (uint8_t x : c) { if (x) tx.mark(500); else tx.space(500); } tx.space(20000); }
        Sig s{tx.r}; s.fsk = true; s.devHz = 30e3;
        cs.push_back({"oil: Apollo Standard", s, "Apollo Ultrasonic / Tekelek", "depth_cm", "135", false});
    }
    // Watchman Sonic Advanced: 500 µs NRZ, AA… 2D D4, length 0E, model 0401, serial, … CRC-16 0x8005 init 0
    {
        std::vector<uint8_t> by = {0x0E, 0x04, 0x01, 0x00, 0x25, 0x9E, 0x98, 0x4F + 0x10, 0xAA, 0x60, 0x28, 0x01, 0x05, 0x03, 0x00};
        const uint16_t c16 = crc16(by.data(), 15, 0x8005, 0); by.push_back(uint8_t(c16 >> 8)); by.push_back(uint8_t(c16));
        Bits c = hb(0xAAAAAAAAAAull, 40); cat(c, hb(0x2DD4, 16)); cat(c, bytesToBits(by)); cat(c, hb(0, 8));
        Sig s{fskNrz(c, 500)}; s.fsk = true; s.devHz = 30e3;
        cs.push_back({"oil: Watchman Advanced", s, "Watchman Sonic Advanced", "depth_cm", "40", false});
    }
    // emonTx: JeeLib RF12 — AA AA AA 2D, group D2, header (node 8), length 26, 13 LE words, CRC-16/IBM LE, AA
    {
        std::vector<uint8_t> by = {0xD2, 0x08, 26};
        const int w[13] = {7, 0, 0, 0, 23665, 3000, 3000, 3000, 3000, 3000, 3000, 1, 0};
        for (int v : w) { by.push_back(uint8_t(v)); by.push_back(uint8_t(v >> 8)); }
        const uint16_t c16 = kit::crc16r(by.data(), by.size(), 0xA001, 0xFFFF); by.push_back(uint8_t(c16)); by.push_back(uint8_t(c16 >> 8));
        Bits c = hb(0xAAAAAA, 24); cat(c, hb(0x2D, 8)); cat(c, bytesToBits(by)); cat(c, hb(0xAA, 8));
        Sig s{fskNrz(c, 2e6 / (49230 + 49261))}; s.fsk = true; s.devHz = 45e3;
        cs.push_back({"emonTx", s, "emonTx", "ct1", "7", false});
    }
    // Efergy e2 classic: FSK PWM, 500 µs sync, 64 µs mark = 1, 136 µs = 0, 200 µs a bit; bytes sum check
    {
        std::vector<uint8_t> by = {0x00, 0x34, 0xEA, 0x40, 0x09, 0xEB, 0x03};
        by.push_back(uint8_t(kit::sumBytes(by.data(), 7)));
        Tx tx; for (int i = 0; i < 16; ++i) { tx.mark(64); tx.space(60); }
        tx.space(1000); tx.mark(500); tx.space(150);
        for (uint8_t x : bytesToBits(by)) { tx.mark(x ? 64 : 136); tx.space(x ? 136 : 64); }
        tx.space(2000);
        Sig s{tx.r}; s.fsk = true; s.devHz = 40e3;
        cs.push_back({"Efergy e2 classic", s, "e2 classic", "id", "59956", false});
    }
    // Current Cost EnviR: 55 55 55 55 2D D4, Manchester 10 = 1; a counter packet (gas, 4321 pulses)
    {
        Bits c = hb(0x55555555ull, 32); cat(c, hb(0x2DD4, 16));
        for (uint8_t x : bytesToBits(hexBytes("404D0003000010E1"))) { c.push_back(x); c.push_back(!x); }
        cat(c, hb(0x55, 8));
        Sig s{fskNrz(c, 1e6 / 3918.0)}; s.fsk = true; s.devHz = 30e3;
        cs.push_back({"Current Cost counter", s, "EnviR impulse counter", "power0", "4321", false});
    }
    // Interlogix contact: PPM, ~120 µs marks, 122 µs gap = 0, 244 µs = 1; 12 zeros + a 1, then LSB-first fields
    {
        Bits d;
        auto lsb = [&](uint32_t v, int n) { for (int i = 0; i < n; ++i) d.push_back((v >> i) & 1); };
        lsb(0x7106D, 20); lsb(0xA, 4);                    // sensor ID, then device type A (contact), each LSB first
        lsb(0, 3); d.push_back(0); d.push_back(1); d.push_back(1); lsb(0, 9);   // trigger 0, battery OK, F1 latch + level set, the rest clear
        // patent bit 55 = even parity over the odd bits 15–55 (the start bit 15 is a 1), bit 56 = odd parity over 16–56
        int odd = 1, even = 0; for (size_t i = 0; i < 39; ++i) (i & 1 ? odd : even) ^= d[i];
        d.push_back(uint8_t(odd ^ 0)); d.push_back(uint8_t(even ^ 1));   // index 39 makes the odd set even with the start bit; 40 makes the even set odd
        d.push_back(0);                                   // bit 57, programmable
        int ones = 1; for (size_t i = 0; i < 39; ++i) ones += d[i];
        d.push_back(0); d.push_back(1); for (int i = 0; i < 3; ++i) d.push_back((ones >> i) & 1); d.push_back(0);   // 58 … 63
        Bits all = hb(0x0001, 12 + 1); cat(all, d);
        Tx tx; tx.mark(976);
        for (uint8_t x : all) { tx.space(x ? 244 : 122); tx.mark(122); }
        tx.space(20000);
        cs.push_back({"Interlogix contact", Sig{tx.r}, "contact", "subtype", "\"contact\"", true});
    }
    // Honeywell 5800: OOK Manchester 136 µs chips, raw preamble 55 55 56, then 01 = 1 pairs: ch 8 · id · event · CRC-16 0x8005
    {
        std::vector<uint8_t> by = {0x8A, 0xA9, 0x9B, 0x80};
        const uint16_t c16 = crc16(by.data(), 4, 0x8005, 0); by.push_back(uint8_t(c16 >> 8)); by.push_back(uint8_t(c16));
        Bits c = hb(0x5555, 16); cat(c, hb(0x555556, 24)); cat(c, manch01(bytesToBits(by))); cat(c, hb(0, 4));
        Tx tx; for (int rep = 0; rep < 3; ++rep) { for (uint8_t x : c) { if (x) tx.mark(136); else tx.space(136); } tx.space(8000); }
        cs.push_back({"Honeywell 5800 contact", Sig{tx.r}, "5800 series", "contact_open", "1", true});
    }
    // DSC: RZ, 500 µs bits, a 250 µs pulse in a 1's second half; 4 sync 1s, a 1 before every 8 further bits; CRC
    {
        uint8_t by[5] = {0x81, 0x24, 0xBD, 0xC8, 0};
        for (int c = 0; c < 256; ++c) { by[4] = uint8_t(c); if (kit::crc8r(by, 5, 0xF5, 0x3D) == 0) break; }
        Bits d = hb(0xF, 4);
        for (int k = 0; k < 4; ++k) { cat(d, hb(by[k], 8)); d.push_back(1); }
        cat(d, hb(by[4], 8));                 // the 1 before the CRC, then the CRC: 48 bits
        const Bits& frame = d;
        Tx tx; tx.mark(2500);
        for (uint8_t x : frame) { if (x) { tx.space(250); tx.mark(250); } else tx.space(500); }
        tx.space(20000);
        cs.push_back({"DSC contact", Sig{tx.r}, "sensor", "status", "129", true});
    }
    // Generic 433.3 MHz motion: 8 copies of a 20-bit code, alternating 888/1332 and 1332/1784 µs marks, 920/472 µs gaps
    {
        const uint32_t code = 0x800A2;
        Tx tx; tx.mark(9000);
        for (int p = 0; p < 8; ++p) {
            const double m1 = p & 1 ? 1332 : 888, m0 = p & 1 ? 1784 : 1332;
            tx.space(p ? 1672 : 1672);
            for (int i = 19; i >= 0; --i) { const bool one = (code >> i) & 1; if (i != 19) tx.space(one ? 920 : 472); tx.mark(one ? m1 : m0); }
        }
        tx.space(5000);
        cs.push_back({"Generic motion sensor", Sig{tx.r}, "motion sensor", nullptr, nullptr, true});
    }
    // LightwaveRF: 250 µs pulses; the line 1 + 10 × (1 + symbol) as short (1) / long (1 then 0) gaps; 6 repeats
    {
        static const uint8_t symb[16] = {0xF6, 0xEE, 0xED, 0xEB, 0xDE, 0xDD, 0xDB, 0xBE, 0xBD, 0xBB, 0xB7, 0x7E, 0x7D, 0x7B, 0x77, 0x6F};
        const int nib[10] = {0, 0, 2, 1, 0xF, 0x2, 0xF, 0x3, 0xD, 0x5};   // param 00, sub-unit 2, on, ID F2F3D5
        Bits line; line.push_back(1);
        for (int k = 0; k < 10; ++k) { line.push_back(1); cat(line, hb(symb[nib[k]], 8)); }
        line.push_back(1);
        Tx tx;
        for (int rep = 0; rep < 6; ++rep) {
            for (size_t i = 0; i < line.size(); ) {          // each 1 is a pulse; a 0 after it lengthens its gap
                if (!line[i]) { ++i; continue; }
                tx.mark(250);
                const bool z = i + 1 < line.size() && !line[i + 1];
                if (i + 1 < line.size()) tx.space(z ? 1250 : 250);
                i += z ? 2 : 1;
            }
            tx.space(10000);
        }
        cs.push_back({"LightwaveRF switch", Sig{tx.r}, "switch", "subunit", "2", true});
    }
    // Nexa / KlikAanKlikUit self-learning: T 270 µs; sync gap 10T; each data bit as two PPM bits (1 → 1 0, 0 → 0 1)
    {
        Bits d = hb(0x129ACDE, 26); d.push_back(0); d.push_back(1); cat(d, hb(0x3, 4));   // group 0, on, unit bits
        Tx tx;
        for (int rep = 0; rep < 6; ++rep) {
            tx.mark(270); tx.space(2650);
            for (uint8_t x : d) { tx.mark(270); tx.space(x ? 1300 : 270); tx.mark(270); tx.space(x ? 270 : 1300); }
            tx.mark(270); tx.space(10000);
        }
        cs.push_back({"Nexa self-learning", Sig{tx.r}, "self-learning", "state", "\"ON\"", true});
    }
    // X10 RF: 9 ms + 4.5 ms sync, 562 µs marks, a long gap reads 1 here; house B (0111), unit 1, on; 5 copies
    {
        const uint8_t b0 = 0x70, b2 = 0x00;
        std::vector<uint8_t> by = {b0, uint8_t(~b0), b2, uint8_t(~b2)};
        Tx tx;
        for (int rep = 0; rep < 5; ++rep) { tx.mark(9000); tx.space(4500); for (uint8_t x : bytesToBits(by)) { tx.mark(562); tx.space(x ? 1687 : 562); } tx.mark(562); tx.space(40000); }
        cs.push_back({"X10 remote", Sig{tx.r}, "remote", "channel", "\"B\"", true});
    }
    // GE Color Effects: 52 µs FSK chips; 1100 ×5, FF 00, then 10 = 0 / 1100 = 1: 00 · ID 6 · command 8 · 0; 3 repeats
    {
        Bits d = hb(0, 2); cat(d, hb(63, 6)); cat(d, hb(0x5A, 8)); d.push_back(0);
        Bits c;
        for (int rep = 0; rep < 3; ++rep) {
            for (int i = 0; i < 5; ++i) cat(c, hb(0xC, 4));
            cat(c, hb(0xFF00, 16));
            for (uint8_t x : d) cat(c, x ? hb(0xC, 4) : hb(0x2, 2));
            cat(c, hb(0, 20));
        }
        Sig s{fskNrz(c, 52)}; s.fsk = true; s.devHz = 30e3;
        cs.push_back({"GE Color Effects remote", s, "Color Effects", "command", "\"change\"", true});
    }
    // ERT SCM: OOK Manchester, 30.5 µs chips; preamble 0x1F2A60 (21 bits), the rest, BCH 0x6F63 over bytes 2–11
    {
        std::vector<uint8_t> by = {0xF9, 0x53, 0x02 | 0x01, uint8_t(12 << 2), 0x08, 0x95, 0x18, 0x41, 0x0C, 0x0C, 0, 0};
        by[2] = (by[2] & ~0x07) | 0x02;   // ID's high bits, 0b01 → 1 << 24 here
        for (uint32_t c = 0; c < 65536; ++c) { by[10] = uint8_t(c >> 8); by[11] = uint8_t(c); if (crc16(by.data() + 2, 10, 0x6F63, 0) == 0) break; }
        Bits d = bytesToBits(by);
        Bits c; for (uint8_t x : d) { c.push_back(x ? 1 : 0); c.push_back(x ? 0 : 1); }   // 10 = 1
        Tx tx; for (uint8_t x : c) { if (x) tx.mark(30.52); else tx.space(30.52); }
        Sig s{fskNrz(c, 30.52)}; s.runs.erase(s.runs.begin(), s.runs.begin() + (s.runs[0] < 0 ? 1 : 0));
        cs.push_back({"ERT SCM meter", s, "SCM", "ert_type", "12", false});
    }
    // Somfy RTS: 604 µs symbols; sync 4×(2416 on, 2416 off)…, a 4.8 ms soft sync, 604 off, 56 Manchester bits (01 = 1)
    {
        uint8_t pl[7] = {0xA7, 0x40, 0x00, 0x09, 0xAB, 0xC4, 0x37};   // seed, Down (4) + checksum nibble, counter 9, address
        uint8_t x = 0; for (int i = 0; i < 7; ++i) x ^= pl[i];
        pl[1] |= uint8_t((x & 0xF) ^ (x >> 4));
        uint8_t sc[7]; sc[0] = pl[0]; for (int i = 1; i < 7; ++i) sc[i] = uint8_t(pl[i] ^ sc[i - 1]);
        Tx tx;
        for (int rep = 0; rep < 3; ++rep) {
            for (int k = 0; k < (rep ? 7 : 2); ++k) { tx.mark(2416); tx.space(2416); }
            tx.mark(4832); tx.space(604);
            for (int i = 0; i < 56; ++i) { const bool one = (sc[i / 8] >> (7 - i % 8)) & 1; if (one) { tx.space(604); tx.mark(604); } else { tx.mark(604); tx.space(604); } }
            tx.space(30400);
        }
        cs.push_back({"Somfy RTS remote", Sig{tx.r}, "RTS remote", "control", "\"Down (4)\"", true});
    }
    // Clemsa: PWM 385 / 2695 µs marks, inverted (a long mark is a 1), 18 bits, 4 copies 10 ms apart
    {
        const uint32_t w = 0x0CE2C | 0x1;
        Tx tx;
        for (int rep = 0; rep < 4; ++rep) { for (int i = 17; i >= 0; --i) { const bool one = (w >> i) & 1; tx.mark(one ? 2695 : 385); tx.space(one ? 385 : 2695); } tx.space(10000); }
        cs.push_back({"Clemsa gate remote", Sig{tx.r}, "gate remote", "button", "1", true});
    }
    // Continental KR5V2X: FSK Manchester 100 µs halves (01 = 1), …f0f06, ID · · button · · sequence · code · XOR = 0
    {
        std::vector<uint8_t> by = {0x2A, 0x79, 0x17, 0x80, 0x00, 0x30, 0x00, 0xD4, 0x85, 0xB9, 0x00, 0x57, 0xD2, 0};
        by[13] = kit::xorBytes(by.data(), 13);
        Bits d = hb(0xFFFFF0F06ull, 36); cat(d, bytesToBits(by)); cat(d, hb(0, 4));
        Sig s{fskNrz(manch01(d), 100)}; s.fsk = true; s.devHz = 30e3;
        cs.push_back({"Continental car key", s, "Key fob", "button_code", "3", true});
    }
    // Chrysler: PWM TE 350 µs (a short mark = 1, then the whole word inverted), two packets 49 + 48 bits, bytes reflected
    {
        uint8_t by[6] = {0x40, 0xE8, 0xCA, 0x2E, 0x40, 0};
        by[5] = uint8_t(kit::sumBytes(by, 5));
        Bits w; for (uint8_t v : by) { const uint8_t r = kit::rev8(v); for (int i = 7; i >= 0; --i) w.push_back(!((r >> i) & 1)); }
        Tx tx;
        for (int p = 0; p < 2; ++p) {
            Bits pk = w; if (p == 0) pk.push_back(1);
            for (uint8_t x : pk) { tx.mark(x ? 350 : 700); tx.space(x ? 700 : 350); }
            tx.space(12000);
        }
        cs.push_back({"Chrysler car key", Sig{tx.r}, "Key fob", "button_code", "4", true});
    }
    // RadioHead ASK: 500 µs NRZ; 0101… then 0xB38 LSB first, bytes as two 4-to-6 symbols LSB first, CRC-16 (~CCITT LSB)
    {
        static const uint8_t k6[16] = {0x0D, 0x0E, 0x13, 0x15, 0x16, 0x19, 0x1A, 0x1C, 0x23, 0x25, 0x26, 0x29, 0x2A, 0x2C, 0x32, 0x34};
        std::vector<uint8_t> p = {12, 0xFF, 0x07, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o'};
        const uint16_t c = uint16_t(~kit::crc16r(p.data(), p.size(), 0x8408, 0xFFFF)); p.push_back(uint8_t(c)); p.push_back(uint8_t(c >> 8));
        Bits d = hb(0x555555, 24); cat(d, hb(0x5551CD, 24));
        for (uint8_t v : p) for (int half = 0; half < 2; ++half) { const uint8_t sym = k6[half ? (v & 0xF) : (v >> 4)]; for (int i = 0; i < 6; ++i) d.push_back((sym >> i) & 1); }
        cat(d, hb(0, 8));
        Tx tx; for (uint8_t x : d) { if (x) tx.mark(500); else tx.space(500); }
        cs.push_back({"RadioHead ASK packet", Sig{tx.r}, "ASK packet", "from", "7", false});
    }
    // X10 security: X10 RF line, 41 bits: ID · its top nibble · low nibble XOR · code · ¬code · ID 2 · even parity
    {
        const uint8_t id = 0x5A, code = 0x0C;   // motion
        std::vector<uint8_t> by = {id, uint8_t(id ^ 0x0F), code, uint8_t(~code), 0x3C};
        Bits w = bytesToBits(by); int par = 0; for (uint8_t x : w) par ^= x; w.push_back(uint8_t(par));
        Tx tx; for (int rep = 0; rep < 3; ++rep) { tx.mark(9000); tx.space(4500); for (uint8_t x : w) { tx.mark(562); tx.space(x ? 1687 : 562); } tx.mark(562); tx.space(20000); }
        cs.push_back({"X10 motion sensor", Sig{tx.r}, "security sensor", "event", "\"MOTION TRIPPED\"", true});
    }
    homeRun(cs);
}

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--write-clip") return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    printf("test-ism — the V12 ISM decoder\n");

    // 1. Published worked examples, straight into the field parsers
    {
        printf(" worked examples\n");
        Reading r;
        // Oregon doc p13: THGR122NX "1D20485C480882835" → ch 3, rolling 85, battery low, −8.4 °C, 28 %, checksum 0x53
        CHECK(oregon::parse(oregonNibbles("1D20485C480882835"), true, r), "Oregon example 1 rejected");
        CHECK(approx(r.tempC, -8.4, 1e-9) && approx(r.hum, 28, 0) && r.channel == 3 && r.battOk == 0 && r.pubId == "85",
              "Oregon example 1 fields: %.1f %.0f ch%d batt%d id %s", r.tempC, r.hum, r.channel, r.battOk, r.pubId.c_str());
        r = Reading();
        CHECK(oregon::parse(oregonNibbles("1D2016B1091073A14"), true, r), "Oregon example 2 rejected");
        CHECK(approx(r.tempC, 19.0, 1e-9) && approx(r.hum, 37, 0) && r.channel == 1 && r.battOk == 1, "Oregon example 2 fields");
        CHECK(!oregon::parse(oregonNibbles("1D2016B1091073A15"), true, r), "Oregon: a bad checksum accepted");
        // WH1080 (Sangeelee) — three messages, CRC-8 0x31 over bytes 0–8
        r = Reading();
        CHECK(fineoffset::wh1080Fields(hexBytes("a1820d5a030600 4e08d3"), false, r), "WH1080 example 1 rejected");
        CHECK(approx(r.tempC, 12.5, 1e-9) && r.hum == 90 && approx(r.windKmh, 3 * 0.34 * 3.6, 1e-9) && approx(r.gustKmh, 6 * 0.34 * 3.6, 1e-9)
              && approx(r.rainMm, 23.4, 1e-9) && approx(r.windDir, 180, 0), "WH1080 example 1 fields %.1f %.0f %.2f %.1f %.0f", r.tempC, r.hum, r.windKmh, r.rainMm, r.windDir);
        r = Reading(); CHECK(fineoffset::wh1080Fields(hexBytes("a1820d59020500 4e0a07"), false, r) && approx(r.tempC, 12.5, 1e-9) && r.hum == 89
                             && approx(r.windDir, 225, 0), "WH1080 example 2");
        r = Reading(); CHECK(fineoffset::wh1080Fields(hexBytes("a1820e5d020400 4e0686"), false, r) && approx(r.tempC, 12.6, 1e-9), "WH1080 example 3");
        // WH1080 V2 FSK (sevenwatt) — 3.9 °C, 71 %
        r = Reading(); CHECK(fineoffset::wh1080Fields(hexBytes("a4f0274700 0003c60cfe"), true, r) && approx(r.tempC, 3.9, 1e-9) && r.hum == 71
                             && r.pubId == "4F", "WH1080 FSK example %.1f %.0f %s", r.tempC, r.hum, r.pubId.c_str());
        // Acurite 592 family (rayshobby part 2): 34 % / ~21 °C, 35 % / ~24 °C, 16 % / ~36 °C — sum checksum ours
        {
            const char* ex[3] = {"9302442209BEC2", "930244A309D85D", "930244900AD74A"};
            const double t[3] = {21.4, 24.0, 36.7}; const int h[3] = {34, 35, 16};
            for (int i = 0; i < 3; ++i) {
                Burst b; b.ook = acuritePwm(hexBytes(ex[i]));
                std::vector<Reading> o; acurite::decodePwm(b, o);
                CHECK(o.size() == 1 && approx(o[0].tempC, t[i], 1e-9) && o[0].hum == h[i], "Acurite 592 example %d", i + 1);
            }
        }
        // Acurite 00899 rain (part 3): 0, 1, 13 tips
        {
            const char* ex[3] = {"72F8F0000000005A", "72F8F000000081DB", "72F8F00000008DE7"};
            const char* want[3] = {"0 bucket tips", "1 bucket tip", "13 bucket tips"};
            for (int i = 0; i < 3; ++i) {
                Burst b; b.ook = acuritePwm(hexBytes(ex[i]));
                std::vector<Reading> o; acurite::decodePwm(b, o);
                CHECK(o.size() == 1 && o[0].extra == want[i], "Acurite 899 example %d: %s", i + 1, o.empty() ? "nothing" : o[0].extra.c_str());
            }
        }
        // Acurite 00782 (part 1): 31.8 °C, −0.5 °C
        {
            Burst b; b.ook = acurite782("8B813E3D"); std::vector<Reading> o; acurite::decode782(b, o);
            CHECK(o.size() == 1 && approx(o[0].tempC, 31.8, 1e-9), "Acurite 782 31.8");
            Burst c; c.ook = acurite782("8B8FFB17"); o.clear(); acurite::decode782(c, o);
            CHECK(o.size() == 1 && approx(o[0].tempC, -0.5, 1e-9), "Acurite 782 -0.5");
        }
        // Current Cost (Kelly wiki): address 63B, 390 W
        {
            Burst b; b.fsk = currentCostTx("063B818600000000"); std::vector<Reading> o; currentcost::decode(b, o);
            CHECK(o.size() == 1 && o[0].pubId == "63B" && approx(o[0].watts, 390, 0), "Current Cost example: %s %.0f",
                  o.empty() ? "-" : o[0].pubId.c_str(), o.empty() ? 0.0 : o[0].watts);
        }
    }

    // 2. End to end: each family encoded, modulated into IQ at 2.4 MS/s, +120 kHz off the capture centre
    struct Case { const char* name; Sig s; const char* model; double temp; };
    std::vector<Case> cases;
    cases.push_back({"Oregon v3 THGR810", {oregonV3(withChecksum(oregonNibbles("F824" "2" "6B" "0" "481" "0" "26" "0"), 17))}, "THGR810", 18.4});
    cases.push_back({"Oregon v2.1 THGR122NX", {oregonV21(withChecksum(oregonNibbles("1D20" "4" "85" "0" "480" "8" "82" "8"), 17))}, "THGR122NX", -8.4});
    cases.push_back({"WH1080", {wh1080("a1820d5a0306004e08d3")}, "WH1080", 12.5});
    cases.push_back({"WH2", {wh2(0x1A5, 21.3, 55)}, "WH2", 21.3});
    cases.push_back({"Nexus", {nexusTx(0x5C, 2, -3.7, 56, true)}, "temperature/humidity", -3.7});
    { Sig s{lacrosseTx(0x2B, 20.2, 61, false)}; s.fsk = true; s.devHz = 40e3; cases.push_back({"LaCrosse TX29", s, "IT+", 20.2}); }
    cases.push_back({"Acurite 592", {acuritePwm(hexBytes("9302442209BEC2"))}, "592TX", 21.4});
    cases.push_back({"Acurite 782", {acurite782("8B813E3D")}, "00782", 31.8});
    { Sig s{currentCostTx("063B818600000000")}; s.fsk = true; s.devHz = 30e3; cases.push_back({"Current Cost", s, "transmitter", NAN}); }
    cases.push_back({"KeeLoq", {keeloqTx(0xDEADBEEF, 0x0ABCDEF, 0x2, false)}, "Key fob", NAN});
    cases.push_back({"EV1527", {fixedCodeTx(0xA5C3E8)}, "Doorbell", NAN});
    cases.push_back({"TPMS B", {tpmsBTx(0x11223344, 88, 18)}, "type B", 18});
    { Sig s{tpmsATx(0x55667788, 0x80, 21)}; s.fsk = true; s.devHz = 40e3; cases.push_back({"TPMS A", s, "type A", 21}); }
    printf(" end to end at 2.4 MS/s, +120 kHz, 20 dB-ish SNR\n");
    for (auto& c : cases) {
        c.s.offHz = 120e3;
        const auto x = synth({c.s}, 2.4e6, 0.05 + std::abs(c.s.runs.size() ? 0 : 0) + 0.9, 0.02, 1);
        std::vector<Burst> bs;
        const auto got = runFront(x, 2.4e6, &bs);
        const Reading* r = findModel(got, c.model);
        CHECK(r != nullptr, "%s: not decoded (%zu bursts, %zu readings%s%s)", c.name, bs.size(), got.size(), got.empty() ? "" : " — ", got.empty() ? "" : (got[0].make + " " + got[0].model).c_str());
        if (r && c.temp == c.temp) CHECK(approx(r->tempC, c.temp, 0.051), "%s: %.2f °C, want %.2f", c.name, r->tempC, c.temp);
        if (r) {
            CHECK(approx(r->freqHz, 433.92e6 + 120e3, 12e3), "%s: frequency %.0f", c.name, r->freqHz);
            printf("   %-22s → %s\n", c.name, readable(*r, 1).c_str());
        }
    }
    /* ★★ V12 wM-Bus (2026-10-26): the 868 band's burst channel is ~500 kS/s now (FrontEnd::setChanRate) and the slicer's
     *  smoothing is in µs — every family must slice through it exactly as it does at 250 k. All of them, not only the
     *  868 ones: the slicer is the same code. */
    printf(" end to end through the 868 band's ~500 kS/s channel\n");
    for (auto& c : cases) {
        const auto x = synth({c.s}, 2.4e6, 0.95, 0.02, 1);
        const auto got = runFront(x, 2.4e6, nullptr, 500e3);
        const Reading* r = findModel(got, c.model);
        CHECK(r != nullptr, "%s: not decoded through the 480 kS/s channel (%zu readings)", c.name, got.size());
        if (r && c.temp == c.temp) CHECK(approx(r->tempC, c.temp, 0.051), "%s (480 k): %.2f °C, want %.2f", c.name, r->tempC, c.temp);
    }
    // the TPMS B values exactly
    {
        Sig s{tpmsBTx(0x11223344, 88, 18)}; s.offHz = -60e3;
        const auto got = runFront(synth({s}, 2.4e6, 1.0, 0.02, 2), 2.4e6);
        const Reading* r = findModel(got, "type B");
        CHECK(r && approx(r->kpa, 220, 1e-9) && r->confirm, "TPMS B: 220 kPa, confirm flag");
    }

    // 3. Drift: ±150 kHz around the band centre, and the capture rates the radios run (Oregon v3, the slowest-edged)
    printf(" drift and capture rates\n");
    {
        const Runs o = oregonV3(withChecksum(oregonNibbles("F824" "1" "22" "0" "571" "0" "45" "0"), 17));
        int ok = 0, n = 0;
        for (double off = -150e3; off <= 150e3; off += 25e3) {
            if (std::fabs(off) < 20e3) continue;
            Sig s{o}; s.offHz = off; ++n;
            if (findModel(runFront(synth({s}, 2.4e6, 0.5, 0.02, uint32_t(off + 1e6)), 2.4e6), "THGR810")) ++ok;
        }
        CHECK(ok == n, "drift: %d of %d offsets decoded", ok, n);
        printf("   drift −150…+150 kHz: %d/%d\n", ok, n);
        const double rates[] = {1.024e6, 2.048e6, 2.4e6, 2.56e6, 3.0e6, 6.0e6, 10.0e6};
        for (double rt : rates) {
            Sig s{wh1080("a1820d5a0306004e08d3")}; s.offHz = std::min(150e3, rt * 0.3);
            const bool got = findModel(runFront(synth({s}, rt, 0.5, 0.02, 7), rt), "WH1080") != nullptr;
            Sig s2{o}; s2.offHz = -std::min(150e3, rt * 0.3);
            const bool got2 = findModel(runFront(synth({s2}, rt, 0.5, 0.02, 8), rt), "THGR810") != nullptr;
            CHECK(got && got2, "rate %.3f MS/s: WH1080 %d, Oregon %d", rt / 1e6, got, got2);
        }
    }

    // 4. SNR: Oregon v3 and WH1080 at falling signal levels (noise σ 0.02 per component)
    printf(" sensitivity\n");
    {
        const Runs o = oregonV3(withChecksum(oregonNibbles("F824" "1" "22" "0" "571" "0" "45" "0"), 17));
        for (double amp : {0.1, 0.03, 0.015, 0.008, 0.005}) {
            int ok = 0;
            for (int k = 0; k < 6; ++k) { Sig s{o}; s.offHz = 90e3; s.amp = amp; if (findModel(runFront(synth({s}, 2.4e6, 0.4, 0.02, uint32_t(100 + k)), 2.4e6), "THGR810")) ++ok; }
            // ★ SNR in the 250 kHz channel: amp² / (2σ² × 250k / 2.4M)
            const double snr = 10 * std::log10(amp * amp / (2 * 0.02 * 0.02 * 250e3 / 2.4e6));
            printf("   Oregon amp %.3f (channel SNR %+.1f dB): %d/6\n", amp, snr, ok);
            if (amp >= 0.015) CHECK(ok == 6, "Oregon at amp %.3f: %d/6", amp, ok);
        }
    }

    // 5. Several transmitters at once, at different frequencies
    {
        Sig a{oregonV3(withChecksum(oregonNibbles("F824" "3" "11" "0" "052" "8" "90" "0"), 17))}; a.offHz = -200e3;
        Sig b{wh1080("a1820d5a0306004e08d3")}; b.offHz = 60e3; b.startS = 0.08;
        Sig c{fixedCodeTx(0x123458)}; c.offHz = 300e3; c.startS = 0.06;
        const auto got = runFront(synth({a, b, c}, 2.4e6, 0.8, 0.02, 9), 2.4e6);
        CHECK(findModel(got, "THGR810") && findModel(got, "WH1080") && findModel(got, "Doorbell"), "three at once: %zu readings", got.size());
        const Reading* ao = findModel(got, "THGR810");
        CHECK(ao && approx(ao->tempC, -25.0, 1e-9), "three at once: Oregon −25.0 °C");
    }

    // 6. Pure noise: nothing — 20 s at 2.4 MS/s, two seeds; and a CW carrier that never stops
    printf(" false positives\n");
    {
        size_t total = 0, bursts = 0;
        for (uint32_t seed : {11u, 12u}) {
            std::vector<Burst> bs;
            total += runFront(synth({}, 2.4e6, 10.0, 0.02, seed), 2.4e6, &bs).size();
            bursts += bs.size();
        }
        CHECK(total == 0, "pure noise decoded %zu readings", total);
        printf("   20 s of noise: %zu bursts detected, %zu readings\n", bursts, total);
        Sig cw; cw.runs = {3000000}; cw.offHz = 170e3; cw.startS = 0.01;
        CHECK(runFront(synth({cw}, 2.4e6, 3.5, 0.02, 13), 2.4e6).empty(), "a CW carrier decoded as something");
    }

    // 7. PRIVACY: the JSON never carries a TPMS ID, a fob's serial or hop code, or a fixed code
    // ★ SUSPECTED KEY FOBS (FobWatch) — modelled on Stuart's car key as recorded 2026-10-25 (FSK ±42.7 kHz, Manchester
    //   200 µs half-bits after an alternating preamble, 68 ms frames, three 161 ms apart). Never a protocol: the shape.
    // ★ FALSE DECODES SEEN ON THE PI 500's 868 BAND (2026-10-25) — each must now be refused
    printf(" false decodes refused\n");
    {
        Reading r; r.make = "AcuRite"; r.kind = "weather";
        CHECK(!bandPlausible(r, 868.3e6) && bandPlausible(r, 433.92e6), "band: AcuRite refused at 868, kept at 433.92");
        Reading t; t.kind = "tpms"; Reading lc; lc.make = "La Crosse"; lc.kind = "weather";
        CHECK(!bandPlausible(t, 868.95e6) && bandPlausible(lc, 868.3e6), "band: TPMS refused at 868, La Crosse kept there");
        // the rain gauge: ONE good frame is no longer enough (three repeats are sent; two must agree)
        std::vector<uint8_t> by = hexBytes("A2 30 01 00 00 01 0B 00");     // 139 tips
        for (int i = 3; i < 7; ++i) if (!evenParityOk(by[size_t(i)])) by[size_t(i)] ^= 0x80;
        unsigned sum = 0; for (int i = 0; i < 7; ++i) sum += by[size_t(i)]; by[7] = uint8_t(sum);
        Tx one; one.space(2250); for (int i = 0; i < 4; ++i) { one.mark(600); one.space(600); }
        for (uint8_t x : bytesToBits(by)) { if (x) { one.mark(400); one.space(225); } else { one.mark(250); one.space(400); } }
        one.space(2250);
        Burst b1; b1.ook = one.r; std::vector<Reading> o1; acurite::decodePwm(b1, o1);
        CHECK(o1.empty(), "rain gauge: a single frame decoded (%zu)", o1.size());
        Burst b3; b3.ook = acuritePwm(by); std::vector<Reading> o3; acurite::decodePwm(b3, o3);
        CHECK(o3.size() == 1 && o3[0].model == "00899 rain gauge", "rain gauge: three repeats still decode (%zu)", o3.size());
        // an impossible tip count (here > 100 000) is refused even when repeated
        std::vector<uint8_t> big = hexBytes("A2 30 01 FF FF FF FF 00");
        for (int i = 3; i < 7; ++i) if (!evenParityOk(big[size_t(i)])) big[size_t(i)] ^= 0x80;
        sum = 0; for (int i = 0; i < 7; ++i) sum += big[size_t(i)]; big[7] = uint8_t(sum);
        Burst bb; bb.ook = acuritePwm(big); std::vector<Reading> ob; acurite::decodePwm(bb, ob);
        CHECK(ob.empty(), "rain gauge: an impossible tip count decoded (%s)", ob.empty() ? "" : ob[0].extra.c_str());
    }

    printf(" suspected key fobs\n");
    {
        // one frame: 16 bits of preamble (1010…), then a fixed made-up payload, Manchester, 200 µs halves = 2.5 kbit/s
        auto keyFrame = []() {
            Bits b; for (int i = 0; i < 16; ++i) b.push_back(i & 1);
            const Bits p = bytesToBits(hexBytes("3C 96 A5 0F E1 72 5B C8 19 D4 6E 0B 93 A7 2D F0 58"));
            b.insert(b.end(), p.begin(), p.end());
            Tx tx; manchesterTx(tx, b, 200, 0); return tx.r;
        };
        const Runs fr = keyFrame();
        // END TO END: three frames through the real front end at 2.4 MS/s → bursts the decoders refuse, FobWatch counts ONE press
        {
            std::vector<Sig> sigs;
            for (int k = 0; k < 3; ++k) { Sig s; s.runs = fr; s.fsk = true; s.devHz = 42.7e3; s.offHz = 214e3; s.amp = 0.3; s.startS = 0.05 + 0.161 * k; sigs.push_back(s); }
            std::vector<Burst> bs;
            const auto x = synth(sigs, 2.4e6, 0.7, 0.01, 77);
            const auto dec = runFront(x, 2.4e6, &bs);
            CHECK(dec.empty(), "fob: a protocol decoder claimed the key (%zu)", dec.size());
            double T = 0, len = 0; size_t cand = 0;
            for (const Burst& b : bs) if (FobWatch::candidate(b, T, len)) ++cand;
            CHECK(cand >= 3, "fob: %zu of %zu bursts look like a key frame (T %.0f µs, %.1f ms)", cand, bs.size(), T, len);
            FobWatch fw; int presses = 0; Reading got;
            for (const Burst& b : bs) { Reading r; if (fw.offer(b, 433.92e6, r)) { ++presses; got = r; } }
            CHECK(presses == 1, "fob: three frames are %d presses, want 1", presses);
            CHECK(got.kind == "suspectfob" && got.secret && approx(got.freqHz, 433.92e6 + 214e3, 10e3), "fob: the reading (%s, %.0f Hz)",
                  got.kind.c_str(), got.freqHz);
            Tracker tr; tr.accept(got, 1.0);
            const std::string j = tr.json(1.0);
            CHECK(j.find("Suspected key fob #1 \xc2\xb7 button press") != std::string::npos && j.find("\"kind\":\"suspectfob\"") != std::string::npos,
                  "fob: the line %s", j.c_str());
            CHECK(j.find("\"id\"") == std::string::npos && j.find("3C96") == std::string::npos, "fob: an id or bits in the JSON");
        }
        // THE RULES, on bursts made directly (seconds of timing without seconds of IQ)
        auto frame = [&](double t, double offHz, double lvl = -10) {
            Burst b; b.tSec = t; b.offHz = offHz; b.fsk = fr; b.fskDevHz = 42.7e3; b.levelDb = float(lvl); b.noiseDb = -45; return b;
        };
        auto count = [&](const std::vector<Burst>& v, FobWatch& fw) { int n = 0; for (const Burst& b : v) { Reading r; if (fw.offer(b, 433.92e6, r)) ++n; } return n; };
        {
            FobWatch fw; std::vector<Burst> v;
            for (int k = 0; k < 6; ++k) v.push_back(frame(1.0 + 1.2 * k, 360e3));          // a sensor: one frame every 1.2 s
            CHECK(count(v, fw) == 0, "fob: single frames every 1.2 s counted as presses");
        }
        {
            FobWatch fw; std::vector<Burst> v;
            for (int k = 0; k < 12; ++k) v.push_back(frame(1.0 + 0.161 * k, 214e3));        // a held button
            CHECK(count(v, fw) == 1, "fob: a held button is not one press");
        }
        {
            FobWatch fw; std::vector<Burst> v;
            for (double t0 : {1.0, 2.3}) for (int k = 0; k < 3; ++k) v.push_back(frame(t0 + 0.161 * k, 214e3));   // 1.3 s apart
            CHECK(count(v, fw) == 2, "fob: two presses 1.3 s apart are not two");
        }
        {
            FobWatch fw; std::vector<Burst> v;                                              // overload images ±120 kHz
            for (int k = 0; k < 3; ++k) { v.push_back(frame(1.0 + 0.161 * k, 214e3)); v.push_back(frame(1.001 + 0.161 * k, 94e3, -12)); v.push_back(frame(1.002 + 0.161 * k, 334e3, -12)); }
            CHECK(count(v, fw) == 1, "fob: a press and its images are not one press");
        }
        {
            FobWatch fw; std::vector<Burst> v;                                              // presses on a 10 s timer = a sensor
            for (int p = 0; p < 5; ++p) for (int k = 0; k < 2; ++k) v.push_back(frame(1.0 + 10.0 * p + 0.161 * k, 500e3));
            const int n = count(v, fw);
            CHECK(n == 3, "fob: a 10 s timer gave %d presses before it was recognised, want 3", n);
            CHECK(fw.takePeriodic().size() == 1, "fob: the timer was not reported");
        }
        {
            FobWatch fw; Burst b = frame(1.0, 214e3); b.fsk.clear();                         // OOK: never
            Burst b2 = b; b2.tSec = 1.161;
            CHECK(count({b, b2}, fw) == 0, "fob: OOK bursts counted");
            Burst w = frame(2.0, 214e3, -38); Burst w2 = frame(2.161, 214e3, -38);          // 7 dB over the noise: too weak to call
            CHECK(count({w, w2}, fw) == 0, "fob: a 7 dB burst counted");
        }
    }

    printf(" privacy\n");
    {
        Tracker tr;
        std::vector<Reading> v;
        Burst kb; kb.ook = keeloqTx(0xDEADBEEF, 0x0ABCDEF, 0x3, true); keeloq::decode(kb, v);
        Burst fb; fb.ook = fixedCodeTx(0xA5C3E8); fixedcode::decode(fb, v);
        Burst tb; tb.ook = tpmsBTx(0x11223344, 88, 18); tpms::decodeB(tb, v);
        Burst tb2; tb2.ook = tpmsBTx(0x99AABBCC, 92, 20); tpms::decodeB(tb2, v);
        CHECK(v.size() == 4, "privacy setup: %zu readings", v.size());
        for (int pass = 0; pass < 2; ++pass) for (const auto& r : v) tr.accept(r, 10.0 + pass);   // twice: TPMS B confirms
        const std::string j = tr.json(12.0);
        const char* banned[] = {"DEADBEEF", "deadbeef", "ABCDEF", "abcdef", "A5C3E", "a5c3e", "11223344", "99AABBCC", "99aabbcc",
                                "3735928559", "11259375", "10863592", "287454020"};
        for (const char* s : banned) CHECK(j.find(s) == std::string::npos, "privacy: the JSON carries %s", s);
        CHECK(j.find("TPMS #1") != std::string::npos && j.find("TPMS #2") != std::string::npos, "privacy: two cars, two nicknames");
        CHECK(j.find("Key fob #1") != std::string::npos && j.find("button 1+2") != std::string::npos && j.find("battery low") != std::string::npos,
              "privacy: the fob line");
        CHECK(j.find("Doorbell / remote #1") != std::string::npos, "privacy: the remote line");
        CHECK(j.find("\"id\"") == std::string::npos, "privacy: an id field on a secret kind");
        printf("   %s\n", j.substr(0, 400).c_str());
        // the same car again → the same nickname; a NEW server (new salt) numbers afresh but never shows the ID either
        Tracker tr2;
        tr2.accept(v[3], 1); tr2.accept(v[3], 2);
        CHECK(tr2.json(3).find("TPMS #1") != std::string::npos, "privacy: numbering is per server");
        // a weather sensor DOES carry its channel and short ID
        Tracker tw; Reading w; w.kind = "weather"; w.make = "Oregon Scientific"; w.model = "THGR810"; w.channel = 2; w.pubId = "6B";
        w.tempC = 18.4; w.hum = 62; tw.accept(w, 1);
        const std::string wj = tw.json(1);
        CHECK(wj.find("Oregon Scientific THGR810 \xc2\xb7 18.4 \xc2\xb0" "C \xc2\xb7 62 % RH \xc2\xb7 ch 2") != std::string::npos, "the weather line: %s", wj.c_str());
        CHECK(wj.find("\"id\":\"6B\"") != std::string::npos, "the weather ID");
    }

    // 8. Repeats fold; units
    {
        Tracker tr; Reading w; w.kind = "weather"; w.make = "Fine Offset"; w.model = "WH2"; w.pubId = "1A5"; w.tempC = 21.3; w.hum = 55;
        tr.accept(w, 1.0); tr.accept(w, 1.5); tr.accept(w, 2.0);
        CHECK(tr.size() == 1 && tr.json(2).find("\"count\":1") != std::string::npos, "repeats fold into one heard");
        tr.accept(w, 60.0);
        CHECK(tr.json(60).find("\"count\":2") != std::string::npos, "a minute later counts again");
        Reading t; t.kind = "tpms"; t.secret = true; t.make = "TPMS"; t.model = "type B"; t.kpa = 220; t.tempC = 18;
        CHECK(readable(t, 2).find("32 PSI") != std::string::npos, "PSI by default: %s", readable(t, 2).c_str());
        Units kp; kp.pressure = 1; CHECK(readable(t, 2, kp).find("220 kPa") != std::string::npos, "kPa");
        Units br; br.pressure = 2; CHECK(readable(t, 2, br).find("2.20 bar") != std::string::npos, "bar");
        Reading a; a.kind = "weather"; a.make = "Fine Offset"; a.model = "WH1080"; a.windKmh = 14; a.windDir = 225; a.rainMm = 2.3; a.tempC = 11.2;
        CHECK(readable(a, 0) == "Fine Offset WH1080 \xc2\xb7 11.2 \xc2\xb0" "C \xc2\xb7 wind 14 km/h SW \xc2\xb7 rain 2.3 mm", "the wind line: %s", readable(a, 0).c_str());
        Units mph; mph.mph = true; CHECK(readable(a, 0, mph).find("wind 9 mph SW") != std::string::npos, "mph");
    }

    // 9. Bands: landing by location, reachability
    {
        auto all = [](const Band&) { return true; };
        CHECK(std::string(landingBand(52.2, -0.9, all)->id) == "433", "UK lands on 433.92");
        CHECK(std::string(landingBand(40.7, -74.0, all)->id) == "315", "US lands on 315");
        CHECK(std::string(landingBand(NAN, NAN, all)->id) == "433", "no position lands on 433.92");
        auto no315 = [](const Band& b) { return std::string(b.id) != "315"; };
        CHECK(std::string(landingBand(40.7, -74.0, no315)->id) == "915", "US without 315 → 915");
        auto none = [](const Band&) { return false; };
        CHECK(landingBand(52.2, -0.9, none) == nullptr, "an HF+ (no band) has no landing");
    }

    // 10. CPU: the front end over 10 s of a busy 433 band at 2.4 MS/s
    {
        std::vector<Sig> sigs;
        for (int k = 0; k < 10; ++k) { Sig s{k % 2 ? wh1080("a1820d5a0306004e08d3") : oregonV3(withChecksum(oregonNibbles("F824" "1" "22" "0" "571" "0" "45" "0"), 17))};
                                       s.offHz = -330e3 + 60e3 * k; s.startS = 0.3 + k * 0.9; sigs.push_back(s); }   // ★ none at DC (the guard)
        const auto x = synth(sigs, 2.4e6, 10.0, 0.02, 21);
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<Burst> bs;
        const auto got = runFront(x, 2.4e6, &bs);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        printf(" CPU: 10 s of capture at 2.4 MS/s (10 bursts) in %.3f s = %.1f %% of one core on this machine; %zu decoded\n",
               sec, sec * 10.0, got.size());
        CHECK(got.size() >= 10, "CPU run decoded %zu of 10", got.size());   // ★ a burst may decode twice (a repeat): the tracker folds it
    }

    // 11. ★ THE SERVICE'S THREADS (2026-10-31, the Pi 2 split): detector, mixer and slicer each on their own thread give
    //     the very bursts the inline front end gives — same times, frequencies, run lists and readings
    {
        std::vector<Sig> sigs;
        for (int k = 0; k < 10; ++k) { Sig s{k % 2 ? wh1080("a1820d5a0306004e08d3") : oregonV3(withChecksum(oregonNibbles("F824" "1" "22" "0" "571" "0" "45" "0"), 17))};
                                       s.offHz = -330e3 + 60e3 * k; s.startS = 0.3 + k * 0.9; sigs.push_back(s); }
        const auto x = synth(sigs, 2.4e6, 10.0, 0.02, 21);
        const Band* b433 = bandById("433");
        auto key = [](const Burst& b, const std::vector<Reading>& r) {
            char k[96]; std::snprintf(k, sizeof k, "%.6f %.1f %.2f %zu %zu|", b.tSec, b.offHz, double(b.levelDb), b.ook.size(), b.fsk.size());
            std::string s = k; for (int d : b.ook) s += std::to_string(d) + ","; for (const auto& x : r) s += "|" + readable(x, 0); return s;
        };
        // inline, limited to the band as the service limits it
        std::vector<std::string> want;
        {
            FrontEnd fe(2.4e6); fe.setDcGuardHz(15e3);
            const double mid = b433->centreHz - 433.92e6, half = b433->needHz + 40e3;
            fe.setUsableHz(mid - half, mid + half);
            auto on = [&](const Burst& b) { std::vector<Reading> r; Service::decodeBurst(b, 433.92e6, r); want.push_back(key(b, r)); };
            for (size_t i = 0; i < x.size(); i += 76800) fe.feed(x.data() + i, std::min<size_t>(76800, x.size() - i), on);
            fe.flush(on);
        }
        std::vector<std::string> got;
        std::mutex gm;
        {
            Service sv;
            sv.setLossless(true);
            sv.setLora(false, 0);
            sv.setTap([&](const Burst& b, const std::vector<Reading>& r) { std::lock_guard<std::mutex> g(gm); got.push_back(key(b, r)); });
            sv.start(*b433, 433.92e6, 2.4e6);
            for (size_t i = 0; i < x.size(); i += 76800) sv.feed(x.data() + i, std::min<size_t>(76800, x.size() - i));
            sv.endOfStream();
            for (int w = 0; w < 3000 && !sv.idle(); ++w) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CHECK(sv.idle(), "the service drained");
            CHECK(sv.droppedBlocks() == 0, "lossless: nothing dropped (%llu)", (unsigned long long)sv.droppedBlocks());
            const std::string rep = sv.report();
            CHECK(rep.find("\"drops\":[0,0,0]") != std::string::npos, "the report says what each stage dropped");
            sv.stop();
        }
        CHECK(!want.empty() && got == want, "threads = inline: %zu bursts against %zu, identical", got.size(), want.size());
        printf(" threads: %zu bursts through vibe-ism / vibe-ism2 / vibe-ism3, identical to inline\n", got.size());

        // 12. ★ A DROPPED BLOCK BEFORE THE MIXER: the channels open across the hole are abandoned (never stitched over
        //     it), the mixer counts the gap, and every burst wholly after it is exactly what it would have been
        Detector det(2.4e6); det.setDcGuardHz(15e3);
        struct Job { uint64_t start; size_t off, n; std::vector<ChanEvent> ev; };
        std::vector<Job> jobs;
        for (size_t i = 0; i < x.size(); i += 76800) {
            Job j; j.start = det.total(); j.off = i; j.n = std::min<size_t>(76800, x.size() - i);
            det.feed(x.data() + i, j.n, j.ev, nullptr); jobs.push_back(std::move(j));
        }
        auto runMix = [&](size_t skip, uint64_t* gaps) {
            ChanMixer mx(2.4e6);
            std::vector<std::pair<double, size_t>> out;   // (start time, samples)
            for (size_t k = 0; k < jobs.size(); ++k) {
                if (k == skip) continue;
                mx.feed(x.data() + jobs[k].off, jobs[k].n, jobs[k].start, jobs[k].ev.data(), jobs[k].ev.size(),
                        [&](RawBurst&& r) { out.push_back({r.tSec, r.bb.size()}); });
            }
            if (gaps) *gaps = mx.gaps();
            return out;
        };
        const auto all = runMix(size_t(-1), nullptr);
        // the block holding the 4th burst's start (0.3 + 3 × 0.9 = 3.0 s)
        const size_t skip = size_t(3.0 * 2.4e6) / 76800;
        uint64_t gaps = 0;
        const auto holed = runMix(skip, &gaps);
        const double holeA = double(jobs[skip].start) / 2.4e6, holeB = holeA + 76800 / 2.4e6;
        size_t after = 0, afterSame = 0, inHole = 0;
        for (const auto& r : holed) if (r.first < holeB && r.first + double(r.second) / 250e3 > holeA) ++inHole;
        for (const auto& r : all) if (r.first >= holeB) { ++after; for (const auto& h : holed) if (h == r) { ++afterSame; break; } }
        CHECK(gaps == 1, "the mixer counted the hole (%llu)", (unsigned long long)gaps);
        CHECK(inHole == 0, "no channel was stitched across the hole (%zu)", inHole);
        CHECK(after > 0 && afterSame == after, "every burst after the hole unchanged (%zu of %zu)", afterSame, after);
        CHECK(holed.size() < all.size(), "the burst across the hole was abandoned (%zu → %zu)", all.size(), holed.size());
    }

    // 13. ★ HOGS ARE HELD, AND NO SENSOR IS LOST FOR IT (2026-10-31). Two wandering emitters (a carrier swept ±30 kHz at
    //     0.7 Hz and keyed on and off at random — what Stuart's Pi 500 capture holds near 433.27 and 434.82, and the floor
    //     cannot absorb because it moves between bins) for the whole 10 s, with sensors far from them, one standing 20 dB
    //     over a hog inside its hold, and one about as strong as a hog beside it. With the hold, the 1.6 s channels stop
    //     churning; everything decoded without the hold is decoded with it.
    printf(" carriers and spurs held\n");
    {
        auto wander = [](std::vector<cf>& x, double rate, double offHz, double amp, uint32_t seed) {
            std::mt19937 g(seed);
            std::uniform_real_distribution<double> onS(0.002, 0.015), offS(0.001, 0.010);
            double ph = 0; bool st = true; size_t next = size_t(onS(g) * rate);
            for (size_t i = 0; i < x.size(); ++i) {
                if (i >= next) { st = !st; next = i + size_t((st ? onS(g) : offS(g)) * rate); }
                const double f = offHz + 30e3 * std::sin(2 * M_PI * 0.7 * double(i) / rate);
                ph = std::fmod(ph + 2 * M_PI * f / rate, 2 * M_PI);
                if (st) x[i] += cf(float(amp * std::cos(ph)), float(amp * std::sin(ph)));
            }
        };
        const auto oreg = oregonV3(withChecksum(oregonNibbles("F824" "1" "22" "0" "571" "0" "45" "0"), 17));
        const auto wh = wh1080("a1820d5a0306004e08d3");
        std::vector<Sig> sigs;
        const double farHz[6] = {-300e3, -150e3, 100e3, 250e3, -380e3, 180e3};
        for (int k = 0; k < 6; ++k) { Sig sg{k % 2 ? wh : oreg}; sg.offHz = farHz[k]; sg.startS = 1.0 + 1.3 * k; sigs.push_back(sg); }
        Sig nearStrong{wh}; nearStrong.offHz = -590e3; nearStrong.startS = 5.2; sigs.push_back(nearStrong);           // 20 dB over the hog
        Sig nearWeak{oreg}; nearWeak.offHz = -560e3; nearWeak.startS = 7.4; nearWeak.amp = 0.04; sigs.push_back(nearWeak); // about the hog's level
        auto x = synth(sigs, 2.4e6, 10.0, 0.02, 31);
        wander(x, 2.4e6, -650e3, 0.03, 41);
        wander(x, 2.4e6, 500e3, 0.03, 42);
        struct Out { std::multiset<std::string> lines; uint64_t longs = 0, held = 0; size_t bursts = 0; bool nearStrong = false;
                     std::vector<std::string> around; };   // ★ the decoded bursts near the strong sensor, for the failure message
        auto run = [&](bool hold) {
            Out o;
            FrontEnd fe(2.4e6); fe.setDcGuardHz(15e3); fe.setHoldHogs(hold);
            auto on = [&](const Burst& b) {
                ++o.bursts;
                std::vector<Reading> r; Service::decodeBurst(b, 433.92e6, r);
                for (const auto& x : r) { o.lines.insert(readable(x, 0)); if (std::fabs(b.offHz - nearStrong.offHz) < FrontEnd::kMatchHz && b.tSec > 5.0 && b.tSec < 5.6) o.nearStrong = true; }
                // ★ kMatchHz, not tighter: a strong burst's first detection group can sit off its carrier (32-bit NEON put
                //   this one's channel at -483 kHz for a -590 kHz sensor — still its own channel, 160 kHz from the hog's)
                if (!r.empty() && b.tSec > 3.0 && b.tSec < 6.0) {
                    char d[200]; std::snprintf(d, sizeof d, "%.3f s %+.1f kHz: %s", b.tSec, b.offHz / 1e3, readable(r[0], 0).c_str()); o.around.push_back(d);
                }
            };
            for (size_t i = 0; i < x.size(); i += 76800) fe.feed(x.data() + i, std::min<size_t>(76800, x.size() - i), on);
            fe.flush(on);
            o.longs = fe.longCloses(); o.held = fe.heldHits();
            return o;
        };
        const Out off = run(false), on = run(true);
        size_t kept = 0;
        for (const auto& l : off.lines) kept += on.lines.count(l) ? 1 : 0;
        printf("   10 s, two wandering emitters: 1.6 s channels %llu → %llu, detections held %llu, bursts %zu → %zu, readings %zu → %zu\n",
               (unsigned long long)off.longs, (unsigned long long)on.longs, (unsigned long long)on.held, off.bursts, on.bursts,
               off.lines.size(), on.lines.size());
        CHECK(off.longs >= 8, "without the hold the emitters churn 1.6 s channels (%llu)", (unsigned long long)off.longs);
        CHECK(on.longs <= 4 && on.held > 0, "with it they are held: %llu 1.6 s channels, %llu detections held", (unsigned long long)on.longs, (unsigned long long)on.held);
        CHECK(kept == off.lines.size(), "every reading without the hold is there with it (%zu of %zu)", kept, off.lines.size());
        CHECK(on.nearStrong, "a sensor 20 dB over a held hog, inside its hold, still decodes");
        if (!on.nearStrong) for (const auto& d : on.around) printf("     decoded near it: %s\n", d.c_str());
        CHECK(on.lines.size() >= 6, "the six sensors away from the emitters decode (%zu readings)", on.lines.size());
    }

    testHome();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails ? 1 : 0;
}
