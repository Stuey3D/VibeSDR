// balloon_horus.h — V12 balloons, part 2: HORUS BINARY v1 / v2 (docs/v12/BALLOONS.md).
//
// Project Horus's binary telemetry for amateur balloons: a 22-byte (v1) or 32-byte (v2) packet, Golay(23,12)-coded,
// interleaved and scrambled, sent as 4-FSK at 100 baud — usually 270 Hz between tones — on 70 cm, received in USB.
// ★ Written from the published packet layout (Project Horus wiki / horusdemodlib README: field order and sizes) and the
//   coding as horusdemodlib (LGPL-2.1) implements it — READ for the bit conventions, nothing copied. The conventions,
//   which a receiver must match bit for bit to interoperate:
//     • frame = UW "$$" (0x24 0x24) | the payload bytes as they are | the Golay parity bits | zero padding to a byte;
//     • parity: one Golay(23,12) codeword per 12 payload bits taken MSB first; its 11 check bits, MSB first, follow one
//       another after the payload. A last, shorter group of r bits is coded as the 12-bit word (value << 1) — the bits
//       sit one place up from the bottom, not at the top;
//     • then everything after the UW is INTERLEAVED (bit n → bit b·n mod N; b = 337 for v1's 43 bytes, 389 for v2's 63)
//       and SCRAMBLED (XOR with a 15-bit additive scrambler, x¹⁵ + x¹⁴ + 1 shape, seeded 0x4A80 each frame) — both index
//       a byte's bits LSB first, while the modem sends each byte MSB first;
//     • 4-FSK: two bits a symbol, the first the more significant; symbol k is the k-th tone up from the lowest;
//     • CRC-16-CCITT (as UKHAS) over every byte but the last two, stored little-endian in them.
//   v2 fields (little-endian): payload ID u16 · counter u16 · h m s · latitude f32 · longitude f32 · altitude u16 ·
//   speed u8 km/h · satellites u8 · temperature i8 °C · battery u8 (0–255 = 0–5 V) · 9 custom bytes · CRC.
//   v1: payload ID u8 · counter u16 · h m s · lat f32 · lon f32 · alt u16 · speed u8 · sats u8 · temp i8 · battery u8 · CRC.
// ★ Our demodulator is our own: the four tones found in the shared spectrum (balloon_dsp.h findFour), a bank of four
//   one-symbol integrators (NEON / SSE2 four lanes — one tone a lane), the symbol clock from where the strongest tone's
//   energy peaks, SOFT bits, the UW found in either tone order, and Golay decoded Chase-style from the soft bits (the
//   four least sure bits tried both ways) — the Golay check bits themselves come from M17's (vibe_m17_fec.h golayCheck:
//   the same generator, 0xC75; test-balloon proves the shortened code matches).
// ★ A packet is believed only when its CRC passes AND its time and position are possible (h < 24, |lat| ≤ 90 …).
#pragma once
#include "balloon_dsp.h"
#include "balloon_ukhas.h"
#include "../vibe_m17_fec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace vibe {
namespace balloon {
namespace horus {

constexpr int kV1Bytes = 22, kV2Bytes = 32;
constexpr uint8_t kUw[2] = { 0x24, 0x24 };

/** Bytes on air (UW included) for a payload of n bytes: v1 45, v2 65. */
inline int txBytes(int n) {
    const int bits = n * 8, cws = (bits + 11) / 12;
    return (16 + bits + cws * 11 + 7) / 8;
}

// ── Golay(23,12) ───────────────────────────────────────────────────────────────────────────────────────────────────
/** The 11 check bits for 12 data bits: M17's extended-Golay check bits less their parity bit (the same cyclic code). */
inline uint16_t parity11(uint16_t d) { return (uint16_t)(vibe::m17::golayCheck((uint16_t)(d & 0xFFF)) >> 1); }
/** Syndrome of a 23-bit word (data in bits 22..11). */
inline uint16_t syndrome23(uint32_t r) { return (uint16_t)((r & 0x7FF) ^ parity11((uint16_t)((r >> 11) & 0xFFF))); }
/** Syndrome → its error pattern of weight ≤ 3 (a perfect code: every syndrome has exactly one). */
inline const std::vector<uint32_t>& golay23Table() {
    static const std::vector<uint32_t> t = [] {
        std::vector<uint32_t> v(2048, 0xFFFFFFFFu);
        v[0] = 0;
        for (int a = 0; a < 23; ++a) {
            const uint32_t ea = 1u << a; v[syndrome23(ea)] = ea;
            for (int b = a + 1; b < 23; ++b) {
                const uint32_t eb = ea | (1u << b); v[syndrome23(eb)] = eb;
                for (int c = b + 1; c < 23; ++c) { const uint32_t ec = eb | (1u << c); v[syndrome23(ec)] = ec; }
            }
        }
        return v;
    }();
    return t;
}
/** Hard decision: the corrected 12 data bits; `errs` = bits corrected. */
inline uint16_t golay23Hard(uint32_t r, int* errs = nullptr) {
    const uint32_t e = golay23Table()[syndrome23(r)];
    if (errs) *errs = __builtin_popcount(e);
    return (uint16_t)(((r ^ e) >> 11) & 0xFFF);
}
/** ★ Soft (Chase-II): 23 soft values, soft[0] = bit 22 (data MSB), > 0 = 1. The kChase least sure bits are tried both ways,
 *  each test word decoded hard, and the codeword nearest the soft values wins. `errs` = received bits it disagrees with. */
constexpr int kChase = 6;   ///< the least sure bits tried both ways (64 test words, 22 codewords a v2 frame)
inline uint16_t golay23Soft(const float* soft, int* errs = nullptr) {
    uint32_t r = 0;
    for (int i = 0; i < 23; ++i) r = (r << 1) | (soft[i] > 0 ? 1u : 0u);
    int idx[23];
    for (int i = 0; i < 23; ++i) idx[i] = i;
    std::partial_sort(idx, idx + kChase, idx + 23, [&](int a, int b) { return std::fabs(soft[a]) < std::fabs(soft[b]); });
    double bestScore = -1e30; uint32_t bestCw = r;
    for (int m = 0; m < (1 << kChase); ++m) {
        uint32_t t = r;
        for (int k = 0; k < kChase; ++k) if ((m >> k) & 1) t ^= 1u << (22 - idx[k]);
        const uint32_t cw = t ^ golay23Table()[syndrome23(t)];
        double score = 0;
        for (int i = 0; i < 23; ++i) score += ((cw >> (22 - i)) & 1) ? soft[i] : -soft[i];
        if (score > bestScore) { bestScore = score; bestCw = cw; }
    }
    if (errs) *errs = __builtin_popcount(bestCw ^ r);
    return (uint16_t)((bestCw >> 11) & 0xFFF);
}

// ── interleaver + scrambler (bits indexed LSB first in each byte) ──────────────────────────────────────────────────
inline int interleaveB(int nbytes) {
    if (nbytes == 43) return 337;
    if (nbytes == 63) return 389;
    const int nbits = nbytes * 8;
    auto prime = [](int x) { if (x < 2) return false; for (int i = 2; i * i <= x; ++i) if (x % i == 0) return false; return true; };
    int b = (int)std::floor(nbits / 1.62) + 1;
    while (!prime(b)) ++b;
    return b;
}
inline bool bitAt(const uint8_t* p, int i) { return (p[i >> 3] >> (i & 7)) & 1; }
/** `inverse` false: bit n goes to b·n mod N (the transmitter); true: the receiver's undo. */
inline void interleave(uint8_t* p, int nbytes, bool inverse) {
    const int nbits = nbytes * 8, b = interleaveB(nbytes);
    std::vector<uint8_t> o((size_t)nbytes, 0);
    for (int n = 0; n < nbits; ++n) {
        const int j = (int)(((long long)b * n) % nbits);
        const int from = inverse ? j : n, to = inverse ? n : j;
        if (bitAt(p, from)) o[(size_t)(to >> 3)] |= (uint8_t)(1u << (to & 7));
    }
    std::memcpy(p, o.data(), (size_t)nbytes);
}
/** The scrambler's output bit for each of nbits bits (its own inverse: applied twice is nothing). */
inline void scrambleBits(std::vector<uint8_t>& out, int nbits) {
    out.assign((size_t)nbits, 0);
    uint16_t s = 0x4A80;
    for (int i = 0; i < nbits; ++i) {
        const uint16_t o = (uint16_t)(((s >> 1) ^ s) & 1);
        out[(size_t)i] = (uint8_t)o;
        s = (uint16_t)((s >> 1) | (o << 14));
    }
}
inline void scramble(uint8_t* p, int nbytes) {
    std::vector<uint8_t> k; scrambleBits(k, nbytes * 8);
    for (int i = 0; i < nbytes * 8; ++i) if (k[(size_t)i]) p[i >> 3] ^= (uint8_t)(1u << (i & 7));
}

// ── the frame ──────────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ OUR TRANSMITTER'S CODER (the tests' and the bench clip's): payload → the bytes on air, UW first. */
inline std::vector<uint8_t> encodeFrame(const uint8_t* payload, int n) {
    const int nt = txBytes(n);
    std::vector<uint8_t> f((size_t)nt, 0);
    f[0] = kUw[0]; f[1] = kUw[1];
    std::memcpy(f.data() + 2, payload, (size_t)n);
    int pbit = (2 + n) * 8;   // where the next parity bit goes, MSB-first numbering
    auto putBit = [&](int v) { if (v) f[(size_t)(pbit >> 3)] |= (uint8_t)(0x80 >> (pbit & 7)); ++pbit; };
    const int bits = n * 8;
    for (int g = 0; g * 12 < bits; ++g) {
        const int r = std::min(12, bits - g * 12);
        uint16_t v = 0;
        for (int i = 0; i < r; ++i) { const int bi = g * 12 + i; v = (uint16_t)((v << 1) | ((payload[bi >> 3] >> (7 - (bi & 7))) & 1)); }
        const uint16_t d = r == 12 ? v : (uint16_t)(v << 1);
        const uint16_t p = parity11(d);
        for (int i = 10; i >= 0; --i) putBit((p >> i) & 1);
    }
    interleave(f.data() + 2, nt - 2, false);
    scramble(f.data() + 2, nt - 2);
    return f;
}
/** ★ THE RECEIVER'S DECODE: the soft bits that followed the UW, in the order they were sent (each byte MSB first; > 0 =
 *  1), (txBytes(n) − 2)·8 of them → the payload. `fixed` = received bits the Golay disagreed with. */
inline void decodeSoft(const float* soft, int n, uint8_t* out, int* fixed) {
    const int nb = txBytes(n) - 2, nbits = nb * 8;
    std::vector<float> s((size_t)nbits), d((size_t)nbits);
    // on-air order → LSB-first indexing, then descramble (a sign flip) and de-interleave
    std::vector<uint8_t> k; scrambleBits(k, nbits);
    for (int q = 0; q < nbits; ++q) { const int i = (q & ~7) | (7 - (q & 7)); s[(size_t)i] = k[(size_t)i] ? -soft[q] : soft[q]; }
    const int b = interleaveB(nb);
    for (int m = 0; m < nbits; ++m) d[(size_t)m] = s[(size_t)(((long long)b * m) % nbits)];
    // back to MSB-first stream order: stream bit m = byte m/8, bit 7 − m%8
    auto at = [&](int m) { return d[(size_t)((m & ~7) | (7 - (m & 7)))]; };
    std::memset(out, 0, (size_t)n);
    const int bits = n * 8;
    int pbit = n * 8, tot = 0;
    for (int g = 0; g * 12 < bits; ++g) {
        const int r = std::min(12, bits - g * 12);
        float w[23];
        if (r == 12) for (int i = 0; i < 12; ++i) w[i] = at(g * 12 + i);
        else {
            // the short group: 12 − r − 1 zeros above it, the r bits, then a zero (known zeros: strongly 0)
            int j = 0;
            for (; j < 11 - r; ++j) w[j] = -100.f;
            for (int i = 0; i < r; ++i) w[j++] = at(g * 12 + i);
            w[j++] = -100.f;
        }
        for (int i = 0; i < 11; ++i) w[12 + i] = at(pbit++);
        int e = 0;
        uint16_t dd = golay23Soft(w, &e);
        tot += e;
        if (r < 12) dd = (uint16_t)(dd >> 1);
        for (int i = 0; i < r; ++i) {
            const int bi = g * 12 + i;
            if ((dd >> (r - 1 - i)) & 1) out[bi >> 3] |= (uint8_t)(0x80 >> (bi & 7));
        }
    }
    if (fixed) *fixed = tot;
}

// ── the packet ─────────────────────────────────────────────────────────────────────────────────────────────────────
struct Fields { int id = 0; int count = 0; int h = 0, m = 0, s = 0; float lat = 0, lon = 0; int alt = 0, speed = 0, sats = 0, temp = 0, batt = 0; uint8_t custom[9] = {0}; };
inline void put16(uint8_t* p, int v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
inline void putF(uint8_t* p, float f) { uint32_t u; std::memcpy(&u, &f, 4); for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(u >> (8 * i)); }
inline float getF(const uint8_t* p) { uint32_t u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; float f; std::memcpy(&f, &u, 4); return f; }
inline int get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
/** Our transmitter's packet: version 1 or 2, the CRC filled in. */
inline std::vector<uint8_t> makePacket(int version, const Fields& f) {
    const int n = version == 1 ? kV1Bytes : kV2Bytes;
    std::vector<uint8_t> p((size_t)n, 0);
    int o = 0;
    if (version == 1) p[o++] = (uint8_t)f.id; else { put16(&p[0], f.id); o = 2; }
    put16(&p[(size_t)o], f.count); o += 2;
    p[(size_t)o++] = (uint8_t)f.h; p[(size_t)o++] = (uint8_t)f.m; p[(size_t)o++] = (uint8_t)f.s;
    putF(&p[(size_t)o], f.lat); o += 4; putF(&p[(size_t)o], f.lon); o += 4;
    put16(&p[(size_t)o], f.alt); o += 2;
    p[(size_t)o++] = (uint8_t)f.speed; p[(size_t)o++] = (uint8_t)f.sats; p[(size_t)o++] = (uint8_t)(int8_t)f.temp; p[(size_t)o++] = (uint8_t)f.batt;
    if (version == 2) { std::memcpy(&p[(size_t)o], f.custom, 9); o += 9; }
    put16(&p[(size_t)o], crc16(p.data(), (size_t)n - 2));
    return p;
}
/** ★ A decoded payload → telemetry, or false when its CRC fails or a field is impossible. */
inline bool parsePacket(const uint8_t* p, int n, Telemetry& t) {
    if (n != kV1Bytes && n != kV2Bytes) return false;
    if (crc16(p, (size_t)n - 2) != (uint16_t)get16(p + n - 2)) return false;
    const bool v2 = n == kV2Bytes;
    int o = 0;
    const int id = v2 ? get16(p) : p[0]; o = v2 ? 2 : 1;
    const int count = get16(p + o); o += 2;
    const int h = p[o], m = p[o + 1], s = p[o + 2]; o += 3;
    const float lat = getF(p + o), lon = getF(p + o + 4); o += 8;
    const int alt = get16(p + o); o += 2;
    const int speed = p[o], sats = p[o + 1], temp = (int8_t)p[o + 2], batt = p[o + 3]; o += 4;
    if (h > 23 || m > 59 || s > 59) return false;
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) return false;
    t = Telemetry();
    t.source = "horus"; t.horusVersion = v2 ? 2 : 1; t.horusId = id;
    char nm[32]; std::snprintf(nm, sizeof nm, "HORUS-%d", id); t.callsign = nm;
    t.count = count; t.hh = h; t.mm = m; t.ss = s;
    if (!(lat == 0 && lon == 0)) { t.lat = lat; t.lon = lon; }
    t.alt = alt; t.speedKmh = speed; t.sats = sats; t.tempC = temp; t.battV = batt * 5.0 / 255.0;
    if (v2) { char hx[24]; for (int i = 0; i < 9; ++i) std::snprintf(hx + 2 * i, 3, "%02x", p[o + i]); t.custom = hx; }
    // ★ written out as the UKHAS sentence a listener would recognise (the CRC its own)
    char b[200];
    std::snprintf(b, sizeof b, "%s,%d,%02d:%02d:%02d,%.5f,%.5f,%d,%d,%d,%d,%.2f", t.callsign.c_str(), count, h, m, s,
                  std::isfinite(t.lat) ? t.lat : 0.0, std::isfinite(t.lon) ? t.lon : 0.0, alt, speed, sats, temp, t.battV);
    t.sentence = makeSentence(b);
    if (!t.sentence.empty() && t.sentence.back() == '\n') t.sentence.pop_back();
    return true;
}

// ── our modulator (tests and the bench clip) ───────────────────────────────────────────────────────────────────────
/** Bytes → 4-FSK, MSB first, continuous phase, tone k = f0 + k·spacing. Appends to `out` at `fs`. */
inline void modulate(const std::vector<uint8_t>& bytes, double fs, double baud, double f0, double spacing, float amp,
                     std::vector<float>& out, double& phase) {
    const double sps = fs / baud;
    double acc = 0;
    for (size_t i = 0; i < bytes.size(); ++i)
        for (int k = 0; k < 4; ++k) {
            const int sym = (bytes[i] >> (6 - 2 * k)) & 3;
            const double w = 2 * M_PI * (f0 + sym * spacing) / fs;
            acc += sps;
            const int ns = (int)std::lround(acc); acc -= ns;
            for (int j = 0; j < ns; ++j) { out.push_back(amp * (float)std::cos(phase)); phase += w; if (phase > M_PI) phase -= 2 * M_PI; }
        }
}
/** A whole transmission: our preamble (0x1B — every tone in turn, so a receiver sees all four early), then the frame. */
inline std::vector<uint8_t> transmission(const std::vector<uint8_t>& packet, int preambleBytes = 8) {
    std::vector<uint8_t> b((size_t)preambleBytes, 0x1B);
    const auto f = encodeFrame(packet.data(), (int)packet.size());
    b.insert(b.end(), f.begin(), f.end());
    return b;
}

// ── the demodulator ────────────────────────────────────────────────────────────────────────────────────────────────
/** ★ 12 kHz audio in (balloon_dsp.h Decim4), tones from the shared spectrum (setTones), decoded packets out.
 *  Bounded: 4 × one symbol of complex history, kBits soft bits, at most 8 pending UWs. */
class Demod {
public:
    static constexpr int kFs = 12000, kBits = 1024;
    /** ★ The UW may have 2 of its 16 bits wrong (measured on Project Horus's own v2 recording, horus_v2_100bd.raw: 1 wrong
     *  allowed found 6 of its 10 packets, 2 found 9 — the tenth is cut off by the file's end). Each extra try costs one
     *  Golay decode; the CRC and the field checks still judge every one (test-balloon: minutes of noise, nothing). */
    static constexpr int kUwErr = 2;
    std::function<void(const Telemetry&)> onPacket;
    explicit Demod(double baud = 100) { setBaud(baud); }
    void setBaud(double baud) {
        baud_ = std::max(25.0, std::min(400.0, baud));
        N_ = (int)std::lround(kFs / baud_);
        for (int k = 0; k < 4; ++k) ring_[k].assign((size_t)N_ * 2, 0.f);
        metric_.assign((size_t)N_, 0.0);
        resetClock_();
    }
    double baud() const { return baud_; }
    /** The four tones (Hz, lowest first) — from findFour. ★ The spectrum only PLACES the tones: its peaks for 100-baud
     *  4-FSK are broad and wander by tens of hertz from one spectrum to the next. Once placed, each tone is held by its own
     *  frequency loop (symbol_: the phase turn between the two halves of every symbol that was that tone), and the
     *  spectrum is listened to again only when it puts a tone a fifth of a spacing (≥ 30 Hz) from where it is held. */
    void setTones(const double t[4]) {
        if (haveTones_) {
            const double sp = std::fabs(tones_[3] - tones_[0]) / 3;
            bool far = false;
            for (int k = 0; k < 4; ++k) if (std::fabs(t[k] - anchor_[k]) > std::max(30.0, 0.2 * sp)) far = true;
            if (!far) return;
        }
        ++toneSets;
        for (int k = 0; k < 4; ++k) { tones_[k] = anchor_[k] = t[k]; setStep_(k); }
        for (int k = 0; k < 4; ++k) std::fill(ring_[k].begin(), ring_[k].end(), 0.f);
        resetClock_();
        haveTones_ = true;
    }
    void clearTones() { haveTones_ = false; }
    bool haveTones() const { return haveTones_; }
    const double* tones() const { return tones_; }
    /** Where the spectrum placed the four (the loops hold each within 40 Hz of these) — what a person is told. */
    const double* placed() const { return anchor_; }
    unsigned long packets = 0, crcFails = 0, uwHits = 0, toneSets = 0;

    void process(const float* x, int n) {
        if (!haveTones_) return;
        for (int i = 0; i < n; ++i) sample_(x[i]);
    }

    /** ★ The per-sample tone bank, four lanes (one tone each): oscillator step, mix, the running one-symbol sums.
     *  Public so the test can run the scalar and SIMD paths on the same input. */
    struct Bank { float oRe[4], oIm[4], stRe[4], stIm[4], sRe[4], sIm[4]; };
    static inline void bankStep(Bank& b, float x, const float* oldRe, const float* oldIm, float* newRe, float* newIm, float* e) {
#if defined(VIBE_BALLOON_NEON)
        const float32x4_t oR = vld1q_f32(b.oRe), oI = vld1q_f32(b.oIm), sR = vld1q_f32(b.stRe), sI = vld1q_f32(b.stIm);
        const float32x4_t nR = vsubq_f32(vmulq_f32(oR, sR), vmulq_f32(oI, sI));
        const float32x4_t nI = vaddq_f32(vmulq_f32(oR, sI), vmulq_f32(oI, sR));
        vst1q_f32(b.oRe, nR); vst1q_f32(b.oIm, nI);
        const float32x4_t xv = vdupq_n_f32(x);
        const float32x4_t zR = vmulq_f32(xv, nR), zI = vmulq_f32(xv, nI);
        vst1q_f32(newRe, zR); vst1q_f32(newIm, zI);
        const float32x4_t aR = vaddq_f32(vld1q_f32(b.sRe), vsubq_f32(zR, vld1q_f32(oldRe)));
        const float32x4_t aI = vaddq_f32(vld1q_f32(b.sIm), vsubq_f32(zI, vld1q_f32(oldIm)));
        vst1q_f32(b.sRe, aR); vst1q_f32(b.sIm, aI);
        vst1q_f32(e, vaddq_f32(vmulq_f32(aR, aR), vmulq_f32(aI, aI)));
#elif defined(VIBE_BALLOON_SSE2)
        const __m128 oR = _mm_loadu_ps(b.oRe), oI = _mm_loadu_ps(b.oIm), sR = _mm_loadu_ps(b.stRe), sI = _mm_loadu_ps(b.stIm);
        const __m128 nR = _mm_sub_ps(_mm_mul_ps(oR, sR), _mm_mul_ps(oI, sI));
        const __m128 nI = _mm_add_ps(_mm_mul_ps(oR, sI), _mm_mul_ps(oI, sR));
        _mm_storeu_ps(b.oRe, nR); _mm_storeu_ps(b.oIm, nI);
        const __m128 xv = _mm_set1_ps(x);
        const __m128 zR = _mm_mul_ps(xv, nR), zI = _mm_mul_ps(xv, nI);
        _mm_storeu_ps(newRe, zR); _mm_storeu_ps(newIm, zI);
        const __m128 aR = _mm_add_ps(_mm_loadu_ps(b.sRe), _mm_sub_ps(zR, _mm_loadu_ps(oldRe)));
        const __m128 aI = _mm_add_ps(_mm_loadu_ps(b.sIm), _mm_sub_ps(zI, _mm_loadu_ps(oldIm)));
        _mm_storeu_ps(b.sRe, aR); _mm_storeu_ps(b.sIm, aI);
        _mm_storeu_ps(e, _mm_add_ps(_mm_mul_ps(aR, aR), _mm_mul_ps(aI, aI)));
#else
        bankStepScalar(b, x, oldRe, oldIm, newRe, newIm, e);
#endif
    }
    static inline void bankStepScalar(Bank& b, float x, const float* oldRe, const float* oldIm, float* newRe, float* newIm, float* e) {
        for (int k = 0; k < 4; ++k) {
            volatile float p1 = b.oRe[k] * b.stRe[k], p2 = b.oIm[k] * b.stIm[k], p3 = b.oRe[k] * b.stIm[k], p4 = b.oIm[k] * b.stRe[k];
            const float nR = p1 - p2, nI = p3 + p4;
            b.oRe[k] = nR; b.oIm[k] = nI;
            const float zR = x * nR, zI = x * nI;
            newRe[k] = zR; newIm[k] = zI;
            volatile float dR = zR - oldRe[k], dI = zI - oldIm[k];
            const float aR = b.sRe[k] + dR, aI = b.sIm[k] + dI;
            b.sRe[k] = aR; b.sIm[k] = aI;
            volatile float q1 = aR * aR, q2 = aI * aI;
            e[k] = q1 + q2;
        }
    }

private:
    void resetClock_() {
        std::fill(metric_.begin(), metric_.end(), 0.0);
        n_ = 0; next_ = N_; locked_ = 0; pos_ = 0;
        for (int k = 0; k < 4; ++k) { bank_.oRe[k] = 1; bank_.oIm[k] = 0; bank_.stRe[k] = stRe_[k]; bank_.stIm[k] = stIm_[k]; bank_.sRe[k] = bank_.sIm[k] = 0; }
    }
    void sample_(float x) {
        for (int k = 0; k < 4; ++k) { bank_.stRe[k] = stRe_[k]; bank_.stIm[k] = stIm_[k]; }
        // the ring holds each lane's last N mixes; slot pos_ is the oldest (about to leave the sum)
        float oldR[4], oldI[4], nwR[4], nwI[4], e[4];
        for (int k = 0; k < 4; ++k) { oldR[k] = ring_[k][(size_t)pos_]; oldI[k] = ring_[k][(size_t)(pos_ + N_)]; }
        bankStep(bank_, x, oldR, oldI, nwR, nwI, e);
        for (int k = 0; k < 4; ++k) { ring_[k][(size_t)pos_] = nwR[k]; ring_[k][(size_t)(pos_ + N_)] = nwI[k]; }
        if (++pos_ == N_) pos_ = 0;
        ++n_;
        // ★ every 50 symbols: the oscillators back to unit size and the sums re-added from the ring (float drift)
        if (n_ % (N_ * 50) == 0) {
            for (int k = 0; k < 4; ++k) {
                const float g = 1.0f / std::sqrt(bank_.oRe[k] * bank_.oRe[k] + bank_.oIm[k] * bank_.oIm[k]);
                bank_.oRe[k] *= g; bank_.oIm[k] *= g;
                double r = 0, im = 0;
                for (int j = 0; j < N_; ++j) { r += ring_[k][(size_t)j]; im += ring_[k][(size_t)(j + N_)]; }
                bank_.sRe[k] = (float)r; bank_.sIm[k] = (float)im;
            }
        }
        const float emax = std::max(std::max(e[0], e[1]), std::max(e[2], e[3]));
        const float esum = e[0] + e[1] + e[2] + e[3];
        // ★ the clock: at the symbol's end the strongest tone's share of the energy peaks
        const int ph = (int)(n_ % N_);
        metric_[(size_t)ph] = metric_[(size_t)ph] * 0.96 + (esum > 0 ? emax / esum : 0.0);
        if (n_ < next_) return;
        // a decision
        double bestM = -1; int bestPh = ph;
        for (int p = 0; p < N_; ++p) if (metric_[(size_t)p] > bestM) { bestM = metric_[(size_t)p]; bestPh = p; }
        int err = bestPh - ph;
        if (err > N_ / 2) err -= N_; else if (err < -N_ / 2) err += N_;
        if (locked_ < 24 && std::abs(err) > N_ / 10) next_ += N_ + err;          // still finding it: jump there
        else next_ += N_ + (err > 1 ? 1 : err < -1 ? -1 : 0);                   // then follow, a sample a symbol
        ++locked_;
        symbol_(e);
    }
    void setStep_(int k) {
        const double w = 2 * M_PI * tones_[k] / kFs;
        stRe_[k] = (float)std::cos(w); stIm_[k] = (float)-std::sin(w);
    }
    /** ★ The frequency loop: the decided tone's mix over the symbol, first half against second — its turn is how far the
     *  tone is off. Only when that tone stood well clear of the others (4×), a twentieth of the error a symbol, kept within
     *  40 Hz of where the spectrum placed it. */
    void afc_(const float* e) {
        // ★ once Horus has decoded, only inside a frame (the 600 bits after a UW): between packets a payload sends RTTY on
        //   the same frequency, and its tones would pull the held ones off by up to the 40 Hz clamp (seen: 254 Hz spacing
        //   reported for a 270 Hz payload after an RTTY burst)
        if (packets > 0 && nb_ - lastUw_ > 600) return;
        int k = 0; for (int j = 1; j < 4; ++j) if (e[j] > e[k]) k = j;
        float second = 0; for (int j = 0; j < 4; ++j) if (j != k) second = std::max(second, e[j]);
        if (e[k] < 4 * second) return;
        const int h = N_ / 2;
        double aR = 0, aI = 0, bR = 0, bI = 0;
        for (int j = 0; j < N_; ++j) {
            const int idx = (pos_ + j) % N_;          // pos_ is the oldest
            const double r = ring_[k][(size_t)idx], im = ring_[k][(size_t)(idx + N_)];
            if (j < h) { aR += r; aI += im; } else { bR += r; bI += im; }
        }
        // the mix is x·e^{−jωt}: a tone above the oscillator turns it forwards
        const double cr = bR * aR + bI * aI, ci = bI * aR - bR * aI;
        const double err = std::atan2(ci, cr) * kFs / (2 * M_PI * h);
        double f = tones_[k] + 0.05 * err;
        f = std::max(anchor_[k] - 40, std::min(anchor_[k] + 40, f));
        if (f != tones_[k]) { tones_[k] = f; setStep_(k); }
    }
    void symbol_(const float* e) {
        afc_(e);
        // ★ scaled by the AVERAGE symbol's strongest tone, not this symbol's own sum: a symbol hit by noise (all four alike)
        //   then says "unsure" instead of being as sure as a clean one — what Chase needs to pick the right bits to try
        const float mx = std::max(std::max(e[0], e[1]), std::max(e[2], e[3]));
        eAvg_ += (mx - eAvg_) * (eAvg_ > 0 ? 0.02f : 1.0f);
        const float sum = eAvg_ + 1e-20f;
        // natural binary: the first bit is the symbol's high bit (tones 2, 3), the second its low (tones 1, 3)
        const float b1 = (std::max(e[2], e[3]) - std::max(e[0], e[1])) / sum;
        const float b0 = (std::max(e[1], e[3]) - std::max(e[0], e[2])) / sum;
        pushBit_(b1); pushBit_(b0);
    }
    void pushBit_(float v) {
        bits_[(size_t)(nb_ % kBits)] = v;
        ++nb_;
        hard_ = (uint16_t)((hard_ << 1) | (v > 0 ? 1 : 0));
        // ★ the UW, either tone order (reversed tones turn every bit over): at most kUwErr bits wrong
        if (nb_ >= 16) {
            const int dn = __builtin_popcount((unsigned)(uint16_t)(hard_ ^ 0x2424)), di = __builtin_popcount((unsigned)(uint16_t)(hard_ ^ 0xDBDB));
            if ((dn <= kUwErr || di <= kUwErr) && pend_.size() < 8) { pend_.push_back({ nb_, di < dn, false }); ++uwHits; lastUw_ = nb_; }
        }
        for (size_t i = 0; i < pend_.size();) {
            Pend& p = pend_[i];
            const long long have = nb_ - p.start;
            bool drop = false;
            if (!p.triedV1 && have >= (txBytes(kV1Bytes) - 2) * 8) { p.triedV1 = true; if (tryDecode_(p, kV1Bytes)) drop = true; }
            if (!drop && have >= (txBytes(kV2Bytes) - 2) * 8) { tryDecode_(p, kV2Bytes); drop = true; }
            if (drop) pend_.erase(pend_.begin() + (long)i); else ++i;
        }
    }
    struct Pend { long long start; bool inv; bool triedV1; };   // start = the bit after the UW
    bool tryDecode_(const Pend& p, int n) {
        const int nbits = (txBytes(n) - 2) * 8;
        if (nb_ - p.start > kBits) return false;
        std::vector<float> s((size_t)nbits);
        for (int i = 0; i < nbits; ++i) { const float v = bits_[(size_t)((p.start + i) % kBits)]; s[(size_t)i] = p.inv ? -v : v; }
        uint8_t out[kV2Bytes]; int fixed = 0;
        decodeSoft(s.data(), n, out, &fixed);
        Telemetry t;
        if (!parsePacket(out, n, t)) { if (n == kV2Bytes) ++crcFails; return false; }
        t.fecFixed = fixed;
        ++packets;
        if (onPacket) onPacket(t);
        return true;
    }

    double baud_ = 100, tones_[4] = {0, 0, 0, 0}, anchor_[4] = {0, 0, 0, 0};
    float stRe_[4] = {1, 1, 1, 1}, stIm_[4] = {0, 0, 0, 0};
    bool haveTones_ = false;
    int N_ = 120, pos_ = 0, locked_ = 0;
    long long n_ = 0, next_ = 120, nb_ = 0, lastUw_ = -100000;
    Bank bank_{};
    std::vector<float> ring_[4];
    std::vector<double> metric_;
    float bits_[kBits] = {0};
    uint16_t hard_ = 0;
    float eAvg_ = 0;
    std::vector<Pend> pend_;
};

} // namespace horus
} // namespace balloon
} // namespace vibe
