// decoders/sarsat_tx.h — V12 our own 406 MHz beacon transmitter (complex baseband) for test-sarsat, the server's end-to-end clip
// and the benchmark row: C/S T.001 §2.2–2.3 — 160 ms of carrier, then 400 bit/s biphase-L phase modulation ±1.1 rad with
// 150 µs rise and fall. docs/v12/SARSAT.md.
#pragma once
#include "decoders/sarsat_msg.h"

#include <cmath>
#include <complex>
#include <random>
#include <vector>

namespace vibe {
namespace sarsat {
namespace tx {

/** Bit and frame sync (normal or self-test) and the BCH parity, around data already set in bits 25 …. */
inline void finish(Bits& b, bool selfTest) {
    for (int i = 1; i <= 15; ++i) b[(size_t)i] = 1;
    setField(b, 16, 24, selfTest ? 0x0D0u : 0x02Fu);   // 011010000 / 000101111
    bch1().encode(b);
    if (b[25]) bch2().encode(b);
}
/** The Annex B example: a short serial-user float-free EPIRB message, bits 25–112 = 56E68 04002 20200 96552 50 (hex). */
inline Bits annexB() {
    Bits b{};
    const char* hex = "56E680400220200965525" "0";
    int bit = 25;
    for (const char* p = hex; *p && bit <= 112; ++p) {
        const int v = *p <= '9' ? *p - '0' : *p - 'A' + 10;
        for (int k = 3; k >= 0 && bit <= 112; --k) b[(size_t)bit++] = (uint8_t)((v >> k) & 1);
    }
    for (int i = 1; i <= 15; ++i) b[(size_t)i] = 1;
    setField(b, 16, 24, 0x02F);
    return b;
}
/** Standard location, EPIRB-MMSI (PC 0010), long, with a position encoded as A3.3.1 says (coarse ¼° + 4 s offset). */
inline Bits standardLocationEpirb(int country, uint32_t mmsi6, double lat, double lon, bool selfTest = false) {
    Bits b{};
    b[25] = 1; b[26] = 0; setField(b, 27, 36, (uint32_t)country); setField(b, 37, 40, 2);
    setField(b, 41, 60, mmsi6); setField(b, 61, 64, 0);
    const double alat = std::fabs(lat), alon = std::fabs(lon);
    const uint32_t qlat = (uint32_t)std::lround(alat * 4), qlon = (uint32_t)std::lround(alon * 4);
    b[65] = lat < 0; setField(b, 66, 74, qlat); b[75] = lon < 0; setField(b, 76, 85, qlon);
    setField(b, 107, 110, 0xD);  // 1101
    b[111] = 1; b[112] = 1;
    auto off = [&](double actual, double coarse, int sign, int minAt) {
        // rounded to 4 s, minus the coarse value; sign 1 = plus
        const double r = std::round(actual * 900.0) / 900.0;
        double d = r - coarse; b[(size_t)sign] = d >= 0 ? 1 : 0; d = std::fabs(d);
        int sec = (int)std::lround(d * 3600.0); int mn = sec / 60; sec -= mn * 60;
        setField(b, minAt, minAt + 4, (uint32_t)mn); setField(b, minAt + 5, minAt + 8, (uint32_t)(sec / 4));
    };
    off(alat, qlat / 4.0, 113, 114); off(alon, qlon / 4.0, 123, 124);
    finish(b, selfTest);
    return b;
}
/** User-location: serial user ELT … (PC 011, type 000), long, 4-minute position in PDF-2. */
inline Bits serialUserLocation(int country, uint32_t serial, double lat, double lon, int type = 0) {
    Bits b{};
    b[25] = 1; b[26] = 1; setField(b, 27, 36, (uint32_t)country); setField(b, 37, 39, 3);
    setField(b, 40, 42, (uint32_t)type); b[43] = 0; setField(b, 44, 63, serial); setField(b, 64, 83, 0); setField(b, 84, 85, 1);
    b[107] = 1;
    const double alat = std::fabs(lat), alon = std::fabs(lon);
    int dl = (int)alat, ml = (int)std::lround((alat - dl) * 60 / 4); if (ml == 15) { ml = 0; ++dl; }
    int dn = (int)alon, mn = (int)std::lround((alon - dn) * 60 / 4); if (mn == 15) { mn = 0; ++dn; }
    b[108] = lat < 0; setField(b, 109, 115, (uint32_t)dl); setField(b, 116, 119, (uint32_t)ml);
    b[120] = lon < 0; setField(b, 121, 128, (uint32_t)dn); setField(b, 129, 132, (uint32_t)mn);
    finish(b, false);
    return b;
}
/** Aviation user, short: a registration (7 modified-Baudot characters, right-justified). */
inline Bits aviationUser(int country, const std::string& reg) {
    Bits b{};
    b[25] = 0; b[26] = 1; setField(b, 27, 36, (uint32_t)country); setField(b, 37, 39, 1);
    std::string r = reg; while (r.size() < 7) r = " " + r;
    for (int k = 0; k < 7; ++k) setField(b, 40 + 6 * k, 45 + 6 * k, baudotCode(r[(size_t)k]));
    setField(b, 82, 83, 0); setField(b, 84, 85, 1);
    setField(b, 107, 112, 0x10);   // automatic + manual activation
    finish(b, false);
    return b;
}
/** Test user protocol (111), short. */
inline Bits testUser(int country) {
    Bits b{};
    b[25] = 0; b[26] = 1; setField(b, 27, 36, (uint32_t)country); setField(b, 37, 39, 7);
    setField(b, 40, 63, 0xABCDE); finish(b, false);
    return b;
}
/** PLB, national location (PC 1011), long. */
inline Bits nationalLocationPlb(int country, uint32_t serial, double lat, double lon) {
    Bits b{};
    b[25] = 1; b[26] = 0; setField(b, 27, 36, (uint32_t)country); setField(b, 37, 40, 11); setField(b, 41, 58, serial);
    const double alat = std::fabs(lat), alon = std::fabs(lon);
    int dl = (int)alat, ml = (int)std::lround((alat - dl) * 30) * 2; if (ml >= 60) { ml = 0; ++dl; }
    int dn = (int)alon, mn = (int)std::lround((alon - dn) * 30) * 2; if (mn >= 60) { mn = 0; ++dn; }
    b[59] = lat < 0; setField(b, 60, 66, (uint32_t)dl); setField(b, 67, 71, (uint32_t)(ml / 2));
    b[72] = lon < 0; setField(b, 73, 80, (uint32_t)dn); setField(b, 81, 85, (uint32_t)(mn / 2));
    setField(b, 107, 109, 6); b[110] = 1; b[111] = 1; b[112] = 0;
    auto off = [&](double actual, double coarse, int sign, int minAt) {
        const double r = std::round(actual * 900.0) / 900.0;
        double d = r - coarse; b[(size_t)sign] = d >= 0 ? 1 : 0; d = std::fabs(d);
        int sec = (int)std::lround(d * 3600.0); int m = sec / 60; sec -= m * 60;
        setField(b, minAt, minAt + 1, (uint32_t)m); setField(b, minAt + 2, minAt + 5, (uint32_t)(sec / 4));
    };
    off(alat, dl + ml / 60.0, 113, 114); off(alon, dn + mn / 60.0, 120, 121);
    setField(b, 127, 132, 0);
    finish(b, false);
    return b;
}

struct Opt {
    double rate = 48000;
    double offsetHz = 0;       ///< carrier off the dial
    double cn0 = 50;           ///< carrier to noise density, dB-Hz
    double bitRate = 400;      ///< 400 ± 1 %
    double index = 1.1;        ///< ±1.1 ± 0.1 rad
    double lead = 0.3, tail = 0.3;   ///< silence before and after (s)
    uint32_t seed = 1;
    bool invert = false;       ///< the receiver's sense swapped (a spectrum-inverted radio)
};
/** One burst (or silence if `b` is null) at complex baseband, noise included. */
inline std::vector<std::complex<float>> burst(const Bits* b, const Opt& o) {
    const int nbits = b ? ((*b)[25] ? 144 : 112) : 0;
    const double dur = o.lead + (b ? 0.160 + nbits / o.bitRate : 0.0) + o.tail;
    const size_t N = (size_t)(dur * o.rate);
    std::vector<std::complex<float>> x(N);
    const double t0 = o.lead, tm = t0 + 0.160, tEnd = b ? tm + nbits / o.bitRate : t0;
    const double rise = 150e-6;
    auto chip = [&](double t) -> double {   // ±1 with raised-cosine edges at every chip boundary
        const double u = (t - tm) * o.bitRate * 2;   // chip index
        auto val = [&](long c) -> double {
            if (c < 0 || c >= 2L * nbits) return 0.0;
            const int bit = (*b)[(size_t)(c / 2 + 1)];
            const double first = bit ? 1.0 : -1.0;       // biphase-L: 1 = + then −, 0 = − then +
            return (c % 2 == 0) ? first : -first;
        };
        const long c = (long)std::floor(u);
        const double frac = (u - c) / (o.bitRate * 2);   // seconds into the chip
        const double half = 1.0 / (o.bitRate * 2);
        const double v = val(c);
        if (frac < rise / 2) { const double p = val(c - 1); const double w = 0.5 - 0.5 * std::cos(M_PI * (frac + rise / 2) / rise); return p + (v - p) * w; }
        if (frac > half - rise / 2) { const double nx = val(c + 1); const double w = 0.5 - 0.5 * std::cos(M_PI * (frac - (half - rise / 2)) / rise); return v + (nx - v) * w; }
        return v;
    };
    for (size_t i = 0; i < N; ++i) {
        const double t = i / o.rate;
        if (!b || t < t0 || t >= tEnd) { x[i] = 0; continue; }
        double ph = 2 * M_PI * o.offsetHz * t;
        if (t >= tm) ph += (o.invert ? -1 : 1) * o.index * chip(t);
        x[i] = std::complex<float>((float)std::cos(ph), (float)std::sin(ph));
    }
    // noise: carrier power 1, N0 = 10^(−cn0/10) per Hz → per-sample variance N0·rate
    std::mt19937 r(o.seed * 2654435761u + 7); std::normal_distribution<float> n(0.f, 1.f);
    const float s = (float)std::sqrt(std::pow(10.0, -o.cn0 / 10) * o.rate / 2.0);
    for (auto& v : x) v += std::complex<float>(s * n(r), s * n(r));
    return x;
}

} // namespace tx
} // namespace sarsat
} // namespace vibe
