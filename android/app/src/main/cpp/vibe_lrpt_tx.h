// vibe_lrpt_tx.h — a METEOR-M LRPT TRANSMITTER, for tests only (V12, 2026-11-19): pictures → MSU-MR MCU packets →
// VCDUs → RS → randomiser → ASM → convolutional code → [differential] → [80k interleave + markers] → QPSK / OQPSK IQ.
//
// ★ The receiver can only be trusted as far as this is independent of it, so it is built from the same standards the
//   other way round: the JPEG side ENCODES with T.81's tables (forward DCT, quantise, Huffman encode); the RS side
//   divides by g(x); the modulator draws root-raised-cosine pulses from the continuous formula at whatever rate it is
//   asked for (never the receiver's taps). What it shares with the receiver are the constants — and a constant wrong in
//   both is exactly what the REAL recording (docs/v12/LRPT.md §Results) is there to catch.
#pragma once

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "vibe_lrpt.h"

namespace vibelrpt {
namespace tx {

/** Canonical Huffman CODES for a table (bits, vals) — code and length per symbol value. */
struct HuffEnc {
    uint16_t code[256] = {}; uint8_t len[256] = {};
    HuffEnc(const uint8_t* bits16, const uint8_t* vals) {
        int c = 0, k = 0;
        for (int L = 1; L <= 16; L++) { for (int i = 0; i < bits16[L - 1]; i++) { code[vals[k]] = uint16_t(c); len[vals[k]] = uint8_t(L); c++; k++; } c <<= 1; }
    }
};
inline const HuffEnc& dcEnc() {
    static const uint8_t bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t v[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    static const HuffEnc h(bits, v); return h;
}
inline const HuffEnc& acEnc() {
    static const HuffEnc h = [] { const Huff& d = acTable(); uint8_t bits[16]; for (int L = 1; L <= 16; L++) bits[L - 1] = uint8_t(d.maxCode[L] >= 0 ? d.maxCode[L] - d.minCode[L] + 1 : 0); return HuffEnc(bits, d.vals.data()); }();
    return h;
}
struct BitWriter {
    std::vector<uint8_t> out; int nb = 0; uint32_t acc = 0;
    void put(uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) { acc = (acc << 1) | ((v >> i) & 1); if (++nb == 8) { out.push_back(uint8_t(acc)); acc = 0; nb = 0; } } }
    void flush() { while (nb) put(1, 1); }      // pad with ones, as JPEG does
};
inline int category(int v) { int a = v < 0 ? -v : v, c = 0; while (a) { c++; a >>= 1; } return c; }
inline uint32_t magBits(int v, int c) { return v >= 0 ? uint32_t(v) : uint32_t(v + (1 << c) - 1); }

/** Encode 14 8×8 blocks (pixels, row-major) at quality q → the MCU segment's entropy-coded bytes. */
inline std::vector<uint8_t> encodeMcus(const uint8_t blocks[kMcuPerPacket][64], int q) {
    int qt[64];
    const int scale = q < 50 ? 5000 / q : 200 - 2 * q;
    for (int i = 0; i < 64; i++) { const int v = (stdQuant()[i] * scale + 50) / 100; qt[i] = v < 1 ? 1 : v; }
    BitWriter bw; int prevDc = 0;
    for (int m = 0; m < kMcuPerPacket; m++) {
        double F[64];
        for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
            double s = 0;
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
                s += (blocks[m][y * 8 + x] - 128.0) * std::cos((2 * x + 1) * u * M_PI / 16) * std::cos((2 * y + 1) * v * M_PI / 16);
            const double cu = u ? 0.5 : std::sqrt(0.125), cv = v ? 0.5 : std::sqrt(0.125);
            F[v * 8 + u] = cu * cv * s;
        }
        int z[64];
        for (int k = 0; k < 64; k++) { const int nat = zigzag()[k]; z[k] = int(std::lround(F[nat] / qt[nat])); }
        const int diff = z[0] - prevDc; prevDc = z[0];
        const int c = category(diff);
        bw.put(dcEnc().code[c], dcEnc().len[c]); if (c) bw.put(magBits(diff, c), c);
        int run = 0;
        for (int k = 1; k < 64; k++) {
            if (z[k] == 0) { run++; continue; }
            while (run > 15) { bw.put(acEnc().code[0xF0], acEnc().len[0xF0]); run -= 16; }
            const int s = category(z[k]); const int rs = (run << 4) | s;
            bw.put(acEnc().code[rs], acEnc().len[rs]); bw.put(magBits(z[k], s), s);
            run = 0;
        }
        if (run) bw.put(acEnc().code[0x00], acEnc().len[0x00]);
    }
    bw.flush();
    return bw.out;
}

struct TxConfig {
    double sampleRate = 187500, symbolRate = 72000;
    bool oqpsk = false, diff = false, interleaved = false;
    double carrierHz = 0, dopplerRateHzS = 0, phase0 = 0.3;
    double symbolPpm = 0;              ///< symbol clock error
    double esn0Db = 30;                ///< Es/N0 of the QPSK symbols
    int transform = 0;                 ///< the channel's ambiguity (applyX inverse) — the receiver must undo it
    std::vector<int> apids = {64, 65, 68};
    int q = 80;
    uint32_t startMs = 43352840;       ///< ms of day of the first period
    uint32_t seed = 1;
};

/** The source picture for channel `apid`: smooth patterns plus edges, different per channel. */
inline uint8_t sourcePixel(int apid, int x, int y) {
    const double fx = x / 1568.0, fy = y / 400.0;
    double v = 128 + 70 * std::sin(6.28 * (fx * (2 + apid % 3) + fy)) + 40 * std::cos(6.28 * fy * 3 + apid);
    if (((x / 64) + (y / 64)) % 2 == 0 && apid == 64) v += 30;
    return uint8_t(std::max(0.0, std::min(255.0, v)));
}

/** Build the packet stream for `periods` scan periods; returns the VCDU (892-byte) sequence. */
inline std::vector<std::vector<uint8_t>> buildVcdus(const TxConfig& c, int periods) {
    std::vector<uint8_t> stream;                       // concatenated packets
    std::vector<size_t> starts;
    int seq = 1000;
    for (int pr = 0; pr < periods; pr++) {
        const uint32_t ms = c.startMs + uint32_t(pr * 1232 - (pr % 4 == 3 ? 4 : 0));
        for (int a : c.apids) for (int mcu = 0; mcu < kMcuPerLine; mcu += kMcuPerPacket) {
            uint8_t blocks[kMcuPerPacket][64];
            for (int b = 0; b < kMcuPerPacket; b++) for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
                blocks[b][y * 8 + x] = sourcePixel(a, (mcu + b) * 8 + x, pr * 8 + y);
            const auto ent = encodeMcus(blocks, c.q);
            std::vector<uint8_t> pk(6);
            const size_t dataLen = 8 + 6 + ent.size();
            pk[0] = uint8_t(0x08 | ((a >> 8) & 7)); pk[1] = uint8_t(a & 0xFF);
            pk[2] = uint8_t(0xC0 | ((seq >> 8) & 0x3F)); pk[3] = uint8_t(seq & 0xFF);
            pk[4] = uint8_t((dataLen - 1) >> 8); pk[5] = uint8_t((dataLen - 1) & 0xFF);
            const uint8_t tm[8] = {0, 1, uint8_t(ms >> 24), uint8_t(ms >> 16), uint8_t(ms >> 8), uint8_t(ms), 0, 0};
            pk.insert(pk.end(), tm, tm + 8);
            const uint8_t mh[6] = {uint8_t(mcu), 0, 0, 0, 0, uint8_t(c.q)};
            pk.insert(pk.end(), mh, mh + 6);
            pk.insert(pk.end(), ent.begin(), ent.end());
            starts.push_back(stream.size());
            stream.insert(stream.end(), pk.begin(), pk.end());
            seq = (seq + 1) & 0x3FFF;
        }
        // telemetry (APID 70): a short packet, ignored by the picture
        std::vector<uint8_t> t = {0x08, 70, uint8_t(0xC0 | ((seq >> 8) & 0x3F)), uint8_t(seq & 0xFF), 0, 20};
        for (int i = 0; i < 21; i++) t.push_back(uint8_t(i));
        starts.push_back(stream.size()); stream.insert(stream.end(), t.begin(), t.end());
        seq = (seq + 1) & 0x3FFF;
    }
    std::vector<std::vector<uint8_t>> out;
    uint32_t ctr = 77;
    size_t at = 0, si = 0;
    while (at < stream.size()) {
        std::vector<uint8_t> v(892, 0);
        v[0] = 0x40; v[1] = uint8_t(5);                 // version 01, SCID 0, VCID 5
        v[2] = uint8_t(ctr >> 16); v[3] = uint8_t(ctr >> 8); v[4] = uint8_t(ctr); ctr = (ctr + 1) & 0xFFFFFF;
        while (si < starts.size() && starts[si] < at) si++;
        int fhp = 0x7FF;
        if (si < starts.size() && starts[si] < at + 882) fhp = int(starts[si] - at);
        v[8] = uint8_t((fhp >> 8) & 7); v[9] = uint8_t(fhp & 0xFF);
        for (int i = 0; i < 882; i++) v[size_t(10 + i)] = at + size_t(i) < stream.size() ? stream[at + size_t(i)] : 0x00;
        at += 882;
        out.push_back(std::move(v));
    }
    return out;
}

/** VCDUs → coded soft bits (±1 as bits 0/1) in transmission order, with diff / interleave applied. */
inline std::vector<uint8_t> codeBits(const TxConfig& c, const std::vector<std::vector<uint8_t>>& vcdus, int fillCadus = 4) {
    std::vector<uint8_t> bits;
    auto addCadu = [&](const uint8_t* vcdu) {
        uint8_t cadu[kCaduBytes];
        cadu[0] = 0x1A; cadu[1] = 0xCF; cadu[2] = 0xFC; cadu[3] = 0x1D;
        rsEncodeFrame(vcdu, cadu + 4);
        derandomise(cadu + 4);                          // XOR is its own inverse
        for (int i = 0; i < kCaduBytes; i++) for (int j = 7; j >= 0; j--) bits.push_back(uint8_t((cadu[i] >> j) & 1));
    };
    std::vector<uint8_t> fill(892, 0); fill[0] = 0x40; fill[1] = 63;   // VCID 63: fill
    for (int i = 0; i < fillCadus; i++) addCadu(fill.data());
    for (auto& v : vcdus) addCadu(v.data());
    for (int i = 0; i < fillCadus; i++) addCadu(fill.data());
    std::vector<uint8_t> coded; coded.reserve(bits.size() * 2);
    convEncode(bits.data(), bits.size(), 0, coded);
    if (c.diff) {
        // inverse of DiffDecoder: first stream a_n = a_{n-1} XNOR d, second b_n = b_{n-1} XOR d
        int pa = 1, pb = 0;
        for (size_t i = 0; i + 1 < coded.size(); i += 2) {
            const int a = (pa == coded[i]) ? 1 : 0;      // decoded d = (a_n == a_{n-1})
            pa = coded[i] ? pa : 1 - pa; (void)a;
            coded[i] = uint8_t(pa);
            pb = pb ^ coded[i + 1];
            coded[i + 1] = uint8_t(pb);
        }
    }
    if (c.interleaved) {
        std::vector<int8_t> s(coded.size());
        for (size_t i = 0; i < coded.size(); i++) s[i] = int8_t(coded[i]);
        // pad so the deinterleaver's latency is flushed
        s.resize(s.size() + size_t(Deinterleaver::latency()) + 72 * 10, 0);
        for (size_t i = coded.size(); i < s.size(); i++) s[i] = int8_t(i * 2654435761u >> 31);
        const auto il = Deinterleaver::interleave(s);
        std::vector<uint8_t> out;
        for (size_t i = 0; i + 72 <= il.size(); i += 72) {
            for (int j = 7; j >= 0; j--) out.push_back(uint8_t((kInterMarker >> j) & 1));
            for (int j = 0; j < 72; j++) out.push_back(uint8_t(il[i + size_t(j)]));
        }
        return out;
    }
    return coded;
}

/** Coded bits → complex IQ at c.sampleRate (QPSK or OQPSK, RRC α 0.6), with carrier, Doppler slope, clock error, noise
 *  and the channel ambiguity. Noise is scaled for the requested Es/N0 (complex AWGN, per symbol). */
inline std::vector<float> modulate(const TxConfig& c, const std::vector<uint8_t>& bits, double leadSec = 0.5, double tailSec = 0.5) {
    const size_t nsym = bits.size() / 2;
    std::vector<double> I(nsym), Q(nsym);
    for (size_t k = 0; k < nsym; k++) {
        // the channel's ambiguity: the receiver applies applyX(transform) to get back (G1, G2); send its inverse
        int a = bits[2 * k] ? 1 : -1, b = bits[2 * k + 1] ? 1 : -1;
        int ra = a, rb = b;
        for (int cand = 0; cand < 4; cand++) {
            const int ta = (cand & 1) ? -1 : 1, tb = (cand & 2) ? -1 : 1;
            int oa, ob; applyX(c.transform, ta, tb, oa, ob);
            if (oa == a && ob == b) { ra = ta; rb = tb; break; }
        }
        I[k] = ra * M_SQRT1_2; Q[k] = rb * M_SQRT1_2;
    }
    const double rs = c.symbolRate * (1 + c.symbolPpm * 1e-6);
    const double dur = nsym / rs + leadSec + tailSec;
    const size_t ns = size_t(dur * c.sampleRate);
    std::vector<float> out(ns * 2, 0.f);
    const double a = 0.6;
    auto rrc = [a](double t) {
        if (std::fabs(t) < 1e-9) return 1.0 - a + 4 * a / M_PI;
        if (std::fabs(std::fabs(4 * a * t) - 1.0) < 1e-9) return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
        return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
    };
    const int span = 5;
    std::mt19937 rng(c.seed);
    std::normal_distribution<double> g(0, 1);
    // Es/N0: symbol energy 1 after the pulse normalisation below; noise per complex sample σ² = N0 · Fs / Rs … expressed
    // per sample: N0 = Es / 10^(Es/N0/10), Es = Σ pulse² (per symbol) ≈ sps for this normalisation.
    const double sps = c.sampleRate / rs;
    const double n0 = sps / std::pow(10.0, c.esn0Db / 10.0);
    const double sigma = std::sqrt(n0 / 2);
    double ph = c.phase0;
    for (size_t n = 0; n < ns; n++) {
        const double t = n / c.sampleRate - leadSec;          // seconds since the first symbol
        const double ts = t * rs;                              // in symbols
        double re = 0, im = 0;
        const long k0 = long(std::floor(ts)) - span, k1 = long(std::floor(ts)) + span + 1;
        for (long k = std::max(0L, k0); k <= k1 && k < long(nsym); k++) {
            re += I[size_t(k)] * rrc(ts - double(k));
            if (c.oqpsk) im += Q[size_t(k)] * rrc(ts - double(k) - 0.5);
            else im += Q[size_t(k)] * rrc(ts - double(k));
        }
        const double f = c.carrierHz + c.dopplerRateHzS * t;
        ph += 2 * M_PI * f / c.sampleRate;
        const double cr = std::cos(ph), si = std::sin(ph);
        out[2 * n] = float(re * cr - im * si + sigma * g(rng));
        out[2 * n + 1] = float(re * si + im * cr + sigma * g(rng));
    }
    return out;
}

} // namespace tx
} // namespace vibelrpt
