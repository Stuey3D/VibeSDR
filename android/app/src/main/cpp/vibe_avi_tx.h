// vibe_avi_tx.h — V12 aviation TRANSMITTERS: ACARS (AM-MSK) and VDL2 (D8PSK, ACARS over AVLC) bursts written into a
// synthetic capture. Our own code (the receivers' layouts run backwards). Used by the tests (test-acars, test-vdl2 — and
// test-vdl2 --write-clip for the server's end-to-end test) and by the benchmark's datalink rows (vibe_benchmark_more.h),
// which time the real receivers on real-shaped traffic. Not on any receive path.
#pragma once
#include "vibe_acars.h"
#include "vibe_vdl2.h"
#include "vibe_hfdl.h"

#include <array>
#include <cmath>
#include <complex>
#include <random>
#include <cstdint>
#include <string>
#include <vector>

namespace vibeavitx {
using cf32 = vibeavi::cf32;

// ── ACARS ────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ── a block's bytes, as an aircraft would send them ────────────────────────────────────────────────────────────────
inline std::vector<uint8_t> makeBlock(char mode, const std::string& reg7, char ack, const std::string& label, char blk,
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
inline std::vector<int> airBits(const std::vector<uint8_t>& blk, int prekeyBits = 128) {
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
inline void acarsBurst(std::vector<cf32>& cap, double fs, double offHz, double A, size_t s0, const std::vector<int>& bits,
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


// ── VDL2 ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline void putAddr(std::vector<uint8_t>& f, uint32_t addr, int type, int status, bool last) {
    const uint32_t r = (uint32_t(status) << 27) | (uint32_t(type) << 24) | (addr & 0xFFFFFFu);
    uint32_t raw = 0;
    for (int i = 0; i < 28; ++i) raw |= ((r >> i) & 1u) << (27 - i);
    for (int k = 0; k < 4; ++k) f.push_back(uint8_t(((raw >> (7 * k)) & 0x7F) << 1 | (last && k == 3 ? 1 : 0)));
}
inline std::vector<uint8_t> avlcAcars(uint32_t src, uint32_t dst, const std::string& reg7, const std::string& label, char blk, const std::string& text) {
    std::vector<uint8_t> f;
    putAddr(f, dst, 4, 0, false);
    putAddr(f, src, 1, 0, true);
    f.push_back(0x10);                                  // an I frame
    f.push_back(0xFF); f.push_back(0xFF); f.push_back(0x01);
    std::vector<uint8_t> a;
    a.push_back(vibeacars::withParity('2'));
    for (char c : reg7) a.push_back(vibeacars::withParity(uint8_t(c)));
    a.push_back(vibeacars::withParity(0x15));
    for (char c : label) a.push_back(vibeacars::withParity(uint8_t(c)));
    a.push_back(vibeacars::withParity(uint8_t(blk)));
    a.push_back(vibeacars::withParity(vibeacars::STX));
    for (char c : text) a.push_back(vibeacars::withParity(uint8_t(c)));
    a.push_back(vibeacars::withParity(vibeacars::ETX));
    const uint16_t c = vibeacars::bcs(a.data(), a.size());
    a.push_back(uint8_t(c)); a.push_back(uint8_t(c >> 8)); a.push_back(0x7F);
    f.insert(f.end(), a.begin(), a.end());
    uint16_t fc = 0xFFFF;
    for (uint8_t x : f) { fc ^= x; for (int b = 0; b < 8; ++b) fc = (fc & 1) ? uint16_t((fc >> 1) ^ 0x8408) : uint16_t(fc >> 1); }
    fc = uint16_t(~fc);
    f.push_back(uint8_t(fc)); f.push_back(uint8_t(fc >> 8));
    return f;
}
/** The burst's 3-bit symbols after the unique word (header + data + parity), from frames already carrying their FCS. */
inline std::vector<int> vdl2Symbols(const std::vector<std::vector<uint8_t>>& frames) {
    std::vector<uint8_t> bits;
    auto flag = [&] { for (int i = 0; i < 8; ++i) bits.push_back(i == 0 || i == 7 ? 0 : 1); };
    flag();
    for (const auto& f : frames) {
        int ones = 0;
        for (uint8_t x : f) for (int i = 0; i < 8; ++i) {
            const int b = (x >> i) & 1; bits.push_back(uint8_t(b));
            if (b) { if (++ones == 5) { bits.push_back(0); ones = 0; } } else ones = 0;
        }
        flag();
    }
    const int len = int(bits.size());
    const vibevdl2::Layout L((len + 7) / 8);
    std::vector<uint8_t> oct(size_t(L.octets), 0);
    for (int i = 0; i < len; ++i) oct[size_t(i / 8)] |= uint8_t(bits[size_t(i)] << (i % 8));
    std::vector<std::array<uint8_t, 255>> blk(size_t(L.blocks));
    for (auto& b : blk) b.fill(0);
    // the same column walk the receiver uses, the other way round
    vibevdl2::dealColumns(L.octets, L.blocks, vibevdl2::Rs::K, L.lastLen, 0, [&](int r, int c, int i) { blk[size_t(r)][size_t(c)] = oct[size_t(i)]; });
    for (auto& b : blk) vibevdl2::Rs::get().encode(b.data());
    std::vector<uint8_t> out(oct);
    std::vector<uint8_t> fec(size_t(L.fecOctets));
    const int fecRows = L.lastPar ? L.blocks : L.blocks - 1;
    vibevdl2::dealColumns(L.fecOctets, fecRows, vibevdl2::Rs::R, L.lastPar ? L.lastPar : vibevdl2::Rs::R, vibevdl2::Rs::K,
                          [&](int r, int c, int i) { fec[size_t(i)] = blk[size_t(r)][size_t(c)]; });
    out.insert(out.end(), fec.begin(), fec.end());
    // header + data bits, scrambled
    std::vector<uint8_t> tx;
    const uint32_t hw = vibevdl2::hdrEncode(uint32_t(len));
    for (int i = 24; i >= 0; --i) tx.push_back(uint8_t((hw >> i) & 1));
    for (uint8_t x : out) for (int i = 0; i < 8; ++i) tx.push_back(uint8_t((x >> i) & 1));
    vibevdl2::Lfsr l;
    for (auto& b : tx) b ^= uint8_t(l.next());
    while (tx.size() % 3) tx.push_back(0);
    std::vector<int> sym;
    for (size_t i = 0; i < tx.size(); i += 3) sym.push_back((tx[i] << 2) | (tx[i + 1] << 1) | tx[i + 2]);
    return sym;
}
/** Add one burst at capture rate fs: ramp-up (5 × 000), the unique word, `sym`, a raised-cosine pulse (α 0.6). */
inline void vdl2Burst(std::vector<cf32>& cap, double fs, double offHz, double A, size_t s0, const std::vector<int>& sym, double ph0) {
    std::vector<int> all(5, 0);
    for (int i = 0; i < 16; ++i) all.push_back(vibevdl2::kUw[i]);
    all.insert(all.end(), sym.begin(), sym.end());
    for (int i = 0; i < 3; ++i) all.push_back(0);
    std::vector<cf32> pts; int a = 0;
    for (int g : all) { a = (a + vibevdl2::stepOf(g)) & 7; pts.push_back(std::polar(1.0f, float(a) * float(M_PI / 4))); }
    const double sps = fs / vibevdl2::kSymRate, al = 0.6;
    auto rc = [&](double t) {   // t in symbols
        const double x = 4 * al * al * t * t;
        if (std::fabs(1 - x) < 1e-6) return M_PI / 4 * std::sin(M_PI / (2 * al)) / (M_PI / (2 * al));
        const double sc = t == 0 ? 1 : std::sin(M_PI * t) / (M_PI * t);
        return sc * std::cos(M_PI * al * t) / (1 - x);
    };
    const size_t n = size_t(double(pts.size()) * sps);
    double cph = ph0;
    for (size_t i = 0; i < n && s0 + i < cap.size(); ++i) {
        const double ts = double(i) / sps;
        const int k0 = int(std::floor(ts));
        cf32 v(0, 0);
        for (int k = k0 - 5; k <= k0 + 6; ++k) if (k >= 0 && k < int(pts.size())) v += pts[size_t(k)] * float(rc(ts - k));
        cph += 2 * M_PI * offHz / fs;
        cap[s0 + i] += v * float(A) * std::polar(1.0f, float(cph));
    }
}

// ── HFDL ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
// ── the transmitter (tests): MPDU bytes → code → interleave → scramble → the burst's symbols ──────────────────────
inline void hfdlPutCrc(std::vector<uint8_t>& b, size_t from) {
    uint16_t c = 0xFFFF;
    for (size_t i = from; i < b.size(); ++i) { c ^= b[i]; for (int k = 0; k < 8; ++k) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1); }
    c ^= 0xFFFF; b.push_back(uint8_t(c)); b.push_back(uint8_t(c >> 8));
}
/** A downlink MPDU with one LPDU: performance data (flight, position) — `lat`/`lon` in degrees. */
inline std::vector<uint8_t> hfdlPerfMpdu(int ac, int gs, const char* flight, double lat, double lon) {
    std::vector<uint8_t> l = { 0x0D, 0xFF, 0xD1 };
    for (int i = 0; i < 6; ++i) l.push_back(uint8_t(flight[i]));
    const uint32_t la = uint32_t(int32_t(std::lround(lat / 180.0 * 0x7FFFF))) & 0xFFFFF, lo = uint32_t(int32_t(std::lround(lon / 180.0 * 0x7FFFF))) & 0xFFFFF;
    l.push_back(uint8_t(la)); l.push_back(uint8_t(la >> 8)); l.push_back(uint8_t(((la >> 16) & 0xF) | ((lo & 0xF) << 4)));
    l.push_back(uint8_t(lo >> 4)); l.push_back(uint8_t(lo >> 12));
    while (l.size() < 1 + 47) l.push_back(uint8_t(l.size()));
    hfdlPutCrc(l, 0);
    std::vector<uint8_t> m = { 0x07, uint8_t(gs), uint8_t(ac), 0x00, 0x01, 0x00, uint8_t(l.size() - 1) };
    hfdlPutCrc(m, 0);
    m.insert(m.end(), l.begin(), l.end());
    return m;
}
inline std::vector<float> hfdlSymbols(int mode, const std::vector<uint8_t>& mpdu, std::vector<cf32>& sym) {
    const vibehfdl::Mode md = vibehfdl::modeOf(mode);
    const int nData = md.segments * vibehfdl::kDataLen, coded = nData * md.bitsPerSym, info = coded / md.rate;
    std::vector<int> bits((size_t)info, 0);
    for (size_t i = 0; i < mpdu.size() * 8 && int(i) < info - 6; ++i) bits[i] = (mpdu[i / 8] >> (i % 8)) & 1;
    std::vector<int> c;
    int r = 0;
    for (int b : bits) {
        r = ((r << 1) | b) & 127;
        const int c0 = __builtin_popcount(r & 0x6D) & 1, c1 = __builtin_popcount(r & 0x4F) & 1;
        if (md.rate == 4) { c.push_back(c0); c.push_back(c0); c.push_back(c1); c.push_back(c1); } else { c.push_back(c0); c.push_back(c1); }
    }
    const auto order = vibehfdl::deinterleaveOrder(coded, md.colShift);
    std::vector<int> tx((size_t)coded);
    for (int i = 0; i < coded; ++i) tx[size_t(order[size_t(i)])] = c[size_t(i)];
    sym.clear();
    for (int i = 0; i < 448; ++i) sym.push_back(cf32((i & 1) ? 1.f : -1.f, 0));      // a pre-key (content irrelevant here)
    const auto& A = vibehfdl::aSeq();
    for (int k = 0; k < 2 * vibehfdl::kALen; ++k) sym.push_back(cf32(A[size_t(k % vibehfdl::kALen)], 0));
    for (int k = 0; k < vibehfdl::kM1Len; ++k) sym.push_back(cf32(vibehfdl::m1Sym(mode, k), 0));
    for (int k = 0; k < vibehfdl::kM2Len; ++k) sym.push_back(cf32(vibehfdl::m1Sym(mode, k), 0));
    for (int k = 0; k < 9 * vibehfdl::kTLen; ++k) sym.push_back(cf32(vibehfdl::tSym(k % vibehfdl::kTLen), 0));
    vibehfdl::Scrambler scr;
    const int M = 1 << md.bitsPerSym;
    size_t ci = 0;
    for (int s = 0; s < md.segments; ++s) {
        for (int k = 0; k < vibehfdl::kDataLen; ++k) {
            int lab = 0;
            for (int b = 0; b < md.bitsPerSym; ++b) lab = (lab << 1) | tx[ci++];
            int idx = 0; for (int i = 0; i < M; ++i) if ((i ^ (i >> 1)) == lab) idx = i;   // the Gray label's point
            cf32 v = std::polar(1.0f, float(idx) * 2 * float(M_PI) / M);
            if (md.bitsPerSym == 1) v = cf32(lab ? -1.f : 1.f, 0);
            if (scr.next()) v = -v;
            sym.push_back(v);
        }
        for (int k = 0; k < vibehfdl::kTLen; ++k) sym.push_back(cf32(vibehfdl::tSym(k), 0));
    }
    return {};
}
/** The symbols at 4 samples a symbol, root-raised-cosine α 0.31, a carrier offset, a second path, noise. */
inline void hfdlModulate(const std::vector<cf32>& sym, std::vector<cf32>& out, double fOff, double snrDb, double echoDb, int echoDelay, std::mt19937& rng) {
    const double a = 0.31;
    auto rrc = [&](double t) {
        if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / M_PI;
        if (std::fabs(std::fabs(4 * a * t) - 1) < 1e-6) return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
        return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
    };
    const size_t pre = 4000, n = sym.size() * 4 + 2 * pre;
    out.assign(n, cf32(0, 0));
    const float eg = float(std::pow(10.0, echoDb / 20));
    for (size_t k = 0; k < sym.size(); ++k)
        for (int j = -32; j <= 32; ++j) {
            const long i = long(pre + k * 4) + j;
            const float h = float(rrc(j / 4.0)) * 0.5f;
            out[size_t(i)] += sym[k] * h;
            if (echoDelay && i + echoDelay < long(n)) out[size_t(i + echoDelay)] += sym[k] * h * eg * cf32(0.6f, 0.8f);
        }
    double sp = 0; for (size_t k = 0; k < sym.size(); ++k) sp += std::norm(out[pre + k * 4]);
    sp /= sym.size();
    const float sigma = float(std::sqrt(sp / std::pow(10.0, snrDb / 10) / 2));
    std::normal_distribution<float> g(0, sigma);
    for (size_t i = 0; i < n; ++i) out[i] = out[i] * std::polar(1.0f, float(2 * M_PI * fOff * i / vibehfdl::kWorkRate)) + cf32(g(rng), g(rng));
}
/** The baseband burst (7200 S/s, complex) as a USB demodulator on the HFDL dial hears it: interpolated to `rate`, put on
 *  the 1440 Hz carrier, real. (The listener's decoder, vibe_hfdl_run.h, takes exactly this.) */
inline std::vector<float> hfdlUsbAudio(const std::vector<cf32>& x, double rate) {
    const double up = rate / vibehfdl::kWorkRate;
    std::vector<float> a(size_t(double(x.size()) * up));
    for (size_t k = 0; k < a.size(); ++k) {
        const double t = double(k) / up; const size_t j = size_t(t); const float f = float(t - double(j));
        auto at = [&](long q) { return q < 0 || q >= long(x.size()) ? cf32(0, 0) : x[size_t(q)]; };
        const cf32 p0 = at(long(j) - 1), p1 = at(long(j)), p2 = at(long(j) + 1), p3 = at(long(j) + 2);
        const cf32 c = (((-0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3) * f + (p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3)) * f + (-0.5f * p0 + 0.5f * p2)) * f + p1;
        a[k] = float((c * std::polar(1.0f, float(2 * M_PI * vibehfdl::kCarrierOffsetHz * double(k) / rate))).real());
    }
    return a;
}

}  // namespace vibeavitx
