// test-wmbus.cpp — V12 Wireless M-Bus meters in ISM mode (vibe_wmbus.h, docs/v12/WMBUS-DECODER.md §7).
//
// ★★ From the PUBLISHED facts, independently of the decoder: the frames, CRCs, 3-out-of-6 words, preambles and syncs
//    below are built by an ENCODER written here from the standard's tables (EN 13757-4 as given by TI AN067 and the
//    Infineon TDA5340 note), not by inverting the decoder. Then: the design's worked frames byte for byte; every field;
//    formats A and B; the headers; the data records; the readable line; the interval; T1, C1-A, C1-B and S1 modulated into
//    noisy IQ at the rates the radios run and decoded through the REAL ISM front end; a sensitivity sweep; pure noise and a
//    carrier decode nothing; the PRIVACY rule (serial yes, encrypted bytes never) in the JSON, the day log and the card;
//    the day-log throttle; a busy street (report size, the cap, CPU).
// ★ Silent: no audio anywhere.
//   g++ -std=c++17 -O2 -I android/app/src/main/cpp -o /tmp/test-wmbus vibeserver/test-wmbus.cpp -lpthread
//   test-wmbus --write-clip <out.u8> <radio centre Hz> <rate>   — a meter clip for scripts/test-server-ism.mjs
//   VIBE_WMBUS_REC=<file.u8> also replays a real 868.6 MHz capture at 2.4 MS/s and requires zero meters from it
#include "vibe_ism_service.h"
#include "vibe_ism_gallery.h"

#include <dirent.h>
#include <sys/stat.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace vibeism;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)
using Bytes = std::vector<uint8_t>;

static Bytes hexBytes(const char* h) {
    Bytes o; for (const char* p = h; *p;) { while (*p == ' ') ++p; if (!p[0] || !p[1]) break; unsigned v; std::sscanf(p, "%2x", &v); o.push_back(uint8_t(v)); p += 2; }
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE ENCODER (from the standard's tables — written separately from vibe_wmbus.h)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** CRC-16/EN-13757 as a bit-serial shift register, one input bit at a time. */
static uint16_t crcRef(const uint8_t* d, size_t n) {
    uint16_t r = 0;
    for (size_t i = 0; i < n; ++i)
        for (int b = 7; b >= 0; --b) { const int in = (d[i] >> b) & 1, top = r >> 15; r = uint16_t(r << 1); if (top ^ in) r ^= 0x3D65; }
    return uint16_t(r ^ 0xFFFF);
}
static void putCrc(Bytes& o, const uint8_t* d, size_t n) { const uint16_t c = crcRef(d, n); o.push_back(uint8_t(c >> 8)); o.push_back(uint8_t(c)); }
/** Format A from the frame's bytes without CRCs (L first): block 1 = 10 bytes, then 16-byte blocks, each with its CRC. */
static Bytes frameA(const Bytes& f) {
    Bytes o; size_t p = 0, blk = 10;
    while (p < f.size()) { blk = std::min(blk, f.size() - p); o.insert(o.end(), f.begin() + long(p), f.begin() + long(p + blk)); putCrc(o, &f[p], blk); p += blk; blk = 16; }
    return o;
}
/** Format B: one CRC over blocks 1+2 at frame byte 126 when the frame is longer than 128, else at the end. */
static Bytes frameB(Bytes f) {
    Bytes o;
    const size_t body = f.size();                     // L … end, no CRCs
    const bool three = body + 2 > 128;
    f[0] = uint8_t(body - 1 + (three ? 4 : 2));
    if (!three) { o = f; putCrc(o, o.data(), o.size()); return o; }
    o.assign(f.begin(), f.begin() + 126); putCrc(o, o.data(), 126);
    const size_t at = o.size();
    o.insert(o.end(), f.begin() + 126, f.end()); putCrc(o, &o[at], o.size() - at);
    return o;
}
/** Bytes → the frame without CRCs, L set (format A counts everything after L, no CRCs). */
static Bytes withL(Bytes body) { body.insert(body.begin(), uint8_t(body.size())); return body; }

// 3-out-of-6, typed from the standard's table (EN 13757-4; TI SWRA234 agrees): nibble → 6 chips
static const char* const kWords[16] = {"010110", "001101", "001110", "001011", "011100", "011001", "011010", "010011",
                                       "101100", "100101", "100110", "100011", "110100", "110001", "110010", "101001"};
using Chips = std::vector<uint8_t>;
static void put(Chips& c, const char* s) { for (; *s; ++s) if (*s == '0' || *s == '1') c.push_back(uint8_t(*s - '0')); }
static void putBits(Chips& c, uint32_t v, int n) { for (int i = n - 1; i >= 0; --i) c.push_back((v >> i) & 1); }
static Chips chipsT(const Bytes& air, int nPre = 24) {
    Chips c; for (int i = 0; i < nPre; ++i) put(c, "01");
    put(c, "0000111101");
    for (uint8_t b : air) { put(c, kWords[b >> 4]); put(c, kWords[b & 15]); }
    put(c, "0101");
    return c;
}
static Chips chipsC(const Bytes& air, bool frameB) {
    Chips c; for (int i = 0; i < 16; ++i) put(c, "01");
    put(c, "0101010000111101"); put(c, frameB ? "0101010000111101" : "0101010011001101");
    for (uint8_t b : air) putBits(c, b, 8);
    put(c, "0101");
    return c;
}
/** S: Manchester, `oneIsHiLo` = a 1 is sent as chips 10. */
static Chips chipsS(const Bytes& air, int nPre = 279, bool oneIsHiLo = false) {
    Chips c; for (int i = 0; i < nPre; ++i) put(c, "01");
    put(c, "000111011010010110");
    for (uint8_t b : air) for (int i = 7; i >= 0; --i) { const bool one = (b >> i) & 1; put(c, one == oneIsHiLo ? "10" : "01"); }
    put(c, "0101");
    return c;
}

// ── IQ: continuous-phase 2-FSK, chip 1 = the upper tone (or the lower, `invert`) ─────────────────────────────────────
struct Tx { Chips chips; double chipHz = 100e3, offHz = 350e3, devHz = 50e3, amp = 0.3, startS = 0.05; bool invert = false; };
static std::vector<cf> synth(const std::vector<Tx>& txs, double rate, double lenS, double noise, uint32_t seed) {
    std::vector<cf> x(size_t(rate * lenS));
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0, float(noise));
    for (auto& v : x) v = cf(g(rng), g(rng));
    for (const Tx& t : txs) {
        double ph = 0;
        const size_t a = size_t(t.startS * rate), n = size_t(double(t.chips.size()) / t.chipHz * rate);
        for (size_t i = 0; i < n && a + i < x.size(); ++i) {
            const size_t k = std::min(t.chips.size() - 1, size_t(double(i) / rate * t.chipHz));
            const double f = t.offHz + ((t.chips[k] != 0) != t.invert ? t.devHz : -t.devHz);
            ph += 2 * M_PI * f / rate;
            x[a + i] += cf(float(t.amp * std::cos(ph)), float(t.amp * std::sin(ph)));
        }
    }
    return x;
}
/** Through the real front end, as the service sets it up on the 868 band (radio at 868.6 MHz). */
static std::vector<Reading> runFront(const std::vector<cf>& x, double rate, wmbus::Stats* st = nullptr, double hw = 868.6e6, size_t* nb = nullptr) {
    FrontEnd fe(rate);
    fe.setDcGuardHz(15e3);
    const Band* band = bandById("868");
    const double mid = band->centreHz - hw, half = band->needHz + 40e3;
    fe.setUsableHz(mid - half, mid + half);
    fe.setChanRate(500e3); fe.setKeepBaseband(true);
    std::vector<Reading> out;
    size_t bursts = 0;
    auto on = [&](const Burst& b) { ++bursts; Service::decodeBurst(b, hw, out, st); };
    for (size_t i = 0; i < x.size(); i += 16384) fe.feed(x.data() + i, std::min<size_t>(16384, x.size() - i), on);
    fe.flush(on);
    if (nb) *nb = bursts;
    return out;
}
static const Reading* findMeter(const std::vector<Reading>& v, const std::string& id) {
    for (const auto& r : v) if (r.kind == "meter" && r.meter && r.meter->id == id) return &r;
    return nullptr;
}

// ── the worked frames ────────────────────────────────────────────────────────────────────────────────────────────────
// The design's §2.3 frames, byte for byte (CRCs included as printed there)
static const char* kSensusA = "2E 44 AE 4C 78 56 34 12 01 07 9C F2 7A 2A 00 20 05 A0 A1 A2 A3 A4 A5 A6 A7 A8 A9 AA 5B 94 "
                              "AB AC AD AE AF B0 B1 B2 B3 B4 B5 B6 B7 B8 B9 BA 2F 4B BB BC BD BE BF 6F 56";
static const char* kKamDocA = "28 44 2D 2C 21 43 65 87 1B 16 ED 2F 7A 0B 00 00 00 0C 13 56 34 12 00 0B 3B 12 00 00 C6 5A "
                              "02 FD 17 00 00 42 6C 40 39 4C 13 11 01 12 00 16 A1";
static const char* kKamDocB = "2A 44 2D 2C 21 43 65 87 1B 16 7A 0B 00 00 00 0C 13 56 34 12 00 0B 3B 12 00 00 02 FD 17 00 00 42 "
                              "6C 40 39 4C 13 11 01 12 00 61 2F";
/** ★ The Kamstrup frame as the design MEANT it: its date bytes "40 39" are day 0 (type G: day = bits 0-4); 30 Sep 2026
 *  is 0x395E → "5E 39". Same records otherwise; CRCs made here. */
static Bytes docKamBody() { return withL(hexBytes("44 2D 2C 21 43 65 87 1B 16 7A 0B 00 00 00 0C 13 56 34 12 00 0B 3B 12 00 00 02 FD 17 00 00 42 6C 40 39 4C 13 11 01 12 00")); }
static Bytes kamBody() { return withL(hexBytes("44 2D 2C 21 43 65 87 1B 16 7A 0B 00 00 00 0C 13 56 34 12 00 0B 3B 12 00 00 02 FD 17 00 00 42 6C 5E 39 4C 13 11 01 12 00")); }
static Bytes sensusBody(int ver = 0x01, int sts = 0x00, uint8_t acc = 0x2A) {
    Bytes b = hexBytes("44 AE 4C 78 56 34 12 00 07 7A 00 00 20 05");
    b[7] = uint8_t(ver); b[10] = acc; b[11] = uint8_t(sts);
    for (int i = 0; i < 32; ++i) b.push_back(uint8_t(0xA0 + i));
    return withL(b);
}

static std::string tmpDir() {
    char t[256]; std::snprintf(t, sizeof t, "%s/wmbus-XXXXXX", std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp");
    return mkdtemp(t) ? std::string(t) : std::string("/tmp");
}
static std::string slurpDir(const std::string& dir) {
    std::string all;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n[0] == '.') continue; std::ifstream f(dir + "/" + n, std::ios::binary); std::stringstream s; s << f.rdbuf(); all += s.str() + "\n"; }
        closedir(d);
    }
    return all;
}
static void rmDir(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) { while (dirent* e = readdir(d)) { std::string n = e->d_name; if (n[0] != '.') std::remove((dir + "/" + n).c_str()); } closedir(d); }
    rmdir(dir.c_str());
}
static int countOf(const std::string& s, const std::string& w) { int n = 0; for (size_t p = 0; (p = s.find(w, p)) != std::string::npos; p += w.size()) ++n; return n; }

/** A reading as decodeBurst builds it, straight from frame bytes (no radio). */
static bool readingOf(const Bytes& air, bool fmtB, Reading& r, const char* wm = "T1") {
    wmbus::Meter m;
    if (wmbus::fromBytes(air, fmtB, wm, m) != 1) return false;
    r = Reading(); r.kind = "meter";
    const char* mk = wmbus::mfrName(m.mfr); r.make = mk ? mk : m.mfr; r.model = wmbus::typeText(m.type); r.pubId = m.id;
    r.freqHz = 868.95e6; r.levelDb = -20; r.snrDb = 24;
    r.meter = std::make_shared<const wmbus::Meter>(m);
    return true;
}

static int writeClip(const char* out, double centre, double rate) {
    // a Sensus (encrypted) as T1 three times 16 s apart is too long a clip; twice 1.2 s apart, then a Kamstrup as C1-A
    std::vector<Tx> txs;
    Tx a; a.chips = chipsT(frameA(sensusBody(0x68, 0, 0x2A))); a.offHz = 868.95e6 - centre; a.startS = 0.3; txs.push_back(a);
    Tx k; k.chips = chipsC(frameA(kamBody()), false); k.offHz = 868.95e6 - centre + 10e3; k.devHz = 45e3; k.startS = 1.0; txs.push_back(k);
    Tx a2 = a; a2.chips = chipsT(frameA(sensusBody(0x68, 0, 0x2B))); a2.startS = 1.8; txs.push_back(a2);
    const auto x = synth(txs, rate, 2.6, 0.02, 99);
    FILE* f = std::fopen(out, "wb");
    if (!f) return 1;
    std::vector<uint8_t> u(x.size() * 2);
    for (size_t i = 0; i < x.size(); ++i) {
        u[2 * i] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].real() * 127.5f + 127.5f))));
        u[2 * i + 1] = uint8_t(std::max(0.0f, std::min(255.0f, std::round(x[i].imag() * 127.5f + 127.5f))));
    }
    std::fwrite(u.data(), 1, u.size(), f);
    std::fclose(f);
    printf("wrote %zu samples (%.1f s) centred on %.3f MHz\n", x.size(), x.size() / rate, centre / 1e6);
    return 0;
}

/** ★ --diag-c1 <A|B> <ppm> <rate factor> <0|1 inverted> <seed>: ONE C1 frame of the "C1 A/B over ±60 ppm" check, rendered
 *  exactly as that check renders it, and every burst the front end makes of it (where its channel sat, what the slicer
 *  measured, whether the meter decoded) — for a frame one machine misses and another does not (2026-10-31). */
static int diagC1(bool b, double ppm, double rf, bool inv, uint32_t seed) {
    Tx t; t.chips = chipsC(b ? frameB(kamBody()) : frameA(kamBody()), b); t.devHz = 45e3;
    t.offHz += ppm * 868.95; t.chipHz *= rf; t.invert = inv;
    const auto x = synth({t}, 2.4e6, 0.35, 0.02, seed);
    FrontEnd fe(2.4e6);
    fe.setDcGuardHz(15e3);
    const Band* band = bandById("868");
    const double mid = band->centreHz - 868.6e6, half = band->needHz + 40e3;
    fe.setUsableHz(mid - half, mid + half);
    fe.setChanRate(500e3); fe.setKeepBaseband(true);
    int found = 0;
    auto on = [&](const Burst& bu) {
        std::vector<Reading> r; wmbus::Stats st; Service::decodeBurst(bu, 868.6e6, r, &st);
        const bool m = findMeter(r, "87654321") != nullptr; found += m;
        printf("  burst %.4f s  channel %+.1f kHz (the frame at %+.1f)  carrier %+.1f kHz  dev %.1f kHz  %zu samples  tried %llu  %s\n",
               bu.tSec, bu.offHz / 1e3, t.offHz / 1e3, bu.carrierHz / 1e3, bu.fskDevHz / 1e3, bu.bb ? bu.bb->size() : size_t(0),
               (unsigned long long)st.tried, m ? "METER" : "-");
    };
    for (size_t i = 0; i < x.size(); i += 16384) fe.feed(x.data() + i, std::min<size_t>(16384, x.size() - i), on);
    fe.flush(on);
    printf("C1-%c %+.0f ppm ×%.2f %s seed %u (hop %.0f µs): %s\n", b ? 'B' : 'A', ppm, rf, inv ? "inverted" : "normal", seed,
           FrontEnd::kHopUs, found ? "decoded" : "MISSED");
    return found ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--write-clip") return writeClip(argv[2], std::atof(argv[3]), std::atof(argv[4]));
    if (argc == 7 && std::string(argv[1]) == "--diag-c1")
        return diagC1(argv[2][0] == 'B', std::atof(argv[3]), std::atof(argv[4]), std::atoi(argv[5]) != 0, uint32_t(std::atoi(argv[6])));
    printf("test-wmbus — Wireless M-Bus meters in ISM mode\n");

    printf(" CRC and the design's worked frames\n");
    {
        const char* nine = "123456789";
        CHECK(wmbus::crc(reinterpret_cast<const uint8_t*>(nine), 9) == 0xC2B7, "CRC check value %04X", wmbus::crc(reinterpret_cast<const uint8_t*>(nine), 9));
        CHECK(crcRef(reinterpret_cast<const uint8_t*>(nine), 9) == 0xC2B7, "the test's own CRC");
        const Bytes s = hexBytes(kSensusA), k = hexBytes(kKamDocA), kb = hexBytes(kKamDocB);
        CHECK(frameA(Bytes(sensusBody())) == s, "the Sensus frame encodes to the design's bytes");
        Bytes f; size_t good = 0;
        CHECK(wmbus::frameA(s.data(), s.size(), f, good) && good == f.size() && f.size() == 0x2F, "Sensus: every block's CRC (%zu)", good);
        CHECK(wmbus::frameA(k.data(), k.size(), f, good) && f.size() == 0x29, "Kamstrup (design, format A): every block's CRC");
        CHECK(wmbus::frameB(kb.data(), kb.size(), f) && f.size() == 0x29, "Kamstrup (design, format B): the CRC");
        Reading r;
        CHECK(readingOf(s, false, r) && wmbus::coreLine(*r.meter) == "Water meter \xc2\xb7 Sensus \xc2\xb7 12345678 \xc2\xb7 v0x01 \xc2\xb7 encrypted (AES)",
              "Sensus line: %s", r.meter ? wmbus::coreLine(*r.meter).c_str() : "-");
        CHECK(r.meter && r.meter->acc == 42 && r.meter->encBlocks == 2 && r.meter->sec == 5 && r.meter->recs.empty(), "Sensus: ACC 42, 2 blocks, mode 5");
        // ★ the design's Kamstrup has day 0 in its type-G date: the date is refused, the stored value still shown
        CHECK(readingOf(k, false, r), "Kamstrup (design) decodes");
        CHECK(wmbus::coreLine(*r.meter).find("stored 120.111 m\xc2\xb3") != std::string::npos, "Kamstrup (design) day 0 date not used: %s", wmbus::coreLine(*r.meter).c_str());
    }

    printf(" fields\n");
    {
        CHECK(wmbus::mfrLetters(0x4CAE) == "SEN" && wmbus::mfrLetters(0x2C2D) == "KAM", "M-field letters");
        CHECK(wmbus::mfrLetters(0x4CAE | 0x8000) == "SEN", "bit 15 set still decodes");
        const uint16_t kam = uint16_t(('K' - 64) * 1024 + ('A' - 64) * 32 + ('M' - 64));
        CHECK(kam == 0x2C2D, "KAM packs to 0x2C2D");
        const uint16_t xyz = uint16_t(('X' - 64) * 1024 + ('Y' - 64) * 32 + ('Z' - 64));
        CHECK(wmbus::mfrLetters(xyz) == "XYZ" && wmbus::mfrName("XYZ") == nullptr, "an unknown code prints its letters");
        const uint8_t id[4] = {0x78, 0x56, 0x34, 0x12};
        CHECK(wmbus::idText(id) == "12345678", "ID LSB first");
        const uint8_t id2[4] = {0x78, 0x56, 0x3C, 0x12};
        CHECK(wmbus::idText(id2) == "123C5678", "a non-BCD nibble printed as hex");
        CHECK(wmbus::typeText(0x07) == "Water meter" && wmbus::typeText(0x16) == "Cold water meter" && wmbus::typeText(0x99) == "Meter (type 0x99)", "device types");
    }

    printf(" 3-out-of-6\n");
    {
        const int8_t* dec = wmbus::decode36Table();
        int valid = 0;
        for (int i = 0; i < 16; ++i) {
            const uint8_t c = wmbus::kCode36[i];
            int ones = 0; for (int b = 0; b < 6; ++b) ones += (c >> b) & 1;
            CHECK(ones == 3, "word %X has %d ones", i, ones);
            uint8_t typed = 0; for (int b = 0; b < 6; ++b) typed = uint8_t(typed << 1 | (kWords[i][b] - '0'));
            CHECK(typed == c, "nibble %X: the decoder's word %02X, the standard's %02X", i, c, typed);
            for (int j = 0; j < i; ++j) CHECK(wmbus::kCode36[j] != c, "words %X and %X equal", i, j);
        }
        for (int w = 0; w < 64; ++w) if (dec[w] >= 0) { ++valid; CHECK(wmbus::kCode36[dec[w]] == w, "inverse of %02X", w); }
        CHECK(valid == 16, "16 valid words (%d)", valid);
        CHECK(dec[0x15] < 0 && dec[0x00] < 0 && dec[0x3F] < 0, "invalid words are code errors");
        // round trip of all 256 bytes through the encoder's chips and the decoder's soft ML word decision
        Chips c; for (int b = 0; b < 256; ++b) { put(c, kWords[b >> 4]); put(c, kWords[b & 15]); }
        std::vector<float> s; for (uint8_t x : c) s.push_back(x ? 1.0f : -1.0f);
        // bytesT stops after the L-field's frame length: feed L = 0xFF first so it reads on
        std::vector<float> s2;
        Chips lc; put(lc, kWords[0xF]); put(lc, kWords[0xF]);
        for (uint8_t x : lc) s2.push_back(x ? 1.0f : -1.0f);
        s2.insert(s2.end(), s.begin(), s.end());
        const Bytes got = wmbus::bytesT(s2, 0, 1, 300);
        bool all = got.size() >= 257; for (int b = 0; all && b < 256; ++b) all = got[size_t(b) + 1] == b;
        CHECK(all, "all 256 bytes round trip (%zu)", got.size());
        // one chip flipped in every word still decodes to the right nibble most of the time — soft ML at work
        int okFlip = 0;
        for (int b = 0; b < 16; ++b) { std::vector<float> w; Chips one; put(one, kWords[0xF]); put(one, kWords[0xF]); put(one, kWords[b]); put(one, kWords[b]);
            for (size_t i = 0; i < one.size(); ++i) w.push_back((one[i] ? 1.0f : -1.0f) * (i == 14 ? -0.3f : 1.0f));
            const Bytes g = wmbus::bytesT(w, 0, 1, 2); if (g.size() == 2 && g[1] == uint8_t(b * 17)) ++okFlip; }
        CHECK(okFlip == 16, "a weakly wrong chip is outvoted (%d/16)", okFlip);
    }

    printf(" format A and B\n");
    {
        Bytes f; size_t good = 0;
        Reading r;
        const Bytes h9 = frameA(withL(hexBytes("44 AE 4C 78 56 34 12 01 07")));
        CHECK(h9.size() == 12 && wmbus::frameA(h9.data(), h9.size(), f, good) && f.size() == 10, "L = 9: header only");
        CHECK(readingOf(h9, false, r) && !r.meter->damaged && r.meter->ci < 0, "L = 9 is a meter, not damaged");
        const Bytes h10 = frameA(withL(hexBytes("44 AE 4C 78 56 34 12 01 07 78")));
        CHECK(wmbus::frameA(h10.data(), h10.size(), f, good) && f.size() == 11, "L = 10: CI only");
        Bytes b41; for (int i = 0; i < 31; ++i) b41.push_back(uint8_t(i));
        Bytes body41 = hexBytes("44 AE 4C 78 56 34 12 01 07 78"); body41.insert(body41.end(), b41.begin(), b41.end());
        body41.resize(41 - 0); const Bytes f41 = frameA(withL(body41));
        CHECK(f41[0] == 41 && (41 - 9) % 16 == 0 && f41.size() == wmbus::lenA(41) && wmbus::frameA(f41.data(), f41.size(), f, good), "L = 41: (L−9) mod 16 = 0");
        // a corrupted block 3 → heard (damaged), a corrupted block 1 → nothing
        Bytes bad3 = frameA(kamBody()); bad3[34] ^= 0x10;
        CHECK(readingOf(bad3, false, r) && r.meter->damaged && r.meter->recs.empty() && r.meter->acc == 0x0B, "block 3 bad: heard, damaged, no readings");
        Bytes bad1 = frameA(kamBody()); bad1[5] ^= 0x01;
        CHECK(!readingOf(bad1, false, r), "block 1 bad: nothing");
        // format B, short and long
        const Bytes fb = frameB(kamBody());
        CHECK(fb.size() == size_t(fb[0]) + 1 && wmbus::frameB(fb.data(), fb.size(), f) && f.size() == kamBody().size(), "format B, one CRC");
        CHECK(frameB(docKamBody()) == hexBytes(kKamDocB) && frameA(docKamBody()) == hexBytes(kKamDocA), "the design's format A and B bytes, built here");
        Bytes longBody = hexBytes("44 2D 2C 21 43 65 87 1B 16 78");
        for (int i = 0; i < 70; ++i) { longBody.push_back(0x0C); longBody.push_back(0x13); longBody.push_back(0x11); longBody.push_back(0x22); }
        longBody.resize(150); longBody = withL(longBody);
        const Bytes fbl = frameB(longBody);
        CHECK(fbl[0] > 127 && wmbus::frameB(fbl.data(), fbl.size(), f) && f.size() == longBody.size(), "format B, L > 127: two CRCs (L %d)", fbl[0]);
        Bytes fbBad = fb; fbBad[3] ^= 0x40;
        CHECK(!wmbus::frameB(fbBad.data(), fbBad.size(), f), "format B: a corrupted byte in block 1 fails the frame");
        CHECK(readingOf(fb, true, r, "C1-B") && r.meter->wm == "C1-B", "format B reading");
    }

    printf(" headers\n");
    {
        Reading r;
        auto line = [&](const Bytes& body) { Reading q; return readingOf(frameA(withL(body)), false, q) ? wmbus::coreLine(*q.meter) : std::string("-"); };
        // CI 0x72: the link layer is a repeater, the meter is the long header's
        Bytes l72 = hexBytes("44 2D 2C 23 01 12 00 01 36 72 78 56 34 12 AE 4C 68 07 05 00 00 05");
        for (int i = 0; i < 16; ++i) l72.push_back(0x55);
        CHECK(readingOf(frameA(withL(l72)), false, r) && r.meter->id == "12345678" && r.meter->mfr == "SEN" && r.meter->via == "KAM 00120123",
              "CI 0x72 via a repeater: %s", wmbus::coreLine(*r.meter).c_str());
        CHECK(line(l72) == "Water meter \xc2\xb7 Sensus \xc2\xb7 12345678 \xc2\xb7 v0x68 \xc2\xb7 encrypted (AES) \xc2\xb7 via KAM 00120123", "0x72 line: %s", line(l72).c_str());
        CHECK(line(hexBytes("44 2D 2C 21 43 65 87 1B 16 78 0C 13 56 34 12 00")).find("123.456 m\xc2\xb3") != std::string::npos, "CI 0x78: records at once");
        CHECK(line(hexBytes("44 2D 2C 21 43 65 87 1B 16 8C 20 07 7A 07 00 00 00 0C 13 56 34 12 00")).find("123.456") != std::string::npos, "0x8C + 0x7A");
        const std::string ell = line(hexBytes("44 2D 2C 21 43 65 87 1B 16 8D 20 07 01 00 00 20 11 22 33 44 55 66 77 88"));
        CHECK(ell.find("encrypted (AES, link layer)") != std::string::npos, "ELL-encrypted 0x8D: %s", ell.c_str());
        const std::string mf = line(hexBytes("44 49 6A 26 59 41 31 01 00 A2 11 22 33"));
        CHECK(mf == "Meter \xc2\xb7 Zenner \xc2\xb7 31415926 \xc2\xb7 v0x01 \xc2\xb7 manufacturer format 0xA2", "0xA2: %s", mf.c_str());
        const int modes[4] = {0, 5, 7, 13};
        const char* txt[4] = {"not encrypted", "encrypted (AES)", "encrypted (AES, KDF)", "encrypted (TLS)"};
        for (int i = 0; i < 4; ++i) {
            Bytes b = hexBytes("44 AE 4C 78 56 34 12 68 07 7A 01 00 00 00");
            b[13] = uint8_t(modes[i]); b[12] = modes[i] == 5 || modes[i] == 7 ? 0x10 : 0;
            if (modes[i] == 7 || modes[i] == 13) b.push_back(0x00);
            for (int k = 0; k < 16; ++k) b.push_back(modes[i] ? uint8_t(0xC0 + k) : 0x2F);
            CHECK(line(b).find(txt[i]) != std::string::npos, "mode %d: %s", modes[i], line(b).c_str());
        }
        // a collector talking to a meter (C 0x53) is counted, not listed
        wmbus::Meter m;
        CHECK(wmbus::fromBytes(frameA(withL(hexBytes("53 AE 4C 78 56 34 12 68 07"))), false, "T1", m) == 2, "a collector's frame = 2");
        // status words
        CHECK(wmbus::statusText(0x04) == "battery low" && wmbus::statusText(0x08) == "permanent error" && wmbus::statusText(0x03) == "alarm", "STS words");
    }

    printf(" data records\n");
    {
        std::vector<wmbus::Record> rec;
        const Bytes p = hexBytes(
            "2F 2F "                    // fillers
            "04 06 10 27 00 00 "        // energy, 32-bit int 10000 × 10^(6-3) Wh = 10 000 kWh
            "04 0E 40 42 0F 00 "        // energy, J × 10^6 = 1 000 000 MJ → 1000 GJ
            "0C 13 56 34 12 00 "        // volume 8-digit BCD 123.456 m³
            "02 1B E8 03 "              // mass 1000 kg
            "02 22 10 00 "              // on time 16 h
            "02 27 05 00 "              // operating time 5 days
            "02 2B 2C 01 "              // power 300 W
            "02 33 02 00 "              // power J/h × 10^3
            "0B 3B 12 00 00 "           // flow 0.012 m³/h
            "02 43 01 00 "              // m³/min
            "02 4D 01 00 "              // m³/s
            "02 53 01 00 "              // kg/h
            "02 5A DC 05 "              // flow temperature 150.0 °C (×0.1)
            "02 5E 2C 01 "              // return temperature 30.0 °C
            "02 62 32 00 "              // ΔT 5.0 K
            "02 66 C8 00 "              // external 20.0 °C
            "02 6A 0A 00 "              // pressure 1.0 bar (×0.1)
            "42 6C 5E 39 "              // storage 1 date G 2026-09-30
            "04 6D 1E 0C 5E 39 "        // date+time F 2026-09-30 12:30
            "02 6E 07 00 "              // HCA 7
            "02 74 3C 00 "              // actuality 60 s
            "0C 78 21 43 65 87 "        // fabrication number 87654321
            "0A 93 01 11 22 "           // volume with a VIFE (qualified)
            "0C 13 99 99 99 F9 "        // negative BCD → −9 999 999 → −9 999.999 m³ … top nibble F = negative
            "02 FD 17 04 00 "           // error flags 0x0004
            "02 FD 74 C8 00 "           // battery 200 days
            "01 FF 05 33 "              // manufacturer-specific: skipped
            "0F 01 02 03");             // manufacturer data to the end: stop
        CHECK(wmbus::parseRecords(p.data(), p.size(), rec), "records parse to the end");
        auto find = [&](const char* q, int st = 0, bool qual = false) -> const wmbus::Record* { for (auto& r : rec) if (r.q == q && r.st == st && r.qualified == qual) return &r; return nullptr; };
        auto near = [](double a, double b) { return std::fabs(a - b) <= 1e-6 * std::max(1.0, std::fabs(b)); };
        const auto* e1 = find("energy");
        CHECK(e1 && e1->u == "kWh" && near(e1->v, 10000), "energy Wh → kWh %.3f", e1 ? e1->v : -1);
        int gj = 0; for (auto& r : rec) if (r.q == "energy" && r.u == "GJ" && near(r.v, 1000)) ++gj;
        CHECK(gj == 1, "energy J → GJ");
        int vol = 0; for (auto& r : rec) if (r.q == "volume" && !r.qualified && near(r.v, 123.456)) ++vol;
        CHECK(vol == 1, "volume 8-digit BCD");
        CHECK(find("mass") && near(find("mass")->v, 1000), "mass");
        CHECK(find("on time") && find("on time")->u == "h" && near(find("on time")->v, 16), "on time");
        CHECK(find("operating time") && find("operating time")->u == "days", "operating time");
        int pw = 0; for (auto& r : rec) if (r.q == "power" && ((r.u == "W" && near(r.v, 300)) || (r.u == "J/h" && near(r.v, 2000)))) ++pw;
        CHECK(pw == 2, "power W and J/h (%d)", pw);
        int fl = 0; for (auto& r : rec) if (r.q == "flow" && ((r.u == "m\xc2\xb3/h" && near(r.v, 0.012)) || (r.u == "m\xc2\xb3/min" && near(r.v, 1e-4)) || (r.u == "m\xc2\xb3/s" && near(r.v, 1e-4)))) ++fl;
        CHECK(fl == 3, "flow m³/h, m³/min, m³/s (%d)", fl);
        CHECK(find("mass flow") && near(find("mass flow")->v, 1), "mass flow");
        CHECK(find("flow temperature") && near(find("flow temperature")->v, 150.0), "flow temperature %.2f", find("flow temperature") ? find("flow temperature")->v : -1);
        CHECK(find("return temperature") && near(find("return temperature")->v, 30.0), "return temperature");
        CHECK(find("temperature difference") && near(find("temperature difference")->v, 5.0), "temperature difference");
        CHECK(find("external temperature") && near(find("external temperature")->v, 20.0), "external temperature");
        CHECK(find("pressure") && near(find("pressure")->v, 1.0), "pressure");
        int dates = 0; for (auto& r : rec) if (r.q == "date" && (r.t == "2026-09-30" || r.t == "2026-09-30 12:30")) ++dates;
        CHECK(dates == 2, "a type G and a type F date (%d)", dates);
        CHECK(find("HCA units") && near(find("HCA units")->v, 7), "HCA units");
        CHECK(find("actuality") && find("actuality")->u == "s", "actuality duration");
        CHECK(find("fabrication number") && near(find("fabrication number")->v, 87654321), "fabrication number");
        CHECK(find("volume", 0, true), "a VIFE makes a record qualified");
        int neg = 0; for (auto& r : rec) if (r.q == "volume" && near(r.v, -9999.999)) ++neg;
        CHECK(neg == 1, "negative BCD");
        CHECK(find("error flags") && near(find("error flags")->v, 4), "error flags");
        CHECK(find("battery") && near(find("battery")->v, 200), "remaining battery");
        CHECK(rec.size() == 26, "every record and nothing from manufacturer data (%zu)", rec.size());
        // the line keeps a qualified volume out
        wmbus::Meter m; m.type = 0x07; m.sec = 0; m.mfr = "KAM"; m.id = "1"; m.recs = rec;
        const auto words = wmbus::readings(m);
        std::string all; for (auto& w : words) all += w + "|";
        CHECK(all == "123.456 m\xc2\xb3|flow 0.012 m\xc2\xb3/h|alarm 0x0004|", "the line's readings: %s", all.c_str());
        // 32-bit integer volume
        rec.clear(); const Bytes p2 = hexBytes("04 13 40 E2 01 00");
        CHECK(wmbus::parseRecords(p2.data(), p2.size(), rec) && rec.size() == 1 && near(rec[0].v, 123.456), "32-bit int volume");
        rec.clear(); const Bytes p3 = hexBytes("0C 13 5A 34 12 00");
        wmbus::parseRecords(p3.data(), p3.size(), rec);
        CHECK(rec.size() == 1 && rec[0].qualified && rec[0].v != rec[0].v, "a non-BCD digit is not a value");
    }

    printf(" the readable line\n");
    {
        Reading r; Tracker tr;
        auto acceptAt = [&](const Bytes& air, double t, float snr) { Reading q; readingOf(air, false, q); q.snrDb = snr; return tr.accept(q, t); };
        // Sensus v0x68, 16 s apart
        acceptAt(frameA(sensusBody(0x68, 0, 10)), 100, 24);
        acceptAt(frameA(sensusBody(0x68, 0, 11)), 116, 24);
        std::string j = tr.json(117);
        CHECK(j.find("\"line\":\"Water meter \xc2\xb7 Sensus \xc2\xb7 12345678 \xc2\xb7 v0x68 \xc2\xb7 encrypted (AES) \xc2\xb7 every 16 s \xc2\xb7 signal 24 dB\"") != std::string::npos,
              "example 1: %s", j.c_str());
        CHECK(j.find("\"name\":\"Water meter \xc2\xb7 Sensus \xc2\xb7 12345678\"") != std::string::npos && j.find("\"extra\":\"v0x68 \xc2\xb7 encrypted (AES) \xc2\xb7 every 16 s\"") != std::string::npos,
              "old clients: name + extra");
        CHECK(j.find("\"id\":\"12345678\"") != std::string::npos && j.find("\"kind\":\"meter\"") != std::string::npos && j.find("\"wm\":\"T1\"") != std::string::npos
              && j.find("\"sec\":5") != std::string::npos && j.find("\"encBlocks\":2") != std::string::npos, "meter JSON fields");
        acceptAt(frameA(sensusBody(0x68, 0x04, 12)), 132, 6);
        j = tr.json(133);
        CHECK(j.find("v0x68 \xc2\xb7 encrypted (AES) \xc2\xb7 battery low \xc2\xb7 every 16 s \xc2\xb7 signal 6 dB") != std::string::npos && j.find("\"batt\":0") != std::string::npos,
              "example 2 (battery low): %s", j.c_str());
        Tracker t2; Reading k; readingOf(frameA(kamBody()), false, k); k.snrDb = 18; t2.accept(k, 10);
        Bytes kb2 = kamBody(); kb2[11] = 0x0C; Reading k2; readingOf(frameA(kb2), false, k2); k2.snrDb = 18; t2.accept(k2, 26);
        const std::string kj = t2.json(26);
        CHECK(kj.find("Cold water meter \xc2\xb7 Kamstrup \xc2\xb7 87654321 \xc2\xb7 v0x1B \xc2\xb7 123.456 m\xc2\xb3 \xc2\xb7 flow 0.012 m\xc2\xb3/h \xc2\xb7 30 Sep 120.111 m\xc2\xb3 \xc2\xb7 no alarms \xc2\xb7 every 16 s \xc2\xb7 signal 18 dB") != std::string::npos,
              "example 3 (Kamstrup): %s", kj.c_str());
        CHECK(kj.find("\"vol\":123.456") != std::string::npos && kj.find("\"flow\":0.012") != std::string::npos && kj.find("\"records\":[") != std::string::npos, "readings in the JSON");
        // a heat meter in kWh
        Bytes heat = hexBytes("44 24 23 23 12 45 00 20 04 78 0C 05 23 51 04 00 02 FD 17 00 00");   // HYD = 0x2324
        Reading h; CHECK(readingOf(frameA(withL(heat)), false, h), "heat frame");
        CHECK(wmbus::coreLine(*h.meter) == "Heat meter \xc2\xb7 Diehl \xc2\xb7 00451223 \xc2\xb7 v0x20 \xc2\xb7 4 512.3 kWh \xc2\xb7 no alarms", "example 4: %s", wmbus::coreLine(*h.meter).c_str());
        // an unknown maker (XYZ = 0x633A): its letters, never a guessed name
        Reading z; readingOf(frameA(withL(hexBytes("44 3A 63 26 59 41 31 01 00 A2 11 22"))), false, z); z.snrDb = 12;
        Tracker t5; t5.accept(z, 1);
        CHECK(t5.json(1).find("\"line\":\"Meter \xc2\xb7 XYZ \xc2\xb7 31415926 \xc2\xb7 v0x01 \xc2\xb7 manufacturer format 0xA2 \xc2\xb7 signal 12 dB\"") != std::string::npos,
              "example 5: %s", t5.json(1).c_str());
    }

    printf(" interval and reception\n");
    {
        std::mt19937 rng(5); std::uniform_real_distribution<double> jit(-1, 1);
        auto run = [&](int skipEvery, bool frozen) {
            Tracker tr; double t = 0; int acc = 0;
            for (int k = 0; k < 30; ++k) {
                t += 16 + jit(rng); acc = (acc + 1) & 255;
                if (skipEvery && k % skipEvery == skipEvery - 1) continue;
                Reading r; readingOf(frameA(sensusBody(0x68, 0, uint8_t(frozen ? 7 : acc))), false, r); tr.accept(r, t);
            }
            return tr.json(t);
        };
        const std::string a = run(0, false);
        CHECK(a.find("every 16 s") != std::string::npos && a.find("\"rx\":100") != std::string::npos, "every 16 s, heard 100 %%: %s", a.c_str());
        const std::string b = run(3, false);
        const size_t rxp = b.find("\"rx\":");
        const int rxv = rxp == std::string::npos ? -1 : std::atoi(b.c_str() + rxp + 5);
        CHECK(b.find("every 16 s") != std::string::npos && std::abs(rxv - 67) <= 2, "every third missed: 16 s, ~67 %% (%d): %s", rxv, b.c_str());
        const std::string c = run(0, true);
        CHECK(c.find("every 16 s") != std::string::npos && c.find("\"rx\"") == std::string::npos, "ACC frozen: raw gaps, no %%: %s", c.c_str());
        CHECK(wmbus::everyText(120) == "every 2 min" && wmbus::everyText(7200) == "every 2 h", "interval words");
        // a repeat inside 3 s (same ACC) folds; a damaged frame keeps the readings
        Tracker tr; Reading k; readingOf(frameA(kamBody()), false, k);
        CHECK(tr.accept(k, 10) && !tr.accept(k, 11), "the same frame twice folds");
        Bytes dmg = frameA(kamBody()); dmg[34] ^= 0x10; Reading kd; readingOf(dmg, false, kd); kd.snrDb = 3;
        CHECK(kd.meter->damaged, "damaged frame");
        tr.accept(kd, 30);
        const std::string j = tr.json(30);
        CHECK(j.find("123.456 m\xc2\xb3") != std::string::npos && j.find("signal 3 dB") != std::string::npos && j.find("\"count\":2") != std::string::npos,
              "a damaged frame: heard, readings untouched: %s", j.c_str());
    }

    printf(" through the real front end\n");
    {
        struct Mode { const char* name; bool s; bool c; bool b; };
        const Mode modes[4] = {{"T1", false, false, false}, {"C1-A", false, true, false}, {"C1-B", false, true, true}, {"S1", true, false, false}};
        auto txFor = [&](const Mode& m, const Bytes& body) {
            Tx t;
            if (m.s) { t.chips = chipsS(frameA(body)); t.chipHz = 32768; t.offHz = 868.30e6 - 868.6e6; }
            else if (m.c) { t.chips = chipsC(m.b ? frameB(body) : frameA(body), m.b); t.devHz = 45e3; }
            else t.chips = chipsT(frameA(body));
            return t;
        };
        const double rates[3] = {2.4e6, 2.048e6, 1.024e6};
        for (const Mode& m : modes)
            for (double rt : rates) {
                Tx t = txFor(m, kamBody());
                wmbus::Stats st;
                const auto got = runFront(synth({t}, rt, 0.4, 0.02, 3), rt, &st);
                const Reading* r = findMeter(got, "87654321");
                CHECK(r && r->meter->wm == m.name && r->meter->recs.size() >= 4, "%s at %.3f MS/s: %s", m.name, rt / 1e6, r ? r->meter->wm.c_str() : "not decoded");
                if (r) CHECK(std::fabs(r->freqHz - (m.s ? 868.30e6 : 868.95e6)) < 10e3, "%s: frequency %.0f", m.name, r->freqHz);
            }
        // the Sensus frame, every mode, encrypted
        for (const Mode& m : modes) {
            const auto got = runFront(synth({txFor(m, sensusBody(0x68))}, 2.4e6, 0.4, 0.02, 4), 2.4e6);
            const Reading* r = findMeter(got, "12345678");
            CHECK(r && r->meter->sec == 5, "Sensus %s through the front end", m.name);
        }
        // ★ one frame out of two channels: the better-centred copy is kept
        {
            std::vector<Reading> v; std::vector<double> ts;
            Reading a; readingOf(frameA(kamBody()), false, a); wmbus::Meter ma = *a.meter; ma.carrierHz = 40e3; a.meter = std::make_shared<const wmbus::Meter>(ma); a.freqHz = 868.99e6;
            Reading b2 = a; wmbus::Meter mb = ma; mb.carrierHz = 2e3; b2.meter = std::make_shared<const wmbus::Meter>(mb); b2.freqHz = 868.951e6;
            v = {a, b2}; ts = {1.0, 1.001};
            Service::dedupeMeters(v, ts);
            CHECK(v.size() == 1 && v[0].freqHz == 868.951e6, "a frame from two channels: one kept, the centred one");
        }
        // carrier ±60 ppm, chip rate ±10 % (T) / ±2 % (S), deviation 40 / 80 kHz, both tone polarities
        int ok = 0, tot = 0;
        for (double ppm : {-60.0, 0.0, 60.0}) for (double rf : {0.9, 1.0, 1.1}) for (double dev : {40e3, 80e3}) for (bool inv : {false, true}) {
            Tx t = txFor(modes[0], kamBody()); t.offHz += ppm * 868.95; t.chipHz *= rf; t.devHz = dev; t.invert = inv;
            ++tot; if (findMeter(runFront(synth({t}, 2.4e6, 0.35, 0.02, uint32_t(tot)), 2.4e6), "87654321")) ++ok; else printf("   T1 missed: %+.0f ppm, rate ×%.2f, dev %.0f k, %s\n", ppm, rf, dev / 1e3, inv ? "inverted" : "normal");
        }
        CHECK(ok == tot, "T1 over ±60 ppm × ±10 %% rate × 40/80 kHz × both polarities: %d/%d", ok, tot);
        ok = 0; tot = 0;
        for (double ppm : {-60.0, 60.0}) for (double rf : {0.98, 1.02}) for (bool inv : {false, true}) for (int mi : {1, 2}) {
            Tx t = txFor(modes[mi], kamBody()); t.offHz += ppm * 868.95; t.chipHz *= rf; t.invert = inv;
            ++tot; if (findMeter(runFront(synth({t}, 2.4e6, 0.35, 0.02, uint32_t(100 + tot)), 2.4e6), "87654321")) ++ok;
            else printf("   C1 %s missed: %+.0f ppm, rate ×%.2f, %s (seed %d)\n", mi == 1 ? "A" : "B", ppm, rf, inv ? "inverted" : "normal", 100 + tot);
        }
        CHECK(ok == tot, "C1 A/B over ±60 ppm × ±2 %% × both polarities: %d/%d", ok, tot);
        ok = 0; tot = 0;
        for (double ppm : {-60.0, 60.0}) for (double rf : {0.98, 1.02}) for (bool inv : {false, true}) for (bool conv : {false, true}) {
            Tx t; t.chips = chipsS(frameA(kamBody()), 279, conv); t.chipHz = 32768 * rf; t.offHz = 868.3e6 - 868.6e6 + ppm * 868.3; t.invert = inv;
            ++tot; if (findMeter(runFront(synth({t}, 2.4e6, 0.5, 0.02, uint32_t(200 + tot)), 2.4e6), "87654321")) ++ok;
        }
        CHECK(ok == tot, "S1 over ±60 ppm × ±2 %% × both polarities × both Manchester conventions: %d/%d", ok, tot);
        { Tx t; t.chips = chipsS(frameA(kamBody()), 15); t.chipHz = 32768; t.offHz = -300e3;     // S2's short preamble
          CHECK(findMeter(runFront(synth({t}, 2.4e6, 0.3, 0.02, 9), 2.4e6), "87654321") != nullptr, "S with a short (15 × 01) preamble"); }

        // two meters 300 ms apart; a T1 and an S1 at once
        {
            Tx a = txFor(modes[0], sensusBody(0x68)); a.startS = 0.05;
            Tx b = txFor(modes[1], kamBody()); b.startS = 0.35;
            const auto got = runFront(synth({a, b}, 2.4e6, 0.6, 0.02, 11), 2.4e6);
            CHECK(findMeter(got, "12345678") && findMeter(got, "87654321"), "two meters 300 ms apart");
            Tx s = txFor(modes[3], kamBody()); s.startS = 0.05;
            Tx t = txFor(modes[0], sensusBody(0x68)); t.startS = 0.06;
            const auto g2 = runFront(synth({s, t}, 2.4e6, 0.4, 0.02, 12), 2.4e6);
            CHECK(findMeter(g2, "12345678") && findMeter(g2, "87654321"), "a T1 and an S1 at once");
        }
        // the 868.95 gap at 0.9 MS/s
        {
            Service sv; const Band* b = bandById("868");
            sv.start(*b, 868.6e6, 0.9e6);
            CHECK(sv.report().find("\"wmGap\":1") != std::string::npos, "0.9 MS/s: 868.95 not covered");
            sv.start(*b, 868.6e6, 1.024e6);
            CHECK(sv.report().find("\"wmGap\"") == std::string::npos && sv.report().find("\"meters\":0") != std::string::npos, "1.024 MS/s: covered");
            sv.stop();
            Service s4; s4.start(*bandById("433"), 434.17e6, 2.4e6);
            CHECK(s4.report().find("\"meters\"") == std::string::npos, "433: no meter fields");
            s4.stop();
        }
    }

    printf(" sensitivity (T1 / C1-A / S1, Kamstrup frame, 2.4 MS/s; SNR in the 480 kHz channel)\n");
    {
        const double amp = 0.1;
        const double sigmas[] = {0.02, 0.03, 0.04, 0.05, 0.06, 0.07, 0.085, 0.1};
        for (int mi = 0; mi < 3; ++mi) {
            printf("   %-5s", mi == 0 ? "T1" : mi == 1 ? "C1-A" : "S1");
            for (double sg : sigmas) {
                int ok = 0;
                const int N = 10;
                for (int k = 0; k < N; ++k) {
                    Tx t;
                    if (mi == 0) t.chips = chipsT(frameA(kamBody()));
                    else if (mi == 1) { t.chips = chipsC(frameA(kamBody()), false); t.devHz = 45e3; }
                    else { t.chips = chipsS(frameA(kamBody())); t.chipHz = 32768; t.offHz = -300e3; }
                    t.amp = amp;
                    if (findMeter(runFront(synth({t}, 2.4e6, 0.35, sg, uint32_t(1000 * mi + 10 * k + int(sg * 1e4))), 2.4e6), "87654321")) ++ok;
                }
                const double snr = 10 * std::log10(amp * amp / (2 * sg * sg * 480e3 / 2.4e6));
                printf("  %4.1f dB %3d %%", snr, 100 * ok / N);
                if (sg <= 0.02) CHECK(ok == N, "mode %d at %.1f dB: %d/%d", mi, snr, ok, N);
            }
            printf("\n");
        }
    }

    printf(" noise and carriers decode nothing\n");
    {
        wmbus::Stats st; size_t nb = 0, meters = 0;
        for (int s = 0; s < 4; ++s) { size_t b = 0; const auto got = runFront(synth({}, 2.4e6, 5.0, 0.02, uint32_t(500 + s)), 2.4e6, &st, 868.6e6, &b);
                                      nb += b; for (auto& r : got) meters += r.kind == "meter"; }
        CHECK(meters == 0, "20 s of noise: %zu meters", meters);
        // a CW carrier at 868.95 and one at 868.3, and an FSK square wave (a preamble with no sync) at 868.95
        Tx cw; cw.chips = Chips(200000, 1); cw.devHz = 0; cw.chipHz = 100e3; cw.offHz = 350e3; cw.startS = 0.1;
        Tx cw2 = cw; cw2.offHz = -300e3;
        Tx sq; for (int i = 0; i < 5000; ++i) put(sq.chips, "01"); sq.startS = 2.4;
        size_t b2 = 0;
        const auto got = runFront(synth({cw, cw2, sq}, 2.4e6, 3.0, 0.02, 600), 2.4e6, &st, 868.6e6, &b2);
        nb += b2;
        size_t m2 = 0; for (auto& r : got) m2 += r.kind == "meter";
        CHECK(m2 == 0, "carriers and a sync-less preamble: %zu meters", m2);
        printf("   %zu bursts from noise and carriers, %llu reached the meter decoder, 0 meters\n", nb, (unsigned long long)st.tried);
        /* ★★ THE DECODER ITSELF, HAMMERED: the front end rarely opens a channel on noise, so the bursts are made here — at
         *  480 kS/s, at 868.95 and 868.30: pure noise, and FSK with a real preamble and sync followed by RANDOM chips (the
         *  worst case: everything up to the frame looks right, and only the CRCs stand in the way). Zero meters allowed. */
        {
            std::mt19937 rng(77); std::normal_distribution<float> g(0, 0.05f); std::uniform_int_distribution<int> bit(0, 1);
            const double R = 480e3;
            size_t fakes = 0, tried = 0;
            auto burstOf = [&](const Chips& c, double chipHz, double offInChan, double amp) {
                const size_t n = size_t(0.008 * R) + size_t(double(c.size()) / chipHz * R) + size_t(0.006 * R);
                auto v = std::make_shared<std::vector<cf>>(n);
                double ph = 0;
                for (size_t i = 0; i < n; ++i) {
                    (*v)[i] = cf(g(rng), g(rng));
                    const double tt = double(i) / R - 0.008;
                    if (tt >= 0 && tt * chipHz < double(c.size())) {
                        ph += 2 * M_PI * (offInChan + (c[size_t(tt * chipHz)] ? 50e3 : -50e3)) / R;
                        (*v)[i] += cf(float(amp * std::cos(ph)), float(amp * std::sin(ph)));
                    }
                }
                return std::shared_ptr<const std::vector<cf>>(v);
            };
            for (int k = 0; k < 3000; ++k) {
                Burst b; b.bbRate = R; b.levelDb = -20; b.noiseDb = -40;
                const int kind = k % 6;
                Chips c;
                if (kind == 0 || kind == 1) c.assign(size_t(1000 + k % 500), 0);                    // noise only (amp 0)
                else if (kind == 2) { for (int i = 0; i < 24; ++i) put(c, "01"); put(c, "0000111101"); for (int i = 0; i < 1200; ++i) c.push_back(uint8_t(bit(rng))); }
                else if (kind == 3) { for (int i = 0; i < 16; ++i) put(c, "01"); put(c, "01010100001111010101010011001101"); for (int i = 0; i < 800; ++i) c.push_back(uint8_t(bit(rng))); }
                else if (kind == 4) { for (int i = 0; i < 16; ++i) put(c, "01"); put(c, "01010100001111010101010000111101"); for (int i = 0; i < 800; ++i) c.push_back(uint8_t(bit(rng))); }
                else { for (int i = 0; i < 40; ++i) put(c, "01"); put(c, "000111011010010110"); for (int i = 0; i < 600; ++i) c.push_back(uint8_t(bit(rng))); }
                const bool s = kind == 5;
                b.offHz = s ? -300e3 : 350e3;
                b.bb = burstOf(c, s ? 32768 : 100e3, 0, kind <= 1 ? 0.0 : 0.3);
                wmbus::Meter m; wmbus::Stats ws;
                if (wmbus::decodeBurst(b, 868.6e6, m, &ws) == 1) ++fakes;
                tried += ws.tried;
            }
            CHECK(fakes == 0, "3000 noise / random-data bursts at the meter decoder: %zu meters", fakes);
            printf("   3000 made bursts (1000 noise, 2000 FSK with a true preamble and sync, then random chips): %zu tried, %zu meters\n", tried, fakes);
        }
        // ★ the real 868.6 MHz capture (the Pi 500's RTL, MW antenna — no meters expected; a no-false-decodes check)
        const char* rec = std::getenv("VIBE_WMBUS_REC");
        if (rec && *rec) {
            FILE* f = std::fopen(rec, "rb");
            CHECK(f != nullptr, "open %s", rec);
            if (f) {
                FrontEnd fe(2.4e6); fe.setDcGuardHz(15e3);
                const Band* band = bandById("868");
                fe.setUsableHz(-band->needHz - 40e3, band->needHz + 40e3);
                fe.setChanRate(500e3); fe.setKeepBaseband(true);
                std::vector<Reading> all; wmbus::Stats rs; size_t bursts = 0;
                auto on = [&](const Burst& b) { ++bursts; Service::decodeBurst(b, 868.6e6, all, &rs); };
                std::vector<uint8_t> u(1 << 20); std::vector<cf> x;
                const auto t0 = std::chrono::steady_clock::now(); double secs = 0;
                for (;;) { const size_t n = std::fread(u.data(), 1, u.size(), f) / 2; if (!n) break; x.resize(n);
                           for (size_t i = 0; i < n; ++i) x[i] = cf((u[2 * i] - 127.5f) / 128.0f, (u[2 * i + 1] - 127.5f) / 128.0f);
                           fe.feed(x.data(), n, on); secs += n / 2.4e6; }
                fe.flush(on); std::fclose(f);
                const double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                size_t m3 = 0; for (auto& r : all) m3 += r.kind == "meter";
                CHECK(m3 == 0, "the real recording: %zu meters", m3);
                printf("   real capture %.1f s: %zu bursts, %llu at 868.30/868.95 tried, %zu readings (%zu meters); %.1f %% of real time\n",
                       secs, bursts, (unsigned long long)rs.tried, all.size(), m3, 100 * cpu / std::max(secs, 1e-9));
            }
        } else printf("   (VIBE_WMBUS_REC not set: the real 868.6 MHz recording was not replayed)\n");
    }

    printf(" privacy on the wire, in the day log and the card\n");
    {
        const std::string dir = tmpDir();
        vibe::gallery::Store store; store.setDir(dir); store.setLimitBytes(50ull << 20);
        int64_t clk = 1798675200;   // 2027-01-01 00:00 UTC
        vibe::ismgal::DayLog::Options o; o.bandId = "868"; o.bandName = "868 MHz SRD"; o.bandHz = 868.6e6; o.origin = "live"; o.clock = [&] { return clk; };
        auto log = std::make_shared<vibe::ismgal::DayLog>(store, o);
        Tracker tr;
        double t = 0;
        int lines = 0;
        // the Sensus every 16 s for an hour (225 frames), unchanged → 1 + 4 lines (first, then once per 15 min)
        for (int k = 0; k < 225; ++k) {
            t = 16.0 * k; clk = 1798675200 + int64_t(t);
            Reading r; readingOf(frameA(sensusBody(0x68, 0, uint8_t(k))), false, r);
            Heard h; if (tr.accept(r, t, &h)) { log->heard(h); ++lines; }
            log->tick();
        }
        const int written = int(log->readings());
        CHECK(written == 4 || written == 5, "an unchanged meter for an hour: %d lines (one per 15 min)", written);
        // a changed status writes at once
        { t += 16; clk = 1798675200 + int64_t(t); Reading r; readingOf(frameA(sensusBody(0x68, 0x04, 230)), false, r); Heard h; tr.accept(r, t, &h); log->heard(h); }
        CHECK(int(log->readings()) == written + 1, "a changed reading/status writes at once");
        // a second meter and a Kamstrup with readings
        { Reading r; readingOf(frameA(kamBody()), false, r); Heard h; if (tr.accept(r, t + 1, &h)) log->heard(h); }
        const std::string json = tr.json(t + 2);
        log->finish();
        const std::string files = slurpDir(dir);
        CHECK(files.find("Water meter \xc2\xb7 Sensus \xc2\xb7 12345678") != std::string::npos, "the serial is in the log and the card");
        CHECK(files.find("2 devices: 2 meters (1 cold water, 1 water)") != std::string::npos, "the card counts meters by type: %s",
              files.substr(std::min(files.find("devices"), files.size()), 60).c_str());   // ★ a failing check reports, never throws
        CHECK(json.find("12345678") != std::string::npos, "the serial is in the JSON");
        // ★★★ never a run of the encrypted payload (A0 … BF): every 4-byte window, hex both cases, spaced or not, and decimal
        int leaks = 0;
        for (int i = 0; i + 4 <= 32; ++i) {
            char a[16], b[16], c[16], d[24], e[24];
            std::snprintf(a, sizeof a, "%02X%02X%02X%02X", 0xA0 + i, 0xA1 + i, 0xA2 + i, 0xA3 + i);
            std::snprintf(b, sizeof b, "%02x%02x%02x%02x", 0xA0 + i, 0xA1 + i, 0xA2 + i, 0xA3 + i);
            std::snprintf(c, sizeof c, "%02X %02X %02X", 0xA0 + i, 0xA1 + i, 0xA2 + i);
            std::snprintf(d, sizeof d, "%d,%d,%d,%d", 0xA0 + i, 0xA1 + i, 0xA2 + i, 0xA3 + i);
            std::snprintf(e, sizeof e, "%u", unsigned(0xA0 + i) | unsigned(0xA1 + i) << 8 | unsigned(0xA2 + i) << 16 | unsigned(0xA3 + i) << 24);
            for (const char* s : {a, b, c, d, e}) leaks += countOf(json, s) + countOf(files, s);
        }
        CHECK(leaks == 0, "the encrypted bytes appear %d times", leaks);
        CHECK(json.find("\"encLen\":32") != std::string::npos, "only the encrypted LENGTH is said");
        rmDir(dir);
        (void)lines;
    }

    printf(" busy street (300 meters every 16 s), the cap, CPU\n");
    {
        Tracker tr;
        for (int m = 0; m < 600; ++m) {
            Bytes b = sensusBody(0x68, 0, uint8_t(m));
            b[4] = uint8_t(m & 0xFF); b[5] = uint8_t(m >> 8);
            Reading r; readingOf(frameA(b), false, r); tr.accept(r, 1.0 + m * 0.05);
        }
        Reading w; w.kind = "weather"; w.make = "Fine Offset"; w.model = "WH2"; w.pubId = "1"; w.tempC = 10; tr.accept(w, 0.5);
        CHECK(tr.meterCount() == Tracker::kMaxMeters && tr.json(40).find("Fine Offset") != std::string::npos, "600 meters: %zu kept (cap %zu), the weather station stays",
              tr.meterCount(), Tracker::kMaxMeters);
        Tracker t3;
        for (int m = 0; m < 300; ++m) { Bytes b = sensusBody(0x68, 0, 1); b[4] = uint8_t(m); b[5] = uint8_t(m >> 8); Reading r; readingOf(frameA(b), false, r); t3.accept(r, 1); }
        for (int m = 0; m < 300; ++m) { Bytes b = sensusBody(0x68, 0, 2); b[4] = uint8_t(m); b[5] = uint8_t(m >> 8); Reading r; readingOf(frameA(b), false, r); t3.accept(r, 17); }
        const size_t bytes = t3.json(18).size();
        printf("   report for 300 meters: %zu bytes (%.0f bytes a meter) → %.0f KB/s per listener at one report a second\n", bytes, double(bytes) / 300, bytes / 1024.0);
        CHECK(bytes < 300 * 900, "report size %zu", bytes);
        // CPU: the demodulator on 50 T1 bursts at 2.4 MS/s (the front end is ISM's own, measured by test-ism)
        std::vector<Tx> txs;
        for (int k = 0; k < 50; ++k) { Tx t; Bytes b = kamBody(); b[4] = uint8_t(k); t.chips = chipsT(frameA(b)); t.startS = 0.05 + 0.2 * k; t.offHz = 350e3 + (k % 5) * 5e3; txs.push_back(t); }
        const auto x = synth(txs, 2.4e6, 10.2, 0.02, 31);
        const auto t0 = std::chrono::steady_clock::now();
        wmbus::Stats st;
        const auto got = runFront(x, 2.4e6, &st);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::set<std::string> ids; for (auto& r : got) if (r.kind == "meter") ids.insert(r.meter->id);
        const size_t n = ids.size();   // ★ a frame may come out of two channels (its skirts); the list folds those
        // time the decoder alone on the same bursts
        FrontEnd fe(2.4e6); fe.setDcGuardHz(15e3); fe.setChanRate(500e3); fe.setKeepBaseband(true);
        std::vector<Burst> bs;
        for (size_t i = 0; i < x.size(); i += 16384) fe.feed(x.data() + i, std::min<size_t>(16384, x.size() - i), [&](const Burst& b) { bs.push_back(b); });
        fe.flush([&](const Burst& b) { bs.push_back(b); });
        const auto t1 = std::chrono::steady_clock::now();
        for (int rep = 0; rep < 5; ++rep) for (const auto& b : bs) { wmbus::Meter m; wmbus::decodeBurst(b, 868.6e6, m); }
        const double dsec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() / 5;
        printf("   CPU: 10.2 s of 868 capture at 2.4 MS/s with 50 T1 frames: %.3f s (%.1f %% of one core, front end + decoders); "
               "the meter decoder alone %.2f ms a burst\n", sec, 100 * sec / 10.2, 1e3 * dsec / std::max<size_t>(1, bs.size()));
        CHECK(n == 50, "busy: %zu of 50 frames decoded", n);
    }

    printf(" reach\n");
    {
        const Band* b = bandById("868");
        CHECK(b && b->needHz == 450e3 && std::fabs(b->centreHz - b->needHz - 868.15e6) < 1 && std::fabs(b->centreHz + b->needHz - 869.05e6) < 1, "868 needs 868.15-869.05");
        Reading m; m.kind = "meter";
        CHECK(bandPlausible(m, 868.95e6) && bandPlausible(m, 868.3e6) && !bandPlausible(m, 868.6e6) && !bandPlausible(m, 433.92e6), "a meter only at 868.3 / 868.95");
    }

    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "ok", checks, fails);
    return fails ? 1 : 0;
}
