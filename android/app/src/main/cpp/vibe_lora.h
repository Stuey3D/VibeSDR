// vibe_lora.h — V12 LoRa physical layer: the codec (whitening, Hamming, diagonal interleaver, Gray, explicit header,
// CRC), a reference modulator for the tests, and the demodulator that finds and decodes frames in one channel sampled
// at one sample per chip. docs/v12/LORA-MESH.md.
//
// ★★★ WRITTEN FROM THE PUBLISHED DESCRIPTIONS, NOT FROM ANYONE'S CODE (Stuart's decoder rule). The chirp itself and the
//     modem parameters are Semtech's (AN1200.22 "LoRa Modulation Basics"; the SX1272/76/SX1261/2 datasheets: SF, BW,
//     CR, LDRO, explicit header, the 2.25-symbol SFD, the sensitivity table). The bit-level chain — whitening, Hamming
//     codes, the diagonal interleaver, the Gray step, the header and its checksum, the CRC — is as the open literature
//     describes it: Knight & Seeber, "Decoding LoRa: Realizing a Modern LPWAN with SDR" (GRCon 2016); Robyns et al.,
//     "A Multi-Channel Software Decoder for the LoRa Modulation Scheme" (2018); Tapparel et al., "An Open-Source LoRa
//     Physical Layer Prototype on GNU Radio" (EPFL, 2020). Every convention a document left open is marked ASSUMED in
//     LORA-MESH.md, and each was then settled on Stuart's real recording (the header checksum and the CRC both pass).
// ★★ Header-only and links nothing, like vibe_ism.h, so test-lora builds with one g++ line. Where the build links PFFFT
//    (VIBE_DAB_PFFFT — the server and the app) the dechirp FFT is PFFFT (NEON/SSE); the radix-2 is the reference.
// ★★ CPU: one FFT of 2^SF per symbol per spreading factor, and nothing else per sample but the dechirp multiply. The
//    search FFTs every window; a frame, once found, costs one FFT per symbol it carries. Measured in test-lora (CPU row).
#pragma once

#include "vibe_ism.h"          // SmallFft / DetFft (PFFFT where linked)
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vibelora {

using cf = std::complex<float>;

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE MODEM'S PARAMETERS
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ Low data rate optimisation: Semtech's datasheets mandate it when a symbol lasts longer than 16 ms (SF11/SF12 at
 *  125 kHz, SF12 at 250 kHz, SF10+ at 62.5 kHz). Every payload symbol then carries SF-2 bits, as the header does. */
inline bool ldroFor(int sf, double bwHz) { return double(1 << sf) / bwHz > 16.0e-3; }

struct Phy {
    int sf = 7;               ///< spreading factor 7-12 (2^SF chips per symbol)
    double bw = 125e3;        ///< bandwidth = chip rate, Hz
    int cr = 1;               ///< coding rate 4/(4+cr): 1..4
    bool crc = true;          ///< payload CRC present (uplinks: yes; LoRaWAN downlinks: no)
    bool ldro = false;        ///< low data rate optimisation
    uint8_t sync = 0x34;      ///< the sync word (0x34 LoRaWAN public, 0x12 private, 0x2B Meshtastic)
    int preamble = 8;         ///< upchirps before the sync word
    bool invertIq = false;    ///< LoRaWAN downlinks swap up and down chirps
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE CODEC — bytes ⇄ symbol values
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** The whitening sequence: an 8-bit LFSR, x^8+x^6+x^5+x^4+1, seeded 0xFF — byte i of the payload is XORed with byte i
 *  of the sequence (0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1 …). The CRC is not whitened. */
inline uint8_t whiten(size_t i) {
    static const std::vector<uint8_t> t = [] {
        std::vector<uint8_t> v(256);
        uint8_t s = 0xFF;
        for (int k = 0; k < 256; ++k) {
            v[size_t(k)] = s;
            const int fb = ((s >> 7) ^ (s >> 5) ^ (s >> 4) ^ (s >> 3)) & 1;
            s = uint8_t((s << 1) | fb);
        }
        return v;
    }();
    return t[i & 255];
}

/** CRC-16/CCITT (polynomial 0x1021, initial 0), MSB first. */
inline uint16_t crc16(const uint8_t* p, size_t n, uint16_t c = 0) {
    for (size_t i = 0; i < n; ++i) {
        c ^= uint16_t(p[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return c;
}
/** ★ The payload CRC as the LoRa chips compute it (Tapparel et al. §III-B, confirmed on the real recording): CRC-16 of
 *  every byte but the last two, then XORed with those two (the second-last as the high byte). Sent low byte first. */
inline uint16_t payloadCrc(const uint8_t* p, size_t n) {
    if (n < 2) { uint16_t c = 0; if (n == 1) c = p[0]; return c; }
    return uint16_t(crc16(p, n - 2) ^ p[n - 1] ^ (uint16_t(p[n - 2]) << 8));
}

/** The explicit header's 5-bit checksum: five parity bits over its 12 bits (length high, length low, CR<<1|CRC),
 *  written as masks over w = n0<<8 | n1<<4 | n2. Returned as c4 c3 c2 c1 c0. */
inline int headerChecksum(int n0, int n1, int n2) {
    const int w = (n0 & 15) << 8 | (n1 & 15) << 4 | (n2 & 15);
    static const int mask[5] = {0xF00, 0x8E1, 0x49A, 0x257, 0x12F};   // c4, c3, c2, c1, c0
    int c = 0;
    for (int i = 0; i < 5; ++i) c = c << 1 | (__builtin_popcount(unsigned(w & mask[i])) & 1);
    return c;
}

/** Hamming: a nibble n (bits n0 = LSB … n3) as a codeword of 4+cr bits, sent MSB first: n0 n1 n2 n3, then parity. */
inline int hammingEncode(int n, int cr) {
    const int d0 = n & 1, d1 = n >> 1 & 1, d2 = n >> 2 & 1, d3 = n >> 3 & 1;
    const int data = d0 << 3 | d1 << 2 | d2 << 1 | d3;
    if (cr == 1) return data << 1 | (d0 ^ d1 ^ d2 ^ d3);
    const int p0 = d0 ^ d1 ^ d2, p1 = d1 ^ d2 ^ d3, p2 = d0 ^ d1 ^ d3, p3 = d0 ^ d2 ^ d3;
    const int par = p0 << 3 | p1 << 2 | p2 << 1 | p3;
    return data << cr | par >> (4 - cr);
}
/** The nearest codeword's nibble (single errors corrected at 4/7 and 4/8; 4/5 and 4/6 only detect). `errs` = the
 *  distance, so the caller can count corrections. A tie keeps the received data bits. */
inline int hammingDecode(int cw, int cr, int* errs = nullptr) {
    const int len = 4 + cr;
    int best = -1, bestD = 99, ties = 0;
    for (int n = 0; n < 16; ++n) {
        const int d = __builtin_popcount(unsigned((hammingEncode(n, cr) ^ cw) & ((1 << len) - 1)));
        if (d < bestD) { bestD = d; best = n; ties = 0; } else if (d == bestD) ++ties;
    }
    if (errs) *errs = bestD;
    if (ties && bestD > 0) {
        const int data = cw >> cr & 15;   // d0 d1 d2 d3 MSB first
        return (data >> 3 & 1) | (data >> 2 & 1) << 1 | (data >> 1 & 1) << 2 | (data & 1) << 3;
    }
    return best;
}

inline int grayEncode(int x) { return x ^ (x >> 1); }
inline int grayDecode(int x) { int r = x; while (x >>= 1) r ^= x; return r; }

/** The diagonal interleaver, one block: `cws` (sfApp codewords of cwLen bits, MSB first) → cwLen symbols of sfApp
 *  bits. Bit j (MSB first) of symbol i is bit i of codeword (i − j − 1) mod sfApp. */
inline void interleave(const std::vector<int>& cws, int sfApp, int cwLen, std::vector<int>& syms) {
    syms.assign(size_t(cwLen), 0);
    for (int i = 0; i < cwLen; ++i) {
        int v = 0;
        for (int j = 0; j < sfApp; ++j) {
            const int k = ((i - j - 1) % sfApp + sfApp) % sfApp;
            const int bit = cws[size_t(k)] >> (cwLen - 1 - i) & 1;
            v = v << 1 | bit;
        }
        syms[size_t(i)] = v;
    }
}
inline void deinterleave(const int* syms, int sfApp, int cwLen, std::vector<int>& cws) {
    cws.assign(size_t(sfApp), 0);
    for (int i = 0; i < cwLen; ++i)
        for (int j = 0; j < sfApp; ++j) {
            const int bit = syms[i] >> (sfApp - 1 - j) & 1;
            const int k = ((i - j - 1) % sfApp + sfApp) % sfApp;
            cws[size_t(k)] |= bit << (cwLen - 1 - i);
        }
}

/** A value of sfApp bits → the chirp's symbol (0..2^SF-1). ★ The +1 and the ×4 of the reduced rate: a reduced-rate
 *  symbol sits at 4·g + 1, so a ±1 bin error in the receiver still rounds to the right value (the literature's "parity
 *  and zero bit" — both come out as zero after the Gray step). */
inline int valueToSymbol(int v, int sf, bool reduced) {
    const int N = 1 << sf;
    return reduced ? (4 * grayDecode(v) + 1) % N : (grayDecode(v) + 1) % N;
}
inline int symbolToValue(int s, int sf, bool reduced) {
    const int N = 1 << sf;
    const int t = ((s - 1) % N + N) % N;
    if (reduced) return grayEncode(((t + 2) >> 2) % (N >> 2));
    return grayEncode(t);
}

/** Encode a frame: payload bytes → symbol values (explicit header). */
inline std::vector<int> encode(const Phy& p, const std::vector<uint8_t>& payload) {
    std::vector<int> nib;
    const int len = int(payload.size());
    const int n0 = len >> 4, n1 = len & 15, n2 = p.cr << 1 | (p.crc ? 1 : 0);
    const int chk = headerChecksum(n0, n1, n2);
    nib.push_back(n0); nib.push_back(n1); nib.push_back(n2); nib.push_back(chk >> 4 & 1); nib.push_back(chk & 15);
    for (size_t i = 0; i < payload.size(); ++i) { const uint8_t b = uint8_t(payload[i] ^ whiten(i)); nib.push_back(b & 15); nib.push_back(b >> 4); }
    if (p.crc) {
        const uint16_t c = payloadCrc(payload.data(), payload.size());
        nib.push_back(c & 15); nib.push_back(c >> 4 & 15); nib.push_back(c >> 8 & 15); nib.push_back(c >> 12 & 15);
    }
    std::vector<int> out, cws, syms;
    size_t at = 0;
    // the first block: SF-2 codewords at 4/8, reduced rate (8 symbols)
    {
        const int sfApp = p.sf - 2;
        cws.clear();
        for (int k = 0; k < sfApp; ++k) cws.push_back(hammingEncode(at < nib.size() ? nib[at] : 0, 4)), ++at;
        interleave(cws, sfApp, 8, syms);
        for (int s : syms) out.push_back(valueToSymbol(s, p.sf, true));
    }
    const int sfApp = p.ldro ? p.sf - 2 : p.sf, cwLen = 4 + p.cr;
    while (at < nib.size()) {
        cws.clear();
        for (int k = 0; k < sfApp; ++k) cws.push_back(hammingEncode(at < nib.size() ? nib[at] : 0, p.cr)), ++at;
        interleave(cws, sfApp, cwLen, syms);
        for (int s : syms) out.push_back(valueToSymbol(s, p.sf, p.ldro));
    }
    return out;
}

/** How many payload symbols follow the 8 header symbols for `len` bytes. */
inline int payloadSymbols(int sf, int cr, bool crc, bool ldro, int len) {
    const int nib = 2 * len + (crc ? 4 : 0) - (sf - 2 - 5);
    if (nib <= 0) return 0;
    const int sfApp = ldro ? sf - 2 : sf;
    return (nib + sfApp - 1) / sfApp * (4 + cr);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE REFERENCE MODULATOR — the continuous chirp, at any sample rate (for the tests and the test clips)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** Append one chirp: symbol `sym` (fractional allowed), `frac` of a symbol long, down when `down`. Phase continuous
 *  through `phase`; `cfoHz` added; `chipScale` stretches the chip clock (1 + ppm·1e-6 → a slower transmitter). */
inline void chirp(std::vector<cf>& out, double fs, double bw, int sf, double sym, bool down, double frac, double& phase,
                  double cfoHz = 0, double chipScale = 1.0, double* tCarry = nullptr) {
    const double N = double(1 << sf), Ts = N / bw * chipScale;
    const double dur = Ts * frac;
    double t = tCarry ? *tCarry : 0;      // the sub-sample remainder carried from the last chirp
    while (t < dur) {
        const double tm = t + 0.5 / fs;
        double u = std::fmod(sym / N + tm / Ts, 1.0);
        double f = (u - 0.5) * bw;
        if (down) f = -f;
        out.push_back(cf(float(std::cos(phase)), float(std::sin(phase))));
        phase += 2 * M_PI * (f + cfoHz) / fs;
        if (phase > M_PI * 64) phase = std::fmod(phase, 2 * M_PI);
        t += 1.0 / fs;
    }
    if (tCarry) *tCarry = t - dur;
}
/** A whole frame (preamble, sync word, 2.25 downchirps, symbols) at `fs`. */
inline std::vector<cf> modulate(const Phy& p, const std::vector<int>& syms, double fs, double cfoHz = 0, double ppm = 0,
                                double startPhase = 0, double subSample = 0) {
    std::vector<cf> v;
    double ph = startPhase, carry = subSample / fs;   // ★ subSample: the frame starts this fraction of a sample late
    const double cs = 1.0 + ppm * 1e-6;
    const bool dn = p.invertIq;
    for (int i = 0; i < p.preamble; ++i) chirp(v, fs, p.bw, p.sf, 0, dn, 1.0, ph, cfoHz, cs, &carry);
    chirp(v, fs, p.bw, p.sf, double((p.sync >> 4) * 8), dn, 1.0, ph, cfoHz, cs, &carry);
    chirp(v, fs, p.bw, p.sf, double((p.sync & 15) * 8), dn, 1.0, ph, cfoHz, cs, &carry);
    chirp(v, fs, p.bw, p.sf, 0, !dn, 1.0, ph, cfoHz, cs, &carry);
    chirp(v, fs, p.bw, p.sf, 0, !dn, 1.0, ph, cfoHz, cs, &carry);
    chirp(v, fs, p.bw, p.sf, 0, !dn, 0.25, ph, cfoHz, cs, &carry);
    for (int s : syms) chirp(v, fs, p.bw, p.sf, double(s), dn, 1.0, ph, cfoHz, cs, &carry);
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ONE DECODED FRAME
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Frame {
    int sf = 0; double bw = 0; int cr = 0; bool crcOn = false, crcOk = false, ldro = false, invertIq = false;
    uint8_t sync = 0;          ///< the sync word as heard (two nibbles, each symbol / 8)
    std::vector<uint8_t> bytes;
    double tSec = 0;           ///< when the preamble ended, seconds of this channel's stream
    double freqHz = 0;         ///< the channel's frequency + the measured offset
    double cfoHz = 0;          ///< the transmitter's offset from the channel
    float snrDb = 0;           ///< from the preamble's dechirped peaks (the LoRa chips' own definition: in the bandwidth)
    float levelDb = -99;       ///< the channel's power during the preamble, dBFS
    int fixes = 0;             ///< Hamming corrections made
    std::string chan;          ///< which plan channel heard it ("EU868 LoRaWAN 868.1")
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ONE CHANNEL'S SAMPLE STREAM — a ring the demodulators read by absolute sample index (one sample per chip)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** A fractional-delay interpolator: 8-tap Blackman-windowed sinc at 256 phases, taps at −3 … +4 around the point.
 *  Flat and linear-phase to ~0.4 cycles/sample. Used by the channeliser (to the chip rate) and the demodulator (to put a
 *  symbol's window on its exact, fractional start — see Demod). */
static constexpr int kTaps = 8, kPhases = 256;
inline const std::vector<float>& sincTable() {
    static const std::vector<float> t = [] {
        std::vector<float> v(size_t(kPhases) * kTaps);
        for (int ph = 0; ph < kPhases; ++ph) {
            const double mu = double(ph) / kPhases;
            double sum = 0;
            for (int k = 0; k < kTaps; ++k) {
                const double x = k - (kTaps / 2 - 1) - mu;
                const double s = std::fabs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
                const double u = (x + kTaps / 2.0) / kTaps;
                const double w = 0.42 - 0.5 * std::cos(2 * M_PI * u) + 0.08 * std::cos(4 * M_PI * u);
                v[size_t(ph * kTaps + k)] = float(s * w); sum += s * w;
            }
            for (int k = 0; k < kTaps; ++k) v[size_t(ph * kTaps + k)] = float(v[size_t(ph * kTaps + k)] / sum);
        }
        return v;
    }();
    return t;
}

/** Σ x[k]·h[k] over the 8 taps. ★ NEON / SSE2 (decoder audit 2026-10-31): the scalar 16 multiply-adds were the
 *  channeliser's largest line on the Pi 2 once the NCO and the divides were gone. */
inline cf sinc8(const cf* x, const float* h) {
    static_assert(kTaps == 8, "sinc8 is the 8-tap interpolator");
#if VIBE_ISM_NEON
    const float32x4x2_t a = vld2q_f32(reinterpret_cast<const float*>(x)), b = vld2q_f32(reinterpret_cast<const float*>(x + 4));
    const float32x4_t h0 = vld1q_f32(h), h1 = vld1q_f32(h + 4);
    const float32x4_t re = vmlaq_f32(vmulq_f32(a.val[0], h0), b.val[0], h1), im = vmlaq_f32(vmulq_f32(a.val[1], h0), b.val[1], h1);
    const float32x2_t r2 = vadd_f32(vget_low_f32(re), vget_high_f32(re)), i2 = vadd_f32(vget_low_f32(im), vget_high_f32(im));
    const float32x2_t s = vpadd_f32(r2, i2);
    return cf(vget_lane_f32(s, 0), vget_lane_f32(s, 1));
#elif VIBE_ISM_SSE2
    const float* f = reinterpret_cast<const float*>(x);
    // pairs (re, im) × (h, h): four products of two complex samples a load, summed lane-wise
    __m128 acc = _mm_setzero_ps();
    for (int k = 0; k < 8; k += 2) {
        const __m128 hk = _mm_setr_ps(h[k], h[k], h[k + 1], h[k + 1]);
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(f + 2 * k), hk));
    }
    float l[4]; _mm_storeu_ps(l, acc);
    return cf(l[0] + l[2], l[1] + l[3]);
#else
    float re = 0, im = 0;
    for (int k = 0; k < kTaps; ++k) { re += x[k].real() * h[k]; im += x[k].imag() * h[k]; }
    return cf(re, im);
#endif
}

class Stream {
public:
    explicit Stream(double rate, size_t ringPow2 = 17) : rate_(rate), ring_(size_t(1) << ringPow2), mask_(ring_.size() - 1) {}
    void push(const cf* x, size_t n) { for (size_t i = 0; i < n; ++i) ring_[size_t((wr_ + i) & mask_)] = x[i]; wr_ += n; }
    void push(cf x) { ring_[size_t(wr_ & mask_)] = x; ++wr_; }
    int64_t written() const { return wr_; }
    int64_t oldest() const { return wr_ - int64_t(ring_.size()) + 1; }
    const cf& at(int64_t i) const { return ring_[size_t(i) & mask_]; }
    /** Samples i … i+n-1 as one pointer when they do not wrap the ring, else nullptr. */
    const cf* run(int64_t i, int n) const { const size_t k = size_t(i) & mask_; return k + size_t(n) <= ring_.size() ? &ring_[k] : nullptr; }
    double rate() const { return rate_; }
    size_t capacity() const { return ring_.size(); }
private:
    double rate_;
    std::vector<cf> ring_;
    size_t mask_;
    int64_t wr_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE DEMODULATOR — one spreading factor on one channel
//
//  SEARCH: dechirp every window of N = 2^SF samples, FFT, take the peak. A preamble is K windows in a row whose peaks
//          agree to ±1 bin and stand well clear of the rest (any misalignment still gives ONE clean tone — consecutive
//          upchirps are periodic). The bin k = CFO + timing lateness, both in bins.
//  ALIGN:  move the window grid back by k (CFO is now folded into timing), then step on until the window that dechirps
//          as a DOWNchirp: the SFD. The two windows before it are the sync word. On the SFD the down-dechirped bin is
//          CFO − lateness = 2·CFO here, which separates the two: the grid is corrected by the CFO, and the preamble peaks
//          re-measured on the corrected grid give the exact offset to remove from every symbol (fraction included).
//  HEADER, PAYLOAD: one FFT per symbol at the drifting grid — ★ the transmitter's crystal error that moved its carrier
//          by CFO also stretches its chips by the same ppm: a 230-symbol SF11 packet at 20 ppm slides 9 samples. The
//          window start follows N·(1 − CFO/f) per symbol and the sub-sample remainder is removed as a frequency offset.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Demod {
public:
    struct Cfg {
        int sf = 7; double bw = 125e3; bool invertIq = false;
        double carrierHz = 868e6;          ///< the channel's frequency (for the ppm of the chip clock)
        int minPreamble = 4;               ///< windows in a row that must agree
        std::vector<uint8_t> syncs;        ///< accepted sync words (empty = any)
        int ldro = -1;                     ///< -1 = by the datasheet rule, 0 off, 1 on
        /** ★ Search every Nth window (1 = all). 2 halves the search cost of SF11/12 — a preamble of 8 still gives ≥ 3
         *  windows to agree; the frame itself is still demodulated symbol by symbol. The governor sets it on weak boxes. */
        int searchStride = 1;
        std::string name;
    };
    Demod(const Cfg& c) : c_(c), N_(1 << c.sf), fft_(1 << c.sf) {
        up_.resize(size_t(N_)); buf_.resize(size_t(N_)); mag_.resize(size_t(N_));
        for (int n = 0; n < N_; ++n) {
            const double ph = 2 * M_PI * (double(n) * n / (2.0 * N_) - n / 2.0);
            up_[size_t(n)] = cf(float(std::cos(ph)), float(std::sin(ph)));
        }
        ldro_ = c.ldro < 0 ? ldroFor(c.sf, c.bw) : c.ldro == 1;
        stride_ = std::max(1, c.searchStride);
    }
    const Cfg& cfg() const { return c_; }
    int N() const { return N_; }
    /** Windows FFTed since start (the CPU's real driver), frames found, headers that failed, CRCs that failed. */
    uint64_t ffts() const { return ffts_; }
    uint64_t found() const { return found_; }
    uint64_t badHeaders() const { return badHdr_; }
    uint64_t badCrc() const { return badCrc_; }
    /** Preambles seen (candidates) and of those, how many reached an SFD with an accepted sync word. */
    uint64_t preambles() const { return pre_; }
    uint64_t sfds() const { return sfd_; }
    bool busy() const { return st_ != SEARCH; }
    /** The last frame's symbols as demodulated (header included) — for the tests' diagnostics. */
    const std::vector<int>& lastSymbols() const { return lastSyms_; }

    /** Process everything the stream holds. `emit(Frame&&)` for each frame decoded (its CRC good, or no CRC). */
    template <class F> void run(const Stream& s, F&& emit) {
        if (pos_ < s.oldest()) pos_ = s.written() - N_;         // fell behind the ring (or first call): resume at the end
        if (pos_ < 0) pos_ = 0;
        for (;;) {
            if (st_ == SEARCH) { if (!search(s)) return; }
            else if (st_ == ALIGN) { if (!align(s)) return; }
            else { if (!symbols(s, emit)) return; }
        }
    }

    // ★ Public for the tests: dechirp the N samples at `w` (up = expect upchirps), FFT, return the peak.
    struct Peak { int bin = 0; float pk = 0, mean = 0; cf a{0, 0}, b{0, 0}, c{0, 0}; };
    /** `startFrac` (0…1): the window starts that far past sample `w` — the samples are interpolated (sincTable). */
    Peak window(const Stream& s, int64_t w, bool up, double fracBins = 0, double startFrac = 0) {
        const bool inv = c_.invertIq;
        // ★ invertIq: conjugating the samples turns a down-chirp frame into an up-chirp one
        cf rot(1, 0), inc(1, 0);
        if (fracBins != 0) { const double a = -2 * M_PI * fracBins / N_; inc = cf(float(std::cos(a)), float(std::sin(a))); }
        const float* h = nullptr;
        if (startFrac > 1.0 / (2 * kPhases)) {
            const int ph = std::min(kPhases - 1, int(startFrac * kPhases + 0.5));
            h = &sincTable()[size_t(ph * kTaps)];
        }
        /* ★★ IN THREE VECTOR PASSES (decoder audit 2026-10-31): the samples (interpolated by sinc8, or a straight copy out
         *  of the ring), the dechirp (cmulRun — conj(x)·r is conj(x·conj(r)), so invertIq is two flags), then the
         *  fractional-bin rotation (rotateRun, only when asked for). Same products as the per-sample loop it replaced. */
        cf* B = buf_.data();
        if (h) {
            for (int n = 0; n < N_; ++n) {
                const int64_t b = w + n - (kTaps / 2 - 1);
                if (const cf* x = s.run(b, kTaps)) { B[n] = sinc8(x, h); continue; }
                float re = 0, im = 0;
                for (int k = 0; k < kTaps; ++k) { const cf& v = s.at(b + k); re += v.real() * h[k]; im += v.imag() * h[k]; }
                B[n] = cf(re, im);
            }
        } else {
            for (int n = 0; n < N_;) {
                const int64_t at = w + n;
                const int run = std::min<int>(N_ - n, int(s.capacity() - (size_t(at) & (s.capacity() - 1))));
                std::memcpy(B + n, &s.at(at), size_t(run) * sizeof(cf));
                n += run;
            }
        }
        vibeism::cmulRun(B, up_.data(), B, N_, up != inv, inv);
        if (fracBins != 0) vibeism::rotateRun(B, B, N_, false, rot, inc);
        fft_.forward(buf_.data());
        ++ffts_;
        Peak p; double sum = 0; int best = 0; float bm = -1;
        for (int k = 0; k < N_; ++k) {
            const float m = std::norm(buf_[size_t(k)]);
            mag_[size_t(k)] = m; sum += m;
            if (m > bm) { bm = m; best = k; }
        }
        p.bin = best; p.pk = bm; p.mean = float((sum - bm) / (N_ - 1));
        p.a = buf_[size_t((best - 1 + N_) % N_)]; p.b = buf_[size_t(best)]; p.c = buf_[size_t((best + 1) % N_)];
        return p;
    }

private:
    enum St { SEARCH, ALIGN, HEADER, PAYLOAD };
    /** A preamble/SFD peak stands this far over the mean bin. ★ 5 (7 dB): alone it passes noise often (the largest of
     *  128 noise bins beats 5× the mean about half the time), but K windows agreeing to ±1 bin does not happen by
     *  chance, and a weak frame needs the room. */
    static constexpr float kSearchRatio = 5.0f;

    int wrapSigned(int k) const { k %= N_; if (k < 0) k += N_; return k >= N_ / 2 ? k - N_ : k; }
    static double frac3(const Peak& p) {
        // ★ the peak's fractional position from its two neighbours (Jacobsen's estimator on the complex bins)
        const cf d = 2.0f * p.b - p.a - p.c;
        if (std::norm(d) < 1e-30f) return 0;
        const double r = std::real((p.a - p.c) / d);
        return std::max(-0.5, std::min(0.5, r));
    }

    bool search(const Stream& s) {
        if (pos_ + N_ > s.written()) return false;
        const Peak p = window(s, pos_, true);
        hist_.push_back({pos_, p.bin, p.pk > kSearchRatio * p.mean});
        if (hist_.size() > 8) hist_.pop_front();
        pos_ += int64_t(N_) * stride_;
        // ★ K of the last K+1 windows agree with the newest to ±1 bin (one window lost to noise is forgiven: at the
        //   sensitivity limit a single symbol's peak dips under the threshold often enough to cost ~1 dB otherwise)
        const int K = stride_ > 1 ? 3 : c_.minPreamble;   // ★ every other window: 3 of the last 4 seen span ~7 symbols
        if (int(hist_.size()) < K) return true;
        const auto& last = hist_.back();
        if (!last.ok) return true;
        int agree = 0;
        for (size_t i = 0; i < hist_.size() && i <= size_t(K); ++i) {
            const auto& h = hist_[hist_.size() - 1 - i];
            if (h.ok && std::abs(wrapSigned(h.bin - last.bin)) <= 1) ++agree;
        }
        if (agree < K) return true;
        // ★ a preamble: put the grid back by its bin so the CFO is folded into the timing (see the class comment)
        alignStart_ = last.w - last.bin;
        // ★ striding, the newest window may be the preamble's last: start the walk two symbols back
        if (stride_ > 1 && alignStart_ - 2 * int64_t(N_) >= s.oldest()) alignStart_ -= 2 * int64_t(N_);
        if (alignStart_ < s.oldest()) { hist_.clear(); return true; }
        st_ = ALIGN; ai_ = 0; aw_.clear(); hist_.clear(); lastPre_ = 0; ++pre_;
        return true;
    }

    struct AW { Peak u, d; };
    bool align(const Stream& s) {
        const int maxW = 72;                               // ★ Meshtastic's 16-symbol preamble + a late start, generously
        for (;;) {
            const int64_t w = alignStart_ + int64_t(ai_) * N_;
            if (w + N_ > s.written()) return false;
            if (w < s.oldest()) { reset(w); return true; }
            /* ★ Both dechirps of every window. A downchirp's UP-dechirp is a smear whose largest bin lands on 0 ± 1 by
             *  chance now and then (1-2 % of SFDs at SF7 in test-lora); without its down-dechirp to compare, that window
             *  read as "still preamble" and the SFD was lost. A preamble window is one whose up-peak beats its down-peak. */
            AW a; a.u = window(s, w, true); a.d = window(s, w, false);
            const bool pre = std::abs(wrapSigned(a.u.bin)) <= 1 && a.u.pk > kSearchRatio * a.u.mean && a.u.pk > a.d.pk;
            if (pre) lastPre_ = ai_;
            aw_.push_back(a);
            ++ai_;
            // ★★ THE SFD IS TWO DOWNCHIRPS: the first window whose down-dechirp peak agrees (±1 bin) with the next one's
            //    and beats its own up-dechirp, two or three windows after the last preamble window (the sync word lies
            //    between). One noisy window can neither fake it nor, by lining up the pair, move it.
            const int ic = ai_ - 2;
            if (ic >= 3 && ic - lastPre_ >= 2 && ic - lastPre_ <= 3 && !pre) {
                const AW& c1 = aw_[size_t(ic)];
                const Peak& d2 = a.d;
                const bool down1 = c1.d.pk > c1.u.pk && c1.d.pk > kSearchRatio * c1.d.mean;
                const bool down2 = d2.pk > a.u.pk && d2.pk > kSearchRatio * d2.mean;
                if (down1 && down2 && std::abs(wrapSigned(c1.d.bin - d2.bin)) <= 1) {
                    // ★ the corrected grid can sit up to N/4 later: wait until the second downchirp is in on it too
                    if (alignStart_ + int64_t(ic + 2) * N_ + N_ / 2 + kTaps > s.written()) { --ai_; aw_.pop_back(); return false; }
                    alignAt(s, ic, d2);
                    return true;
                }
            }
            if (ai_ - lastPre_ > 5) {   // the preamble ended and no SFD followed
#ifdef VIBE_LORA_DEBUG
                std::fprintf(stderr, "align abort sf%d lastPre %d:", c_.sf, lastPre_);
                for (auto& x : aw_) std::fprintf(stderr, " [u%d %.1f d%d %.1f]", wrapSigned(x.u.bin), x.u.pk / x.u.mean, wrapSigned(x.d.bin), x.d.mean > 0 ? x.d.pk / x.d.mean : 0);
                std::fprintf(stderr, "\n");
#endif
                abort(w + N_); return true;
            }
            if (ai_ >= maxW) { abort(w + N_); return true; }
        }
    }

    /** Window `ic` is the SFD's first downchirp (`d2` the second's down-dechirp): settle CFO, timing and sync word. */
    void alignAt(const Stream& s, int ic, const Peak& d2) {
        {
            {
                const int64_t w = alignStart_ + int64_t(ic) * N_;
                // the preamble's fraction on this grid (average of up to 4 windows), then the CFO
                double f0 = 0; int nf = 0;
                for (int i = ic - 3; i >= 0 && i >= ic - 6; --i) if (std::abs(wrapSigned(aw_[size_t(i)].u.bin)) <= 1) {
                    f0 += wrapSigned(aw_[size_t(i)].u.bin) + frac3(aw_[size_t(i)].u); ++nf; }
                if (nf) f0 /= nf;
                const double kd = wrapSigned(d2.bin) + frac3(d2);
                const double cfo = 0.5 * (kd + f0);                        // bins
                const int shift = int(std::lround(cfo - f0));
                const int64_t g = alignStart_ + shift;                      // the corrected grid
                const int64_t sfd = g + int64_t(ic) * N_;                   // where the first downchirp starts
                // re-measure the preamble on the corrected grid: the offset every symbol carries
                double o = 0, iSum = 0; int no = 0; float mn = 0, sg = 0;
                for (int i = ic - 3; i >= 0 && i >= ic - 7; --i) {
                    const int64_t wi = g + int64_t(i) * N_;
                    if (wi < s.oldest()) break;
                    const Peak q = window(s, wi, true);
                    const int b = wrapSigned(q.bin);
                    if (std::abs(b - shift) > 1) continue;
                    o += b + frac3(q); iSum += i; ++no; mn += q.mean;
                    sg += std::norm(q.a) + q.pk + std::norm(q.c) - 3 * q.mean;   // ★ a fractional peak spills into its neighbours
                }
                if (!no) { abort(w + 2 * N_); return; }
                o /= no; mn /= no; sg /= no;
                // sync word, on the corrected grid
                int sw[2];
                for (int k = 0; k < 2; ++k) {
                    const Peak q = window(s, g + int64_t(ic - 2 + k) * N_, true, o - std::lround(o));
                    const int v = ((q.bin - int(std::lround(o))) % N_ + N_) % N_;
                    sw[k] = int(std::lround(v / 8.0)) & 15;
                }
                sync_ = uint8_t(sw[0] << 4 | sw[1]);
                if (!c_.syncs.empty() && std::find(c_.syncs.begin(), c_.syncs.end(), sync_) == c_.syncs.end()) { abort(sfd + 2 * N_); return; }
                /* ★★★ CFO AND TIMING, BOTH TO A FRACTION. On this (nearly aligned) grid the preamble's up-dechirp is at
                 *  CFO + δ and the second downchirp's down-dechirp at CFO − δ (δ = how late the grid is). A fractional δ
                 *  must NOT be treated as a frequency offset: inside each symbol the chirp wraps from +BW/2 to −BW/2, and
                 *  sampled δ off the chip grid that wrap is a PHASE JUMP of 2πδ — the tone splits between two bins by an
                 *  amount that depends on the symbol's value. At one sample per chip with no channel filter the jump is
                 *  invisible (test-lora's first round trips); through the channeliser every SF11 frame with a few kHz of
                 *  CFO lost symbols at 40 dB SNR. So δ is measured and each symbol's window is INTERPOLATED onto its
                 *  exact start (window(…, startFrac)); only the true CFO is removed as a frequency. */
                const Peak d2g = window(s, g + int64_t(ic + 1) * N_, false);
                const double kdg = wrapSigned(d2g.bin) + frac3(d2g);
                const double iAvg = iSum / no;
                double cfoB = 0.5 * (o + kdg);
                for (int it = 0; it < 2; ++it) {
                    // ★ the chips slid between the preamble windows and the SFD: N·CFO/f per symbol (see nEff_)
                    const double slide = N_ * (cfoB * c_.bw / N_) / c_.carrierHz;
                    cfoB = 0.5 * (o + kdg + (ic + 1 - iAvg) * slide);
                }
                cfoHz_ = cfoB * c_.bw / N_;
                // ★ the chip clock: a transmitter whose carrier is high by CFO runs its chips fast by the same ppm
                nEff_ = N_ * (1.0 - cfoHz_ / c_.carrierHz);
                const double delta = (o - cfoB) + (ic - iAvg) * (N_ - nEff_);   // lateness of the grid at the SFD
                off_ = cfoB;
                t0_ = double(sfd) - delta + 2.25 * nEff_;
                /* ★ SNR from the preamble re-dechirped WITH its fraction removed: a tone half a bin off leaks ~19 % of
                 *  its energy past its neighbours, and an estimate from the raw windows topped out near 6 dB however
                 *  strong the signal (test-lora, a 30 dB frame read 1.7 dB). */
                {
                    float spk = 0, smn = 0; int ns = 0;
                    for (int i = ic - 3; i >= 0 && i >= ic - 6; --i) {
                        const int64_t wi = g + int64_t(i) * N_;
                        if (wi < s.oldest()) break;
                        const double oi = o + (i - iSum / no) * (N_ - nEff_);
                        const Peak q = window(s, wi, true, oi - std::lround(oi));
                        if (std::abs(wrapSigned(q.bin - int(std::lround(oi)))) > 0) continue;
                        spk += q.pk; smn += q.mean; ++ns;
                    }
                    if (ns) { sg = (spk - smn) / ns; mn = smn / ns; }
                }
                // ★ SNR in the bandwidth, the chips' own definition: the tone holds the signal's energy N·N·A², each bin
                //   the noise's N·σ² — so A²/σ² = signal ÷ (mean bin · N). Level = the channel's power, dBFS.
                snr_ = mn > 0 ? float(10 * std::log10(std::max(1e-6, double(sg) / (double(mn) * N_)))) : 0;
                level_ = float(10 * std::log10(std::max(1e-12, (double(sg) / N_ + double(mn)) / double(N_))));
                tEnd_ = double(sfd) / c_.bw;
                syms_.clear(); k_ = 0; nSyms_ = 8; hdrOk_ = false; ++sfd_;
                st_ = HEADER;
            }
        }
    }

    template <class F> bool symbols(const Stream& s, F&& emit) {
        while (k_ < nSyms_) {
            const double t = t0_ + k_ * nEff_;
            const int64_t w = int64_t(std::floor(t));
            if (w + N_ + kTaps > s.written()) return false;
            if (w - kTaps < s.oldest()) { reset(w); return true; }
            const double o = off_;
            const long oi = std::lround(o);
            const Peak q = window(s, w, true, o - double(oi), t - double(w));   // ★ on its exact start (see alignAt)
            syms_.push_back(int(((q.bin - oi) % N_ + N_) % N_));
#ifdef VIBE_LORA_DEBUG
            std::fprintf(stderr, "sym %d frac %+.3f o %+.3f\n", k_, frac3(q), o);
#endif
            ++k_;
            if (st_ == HEADER && k_ == 8) {
                if (!header()) { ++badHdr_; abort(w + N_); return true; }
                st_ = PAYLOAD;
            }
        }
        // the whole frame is in
        Frame f;
        lastSyms_ = syms_;
        if (finish(f)) { emit(std::move(f)); }
        const int64_t end = int64_t(std::lround(t0_ + k_ * nEff_));
        reset(end);
        return true;
    }

    bool header() {
        const int sfApp = c_.sf - 2;
        int v[8];
        for (int i = 0; i < 8; ++i) v[i] = symbolToValue(syms_[size_t(i)], c_.sf, true);
        std::vector<int> cws;
        deinterleave(v, sfApp, 8, cws);
        nib_.clear(); fixes_ = 0;
        for (int cw : cws) { int e = 0; nib_.push_back(hammingDecode(cw, 4, &e)); fixes_ += e; }
        const int n0 = nib_[0], n1 = nib_[1], n2 = nib_[2];
        const int chk = (nib_[3] & 1) << 4 | nib_[4];
        if (headerChecksum(n0, n1, n2) != chk) return false;
        len_ = n0 << 4 | n1; cr_ = n2 >> 1; crcOn_ = n2 & 1;
        if (cr_ < 1 || cr_ > 4 || len_ == 0) return false;
        nSyms_ = 8 + payloadSymbols(c_.sf, cr_, crcOn_, ldro_, len_);
        return true;
    }

    bool finish(Frame& f) {
        const int sfApp = ldro_ ? c_.sf - 2 : c_.sf, cwLen = 4 + cr_;
        std::vector<int> cws, v(static_cast<size_t>(cwLen));
        for (size_t at = 8; at + size_t(cwLen) <= syms_.size(); at += size_t(cwLen)) {
            for (int i = 0; i < cwLen; ++i) v[size_t(i)] = symbolToValue(syms_[at + size_t(i)], c_.sf, ldro_);
            deinterleave(v.data(), sfApp, cwLen, cws);
            for (int cw : cws) { int e = 0; nib_.push_back(hammingDecode(cw, cr_, &e)); if (cr_ >= 3) fixes_ += e; }
        }
        // nibbles 5.. are the payload, low nibble first, then the CRC
        const size_t need = 5 + size_t(2 * len_) + (crcOn_ ? 4 : 0);
        if (nib_.size() < need) return false;
        f.bytes.resize(size_t(len_));
        for (int i = 0; i < len_; ++i) f.bytes[size_t(i)] = uint8_t((nib_[5 + 2 * size_t(i)] | nib_[6 + 2 * size_t(i)] << 4) ^ whiten(size_t(i)));
        f.crcOn = crcOn_;
        if (crcOn_) {
            const size_t c = 5 + 2 * size_t(len_);
            const uint16_t got = uint16_t(nib_[c] | nib_[c + 1] << 4 | nib_[c + 2] << 8 | nib_[c + 3] << 12);
            f.crcOk = got == payloadCrc(f.bytes.data(), f.bytes.size());
            if (!f.crcOk) { ++badCrc_; return false; }
        }
        f.sf = c_.sf; f.bw = c_.bw; f.cr = cr_; f.ldro = ldro_; f.invertIq = c_.invertIq; f.sync = sync_;
        f.cfoHz = cfoHz_; f.snrDb = snr_; f.levelDb = level_; f.tSec = tEnd_; f.fixes = fixes_; f.chan = c_.name;
        ++found_;
        return true;
    }

    void abort(int64_t resume) { reset(resume); }
    void reset(int64_t resume) { st_ = SEARCH; pos_ = resume; hist_.clear(); aw_.clear(); syms_.clear(); nib_.clear(); }

    Cfg c_;
    int N_;
    vibeism::DetFft fft_;
    std::vector<cf> up_, buf_;
    std::vector<float> mag_;
    bool ldro_ = false;
    int stride_ = 1;
    St st_ = SEARCH;
    int64_t pos_ = -1;
    struct H { int64_t w; int bin; bool ok; };
    std::deque<H> hist_;
    int64_t alignStart_ = 0;
    int ai_ = 0, lastPre_ = 0;
    std::vector<AW> aw_;
    double off_ = 0, cfoHz_ = 0, nEff_ = 0, t0_ = 0, tEnd_ = 0;
    float snr_ = 0, level_ = -99;
    uint8_t sync_ = 0;
    int k_ = 0, nSyms_ = 0, len_ = 0, cr_ = 1, fixes_ = 0;
    bool crcOn_ = false, hdrOk_ = false;
    std::vector<int> syms_, nib_, lastSyms_;
    uint64_t ffts_ = 0, found_ = 0, badHdr_ = 0, badCrc_ = 0, pre_ = 0, sfd_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE CHANNELISER — every plan channel out of the whole capture, at exactly one sample per chip
//
//  ★★ ONE FFT FOR ALL THE CHANNELS (fast convolution, overlap-save). The capture is transformed once per block; each
//     channel takes the few bins around its centre, weights them with a windowed-sinc low-pass, and transforms them
//     back at a SMALL size — which is the decimation. Eight LoRaWAN channels, Meshtastic and MeshCore then cost one
//     2048-point FFT per 1536 capture samples plus a 256-point one per channel, where a mixer and a FIR per channel
//     would cost a multiply per capture sample each. A cubic interpolator then lands each on exactly its chip rate.
//  ★ The bin a channel is taken from is the nearest one; what is left (under half a bin, ≤ 600 Hz) is removed by a
//    small NCO after the decimation, and each block's phase is put right for where it starts in the capture.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Channeliser {
public:
    explicit Channeliser(double fs) : fs_(fs) {
        L_ = 256;
        while (fs / L_ > 1200.0) L_ *= 2;
        S_ = L_ - L_ / 4;
        fft_.reset(new vibeism::DetFft(L_));
        in_.assign(size_t(L_), cf(0, 0));
        X_.resize(size_t(L_));
        fill_ = L_ / 4;   // the first block's overlap is silence
    }
    double rate() const { return fs_; }
    int fftSize() const { return L_; }
    /** A channel `offHz` from the capture centre, `bw` wide, delivered at `bw` samples a second into `dst`. */
    void add(double offHz, double bw, Stream* dst) {
        Ch c;
        c.dst = dst; c.bw = bw;
        c.kc = int(std::lround(offHz / (fs_ / L_)));
        c.resid = offHz - c.kc * (fs_ / L_);
        c.Lc = 32;
        while (fs_ * c.Lc / L_ < 1.5 * bw) c.Lc *= 2;
        c.ri = fs_ * c.Lc / L_;
        c.step = c.ri / bw;
        c.ncoDph = -2 * M_PI * c.resid / c.ri;
        c.ncoInc = cf(float(std::cos(c.ncoDph)), float(std::sin(c.ncoDph)));
        c.ifft.reset(new vibeism::DetFft(c.Lc));
        c.Y.resize(size_t(c.Lc));
        // the low-pass: Hamming-windowed sinc, M = L/4 + 1 taps, its −6 dB point half a transition past the band edge
        const int M = L_ / 4 + 1;
        const double trans = 3.3 * fs_ / M;
        const double fc = std::min(0.5 * bw + 0.5 * trans, 0.5 * c.ri - 0.5 * trans) / fs_;
        std::vector<cf> h(size_t(L_), cf(0, 0));
        double sum = 0;
        for (int n = 0; n < M; ++n) {
            const double t = n - (M - 1) / 2.0;
            const double s = t == 0 ? 2 * fc : std::sin(2 * M_PI * fc * t) / (M_PI * t);
            const double w = 0.54 - 0.46 * std::cos(2 * M_PI * n / (M - 1));
            h[size_t(n)] = cf(float(s * w), 0); sum += s * w;
        }
        for (auto& v : h) v /= float(sum);
        fft_->forward(h.data());
        c.H.resize(size_t(c.Lc));
        for (int m = -c.Lc / 2; m < c.Lc / 2; ++m) c.H[size_t((m + c.Lc) % c.Lc)] = h[size_t((m + L_) % L_)] * (1.0f / float(L_));
        c.pend.assign(size_t(kTaps / 2 - 1), cf(0, 0));
        c.t = kTaps / 2 - 1;
        ch_.push_back(std::move(c));
    }
    size_t channels() const { return ch_.size(); }
    uint64_t ffts() const { return ffts_; }

    void feed(const cf* x, size_t n) {
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(n - i, size_t(L_ - fill_));
            std::memcpy(&in_[size_t(fill_)], x + i, take * sizeof(cf));
            fill_ += int(take); i += take;
            if (fill_ == L_) block();
        }
    }

private:
    struct Ch {
        Stream* dst = nullptr; double bw = 0;
        int kc = 0, Lc = 0; double resid = 0, ri = 0, step = 0;
        std::unique_ptr<vibeism::DetFft> ifft;
        std::vector<cf> H, Y, pend;
        double t = 1.0;
        double ncoPh = 0, ncoDph = 0;   ///< the residual NCO's phase at the next output, and its step (rad/sample)
        cf ncoInc{1, 0};                ///< e^{j·ncoDph}
    };
    void block() {
        std::memcpy(X_.data(), in_.data(), size_t(L_) * sizeof(cf));
        fft_->forward(X_.data());
        ++ffts_;
        const int64_t start = blocks_ * int64_t(S_);
        /* ★★ PI 2 (2026-10-31, the decoder SIMD audit — qemu-arm instruction count over Stuart's 433.92 capture): this
         *  loop was over a third of ISM mode. Three things, none of them the arithmetic:
         *   · two sin/cos in DOUBLE per output sample (the NCO) — 17 % of ISM mode on its own. Now ONE per channel per
         *     block: the block phase × the NCO's phase at the block start, then a float rotation (a 3/4·Lc-sample run,
         *     ≤ 768 steps: the rotator drifts ~1e-5 in magnitude, far under the samples' own noise), and the NCO's phase
         *     carried in double across blocks exactly as before;
         *   · `%` with a variable divisor — 32-bit ARMv7-A has no divide instruction, so four library calls a bin
         *     (13 % of ISM mode). L_ and Lc are powers of two: a mask is the same non-negative remainder;
         *   · std::complex's NaN-checked products (cmulf). */
        const int lmask = L_ - 1;
        for (auto& c : ch_) {
            const int Lc = c.Lc, cmask = Lc - 1;
            const cf* X = X_.data();
            const cf* H = c.H.data();
            cf* Y = c.Y.data();
            // the inverse transform by the forward one: conjugate in, conjugate out. In runs where both the capture's
            // bins (k) and the channel's (j) are contiguous — at most four — four products at a time (cmulRun).
            for (int m = -Lc / 2; m < Lc / 2;) {
                const int j = m & cmask, k = (c.kc + m) & lmask;
                int run = std::min(Lc / 2 - m, L_ - k);
                if (m < 0) run = std::min(run, -m);
                vibeism::cmulRun(X + k, H + j, Y + j, run, false, true);
                m += run;
            }
            c.ifft->forward(c.Y.data());
            // the block's phase for where it starts in the capture, and the NCO for the residual offset
            const int keep0 = Lc / 4, n = Lc - keep0;
            const double a0 = -2 * M_PI * double(c.kc) * double(start & int64_t(lmask)) / L_ + c.ncoPh;
            cf rot(float(std::cos(a0)), float(std::sin(a0)));
            const size_t was = c.pend.size();
            c.pend.resize(was + size_t(n));
            cf* out = c.pend.data() + was;
            vibeism::rotateRun(Y + keep0, out, n, true, rot, c.ncoInc);
            c.ncoPh = std::remainder(c.ncoPh + double(n) * c.ncoDph, 2 * M_PI);
            resample(c);
        }
        // keep the last quarter as the next block's overlap
        std::memmove(in_.data(), in_.data() + S_, size_t(L_ - S_) * sizeof(cf));
        fill_ = L_ - S_;
        ++blocks_;
    }
    /** From the channel's decimated rate to exactly its chip rate: the 8-tap windowed sinc (sincTable). ★ Not a cubic:
     *  a 4-point Lagrange's phase is visibly non-linear at 0.2 cycles/sample (a 250 kHz channel at 600 kS/s). */
    static void resample(Ch& c) {
        auto& p = c.pend;
        const std::vector<float>& tab = sincTable();
        while (c.t + kTaps / 2 < double(p.size())) {
            const int i = int(c.t);
            const int ph = std::min(kPhases - 1, int((c.t - i) * kPhases + 0.5));
            const float* h = &tab[size_t(ph * kTaps)];
            const cf* x = &p[size_t(i - (kTaps / 2 - 1))];
            c.dst->push(sinc8(x, h));
            c.t += c.step;
        }
        const int drop = int(c.t) - (kTaps / 2 - 1);
        if (drop > 0) { p.erase(p.begin(), p.begin() + drop); c.t -= drop; }
    }

    double fs_;
    int L_ = 0, S_ = 0, fill_ = 0;
    int64_t blocks_ = 0;
    std::unique_ptr<vibeism::DetFft> fft_;
    std::vector<cf> in_, X_;
    std::vector<Ch> ch_;
    uint64_t ffts_ = 0;
};

} // namespace vibelora
