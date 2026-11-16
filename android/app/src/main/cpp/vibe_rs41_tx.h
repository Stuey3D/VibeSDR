// vibe_rs41_tx.h — V12 RS41: OUR OWN TRANSMITTER, for the tests and the benchmark row (docs/v12/RS41.md).
// Built from the same listener documentation as the decoder (vibe_rs41.h): the frame's blocks with their CRCs, both
// Reed–Solomon codewords' parity, the whitening, the header, then GFSK at 4800 bit/s (bytes LSB first).
// ★ A decoder checked only against its own encoder proves self-consistency, not the air: RS41.md says which facts the
//   two share and which are unverified until a real sonde is heard.
#pragma once
#include "vibe_rs41.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace vibers41tx {

using cf32 = std::complex<float>;
using namespace vibers41;

/** What a simulated sonde reports. Position moves with its velocity; the calibration is built so that a complete set
 *  turns the PTU counts back into `tempC`. */
struct Sonde {
    std::string serial = "S1234567";
    double lat = 50.2, lon = -5.3, alt = 1200;     // start
    double vE = 4, vN = 3, vU = 5;                 // m/s
    double tempC = -12.5;
    double txHz = 403.3e6;
    int frame0 = 1000;
    bool extended = false;
    bool encrypted = false;                        // a 0x80 block in place of the GPS position
    double unix0 = 1763200000.0;                   // the first frame's time
    uint8_t cal[kCalLen] = {0};

    explicit Sonde(uint32_t seed = 1) {
        std::mt19937 r(seed);
        for (auto& b : cal) b = uint8_t(r());
        const uint16_t fq = uint16_t(std::lround((txHz - 400e6) * 64.0 / 10e3));
        cal[2] = uint8_t(fq); cal[3] = uint8_t(fq >> 8);
        putF(kCalRf1, 750.f); putF(kCalRf2, 1100.f);
        putF(kCalCo1, -245.0f); putF(kCalCo1 + 4, 0.2365f); putF(kCalCo1 + 8, 8.5e-6f);
        putF(kCalT1, 1.0f); putF(kCalT1 + 4, 0.0f); putF(kCalT1 + 8, 0.0f);
    }
    void setTx(double hz) { txHz = hz; const uint16_t fq = uint16_t(std::lround((hz - 400e6) * 64.0 / 10e3)); cal[2] = uint8_t(fq); cal[3] = uint8_t(fq >> 8); }
    void putF(int off, float v) { std::memcpy(cal + off, &v, 4); }
    float getF(int off) const { float v; std::memcpy(&v, cal + off, 4); return v; }
    /** The position k frames (seconds) after the first. */
    void at(int k, double& la, double& lo, double& h) const {
        la = lat + vN * k / 111320.0;
        lo = lon + vE * k / (111320.0 * std::cos(lat * M_PI / 180));
        h = alt + vU * k;
    }
};

inline void block(std::vector<uint8_t>& f, int& pos, uint8_t id, const std::vector<uint8_t>& d) {
    f[size_t(pos)] = id; f[size_t(pos) + 1] = uint8_t(d.size());
    std::memcpy(f.data() + pos + 2, d.data(), d.size());
    const uint16_t c = crc16(d.data(), d.size());
    f[size_t(pos) + 2 + d.size()] = uint8_t(c); f[size_t(pos) + 3 + d.size()] = uint8_t(c >> 8);
    pos += 4 + int(d.size());
}
inline void put16(std::vector<uint8_t>& d, size_t o, uint32_t v) { d[o] = uint8_t(v); d[o + 1] = uint8_t(v >> 8); }
inline void put24(std::vector<uint8_t>& d, size_t o, uint32_t v) { d[o] = uint8_t(v); d[o + 1] = uint8_t(v >> 8); d[o + 2] = uint8_t(v >> 16); }
inline void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) d[o + size_t(i)] = uint8_t(v >> (8 * i)); }

/** Frame k of a sonde, DE-WHITENED (header, parity, blocks): the bytes the decoder should get back. */
inline std::vector<uint8_t> frameClear(const Sonde& s, int k, uint32_t seed = 7) {
    const int len = s.extended ? kExtLen : kStdLen;
    std::vector<uint8_t> f(size_t(len), 0);
    for (int i = 0; i < 8; ++i) f[size_t(i)] = headerOnAir()[i] ^ mask()[i];
    f[kTypePos] = s.extended ? 0xF0 : 0x0F;
    int pos = kBlocksPos;
    std::mt19937 r(seed + uint32_t(k));
    // 0x79 status
    {
        std::vector<uint8_t> d(40, 0);
        const int fn = s.frame0 + k;
        put16(d, 0, uint32_t(fn));
        for (int i = 0; i < 8; ++i) d[size_t(2 + i)] = uint8_t(s.serial[size_t(i)]);
        d[10] = 29;                                    // 2.9 V
        const int sub = fn % kCalSubframes;
        d[23] = uint8_t(sub);
        std::memcpy(d.data() + 24, s.cal + 16 * sub, 16);
        block(f, pos, 0x79, d);
    }
    // 0x7A PTU: the main temperature sensor's counts against the two references (gain 200 counts/Ω, 50 Ω offset)
    {
        std::vector<uint8_t> d(42, 0);
        const double c0 = s.getF(kCalCo1), c1 = s.getF(kCalCo1 + 4), c2 = s.getF(kCalCo1 + 8);
        const double R = (-c1 + std::sqrt(c1 * c1 - 4 * c2 * (c0 - s.tempC))) / (2 * c2);
        const double g = 200, rb = 50, rf1 = s.getF(kCalRf1), rf2 = s.getF(kCalRf2);
        put24(d, 0, uint32_t(std::lround(g * (R / s.getF(kCalT1) + rb))));
        put24(d, 3, uint32_t(std::lround(g * (rf1 + rb))));
        put24(d, 6, uint32_t(std::lround(g * (rf2 + rb))));
        for (int i = 3; i < 12; ++i) put24(d, size_t(3 * i), 100000 + (r() % 50000));
        block(f, pos, 0x7A, d);
    }
    // 0x7C GPS time
    {
        std::vector<uint8_t> d(30, 0);
        const double g = s.unix0 + k + 18.0 - 315964800.0;
        const int week = int(g / 604800.0);
        put16(d, 0, uint32_t(week));
        put32(d, 2, uint32_t(std::lround((g - week * 604800.0) * 1000.0)));
        for (size_t i = 6; i < d.size(); ++i) d[i] = uint8_t(r());
        block(f, pos, 0x7C, d);
    }
    // 0x7D GPS raw (content not read)
    { std::vector<uint8_t> d(89); for (auto& b : d) b = uint8_t(r()); block(f, pos, 0x7D, d); }
    // 0x7B GPS position + velocity, or 0x80 on an encrypted sonde
    if (s.encrypted) {
        std::vector<uint8_t> d(21); for (auto& b : d) b = uint8_t(r()); block(f, pos, 0x80, d);
    } else {
        std::vector<uint8_t> d(21, 0);
        double la, lo, h, x, y, z;
        s.at(k, la, lo, h);
        llaToEcef(la, lo, h, x, y, z);
        put32(d, 0, uint32_t(int32_t(std::lround(x * 100)))); put32(d, 4, uint32_t(int32_t(std::lround(y * 100))));
        put32(d, 8, uint32_t(int32_t(std::lround(z * 100))));
        // ENU → ECEF velocity
        const double a = la * M_PI / 180, b = lo * M_PI / 180;
        const double vx = -std::sin(b) * s.vE - std::sin(a) * std::cos(b) * s.vN + std::cos(a) * std::cos(b) * s.vU;
        const double vy = std::cos(b) * s.vE - std::sin(a) * std::sin(b) * s.vN + std::cos(a) * std::sin(b) * s.vU;
        const double vz = std::cos(a) * s.vN + std::sin(a) * s.vU;
        put16(d, 12, uint32_t(uint16_t(int16_t(std::lround(vx * 100))))); put16(d, 14, uint32_t(uint16_t(int16_t(std::lround(vy * 100)))));
        put16(d, 16, uint32_t(uint16_t(int16_t(std::lround(vz * 100)))));
        d[18] = 9; d[19] = 20; d[20] = 15;
        block(f, pos, 0x7B, d);
    }
    if (s.extended) {   // an extra-data block (an attached instrument), then padding to the end
        std::vector<uint8_t> d(120); for (auto& b : d) b = uint8_t(r()); block(f, pos, 0x7E, d);
    }
    { std::vector<uint8_t> d(size_t(len - pos - 4), 0x02); block(f, pos, 0x76, d); }
    addParity(f.data(), len);
    return f;
}
/** Frame k as SENT (whitened). */
inline std::vector<uint8_t> frameOnAir(const Sonde& s, int k, uint32_t seed = 7) {
    std::vector<uint8_t> f = frameClear(s, k, seed);
    whiten(f.data(), int(f.size()));
    return f;
}

/** A GFSK transmitter at a capture rate: one RS41 frame a second (a preamble of alternating bits, then the frame, LSB
 *  first), BT 0.5, ±`devHz`. `render` ADDS its samples for absolute capture indices [base, base+n). Between frames the
 *  carrier is off (`continuous` false) or carries alternating bits. `clockPpm` skews its bit rate. */
class Gfsk {
public:
    Gfsk(double fs, double offHz, double amp, double devHz = 2400, double startS = 0.1, double clockPpm = 0, bool continuous = false)
        : fs_(fs), off_(offHz), amp_(amp), dev_(devHz), t0_(startS), baud_(kBaud * (1 + clockPpm * 1e-6)), cont_(continuous) {
        const double bt = 0.5, kap = M_PI * bt * std::sqrt(2.0 / std::log(2.0));
        for (int i = 0; i < kTab; ++i) {
            const double u = -2.0 + 4.0 * i / (kTab - 1);
            tab_[i] = float(0.5 * (std::erf(kap * (u + 0.5)) - std::erf(kap * (u - 0.5))));
        }
    }
    /** Add frame bytes (on air) to start at second `slot` after the start (one slot = one second). */
    void addFrame(int slot, const std::vector<uint8_t>& onAir) {
        const size_t first = size_t(std::lround(slot * baud_));
        const size_t need = first + 40 + onAir.size() * 8 + 8;
        if (bits_.size() < need) bits_.resize(need, int8_t(0));
        for (size_t i = 0; i < 40; ++i) bits_[first + i] = (i & 1) ? 1 : -1;
        for (size_t b = 0; b < onAir.size(); ++b)
            for (int i = 0; i < 8; ++i) bits_[first + 40 + b * 8 + size_t(i)] = ((onAir[b] >> i) & 1) ? 1 : -1;
    }
    void render(cf32* out, size_t n, size_t base) {
        for (size_t i = 0; i < n; ++i) {
            const double t = double(base + i) / fs_ - t0_;
            const double u = t * baud_;
            const long k0 = long(std::floor(u));
            double f = 0;
            const bool on = t >= 0 && bitAt(k0) != 0;
            for (long k = k0 - 2; k <= k0 + 2; ++k) {
                const int8_t b = bitAt(k);
                if (!b) continue;
                const double x = u - double(k) - 0.5;
                if (x <= -2 || x >= 2) continue;
                const double p = (x + 2) / 4 * (kTab - 1);
                const int j = int(p); const double fr = p - j;
                f += b * (tab_[j] + fr * (tab_[std::min(j + 1, kTab - 1)] - tab_[j]));
            }
            ph_ += 2 * M_PI * (off_ + dev_ * f) / fs_;
            if (ph_ > M_PI) ph_ -= 2 * M_PI; else if (ph_ < -M_PI) ph_ += 2 * M_PI;
            if (on) out[i] += cf32(float(amp_ * std::cos(ph_)), float(amp_ * std::sin(ph_)));
        }
    }
    double seconds() const { return t0_ + bits_.size() / baud_; }

private:
    int8_t bitAt(long k) const {
        if (k < 0 || size_t(k) >= bits_.size()) return 0;
        const int8_t b = bits_[size_t(k)];
        if (b || !cont_) return b;
        return (k & 1) ? 1 : -1;
    }
    static constexpr int kTab = 1025;
    double fs_, off_, amp_, dev_, t0_, baud_;
    bool cont_;
    float tab_[kTab];
    std::vector<int8_t> bits_;
    double ph_ = 0;
};

}  // namespace vibers41tx
