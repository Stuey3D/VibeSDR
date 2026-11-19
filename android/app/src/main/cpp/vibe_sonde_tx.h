// vibe_sonde_tx.h — V12 radiosondes: OUR OWN DFM / M10 / M20 TRANSMITTERS, for the tests and the benchmark rows
// (docs/v12/SONDES-TYPES.md). Built from the same listener documentation as the decoders (vibe_sonde_dfm.h,
// vibe_sonde_mxx.h) — ★ a decoder checked against its own encoder proves self-consistency, not the air.
#pragma once
#include "vibe_sonde_dfm.h"
#include "vibe_sonde_mxx.h"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace vibesondetx {

using cf32 = std::complex<float>;

/** Chips (±1) at a chip rate, Gaussian-shaped (BT 0.5) FSK at ±devHz around `offHz`. `render` ADDS samples for absolute
 *  capture indices [base, base+n). Where no chip is set the carrier is off. `clockPpm` skews the chip rate. */
class ChipFsk {
public:
    ChipFsk(double fs, double offHz, double amp, double devHz, double chipRate, double startS = 0.1, double clockPpm = 0)
        : fs_(fs), off_(offHz), amp_(amp), dev_(devHz), t0_(startS), rate_(chipRate * (1 + clockPpm * 1e-6)) {
        const double bt = 0.5, kap = M_PI * bt * std::sqrt(2.0 / std::log(2.0));
        for (int i = 0; i < kTab; ++i) {
            const double u = -2.0 + 4.0 * i / (kTab - 1);
            tab_[i] = float(0.5 * (std::erf(kap * (u + 0.5)) - std::erf(kap * (u - 0.5))));
        }
    }
    double chipRate() const { return rate_; }
    /** Chips from chip index `first` (chip 0 at the start time). */
    void put(size_t first, const std::vector<int8_t>& c) {
        if (chips_.size() < first + c.size()) chips_.resize(first + c.size(), int8_t(0));
        std::memcpy(chips_.data() + first, c.data(), c.size());
    }
    size_t indexAt(double sec) const { return size_t(std::lround(sec * rate_)); }
    size_t size() const { return chips_.size(); }
    void render(cf32* out, size_t n, size_t base) {
        for (size_t i = 0; i < n; ++i) {
            const double t = double(base + i) / fs_ - t0_;
            const double u = t * rate_;
            const long k0 = long(std::floor(u));
            double f = 0;
            const bool on = t >= 0 && at_(k0) != 0;
            for (long k = k0 - 2; k <= k0 + 2; ++k) {
                const int8_t b = at_(k);
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

private:
    int8_t at_(long k) const { return (k < 0 || size_t(k) >= chips_.size()) ? 0 : chips_[size_t(k)]; }
    static constexpr int kTab = 1025;
    double fs_, off_, amp_, dev_, t0_, rate_;
    float tab_[kTab];
    std::vector<int8_t> chips_;
    double ph_ = 0;
};

/** Bits → Manchester chips ("01" = 1, "10" = 0), appended. */
inline void manchester(const uint8_t* bits, size_t n, std::vector<int8_t>& out) {
    for (size_t i = 0; i < n; ++i) { out.push_back(bits[i] ? -1 : 1); out.push_back(bits[i] ? 1 : -1); }
}
inline void rawChips(const char* s, std::vector<int8_t>& out) { for (; *s; ++s) out.push_back(*s == '1' ? 1 : -1); }

/** A simulated sonde's flight: position at time k seconds after the start. */
struct Flight {
    double lat = 52.2, lon = 0.1, alt = 900, vE = 5, vN = -3, vU = 5.5;
    double unix0 = 1763200000.0;
    void at(double k, double& la, double& lo, double& h) const {
        la = lat + vN * k / 111320.0;
        lo = lon + vE * k / (111320.0 * std::cos(lat * M_PI / 180));
        h = alt + vU * k;
    }
};

// ── DFM ─────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** A 24-bit DFM measurement "float" for v ≥ 0 (the largest exponent that keeps the mantissa in 20 bits). */
inline uint32_t toFl24(double v) {
    for (int e = 15; e >= 0; --e) { const double m = v * double(1u << e); if (m < 1048575.5) return uint32_t(e) << 20 | uint32_t(std::lround(m)); }
    return 0xFFFFF;
}
struct Dfm {
    Flight fl;
    uint32_t serial = 19012345;
    int snId = 0xA;               ///< 0xA DFM-09, 0xB DFM-17 …; 6 = a DFM-06 (serial `sn6`, hex digits)
    uint32_t sn6 = 0x412345;
    double tempC = -21.5;
    int mode = 2;                 ///< the GPS packets' layout (≤ 2 or 3)
    /** The frames for `secs` seconds, back to back (≈ 4.46 a second), as chips — a GPS epoch is the nine DAT packets. */
    std::vector<int8_t> chips(int secs, int frames0 = 0) const {
        std::vector<int8_t> out;
        const int nFrames = int(secs * 2500.0 / 560.0);
        const double rf = snId == 0xB ? 332e3 : 220e3;
        const double R = vibedfm::Thermistor::get().ohms(tempC), Rs = 20e3, g = 2.0;
        uint32_t meas[9];
        for (int k = 0; k < 9; ++k) meas[k] = toFl24(1000.0 + 37.0 * k);
        meas[0] = toFl24(g * (R + Rs)); meas[3] = toFl24(g * Rs); meas[4] = toFl24(g * rf);
        // the CONF cycle: measurement channels 0…8, then the serial channel (its halves in turn)
        int confK = 0, half = 0, dat = 0;
        for (int fno = frames0; fno < frames0 + nFrames; ++fno) {
            uint8_t conf[7], d[2][13];
            const int slot = confK++ % 10;
            if (slot < 9) vibedfm::confNibbles(slot, meas[slot], conf);
            else if (snId == 6) {
                if (half++ & 1) vibedfm::confNibbles(6, sn6, conf);
                else vibedfm::confNibbles(5, 0xA00000, conf);   // the DFM-06's "null" channel 0x5A…
            } else {
                const int hl = half++ & 1;
                const uint32_t h16 = hl ? (serial & 0xFFFF) : (serial >> 16);
                vibedfm::confNibbles(snId, 0xC00000u | (h16 << 4) | uint32_t(hl), conf);
            }
            for (int q = 0; q < 2; ++q, ++dat) {
                const int id = dat % 9, epoch = dat / 9;
                const double t = epoch;              // ★ one epoch a second
                double la, lo, h; fl.at(t, la, lo, h);
                const double hv = std::hypot(fl.vE, fl.vN);
                double dir = std::atan2(fl.vE, fl.vN) * 180 / M_PI; if (dir < 0) dir += 360;
                uint32_t v32 = 0; uint16_t v16 = 0;
                const double un = fl.unix0 + t;
                const long day = long(std::floor(un / 86400)); const double sod = un - day * 86400.0;
                const int msInMin = int(std::lround(std::fmod(sod, 60.0) * 1000));
                if (mode <= 2) {
                    switch (id) {
                    case 0: v32 = uint32_t(mode) << 8 | uint32_t(epoch & 0xFF); break;
                    case 1: v32 = 0x0F0F0F0F; v16 = uint16_t(msInMin); break;
                    case 2: v32 = uint32_t(int32_t(std::lround(la * 1e7))); v16 = uint16_t(int16_t(std::lround(hv * 100))); break;
                    case 3: v32 = uint32_t(int32_t(std::lround(lo * 1e7))); v16 = uint16_t(std::lround(dir * 100)); break;
                    case 4: v32 = uint32_t(int32_t(std::lround(h * 100))); v16 = uint16_t(int16_t(std::lround(fl.vU * 100))); break;
                    case 8: {
                        // civil date from days
                        long z = day + 719468; const long era = (z >= 0 ? z : z - 146096) / 146097; const long doe = z - era * 146097;
                        const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long y = yoe + era * 400;
                        const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100); const long mp = (5 * doy + 2) / 153;
                        const long dd = doy - (153 * mp + 2) / 5 + 1; const long mm = mp < 10 ? mp + 3 : mp - 9; y += mm <= 2;
                        const int hh = int(sod / 3600), mi = int(std::fmod(sod, 3600) / 60);
                        v32 = uint32_t(y) << 20 | uint32_t(mm) << 16 | uint32_t(dd) << 11 | uint32_t(hh) << 6 | uint32_t(mi);
                        v16 = uint16_t(9 << 8);
                        break; }
                    default: v32 = 0x12345678u + uint32_t(id); v16 = 0x1111; break;
                    }
                } else {
                    switch (id) {
                    case 0: v32 = uint32_t(msInMin) << 16 | uint32_t(mode) << 8 | uint32_t(epoch & 0xFF); v16 = uint16_t(int16_t(std::lround(hv * 100))); break;
                    case 1: v32 = uint32_t(int32_t(std::lround(la * 1e7))); v16 = uint16_t(std::lround(dir * 100)); break;
                    case 2: v32 = uint32_t(int32_t(std::lround(lo * 1e7))); v16 = uint16_t(int16_t(std::lround(fl.vU * 100))); break;
                    case 3: v32 = uint32_t(int32_t(std::lround(h * 100))); break;
                    default: v32 = 0x01020304u * uint32_t(id); break;
                    }
                }
                vibedfm::datNibbles(v32, v16, id, d[q]);
            }
            uint8_t bits[vibedfm::kFrameBits];
            vibedfm::buildFrame(conf, d[0], d[1], bits);
            manchester(bits, vibedfm::kFrameBits, out);
        }
        return out;
    }
};

// ── M10 / M20 ───────────────────────────────────────────────────────────────────────────────────────────────────────
struct Mxx {
    Flight fl;
    bool m20 = false;
    uint8_t sn[5] = { 0x02, 0x00, 0x93, 0x34, 0x52 };   ///< M10 serial bytes 0x5D…0x61
    uint32_t sn20 = (5u * 12 + 3) | (1u << 7) | (1234u << 10);   ///< M20: "504-2-01234" (year 5, month 4, …)
    int week = 2390;
    uint8_t utcOfs = 18;
    /** Frame `k` (k seconds after the start), on-air bytes. */
    std::vector<uint8_t> frame(int k) const {
        double la, lo, h; fl.at(k, la, lo, h);
        const double un = fl.unix0 + k;
        const double gps = un - 315964800.0 + 18.0;
        const int wk = int(gps / 604800.0);
        const double tow = gps - wk * 604800.0;
        auto p16 = [](uint8_t* p, int v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); };
        auto p24 = [](uint8_t* p, int v) { p[0] = uint8_t(v >> 16); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v); };
        auto p32 = [](uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); };
        std::vector<uint8_t> f;
        if (!m20) {
            f.assign(vibemxx::kM10Len, 0);
            std::mt19937 r(uint32_t(k) * 7 + 3);
            for (auto& b : f) b = uint8_t(r());
            f[0] = 0x64; f[1] = 0x9F; f[2] = 0x20;
            p16(&f[4], int(std::lround(fl.vE * 200))); p16(&f[6], int(std::lround(fl.vN * 200))); p16(&f[8], int(std::lround(fl.vU * 200)));
            p32(&f[0x0A], uint32_t(std::lround(tow * 1000)));
            p32(&f[0x0E], uint32_t(int32_t(std::lround(la * 4294967296.0 / 360.0))));
            p32(&f[0x12], uint32_t(int32_t(std::lround(lo * 4294967296.0 / 360.0))));
            p32(&f[0x16], uint32_t(int32_t(std::lround(h * 1000))));
            f[0x1D] = 0x00; f[0x1E] = 8; f[0x1F] = utcOfs;
            p16(&f[0x20], wk);
            std::memcpy(&f[0x5D], sn, 5);
            f[0x62] = uint8_t(k);
        } else {
            f.assign(vibemxx::kM20Len, 0);
            std::mt19937 r(uint32_t(k) * 11 + 5);
            for (auto& b : f) b = uint8_t(r());
            f[0] = 0x45; f[1] = 0x20;
            p24(&f[0x08], int(std::lround(h * 100)));
            p16(&f[0x0B], int(std::lround(fl.vE * 100))); p16(&f[0x0D], int(std::lround(fl.vN * 100)));
            p24(&f[0x0F], int(std::lround(tow)));
            f[0x12] = uint8_t(sn20); f[0x13] = uint8_t(sn20 >> 8); f[0x14] = uint8_t(sn20 >> 16);
            f[0x15] = uint8_t(k);
            p16(&f[0x18], int(std::lround(fl.vU * 100)));
            p16(&f[0x1A], wk);
            p32(&f[0x1C], uint32_t(int32_t(std::lround(la * 1e6))));
            p32(&f[0x20], uint32_t(int32_t(std::lround(lo * 1e6))));
        }
        vibemxx::seal(f.data(), int(f.size()));
        return f;
    }
    /** Frame k's chips: a preamble, the raw header, then the frame Manchester-coded after the differential code. */
    std::vector<int8_t> chips(int k) const {
        std::vector<int8_t> out;
        for (int i = 0; i < 48; ++i) out.push_back((i & 1) ? -1 : 1);
        rawChips(vibemxx::kRawHeader, out);
        const std::vector<uint8_t> f = frame(k);
        // ★ the header's last pair "01" reads as Manchester 1: the differential code starts from it
        const std::vector<uint8_t> m = vibemxx::diffEncode(f.data(), int(f.size()), 1);
        manchester(m.data(), m.size(), out);
        return out;
    }
    std::string serialText() const {
        uint8_t f[vibemxx::kM10Len] = {0};
        if (m20) { f[0x12] = uint8_t(sn20); f[0x13] = uint8_t(sn20 >> 8); f[0x14] = uint8_t(sn20 >> 16); return vibemxx::m20Serial(f); }
        std::memcpy(f + 0x5D, sn, 5);
        return vibemxx::m10Serial(f);
    }
};

}  // namespace vibesondetx
