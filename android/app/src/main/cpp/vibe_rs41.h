// vibe_rs41.h — V12 RS41 radiosonde: THE FRAME (whitening, Reed–Solomon, blocks, GPS, calibration, temperature).
// docs/v12/RS41.md. Our own code, built from LISTENER DOCUMENTATION of the RS41 frame (Stuart, 2026-11-15: Vaisala
// publishes no standard; community write-ups are the standard here, and real reports from users will confirm it).
// GPL decoders were not copied. ★ NOTHING HERE HAS BEEN DECODED OFF AIR YET — RS41.md lists which facts are unverified.
//
// The frame as the listener documentation describes it (byte offsets in the de-whitened frame):
//   0…7     the header, 8 bytes (on air: 10 B6 CA 11 22 96 12 F8, i.e. the bytes below XOR the mask)
//   8…31    Reed–Solomon parity of codeword 1 (24 bytes)          ┐ two RS(255,231) codewords over GF(2⁸)/0x11D,
//   32…55   Reed–Solomon parity of codeword 2 (24 bytes)          │ roots α⁰…α²³, shortened; codeword 1 takes the
//   56      the frame type: 0x0F standard (320 bytes), 0xF0 extended (518 bytes)   ┘ EVEN bytes from 56, codeword 2 the odd
//   57…     blocks: id, length, data[length], CRC-16 (CCITT, preset 0xFFFF, over the data only, little-endian)
//           0x79 status (frame number, serial, battery, one 16-byte calibration subframe of 51)
//           0x7A PTU measurements (24-bit counts)   0x7C GPS time   0x7D GPS raw   0x7B GPS position + velocity (ECEF)
//           0x76 padding   0x7E extra data   0x80 encrypted (the military RS41-SGM: nothing to show)
// The whole frame (header included) is XORed with a 64-byte repeating mask before it is sent; bytes go LSB first.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vibers41 {

constexpr double kBaud = 4800.0;
constexpr int kStdLen = 320, kExtLen = 518, kMaxLen = 518;
constexpr int kParPos = 8, kMsgPos = 56, kTypePos = 56, kBlocksPos = 57;
constexpr int kCalSubframes = 51, kCalLen = kCalSubframes * 16;

/** The whitening mask (64 bytes, repeating from frame byte 0). From the listener documentation; unverified on air. */
inline const uint8_t* mask() {
    static const uint8_t m[64] = {
        0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98, 0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
        0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1, 0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
        0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C, 0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
        0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23, 0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1,
    };
    return m;
}
/** The header as it is SENT (after whitening). */
inline const uint8_t* headerOnAir() { static const uint8_t h[8] = { 0x10, 0xB6, 0xCA, 0x11, 0x22, 0x96, 0x12, 0xF8 }; return h; }
/** The header's 64 bits in send order (bytes in order, each LSB first), the first bit sent in bit 63. */
inline uint64_t headerBits() {
    uint64_t p = 0;
    for (int i = 0; i < 8; ++i) for (int b = 0; b < 8; ++b) p = (p << 1) | ((headerOnAir()[i] >> b) & 1u);
    return p;
}

/** CRC-16, polynomial 0x1021, preset 0xFFFF, MSB first, no final XOR (the block check). */
inline uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= uint16_t(d[i]) << 8;
        for (int b = 0; b < 8; ++b) c = (c & 0x8000) ? uint16_t((c << 1) ^ 0x1021) : uint16_t(c << 1);
    }
    return c;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  Reed–Solomon (255,231) over GF(2⁸) / x⁸+x⁴+x³+x²+1 (0x11D), generator roots α⁰ … α²³. Codeword byte i is the
//  coefficient of xⁱ: parity in 0…23, data from 24 (a shortened codeword's unused top is zero).
//  Errors and erasures (Berlekamp–Massey, Chien, Forney) — the same algorithm as our VDL2 RS (vibe_vdl2_fec.h), here
//  for 24 checks and the ascending byte order.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
class Rs {
public:
    static constexpr int N = 255, R = 24, K = N - R, FCR = 0;
    static const Rs& get() { static Rs r; return r; }

    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp_[log_[a] + log_[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp_[(log_[a] + 255 - log_[b]) % 255] : 0; }
    uint8_t pw(int e) const { return exp_[((e % 255) + 255) % 255]; }

    /** Parity for cw[24…254] into cw[0…23]: the remainder of x²⁴·M(x) by g(x). */
    void encode(uint8_t* cw) const {
        uint8_t rem[R] = {0};
        for (int i = N - 1; i >= R; --i) {
            const uint8_t fb = cw[i] ^ rem[R - 1];
            for (int k = R - 1; k >= 1; --k) rem[k] = rem[k - 1] ^ mul(fb, gen_[k]);
            rem[0] = mul(fb, gen_[0]);
        }
        std::memcpy(cw, rem, R);
    }
    void syndromes(const uint8_t* cw, uint8_t* S) const {
        for (int j = 0; j < R; ++j) {
            const uint8_t a = pw(FCR + j);
            uint8_t s = 0;
            for (int i = N - 1; i >= 0; --i) s = mul(s, a) ^ cw[i];
            S[j] = s;
        }
    }
    /** Decode in place. `eras` = erased byte positions (degrees). `used` = how many bytes from 0 are real (parity +
     *  data); an error located beyond them (a shortened codeword's zero padding) means failure. Returns bytes changed or −1. */
    int decode(uint8_t* cw, const int* eras, int ne, int used) const {
        uint8_t S[R];
        syndromes(cw, S);
        bool any = false;
        for (int j = 0; j < R; ++j) any |= S[j] != 0;
        if (!any) return 0;
        if (ne > R) return -1;
        uint8_t L[R + 1] = {1}, B[R + 1], T[R + 1];
        for (int k = 0; k < ne; ++k) {
            const uint8_t X = pw(eras[k]);
            for (int d = std::min(k + 1, R); d >= 1; --d) L[d] ^= mul(X, L[d - 1]);
        }
        std::memcpy(B, L, sizeof B);
        int deg = ne;
        for (int r = ne; r < R; ++r) {
            uint8_t d = 0;
            for (int i = 0; i <= deg && i <= r; ++i) d ^= mul(L[i], S[r - i]);
            for (int i = R; i >= 1; --i) B[i] = B[i - 1];
            B[0] = 0;
            if (!d) continue;
            for (int i = 0; i <= R; ++i) T[i] = L[i] ^ mul(d, B[i]);
            if (2 * deg <= r + ne) {
                deg = r + 1 + ne - deg;
                for (int i = 0; i <= R; ++i) B[i] = div(L[i], d);
            }
            std::memcpy(L, T, sizeof L);
        }
        int dl = R; while (dl > 0 && !L[dl]) --dl;
        if (dl == 0 || 2 * (dl - ne) + ne > R) return -1;
        uint8_t O[R] = {0};
        for (int i = 0; i < R; ++i) for (int j = 0; j <= i && j <= dl; ++j) O[i] ^= mul(L[j], S[i - j]);
        int found = 0, pos[R]; uint8_t val[R];
        for (int i = 0; i < N; ++i) {
            const uint8_t xi = pw(-i);                     // X⁻¹, X = αⁱ
            uint8_t v = 0, p = 1;
            for (int k = 0; k <= dl; ++k) { v ^= mul(L[k], p); p = mul(p, xi); }
            if (v) continue;
            if (found == dl || i >= used) return -1;
            uint8_t num = 0; p = 1;
            for (int k = 0; k < R; ++k) { num ^= mul(O[k], p); p = mul(p, xi); }
            uint8_t den = 0; p = 1;
            for (int k = 1; k <= dl; k += 2) { den ^= mul(L[k], p); p = mul(p, mul(xi, xi)); }
            if (!den) return -1;
            val[found] = mul(pw(i * (1 - FCR)), div(num, den));   // X^(1−FCR) · Ω(X⁻¹) / Λ'(X⁻¹)
            pos[found++] = i;
        }
        if (found != dl) return -1;
        for (int k = 0; k < found; ++k) cw[pos[k]] ^= val[k];
        syndromes(cw, S);
        for (int j = 0; j < R; ++j) if (S[j]) return -1;
        int changed = 0;
        for (int k = 0; k < found; ++k) changed += val[k] != 0;
        return changed;
    }

private:
    Rs() {
        int x = 1;
        for (int i = 0; i < 255; ++i) { exp_[i] = uint8_t(x); log_[x] = uint8_t(i); x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; ++i) exp_[i] = exp_[i - 255];
        log_[0] = 0;
        uint8_t g[R + 1] = {1};
        for (int j = 0; j < R; ++j) {
            const uint8_t r = pw(FCR + j);
            for (int k = j + 1; k >= 1; --k) g[k] = g[k - 1] ^ mul(g[k], r);
            g[0] = mul(g[0], r);
        }
        std::memcpy(gen_, g, sizeof gen_);
    }
    uint8_t exp_[512]{}, log_[256]{}, gen_[R + 1]{};
};

/** The frame's length from its type byte (≤ 2 bits wrong accepted), or 0 = neither. */
inline int lengthForType(uint8_t t) {
    const int a = __builtin_popcount(unsigned(t ^ 0x0F)), b = __builtin_popcount(unsigned(t ^ 0xF0));
    if (a <= 2 && a < b) return kStdLen;
    if (b <= 2 && b < a) return kExtLen;
    return 0;
}

/** The two codewords of a de-whitened frame of `len` bytes: assemble, correct, write back. `soft[i]` (optional, one per
 *  frame byte: the weakest bit's confidence) drives the erasure retries. Returns the bytes corrected, or −1. */
inline int correct(uint8_t* f, int len, const float* soft = nullptr) {
    const Rs& rs = Rs::get();
    const int half = (len - kMsgPos) / 2;
    int total = 0;
    for (int c = 0; c < 2; ++c) {
        uint8_t cw[255] = {0};
        for (int i = 0; i < Rs::R; ++i) cw[i] = f[kParPos + c * Rs::R + i];
        for (int j = 0; j < half; ++j) cw[Rs::R + j] = f[kMsgPos + 2 * j + c];
        auto frameIdx = [&](int deg) { return deg < Rs::R ? kParPos + c * Rs::R + deg : kMsgPos + 2 * (deg - Rs::R) + c; };
        uint8_t w[255];
        std::memcpy(w, cw, sizeof w);
        int r = rs.decode(w, nullptr, 0, Rs::R + half);
        if (r < 0 && soft) {
            // ★ errors-only failed: erase the weakest bytes (2e + E ≤ 24, so at least four checks always remain)
            std::vector<std::pair<float, int>> s;
            for (int d = 0; d < Rs::R + half; ++d) s.push_back({ soft[frameIdx(d)], d });
            std::sort(s.begin(), s.end());
            for (int ne : { 4, 8, 12, 16, 20 }) {
                int er[24];
                for (int k = 0; k < ne; ++k) er[k] = s[size_t(k)].second;
                std::memcpy(w, cw, sizeof w);
                r = rs.decode(w, er, ne, Rs::R + half);
                if (r >= 0) break;
            }
        }
        if (r < 0) return -1;
        total += r;
        for (int d = 0; d < Rs::R + half; ++d) f[frameIdx(d)] = w[d];
    }
    return total;
}

/** Writes both codewords' parity into a de-whitened frame (the encoder's half). */
inline void addParity(uint8_t* f, int len) {
    const Rs& rs = Rs::get();
    const int half = (len - kMsgPos) / 2;
    for (int c = 0; c < 2; ++c) {
        uint8_t cw[255] = {0};
        for (int j = 0; j < half; ++j) cw[Rs::R + j] = f[kMsgPos + 2 * j + c];
        rs.encode(cw);
        for (int i = 0; i < Rs::R; ++i) f[kParPos + c * Rs::R + i] = cw[i];
    }
}

inline void whiten(uint8_t* f, int len) { for (int i = 0; i < len; ++i) f[i] ^= mask()[i & 63]; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  Geodesy: ECEF (WGS-84) → latitude, longitude, height; ECEF velocity → east, north, up.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline void ecefToLla(double x, double y, double z, double& latDeg, double& lonDeg, double& h) {
    const double a = 6378137.0, f = 1 / 298.257223563, b = a * (1 - f), e2 = f * (2 - f), ep2 = (a * a - b * b) / (b * b);
    const double p = std::hypot(x, y);
    const double th = std::atan2(z * a, p * b);
    const double st = std::sin(th), ct = std::cos(th);
    const double lat = std::atan2(z + ep2 * b * st * st * st, p - e2 * a * ct * ct * ct);
    const double lon = std::atan2(y, x);
    const double sl = std::sin(lat);
    const double N = a / std::sqrt(1 - e2 * sl * sl);
    h = p / std::cos(lat) - N;
    latDeg = lat * 180.0 / M_PI; lonDeg = lon * 180.0 / M_PI;
}
inline void llaToEcef(double latDeg, double lonDeg, double h, double& x, double& y, double& z) {
    const double a = 6378137.0, f = 1 / 298.257223563, e2 = f * (2 - f);
    const double lat = latDeg * M_PI / 180, lon = lonDeg * M_PI / 180;
    const double N = a / std::sqrt(1 - e2 * std::sin(lat) * std::sin(lat));
    x = (N + h) * std::cos(lat) * std::cos(lon); y = (N + h) * std::cos(lat) * std::sin(lon); z = (N * (1 - e2) + h) * std::sin(lat);
}
inline void ecefVelToEnu(double latDeg, double lonDeg, double vx, double vy, double vz, double& ve, double& vn, double& vu) {
    const double la = latDeg * M_PI / 180, lo = lonDeg * M_PI / 180;
    const double sla = std::sin(la), cla = std::cos(la), slo = std::sin(lo), clo = std::cos(lo);
    ve = -slo * vx + clo * vy;
    vn = -sla * clo * vx - sla * slo * vy + cla * vz;
    vu = cla * clo * vx + cla * slo * vy + sla * vz;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  One decoded frame (only the blocks whose CRC passed are read).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Frame {
    int len = 0;                    ///< 320 or 518
    int rsFixed = 0;                ///< bytes Reed–Solomon corrected
    bool statusOk = false;          ///< the 0x79 block's CRC passed (frame number, serial, calibration)
    int frameNo = -1;
    std::string serial;             ///< e.g. "S1234567"
    double batteryV = NAN;
    int calIndex = -1;              ///< 0…50
    uint8_t cal[16] = {0};
    bool ptuOk = false;
    uint32_t meas[12] = {0};        ///< the PTU block's 24-bit counts (0…2: temperature sensor and its two references)
    bool timeOk = false;
    int gpsWeek = 0; uint32_t towMs = 0;
    bool posOk = false;
    double lat = NAN, lon = NAN, alt = NAN;      ///< WGS-84 degrees, metres (ellipsoid height)
    double vE = NAN, vN = NAN, vU = NAN;         ///< m/s
    int sats = 0; double pdop = NAN;
    bool encrypted = false;         ///< a 0x80 block (RS41-SGM): the position is not ours to read
    int blocksOk = 0, blocksBad = 0;
    // ★ V12 other sonde types (docs/v12/SONDES-TYPES.md): which decoder made this frame. RS41 frames leave these as
    //   they are; a DFM / M10 / M20 frame carries its own serial (already proven by that type's checks), its temperature
    //   (only where its calibration is complete — never a guess) and how far that calibration has got.
    const char* kind = "RS41";      ///< "RS41", "DFM", "M10", "M20" — the row's key with the serial
    std::string model;              ///< the finer type where the frame says it ("DFM-09", "DFM-17", "M10+") — else empty
    double tempC = NAN;             ///< non-RS41: the decoder's temperature (NAN = none / not yet)
    int calN = -1, calOf = -1;      ///< non-RS41: calibration pieces received / needed (calOf 0 = no temperature for this type)
    double unixTime = 0;            ///< non-RS41: the frame's UTC time from its GPS, 0 = none
    // the receiver's own measures, filled by the demodulator
    double hz = 0;                  ///< where the channel sat (absolute)
    double offsetHz = 0;            ///< the frame's measured frequency error from that
    double snrDb = 0;
};

inline bool serialPlausible(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return false;
    return s[0] >= 'A' && s[0] <= 'Z';
}

inline uint32_t u24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline int16_t  i16(const uint8_t* p) { return int16_t(u16(p)); }
inline uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline int32_t  i32(const uint8_t* p) { return int32_t(u32(p)); }

/** Read a corrected, de-whitened frame's blocks into `fr` (fr.len set). */
inline void parse(const uint8_t* f, Frame& fr) {
    int pos = kBlocksPos;
    while (pos + 4 <= fr.len) {
        const uint8_t id = f[pos], n = f[pos + 1];
        if (pos + 4 + n > fr.len) break;
        const uint8_t* d = f + pos + 2;
        const bool ok = crc16(d, n) == u16(d + n);
        if (!ok) { ++fr.blocksBad; pos += 4 + n; continue; }
        ++fr.blocksOk;
        if (id == 0x79 && n >= 40) {
            fr.statusOk = true;
            fr.frameNo = u16(d);
            fr.serial.assign(reinterpret_cast<const char*>(d + 2), 8);
            fr.batteryV = d[10] / 10.0;
            fr.calIndex = d[23] < kCalSubframes ? d[23] : -1;
            std::memcpy(fr.cal, d + 24, 16);
        } else if (id == 0x7A && n >= 42) {
            fr.ptuOk = true;
            for (int k = 0; k < 12; ++k) fr.meas[k] = u24(d + 3 * k);
        } else if (id == 0x7C && n >= 6) {
            fr.timeOk = true;
            fr.gpsWeek = u16(d); fr.towMs = u32(d + 2);
        } else if (id == 0x7B && n >= 21) {
            const double x = i32(d) / 100.0, y = i32(d + 4) / 100.0, z = i32(d + 8) / 100.0;
            const double r = std::sqrt(x * x + y * y + z * z);
            if (r > 6300e3 && r < 6460e3) {          // ★ within the Earth's radii, up to ~80 km — else not a fix
                ecefToLla(x, y, z, fr.lat, fr.lon, fr.alt);
                ecefVelToEnu(fr.lat, fr.lon, i16(d + 12) / 100.0, i16(d + 14) / 100.0, i16(d + 16) / 100.0, fr.vE, fr.vN, fr.vU);
                fr.sats = d[18];
                fr.pdop = d[20] / 10.0;
                fr.posOk = fr.alt > -1000 && fr.alt < 60000;
            }
        } else if (id == 0x80) {
            fr.encrypted = true;
        }
        pos += 4 + n;
    }
    if (fr.statusOk && !serialPlausible(fr.serial)) { fr.statusOk = false; fr.serial.clear(); }
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  Calibration: 51 subframes of 16 bytes, one per frame. Only a COMPLETE set is used — never a guessed PTU.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Calibration {
    uint8_t bytes[kCalLen] = {0};
    uint64_t have = 0;               ///< bit i = subframe i received (CRC-good status block)
    void add(int idx, const uint8_t* b) { if (idx < 0 || idx >= kCalSubframes) return; std::memcpy(bytes + 16 * idx, b, 16); have |= 1ull << idx; }
    int count() const { return __builtin_popcountll(have); }
    bool complete() const { return count() == kCalSubframes; }
    float f32(int off) const { float v; std::memcpy(&v, bytes + off, 4); return v; }
    /** The transmit frequency the sonde says it is on (subframe 0, bytes 2–3), Hz; 0 = not yet. Listener docs: 400 MHz +
     *  value × 10 kHz / 64. */
    double txHz() const { if (!(have & 1)) return 0; return 400e6 + double(u16(bytes + 2)) * 10e3 / 64.0; }
};

// Calibration offsets used for temperature (listener documentation; unverified on air): the two reference resistors
// (floats at 0x3D, 0x41), the sensor polynomial co1[3] (0x4D) and its correction calT1[3] (0x59).
constexpr int kCalRf1 = 0x3D, kCalRf2 = 0x41, kCalCo1 = 0x4D, kCalT1 = 0x59;

/** The air temperature, °C, from the main sensor's three counts and a COMPLETE calibration; NAN when the calibration
 *  is incomplete or the result is not a plausible air temperature (−100…+60 °C). */
inline double temperature(const Calibration& c, const uint32_t* meas) {
    if (!c.complete()) return NAN;
    const double f = meas[0], f1 = meas[1], f2 = meas[2];
    const double rf1 = c.f32(kCalRf1), rf2 = c.f32(kCalRf2);
    if (!(f2 > f1) || !(rf2 > rf1) || !std::isfinite(rf1) || !std::isfinite(rf2)) return NAN;
    const double g = (f2 - f1) / (rf2 - rf1);
    const double rb = (f1 * rf2 - f2 * rf1) / (f2 - f1);
    const double rc = f / g - rb;
    const double r = rc * c.f32(kCalT1);
    const double t = (c.f32(kCalCo1) + c.f32(kCalCo1 + 4) * r + c.f32(kCalCo1 + 8) * r * r + c.f32(kCalT1 + 4)) * (1.0 + c.f32(kCalT1 + 8));
    if (!std::isfinite(t) || t < -100 || t > 60) return NAN;
    return t;
}

/** GPS week + time of week (ms) → UNIX seconds (UTC: 18 leap seconds since 2017). */
inline double gpsToUnix(int week, uint32_t towMs) { return 315964800.0 + double(week) * 604800.0 + towMs / 1000.0 - 18.0; }

}  // namespace vibers41
