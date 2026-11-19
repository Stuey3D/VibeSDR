// ★★ V12 DIGITAL-VOICE SIGNALLING (vibe_dvsig*.h, docs/v12/DV-SIGNALLING.md) — our own transmitter for each system,
// built from the published facts, against our own receiver, end to end. IDENTITY AND DATA ONLY: no voice anywhere.
//
//   1. The specs' own vectors: DMR Golay(20,8) = table B.11 row by row, QR(16,7) and the Hamming matrices' minimum
//      distances, RS(12,9) = table B.18 AND a codeword vanishes at α, α², α³ (the generator B.9 — two published facts
//      agree), CRC check values, the scrambling sequences = their published bytes (YSF whitening, D-STAR 70 4F 93,
//      NXDN's per-symbol table), the BPTC / embedded / D-STAR / YSF / NXDN / dPMR codes through bit errors.
//   2. The correlator kernels: NEON / SSE2 = scalar on random windows, and a whole decode run both ways.
//   3. Over the air (FM, AWGN at several C/N, frequency offsets, through the resampler at 48 kHz): DMR on a repeater
//      downlink (group call, talker alias, embedded GPS, privacy) and MS-sourced simplex (private call), DMR short data
//      (SMS), YSF DN (callsigns, DG-ID, GPS), D-STAR (header callsigns, slow-data message, D-PRS) and a late join (the
//      header rebuilt from slow data), NXDN48 and NXDN96 (RAN, unit → group), dPMR (IDs, channel code); AUTO says which
//      system; a pin hears only its own.
//   4. Noise only (minutes) and analogue FM speech (with CTCSS): ZERO entries.
//   5. The report: the identities copy and the public copy (the public one names no one).
//   6. The threaded listener.
//   7. CPU: AUTO on signal + noise against real time.
//   Tools: `test-dvsig clip OUT.cu8 [rate=288000] [sec=30] [cn=20]` writes the server bench clip (one transmission of
//          every system in turn, at +25 kHz) — scripts/test-server-dvsig.mjs replays it through a fake rtl_tcp.
//          `test-dvsig sweep [trials=10]` prints the sensitivity table in docs/v12/DV-SIGNALLING.md.
#include "vibe_dvsig_tx.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace d = vibe::dvsig;
namespace tx = vibe::dvsig::tx;
using cf = std::complex<float>;

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("   %s %s%s%s\n", ok ? "\033[32mPASS\033[0m" : "\033[31mFAIL\033[0m", what, detail.empty() ? "" : " — ", detail.c_str());
    (ok ? g_pass : g_fail)++;
}
static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char* f, ...) { char b[600]; va_list a; va_start(a, f); std::vsnprintf(b, sizeof b, f, a); va_end(a); return b; }

// ── 1 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static int minDistance(const d::Hamming& h) { int m = 99; for (uint32_t x = 1; x < (1u << h.k); ++x) m = std::min(m, d::popc(h.encode(x))); return m; }
static void specVectors() {
    std::printf("\n── 1. the specs' own vectors ──\n");
    // DMR Golay(20,8), TS 102 361-1 table B.11 (the parity half of each row, data MSB first)
    const uint16_t b11[8] = {0b111101101010, 0b110110011001, 0b011011001101, 0b001101100111, 0b110111000110, 0b101010010111, 0b100100111110, 0b100011101011};
    // ★ the table as printed: row i = data bit 7−i, columns 9..20 = the 12 parity bits
    const uint16_t b11rows[8] = {0x3DA, 0xD99, 0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB};
    bool ok = true;
    for (int i = 0; i < 8; ++i) if ((d::golay20Encode(uint8_t(0x80 >> i)) & 0xFFF) != b11rows[i]) ok = false;
    (void)b11;
    check(ok, "DMR Golay(20,8) = table B.11, all eight rows");
    { int md = 99; for (int x = 1; x < 256; ++x) md = std::min(md, d::popc(d::golay20Encode(uint8_t(x)))); check(md == 8, "Golay(20,8) minimum distance 8", fmt("%d", md)); }
    { int md = 99; for (int x = 1; x < 128; ++x) md = std::min(md, d::popc(d::qr16Encode(uint8_t(x)))); check(md == 6, "QR(16,7,6) from table B.12: minimum distance 6", fmt("%d", md)); }
    check(minDistance(d::h13_9()) == 3 && minDistance(d::h15_11()) == 3 && minDistance(d::h16_11()) == 4 && minDistance(d::h7_4()) == 3 && minDistance(d::h12_8()) >= 3,
          "Hamming (13,9,3) (15,11,3) (16,11,4) (7,4,3) and dPMR (12,8): the matrices' minimum distances",
          fmt("%d %d %d %d %d", minDistance(d::h13_9()), minDistance(d::h15_11()), minDistance(d::h16_11()), minDistance(d::h7_4()), minDistance(d::h12_8())));
    // RS(12,9): table B.18's parity, and the codeword c(x) = m(x)·x³ + p(x) has roots α, α², α³ (generator B.9/B.10)
    {
        std::mt19937 rng(7); bool roots = true;
        for (int t = 0; t < 200 && roots; ++t) {
            uint8_t m[9], p[3]; for (auto& v : m) v = uint8_t(rng()); d::rs129Parity(m, p);
            uint8_t c[12]; std::memcpy(c, m, 9); std::memcpy(c + 9, p, 3);   // c[0] = highest power (x¹¹)
            for (int r = 1; r <= 3; ++r) {
                uint8_t a = 2; for (int k = 1; k < r; ++k) a = d::gfMul(a, 2);   // α^r
                uint8_t acc = 0; for (int i = 0; i < 12; ++i) acc = uint8_t(d::gfMul(acc, a) ^ c[i]);
                if (acc) roots = false;
            }
        }
        check(roots, "RS(12,9): table B.18's codewords vanish at α, α², α³ — the generator matrix and g(x) agree");
    }
    { const char* s = "123456789"; check(d::crcCcittMsb(reinterpret_cast<const uint8_t*>(s), 9) == 0xCE3C, "CRC-CCITT (MSB first, 0, inverted): check value 0xCE3C", fmt("%04X", d::crcCcittMsb(reinterpret_cast<const uint8_t*>(s), 9)));
      check(d::crcX25(reinterpret_cast<const uint8_t*>(s), 9) == 0x906E, "CRC-16 X.25 (D-STAR P_FCS): check value 0x906E", fmt("%04X", d::crcX25(reinterpret_cast<const uint8_t*>(s), 9))); }
    {   // the scrambling sequences = their published bytes
        uint8_t w[20] = {}; d::ysfWhiten(w, 20);
        const uint8_t pub[20] = {0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F, 0xF8, 0x3D, 0xF1, 0x73, 0x20, 0x94, 0xED, 0x1E, 0x7C, 0xD8};
        check(std::memcmp(w, pub, 20) == 0, "YSF whitening = x⁹+x⁵+1 reproduces the 20 published bytes");
        const d::Bits& pn = d::dstarPn(); uint8_t o[3] = {};
        for (int i = 0; i < 24; ++i) if (pn[size_t(i)]) o[i >> 3] |= uint8_t(1u << (i & 7));
        check(o[0] == 0x70 && o[1] == 0x4F && o[2] == 0x93, "D-STAR x⁷+x⁴+1 from all ones = the slow-data bytes 70 4F 93", fmt("%02X %02X %02X", o[0], o[1], o[2]));
        const d::Bits& nx = d::nxdnPn(); uint8_t q[6] = {};
        for (int k = 0; k < 24; ++k) if (nx[size_t(k)]) q[(2 * k) >> 3] |= uint8_t(0x80u >> ((2 * k) & 7));
        check(q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 0x82 && q[4] == 0xA0 && q[5] == 0x88, "NXDN per-symbol scrambling reproduces the published 00 00 00 82 A0 88");
    }
    // BPTC(196,96) through errors
    {
        std::mt19937 rng(11); int okn = 0, tried = 0;
        for (int t = 0; t < 300; ++t) {
            uint8_t info[96], txb[196], out[96]; for (auto& v : info) v = uint8_t(rng() & 1);
            d::bptc196Encode(info, txb);
            const int ne = t % 4;   // 0..3 random errors
            for (int e = 0; e < ne; ++e) txb[1 + rng() % 195] ^= 1;
            ++tried; int fx = 0;
            if (d::bptc196Decode(txb, out, &fx) && std::memcmp(info, out, 96) == 0) ++okn;
        }
        check(okn >= 296, "BPTC(196,96): 300 blocks with 0–3 random bit errors decode exactly", fmt("%d/%d", okn, tried));
        uint8_t lc[9] = {0x00, 0x00, 0x20, 0x00, 0x00, 0xEB, 0x23, 0xCA, 0xCE}, f[4][32], o[9];
        d::embEncode(lc, f); f[1][7] ^= 1; f[3][20] ^= 1;
        check(d::embDecode(f, o, nullptr) && std::memcmp(lc, o, 9) == 0, "embedded LC BPTC + 5-bit checksum: two scattered errors corrected");
        f[0][0] ^= 1; f[0][8] ^= 1; f[0][16] ^= 1;
        check(!d::embDecode(f, o, nullptr) || std::memcmp(lc, o, 9) != 0 ? true : false, "embedded LC: a burst of errors in one row is refused, never miscorrected into another LC");
    }
    {   // D-STAR header
        tx::DstarCall c; uint8_t h[41], o[41]; tx::dsHeader41(c, h);
        d::Bits b = d::dstarHeaderEncode(h);
        std::mt19937 rng(3);
        std::vector<int8_t> s(660); for (int i = 0; i < 660; ++i) s[size_t(i)] = b[size_t(i)] ? 60 : -60;
        for (int e = 0; e < 25; ++e) { const int i = int(rng() % 660); s[size_t(i)] = int8_t(-s[size_t(i)]); }
        int errs = 0;
        check(d::dstarHeaderDecode(s.data(), o, &errs) && std::memcmp(h, o, 41) == 0, "D-STAR header: K=3 + interleave + scrambler through 25 bit errors", fmt("%d corrected", errs));
    }
    {   // YSF FICH and a DCH
        uint8_t f[4] = {0x40, 0x3F, 0x02, 0x05}, o[4];
        d::Bits b = d::ysfFichEncode(f); std::vector<int8_t> s(200); for (int i = 0; i < 200; ++i) s[size_t(i)] = b[size_t(i)] ? 50 : -50;
        for (int i : {3, 77, 140, 199}) s[size_t(i)] = int8_t(-s[size_t(i)]);
        int e = 0;
        check(d::ysfFichDecode(s.data(), o, &e) && std::memcmp(f, o, 4) == 0, "YSF FICH: Golay ×4 + CRC through the K=5 Viterbi with 4 errors");
    }
    {   // NXDN SACCH and FACCH1
        std::mt19937 rng(5); int good = 0;
        for (int t = 0; t < 50; ++t) {
            uint8_t in[92], out[92]; for (int i = 0; i < 80; ++i) in[i] = uint8_t(rng() & 1); d::putBits(in, 80, 12, d::nxdnCrc12(in, 80));
            const d::Bits b = d::nxdnEncode(in, 92, true); std::vector<int8_t> s(144); for (int i = 0; i < 144; ++i) s[size_t(i)] = b[size_t(i)] ? 50 : -50;
            for (int e = 0; e < 2; ++e) { const int i = int(rng() % 144); s[size_t(i)] = int8_t(-s[size_t(i)]); }
            d::nxdnDecode(s.data(), 92, true, out);
            if (std::memcmp(in, out, 92) == 0 && d::nxdnCrc12(out, 80) == d::takeBits(out, 80, 12)) ++good;
        }
        // ★ FACCH1 is K=5 punctured to rate ⅔: its free distance makes a few 2-error patterns (hard decisions) fatal
        check(good >= 47, "NXDN FACCH1: 50 blocks through puncturing and 2 hard errors (≥ 47 exact)", fmt("%d/50", good));
        uint8_t si[32] = {}, so[32]; d::putBits(si, 0, 8, 0x45); d::putBits(si, 26, 6, d::nxdnCrc6(si, 26));
        const d::Bits b = d::nxdnEncode(si, 32, false); std::vector<int8_t> s(60); for (int i = 0; i < 60; ++i) s[size_t(i)] = b[size_t(i)] ? 50 : -50;
        s[10] = int8_t(-s[10]);
        d::nxdnDecode(s.data(), 32, false, so);
        check(std::memcmp(si, so, 32) == 0, "NXDN SACCH: puncturing + 1 error");
    }
    {   // dPMR
        uint8_t hi[80] = {}, o[80]; d::putBits(hi, 4, 24, 0x123456); d::putBits(hi, 72, 8, d::dpmrCrc8(hi, 72));
        d::Bits b = d::dpmrBlockEncode(hi, 80); b[5] ^= 1; b[61] ^= 1;
        int fx = 0;
        check(d::dpmrBlockDecode(b.data(), 10, o, &fx) && std::memcmp(hi, o, 80) == 0 && fx == 2, "dPMR header: Hamming(12,8) ×10 + 12×10 interleave + PN, 2 errors in different octets");
    }
}

// ── 2 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void kernels() {
    std::printf("\n── 2. the correlator kernels (%s) ──\n", d::simdName());
    d::Path pa(d::patsA()), pb(d::patsB());
    std::mt19937 rng(1); bool same = true;
    for (int t = 0; t < 20000 && same; ++t) {
        int16_t w[24]; for (auto& v : w) v = int16_t(int(rng() % 65535) - 32767);
        for (int blk = 0; blk < 3; ++blk) { int32_t a[4], b[4]; pa.dots(w, blk, d::Kern::Scalar, a); pa.dots(w, blk, d::Kern::Auto, b); if (std::memcmp(a, b, sizeof a)) same = false; }
        for (int blk = 0; blk < 2; ++blk) { int32_t a[4], b[4]; pb.dots(w, blk, d::Kern::Scalar, a); pb.dots(w, blk, d::Kern::Auto, b); if (std::memcmp(a, b, sizeof a)) same = false; }
    }
    check(same, "20 000 random full-scale windows × every block: the vector kernel = the scalar one, bit for bit");
}

// ── helpers ──────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Run { std::vector<d::Entry> log; d::Receiver::Counts c; std::string ids, pub; long cands; double sec; };
static Run decode(std::vector<cf> iq, double cn, uint32_t seed, d::Sys pin = d::Sys::None, double rate = 24000, d::Kern k = d::Kern::Auto, double lead = 0.3) {
    std::vector<cf> v; tx::silence(v, lead); v.insert(v.end(), iq.begin(), iq.end()); tx::silence(v, 0.5);
    if (cn < 90) tx::awgn(v, cn, 9000, seed);
    d::Receiver rx; rx.setClock(0); rx.setKernel(k); rx.setPin(pin);
    const auto t0 = std::chrono::steady_clock::now();
    if (rate != 24000) {
        // ★ through the front end's resampler: up to `rate` by linear interpolation of an oversampled copy is not
        //   exact, so the test resamples with vibedsp itself (the same resampler, the other way)
        vibedsp::RationalResampler ri(24000.0, int(rate), vibedsp::RationalResampler::ExactRate{}), rq(24000.0, int(rate), vibedsp::RationalResampler::ExactRate{});
        std::vector<float> re(v.size()), im(v.size()); for (size_t i = 0; i < v.size(); ++i) { re[i] = v[i].real(); im[i] = v[i].imag(); }
        std::vector<float> a(size_t(ri.maxOut(int(v.size())))), b(a.size());
        const int na = ri.process(re.data(), int(re.size()), a.data()), nb = rq.process(im.data(), int(im.size()), b.data());
        std::vector<cf> u(size_t(std::min(na, nb))); for (size_t i = 0; i < u.size(); ++i) u[i] = cf(a[i], b[i]);
        const int blk = int(rate / 100);
        for (size_t i = 0; i < u.size(); i += size_t(blk)) rx.feedIq(u.data() + i, int(std::min<size_t>(size_t(blk), u.size() - i)), rate);
    } else {
        for (size_t i = 0; i < v.size(); i += 240) rx.feed24k(v.data() + i, int(std::min<size_t>(240, v.size() - i)));
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Run r; r.log.assign(rx.log().begin(), rx.log().end()); r.c = rx.counts(); r.ids = rx.json(true, "70 cm amateur"); r.pub = rx.json(false, ""); r.cands = rx.candidates(); r.sec = sec;
    return r;
}
static const d::Entry* find(const Run& r, d::Sys s, const char* kind = nullptr) { for (const auto& e : r.log) if (e.sys == s && (!kind || e.kind == kind)) return &e; return nullptr; }
static std::string desc(const Run& r) {
    std::string o;
    for (const auto& e : r.log) o += fmt("[%s %s slot%d cc%d ids%d %u→%u '%s'→'%s' via '%s' alias '%s' text '%s' gps%d %.4f,%.4f enc%d open%d] ", d::sysName(e.sys), e.kind.c_str(), e.slot, e.cc, e.ids, e.src, e.dst, e.from.c_str(), e.to.c_str(), e.via1.c_str(), e.alias.c_str(), e.text.c_str(), e.hasGps, e.lat, e.lon, e.enc, e.open);
    return o.empty() ? "(nothing)" : o;
}

// ── 3 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void overTheAir() {
    std::printf("\n── 3. over the air ──\n");
    // DMR, a repeater's downlink: group call, alias, GPS
    tx::DmrCall c; c.cc = 3; c.slot = 2; c.src = 2345678; c.dst = 2350; c.alias = "G4ABC Stuart"; c.gps = true; c.lat = 52.62345; c.lon = -1.12345; c.superframes = 8;
    for (double cn : {30.0, 18.0, 14.0, 12.0}) {
        const Run r = decode(tx::dmrIq(c, true, 1), cn, 100 + uint32_t(cn));
        const d::Entry* e = find(r, d::Sys::Dmr, "voice");
        const bool ok = e && e->ids && e->group && e->src == c.src && e->dst == c.dst && e->cc == 3 && e->slot == 2 && !e->open;
        const bool extra = e && e->alias == c.alias && e->hasGps && std::fabs(e->lat - c.lat) < 1e-4 && std::fabs(e->lon - c.lon) < 1e-4;
        if (cn >= 14) check(ok && extra, fmt("DMR downlink at %.0f dB C/N: TG 2350 from 2345678, CC 3, slot 2, alias and GPS, closed by the terminator", cn).c_str(), ok && extra ? fmt("bursts %ld data %ld emb %ld lc %ld", r.c.dmrBursts, r.c.dmrData, r.c.dmrEmb, r.c.dmrLc) : desc(r));
        else std::printf("   (measured) DMR at %.0f dB: %s — %s\n", cn, ok ? (extra ? "all" : "call, not all extras") : "not decoded", desc(r).c_str());
    }
    { const Run r = decode(tx::dmrIq(c, true, 2, 1200.0), 20, 7); const d::Entry* e = find(r, d::Sys::Dmr, "voice"); check(e && e->src == c.src && e->dst == c.dst, "DMR 1.2 kHz off the dial", desc(r)); }
    { const Run r = decode(tx::dmrIq(c, true, 2, -1500.0), 20, 8, d::Sys::None, 48000); const d::Entry* e = find(r, d::Sys::Dmr, "voice"); check(e && e->src == c.src && e->alias == c.alias, "DMR −1.5 kHz, fed at 48 kHz through the front end's resampler", desc(r)); }
    {   // MS-sourced simplex, private call, privacy
        tx::DmrCall m; m.cc = 7; m.group = false; m.privacy = true; m.src = 2341111; m.dst = 2342222; m.superframes = 5;
        const Run r = decode(tx::dmrIq(m, false, 3), 22, 9);
        const d::Entry* e = find(r, d::Sys::Dmr, "voice");
        check(e && !e->group && e->src == m.src && e->dst == m.dst && e->cc == 7 && e->enc, "DMR MS-sourced (simplex): a private call 2341111 → 2342222, CC 7, marked encrypted", desc(r));
    }
    {   // short data
        tx::DmrCall m; m.cc = 1; m.slot = 1; m.group = false; m.src = 2340001; m.dst = 2340002; m.sms = "Hello from G4ABC on DMR";
        const Run r = decode(tx::dmrIq(m, true, 4), 22, 10);
        const d::Entry* e = find(r, d::Sys::Dmr, "sms");
        check(e && e->text == m.sms && e->src == m.src && e->dst == m.dst, "DMR Short Data (defined, ISO 8859-1): the text, rate ½ blocks + CRC-32", desc(r));
    }
    // YSF
    tx::YsfCall y; y.src = "G4ABC"; y.dst = "ALL"; y.down = "GB7XX"; y.dg = 23; y.gps = true; y.lat = 52.6235; y.lon = -1.1235; y.frames = 24;
    for (double cn : {30.0, 16.0, 13.0}) {
        const Run r = decode(tx::ysfIq(y, 5), cn, 200 + uint32_t(cn));
        const d::Entry* e = find(r, d::Sys::Ysf);
        const bool ok = e && e->from == "G4ABC" && e->to == "ALL" && e->via1 == "GB7XX" && e->dg == 23 && e->mode == "DN" && !e->open;
        const bool gps = e && e->hasGps && std::fabs(e->lat - 52.6235) < 2e-4 && std::fabs(e->lon + 1.1235) < 2e-4;
        if (cn >= 16) check(ok && gps, fmt("YSF DN at %.0f dB: G4ABC → ALL via GB7XX, DG-ID 23, GPS (Mic-E)", cn).c_str(), ok && gps ? fmt("FICH %ld", r.c.ysfFich) : desc(r));
        else std::printf("   (measured) YSF at %.0f dB: %s — %s\n", cn, ok ? (gps ? "all" : "callsigns") : "not decoded", desc(r).c_str());
    }
    // D-STAR
    tx::DstarCall ds; ds.dprs = "$$CRC0000,G4ABC-7>API51,DSTAR*:!5237.41N/00107.41W>VibeSDR";
    { // the D-PRS line with its real CRC
        const std::string body = ds.dprs.substr(ds.dprs.find(',') + 1);
        char hex[8]; std::snprintf(hex, sizeof hex, "%04X", d::crcX25(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
        ds.dprs.replace(5, 4, hex);
    }
    for (double cn : {30.0, 16.0, 12.0}) {
        const Run r = decode(tx::dstarIq(ds, 6), cn, 300 + uint32_t(cn));
        const d::Entry* e = find(r, d::Sys::Dstar);
        const bool ok = e && e->from == "G4ABC/ID51" && e->to == "CQCQCQ" && e->via1 == "GB7XX  B" && e->via2 == "GB7XX  G" && !e->open;
        const bool more = e && e->text == "Hello from VibeSDR" && e->hasGps && std::fabs(e->lat - (52 + 37.41 / 60)) < 1e-4 && std::fabs(e->lon + (1 + 7.41 / 60)) < 1e-4;
        if (cn >= 16) check(ok && more, fmt("D-STAR at %.0f dB: MY G4ABC/ID51, UR CQCQCQ, RPT1/RPT2, the slow-data message and a D-PRS position", cn).c_str(), ok && more ? fmt("headers %ld", r.c.dsHdr) : desc(r));
        else std::printf("   (measured) D-STAR at %.0f dB: %s — %s\n", cn, ok ? (more ? "all" : "header") : "not decoded", desc(r).c_str());
    }
    {   // a late join: no header — the callsigns from the header repeated in the slow data
        tx::DstarCall late = ds; late.header = false; late.frames = 126;
        const Run r = decode(tx::dstarIq(late, 7), 22, 11);
        const d::Entry* e = find(r, d::Sys::Dstar);
        check(e && e->from == "G4ABC/ID51" && e->to == "CQCQCQ", "D-STAR late join: two data syncs 420 ms apart, then the header from slow data", desc(r));
    }
    // NXDN
    tx::NxdnCall nx; nx.ran = 5; nx.src = 4321; nx.dst = 77;
    { const Run r = decode(tx::nxdnIq(nx, 8, false), 22, 12); const d::Entry* e = find(r, d::Sys::Nxdn);
      check(e && e->ids && e->src == 4321 && e->dst == 77 && e->group && e->cc == 5 && e->mode == "NXDN48" && !e->open, "NXDN48: RAN 5, unit 4321 → group 77, closed by TX_REL", desc(r) + fmt(" sacch %ld facch %ld", r.c.nxSacch, r.c.nxFacch)); }
    { const Run r = decode(tx::nxdnIq(nx, 9, true), 22, 13); const d::Entry* e = find(r, d::Sys::Nxdn);
      check(e && e->src == 4321 && e->dst == 77 && e->mode == "NXDN96", "NXDN96 (4800 Bd): the same call", desc(r)); }
    // dPMR
    tx::DpmrCall pm;
    { const Run r = decode(tx::dpmrIq(pm, 10), 22, 14); const d::Entry* e = find(r, d::Sys::Dpmr);
      check(e && e->ids && e->src == pm.own && e->dst == pm.called && e->cc == pm.cc && !e->open, "dPMR: own ID 1234567 → called 7654321, channel code 0x28A, closed by the end frame", desc(r) + fmt(" hdr %ld cch %ld", r.c.pmHdr, r.c.pmCch)); }
    // pins
    { const Run r = decode(tx::ysfIq(y, 5), 25, 15, d::Sys::Dmr); check(r.log.empty(), "pinned to DMR: a YSF transmission is not reported", desc(r)); }
    { const Run r = decode(tx::ysfIq(y, 5), 25, 15, d::Sys::Ysf); check(find(r, d::Sys::Ysf) != nullptr, "pinned to YSF: it is"); }
    // kernels on a whole decode
    { const Run a = decode(tx::dmrIq(c, true, 1), 15, 99, d::Sys::None, 24000, d::Kern::Scalar), b = decode(tx::dmrIq(c, true, 1), 15, 99, d::Sys::None, 24000, d::Kern::Auto);
      check(a.ids == b.ids && a.cands == b.cands, "a whole DMR decode at 15 dB: scalar and vector correlators give the same candidates and the same report", fmt("%ld / %ld candidates", a.cands, b.cands)); }
}

// ── 4 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static std::vector<cf> fmSpeech(double sec, uint32_t seed, bool ctcss) {
    // an analogue NFM voice: a gliding harmonic series through two moving formants, in syllables, ±2.5 kHz peak deviation
    const int fs = 24000; const size_t n = size_t(sec * fs);
    std::vector<cf> v(n); std::mt19937 rng(seed); std::normal_distribution<double> nd(0, 1);
    double ph = 0, vph = 0, lp = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / fs;
        const double f0 = 110 + 40 * std::sin(2 * M_PI * 0.6 * t + seed), f1 = 550 + 300 * std::sin(2 * M_PI * 1.1 * t), f2 = 1600 + 600 * std::sin(2 * M_PI * 0.8 * t + 1);
        vph += 2 * M_PI * f0 / fs;
        double s = 0;
        for (int h = 1; h * f0 < 3000; ++h) { const double fh = h * f0; s += (std::exp(-std::pow((fh - f1) / 180, 2)) + 0.6 * std::exp(-std::pow((fh - f2) / 260, 2)) + 0.04) * std::sin(h * vph); }
        const double syl = std::max(0.0, std::sin(2 * M_PI * 3.3 * t));
        lp = 0.9 * lp + 0.1 * nd(rng);
        double dev = 2200 * syl * s / 3 + 80 * lp;
        if (ctcss) dev += 300 * std::sin(2 * M_PI * 88.5 * t);
        dev = std::max(-2500.0, std::min(2500.0, dev));
        ph += 2 * M_PI * dev / fs;
        v[i] = cf(float(std::cos(ph)), float(std::sin(ph)));
    }
    return v;
}
static void noiseOnly(double noiseSec, double speechSec) {
    std::printf("\n── 4. noise and analogue FM: nothing ──\n");
    {
        d::Receiver rx; rx.setClock(0);
        std::mt19937 rng(42); std::normal_distribution<float> nd(0, 1);
        std::vector<cf> b(2400);
        const auto t0 = std::chrono::steady_clock::now();
        for (double t = 0; t < noiseSec; t += 0.1) { for (auto& z : b) z = cf(nd(rng), nd(rng)); rx.feed24k(b.data(), int(b.size())); }
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(rx.log().empty(), fmt("%.0f s of white noise: no entry", noiseSec).c_str(), fmt("%zu entries, %ld candidates looked at, %.2f%% of a core", rx.log().size(), rx.candidates(), 100 * el / noiseSec));
    }
    {
        d::Receiver rx; rx.setClock(0);
        std::vector<cf> v = fmSpeech(speechSec, 3, false); tx::awgn(v, 20, 9000, 5);
        for (size_t i = 0; i < v.size(); i += 240) rx.feed24k(v.data() + i, int(std::min<size_t>(240, v.size() - i)));
        check(rx.log().empty(), fmt("%.0f s of analogue NFM speech at 20 dB: no entry", speechSec).c_str(), fmt("%zu entries, %ld candidates", rx.log().size(), rx.candidates()));
    }
    {
        d::Receiver rx; rx.setClock(0);
        std::vector<cf> v = fmSpeech(speechSec / 2, 8, true); tx::awgn(v, 30, 9000, 6);
        for (size_t i = 0; i < v.size(); i += 240) rx.feed24k(v.data() + i, int(std::min<size_t>(240, v.size() - i)));
        check(rx.log().empty(), fmt("%.0f s of NFM speech with an 88.5 Hz CTCSS tone: no entry", speechSec / 2).c_str(), fmt("%zu entries", rx.log().size()));
    }
    {   // an unmodulated carrier, and a carrier with a 1 kHz tone (a repeater's courtesy tone)
        d::Receiver rx; rx.setClock(0);
        std::vector<cf> v(size_t(24000 * 20)); double ph = 0;
        for (size_t i = 0; i < v.size(); ++i) { const double t = double(i) / 24000; ph += 2 * M_PI * (i < v.size() / 2 ? 0.0 : 2500 * std::sin(2 * M_PI * 1000 * t)) / 24000; v[i] = cf(float(std::cos(ph)), float(std::sin(ph))); }
        tx::awgn(v, 25, 9000, 7);
        for (size_t i = 0; i < v.size(); i += 240) rx.feed24k(v.data() + i, int(std::min<size_t>(240, v.size() - i)));
        check(rx.log().empty(), "20 s of a carrier, plain and with a 1 kHz tone: no entry");
    }
}

// ── 5 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void report() {
    std::printf("\n── 5. the report: identities and public copies ──\n");
    tx::DmrCall c; c.cc = 2; c.slot = 1; c.alias = "M0XYZ"; c.gps = true; c.lat = 51.5; c.lon = -0.12;
    const Run r = decode(tx::dmrIq(c, true, 21), 25, 31);
    check(r.ids.find("\"src\":2345678") != std::string::npos && r.ids.find("\"alias\":\"M0XYZ\"") != std::string::npos && r.ids.find("\"gps\"") != std::string::npos && r.ids.find("\"amateur\":true") != std::string::npos,
          "identities copy: src, alias, GPS, the band", r.ids.substr(0, 300));
    const bool clean = r.pub.find("src") == std::string::npos && r.pub.find("dst") == std::string::npos && r.pub.find("alias") == std::string::npos &&
                       r.pub.find("gps") == std::string::npos && r.pub.find("M0XYZ") == std::string::npos && r.pub.find("2345678") == std::string::npos;
    check(clean && r.pub.find("\"cc\":2") != std::string::npos && r.pub.find("\"slot\":1") != std::string::npos && r.pub.find("\"sys\":\"DMR\"") != std::string::npos,
          "public copy: DMR heard, CC 2, slot 1 — no ID, no alias, no position", r.pub.substr(0, 300));
    tx::DstarCall ds;
    const Run q = decode(tx::dstarIq(ds, 22), 25, 32);
    check(q.pub.find("G4ABC") == std::string::npos && q.pub.find("GB7XX") == std::string::npos && q.pub.find("Hello") == std::string::npos && q.ids.find("G4ABC") != std::string::npos,
          "D-STAR public copy names no callsign and carries no text");
}

// ── 6 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void listener() {
    std::printf("\n── 6. the threaded listener ──\n");
    tx::YsfCall y; y.frames = 12;
    std::vector<cf> v; tx::silence(v, 0.3); auto s = tx::ysfIq(y, 41); v.insert(v.end(), s.begin(), s.end()); tx::silence(v, 0.5); tx::awgn(v, 22, 9000, 41);
    d::Listener L;
    for (size_t i = 0; i < v.size(); i += 480) {
        L.feedIq(v.data() + i, int(std::min<size_t>(480, v.size() - i)), 24000.0);
        while (L.queuedFrames() > 12000) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // ★ no faster than it decodes
    }
    std::string ids, pub; uint64_t ver = 0;
    for (int w = 0; w < 400; ++w) { L.status("2 m amateur", &ids, &pub, &ver); if (ids.find("G4ABC") != std::string::npos && L.queuedFrames() == 0) break; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    check(ids.find("\"from\":\"G4ABC\"") != std::string::npos && ids.find("\"amateur\":true,\"band\":\"2 m amateur\"") != std::string::npos, "the listener's identities copy, with the caller's band", ids.substr(0, 200));
    check(pub.find("G4ABC") == std::string::npos && pub.find("\"sys\":\"YSF\"") != std::string::npos, "its public copy");
    check(L.droppedBlocks() == 0, "no block dropped");
}

// ── 7 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static void cpu() {
    std::printf("\n── 7. CPU (AUTO, every system's correlators) ──\n");
    tx::DmrCall c; c.superframes = 40;
    std::vector<cf> v = tx::dmrIq(c, true, 51);
    tx::DstarCall ds; ds.frames = 400; auto b = tx::dstarIq(ds, 52); v.insert(v.end(), b.begin(), b.end());
    tx::awgn(v, 18, 9000, 53);
    const double sec = double(v.size()) / 24000;
    for (int k = 0; k < 2; ++k) {
        d::Receiver rx; rx.setKernel(k ? d::Kern::Auto : d::Kern::Scalar);
        const auto t0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < v.size(); i += 240) rx.feed24k(v.data() + i, int(std::min<size_t>(240, v.size() - i)));
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("   %s correlators: %.1f s of DMR + D-STAR in %.3f s = %.2f %% of one core (%zu entries)\n", k ? d::simdName() : "scalar", sec, el, 100 * el / sec, rx.log().size());
        if (k) check(el / sec < 0.10, "AUTO costs under 10 % of one core here", fmt("%.2f %%", 100 * el / sec));
    }
}

// ── the bench clip for the server test ───────────────────────────────────────────────────────────────────────────────
static int writeClip(const char* path, int rate, double sec, double cn) {
    std::vector<cf> v; tx::silence(v, 1.0);
    auto add = [&](std::vector<cf> s) { v.insert(v.end(), s.begin(), s.end()); tx::silence(v, 1.5); };
    tx::DmrCall c; c.cc = 1; c.slot = 1; c.alias = "G4ABC Stuart"; c.gps = true; c.lat = 52.6235; c.lon = -1.1235; c.superframes = 10;
    add(tx::dmrIq(c, true, 61));
    tx::YsfCall y; y.gps = true; y.lat = 52.6235; y.lon = -1.1235; y.frames = 24; add(tx::ysfIq(y, 62));
    tx::DstarCall ds; add(tx::dstarIq(ds, 63));
    tx::NxdnCall nx; add(tx::nxdnIq(nx, 64));
    tx::DpmrCall pm; add(tx::dpmrIq(pm, 65));
    while (double(v.size()) / 24000 < sec) tx::silence(v, 0.5);
    tx::awgn(v, cn, 9000, 66);
    // up to `rate` at +25 kHz, as unsigned 8-bit IQ (rtl_tcp)
    vibedsp::RationalResampler ri(24000.0, rate, vibedsp::RationalResampler::ExactRate{}), rq(24000.0, rate, vibedsp::RationalResampler::ExactRate{});
    std::vector<float> re(v.size()), im(v.size()); for (size_t i = 0; i < v.size(); ++i) { re[i] = v[i].real(); im[i] = v[i].imag(); }
    std::vector<float> a(size_t(ri.maxOut(int(v.size())))), b(a.size());
    const int n = std::min(ri.process(re.data(), int(re.size()), a.data()), rq.process(im.data(), int(im.size()), b.data()));
    FILE* f = std::fopen(path, "wb"); if (!f) return 1;
    std::mt19937 rng(9); std::normal_distribution<float> nd(0, 0.02f);
    double ph = 0; std::vector<uint8_t> o(size_t(2 * n));
    for (int i = 0; i < n; ++i) {
        ph += 2 * M_PI * 25000.0 / rate;
        const cf z = cf(a[size_t(i)], b[size_t(i)]) * cf(float(std::cos(ph)), float(std::sin(ph))) * 0.35f + cf(nd(rng), nd(rng));
        o[size_t(2 * i)] = uint8_t(std::max(0.f, std::min(255.f, 127.5f + 127.f * z.real())));
        o[size_t(2 * i + 1)] = uint8_t(std::max(0.f, std::min(255.f, 127.5f + 127.f * z.imag())));
    }
    std::fwrite(o.data(), 1, o.size(), f); std::fclose(f);
    std::printf("wrote %s: %.1f s at %d S/s, %.0f dB C/N, +25 kHz\n", path, double(n) / rate, rate, cn);
    return 0;
}

/** `test-dvsig sweep [trials]`: per system, how many trials decode the identity, by C/N (9 kHz) — the doc's table. */
static int sweep(int trials) {
    std::printf("C/N in 9 kHz, %d trials a point: identity decoded (DMR: src+TG; YSF / D-STAR: the source callsign; NXDN: unit+group; dPMR: both IDs)\n", trials);
    std::printf("%6s %6s %6s %6s %6s %6s %6s\n", "C/N", "DMR", "DMR al", "YSF", "D-STAR", "NXDN48", "dPMR");
    for (double cn : {6.0, 8.0, 10.0, 12.0, 14.0, 16.0, 20.0}) {
        int n[6] = {};
        for (int t = 0; t < trials; ++t) {
            const uint32_t sd = uint32_t(1000 * cn + t);
            tx::DmrCall c; c.alias = "G4ABC Stuart"; c.superframes = 6;
            { const Run r = decode(tx::dmrIq(c, true, sd), cn, sd + 1); const d::Entry* e = find(r, d::Sys::Dmr, "voice");
              if (e && e->src == c.src && e->dst == c.dst) ++n[0]; if (e && e->alias == c.alias) ++n[1]; }
            tx::YsfCall y; y.frames = 16;
            { const Run r = decode(tx::ysfIq(y, sd), cn, sd + 2); const d::Entry* e = find(r, d::Sys::Ysf); if (e && e->from == "G4ABC") ++n[2]; }
            tx::DstarCall ds; ds.frames = 42;
            { const Run r = decode(tx::dstarIq(ds, sd), cn, sd + 3); const d::Entry* e = find(r, d::Sys::Dstar); if (e && e->from == "G4ABC/ID51") ++n[3]; }
            tx::NxdnCall nx;
            { const Run r = decode(tx::nxdnIq(nx, sd, false), cn, sd + 4); const d::Entry* e = find(r, d::Sys::Nxdn); if (e && e->src == nx.src && e->dst == nx.dst) ++n[4]; }
            tx::DpmrCall pm;
            { const Run r = decode(tx::dpmrIq(pm, sd), cn, sd + 5); const d::Entry* e = find(r, d::Sys::Dpmr); if (e && e->src == pm.own && e->dst == pm.called) ++n[5]; }
        }
        std::printf("%4.0f dB %6d %6d %6d %6d %6d %6d\n", cn, n[0], n[1], n[2], n[3], n[4], n[5]);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "sweep") return sweep(argc > 2 ? std::atoi(argv[2]) : 10);
    if (argc > 2 && std::string(argv[1]) == "clip") return writeClip(argv[2], argc > 3 ? std::atoi(argv[3]) : 288000, argc > 4 ? std::atof(argv[4]) : 30, argc > 5 ? std::atof(argv[5]) : 20);
    const bool quick = std::getenv("VIBE_DVSIG_QUICK") != nullptr;
    std::printf("DV signalling — correlators %s; Viterbi %s\n", d::simdName(), vibe::m17::acsName());
    specVectors();
    kernels();
    overTheAir();
    const char* ns = std::getenv("VIBE_DVSIG_NOISE_SEC");
    noiseOnly(ns ? std::atof(ns) : quick ? 20 : 180, quick ? 10 : 60);
    report();
    listener();
    cpu();
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
