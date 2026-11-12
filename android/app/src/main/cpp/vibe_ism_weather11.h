// vibe_ism_weather11.h — V12 ISM decoder, weather pass 2026-11-11: TFA Drop 30.3233.01 rain gauge, Inkbird ITH-20R /
// IBS-P01R. Sources: the TFA Drop protocol discussion (rtl_433 issue #1240) and Dmitriy Kozyrev's Inkbird analysis
// (rtl_433 Google group), via rtl_433's tfa_drop_30.3233.c / inkbird_ith20r.c field tables — FACTS only.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx11 {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TFA DROP 30.3233.01 — OOK PWM, ~500 µs = 1, ~250 µs = 0, 750 µs syncs; 66 bits, repeated: 3 · ID 20 · battery low ·
//  reset · 2 · counter 4 · rain low byte · 0xAA · rain high byte · reflected LFSR digest (generator 0x31, key 0xF4) of
//  the 7 bytes · 2. Tips = (16-bit count + 10) mod 65536, 0.254 mm each.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tfaDrop(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::pwm(b.ook, 255, 510, 1300, 0, 750);
    const int i = wx::repeatedRow(rows, 2, 66, 82);
    if (i < 0) return false;
    const auto by = bytesAt(rows[size_t(i)], 0, 8);
    if ((by[0] >> 4) != 3 || by[5] != 0xAA || wx::lfsrDigest8Refl(by.data(), 7, 0x31, 0xF4) != by[7]) return false;
    Reading r = wx::wxReading("TFA Dostmann", "Drop 30.3233 rain", wx::hex(unsigned(by[0] & 0xF) << 16 | by[1] << 8 | by[2], 5));
    r.rainMm = uint16_t((by[6] << 8 | by[4]) + 10) * 0.254;
    r.battOk = (by[3] & 0x80) ? 0 : 1;
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  INKBIRD ITH-20R / IBS-P01R pool — 433.92 MHz FSK NRZ, 100 µs bits; a ~1.4 s AA preamble, sync 2D D4, then 19 bytes:
//  type 3 · status · sensors · ? 2 · battery % · ID 16 · temperature 16 (LE, signed, 0.1 °C) · external 16 · humidity 16
//  (LE, 0.1 %) · CRC-16 (poly 0x8005 reflected, init 0x2F61 reflected) over 0–15, little-endian.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool inkbird(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : wx::fskRows(b, 100, 40, 160)) {
        for (size_t s : wx::findAll(row, 0xAA2DD4, 24)) {
            if (s + 24 + 18 * 8 > row.size()) break;
            const auto m = bytesAt(row, s + 24, 18);
            if (wx::crc16le(m.data(), 16, 0xA001, 0x86F4) != uint16_t(m[17] << 8 | m[16])) continue;
            Reading r = wx::wxReading("Inkbird", "ITH-20R / IBS-P01R", wx::hex(unsigned(m[9] << 8 | m[8]), 4));
            r.tempC = int16_t(m[11] << 8 | m[10]) / 10.0;
            const double h = (m[15] << 8 | m[14]) / 10.0;
            if (h <= 100) r.hum = h;
            r.battOk = m[7] > 15 ? 1 : 0;
            const int16_t ext = int16_t(m[13] << 8 | m[12]);
            if (ext != 0x1405) { char e[32]; std::snprintf(e, sizeof e, "probe %.1f °C", ext / 10.0); r.extra = e; }
            if (!wx::tempOk(r.tempC, -40, 100)) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (inkbird(b, out)) return true;
    if (tfaDrop(b, out)) return true;
    return false;
}

} // namespace wx11
} // namespace vibeism
