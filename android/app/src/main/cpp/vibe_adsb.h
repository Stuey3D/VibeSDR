// vibe_adsb.h — ADS-B / Mode S on 1090 MHz: demodulator, CRC, message decoding, CPR positions and the
// aircraft table. Header-only, like the DAB pieces beside it.
//
// ★★★ OUR OWN, FROM THE PUBLISHED STANDARD. ICAO Annex 10 Vol IV (the Mode S downlink: PPM at 1 Mb/s, the
//     8 µs preamble, CRC-24 with generator 0x1FFF409, address/parity overlay) and the DO-260B message
//     layouts as documented openly (Junzi Sun, "The 1090 Megahertz Riddle", 2nd ed., TU Delft, 2021 — the
//     test vectors in vibeserver/test-adsb.cpp are its worked examples). dump1090/readsb are GPL and are
//     NOT copied — they are a behaviour reference only (DECODER-RESEARCH.md principle 2).
//
// ★★ WHY THE WHOLE RADIO. A Mode S reply is 1 µs bits split into 0.5 µs halves: it needs ≥ 2 MS/s centred
//    on 1090 MHz, which is every sample the radio makes. So this is a whole-profile mode like DAB, not a
//    channel cut from a wider capture.
//
// ★★ WHAT IS BELIEVED, AND WHY. Noise produces thousands of preamble-shaped bumps a second. Only these
//    create an aircraft:
//      · DF17/18 extended squitter with a CRC remainder of 0 (or 1 bit repaired — never 2, which would
//        let noise in);
//      · DF11 all-call with a remainder of exactly 0.
//    Every other reply (DF0/4/5/16/20/21) carries its address XOR'd into the parity, so ANY 56/112 bits
//    yield SOME address — they are used only for an aircraft already heard by one of the two above.
//
// ★ Positions: CPR, globally from an even+odd pair 10 s apart at most, then locally from the aircraft's
//   own last position. Surface positions need a reference (the receiver's location, or the aircraft's last
//   airborne fix) — without one they are left out rather than guessed into the wrong quadrant.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vibeadsb {

// ── CRC-24 (Annex 10 Vol IV 3.1.2.3.3) ───────────────────────────────────────────────────────────────
inline const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 16;
            for (int k = 0; k < 8; ++k) c = (c & 0x800000) ? ((c << 1) ^ 0xFFF409) : (c << 1);
            a[i] = c & 0xFFFFFF;
        }
        return a;
    }();
    return t;
}

/** The CRC remainder XOR the transmitted parity: 0 for an intact DF11 (IID 0) / DF17 / DF18; the
 *  aircraft's address for the address/parity replies. `len` is 7 or 14 bytes. */
inline uint32_t syndrome(const uint8_t* m, int len) {
    const auto& t = crcTable();
    uint32_t c = 0;
    for (int i = 0; i < len - 3; ++i) c = ((c << 8) ^ t[((c >> 16) ^ m[i]) & 0xFF]) & 0xFFFFFF;
    return c ^ (uint32_t(m[len - 3]) << 16 | uint32_t(m[len - 2]) << 8 | m[len - 1]);
}

/** The syndrome of a single flipped bit in a 112-bit message → that bit. The CRC is linear, so a frame
 *  with one bad bit has exactly that bit's syndrome. Bits 0–4 (the DF) are never repaired: a "repair"
 *  that changes what kind of message it is would turn noise into a DF17. */
inline const std::unordered_map<uint32_t, int>& singleBitSyndromes() {
    static const std::unordered_map<uint32_t, int> m = [] {
        std::unordered_map<uint32_t, int> r;
        for (int b = 5; b < 112; ++b) {
            uint8_t e[14] = {};
            e[b >> 3] = uint8_t(0x80 >> (b & 7));
            r[syndrome(e, 14)] = b;
        }
        return r;
    }();
    return m;
}

inline int dfLengthBytes(int df) { return df >= 16 ? 14 : 7; }

// ── The demodulator ──────────────────────────────────────────────────────────────────────────────────
/** One reply as received: the bytes and the receive conditions. Not yet believed — see Tracker. */
struct Frame {
    uint8_t  b[14] = {};
    int      len = 0;          ///< 7 or 14
    float    level = 0;        ///< pulse power, dBFS (mean of the four preamble pulses)
    uint64_t sample = 0;       ///< position on the fine grid (125 ns units), for ordering
};

/**
 * PPM demodulation, measured on a FINE grid: every half-bit window (0.5 µs) is the mean |IQ| over it, at
 * four starting points per half-bit (125 ns apart), from a running integral of the capture's magnitude.
 *
 * ★★★ WHY NOT ONE SAMPLE PER HALF-BIT. At exactly 2 MS/s, with a reply that happens to start on a sample,
 *     one sample per half-bit is perfect — and that is the only case. A reply starting half a sample late
 *     smears every pulse over two samples, and at 2.048 MS/s the grid drifts across the reply: the first
 *     attempt heard 2–22 % of clean frames there (test-adsb). Measuring each window at four phases finds the
 *     one where the pulses sit inside their windows. ★ Even so, 2.0/2.048 MS/s lose the replies that split
 *     50/50 — ADS-B mode asks for 2.4 MS/s or more.
 * ★★★ TWO STAGES, FOR THE PI 2. Measuring the fine grid everywhere is 8 million windows a second, nearly all
 *     of them noise: 6.5 % of an M-series core, so most of a Pi 2's. A cheap test on the raw samples first —
 *     four samples above the floor where the four preamble pulses would be — and the fine grid is measured
 *     only around a sample that passes.
 * ★★ Where several phases pass the preamble, the checksum chooses: a DF11/17/18 that comes out intact (or one
 *    bit from it) on that phase. An address/parity reply proves itself only by naming an aircraft already
 *    tracked (`known`), or a wrong phase's rubbish would be passed on and the real frame behind it skipped.
 */
class Demod {
public:
    explicit Demod(double sampleRate)
        : rate_(sampleRate), h_(sampleRate * 0.5e-6), q_(sampleRate * 0.125e-6),
          span_(size_t(std::ceil(sampleRate * 121e-6)) + 4) {
        cum_.push_back(0.0);
        for (int k = 0; k < 4; ++k) pulseAt_[k] = int(std::lround(kPulseUs[k] * 1e-6 * sampleRate));
    }

    bool usable() const { return rate_ >= 1.99e6; }

    /** Feed raw IQ (any scale; levels are reported relative to 1.0 = full scale). */
    template <class Cb>
    void feed(const std::complex<float>* iq, size_t n, Cb&& onFrame) {
        auto none = [](uint32_t) { return false; };
        feed(iq, n, onFrame, none);
    }

    /** …with `known(address)`: is this an aircraft already tracked? (Tracker::known) — the only way an
     *  address/parity reply (DF0/4/5/16/20/21) can be told from noise. */
    template <class Cb, class Known>
    void feed(const std::complex<float>* iq, size_t n, Cb&& onFrame, Known&& known) {
        if (!usable()) return;
        mag_.reserve(mag_.size() + n);
        cum_.reserve(cum_.size() + n);
        for (size_t i = 0; i < n; ++i) {
            const float a = std::sqrt(iq[i].real() * iq[i].real() + iq[i].imag() * iq[i].imag());
            mag_.push_back(a);
            cum_.push_back(cum_.back() + a);
        }
        const float* m = mag_.data();
        while (pos_ + span_ < mag_.size()) {
            const size_t i = pos_++;
            const float v = m[i];
            // a noise floor that follows slowly and ignores pulses: replies are short and sparse
            noise_ += (std::min(v, noise_ * 3.0f) - noise_) * 1e-5f;
            if (i < skip_ || i < 2) continue;
            // stage 1: something above the floor at each of the four pulse positions (± a sample for smear)
            const float thr = noise_ * 2.2f;
            auto peak = [&](size_t k) { return std::max(m[k], m[k + 1]); };
            if (peak(i) < thr) continue;
            if (peak(i + size_t(pulseAt_[1])) < thr || peak(i + size_t(pulseAt_[2])) < thr || peak(i + size_t(pulseAt_[3])) < thr) continue;
            // stage 2: the fine grid around here
            const size_t used = tryAt(double(i) - 1.0, onFrame, known);
            if (used) skip_ = i + used;
        }
        trim();
    }

    uint64_t preambles() const { return preambles_; }   ///< preamble-shaped bumps that went on to bit slicing

private:
    static constexpr double kPulseUs[4] = {0.0, 1.0, 3.5, 4.5};
    // ★★ RELATIVE, AND LOOSE. Below 2.5 MS/s a 0.5 µs pulse straddles two samples and smears half into the gap
    //    beside it — the receiver's own filtering does the same — so the gaps of a clean preamble sit at 70–90 %
    //    of its pulses, not near zero. The CRC, not this test, is what keeps noise out.
    static constexpr float kLoMax = 0.92f, kGapMax = 0.85f;

    double integral(double x) const {                    // ∫ |IQ| from the buffer start to x (in samples)
        const size_t k = size_t(x);
        return cum_[k] + (x - double(k)) * mag_[k];
    }
    /** The mean |IQ| over the half-bit starting `fine` steps (125 ns) after x. */
    float win(double x, int fine) const {
        const double a = x + fine * q_;
        return float((integral(a + h_) - integral(a)) / h_);
    }

    /** Does a preamble start at x? Returns its score (pulses minus gaps), or 0. Fine offsets: 4 per half-bit. */
    float preambleAt(double x, float& hiOut) const {
        float p[57];
        for (int k : {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 44, 48, 52, 56}) p[k] = win(x, k);
        // pulses at 0, 1.0, 3.5, 4.5 µs → fine 0, 8, 28, 36; the half-bits between them must be lower
        if (!(p[0] > p[4] && p[8] > p[4] && p[8] > p[12] && p[28] > p[24] && p[28] > p[32] && p[36] > p[32])) return 0;
        const float hi = (p[0] + p[8] + p[28] + p[36]) * 0.25f;
        if (hi < noise_ * 2.8f) return 0;                // ≈ 9 dB above the floor
        const float lo = (p[4] + p[12] + p[24] + p[32]) * 0.25f;
        if (lo > hi * kLoMax) return 0;
        const float gap = (p[16] + p[20] + p[44] + p[48] + p[52] + p[56]) / 6.0f;   // 2–3 µs and 5.5–7.5 µs
        if (gap > hi * kGapMax) return 0;
        hiOut = hi;
        return hi - lo - gap;
    }

    Frame slice(double x, float hi) const {
        Frame f;
        auto bitAt = [&](int i) { return win(x, 64 + 8 * i) > win(x, 68 + 8 * i); };   // data from 8 µs
        for (int i = 0; i < 8; ++i) if (bitAt(i)) f.b[0] |= uint8_t(0x80 >> i);
        f.len = dfLengthBytes(f.b[0] >> 3);
        for (int i = 8; i < f.len * 8; ++i) if (bitAt(i)) f.b[i >> 3] |= uint8_t(0x80 >> (i & 7));
        f.level = 10.0f * std::log10(std::max(hi * hi, 1e-12f));
        f.sample = base_ + uint64_t(std::max(0.0, x / q_));
        return f;
    }

    /** Search the phases from x across the next two samples; returns the samples the reply used, or 0. */
    template <class Cb, class Known>
    size_t tryAt(double x0, Cb& onFrame, Known& known) {
        struct Cand { double x; float score, hi; };
        Cand c[24]; int nc = 0;
        for (double x = std::max(x0, 0.0); x < x0 + 2.0 && nc < 24; x += q_) {
            float h; const float s = preambleAt(x, h);
            if (s > 0) c[nc++] = {x, s, h};
        }
        if (!nc) return 0;
        ++preambles_;
        std::sort(c, c + nc, [](const Cand& a, const Cand& b) { return a.score > b.score; });
        for (int i = 0; i < std::min(nc, 8); ++i) {
            Frame f = slice(c[i].x, c[i].hi);
            const int df = f.b[0] >> 3;
            const uint32_t s = syndrome(f.b, f.len);
            bool good = false;
            if (df == 11) good = (s & ~0x7Fu) == 0;
            else if (df == 17 || df == 18) good = s == 0 || singleBitSyndromes().count(s);
            else if (plausibleDf(df)) good = known(s);
            if (good) {
                onFrame(f);
                return size_t((c[i].x - x0) + (8 + f.len * 8) * 1e-6 * rate_);
            }
        }
        return 0;
    }

    void trim() {
        if (pos_ < 262144) return;
        const size_t drop = pos_ - 4;                    // keep a little behind the cursor for the ± search
        mag_.erase(mag_.begin(), mag_.begin() + long(drop));
        const double c0 = cum_[drop];
        cum_.erase(cum_.begin(), cum_.begin() + long(drop));
        for (double& v : cum_) v -= c0;                    // keep the running sum small, so it stays exact
        pos_ -= drop; skip_ = skip_ > drop ? skip_ - drop : 0;
        base_ += uint64_t(double(drop) / q_);
    }

    static bool plausibleDf(int df) {
        return df == 0 || df == 4 || df == 5 || df == 11 || df == 16 || df == 17 || df == 18 || df == 20 || df == 21;
    }

    double rate_, h_, q_;
    size_t span_;                                        // samples a whole reply needs ahead of its start
    int pulseAt_[4] = {};
    std::vector<float> mag_;
    std::vector<double> cum_;
    size_t pos_ = 0, skip_ = 0;
    uint64_t base_ = 0, preambles_ = 0;
    float noise_ = 1e-3f;
};

// ── Field decoding helpers ───────────────────────────────────────────────────────────────────────────
/** Bits [first, last] of a byte array, 1-based as the standard numbers them. */
inline uint32_t bits(const uint8_t* m, int first, int last) {
    uint32_t v = 0;
    for (int i = first; i <= last; ++i) v = (v << 1) | ((m[(i - 1) >> 3] >> (7 - ((i - 1) & 7))) & 1);
    return v;
}

/** Gillham (Mode C) 100-ft altitude from the bits in the order D2 D4 A1 A2 A4 B1 B2 B4 C1 C2 C4.
 *  Returns false for a code that is not an altitude. */
inline bool gillhamToFeet(uint32_t d2d4a1a2a4b1b2b4, uint32_t c1c2c4, int& feet) {
    auto grayToInt = [](uint32_t g) { uint32_t n = g; while (g >>= 1) n ^= g; return n; };
    int n500 = int(grayToInt(d2d4a1a2a4b1b2b4));
    int n100 = int(grayToInt(c1c2c4));
    if (n100 == 0 || n100 == 5 || n100 == 6) return false;
    if (n100 == 7) n100 = 5;
    if (n500 & 1) n100 = 6 - n100;
    feet = n500 * 500 + n100 * 100 - 1300;
    return true;
}

/** The 13-bit AC field of DF0/4/16/20: C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4. */
inline bool altitude13(uint32_t ac, int& feet) {
    if (ac == 0) return false;
    if (ac & 0x40) return false;                         // M = 1: metric, not seen in practice
    if (ac & 0x10) {                                     // Q = 1: 25 ft steps
        const uint32_t n = ((ac & 0x1F80) >> 2) | ((ac & 0x20) >> 1) | (ac & 0x0F);
        feet = int(n) * 25 - 1000;
        return true;
    }
    auto b = [&](int i) { return (ac >> (12 - i)) & 1; };   // i = 0..12, left to right
    const uint32_t C1 = b(0), A1 = b(1), C2 = b(2), A2 = b(3), C4 = b(4), A4 = b(5), B1 = b(7), B2 = b(9),
                   D2 = b(10), B4 = b(11), D4 = b(12);
    const uint32_t g500 = D2 << 7 | D4 << 6 | A1 << 5 | A2 << 4 | A4 << 3 | B1 << 2 | B2 << 1 | B4;
    return gillhamToFeet(g500, C1 << 2 | C2 << 1 | C4, feet);
}

/** The 12-bit ALT field of an airborne position (the AC field without M). */
inline bool altitude12(uint32_t alt, int& feet) {
    const uint32_t ac = ((alt & 0xFC0) << 1) | (alt & 0x3F);  // re-insert M = 0
    return altitude13(ac, feet);
}

/** The 13-bit ID field (DF5/21, and TC 28): C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4 → the 4-digit squawk. */
inline int squawk13(uint32_t id) {
    auto b = [&](int i) { return int((id >> (12 - i)) & 1); };
    const int A = b(5) << 2 | b(3) << 1 | b(1);
    const int B = b(11) << 2 | b(9) << 1 | b(7);
    const int C = b(4) << 2 | b(2) << 1 | b(0);
    const int D = b(12) << 2 | b(10) << 1 | b(8);
    return A * 1000 + B * 100 + C * 10 + D;
}

// ── CPR (DO-260B A.1.7) ─────────────────────────────────────────────────────────────────────────────
/** NL(lat): the number of longitude zones at a latitude, from the transition-latitude table rather than the
 *  closed form, so an aircraft exactly on a boundary gets the same answer every time. */
inline int cprNL(double lat) {
    static const std::array<double, 58> trans = [] {
        std::array<double, 58> t{};
        const double nz = 15.0, pi = 3.14159265358979323846;
        for (int nl = 59; nl >= 2; --nl)
            t[59 - nl] = 180.0 / pi * std::acos(std::sqrt((1 - std::cos(pi / (2 * nz))) / (1 - std::cos(2 * pi / nl))));
        return t;
    }();
    lat = std::fabs(lat);
    for (int nl = 59; nl >= 2; --nl) if (lat < trans[59 - nl]) return nl;
    return 1;
}

inline double cprMod(double a, double b) { const double r = a - b * std::floor(a / b); return r < 0 ? r + b : r; }

/** Globally unambiguous airborne (span 360) or surface (90, needs `refLat` to pick the quadrant) position from
 *  an even/odd pair; `oddNewer` picks which frame's time the answer is for. */
inline bool cprGlobal(double latE, double lonE, double latO, double lonO, bool oddNewer, bool surface,
                      double refLat, double refLon, double& lat, double& lon) {
    const double span = surface ? 90.0 : 360.0;
    latE /= 131072.0; lonE /= 131072.0; latO /= 131072.0; lonO /= 131072.0;
    const double dE = span / 60.0, dO = span / 59.0;
    const double j = std::floor(59 * latE - 60 * latO + 0.5);
    double rE = dE * (cprMod(j, 60) + latE), rO = dO * (cprMod(j, 59) + latO);
    if (surface) {
        // ★ four candidates 90° apart — the one on the receiver's side of the world
        auto near = [&](double v) { double best = v; for (double c : {v - 90, v, v + 90 - 180, v - 180}) if (std::fabs(c - refLat) < std::fabs(best - refLat)) best = c; return best; };
        rE = near(rE); rO = near(rO);
    } else {
        if (rE >= 270) rE -= 360;
        if (rO >= 270) rO -= 360;
    }
    if (rE < -90 || rE > 90 || rO < -90 || rO > 90) return false;
    if (cprNL(rE) != cprNL(rO)) return false;           // the pair straddles a zone boundary: wait for the next
    lat = oddNewer ? rO : rE;
    const int nl = cprNL(lat);
    const int ni = std::max(nl - (oddNewer ? 1 : 0), 1);
    const double m = std::floor(lonE * (nl - 1) - lonO * nl + 0.5);
    lon = (span / ni) * (cprMod(m, ni) + (oddNewer ? lonO : lonE));
    if (surface) {
        double best = lon;
        for (double c : {lon, lon + 90, lon + 180, lon + 270, lon - 90, lon - 180, lon - 270})
            if (c >= -180 && c < 180 && std::fabs(cprMod(c - refLon + 180, 360) - 180) < std::fabs(cprMod(best - refLon + 180, 360) - 180)) best = c;
        lon = best;
    }
    if (lon >= 180) lon -= 360;
    return true;
}

/** Locally unambiguous position from one frame and a reference within half a zone (≈ 180 NM airborne,
 *  45 NM surface). */
inline void cprLocal(double latC, double lonC, bool odd, bool surface, double refLat, double refLon,
                     double& lat, double& lon) {
    const double span = surface ? 90.0 : 360.0;
    latC /= 131072.0; lonC /= 131072.0;
    const double dLat = span / (odd ? 59.0 : 60.0);
    const double j = std::floor(refLat / dLat) + std::floor(0.5 + cprMod(refLat, dLat) / dLat - latC);
    lat = dLat * (j + latC);
    const int ni = std::max(cprNL(lat) - (odd ? 1 : 0), 1);
    const double dLon = span / ni;
    const double m = std::floor(refLon / dLon) + std::floor(0.5 + cprMod(refLon, dLon) / dLon - lonC);
    lon = dLon * (m + lonC);
    if (lon >= 180) lon -= 360;
    if (lon < -180) lon += 360;
}

/** Great-circle distance in nautical miles. */
inline double distanceNm(double la1, double lo1, double la2, double lo2) {
    const double r = 3.14159265358979323846 / 180.0;
    const double a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 2 * 3440.065 * std::asin(std::min(1.0, std::sqrt(h)));
}

// ── The aircraft table ───────────────────────────────────────────────────────────────────────────────
struct Aircraft {
    uint32_t icao = 0;
    bool     nonIcao = false;          ///< DF18 CF=1: an anonymous/temporary address
    std::string callsign;
    int      category = 0;             ///< (TC << 4) | CA from identification, e.g. 0xA3 = A3 (large)
    bool     hasPos = false;  double lat = 0, lon = 0; bool onGround = false;
    bool     hasAlt = false;  int altBaro = 0;
    bool     hasGeo = false;  int altGeom = 0;
    bool     hasGs = false;   double gs = 0;  double track = 0;  bool hasTrack = false;
    bool     hasVr = false;   int vr = 0;
    bool     hasHdg = false;  double heading = 0;
    bool     hasAs = false;   int airspeed = 0; bool tas = false;
    int      squawk = -1;
    int      emergency = 0;            ///< TC 28 emergency state, 0 = none
    double   seen = 0, seenPos = 0;    ///< seconds (the tracker's clock) of the last message / last position
    uint32_t messages = 0;
    float    level = -99;              ///< dBFS, smoothed
    // CPR state
    bool     haveE = false, haveO = false, cprSurface = false;
    double   tE = 0, tO = 0; uint32_t latE = 0, lonE = 0, latO = 0, lonO = 0;
    double   pendLat = 0, pendLon = 0;   ///< a single-frame fix near the receiver, awaiting a second to agree
};

class Tracker {
public:
    /** The receiver's position, if the server knows it: needed for surface positions, and used to throw out
     *  a global decode that lands implausibly far away. */
    void setReceiver(double lat, double lon) { haveRx_ = true; rxLat_ = lat; rxLon_ = lon; }
    void setMaxRangeNm(double nm) { maxRangeNm_ = nm; }

    /** Judge and apply one frame. `now` in seconds. Returns true if it was believed. */
    bool accept(const Frame& f, double now) {
        const int df = f.b[0] >> 3;
        uint32_t s = syndrome(f.b, f.len);
        uint8_t m[14];
        std::memcpy(m, f.b, f.len);
        if (df == 17 || df == 18) {
            if (s != 0) {
                const auto& t = singleBitSyndromes();
                auto it = t.find(s);
                if (it == t.end()) { ++rejected_; return false; }
                m[it->second >> 3] ^= uint8_t(0x80 >> (it->second & 7));
                ++repaired_;
            }
            const uint32_t addr = uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
            if (df == 18) {
                const int cf = m[0] & 7;
                if (cf != 0 && cf != 1) { ++rejected_; return false; }   // TIS-B / ADS-R relays: not this aircraft's own
            }
            Aircraft& a = touch(addr, f, now);
            if (df == 18 && (m[0] & 7) == 1) a.nonIcao = true;
            extended(a, m + 4, now);
            ++good_;
            return true;
        }
        if (df == 11) {
            const uint32_t addr = uint32_t(m[1]) << 16 | uint32_t(m[2]) << 8 | m[3];
            if (s == 0 || ((s & ~0x7Fu) == 0 && known(addr, now))) { touch(addr, f, now); ++good_; return true; }
            ++rejected_; return false;
        }
        // address/parity replies: the syndrome IS the address
        if (!known(s, now)) { ++rejected_; return false; }
        Aircraft& a = touch(s, f, now);
        if (df == 0 || df == 4 || df == 16 || df == 20) {
            int ft; if (altitude13(bits(m, 20, 32), ft)) { a.hasAlt = true; a.altBaro = ft; }
            a.onGround = (df == 4 || df == 20) && (m[0] & 7) == 1 ? true : a.onGround;
        }
        if (df == 5 || df == 21) a.squawk = squawk13(bits(m, 20, 32));
        ++good_;
        return true;
    }

    /** Forget aircraft not heard for `age` seconds. */
    void expire(double now, double age = 60.0) {
        for (auto it = ac_.begin(); it != ac_.end();) it = (now - it->second.seen > age) ? ac_.erase(it) : std::next(it);
    }

    const std::unordered_map<uint32_t, Aircraft>& aircraft() const { return ac_; }
    uint64_t good() const { return good_; }
    uint64_t rejected() const { return rejected_; }
    uint64_t repaired() const { return repaired_; }

    /** The table as JSON, in the field names dump1090's aircraft.json made familiar (hex, flight, lat, lon,
     *  alt_baro, gs, track, baro_rate, squawk, seen, seen_pos, messages, rssi). Positions older than 60 s are
     *  dropped, not drawn where the aircraft used to be. */
    std::string json(double now) const {
        std::string o;
        o.reserve(96 + ac_.size() * 220);
        char t[64];
        std::snprintf(t, sizeof t, "{\"now\":%.1f,\"messages\":%llu,\"aircraft\":[", now, (unsigned long long)good_);
        o += t;
        bool first = true;
        for (const auto& kv : ac_) {
            const Aircraft& a = kv.second;
            if (!first) o += ',';
            first = false;
            char b[512];
            int n = std::snprintf(b, sizeof b, "{\"hex\":\"%s%06x\"", a.nonIcao ? "~" : "", a.icao);
#define VIBE_ADSB_ADD(...) (n += std::snprintf(b + n, sizeof b - size_t(n), __VA_ARGS__))
            if (!a.callsign.empty()) VIBE_ADSB_ADD(",\"flight\":\"%s\"", a.callsign.c_str());
            if (a.category) VIBE_ADSB_ADD(",\"category\":\"%c%d\"", "?DCBA"[std::min(a.category >> 4, 4)], a.category & 7);
            if (a.hasPos && now - a.seenPos <= 60) VIBE_ADSB_ADD(",\"lat\":%.5f,\"lon\":%.5f,\"seen_pos\":%.1f", a.lat, a.lon, now - a.seenPos);
            if (a.onGround) VIBE_ADSB_ADD(",\"alt_baro\":\"ground\"");
            else if (a.hasAlt) VIBE_ADSB_ADD(",\"alt_baro\":%d", a.altBaro);
            if (a.hasGeo) VIBE_ADSB_ADD(",\"alt_geom\":%d", a.altGeom);
            if (a.hasGs) VIBE_ADSB_ADD(",\"gs\":%.1f", a.gs);
            if (a.hasTrack) VIBE_ADSB_ADD(",\"track\":%.1f", a.track);
            if (a.hasHdg) VIBE_ADSB_ADD(",\"mag_heading\":%.1f", a.heading);
            if (a.hasAs) { if (a.tas) VIBE_ADSB_ADD(",\"tas\":%d", a.airspeed); else VIBE_ADSB_ADD(",\"ias\":%d", a.airspeed); }
            if (a.hasVr) VIBE_ADSB_ADD(",\"baro_rate\":%d", a.vr);
            if (a.squawk >= 0) VIBE_ADSB_ADD(",\"squawk\":\"%04d\"", a.squawk);
            if (a.emergency) VIBE_ADSB_ADD(",\"emergency\":%d", a.emergency);
            VIBE_ADSB_ADD(",\"seen\":%.1f,\"messages\":%u,\"rssi\":%.1f}", now - a.seen, a.messages, a.level);
            o.append(b, size_t(std::min(n, int(sizeof b) - 1)));
#undef VIBE_ADSB_ADD
        }
        o += "]}";
        return o;
    }

    /** An aircraft heard in the last minute — for Demod's address/parity check. */
    bool known(uint32_t addr, double now) const {
        auto it = ac_.find(addr);
        return it != ac_.end() && now - it->second.seen < 60;
    }

private:

    Aircraft& touch(uint32_t addr, const Frame& f, double now) {
        Aircraft& a = ac_[addr];
        if (a.messages == 0) { a.icao = addr; a.level = f.level; }
        a.seen = now;
        ++a.messages;
        a.level += (f.level - a.level) * 0.25f;
        return a;
    }

    void extended(Aircraft& a, const uint8_t* me, double now) {
        const int tc = me[0] >> 3;
        if (tc >= 1 && tc <= 4) {
            static const char* cs = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
            std::string s;
            for (int i = 0; i < 8; ++i) { const char c = cs[bits(me, 9 + 6 * i, 14 + 6 * i)]; if (c != '#') s += c; }
            while (!s.empty() && s.back() == ' ') s.pop_back();
            if (!s.empty()) a.callsign = s;
            a.category = (tc == 4 ? 0xA0 : tc == 3 ? 0xB0 : tc == 2 ? 0xC0 : 0xD0) | (me[0] & 7);
            return;
        }
        if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
            const uint32_t alt = bits(me, 9, 20);
            if (tc <= 18) { int ft; if (altitude12(alt, ft)) { a.hasAlt = true; a.altBaro = ft; } }
            else if (alt) { a.hasGeo = true; a.altGeom = int(std::lround(alt * 3.28084)); }
            a.onGround = false;
            position(a, me, now, false);
            return;
        }
        if (tc >= 5 && tc <= 8) {
            a.onGround = true;
            const int mov = int(bits(me, 6, 12));
            double kt;
            if (groundSpeed(mov, kt)) { a.hasGs = true; a.gs = kt; }
            if (bits(me, 13, 13)) { a.hasTrack = true; a.track = bits(me, 14, 20) * 360.0 / 128.0; }
            position(a, me, now, true);
            return;
        }
        if (tc == 19) {
            const int st = int(bits(me, 6, 8));
            if (st == 1 || st == 2) {
                const int vew = int(bits(me, 15, 24)), vns = int(bits(me, 26, 35));
                if (vew && vns) {
                    const int k = st == 2 ? 4 : 1;
                    const double vx = (bits(me, 14, 14) ? -1 : 1) * (vew - 1) * k;
                    const double vy = (bits(me, 25, 25) ? -1 : 1) * (vns - 1) * k;
                    a.hasGs = true; a.gs = std::hypot(vx, vy);
                    a.hasTrack = true; a.track = cprMod(std::atan2(vx, vy) * 180.0 / 3.14159265358979323846, 360);
                }
            } else if (st == 3 || st == 4) {
                if (bits(me, 14, 14)) { a.hasHdg = true; a.heading = bits(me, 15, 24) * 360.0 / 1024.0; }
                const int as = int(bits(me, 26, 35));
                if (as) { a.hasAs = true; a.airspeed = (as - 1) * (st == 4 ? 4 : 1); a.tas = bits(me, 25, 25); }
            }
            const int vr = int(bits(me, 38, 46));
            if (vr) { a.hasVr = true; a.vr = (bits(me, 37, 37) ? -1 : 1) * (vr - 1) * 64; }
            return;
        }
        if (tc == 28 && bits(me, 6, 8) == 1) {
            a.emergency = int(bits(me, 9, 11));
            const uint32_t id = bits(me, 12, 24);
            if (id) a.squawk = squawk13(id);
        }
    }

    /** DO-260B surface movement → knots (piecewise linear between the standard's break points). */
    static bool groundSpeed(int mov, double& kt) {
        if (mov == 0 || mov > 124) return false;
        if (mov == 1) { kt = 0; return true; }
        if (mov == 124) { kt = 175; return true; }
        static const int mv[] = {2, 9, 13, 39, 94, 109, 124};
        static const double ks[] = {0.125, 1, 2, 15, 70, 100, 175};
        for (int i = 1; i < 7; ++i)
            if (mov < mv[i]) { kt = ks[i - 1] + (mov - mv[i - 1]) * (ks[i] - ks[i - 1]) / (mv[i] - mv[i - 1]); return true; }
        return false;
    }

    void position(Aircraft& a, const uint8_t* me, double now, bool surface) {
        const bool odd = bits(me, 22, 22);
        const uint32_t la = bits(me, 23, 39), lo = bits(me, 40, 56);
        if (surface != a.cprSurface) { a.haveE = a.haveO = false; a.cprSurface = surface; }
        if (odd) { a.haveO = true; a.tO = now; a.latO = la; a.lonO = lo; }
        else     { a.haveE = true; a.tE = now; a.latE = la; a.lonE = lo; }

        double lat, lon;
        const double localLimit = surface ? 45 : 180;
        // 1. Local, from this aircraft's own recent fix: the normal case once it is tracked.
        if (a.hasPos && now - a.seenPos < (surface ? 50 : 30)) {
            cprLocal(la, lo, odd, surface, a.lat, a.lon, lat, lon);
            const double moved = distanceNm(a.lat, a.lon, lat, lon);
            const double allowed = 2 + (now - a.seenPos) * (surface ? 0.1 : 0.4);   // ~1400 kt airborne
            if (moved <= allowed) { setPos(a, lat, lon, now); return; }
            // a jump: fall through to a fresh global fix rather than drawing it
        }
        // 2. Global, from an even/odd pair close in time.
        if (a.haveE && a.haveO && std::fabs(a.tE - a.tO) <= (surface ? 25 : 10)) {
            if (surface && !haveRx_ && !(a.hasPos)) return;
            const double rLat = haveRx_ ? rxLat_ : a.lat, rLon = haveRx_ ? rxLon_ : a.lon;
            if (cprGlobal(a.latE, a.lonE, a.latO, a.lonO, odd, surface, rLat, rLon, lat, lon)) {
                if (haveRx_ && distanceNm(rxLat_, rxLon_, lat, lon) > maxRangeNm_) return;
                setPos(a, lat, lon, now);
                return;
            }
        }
        // 3. Local from the receiver, for a lone frame near it (only inside half a zone, so it cannot alias).
        if (haveRx_ && !a.hasPos) {
            cprLocal(la, lo, odd, surface, rxLat_, rxLon_, lat, lon);
            if (distanceNm(rxLat_, rxLon_, lat, lon) < std::min(localLimit, maxRangeNm_) * 0.5) {
                // ★ not trusted alone: a fix from a single frame waits for a second one to agree
                if (a.pendLat != 0 || a.pendLon != 0) {
                    if (distanceNm(a.pendLat, a.pendLon, lat, lon) < 5) { setPos(a, lat, lon, now); a.pendLat = a.pendLon = 0; return; }
                }
                a.pendLat = lat; a.pendLon = lon;
            }
        }
    }

    static void setPos(Aircraft& a, double lat, double lon, double now) {
        a.hasPos = true; a.lat = lat; a.lon = lon; a.seenPos = now;
    }

    std::unordered_map<uint32_t, Aircraft> ac_;
    bool haveRx_ = false; double rxLat_ = 0, rxLon_ = 0;
    double maxRangeNm_ = 400;
    uint64_t good_ = 0, rejected_ = 0, repaired_ = 0;
};

} // namespace vibeadsb
