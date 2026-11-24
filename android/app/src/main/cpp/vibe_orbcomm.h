// vibe_orbcomm.h — ORBCOMM subscriber downlink (137–138 MHz, 4800 bit/s SDPSK): demodulator, framer and the packets'
// named values (V12, 2026-11-24 — Stuart: "we can treat orbcomm as a regular satellite, just put the caveat that
// encrypted messages not decoded but we can show the raw data").
//
//   IQ slice (any rate ≥ ~29 kS/s) → Scanner (which carriers are up) → one Chan per carrier:
//     NCO (its carrier, moved by the AFC) → decimating low-pass (SIMD) → root-raised-cosine (α 0.4) + interpolator in one
//     (vibelrpt::PolyRrc) → Gardner timing → differential detection with a 4th-power frequency loop → the step's sign is the bit
//     → Framer (24-bit sync 65 A8 F9, 12-byte packets, a 24-byte ephemeris, Fletcher check + a small Chase repair)
//
// ★★★ SOURCES (published descriptions; every line of this file is ours):
//   · M. Kenny, "Ever wondered what is on the Orbcomm Satellite Downlink?" (May 2000, corrected Dec 2002) — SDPSK and
//     NRZ-M, 4800 bit/s, RRC α 0.4, LSB first, 600-byte minor frames of 50 packets, the packet types (65 sync · 1A message
//     · 1B uplink · 1C downlink · 1D NCC · 1E fill · 1F ephemeris · 22 orbital elements), the sync packet's fields, the
//     channel plan (137.0 MHz + N × 2.5 kHz downlink, 148.0 MHz + N × 2.5 kHz uplink), the 1B/1C channel nibbles, the 22
//     packet's mean anomaly / mean motion, GPS week + time of week, and the Fletcher routine (from ORBCOMM's own Serial
//     Interface Specification E80050015, Appendix F, reproduced in his Appendix B).
//   · ORBCOMM Serial Interface Specification E80050015 Rev F (via F. Bieberly's MIT-licensed ORBCOMM-receiver, read for
//     FACTS only): the ephemeris position/velocity scaling — 20-bit offset values over ±8 378 155 m and ±7 700 m/s — and
//     their order in the packet (x y z ẋ ẏ ż from the least significant bits up).
//   · J. Harms, "The Orbcomm Experience" (OHB, 2004) and OrbID (arXiv 2503.02118, 2025) — that the second-generation
//     (OG2) satellites still send this format, and the sync packet's satellite ID / downlink channel number.
//
// ★★ WHAT IS NEVER DECODED: subscriber traffic (type 1A "message" packets) is private user data, sent "encoded,
//    compressed or encrypted in an unknown manner" (Kenny). It is shown only as RAW BYTES and labelled as not decoded.
//    Everything decoded here is the satellite's own broadcast housekeeping, which every subscriber terminal reads.
//
// ★ COST (Pi 2 / 256 MB rule): per carrier one complex NCO at the slice rate and a low-pass evaluated only at the
//   decimated rate (~29–58 kS/s); the matched filter only at 2 points a symbol. Memory is fixed per carrier (a few KB) and
//   at most kMaxChans carriers. The scanner is one small FFT every ~50 ms.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vibe_lrpt_demod.h"   // vibelrpt::PolyRrc (matched filter + interpolator), fftInPlace — shared, not copied

namespace vibeorb {

constexpr double kBaud = 4800;               ///< bit/s (the satellites can be set to 9600; none is seen doing so)
constexpr double kRrcAlpha = 0.4;
constexpr double kDownBaseHz = 137.0e6;      ///< downlink channel N = 137.0 MHz + N × 2.5 kHz
constexpr double kUpBaseHz = 148.0e6;        ///< uplink channel N = 148.0 MHz + N × 2.5 kHz
constexpr double kChanStepHz = 2500;
constexpr int kMaxChans = 6;                 ///< carriers demodulated at once (a slice of the whole band sees 3–5)
inline double downlinkHz(int n) { return kDownBaseHz + n * kChanStepHz; }
inline double uplinkHz(int n) { return kUpBaseHz + n * kChanStepHz; }

// ── Packets ───────────────────────────────────────────────────────────────────────────────────────────────────────
enum : uint8_t { kSync = 0x65, kMessage = 0x1A, kUplink = 0x1B, kDownlink = 0x1C, kNcc = 0x1D, kFill = 0x1E, kEphemeris = 0x1F, kElements = 0x22 };
inline int packetLen(uint8_t type) { return type == kEphemeris ? 24 : 12; }
inline bool knownType(uint8_t t) { return t == kSync || (t >= kMessage && t <= kEphemeris) || t == kElements; }
inline const char* typeName(uint8_t t) {
    switch (t) {
    case kSync: return "sync"; case kMessage: return "message"; case kUplink: return "uplink"; case kDownlink: return "downlink";
    case kNcc: return "ncc"; case kFill: return "fill"; case kEphemeris: return "ephemeris"; case kElements: return "elements";
    default: return "unknown";
    }
}
/** ORBCOMM's Fletcher check (E80050015 Appendix F): both running sums are zero over an error-free packet. */
inline bool fletcherOk(const uint8_t* p, int n) {
    uint8_t c0 = 0, c1 = 0;
    for (int i = 0; i < n; i++) { c0 = uint8_t(c0 + p[i]); c1 = uint8_t(c1 + c0); }
    return c0 == 0 && c1 == 0;
}
/** The two check bytes for `n-2` data bytes (the encoder; the tests build packets with it). */
inline void fletcherSet(uint8_t* p, int n) {
    p[n - 2] = p[n - 1] = 0;
    uint8_t c0 = 0, c1 = 0;
    for (int i = 0; i < n; i++) { c0 = uint8_t(c0 + p[i]); c1 = uint8_t(c1 + c0); }
    p[n - 2] = uint8_t(c0 - c1);          // (Kenny prints the routine with the opposite sign convention)
    p[n - 1] = uint8_t(c1 - 2 * c0);
}

struct Packet {
    std::array<uint8_t, 24> b{};
    int len = 12;
    bool ok = false;         ///< the Fletcher check passed (after any repair)
    int repaired = 0;        ///< symbols the Chase repair flipped (0 = as received)
    uint64_t sym = 0;        ///< the channel's symbol count at the packet's last bit (its time)
    uint8_t type() const { return b[0]; }
    std::string hex() const {
        static const char* d = "0123456789ABCDEF";
        std::string s; s.reserve(size_t(len) * 3);
        for (int i = 0; i < len; i++) { if (i) s += ' '; s += d[b[size_t(i)] >> 4]; s += d[b[size_t(i)] & 15]; }
        return s;
    }
};

/** Sync packet: 65 A8 F9 · ID · 10 · F · MV · 00 00 00 · FCS. N = F, plus 0x100 when F < 0x50 (Kenny: channels 80–320). */
struct SyncInfo { int id = 0; int channel = 0; int frame = 0; int flag = 0; };
inline bool decodeSync(const Packet& p, SyncInfo& s) {
    if (p.len != 12 || p.b[0] != 0x65 || p.b[1] != 0xA8 || p.b[2] != 0xF9) return false;
    s.id = p.b[3];
    s.channel = p.b[5] + (p.b[5] < 0x50 ? 0x100 : 0);
    s.frame = p.b[6] >> 4; s.flag = p.b[6] & 15;
    return true;
}
/** 1B: byte 2 = retries (high nibble) and A/C slots (low); bytes 3–9 little-endian = four 12-bit uplink channels. */
struct UplinkInfo { int retries = 0, slots = 0; std::vector<int> channels; };
inline bool decodeUplink(const Packet& p, UplinkInfo& u) {
    if (p.type() != kUplink) return false;
    u.retries = p.b[2] >> 4; u.slots = p.b[2] & 15;
    uint64_t v = 0; for (int i = 9; i >= 3; i--) v = (v << 8) | p.b[size_t(i)];
    for (int k = 0; k < 4; k++) { const int c = int((v >> (12 * k)) & 0xFFF); if (c) u.channels.push_back(c); }
    return true;
}
/** 1C: byte 1 = packets in the set (high nibble) and this one's index (low); bytes 2–9 little-endian = five 12-bit
 *  downlink channels (the first of the first packet is the satellite's own). */
struct DownlinkInfo { int count = 0, index = 0; std::vector<int> channels; };
inline bool decodeDownlink(const Packet& p, DownlinkInfo& d) {
    if (p.type() != kDownlink) return false;
    d.count = p.b[1] >> 4; d.index = p.b[1] & 15;
    uint64_t v = 0; for (int i = 9; i >= 2; i--) v = (v << 8) | p.b[size_t(i)];
    for (int k = 0; k < 5; k++) { const int c = int((v >> (12 * k)) & 0xFFF); if (c) d.channels.push_back(c); }
    return true;
}
/** 22: the spacecraft the elements are for (bytes 1–2), mean anomaly (24 bits, bytes 3–5) and mean motion (32 bits,
 *  bytes 6–9), both little-endian fractions (Kenny: MM full scale 15.00000106 rev/day, found empirically). */
struct ElementsInfo { int sc = 0; double meanAnomalyDeg = 0, meanMotionRevDay = 0; };
inline bool decodeElements(const Packet& p, ElementsInfo& e) {
    if (p.type() != kElements) return false;
    e.sc = p.b[1];
    const uint32_t ma = uint32_t(p.b[3]) | uint32_t(p.b[4]) << 8 | uint32_t(p.b[5]) << 16;
    const uint32_t mm = uint32_t(p.b[6]) | uint32_t(p.b[7]) << 8 | uint32_t(p.b[8]) << 16 | uint32_t(p.b[9]) << 24;
    e.meanAnomalyDeg = double(ma) / 16777215.0 * 360.0;
    e.meanMotionRevDay = double(mm) / 4294967295.0 * 15.00000106;
    return true;
}
/** 1F (24 bytes): ID, then 120 bits little-endian = x y z ẋ ẏ ż as 20-bit offset values (E80050015 Rev F scaling),
 *  then time of week (24 bits) and GPS week (16 bits). Position in metres ECEF, velocity m/s. */
struct Ephemeris {
    int id = 0, week = 0, tow = 0;
    double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    double latDeg = 0, lonDeg = 0, altKm = 0, speedKmS = 0;
    double unix = 0;   ///< the satellite's clock as UTC (GPS − leap seconds), 0 = implausible
};
constexpr double kEphRmax = 8378155.0, kEphVmax = 7700.0;
constexpr int kGpsLeap = 18;                 ///< GPS − UTC since 2017 (no leap second announced since)
inline bool decodeEphemeris(const Packet& p, Ephemeris& e, double nowUnix = 0) {
    if (p.type() != kEphemeris || p.len != 24) return false;
    e.id = p.b[1];
    // 120 bits little-endian from byte 2 — three 40-bit halves keep the shifts inside 64 bits
    auto field = [&](int k) -> uint32_t {   // the k-th 20-bit field
        const int bit0 = 20 * k;
        uint32_t v = 0;
        for (int i = 0; i < 20; i++) { const int b = bit0 + i; if (p.b[size_t(2 + b / 8)] >> (b % 8) & 1) v |= 1u << i; }
        return v;
    };
    auto pos = [](uint32_t v) { return 2.0 * double(v) * kEphRmax / 1048576.0 - kEphRmax; };
    auto vel = [](uint32_t v) { return 2.0 * double(v) * kEphVmax / 1048576.0 - kEphVmax; };
    e.x = pos(field(0)); e.y = pos(field(1)); e.z = pos(field(2));
    e.vx = vel(field(3)); e.vy = vel(field(4)); e.vz = vel(field(5));
    e.tow = int(p.b[17]) | int(p.b[18]) << 8 | int(p.b[19]) << 16;
    e.week = int(p.b[20]) | int(p.b[21]) << 8;
    const double r = std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
    e.speedKmS = std::sqrt(e.vx * e.vx + e.vy * e.vy + e.vz * e.vz) / 1000.0;
    // geodetic (WGS-84, a few Bowring steps)
    const double a = 6378137.0, f = 1.0 / 298.257223563, e2 = f * (2 - f);
    const double pr = std::sqrt(e.x * e.x + e.y * e.y);
    double lat = std::atan2(e.z, pr * (1 - e2));
    for (int i = 0; i < 4; i++) { const double sl = std::sin(lat), n = a / std::sqrt(1 - e2 * sl * sl); lat = std::atan2(e.z + n * e2 * sl, pr); }
    const double sl = std::sin(lat), n = a / std::sqrt(1 - e2 * sl * sl);
    e.latDeg = lat * 180.0 / M_PI; e.lonDeg = std::atan2(e.y, e.x) * 180.0 / M_PI;
    e.altKm = (std::fabs(std::cos(lat)) > 1e-6 ? pr / std::cos(lat) - n : std::fabs(e.z) - n * (1 - e2)) / 1000.0;
    // GPS week + seconds → UTC. A 10-bit (rolled-over) week is brought forward to the nearest era of `nowUnix`.
    if (e.tow < 604800 && r > 6.4e6 && r < 8.4e6) {
        double t = 315964800.0 + double(e.week) * 604800.0 + double(e.tow) - kGpsLeap;
        if (nowUnix > 0) while (t < nowUnix - 512.0 * 604800.0) t += 1024.0 * 604800.0;
        e.unix = t;
    }
    return r > 6.4e6 && r < 8.4e6;
}

// ── The framer: info bits (with each symbol's confidence) → packets ─────────────────────────────────────────────────
/** Bits arrive LSB first. Hunt: the 24-bit sync 65 A8 F9 (≤ 2 bits wrong, then the sync packet's own Fletcher must pass).
 *  Locked: 12-byte packets (24 for an ephemeris) back to back, 4800 bits a minor frame; each minor frame must start
 *  with a sync again (≤ 4 bits wrong) — two missed in a row = hunt.
 *  ★ REPAIR (Chase): one wrong SYMBOL is one wrong bit (the bit is the phase step), so a packet that fails its check gets
 *    its weakest symbols flipped — singly and in pairs (kChase of them: 28 trials, a false pass ≈ 28 / 65 536). */
class Framer {
public:
    static constexpr int kChase = 7;
    std::function<void(const Packet&)> onPacket;
    struct Stats { uint64_t ok = 0, bad = 0, repaired = 0, frames = 0, syncMiss = 0; };

    void reset() { st_ = Hunt; sr_ = 0; n_ = 0; histN_ = 0; }
    bool locked() const { return st_ != Hunt; }
    const Stats& stats() const { return stats_; }

    /** One info bit, the confidence of the symbol it came from (bigger = surer) and of the symbol before it. */
    void push(int bit, float conf, uint64_t sym) {
        sym_ = sym;
        hist_[histN_ % kHist] = { uint8_t(bit), conf };
        histN_++;
        sr_ = (sr_ >> 1) | (uint32_t(bit & 1) << 23);
        if (st_ == Hunt) {
            /* ★ EITHER POLARITY: which sign of the ±90° step is a 1 depends on the spectrum's orientation (an IQ swap, a
             *  mixer's side) — Kenny's table has 1 = −90°; the sync word says which, and the rest of the run follows it. */
            const bool pos = popcount24(sr_ ^ kSyncWord) <= 2, neg = popcount24(sr_ ^ (~kSyncWord & 0xFFFFFF)) <= 2;
            if ((pos || neg) && histN_ >= 25) {
                inv_ = neg && !pos ? 1 : 0;
                // the 24 sync bits (and the confidence of the symbol before them) become the packet's start
                n_ = 0;
                for (int i = 24; i >= 1; i--) { const Bit& h = hist_[(histN_ - uint64_t(i)) % kHist]; bits_[n_] = uint8_t(h.b ^ inv_); conf_[n_ + 1] = h.c; n_++; }
                conf_[0] = hist_[(histN_ - 25) % kHist].c;
                frameBits_ = 24; want_ = 96; st_ = Tentative;
            }
            return;
        }
        bits_[n_] = uint8_t((bit ^ inv_) & 1); conf_[n_ + 1] = conf; n_++; frameBits_++;
        if (n_ == 8) {
            const uint8_t t = byteAt(0);
            want_ = packetLen(t) * 8;
            if (frameBits_ - 8 + want_ > 4800) want_ = 96;   // an ephemeris never straddles the minor frame
        }
        if (n_ == 24 && frameBits_ == 24 && st_ == Locked) {   // a minor frame's first packet: must be the sync
            if (popcount24(sr_ ^ (inv_ ? (~kSyncWord & 0xFFFFFF) : kSyncWord)) > 4) {
                stats_.syncMiss++;
                if (++missed_ >= 2) { st_ = Hunt; missed_ = 0; return; }
            } else missed_ = 0;
        }
        if (n_ >= want_) finishPacket_();
    }
private:
    static constexpr uint32_t kSyncWord = 0xF9A865;   // 65 A8 F9, LSB first, first bit at bit 0
    static constexpr size_t kHist = 64;
    enum State { Hunt, Tentative, Locked };
    struct Bit { uint8_t b = 0; float c = 0; };
    static int popcount24(uint32_t v) { v &= 0xFFFFFF; int n = 0; while (v) { v &= v - 1; n++; } return n; }
    uint8_t byteAt(int k) const { uint8_t v = 0; for (int i = 0; i < 8; i++) v |= uint8_t(bits_[size_t(8 * k + i)] << i); return v; }
    void finishPacket_() {
        Packet p; p.len = n_ / 8; p.sym = sym_;
        for (int k = 0; k < p.len; k++) p.b[size_t(k)] = byteAt(k);
        p.ok = fletcherOk(p.b.data(), p.len);
        if (!p.ok) chase_(p);
        const bool wasTentative = st_ == Tentative;
        if (wasTentative) {
            if (!p.ok || p.b[0] != 0x65) { st_ = Hunt; n_ = 0; return; }   // a false sync: keep hunting
            st_ = Locked; missed_ = 0;
        }
        if (p.b[0] == 0x65 && frameBits_ == p.len * 8) stats_.frames++;
        if (p.ok) { stats_.ok++; if (p.repaired) stats_.repaired++; } else stats_.bad++;
        if (onPacket) onPacket(p);
        n_ = 0;
        if (frameBits_ >= 4800) frameBits_ = 0;
        want_ = 96;
    }
    /** Flip the weakest symbols (one symbol = one bit: the data IS the phase step) until the check passes. */
    void chase_(Packet& p) {
        const int L = n_;
        std::array<int, kChase> weak{}; int nw = 0;
        // index s = bit s−1's symbol (conf_[0], the symbol before the packet, carries no bit of it)
        std::vector<std::pair<float, int>> c; c.reserve(size_t(L) + 1);
        for (int s = 0; s <= L; s++) c.push_back({ conf_[size_t(s)], s });
        const int k = std::min<int>(kChase, int(c.size()));
        std::partial_sort(c.begin(), c.begin() + k, c.end());
        for (int i = 0; i < k; i++) weak[size_t(nw++)] = c[size_t(i)].second;
        auto flip = [&](std::array<uint8_t, 24>& b, int s) {
            const int bit = s - 1; if (bit >= 0 && bit < L) b[size_t(bit / 8)] ^= uint8_t(1u << (bit % 8));
        };
        for (int i = 0; i < nw; i++) {
            std::array<uint8_t, 24> b = p.b; flip(b, weak[size_t(i)]);
            if (fletcherOk(b.data(), p.len)) { p.b = b; p.ok = true; p.repaired = 1; return; }
        }
        for (int i = 0; i < nw; i++) for (int j = i + 1; j < nw; j++) {
            std::array<uint8_t, 24> b = p.b; flip(b, weak[size_t(i)]); flip(b, weak[size_t(j)]);
            if (fletcherOk(b.data(), p.len)) { p.b = b; p.ok = true; p.repaired = 2; return; }
        }
    }
    State st_ = Hunt;
    uint32_t sr_ = 0;
    std::array<Bit, kHist> hist_{};
    uint64_t histN_ = 0, sym_ = 0;
    std::array<uint8_t, 192> bits_{};
    std::array<float, 193> conf_{};
    int n_ = 0, want_ = 96, frameBits_ = 0, missed_ = 0, inv_ = 0;
    Stats stats_;
};

// ── One carrier ─────────────────────────────────────────────────────────────────────────────────────────────────────
/** Decimating real-tap low-pass for a complex stream: every sample is stored, the dot product is taken only every D-th.
 *  NEON / SSE2 / scalar with the same four-lane partial sums (bit-identical, like vibelrpt::CFir). */
class DecimLp {
public:
    void configure(double fsIn, int decim, double cutoffHz, double transitionHz) {
        d_ = std::max(1, decim);
        int L = int(std::ceil(3.3 * fsIn / std::max(1000.0, transitionHz))) | 1;
        L = std::max(L, 9);
        n_ = (size_t(L) + 3) & ~size_t(3);
        taps_.assign(n_, 0.f);
        const double fc = cutoffHz / fsIn;
        double sum = 0;
        for (int i = 0; i < L; i++) {
            const double m = i - (L - 1) / 2.0;
            const double s = std::fabs(m) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * m) / (M_PI * m);
            const double w = 0.54 - 0.46 * std::cos(2 * M_PI * i / (L - 1));   // Hamming
            taps_[size_t(i)] = float(s * w); sum += s * w;
        }
        for (int i = 0; i < L; i++) taps_[size_t(i)] = float(taps_[size_t(i)] / sum);
        re_.assign(2 * n_, 0.f); im_.assign(2 * n_, 0.f); pos_ = 0; ph_ = 0;
    }
    int decim() const { return d_; }
    /** Push one sample; true (and the output) every D-th. */
    bool push(float r, float i, float& orr, float& oi) {
        re_[pos_] = re_[pos_ + n_] = r; im_[pos_] = im_[pos_ + n_] = i;
        pos_ = (pos_ + 1) % n_;
        if (++ph_ < d_) return false;
        ph_ = 0;
        orr = dot(&re_[pos_]); oi = dot(&im_[pos_]);
        return true;
    }
    bool forceScalar = false;
private:
    float dot(const float* x) const {
        const float* h = taps_.data();
        float l[4];
        if (forceScalar) { dotScalar(x, h, l); return (l[0] + l[1]) + (l[2] + l[3]); }
#if defined(VIBE_LRPT_FIR_NEON)
        float32x4_t a = vdupq_n_f32(0.f);
        for (size_t i = 0; i < n_; i += 4) a = vaddq_f32(a, vmulq_f32(vld1q_f32(x + i), vld1q_f32(h + i)));
        vst1q_f32(l, a);
#elif defined(VIBE_LRPT_FIR_SSE2)
        __m128 a = _mm_setzero_ps();
        for (size_t i = 0; i < n_; i += 4) a = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(x + i), _mm_loadu_ps(h + i)));
        _mm_storeu_ps(l, a);
#else
        dotScalar(x, h, l);
#endif
        return (l[0] + l[1]) + (l[2] + l[3]);
    }
    void dotScalar(const float* x, const float* h, float* l) const {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        l[0] = l[1] = l[2] = l[3] = 0.f;
        for (size_t i = 0; i < n_; i += 4)
            for (int k = 0; k < 4; k++) l[k] = l[k] + x[i + size_t(k)] * h[i + size_t(k)];
    }
    std::vector<float> taps_, re_, im_;
    size_t n_ = 4, pos_ = 0;
    int d_ = 1, ph_ = 0;
};

/** One ORBCOMM carrier cut from the slice: mix → decimate → matched filter + Gardner → differential detection with a
 *  4th-power AFC → the steps' signs (bits) → Framer. `offsetHz` is the carrier's place in the slice (Hz from its centre). */
class Chan {
public:
    struct Cfg {
        double fsIn = 0;              ///< the slice's rate
        double offsetHz = 0;          ///< where the scanner saw the carrier
        double acqRangeHz = 2400;     ///< the 4th-power search either side of it (the comb's whole unambiguous range)
    };
    explicit Chan(const Cfg& c) : cfg_(c), off_(c.offsetHz) {
        const int D = std::max(1, int(std::floor(c.fsIn / 28800.0)));
        fsD_ = c.fsIn / D;
        // pass ±(3.4 kHz signal + 1.5 kHz search), stop before anything that would fold onto ±5 kHz
        lp_.configure(c.fsIn, D, 7500, std::max(4000.0, fsD_ - 5000 - 7500));
        sps_ = fsD_ / kBaud;
        rrc_.configure(sps_, kRrcAlpha, 4);
        tau_ = rrc_.half() + 2;
        T_ = sps_;
        framer_.onPacket = [this](const Packet& p) { if (onPacket) onPacket(p, *this); };
        acq_.reserve(kAcqN);
    }
    std::function<void(const Packet&, const Chan&)> onPacket;

    /** Feed slice samples (the slice's own block). */
    void feed(const std::complex<float>* x, int n) {
        const double w = -2 * M_PI * off_ / cfg_.fsIn;
        // ★ by hand (no std::complex multiply: without -ffast-math it is a library call with NaN checks — the Pi 2 rule)
        const double sr = std::cos(w), si = std::sin(w);
        double cr = nco_.real(), ci = nco_.imag();
        for (int i = 0; i < n; i++) {
            const double xr = x[i].real(), xi = x[i].imag();
            const float mr = float(xr * cr - xi * ci), mi = float(xr * ci + xi * cr);
            const double t = cr * sr - ci * si; ci = cr * si + ci * sr; cr = t;
            float r, q;
            if (!lp_.push(mr, mi, r, q)) continue;
            rrc_.push(r, q);
            pump_();
        }
        const double a = std::sqrt(cr * cr + ci * ci);
        nco_ = { cr / a, ci / a };
    }
    double offsetHz() const { return off_; }
    double snrDb() const {
        const double s2 = 2 * m2_ * m2_ - m4_;
        if (!(m2_ > 0) || s2 <= 0) return -10;
        const double S = std::sqrt(s2), N = m2_ - S;
        return N > 0 ? std::min(40.0, 10 * std::log10(S / N)) : 40.0;
    }
    bool acquired() const { return acquired_; }
    bool rejected() const { return rejected_; }
    bool spur() const { return spur_; }                ///< rejected as a steady line (banned longer: spurs do not move)       ///< the x⁴ comb showed no SDPSK line: not an ORBCOMM carrier
    double acqRatio() const { return acqRatio_; }      ///< the line over the comb's mean (the tests read it)
    double stepMean() const { return stepAvg_; }       ///< mean |sin| of the symbol steps (the spur check; tests)
    /* (History: the x⁴ ratio was the gate — 16, then 7 over ±2 bins — until an oscillator's wander pushed a real 6 dB carrier
     *  under the noise's best: measured synthetic 6 dB 10.4, 8 dB 21, the 09:38 capture's noise at most 5.1. It is now only
     *  reported; the step statistic below is the gate.) */
    bool locked() const { return framer_.locked(); }
    uint64_t symbols() const { return syms_; }
    const Framer::Stats& stats() const { return framer_.stats(); }
    double sampleRate() const { return fsD_; }
    /** Packets that passed since the last call to ageGood() reset — the Receiver's drop rule reads it. */
    uint64_t lastGoodSym = 0;
    double createdSec = 0;            ///< the Receiver's clock when made (the drop rule)
    int id = 0;                       ///< the Receiver's number for it
private:
    static constexpr size_t kAcqN = 4096;   ///< four points a symbol: 0.21 s, bins of 1.2 Hz (of Δf)
    void pump_() {
        const uint64_t have = rrc_.count();
        while (true) {
            const double need = tau_ + rrc_.half() + 2;
            if (double(have) < need) return;
            const double tm = tau_ - T_ / 2;
            float yr, yi, mr, mi;
            const uint64_t n0 = uint64_t(std::floor(tau_)), n1 = uint64_t(std::floor(tm));
            rrc_.at(n0, tau_ - double(n0), yr, yi);
            rrc_.at(n1, tm - double(n1), mr, mi);
            const std::complex<float> y(yr, yi), mid(mr, mi);
            if (!acquired_) {   // the quarter points too: four points a symbol for the acquisition
                float ar, ai, br, bi;
                const double ta = tau_ - 3 * T_ / 4, tb = tau_ - T_ / 4;
                const uint64_t na = uint64_t(std::floor(ta)), nb = uint64_t(std::floor(tb));
                rrc_.at(na, ta - double(na), ar, ai);
                rrc_.at(nb, tb - double(nb), br, bi);
                acq_.push_back({ ar, ai }); acq_.push_back(mid); acq_.push_back({ br, bi }); acq_.push_back(y);
                if (acq_.size() >= kAcqN) acquire_();
                prev_ = y;
                tau_ += T_;
                continue;
            }
            symbol_(y, mid);
            tau_ += T_;
        }
    }
    void symbol_(std::complex<float> y, std::complex<float> mid) {
        // power (the AGC's scale and the SNR's moments)
        const float p = std::norm(y);
        pAvg_ += 0.002 * (p - pAvg_);
        m2_ += 0.002 * (double(p) - m2_); m4_ += 0.002 * (double(p) * p - m4_);
        // Gardner: e = Re{(y[k−1] − y[k]) · conj(mid)} / power — late gives e < 0
        const std::complex<float> dy = prev_ - y;
        const double e = pAvg_ > 0 ? double(dy.real() * mid.real() + dy.imag() * mid.imag()) / pAvg_ : 0.0;
        const double ec = std::max(-1.0, std::min(1.0, e));
        tau_ += kTp_ * ec * sps_;
        T_ += kTi_ * ec * sps_;
        T_ = std::max(sps_ * 0.995, std::min(sps_ * 1.005, T_));
        // differential detection: ±90° a symbol; d⁴ removes the data and leaves 4 × the residual rotation
        const std::complex<float> d = y * std::conj(prev_);
        prev_ = y;
        syms_++;
        const float ad = std::abs(d);
        if (ad > 0) {
            std::complex<float> u = d / ad;
            const std::complex<float> u2 = u * u, u4 = u2 * u2;
            acc4_ += 0.03f * (u4 * ad - acc4_);
        }
        const float th = std::arg(acc4_) / 4.f;          // residual rotation a symbol (radians)
        off_ += kAfc_ * double(th) * kBaud / (2 * M_PI);  // the NCO follows (feedback, a few hundred symbols)
        const std::complex<float> rot(std::cos(-th), std::sin(-th));
        const std::complex<float> dr = d * rot;
        /* ★★★ THE BIT IS THE PHASE STEP ITSELF (Kenny's own table: 1 = −90°, 0 = +90°; the step from symbol to symbol IS
         *  the differential coding he calls NRZ-M). The first build XORed successive steps as well — our transmitter did the
         *  same, so every synthetic test passed while the live FM116 pass (2026-11-24 15:09) never found a sync. */
        const int b = dr.imag() < 0 ? 1 : 0;
        // ★ a steady SPUR (the RTL's / an LNA's own lines) is no SDPSK: its steps sit at 0°, not ±90°
        stepAvg_ += 0.01 * (double(ad > 0 ? std::fabs(dr.imag()) / ad : 0.f) - stepAvg_);
        cosAvg_ += 0.01 * (double(ad > 0 ? std::fabs(dr.real()) / ad : 0.f) - cosAvg_);
        /* ★★ THE GATE (the first raw capture: the RTL's ripple adopted as 268 "carriers" in 10 minutes; the live FM116 pass:
         *  steady spurs at 137.474 / 137.282 / 137.666 found again and again). Half a second in, the mean |sin| of the steps:
         *  SDPSK ≥ 0.80 from 6 dB; noise 2/π = 0.64; a CW spur 0.40 down to 0.06. Below kNotSdpskStep = dropped; below
         *  kSpurStep it is a steady line and banned longer. */
        /* ★★ A QUARTER TURN OUT: acquisition can land a multiple of 1200 Hz off (one quarter turn a symbol — invisible to the
         *  d⁴ loop); then the steps sit at 0°/180° and look like a SPUR, but their |cos| is as high as SDPSK's |sin|. A
         *  spur's steps are near 0° too — the difference: a quarter-turn-out carrier's steps are 0° AND 180° (|cos| high,
         *  data-like), a spur's 0° only. So when |cos| is high, move 1200 Hz towards where the scanner saw the carrier and
         *  look again (twice at most); a steady line is caught by sign: a spur's mean step stays at +|d| (wantsQuarter_). */
        if (syms_ == lookAt_) {
#ifdef VIBE_ORB_DEBUG
            std::fprintf(stderr, "look: off %.0f (scan %.0f) sin %.3f cos %.3f turn %.3f acqRatio %.1f\n", off_, cfg_.offsetHz, stepAvg_, cosAvg_, turnSign_, acqRatio_);
#endif
            const bool quarter = cosAvg_ >= 0.66 && cosAvg_ > stepAvg_ + 0.08 && turnSign_ < kTurnSignMax && quarters_ < 2;   // noise: both ≈ 0.62
            if (quarter) {
                off_ += off_ > cfg_.offsetHz ? -1200.0 : 1200.0;
                quarters_++; lookAt_ = syms_ + kSpurLookSym; stepAvg_ = cosAvg_ = turnSum_ = 0; turnN_ = 0;
            } else if (stepAvg_ < kNotSdpskStep) { rejected_ = true; spur_ = stepAvg_ < kSpurStep; }
        }
        turnSum_ += ad > 0 ? double(dr.real()) / ad : 0.0; turnN_++;
        turnSign_ = turnN_ ? std::fabs(turnSum_ / turnN_) : 0.0;
        framer_.push(b, std::fabs(dr.imag()) / std::max(1e-12f, float(pAvg_)), syms_);
    }
    void acquire_() {
        // x⁴ (each point unit-normalised: the line, not the strongest burst), Hann window, FFT
        size_t N = 1; while (N < acq_.size()) N <<= 1;
        std::vector<std::complex<float>> a(N);
        for (size_t i = 0; i < acq_.size(); i++) {
            std::complex<float> v = acq_[i]; const float m = std::abs(v);
            if (m > 0) v /= m;
            const std::complex<float> v2 = v * v;
            const float w = 0.5f - 0.5f * std::cos(float(2 * M_PI * double(i) / double(acq_.size() - 1)));
            a[i] = v2 * v2 * w;
        }
        vibelrpt::fftInPlace(a);
        /* ★★ SDPSK's x⁴ is +1 at each symbol and −(smaller) half-way between (the ±45° transit), so its lines sit at 4Δf
         *  AND 4Δf ± the symbol rate — at two points a symbol the side line aliases onto 4Δf + 4800 and wins (a 1200 Hz
         *  error: exactly one quarter turn a symbol, the one rotation differential detection cannot see). Four points a
         *  symbol and the three lines summed (a comb) leave one answer within ±2400 Hz. */
        const double fsA = 4 * kBaud;
        const double binHz = fsA / double(N);
        const int side = int(std::lround(kBaud / binHz));
        auto pw = [&](int k) { return std::norm(a[size_t(((k % int(N)) + int(N)) % int(N))]); };
        /* ★ ±2 bins each (a pass's Doppler rate, up to ~35 Hz/s, smears 4× its drift over the 0.21 s look: ±30 Hz of x⁴) */
        auto line = [&](int k) { float a = 0; for (int j = -2; j <= 2; j++) a += pw(k + j); return a; };
        /* ★★ BOTH SIDE LINES, NOT THE CENTRE (the live-like test: a steady spur 6 kHz beside the carrier at 10 dB). A CW
         *  spur's x⁴ is ONE line — it can fill the centre slot but never both side slots at once; SDPSK always fills both.
         *  So the score is the weaker of the two side lines PLUS the centre: the side pair alone repeats every 9600 Hz of x⁴
         *  (a 2400 Hz error — 2 quarter turns a symbol, which the d⁴ loop cannot see either: the live-like test locked
         *  there and stayed); the centre line breaks that tie, so the answer is unique over ±2400 Hz. */
        auto comb = [&](int k) { return std::min(line(k - side), line(k + side)) + line(k); };
        int best = 0; float bestP = -1;
        const int lim = int(std::min(fsA / 2 - 3 * binHz, 4 * cfg_.acqRangeHz) / binHz);
        double sum = 0; int cnt = 0;
        for (int k = -lim; k <= lim; k++) {
            const float q = comb(k);
            sum += q; cnt++;
            if (q > bestP) { bestP = q; best = k; }
        }
        /* ★ The line over the comb's mean — REPORTED, no longer a gate: an oscillator's wander (or a fast Doppler rate)
         *  smears the x⁴ lines so a real 6 dB carrier scored under the noise's best (live-like test). The gate is the
         *  symbol steps' statistic half a second in (kNotSdpskStep / kSpurStep), which needs only the AFC to have pulled in. */
        acqRatio_ = cnt > 0 && sum > 0 ? double(bestP) / (sum / cnt) : 0.0;
        // parabolic refinement
        const float l = comb(best - 1), c = comb(best), r = comb(best + 1);
        const float den = l - 2 * c + r;
        const double frac = std::fabs(den) > 1e-20f ? 0.5 * (l - r) / den : 0.0;
        const double df = (best + frac) * binHz / 4.0;
        off_ += df;
        acq_.clear();
        acquired_ = true;
        acc4_ = { 0, 0 };
    }
    Cfg cfg_;
    double off_ = 0, fsD_ = 0, sps_ = 0, tau_ = 0, T_ = 0;
    std::complex<double> nco_{ 1, 0 };
    DecimLp lp_;
    vibelrpt::PolyRrc rrc_;
    std::vector<std::complex<float>> acq_;
    bool acquired_ = false, rejected_ = false, spur_ = false;
    double stepAvg_ = 0, cosAvg_ = 0, turnSum_ = 0, turnSign_ = 0;
    uint64_t turnN_ = 0, lookAt_ = kSpurLookSym;
    int quarters_ = 0;
    static constexpr double kTurnSignMax = 0.5;        ///< |mean cos| of the steps: a spur ≈ 1 (always +), data ≈ 0                               ///< mean |sin| of the symbol steps (SDPSK ≈ 0.8–1, a CW spur ≈ 0.4–0.6 in noise)
    static constexpr uint64_t kSpurLookSym = 2400;    ///< half a second after acquisition
    static constexpr double kSpurStep = 0.5;           ///< measured: a CW spur at C/N 3–20 dB 0.40–0.06
    static constexpr double kNotSdpskStep = 0.72;      ///< measured: SDPSK 6 dB 0.80, 8 dB 0.90; noise 0.64
    double acqRatio_ = 0;
    std::complex<float> prev_{ 0, 0 }, acc4_{ 0, 0 };
    double pAvg_ = 0, m2_ = 0, m4_ = 0;
    int lastB_ = 0;
    uint64_t syms_ = 0;
    Framer framer_;
    static constexpr double kTp_ = 0.02, kTi_ = 0.0002, kAfc_ = 0.004;
};

// ── The scanner: which carriers are up in the slice ─────────────────────────────────────────────────────────────────
/** Averages a power spectrum of the slice over ~1 s (an FFT every ~50 ms, never every sample) and names the ORBCOMM-
 *  shaped bumps: a peak of a ~6 kHz-wide smoothing ≥ kMinDb over the floor, peaks ≥ 12 kHz apart, centre = the power
 *  centroid over ±3.4 kHz. Only the flat part of the slice (±0.45 of its rate) is searched. */
class Scanner {
public:
    static constexpr double kMinDb = 2.0;
    struct Peak { double offsetHz = 0, db = 0; };
    void configure(double fs) {
        fs_ = fs;
        n_ = 256; while (fs / double(n_) > 400 && n_ < 8192) n_ <<= 1;
        acc_.assign(n_, 0.0); buf_.clear(); buf_.reserve(n_); every_ = std::max<size_t>(n_, size_t(fs * 0.05)); skip_ = 0; frames_ = 0;
        win_.resize(n_); for (size_t i = 0; i < n_; i++) win_[i] = float(0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(n_ - 1)));
    }
    double rate() const { return fs_; }
    /** Feed the slice; returns true when ~1 s has been averaged (then peaks() is fresh). */
    bool feed(const std::complex<float>* x, int n) {
        bool done = false;
        for (int i = 0; i < n; i++) {
            if (skip_ > 0) { skip_--; continue; }
            buf_.push_back(x[i]);
            if (buf_.size() < n_) continue;
            std::vector<std::complex<float>> a(n_);
            for (size_t k = 0; k < n_; k++) a[k] = buf_[k] * win_[k];
            vibelrpt::fftInPlace(a);
            for (size_t k = 0; k < n_; k++) acc_[k] += std::norm(a[k]);
            buf_.clear();
            skip_ = every_ - n_;
            // the first look after ~0.25 s (a pass should not wait), then one a second
            if (++frames_ >= (looks_ == 0 ? size_t(5) : std::max<size_t>(4, size_t(fs_ / double(every_))))) { analyse_(); looks_++; done = true; }
        }
        return done;
    }
    const std::vector<Peak>& peaks() const { return peaks_; }
    double floorDb() const { return floorDb_; }
private:
    void analyse_() {
        const double bin = fs_ / double(n_);
        const int half = int(n_ / 2);
        // fft-shifted power, dB-free (linear); bins −half … half−1
        std::vector<double> p(n_);
        for (int k = -half; k < half; k++) p[size_t(k + half)] = acc_[size_t((k + int(n_)) % int(n_))] / double(frames_);
        std::fill(acc_.begin(), acc_.end(), 0.0); frames_ = 0;
        const int lim = int(0.45 * fs_ / bin);
        std::vector<double> flat;
        for (int k = -lim; k < lim; k++) flat.push_back(p[size_t(k + half)]);
        if (flat.size() < 8) { peaks_.clear(); return; }
        /* ★ NARROW LINES OUT FIRST (spurs: an RTL's, an LNA's — one or two bins; ORBCOMM fills ~17+): a bin over 4× the
         *  median of its ±6 neighbours is cut to that median, so a spur never makes a peak, nor drags a carrier's centroid. */
        {
            std::vector<double> c = flat, w;
            for (size_t i = 0; i < flat.size(); i++) {
                w.clear();
                for (long j = long(i) - 6; j <= long(i) + 6; j++) if (j >= 0 && j < long(flat.size()) && j != long(i)) w.push_back(flat[size_t(j)]);
                std::nth_element(w.begin(), w.begin() + long(w.size() / 2), w.end());
                const double med = w[w.size() / 2];
                if (flat[i] > 4 * med) c[i] = med;
            }
            flat.swap(c);
        }
        std::vector<double> s = flat; std::nth_element(s.begin(), s.begin() + long(s.size() * 3 / 10), s.end());
        const double floor = std::max(1e-30, s[s.size() * 3 / 10]);
        floorDb_ = 10 * std::log10(floor);
        const int w = std::max(1, int(std::lround(5000 / bin)));   // a 5 kHz box
        std::vector<double> sm(flat.size(), 0.0);
        for (size_t i = 0; i < flat.size(); i++) {
            double a = 0; int c = 0;
            for (int j = -w / 2; j <= w / 2; j++) { const long q = long(i) + j; if (q >= 0 && q < long(flat.size())) { a += flat[size_t(q)]; c++; } }
            sm[i] = a / std::max(1, c);
        }
        const int sep = std::max(1, int(12000 / bin));
        std::vector<Peak> out;
        const double thr = floor * std::pow(10.0, kMinDb / 10.0);
        for (size_t i = 0; i < sm.size(); i++) {
            if (sm[i] < thr) continue;
            bool top = true;
            for (int j = -sep; j <= sep && top; j++) { const long q = long(i) + j; if (j && q >= 0 && q < long(sm.size()) && sm[size_t(q)] > sm[i]) top = false; }
            if (!top) continue;
            // centroid of the excess over ±3.4 kHz
            const int h = std::max(1, int(3400 / bin));
            double num = 0, den = 0;
            for (int j = -h; j <= h; j++) { const long q = long(i) + j; if (q < 0 || q >= long(flat.size())) continue; const double ex = std::max(0.0, flat[size_t(q)] - floor); num += ex * double(q); den += ex; }
            const double ci = den > 0 ? num / den : double(i);
            Peak pk; pk.offsetHz = (ci - lim) * bin; pk.db = 10 * std::log10(sm[i] / floor);
            out.push_back(pk);
            i += size_t(sep);
        }
        std::sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.db > b.db; });
        peaks_ = std::move(out);
    }
    double fs_ = 0, floorDb_ = 0;
    size_t n_ = 256, every_ = 256, skip_ = 0, frames_ = 0, looks_ = 0;
    std::vector<float> win_;
    std::vector<std::complex<float>> buf_;
    std::vector<double> acc_;
    std::vector<Peak> peaks_;
};

// ── The receiver: the scanner + a carrier each ──────────────────────────────────────────────────────────────────────
/** What a decoded packet comes out as: the packet, which carrier (its RF frequency now), the carrier's SNR, when. */
struct Rx {
    Packet p;
    int chan = 0;          ///< the Receiver's carrier number (stable while the carrier lives)
    double rfHz = 0;       ///< the carrier as received now (slice centre + its offset)
    double snrDb = 0;
    double t = 0;          ///< seconds on the caller's clock (the block's time + the packet's place in it)
};
class Receiver {
public:
    struct Cfg {
        int maxChans = 4;              ///< AUTO; the client's manual control can lower or raise it (≤ kMaxChans)
        double dropSec = 8;            ///< a carrier with no good packet this long is dropped
        double banSec = 12;            ///< … and its frequency is not retried for this long (a spur: 4×, ±2 kHz)
        /** Only carriers within this of the slice centre (Hz); 0 = the whole flat slice. A single-channel pick narrows it. */
        double windowHz = 0;
    };
    Receiver() : cfg_() {}
    explicit Receiver(Cfg c) : cfg_(c) {}
    std::function<void(const Rx&)> onPacket;
    void setMaxChans(int n) { cfg_.maxChans = std::max(1, std::min(kMaxChans, n)); }
    int maxChans() const { return cfg_.maxChans; }

    /** One block of the slice: rate, its RF centre, the time (s) of its first sample. */
    void feed(const std::complex<float>* x, int n, double fs, double centreHz, double t0) {
        if (n <= 0 || !(fs > 0)) return;
        if (std::fabs(fs - fs_) > 0.5) { fs_ = fs; scan_.configure(fs); chans_.clear(); }
        centre_ = centreHz;
        // the carriers first (their packets carry the block's time)
        for (auto& c : chans_) {
            blockT0_ = t0; blockSym0_ = c->symbols(); cur_ = c.get();
            c->feed(x, n);
        }
        const double tEnd = t0 + n / fs;
        now_ = tEnd;
        if (scan_.feed(x, n)) adopt_();
        // the drop rule
        for (size_t i = 0; i < chans_.size();) {
            Chan& c = *chans_[i];
            const double quietSec = double(c.symbols() - c.lastGoodSym) / kBaud;
            if (c.rejected() || (now_ - c.createdSec > cfg_.dropSec && quietSec > cfg_.dropSec)) {
                // ★ a spur does not move: banned 4× longer but only ±2 kHz (a real carrier 6 kHz beside it stays findable)
                /* ★ STRIKES: the same place refused again (the RTL's ripple does not move; a carrier moves with Doppler) doubles
                 *  its ban, up to 5 minutes — the 09:38 empty-sky capture opened ~965 short-lived candidates in 10 minutes
                 *  without this. A strike is forgotten after 10 minutes. */
                const long key = long(std::lround(c.offsetHz() / 2000.0));
                Strike& st = strikes_[key];
                if (now_ - st.at > 600) st.n = 0;
                st.at = now_;
                const double ban = std::min(300.0, (c.spur() ? 4 * cfg_.banSec : cfg_.banSec) * double(1 << std::min(st.n, 5)));
                if (c.rejected()) st.n++;
                if (strikes_.size() > 512) strikes_.clear();
                bans_.push_back({ c.offsetHz(), now_ + ban, c.spur() ? 2000.0 : 6000.0 });
                if (c.spur()) spurs_++; else if (c.rejected()) notSdpsk_++;
                chans_.erase(chans_.begin() + long(i));
            } else i++;
        }
        bans_.erase(std::remove_if(bans_.begin(), bans_.end(), [&](const Ban& b) { return b.until < now_; }), bans_.end());
    }
    struct ChanInfo { int id; double rfHz, snrDb; bool locked; uint64_t ok, bad, repaired, frames; };
    std::vector<ChanInfo> channels() const {
        std::vector<ChanInfo> v;
        for (auto& c : chans_) v.push_back({ c->id, centre_ + c->offsetHz(), c->snrDb(), c->locked(), c->stats().ok, c->stats().bad, c->stats().repaired, c->stats().frames });
        return v;
    }
    const std::vector<Scanner::Peak>& peaks() const { return scan_.peaks(); }
    uint64_t spursRejected() const { return spurs_; }      ///< steady lines refused (the spur check)
    uint64_t notSdpskRejected() const { return notSdpsk_; } ///< peaks with no x⁴ line (the carrier check)
    double floorDb() const { return scan_.floorDb(); }
    /** The scanner's level (dB over the floor) at the carrier nearest `rfHz` (within 4 kHz), or −99. */
    double levelAt(double rfHz) const {
        for (auto& p : scan_.peaks()) if (std::fabs(centre_ + p.offsetHz - rfHz) < 4000) return p.db;
        return -99;
    }
private:
    struct Ban { double off, until, radius; };
    void adopt_() {
        for (const Scanner::Peak& pk : scan_.peaks()) {
            if (int(chans_.size()) >= cfg_.maxChans) break;
            if (cfg_.windowHz > 0 && std::fabs(pk.offsetHz) > cfg_.windowHz) continue;
            bool near = false;
            for (auto& c : chans_) if (std::fabs(c->offsetHz() - pk.offsetHz) < 9000) near = true;
            for (auto& b : bans_) if (std::fabs(b.off - pk.offsetHz) < b.radius) near = true;
            if (near) continue;
            Chan::Cfg cc; cc.fsIn = fs_; cc.offsetHz = pk.offsetHz;
            std::unique_ptr<Chan> c(new Chan(cc));
            c->createdSec = now_; c->id = ++nextId_;
            c->onPacket = [this](const Packet& p, const Chan& ch) {
                Chan& m = const_cast<Chan&>(ch);
                if (p.ok) m.lastGoodSym = ch.symbols();
                if (!onPacket) return;
                Rx r; r.p = p; r.chan = ch.id; r.rfHz = centre_ + ch.offsetHz(); r.snrDb = ch.snrDb();
                r.t = (cur_ == &ch ? blockT0_ : now_) + double(ch.symbols() - (cur_ == &ch ? blockSym0_ : ch.symbols())) / kBaud;
                onPacket(r);
            };
            chans_.push_back(std::move(c));
        }
    }
    Cfg cfg_;
    double fs_ = 0, centre_ = 0, now_ = 0, blockT0_ = 0;
    uint64_t blockSym0_ = 0;
    const Chan* cur_ = nullptr;
    int nextId_ = 0;
    uint64_t spurs_ = 0, notSdpsk_ = 0;
    Scanner scan_;
    std::vector<std::unique_ptr<Chan>> chans_;
    std::vector<Ban> bans_;
    struct Strike { int n = 0; double at = 0; };
    std::map<long, Strike> strikes_;
};

}  // namespace vibeorb
