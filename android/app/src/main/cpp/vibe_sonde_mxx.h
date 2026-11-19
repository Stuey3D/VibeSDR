// vibe_sonde_mxx.h — V12 radiosondes: the METEOMODEM M10 / M10+ / M20 frames (docs/v12/SONDES-TYPES.md). Our own code,
// from LISTENER DOCUMENTATION of the formats (Meteomodem publishes none — Stuart's RS41 decision applies). GPL decoders
// were read for facts only; nothing here is copied. ★ NOTHING HERE HAS BEEN DECODED OFF AIR YET.
//
// The air, as documented: FSK ≈ 9600 chips/s (M10 ≈ 9615, M20 ≈ 9600), Manchester ("01" = 1) and then DIFFERENTIAL —
// a frame bit is 1 when its Manchester bit EQUALS the one before (so the receiver's polarity does not matter). The raw
// header (32 chips) 10011001100110010100110010011001 is the same on both; the frame follows it, bytes most significant
// bit first. Byte 0 = the length − 1, byte 1 the type:
//   M10  (0x64, 0x9F) 101 bytes — a Trimble-style GPS packet from byte 2: 4/6/8 velocity E/N/U (i16, 0.005 m/s),
//        0x0A time of week (ms), 0x0E / 0x12 lat / lon (i32, 2³²/360 per degree), 0x16 altitude (i32 mm, ellipsoid),
//        0x1D fix flags (bit 0 set = no fix), 0x1E satellites, 0x1F GPS−UTC seconds, 0x20 week; 0x5D…0x61 serial.
//   M10+ (0x64, 0xAF) the same frame with another GPS: 0x04 / 0x08 lat / lon (i32, 1e-6°), 0x0C altitude (24-bit signed
//        cm), 0x0F / 0x11 / 0x13 velocity E/N/U (i16 cm/s), 0x15 time hhmmss, 0x18 date ddmmyy; serial as the M10.
//   M20  (0x45, 0x20) 70 bytes — 0x08 altitude (24 bits, cm), 0x0B / 0x0D velocity E/N (i16 cm/s), 0x0F time of week (24
//        bits, s), 0x12 serial (24 bits, little-endian), 0x18 climb (i16 cm/s), 0x1A week, 0x1C / 0x20 lat / lon (i32, 1e-6°).
//   Multi-byte values big-endian unless said. The LAST TWO BYTES are a 16-bit check of everything before them (the
//   documented linear update, `checkUpdate`) — with the fixed length and type bytes that is 32 bits a noise frame must hit.
// ★ No ECEF here: both send latitude / longitude directly (only the RS41 sends ECEF).
// ★★ NO TEMPERATURE for the M10 / M20 (calOf 0): the documented conversions rest on a thermistor whose maker and curve
//    the write-ups mark as uncertain — Stuart's rule is no guessed PTU. The clients say "temperature not decoded".
#pragma once
#include "vibe_rs41.h"
#include "vibe_sonde_manch.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace vibemxx {

constexpr double kChipRate = 9608.0;     ///< between the M10's ≈ 9615 and the M20's ≈ 9600; the timing loop follows either
constexpr const char* kRawHeader = "10011001100110010100110010011001";
constexpr int kM10Len = 101, kM20Len = 70, kMaxLen = 120;

/** The frame check's update: state c (16 bits), next byte b → (low byte of c) << 8 | (B(b) ⊕ A1(c) ⊕ A2(c)) with
 *  B = rotate right by one then x ⊕= x >> 2; A1 = c's low six bits with bit 6 = c0⊕c2⊕c4 and bit 7 = c1⊕c3⊕c5;
 *  A2 = (c >> 7) & 0xFF, then x ⊕= x >> 2 — the documented linear map. */
inline uint16_t checkUpdate(uint16_t c, uint8_t b) {
    uint8_t x = uint8_t((b >> 1) | (b << 7));
    x = uint8_t(x ^ (x >> 2));
    const unsigned par6 = (c ^ (c >> 2) ^ (c >> 4)) & 1u, par7 = ((c >> 1) ^ (c >> 3) ^ (c >> 5)) & 1u;
    const uint8_t a1 = uint8_t((c & 0x3F) | (par6 << 6) | (par7 << 7));
    uint8_t a2 = uint8_t((c >> 7) & 0xFF);
    a2 = uint8_t(a2 ^ (a2 >> 2));
    return uint16_t(((c & 0xFF) << 8) | uint8_t(x ^ a1 ^ a2));
}
inline uint16_t check(const uint8_t* f, int n) { uint16_t c = 0; for (int i = 0; i < n; ++i) c = checkUpdate(c, f[i]); return c; }
inline int lengthFor(uint8_t b0) { return b0 == 0x64 ? kM10Len : b0 == 0x45 ? kM20Len : 0; }

inline uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
inline uint32_t be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline int32_t sx24(uint32_t v) { return (v & 0x800000) ? int32_t(v) - 0x1000000 : int32_t(v); }

/** The M10 / M10+ serial from bytes 0x5D…0x61: "<hex><2 digits>-<hex>-<digit><4 digits>". Empty when implausible. */
inline std::string m10Serial(const uint8_t* f) {
    const uint8_t* s = f + 0x5D;
    if (!(s[0] | s[1] | s[2] | s[3] | s[4])) return "";
    const unsigned w = s[3] | unsigned(s[4]) << 8;
    char b[24];
    std::snprintf(b, sizeof b, "%X%02u-%X-%u%04u", (s[2] >> 4) & 0xF, s[2] & 0xF, s[0] & 0xF, (w >> 13) & 7, w & 0x1FFF);
    return b;
}
/** The M20 serial from its 24 bits at 0x12 (little-endian): year·12 + month in the low 7 bits. Empty when implausible. */
inline std::string m20Serial(const uint8_t* f) {
    const uint32_t v = uint32_t(f[0x12]) | uint32_t(f[0x13]) << 8 | uint32_t(f[0x14]) << 16;
    const unsigned ym = v & 0x7F;
    if (v == 0 || ym >= 120) return "";
    char b[24];
    std::snprintf(b, sizeof b, "%u%02u-%u-%u%04u", ym / 12, ym % 12 + 1, ((v >> 7) & 7) + 1, (v >> 23) & 1, (v >> 10) & 0x1FFF);
    return b;
}

inline bool posPlausible(double la, double lo, double h) {
    return std::fabs(la) <= 90 && std::fabs(lo) <= 180 && h > -500 && h < 60000 && !(la == 0 && lo == 0);
}

/** Parse a whole frame (bytes, length from byte 0). False = not a frame (length, type, check, serial). */
inline bool parse(const uint8_t* f, int len, vibers41::Frame& fr) {
    if (len < 8 || lengthFor(f[0]) != len) return false;
    if (check(f, len - 2) != be16(f + len - 2)) return false;
    fr.len = len;
    if (len == kM10Len && (f[1] == 0x9F || f[1] == 0xAF)) {
        fr.serial = m10Serial(f);
        if (fr.serial.empty()) return false;
        fr.statusOk = true;
        fr.frameNo = f[0x62];
        if (f[1] == 0x9F) {
            fr.kind = "M10";
            if (f[2] == 0x20 && !(f[0x1D] & 1)) {
                const double la = int32_t(be32(f + 0x0E)) * (360.0 / 4294967296.0), lo = int32_t(be32(f + 0x12)) * (360.0 / 4294967296.0);
                const double h = int32_t(be32(f + 0x16)) / 1000.0;
                if (posPlausible(la, lo, h)) {
                    fr.posOk = true; fr.lat = la; fr.lon = lo; fr.alt = h;
                    fr.vE = int16_t(be16(f + 4)) / 200.0; fr.vN = int16_t(be16(f + 6)) / 200.0; fr.vU = int16_t(be16(f + 8)) / 200.0;
                    fr.sats = f[0x1E];
                }
                int week = int(be16(f + 0x20));
                if (week > 0 && week < 4000) {
                    if (week < 1304) week += 1024;   // ★ the GPS week's 10-bit roll-over, as documented for this receiver
                    const int leap = f[0x1F] >= 10 && f[0x1F] <= 30 ? f[0x1F] : 18;
                    fr.unixTime = 315964800.0 + week * 604800.0 + be32(f + 0x0A) / 1000.0 - leap;
                }
            }
        } else {
            fr.kind = "M10"; fr.model = "M10+";
            const double la = int32_t(be32(f + 0x04)) / 1e6, lo = int32_t(be32(f + 0x08)) / 1e6, h = sx24(be24(f + 0x0C)) / 100.0;
            if (posPlausible(la, lo, h)) {
                fr.posOk = true; fr.lat = la; fr.lon = lo; fr.alt = h;
                fr.vE = int16_t(be16(f + 0x0F)) / 100.0; fr.vN = int16_t(be16(f + 0x11)) / 100.0; fr.vU = int16_t(be16(f + 0x13)) / 100.0;
            }
            const unsigned tm = be24(f + 0x15), dt = be24(f + 0x18);
            const int hh = int(tm / 10000), mi = int(tm / 100 % 100), ss = int(tm % 100);
            const int dd = int(dt / 10000), mo = int(dt / 100 % 100), yy = 2000 + int(dt % 100);
            if (hh < 24 && mi < 60 && ss < 61 && dd >= 1 && dd <= 31 && mo >= 1 && mo <= 12)
                fr.unixTime = double(vibesonde::daysFromCivil(yy, mo, dd)) * 86400 + hh * 3600 + mi * 60 + ss;
        }
    } else if (len == kM20Len && f[1] == 0x20) {
        fr.kind = "M20";
        fr.serial = m20Serial(f);
        if (fr.serial.empty()) return false;
        fr.statusOk = true;
        fr.frameNo = f[0x15];
        const double la = int32_t(be32(f + 0x1C)) / 1e6, lo = int32_t(be32(f + 0x20)) / 1e6, h = sx24(be24(f + 0x08)) / 100.0;
        if (posPlausible(la, lo, h)) {
            fr.posOk = true; fr.lat = la; fr.lon = lo; fr.alt = h;
            fr.vE = int16_t(be16(f + 0x0B)) / 100.0; fr.vN = int16_t(be16(f + 0x0D)) / 100.0; fr.vU = int16_t(be16(f + 0x18)) / 100.0;
        }
        int week = int(be16(f + 0x1A));
        if (week > 0 && week < 4000) {
            if (week < 1304) week += 1024;
            fr.unixTime = 315964800.0 + week * 604800.0 + be24(f + 0x0F) - 18.0;
        }
    } else {
        return false;
    }
    fr.calN = 0; fr.calOf = 0;
    return true;
}

/** Frame bytes → the frame bits as SENT (Manchester bits, before the differential code is undone): m[k] = m[k−1] ⊕ ¬bit,
 *  starting from `prev` — for our transmitter. */
inline std::vector<uint8_t> diffEncode(const uint8_t* f, int len, int prev) {
    std::vector<uint8_t> m(size_t(len) * 8);
    int p = prev;
    for (int i = 0; i < len * 8; ++i) {
        const int b = (f[i >> 3] >> (7 - (i & 7))) & 1;
        p ^= 1 - b;
        m[size_t(i)] = uint8_t(p);
    }
    return m;
}
/** Write the check into the last two bytes. */
inline void seal(uint8_t* f, int len) { const uint16_t c = check(f, len - 2); f[len - 2] = uint8_t(c >> 8); f[len - 1] = uint8_t(c); }

// ── One channel's M10 / M20 receiver ────────────────────────────────────────────────────────────────────────────────
class Chan {
public:
    explicit Chan(double chRate) : rx_(chRate, kChipRate, kRawHeader, 3) {}
    uint64_t syncs() const { return rx_.syncs(); }
    uint64_t checkFails() const { return fails_; }
    template <class F> void process(const vibesonde::cf32* z, int n, F&& onFrame) {
        Sink<F> s{ this, onFrame };
        rx_.process(z, n, s);
    }

private:
    template <class F> struct Sink {
        Chan* c; F& f;
        int need(const float* soft, int n, float prevSoft) {
            if (n < 8) return 8;
            if (n == 8) {
                uint8_t b0 = 0;
                float pv = prevSoft;
                for (int i = 0; i < 8; ++i) { b0 = uint8_t(b0 << 1 | ((soft[i] * pv) > 0 ? 1 : 0)); pv = soft[i]; }
                const int L = lengthFor(b0);
                c->want_ = L ? L * 8 : -1;
            }
            return c->want_;
        }
        void done(const float* soft, int n, float prevSoft, double snr, double off, double) {
            uint8_t fb[kMaxLen] = {0};
            const int len = n / 8;
            if (len > kMaxLen) return;
            float pv = prevSoft;
            for (int i = 0; i < n; ++i) { if ((soft[i] * pv) > 0) fb[i >> 3] |= uint8_t(0x80 >> (i & 7)); pv = soft[i]; }
            vibers41::Frame fr;
            if (!parse(fb, len, fr)) { ++c->fails_; return; }
            fr.snrDb = snr; fr.offsetHz = off;
            f(fr);
        }
    };
    vibesonde::ManchRx rx_;
    int want_ = 0;
    uint64_t fails_ = 0;
};

}  // namespace vibemxx
