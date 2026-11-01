// vibe_hfdl.h — V12 HFDL: the HF aircraft datalink. PSK at 1800 symbols/s on USB (the carrier 1440 Hz above the dial),
// the preamble, an adaptive equaliser trained on every probe, the descrambler and deinterleaver, a soft K=7 Viterbi,
// and the PDUs (MPDU, LPDU, HFNPDU, ACARS inside). docs/v12/AVIATION.md §HFDL.
//
// ★★★ SOURCES. ITU-R M.1458 (free): M-PSK, 1800 Bd, the carrier at the SSB reference + 1440 Hz, raised-cosine shaping.
//     Everything else lives in ICAO Doc 9741 / ARINC 635 (paid); those facts were READ in dumphfdl (GPL-3.0-or-later,
//     read for inspiration only, no code copied): the A (127) and M1 (127, eight cyclic shifts = the eight rate/slot
//     modes) sequences, M2, the 15-symbol probe T (0x9AF), the frame of 30 data + 15 probe symbols (72 or 168 of them), the
//     15-bit scrambler restarted every 120 data symbols, the 40-row interleaver and its column steps (17 / 23), the
//     (171,133) K=7 code (rate ¼ = each coded bit sent twice), the PDU layouts and their CRC. Our own code throughout.
// ★★ THE RECEIVER (per channel, at exactly 4 samples a symbol = 7200 S/s):
//    1. DETECT the A sequence by DIFFERENTIAL correlation (y[n]·y*[n−4] against a_k·a_{k−1}): blind to the carrier's
//       frequency, so a ±20 Hz aircraft and HF Doppler do not smear it; the A1/A2 pair is told apart by which peak is
//       first.
//    2. BUFFER the whole burst (its length is known from M1), then work on it as a block — non-causally:
//       • timing: the fractional offset (¼-sample steps) with the highest 16-symbol coherence over A1+A2;
//       • frequency: the phase slope of 16-symbol segment correlations over the 254 known A symbols;
//       • M1: which of the eight shifts (the mode: BPSK/QPSK/8PSK, single/double slot);
//       • a T/2-spaced NLMS equaliser (15 taps) with a decision-directed carrier loop, TRAINED TWICE over the 531
//         known preamble symbols, then decision-directed through the data and re-trained on every 15-symbol probe.
//    3. soft values (the equaliser output against the noise measured on the probes) → descramble → deinterleave →
//       the rate-¼ pairs ADDED (soft combining) → Viterbi → the PDU bytes (LSB first).
#pragma once
#include "vibe_avi_front.h"
#include "vibe_acars.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace vibehfdl {

using cf32 = vibedsp::cf32;
static constexpr double kSymRate = 1800.0;
static constexpr int kSps = 4;
static constexpr double kWorkRate = kSymRate * kSps;   ///< 7200 S/s
static constexpr double kCarrierOffsetHz = 1440.0;      ///< above the USB dial (ITU-R M.1458)

static constexpr int kALen = 127, kM1Len = 127, kM2Len = 15, kTLen = 15, kDataLen = 30;
static constexpr int kPreambleKnown = 2 * kALen + kM1Len + kM2Len + 9 * kTLen;   ///< 531 symbols after the pre-key

/** The A sequence (127 BPSK symbols): bit 0 → +1, 1 → −1, first symbol on the air first. */
inline const std::array<int8_t, kALen>& aSeq() {
    static const std::array<int8_t, kALen> a = [] {
        static const uint8_t oct[16] = { 0x5B, 0xBC, 0x74, 0x57, 0x03, 0xD9, 0x89, 0x39, 0xF2, 0x08, 0xD5, 0x36, 0x94, 0x2C, 0x32, 0xFE };
        std::array<int8_t, kALen> r{};
        for (int i = 0; i < kALen; ++i) r[size_t(i)] = int8_t(((oct[i / 8] >> (7 - i % 8)) & 1) ? -1 : 1);
        return r;
    }();
    return a;
}
/** M1's base sequence (127 bits) and the eight shifts: index = the mode (0–3 single slot 300/600/1200/1800 bit/s, 4–7
 *  the same, double slot). */
inline int m1Bit(int i) {
    static const char* s = "0111011011110100010110010111110001000000110011011000111001110101110000100110000010101011010010010100111100100011010100001111111";
    return s[i % kM1Len] - '0';
}
static constexpr int kM1Shift[8] = { 72, 82, 113, 123, 61, 103, 93, 9 };
inline int8_t m1Sym(int mode, int j) { return int8_t(m1Bit(kM1Shift[mode] + j) ? -1 : 1); }
/** The probe / training sequence T (15 symbols), 0x9AF MSB first. */
inline int8_t tSym(int j) { return int8_t(((0x9AFu >> (14 - j)) & 1) ? -1 : 1); }

struct Mode { int bitsPerSym, segments, rate, colShift; };   // rate 4 = each coded bit twice
inline Mode modeOf(int m) {
    const int bps = (m & 3) == 0 || (m & 3) == 1 ? 1 : (m & 3) == 2 ? 2 : 3;
    return { bps, m < 4 ? 72 : 168, (m & 3) == 0 ? 4 : 2, m < 4 ? 17 : 23 };
}
inline int bitRate(int m) { static const int r[4] = { 300, 600, 1200, 1800 }; return r[m & 3]; }

struct Tuning { float minCoh = 0.25f, tSpan = 4.0f; float detQ = 0.55f; float mu = 0.03f; float pllA = 0.05f, pllB = 0.002f; int eqTaps = 15; bool scrambleOld = true; };
inline Tuning& tuning() { static Tuning t; return t; }
inline bool debug() { static const bool d = std::getenv("HFDL_DEBUG") != nullptr; return d; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  The coding layers
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** The scrambler: a 15-bit maximal sequence (taps x⁰ and x¹⁴ of the state, shifted left, the new bit in at the bottom),
 *  state 0x4D4B (0x6959 bit-reversed) at the start of every 120 data symbols; a 1 turns the symbol by π. */
struct Scrambler {
    uint32_t v = 0x4D4B; int pos = 0;
    int next() {
        if (pos == 120) { pos = 0; v = 0x4D4B; }
        ++pos;
        const int b = __builtin_popcount(v & 0x4001u) & 1;
        v = ((v << 1) | uint32_t(b)) & 0x7FFFu;
        return b;
    }
};

/** The deinterleaver's walk: values are written down 40 rows, the column stepping back `shift` after each write (and on
 *  a full column), then read in rows of 9 at a time. Returns the read order as indices into the written stream. */
inline std::vector<int> deinterleaveOrder(int total, int shift) {
    const int rows = 40, cols = total / rows;
    std::vector<int> table(size_t(rows * cols), -1);
    int r = 0, c = 0;
    for (int i = 0; i < total; ++i) {
        table[size_t(r * cols + c)] = i;
        if (++r == rows) { r = 0; ++c; }
        c -= shift; if (c < 0) c += cols;
    }
    std::vector<int> order;
    order.reserve(size_t(total));
    r = 0; c = 0;
    for (int i = 0; i < total; ++i) {
        order.push_back(table[size_t(r * cols + c)]);
        r = (r + 9) % rows;
        if (r == 0) ++c;
    }
    return order;
}

/** Soft K=7 rate-½ Viterbi, generators 0x6D (first) and 0x4F, encoder register = (state << 1 | bit), started and ended in
 *  state 0. `soft[2i], soft[2i+1]` = the pair for bit i, positive = 0. Returns the decoded bits (0/1). */
inline std::vector<uint8_t> viterbi27(const std::vector<float>& soft) {
    const int nb = int(soft.size() / 2);
    static const std::array<uint8_t, 128> outs = [] {
        std::array<uint8_t, 128> o{};
        for (int r = 0; r < 128; ++r) o[size_t(r)] = uint8_t((__builtin_popcount(r & 0x6D) & 1) | ((__builtin_popcount(r & 0x4F) & 1) << 1));
        return o;
    }();
    std::vector<float> pm(64, -1e30f), nm(64);
    pm[0] = 0;
    std::vector<uint64_t> dec(size_t(nb), 0);
    for (int i = 0; i < nb; ++i) {
        const float s0 = soft[size_t(2 * i)], s1 = soft[size_t(2 * i + 1)];
        float bm[4];
        for (int o = 0; o < 4; ++o) bm[o] = ((o & 1) ? -s0 : s0) + ((o & 2) ? -s1 : s1);
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ++ns) {
            // ns = ((ps << 1) | b) & 63 → b = ns & 1; ps = (ns >> 1) | (hi << 5), hi ∈ {0,1}
            const int b = ns & 1;
            const int p0 = ns >> 1, p1 = (ns >> 1) | 32;
            const float m0 = pm[size_t(p0)] + bm[outs[size_t(((p0 << 1) | b) & 127)]];
            const float m1 = pm[size_t(p1)] + bm[outs[size_t(((p1 << 1) | b) & 127)]];
            if (m1 > m0) { nm[size_t(ns)] = m1; d |= 1ull << ns; } else nm[size_t(ns)] = m0;
        }
        dec[size_t(i)] = d;
        pm.swap(nm);
    }
    std::vector<uint8_t> bits((size_t)nb);
    int s = 0;
    for (int i = nb - 1; i >= 0; --i) {
        bits[size_t(i)] = uint8_t(s & 1);
        const int hi = int((dec[size_t(i)] >> s) & 1);
        s = (s >> 1) | (hi << 5);
    }
    return bits;
}

/** The HFDL PDU check: CRC-16 reflected 0x1021, preset 0xFFFF, complemented, sent low byte first after `n` bytes. */
inline bool crcOk(const uint8_t* b, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) { c ^= b[i]; for (int k = 0; k < 8; ++k) c = (c & 1) ? uint16_t((c >> 1) ^ 0x8408) : uint16_t(c >> 1); }
    c ^= 0xFFFF;
    return (b[n] | (b[n + 1] << 8)) == c;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  PDUs
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint8_t rev8(uint8_t b) { b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4); b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2); return uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1); }
inline uint32_t icaoOf(const uint8_t* b) { return uint32_t(rev8(b[0])) << 16 | uint32_t(rev8(b[1])) << 8 | rev8(b[2]); }
inline double coord20(uint32_t c) { int32_t r = int32_t(c << 12) >> 12; return r * 180.0 / double(0x7FFFF); }

inline const char* gsName(int id) {
    switch (id) {
        case 1: return "San Francisco"; case 2: return "Molokai"; case 3: return "Reykjavik"; case 4: return "Riverhead";
        case 5: return "Auckland"; case 6: return "Hat Yai"; case 7: return "Shannon"; case 8: return "Johannesburg";
        case 9: return "Barrow"; case 10: return "Muan"; case 11: return "Albrook"; case 13: return "Santa Cruz";
        case 14: return "Krasnoyarsk"; case 15: return "Al Muharraq"; case 16: return "Agana"; case 17: return "Canarias";
        default: return "";
    }
}

/** One LPDU as decoded (aircraft-sourced or uplink). */
struct Message {
    bool downlink = true;
    int srcId = -1, dstId = -1;        ///< aircraft id (on this ground station) / ground station id
    int lpduType = -1;                 ///< 0x0D unnumbered data, 0x1D acked data, 0x8F logon request, …
    std::string typeName;
    uint32_t icao = 0;                 ///< logon PDUs
    int hfnpdu = -1;                   ///< 0xD1 performance data, 0xD5 frequency data, 0xFF ACARS, …
    std::string flight;
    double lat = NAN, lon = NAN;
    bool hasAcars = false, acarsCrcOk = false;
    vibeacars::Block acars;
    // the burst
    int channel = -1; double hz = 0, t = 0; int mode = -1; float snrDb = 0, freqErrHz = 0;
};

inline std::string lpduName(int t) {
    switch (t) {
        case 0x0D: return "Unnumbered data"; case 0x1D: return "Unnumbered ack'ed data"; case 0x2F: return "Logon denied";
        case 0x3F: return "Logoff request"; case 0x5F: return "Logon resume confirm"; case 0x4F: return "Logon resume";
        case 0x8F: return "Logon request (normal)"; case 0x9F: return "Logon confirm"; case 0xBF: return "Logon request (DLS)";
        default: return "LPDU";
    }
}

inline void parseHfnpdu(const uint8_t* b, size_t n, Message& m) {
    if (n < 2 || b[0] != 0xFF) return;
    m.hfnpdu = b[1];
    if ((b[1] == 0xD1 || b[1] == 0xD5) && n >= 15) {
        m.flight.assign(reinterpret_cast<const char*>(b + 2), 6);
        while (!m.flight.empty() && m.flight.back() == ' ') m.flight.pop_back();
        while (!m.flight.empty() && m.flight.front() == ' ') m.flight.erase(0, 1);
        const uint32_t la = uint32_t(b[8]) | uint32_t(b[9]) << 8 | uint32_t(b[10] & 0xF) << 16;
        const uint32_t lo = uint32_t(b[10] & 0xF0) >> 4 | uint32_t(b[11]) << 4 | uint32_t(b[12]) << 12;
        m.lat = coord20(la); m.lon = coord20(lo);
    } else if (b[1] == 0xFF && n > 2) {
        // ★ enveloped ACARS: the block as VHF sends it after its sync, SOH first (measured: FF FF 01 32 C8 …)
        size_t s0 = 2;
        if (b[s0] == vibeacars::SOH) ++s0;
        std::vector<uint8_t> a(b + s0, b + n);
        if (a.size() >= 16 && (a.back() & 0x7F) == vibeacars::DEL) {
            a.pop_back();
            m.acarsCrcOk = vibeacars::bcs(a.data(), a.size()) == 0;
            std::vector<uint8_t> p;
            for (size_t i = 0; i + 2 < a.size(); ++i) p.push_back(a[i] & 0x7F);
            m.hasAcars = vibeacars::parse(p, m.acars);
            if (m.hasAcars && m.flight.empty()) m.flight = m.acars.flight;
        }
    }
}

/** An MPDU (first byte's bit 0 set): every LPDU whose own CRC passes. */
inline std::vector<Message> parseMpdu(const uint8_t* b, size_t n) {
    std::vector<Message> out;
    if (n < 4 || !(b[0] & 1)) return out;
    struct L { int dst; const uint8_t* lens; int cnt; };
    std::vector<L> lists;
    size_t hdr;
    const bool down = b[0] & 2;
    if (down) {
        const int cnt = (b[0] >> 2) & 0xF;
        hdr = 6 + size_t(cnt);
        if (n < hdr + 2) return out;
        lists.push_back({ -1, b + 6, cnt });
    } else {
        const int ac = ((b[0] & 0x70) >> 4) + 1;
        hdr = 2;
        for (int i = 0; i < ac; ++i) {
            if (n < hdr + 2) return out;
            const int cnt = b[hdr + 1] >> 4;
            lists.push_back({ b[hdr], b + hdr + 2, cnt });
            hdr += 2 + size_t(cnt);
        }
        if (n < hdr + 2) return out;
    }
    if (!crcOk(b, hdr)) return out;
    const uint8_t* p = b + hdr + 2;
    const uint8_t* end = b + n;
    for (const L& l : lists)
        for (int j = 0; j < l.cnt; ++j) {
            const size_t len = size_t(l.lens[j]) + 1;
            if (p + len > end) return out;
            if (len >= 3 && crcOk(p, len - 2)) {
                Message m;
                m.downlink = down;
                if (down) { m.srcId = b[2]; m.dstId = b[1] & 0x7F; } else { m.srcId = b[1] & 0x7F; m.dstId = l.dst; }
                m.lpduType = p[0];
                m.typeName = lpduName(p[0]);
                const size_t body = len - 2;
                if (p[0] == 0x0D || p[0] == 0x1D) parseHfnpdu(p + 1, body - 1, m);
                else if ((p[0] == 0x8F || p[0] == 0xBF || p[0] == 0x4F || p[0] == 0x9F || p[0] == 0x5F || p[0] == 0x3F || p[0] == 0x2F) && body >= 4)
                    m.icao = icaoOf(p + 1);
                out.push_back(m);
            }
            p += len;
        }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ONE CHANNEL
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Stats { uint64_t detections = 0, noM1 = 0, bursts = 0, pdus = 0, crcBad = 0, messages = 0; };

class Demod {
public:
    explicit Demod(double fs) : fs_(fs), step_(fs / kWorkRate) {
        // a low-pass for the channel (the signal is ±1.2 kHz): flat to 1.3 kHz, shut by 2.2 kHz, at 7200 S/s
        lp_.reset(new vibedsp::FirDecimator(vibedsp::designLowpass(1750.0 / kWorkRate, 900.0 / kWorkRate, true), 1));
        const auto& a = aSeq();
        for (int k = 1; k < kALen; ++k) bb_[size_t(k - 1)] = float(a[size_t(k)] * a[size_t(k - 1)]);
    }
    void setChannel(int c, double hz) { chan_ = c; hz_ = hz; }
    const Stats& stats() const { return st_; }

    template <class Out> void feed(const cf32* z, int n, Out&& out) {
        rs_.clear();
        for (int i = 0; i < n; ++i) {
            h_[0] = h_[1]; h_[1] = h_[2]; h_[2] = h_[3]; h_[3] = z[i];
            while (mu_ < 1.0) {
                const float m = float(mu_);
                const cf32 p0 = h_[0], p1 = h_[1], p2 = h_[2], p3 = h_[3];
                const cf32 a0 = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3, a1 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3, a2 = -0.5f * p0 + 0.5f * p2;
                rs_.push_back(((a0 * m + a1) * m + a2) * m + p1);
                mu_ += step_;
            }
            mu_ -= 1.0;
        }
        y_.resize(rs_.size() + 4);
        const int k = lp_->process(rs_.data(), int(rs_.size()), y_.data());
        for (int i = 0; i < k; ++i) sample_(y_[size_t(i)], out);
    }

private:
    static constexpr int kRing = 4096;    // > A1 + A2 (254 symbols × 4) plus margin
    template <class Out> void sample_(cf32 v, Out& out) {
        const int64_t n = ++n_;
        ring_[size_t(n & (kRing - 1))] = v;
        const cf32 d = v * std::conj(ring_[size_t((n - kSps) & (kRing - 1))]);
        dif_[size_t(n & (kRing - 1))] = d;
        difAbs_[size_t(n & (kRing - 1))] = std::abs(d);
        if (inBurst_) {
            burst_.push_back(v);
            if (int(burst_.size()) >= need_) { process_(out); inBurst_ = false; burst_.clear(); cool_ = 4 * 200; }
            return;
        }
        if (cool_ > 0) { --cool_; return; }
        if (n < 4 * 260) return;
        // ★ the differential correlator: D(n) = Σ_k bb[k]·d[n − 4(125−k)]
        cf32 D(0, 0); float sa = 0;
        for (int k = 0; k < 126; ++k) {
            const size_t ix = size_t((n - 4 * (125 - k)) & (kRing - 1));
            D += bb_[size_t(k)] * dif_[ix]; sa += difAbs_[ix];
        }
        const float q = sa > 0 ? std::abs(D) / sa : 0;
        if (cand_) {
            if (q > bestQ_) { bestQ_ = q; bestN_ = n; }
            if (n - bestN_ > 8) {
                // ★ A1 or A2? A2's peak is 127 symbols after A1's, so a burst is first seen at A1 — but if the
                //   correlator was quiet then (fading), it may be A2. Assume A1; process_() checks the other way.
                st_.detections++;
                startBurst_(bestN_);
                cand_ = false;
            }
        } else if (q > tuning().detQ) { cand_ = true; bestQ_ = q; bestN_ = n; }
    }

    /** The burst's samples from 2 symbols before A1's first symbol, to the longest frame's end (refined in process_). */
    void startBurst_(int64_t aEnd) {
        const int64_t a1Start = aEnd - 4 * 126;
        burst_.clear();
        // ★ from 127 symbols EARLIER too: if A1 faded and this was A2, A1 is there (process_ tries both)
        for (int64_t m = a1Start - 8 - 4 * kALen; m <= n_; ++m) burst_.push_back(ring_[size_t(m & (kRing - 1))]);
        burstOff_ = 8 + 4 * kALen;                         // A1's first symbol (if this was A1) at burst_[burstOff_]
        // the longest burst: A1 A2 M1 M2 9T + 168 × 45, plus margin
        need_ = burstOff_ + 4 * (kPreambleKnown + 168 * 45 + 20);
        inBurst_ = true;
        burstT_ = double(a1Start) / kWorkRate;
    }

    cf32 at_(double t) const {   // the burst at a fractional sample index
        const int i = int(std::floor(t));
        if (i < 0 || i + 1 >= int(burst_.size())) return cf32(0, 0);
        const float f = float(t - i);
        return burst_[size_t(i)] * (1 - f) + burst_[size_t(i + 1)] * f;
    }

    template <class Out> void process_(Out& out) {
        const auto& A = aSeq();
        // 1. timing and frequency from A1 + A2 (254 known symbols): ¼-sample steps over ±2 samples
        double bestT = 0; float bestCoh = -1; double bestF = 0;
        for (int which = 0; which < 2; ++which)
        for (double off = -tuning().tSpan; off <= tuning().tSpan; off += 0.25) {
            const double t0 = burstOff_ - (which ? 4 * kALen : 0) + off;   // which = 1: the detection was A2
            // 16-symbol segments
            cf32 seg[16]; float mag = 0;
            for (int s = 0; s < 15; ++s) {
                cf32 c(0, 0);
                for (int k = 0; k < 16; ++k) {
                    const int idx = s * 16 + k;
                    const cf32 v = at_(t0 + 4.0 * idx);
                    c += v * float(A[size_t(idx % kALen)]);
                    mag += std::abs(v);
                }
                seg[s] = c;
            }
            float coh = 0; for (int s = 0; s < 15; ++s) coh += std::abs(seg[s]);
            coh /= (mag + 1e-12f);
            if (coh > bestCoh) {
                bestCoh = coh; bestT = t0;
                cf32 r(0, 0); for (int s = 1; s < 15; ++s) r += seg[s] * std::conj(seg[s - 1]);
                bestF = std::arg(r) / 16.0;                 // rad a symbol
            }
        }
        if (bestCoh < tuning().minCoh) { if (debug()) std::fprintf(stderr, "  [hfdl ch%d t=%.2f] A coherence %.2f: no burst\n", chan_, burstT_, bestCoh); return; }
        // the carrier: derotate by bestF, phase from A1+A2
        auto sym = [&](int k) { return at_(bestT + 4.0 * k) * std::polar(1.0f, -float(bestF * k)); };
        cf32 ph(0, 0);
        for (int k = 0; k < 2 * kALen; ++k) ph += sym(k) * float(A[size_t(k % kALen)]);
        const float phi0 = std::arg(ph);
        // 2. M1: which of the eight modes (coherent, 16-symbol segments so a slow phase drift does not hurt)
        int mode = -1; float bestM = 0, second = 0;
        for (int m = 0; m < 8; ++m) {
            float s = 0, mag = 0;
            for (int g = 0; g < 8; ++g) {
                cf32 c(0, 0);
                for (int k = g * 16; k < std::min(kM1Len, g * 16 + 16); ++k) { const cf32 v = sym(2 * kALen + k); c += v * float(m1Sym(m, k)); mag += std::abs(v); }
                s += std::abs(c);
            }
            s /= (mag + 1e-12f);
            if (s > bestM) { second = bestM; bestM = s; mode = m; } else if (s > second) second = s;
        }
        if (mode < 0 || bestM < 0.3f || bestM < second * 1.4f) {
            st_.noM1++;
            if (debug()) std::fprintf(stderr, "  [hfdl ch%d t=%.2f] A coherence %.2f, M1 unclear (%.2f vs %.2f)\n", chan_, burstT_, bestCoh, bestM, second);
            return;
        }
        st_.bursts++;
        const Mode md = modeOf(mode);
        const int nData = md.segments * kDataLen;
        const int total = kPreambleKnown + md.segments * (kDataLen + kTLen);
        // 3. the equaliser over the whole burst: taps on half-symbol spacing (2 samples at 4 a symbol)
        const int L = std::max(5, tuning().eqTaps | 1), C = L / 2;
        std::vector<cf32> w(size_t(L), cf32(0, 0));
        w[size_t(C)] = cf32(1, 0);
        float phi = phi0, dphi = float(bestF);
        const float mu = tuning().mu, pa = tuning().pllA, pb = tuning().pllB;
        // the known symbol at burst position k (preamble), or 0 for data
        auto known = [&](int k) -> float {
            if (k < kALen) return A[size_t(k)];
            if (k < 2 * kALen) return A[size_t(k - kALen)];
            if (k < 2 * kALen + kM1Len) return m1Sym(mode, k - 2 * kALen);
            if (k < 2 * kALen + kM1Len + kM2Len) return m1Sym(mode, k - 2 * kALen - kM1Len);
            if (k < kPreambleKnown) return tSym((k - 2 * kALen - kM1Len - kM2Len) % kTLen);
            const int r = (k - kPreambleKnown) % (kDataLen + kTLen);
            return r >= kDataLen ? tSym(r - kDataLen) : 0.0f;
        };
        std::vector<cf32> x((size_t)L);
        auto eqOut = [&](int k, float ph) {
            const double tc = bestT + 4.0 * k;
            float e2 = 0;
            for (int j = 0; j < L; ++j) { x[size_t(j)] = at_(tc + 2.0 * (j - C)) * std::polar(1.0f, -ph); e2 += std::norm(x[size_t(j)]); }
            cf32 o(0, 0);
            for (int j = 0; j < L; ++j) o += w[size_t(j)] * x[size_t(j)];
            return std::make_pair(o, e2);
        };
        auto adapt = [&](cf32 o, cf32 dsym, float e2) {
            const cf32 e = dsym - o;
            const float g = mu / (e2 + 1e-9f);
            for (int j = 0; j < L; ++j) w[size_t(j)] += g * e * std::conj(x[size_t(j)]);
        };
        // the scale: equaliser output amplitude on the known symbols → normalise to unit symbols
        float amp = 0; for (int k = 0; k < 2 * kALen; ++k) amp += std::abs(sym(k));
        amp /= (2 * kALen);
        for (auto& c : w) c /= amp + 1e-12f;
        // train twice over the known preamble (non-causal: the burst is buffered)
        for (int pass = 0; pass < 2; ++pass) {
            float p = phi0, dp = float(bestF);
            for (int k = 0; k < kPreambleKnown; ++k) {
                auto [o, e2] = eqOut(k, p);
                const float ref = known(k);
                adapt(o, cf32(ref, 0), e2);
                const float pe = std::arg(o * ref);
                p += dp + pa * pe; dp += pb * pe;
            }
            if (pass == 1) { phi = p; dphi = dp; }
        }
        // through the data: decision-directed on data, trained on every probe
        std::vector<cf32> dsyms; dsyms.reserve(size_t(nData));
        float noise = 0; int noiseN = 0;
        const int M = 1 << md.bitsPerSym;
        Scrambler scr;
        for (int k = kPreambleKnown; k < total; ++k) {
            auto [o, e2] = eqOut(k, phi);
            const float ref = known(k);
            cf32 dsym;
            if (ref != 0) { dsym = cf32(ref, 0); noise += std::norm(o - dsym); ++noiseN; }
            else {
                // the nearest point of the constellation (the scrambler's π is a point of every PSK used)
                const float a = std::arg(o);
                const int idx = int(std::lround(a / (2 * float(M_PI) / M)));
                dsym = std::polar(1.0f, float(idx) * 2 * float(M_PI) / M);
                dsyms.push_back(o);
            }
            adapt(o, dsym, e2);
            const float pe = std::arg(o * std::conj(dsym));
            phi += dphi + pa * pe; dphi += pb * pe;
        }
        const float sigma2 = noiseN ? noise / noiseN : 0.5f;
        // 4. soft bits: descramble (a π turn when the scrambler gives 1), BPSK/QPSK/8PSK soft values, + = 0
        std::vector<float> coded;
        coded.reserve(size_t(nData * md.bitsPerSym));
        for (cf32 o : dsyms) {
            if (scr.next()) o = -o;
            if (md.bitsPerSym == 1) coded.push_back(2 * o.real() / sigma2);
            else {
                // ★ QPSK / 8PSK: max-log soft bits for natural Gray-coded PSK (point i at i·2π/M, label gray(i)).
                //   UNVERIFIED on air — the reference recordings hold only 300 and 600 bit/s.
                float best0[3] = { -1e30f, -1e30f, -1e30f }, best1[3] = { -1e30f, -1e30f, -1e30f };
                for (int i = 0; i < M; ++i) {
                    const cf32 pt = std::polar(1.0f, float(i) * 2 * float(M_PI) / M);
                    const float met = -std::norm(o - pt) / sigma2;
                    const int lab = i ^ (i >> 1);
                    for (int b = 0; b < md.bitsPerSym; ++b) {
                        float& slot = ((lab >> (md.bitsPerSym - 1 - b)) & 1) ? best1[b] : best0[b];
                        slot = std::max(slot, met);
                    }
                }
                for (int b = 0; b < md.bitsPerSym; ++b) coded.push_back(best0[b] - best1[b]);
            }
        }
        const std::vector<int> order = deinterleaveOrder(int(coded.size()), md.colShift);
        std::vector<float> v;
        v.reserve(coded.size());
        for (int idx : order) v.push_back(coded[size_t(idx)]);
        if (md.rate == 4) {   // each coded bit sent twice: add the pair
            std::vector<float> c2(v.size() / 2);
            for (size_t i = 0; i < c2.size(); ++i) c2[i] = v[2 * i] + v[2 * i + 1];
            v.swap(c2);
        }
        const auto bits = viterbi27(v);
        std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
        for (size_t i = 0; i < bits.size(); ++i) bytes[i / 8] |= uint8_t(bits[i] << (i % 8));
        st_.pdus++;
        const float snr = 10 * std::log10(1.0f / (sigma2 + 1e-12f));
        if (debug()) {
            std::fprintf(stderr, "  [hfdl ch%d t=%.2f] mode %d (%d bit/s %c), A coh %.2f, M1 %.2f/%.2f, f %+.1f Hz, probe SNR %.1f dB, bytes:", chan_, burstT_, mode,
                         bitRate(mode), mode < 4 ? 'S' : 'D', bestCoh, bestM, second, bestF / (2 * M_PI) * kSymRate, snr);
            for (size_t i = 0; i < 24 && i < bytes.size(); ++i) std::fprintf(stderr, " %02x", bytes[i]);
            std::fprintf(stderr, "\n");
        }
        auto msgs = parseMpdu(bytes.data(), bytes.size());
        if (msgs.empty()) st_.crcBad++;
        for (auto& m : msgs) {
            m.channel = chan_; m.hz = hz_; m.t = burstT_; m.mode = mode; m.snrDb = snr;
            m.freqErrHz = float(bestF / (2 * M_PI) * kSymRate);
            st_.messages++;
            out(m);
        }
    }

    double fs_, step_, mu_ = 0;
    cf32 h_[4] = {};
    std::vector<cf32> rs_, y_;
    std::unique_ptr<vibedsp::FirDecimator> lp_;
    cf32 ring_[kRing] = {}, dif_[kRing] = {};
    float difAbs_[kRing] = {};
    float bb_[126];
    int64_t n_ = 0;
    bool cand_ = false; float bestQ_ = 0; int64_t bestN_ = 0;
    bool inBurst_ = false; int need_ = 0, burstOff_ = 0, cool_ = 0; double burstT_ = 0;
    std::vector<cf32> burst_;
    Stats st_;
    int chan_ = -1; double hz_ = 0;
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  Many channels from one capture (the offline tool, and a wide capture); a listener's dial uses one Demod directly
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Receiver {
public:
    /** `dialHz` = the HFDL frequencies (the USB dial, kHz × 1000); each channel is centred 1440 Hz above. */
    Receiver(double rate, double hwCentreHz, const std::vector<double>& dialHz)
        : mc_(rate, hwCentreHz, carriers_(dialHz), 3000, 7200, 400) {
        for (size_t c = 0; c < dialHz.size(); ++c) {
            demod_.emplace_back(new Demod(mc_.chanRate()));
            demod_.back()->setChannel(int(c), dialHz[c]);
        }
    }
    template <class Out> void feed(const cf32* iq, size_t n, Out&& out) {
        mc_.feed(iq, n, [&](int c, const cf32* z, int k) { demod_[size_t(c)]->feed(z, k, out); });
    }
    const vibeavi::MultiChannel& front() const { return mc_; }
    const Demod& demod(size_t c) const { return *demod_[c]; }
    size_t channels() const { return demod_.size(); }

private:
    static std::vector<double> carriers_(const std::vector<double>& d) { std::vector<double> v; for (double x : d) v.push_back(x + kCarrierOffsetHz); return v; }
    vibeavi::MultiChannel mc_;
    std::vector<std::unique_ptr<Demod>> demod_;
};

}  // namespace vibehfdl
