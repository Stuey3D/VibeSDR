// test-ax25hs.cpp — V12 9600 bit/s packet (G3RUH), FX.25 and IL2P: vibe_rs8.h, vibe_fx25.h, vibe_il2p.h,
// vibe_pktlink.h, vibe_g3ruh.h, and their use in vibe_aprs.h / vibe_aprs_run.h. docs/v12/AX25-9600-FX25-IL2P.md.
//
//  1. Reed-Solomon: every code FX.25 and IL2P use, random words with up to t byte errors corrected exactly; t+1 never
//     "corrected" into a wrong word that passes as right without being flagged (counted).
//  2. FX.25 on a REAL off-air code block (Tag_01, RS(255,239), seven bad bytes — published in the Dire Wolf FX.25 paper
//     as received from a KD4HBO-1 transmission): it decodes, the seven positions are the paper's, the frame inside passes
//     its FCS and reads "KD4HBO-1>CQ,TELEM:SYS 241830 …". With first root 0 it does not decode (the RS parameter check).
//  3. FX.25 through our own encoder and the bit receiver, every code, byte errors up to the limit.
//  4. IL2P: the spec's three example packets decode to the spec's AX.25 bytes (with the trailing CRC verified), and our
//     encoder reproduces the spec's IL2P bytes exactly; inverted polarity; baseline FEC; Type 0; errors up to the limit.
//  5. The 9600 modem: G3RUH from our transmitter, IQ (48 / 37.5 / 250 kS/s) and discriminator audio, AWGN sweep by
//     Eb/N0, plain AX.25 vs FX.25 vs IL2P; one slicer vs the bank; carrier offsets with the AFC; inverted audio.
//  6. 1200 Bd AFSK with FX.25 and IL2P through the APRS demodulator.
//  7. Noise only (IQ and audio, every receiver on): ZERO frames.
//  8. The run: a non-APRS frame (cubesat telemetry) in the frame log as hex/ASCII, the satellite name, the JSON.
//  9. CPU per second of signal (this machine).
#include "vibe_aprs_run.h"
#include "vibe_g3ruh.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_pass; else { ++g_fail; std::printf("   FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using cf32 = std::complex<float>;

// ── AX.25 frames ─────────────────────────────────────────────────────────────────────────────────────────────────────
static void addr(std::vector<uint8_t>& b, const std::string& call, int ssid, bool last, bool cbit = false) {
    for (int i = 0; i < 6; ++i) b.push_back(uint8_t((i < int(call.size()) ? call[size_t(i)] : ' ') << 1));
    b.push_back(uint8_t(0x60 | (ssid << 1) | (last ? 1 : 0) | (cbit ? 0x80 : 0)));
}
static std::vector<uint8_t> ui(const std::string& src, int ssid, const std::string& dst, const std::string& info, int pid = 0xF0) {
    std::vector<uint8_t> b;
    addr(b, dst, 0, false, true);
    addr(b, src, ssid, true);
    b.push_back(0x03); b.push_back(uint8_t(pid));
    b.insert(b.end(), info.begin(), info.end());
    return b;
}
/** A cubesat-style telemetry frame: UI, PID F0, `n` bytes of binary payload. */
static std::vector<uint8_t> telem(std::mt19937& rng, int n, const std::string& src = "OM9GRB") {
    std::vector<uint8_t> b = ui(src, 0, "CQ", "");
    for (int i = 0; i < n; ++i) b.push_back(uint8_t(rng()));
    return b;
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
static void testRs() {
    std::printf("1. Reed-Solomon\n");
    std::mt19937 rng(11);
    struct C { int nr, fcr, n; } cs[] = { { 16, 1, 255 }, { 16, 1, 48 }, { 32, 1, 160 }, { 64, 1, 128 }, { 64, 1, 255 },
                                          { 2, 0, 15 }, { 4, 0, 80 }, { 8, 0, 255 }, { 16, 0, 40 }, { 16, 0, 255 } };
    for (auto c : cs) {
        vibers::Rs rs(c.nr, c.fcr);
        int okExact = 0, tries = 0, over = 0, overWrong = 0;
        for (int it = 0; it < 200; ++it) {
            std::vector<uint8_t> w(size_t(c.n));
            for (int i = 0; i < c.n - c.nr; ++i) w[size_t(i)] = uint8_t(rng());
            rs.encode(w.data(), c.n - c.nr, w.data() + c.n - c.nr);
            const int t = c.nr / 2;
            for (int e = 0; e <= t + 1; ++e) {
                std::vector<uint8_t> r = w;
                std::vector<int> pos(size_t(c.n)); for (int i = 0; i < c.n; ++i) pos[size_t(i)] = i;
                std::shuffle(pos.begin(), pos.end(), rng);
                for (int k = 0; k < e; ++k) r[size_t(pos[size_t(k)])] ^= uint8_t(1 + rng() % 255);
                const int f = rs.decode(r.data(), c.n);
                if (e <= t) { ++tries; if (f == e && r == w) ++okExact; }
                else { ++over; if (f >= 0 && r != w) ++overWrong; }
            }
        }
        CHECK(okExact == tries, "RS(%d,%d) fcr %d: %d/%d words with ≤ t errors corrected exactly", c.n, c.n - c.nr, c.fcr, okExact, tries);
        std::printf("   RS(%3d,%3d) fcr %d: ≤t errors %d/%d exact; t+1 errors miscorrected %d/%d\n", c.n, c.n - c.nr, c.fcr, okExact, tries, overWrong, over);
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// The off-air FX.25 block: Dire Wolf's "AX.25 + FEC = FX.25" paper (WB2OSZ, 2020), the -dxx dump of the fourth frame of
// ande2_demo_02aug2009.wav (a KD4HBO-1 FX.25 transmission, AMSAT-BB 2009): "Matched correlation tag 0x01 … FEC complete,
// fixed 7 errors in byte positions: 127 128 228 251 252 253 254". The bytes are data, as received — not code.
static const uint8_t kOffAir[255] = {
0x7e, 0x86, 0xa2, 0x40, 0x40, 0x40, 0x40, 0x60, 0x96, 0x88, 0x68, 0x90, 0x84, 0x9e, 0x62, 0xa8, 0x8a, 0x98, 0x8a, 0x9a,
0x40, 0x61, 0x03, 0xf0, 0xa5, 0xb2, 0xa6, 0x40, 0x64, 0x68, 0x62, 0x70, 0x66, 0x60, 0x40, 0x6c, 0x6a, 0x6a, 0x66, 0x6a,
0x40, 0x60, 0x40, 0x66, 0x60, 0x40, 0x60, 0x40, 0xcc, 0xcc, 0xc4, 0x70, 0x40, 0x60, 0xcc, 0x62, 0x70, 0x40, 0xcc, 0xcc,
0xc8, 0x60, 0x40, 0x60, 0xc4, 0xc8, 0x60, 0x40, 0xcc, 0xcc, 0xcc, 0x62, 0x40, 0x62, 0x64, 0x72, 0x70, 0x40, 0xcc, 0xca,
0x6e, 0x6a, 0x40, 0x60, 0xcc, 0xc6, 0x60, 0x40, 0xcc, 0xcc, 0xca, 0xc2, 0x40, 0x62, 0x60, 0xc8, 0x70, 0x40, 0xcc, 0xcc,
0xcc, 0x60, 0x40, 0x62, 0x62, 0x72, 0x60, 0x40, 0x62, 0x66, 0x5c, 0x70, 0x40, 0x6a, 0x5c, 0x60, 0x40, 0x72, 0x5c, 0x72,
0x40, 0x40, 0x5c, 0x62, 0x66, 0x40, 0x60, 0xdc, 0x63, 0x66, 0x40, 0x62, 0x5c, 0x6e, 0x72, 0x40, 0x62, 0x62, 0x5c, 0x60,
0x6c, 0x40, 0x70, 0x5c, 0x62, 0x72, 0x40, 0x70, 0x5c, 0x62, 0x72, 0x40, 0x68, 0x62, 0x40, 0x68, 0x6a, 0x40, 0x68, 0x64,
0x40, 0x40, 0x6c, 0x64, 0xfd, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc,
0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc,
0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc,
0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xff, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0xfc, 0x80,
0x1d, 0x7e, 0xb6, 0x54, 0x80, 0x16, 0xda, 0x14, 0xf5, 0xf4, 0x4a, 0x17, 0xbd, 0xa9, 0x6c };

static void testFx25() {
    std::printf("2. FX.25 on a real off-air code block\n");
    {
        std::vector<uint8_t> b(kOffAir, kOffAir + 255);
        int where[64];
        const int f = vibefx25::codec(16).decode(b.data(), 255, -1, where);
        std::vector<int> w(where, where + std::max(0, f)); std::sort(w.begin(), w.end());
        CHECK(f == 7 && w == std::vector<int>({ 127, 128, 228, 251, 252, 253, 254 }), "fixed %d bytes (the paper: 7 at 127 128 228 251–254)", f);
        std::vector<uint8_t> fr;
        CHECK(vibefx25::extractFrame(b.data(), 239, fr), "the AX.25 frame inside passes its FCS");
        vibeaprs::Ax25 ax;
        const bool p = vibeaprs::parseAx25(fr, ax);
        const std::string t = p ? vibeaprs::tnc2(ax) : "";
        CHECK(p && t.rfind("KD4HBO-1>CQ,TELEM:SYS 241830 65535 0 30 0 ffb8", 0) == 0 && fr.size() == 161, "it reads %s (%zu bytes)", t.substr(0, 50).c_str(), fr.size());
        std::printf("   %s\n", t.substr(0, 90).c_str());
        std::vector<uint8_t> c(kOffAir, kOffAir + 255);
        vibers::Rs r0(16, 0);
        CHECK(r0.decode(c.data(), 255) < 0, "with first root 0 it does not decode — FX.25's first root is 1");
        // the whole frame on the bit level: the tag, then the block LSb first, through the receiver
        vibefx25::Rx rx;
        int got = 0, fixed = -1;
        auto feed = [&](int bit) { rx.bit(bit, [&](const vibefx25::Result& r) { ++got; fixed = r.fixed; }); };
        for (int i = 0; i < 64; ++i) feed(0x55 >> (i & 7) & 1);
        int nm = 0; const vibefx25::Mode* ms = vibefx25::modes(nm);
        uint64_t tag = ms[0].value ^ 0x8000000100000001ULL;   // three tag bits wrong: still matched
        for (int i = 0; i < 64; ++i) feed(int((tag >> i) & 1));
        for (int i = 0; i < 255 * 8; ++i) feed((kOffAir[i >> 3] >> (i & 7)) & 1);
        CHECK(got == 1 && fixed == 7, "the bit receiver (tag with 3 bits wrong) gives the frame, 7 fixed (%d, %d)", got, fixed);
    }
    std::printf("3. FX.25 through our encoder\n");
    std::mt19937 rng(5);
    for (int nr : { 16, 32, 64 }) {
        int sent = 0, got = 0, wrong = 0;
        std::vector<int> tagsSeen;
        for (int it = 0; it < 60; ++it) {
            const int len = 10 + int(rng() % (nr == 64 ? 100 : 180));
            const std::vector<uint8_t> fr = telem(rng, len);
            int tag = 0;
            std::vector<int> bits = vibefx25::encodeBits(fr, nr, 4, 2, &tag);
            if (bits.empty()) continue;
            tagsSeen.push_back(tag);
            // byte errors up to the code's limit, in the codeblock (bits 4·8 + 64 on)
            int n = 0; for (int i = 0; i < vibefx25::modes(n)[0].n; ++i) {}
            const size_t cb0 = 32 + 64;
            const int nbytes = int((bits.size() - cb0 - 16) / 8);
            for (int e = 0; e < nr / 2; ++e) { const size_t at = cb0 + size_t(rng() % unsigned(nbytes)) * 8; for (int k = 0; k < 8; ++k) if (rng() & 1) bits[at + size_t(k)] ^= 1; }
            ++sent;
            vibefx25::Rx rx;
            for (int b : bits) rx.bit(b, [&](const vibefx25::Result& r) { if (r.bytes == fr) ++got; else ++wrong; });
        }
        CHECK(got == sent && wrong == 0, "RS check bytes %d: %d/%d frames with t byte errors, %d wrong", nr, got, sent, wrong);
        std::sort(tagsSeen.begin(), tagsSeen.end()); tagsSeen.erase(std::unique(tagsSeen.begin(), tagsSeen.end()), tagsSeen.end());
        std::printf("   %d check bytes: %d/%d decoded with %d bad bytes each, tags used:", nr, got, sent, nr / 2);
        for (int t : tagsSeen) std::printf(" %02X", t);
        std::printf("\n");
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
static std::vector<uint8_t> hexv(const char* s) { std::vector<uint8_t> v; unsigned x; int n; while (std::sscanf(s, " %x%n", &x, &n) == 1) { v.push_back(uint8_t(x)); s += n; } return v; }

static void testIl2p() {
    std::printf("4. IL2P — the spec's example packets (IL2P draft v0.6, \"Example Encoded Packets\")\n");
    struct V { const char* name; const char* ax; const char* il2p; } vs[] = {
        { "S-frame RR, KK4HEJ-7 → KA2DEW-2", "96 82 64 88 8A AE E4 96 96 68 90 8A 94 6F 81",
          "26 57 4D 57 F1 D2 A8 F0 6A F2 7B AD 23 BD C0 7F 00 1D 2B" },
        { "UI frame, KK4HEJ-15 → CQ", "86 A2 40 40 40 40 60 96 96 68 90 8A 94 FF 03 F0",
          "6A EA 9C C2 01 11 FC 14 1F DA 6E F2 53 91 BD 47 6C 54 54" },
        { "I-frame, TheNET PID, 9 bytes", "96 82 64 88 8A AE E4 96 96 68 90 8A 94 65 B8 CF 30 31 32 33 34 35 36 37 38",
          "26 13 6D 02 8C FE FB E8 AA 94 2D 6A 34 43 35 3C 69 9F 0C 75 5A 38 A1 7F A5 DA D8 F6 EA 57 37 3D B1 2A B0 DE 44 A8 20 D0 1D 5A 2B 38" },
    };
    for (const V& v : vs) {
        const std::vector<uint8_t> ax = hexv(v.ax), il = hexv(v.il2p);
        const std::vector<uint8_t> enc = vibeil2p::encode(ax, true, true, 0);
        CHECK(enc == il, "%s: our encoder gives the spec's %zu bytes (%zu)", v.name, il.size(), enc.size());
        for (int inv = 0; inv < 2; ++inv) {
            vibeil2p::Rx rx; int got = 0; bool crc = false, same = false;
            for (int b : vibeil2p::lineBits(il, 4, inv != 0)) rx.bit(b, [&](const vibeil2p::Result& r) { ++got; crc = r.crc; same = r.bytes == ax; });
            CHECK(got == 1 && same && crc, "%s%s: decoded to the spec's AX.25 bytes, CRC verified", v.name, inv ? " (inverted)" : "");
        }
        std::printf("   %-34s encoder = spec, decoder = spec, CRC ok, either polarity\n", v.name);
    }
    std::mt19937 rng(9);
    // Type 0 (a digipeater path cannot translate), baseline FEC, errors up to the limit, with and without the CRC
    struct K { const char* name; bool maxFec, crc, digi; int len; } ks[] = {
        { "Type 1, max FEC, CRC", true, true, false, 200 }, { "Type 1, max FEC, no CRC", true, false, false, 600 },
        { "Type 1, baseline FEC, CRC", false, true, false, 300 }, { "Type 0 (via WIDE1-1), max FEC, CRC", true, true, true, 100 },
        { "Type 0, baseline, no CRC", false, false, true, 40 } };
    for (const K& k : ks) {
        int sent = 0, got = 0, wrong = 0;
        for (int it = 0; it < 40; ++it) {
            std::vector<uint8_t> ax = telem(rng, 1 + int(rng() % unsigned(k.len)));
            if (k.digi) { std::vector<uint8_t> d; addr(d, "CQ", 0, false, true); addr(d, "G4ABC", 9, false); addr(d, "WIDE1", 1, true); d.push_back(0x03); d.push_back(0xF0); d.insert(d.end(), ax.begin() + 16, ax.end()); ax = d; }
            std::vector<uint8_t> il = vibeil2p::encode(ax, k.maxFec, k.crc);
            // errors: one header byte when the CRC is there (otherwise none), and up to what each block's rule accepts
            const vibeil2p::Layout L = vibeil2p::layout(int(ax.size()) - (k.digi ? 0 : 16), k.maxFec);
            if (k.crc) il[size_t(rng() % 15)] ^= uint8_t(1 + rng() % 255);
            size_t p = 15;
            for (int b = 0; b < L.blocks; ++b) {
                const int sz = (b < L.nLarge ? L.large : L.small) + L.parity;
                const int ne = k.crc ? L.parity / 2 : (L.parity >= 8 ? L.parity / 2 - 1 : 0);
                for (int e = 0; e < ne; ++e) il[p + size_t(rng() % unsigned(sz))] ^= uint8_t(1 + rng() % 255);
                p += size_t(sz);
            }
            ++sent;
            vibeil2p::Rx rx;
            for (int b : vibeil2p::lineBits(il, 2)) rx.bit(b, [&](const vibeil2p::Result& r) { if (r.bytes == ax) ++got; else ++wrong; });
        }
        CHECK(got == sent && wrong == 0, "%s: %d/%d with errors, %d wrong", k.name, got, sent, wrong);
        std::printf("   %-36s %d/%d decoded with errors at the rule's limit, %d wrong\n", k.name, got, sent, wrong);
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
//  THE 9600 MODEM
// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
enum Link { kPlain, kFx16, kFx32, kIl2p };
static const char* linkName(int l) { return l == kPlain ? "AX.25" : l == kFx16 ? "FX.25 16" : l == kFx32 ? "FX.25 32" : "IL2P"; }

/** One transmission's line bits: a preamble, `frames` frames, a postamble. */
static std::vector<int> txLine(const std::vector<std::vector<uint8_t>>& frames, int link, vibepkt::Scrambler& scr, int& lvl) {
    std::vector<int> data;
    if (link == kIl2p) {
        std::vector<int> line;
        // 9600 IL2P: no G3RUH scrambler, no NRZI (vibe_pktlink.h); its own preamble is alternating bits
        for (const auto& f : frames) { auto b = vibeil2p::lineBits(vibeil2p::encode(f, true, true), line.empty() ? 24 : 0); line.insert(line.end(), b.begin(), b.end()); }
        return line;
    }
    for (int i = 0; i < 24; ++i) vibehdlc::flagBits(data);
    for (const auto& f : frames) {
        if (link == kPlain) {
            std::vector<uint8_t> g = f; vibehdlc::appendFcs(g);
            vibehdlc::stuffBits(g, data); vibehdlc::flagBits(data);
        } else {
            auto b = vibefx25::encodeBits(f, link == kFx16 ? 16 : 32, 1, 1);
            data.insert(data.end(), b.begin(), b.end());
        }
    }
    for (int i = 0; i < 4; ++i) vibehdlc::flagBits(data);
    return vibeg3ruh::lineFromData(data, scr, lvl);
}

struct Score { int sent = 0, got = 0, wrong = 0, fec = 0; };
/** Bursts of `frames` through AWGN at `ebn0` dB, IQ at `fs` (or discriminator audio when `audio`). */
static Score runModem(int link, double ebn0, double fs, bool audio, int bursts, int seed, const vibeg3ruh::Opts& o,
                      double offHz = 0, bool invert = false, double* cpuSecPerSec = nullptr, double* afcOut = nullptr, double txDev = 0) {
    if (!(txDev > 0)) txDev = o.devHz;
    std::mt19937 rng{uint32_t(seed)};
    std::normal_distribution<float> nd(0.f, 1.f);
    vibeg3ruh::Demod dm(o);
    Score s;
    std::vector<std::vector<uint8_t>> sentFrames;
    auto onF = [&](const vibepkt::PktFrame& f) {
        bool ok = false; for (size_t q = 0; q < sentFrames.size(); ++q) if (sentFrames[q] == f.bytes) { ok = true; if (getenv("AX25HS_WHICH")) std::printf(" got#%zu(sl%d)", q, f.slicer); }
        if (ok) { ++s.got; if (f.fec != vibepkt::kAx25) ++s.fec; } else ++s.wrong;
    };
    // Eb/N0 → noise: unit-power carrier; σ² (complex, total) = fs / (baud · Eb/N0)
    const double sig2 = fs / (o.baud * std::pow(10.0, ebn0 / 10));
    const float sd = float(std::sqrt(sig2 / 2));
    double cpu = 0, dur = 0;
    vibepkt::Scrambler scr; int lvl = 0;
    for (int b = 0; b < bursts; ++b) {
        std::vector<std::vector<uint8_t>> fr;
        fr.push_back(b % 3 == 0 ? telem(rng, 40 + int(rng() % 150)) : ui("G4ABC", int(b % 15), "APRS", "!5130.00N/00010.00W>burst " + std::to_string(b) + " 9600 test"));
        for (auto& f : fr) sentFrames.push_back(f);
        s.sent += int(fr.size());
        const std::vector<int> line = txLine(fr, link, scr, lvl);
        std::vector<float> bb = vibeg3ruh::baseband(line, fs, o.baud, 0.5);
        // 20 ms of no carrier before each burst
        const size_t gap = getenv("AX25HS_NOGAP") ? 0 : size_t(fs * 0.02);
        if (audio) {
            // discriminator audio: what an FM receiver's data port gives — the baseband (+ the discriminator's noise,
            // modelled from the IQ by demodulating it; simplest faithful model)
            std::vector<cf32> iq = vibeg3ruh::fmIq(bb, fs, txDev, offHz, double(rng() % 628) / 100);
            std::vector<cf32> all(gap + iq.size());
            for (size_t i = 0; i < all.size(); ++i) all[i] = (i >= gap ? iq[i - gap] : cf32(0, 0)) + cf32(sd * nd(rng), sd * nd(rng));
            vibedsp::FmDemod fm(float(fs / (2 * M_PI * txDev)));
            std::vector<float> a(all.size());
            fm.process(all.data(), a.data(), int(all.size()));
            if (invert) for (auto& v : a) v = -v;
            const auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < a.size(); i += 4800) dm.feedAudio(a.data() + i, int(std::min<size_t>(4800, a.size() - i)), fs, onF);
            cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            dur += a.size() / fs;
        } else {
            std::vector<cf32> iq = vibeg3ruh::fmIq(bb, fs, txDev, offHz, double(rng() % 628) / 100);
            std::vector<cf32> all(gap + iq.size());
            for (size_t i = 0; i < all.size(); ++i) all[i] = (i >= gap ? iq[i - gap] : cf32(0, 0)) + cf32(sd * nd(rng), sd * nd(rng));
            const auto t0 = std::chrono::steady_clock::now();
            const size_t blk = size_t(fs / 100);
            for (size_t i = 0; i < all.size(); i += blk) {
                dm.feedIq(all.data() + i, int(std::min(blk, all.size() - i)), fs, onF);
                if (getenv("AX25HS_EYE") && i > gap + blk * 3 && i + blk * 2 < all.size()) std::printf(" %.2f", dm.eye());
            }
            if (getenv("AX25HS_EYE")) std::printf(" | afc %.0f\n", dm.afcHz());
            cpu += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            dur += all.size() / fs;
        }
    }
    // flush: a little noise at the end
    std::vector<cf32> tail(size_t(fs * 0.05));
    for (auto& v : tail) v = cf32(sd * nd(rng), sd * nd(rng));
    if (!audio) dm.feedIq(tail.data(), int(tail.size()), fs, onF);
    else { std::vector<float> a(tail.size()); for (size_t i = 0; i < a.size(); ++i) a[i] = sd * nd(rng); dm.feedAudio(a.data(), int(a.size()), fs, onF); }
    if (cpuSecPerSec) *cpuSecPerSec = cpu / dur;
    if (afcOut) *afcOut = dm.afcHz();
    if (getenv("AX25HS_EYE")) std::printf("      [eye at the end %.3f]\n", dm.eye());
    if (getenv("AX25HS_FX")) std::printf("      [%s %.0f dB: sent %d got %d via-fec %d | fx25 tags %llu rsfail %llu ok %llu]\n", linkName(link), ebn0, s.sent, s.got, s.fec,
        (unsigned long long)dm.fx25Tags(), (unsigned long long)dm.fx25RsFailed(), (unsigned long long)dm.fx25Decoded());
    return s;
}

static void testModem(bool full) {
    std::printf("5. The 9600 modem (G3RUH, GFSK BT 0.5, ±3 kHz) — %% of frames decoded vs Eb/N0\n");
    vibeg3ruh::Opts all; all.fx25 = true; all.il2p = true;
    // clean signals through every input
    for (double fs : { 48000.0, 37500.0, 250000.0 }) {
        Score sc = runModem(kPlain, 30, fs, false, 12, 1, all);
        CHECK(sc.got == sc.sent && sc.wrong == 0, "IQ at %.0f S/s, strong: %d/%d (%d wrong)", fs, sc.got, sc.sent, sc.wrong);
    }
    {
        Score sc = runModem(kPlain, 30, 48000, true, 12, 2, all);
        CHECK(sc.got == sc.sent && sc.wrong == 0, "discriminator audio 48 kHz, strong: %d/%d", sc.got, sc.sent);
        sc = runModem(kPlain, 30, 44100, true, 12, 3, all, 0, true);
        CHECK(sc.got == sc.sent && sc.wrong == 0, "discriminator audio 44.1 kHz INVERTED, strong: %d/%d", sc.got, sc.sent);
        sc = runModem(kIl2p, 30, 48000, true, 12, 4, all, 0, true);
        CHECK(sc.got == sc.sent && sc.wrong == 0, "IL2P on inverted audio: %d/%d", sc.got, sc.sent);
    }
    for (int l : { kFx16, kFx32, kIl2p }) {
        Score sc = runModem(l, 30, 48000, false, 12, 5 + l, all);
        CHECK(sc.got == sc.sent && sc.wrong == 0, "%s strong: %d/%d", linkName(l), sc.got, sc.sent);
    }
    // the sweep
    const std::vector<double> eb = full ? std::vector<double>{ 6, 7, 8, 9, 10, 11, 12, 14 } : std::vector<double>{ 8, 10, 12 };
    const int B = full ? 60 : 24;
    std::printf("   %-26s", "Eb/N0 (dB)"); for (double e : eb) std::printf("%6.0f", e); std::printf("   wrong\n");
    vibeg3ruh::Opts one = all; one.slicers = 1;
    vibeg3ruh::Opts plain; plain.fx25 = false; plain.il2p = false;
    vibeg3ruh::Opts plain1 = plain; plain1.slicers = 1;
    struct Row { const char* name; int link; vibeg3ruh::Opts o; } rows[] = {
        { "AX.25, one slicer", kPlain, plain1 }, { "AX.25, five slicers", kPlain, plain }, { "FX.25 16, five", kFx16, all },
        { "FX.25 32, five", kFx32, all }, { "IL2P, five", kIl2p, all }, { "IL2P, one slicer", kIl2p, one } };
    int at10plain = 0, at10fx = 0, at10il = 0;
    for (const Row& r : rows) {
        std::printf("   %-26s", r.name);
        int wrong = 0;
        for (double e : eb) {
            Score sc = runModem(r.link, e, 48000, false, B, 100 + int(e * 10), r.o);
            std::printf("%5.0f%%", 100.0 * sc.got / std::max(1, sc.sent));
            wrong += sc.wrong;
            if (e == 10) { if (r.link == kPlain && r.o.slicers > 1) at10plain = sc.got; if (r.link == kFx32) at10fx = sc.got; if (r.link == kIl2p && r.o.slicers > 1) at10il = sc.got; }
        }
        std::printf("   %d\n", wrong);
        CHECK(wrong == 0, "%s: no wrong frames in the sweep (%d)", r.name, wrong);
    }
    CHECK(at10fx > at10plain && at10il > at10plain, "at 10 dB FX.25 32 (%d) and IL2P (%d) beat plain AX.25 (%d)", at10fx, at10il, at10plain);
    // carrier offsets (an RTL's crystal, a satellite's residual Doppler) — the AFC
    {   // a cubesat's GMSK-style 9k6 (h = 0.5: ±2.4 kHz) and a hot ±4 kHz transmitter, both at 14 dB
        for (double dev : { 2400.0, 4000.0 }) {
            vibeg3ruh::Opts o = all; o.devHz = dev;
            Score sc = runModem(kPlain, 14, 48000, false, 20, 400 + int(dev), o);
            vibeg3ruh::Opts o3 = all;   // the receiver assumes ±3 kHz; the eye tracker takes up the difference
            Score s3 = runModem(kPlain, 14, 48000, false, 20, 400 + int(dev), o3, 0, false, nullptr, nullptr, dev);
            std::printf("   deviation ±%.0f Hz at 14 dB: %d/%d frames (receiver told ±%.0f), %d/%d (receiver at its ±3 kHz default)\n", dev, sc.got, sc.sent, dev, s3.got, s3.sent);
            CHECK(s3.got >= s3.sent * 7 / 10 && s3.wrong == 0, "deviation ±%.0f with the default receiver: %d/%d", dev, s3.got, s3.sent);
        }
    }
    for (double off : { -4000.0, 2500.0, 6000.0 }) {
        double afc = 0;
        Score sc = runModem(kPlain, 16, 48000, false, 20, 300 + int(off), all, off, false, nullptr, &afc);
        CHECK(sc.got >= sc.sent - 1 && sc.wrong == 0, "carrier %+.0f Hz off at 16 dB: %d/%d, AFC %.0f Hz", off, sc.got, sc.sent, afc);
        std::printf("   carrier %+5.0f Hz off, 16 dB: %d/%d frames, the AFC settled at %+.0f Hz\n", off, sc.got, sc.sent, afc);
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
static void testNoise(double minutes) {
    std::printf("7. Noise only, every receiver on (AX.25 + FX.25 + IL2P, five slicers): %.1f min IQ + %.1f min audio\n", minutes, minutes);
    std::mt19937 rng(77);
    std::normal_distribution<float> nd(0.f, 1.f);
    vibeg3ruh::Opts o;
    int frames = 0;
    auto onF = [&](const vibepkt::PktFrame& f) { ++frames; std::printf("   NOISE FRAME %s %zu bytes\n", vibepkt::fecName(f.fec), f.bytes.size()); };
    {
        vibeg3ruh::Demod dm(o);
        const double fs = 48000; std::vector<cf32> b(4800);
        for (double t = 0; t < minutes * 60; t += 0.1) { for (auto& v : b) v = cf32(nd(rng), nd(rng)); dm.feedIq(b.data(), int(b.size()), fs, onF); }
        std::printf("   IQ: %llu FX.25 tags, %llu IL2P syncs matched by chance, 0 should become frames; eye %.2f, AFC %+.0f Hz\n", (unsigned long long)dm.fx25Tags(), (unsigned long long)dm.il2pSyncs(), dm.eye(), dm.afcHz());
        CHECK(std::fabs(dm.afcHz()) < 200, "the AFC does not wander on noise (%+.0f Hz)", dm.afcHz());
    }
    {
        vibeg3ruh::Demod dm(o);
        std::vector<float> b(4800);
        for (double t = 0; t < minutes * 60; t += 0.1) { for (auto& v : b) v = nd(rng); dm.feedAudio(b.data(), int(b.size()), 48000, onF); }
    }
    CHECK(frames == 0, "noise: %d frames", frames);
}

/** Raw line-bit error rate of the centre slicer: a long random line sequence, aligned by the best match. */
static double lineBer(double ebn0, const vibeg3ruh::Opts& o, int nbits = 40000) {
    std::mt19937 rng(42);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::vector<int> line{}; line.resize(size_t(nbits)); for (auto& b : line) b = int(rng() & 1);
    const double fs = 48000;
    std::vector<float> bb = vibeg3ruh::baseband(line, fs, o.baud, 0.5);
    std::vector<cf32> iq = vibeg3ruh::fmIq(bb, fs, o.devHz);
    const float sd = float(std::sqrt(fs / (o.baud * std::pow(10.0, ebn0 / 10)) / 2));
    for (auto& v : iq) v += cf32(sd * nd(rng), sd * nd(rng));
    vibeg3ruh::Demod dm(o);
    std::vector<int> got;
    const int centre = (dm.slicers() - 1) / 2;
    dm.levelTap = [&](int k, float l) { if (k == centre) got.push_back(l >= 0); };
    for (size_t i = 0; i < iq.size(); i += 480) dm.feedIq(iq.data() + i, int(std::min<size_t>(480, iq.size() - i)), fs, [](const vibepkt::PktFrame&) {});
    // align: try offsets 0…60 on the middle of the run, then count errors in windows to see slips
    int bestOff = 0, bestErr = 1 << 30;
    for (int off = -60; off <= 60; ++off) {
        int e = 0;
        for (int i = 2000; i < 4000; ++i) { const long j = i + off; if (j < 0 || j >= long(got.size())) { e += 1; continue; } e += got[size_t(j)] != line[size_t(i)]; }
        if (e < bestErr) { bestErr = e; bestOff = off; }
    }
    long errs = 0, n = 0; int slips = 0, off = bestOff;
    for (int i = 1000; i + 1000 < nbits && i + off + 70 < long(got.size()); i += 500) {
        // re-align each 500-bit window (a slip moves the alignment)
        int be = 1 << 30, bo = off;
        for (int d = -2; d <= 2; ++d) { int e = 0; for (int k = 0; k < 500; ++k) { const long j = i + k + off + d; if (j >= 0 && j < long(got.size())) e += got[size_t(j)] != line[size_t(i + k)]; } if (e < be) { be = e; bo = off + d; } }
        if (bo != off) ++slips;
        off = bo; errs += be; n += 500;
    }
    if (!getenv("AX25HS_QUIET")) std::printf("      Eb/N0 %4.1f dB: line BER %.2e over %ld bits, %d clock slips (%zu levels for %d bits)\n", ebn0, double(errs) / n, n, slips, got.size(), nbits);
    return double(errs) / n;
}

static void tune() {
    for (float rel : { 1.f / 1000, 1.f / 500, 1.f / 300 }) {
        vibeg3ruh::Opts all; all.eyeRelease = rel;
        int g = 0, n = 0;
        for (double fs : { 48000.0, 44100.0 }) for (int inv = 0; inv < 2; ++inv) for (int seed : { 3, 4, 5, 6 }) { Score sc = runModem(kPlain, 30, fs, true, 12, seed, all, 0, inv != 0); g += sc.got; n += sc.sent; }
        std::printf("   release 1/%.0f: audio with gaps %d/%d;", 1 / rel, g, n);
        vibeg3ruh::Opts o1 = all; o1.slicers = 1;
        for (double e : { 10.0, 8.0 }) std::printf(" BER@%.0f %.2e", e, lineBer(e, o1));
        Score a = runModem(kPlain, 10, 48000, false, 40, 77, all), f = runModem(kFx32, 8, 48000, false, 40, 78, all);
        std::printf("  AX.25@10 %d/%d FX32@8 %d/%d\n", a.got, a.sent, f.got, f.sent);
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--tune")) { tune(); return 0; }
    bool full = false; double noiseMin = 2;
    for (int i = 1; i < argc; ++i) { if (!std::strcmp(argv[i], "--full")) { full = true; noiseMin = 10; } }
    testRs();
    testFx25();
    testIl2p();
    testModem(full);
    testNoise(noiseMin);
    std::printf("\ntest-ax25hs: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
