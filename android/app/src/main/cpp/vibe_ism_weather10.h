// vibe_ism_weather10.h — V12 ISM decoder, weather pass 2026-11-11: Bresser 868 MHz FSK stations (5-in-1, 6-in-1 family —
// 7-in-1 indoor, new 5-in-1, Froggit WH6000, Ventus C8488A, Sencor SWS 9898, soil / pool / 7-channel T/H, 3-in-1 wind;
// 7-in-1 / 3-in-1 / 8-in-1 and the PM / CO2 / HCHO air sensors), Maverick ET-732/733 BBQ, Companion WTR001.
// Sources: andreafabrizi/BresserWeatherCenter (the 5-in-1 layout), the Adafruit forum Maverick thread, and rtl_433
// device-file field tables (bresser_5in1.c, bresser_6in1.c, bresser_7in1.c, maverick_et73x.c, companion_wtr001.c) for
// FACTS only. Our own code throughout.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx10 {

inline int bcd2(uint8_t v) { return (v >> 4) * 10 + (v & 0xF); }
inline bool bcdOk(uint8_t v) { return (v >> 4) <= 9 && (v & 0xF) <= 9; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  BRESSER — 868.3 MHz FSK NRZ, 124 µs bits; preamble AA…, sync 2D D4.
//   • 6-in-1 family (18 bytes): LFSR-16 digest (generator 0x8810, key 0x5412) of bytes 2–16 = bytes 0–1 · ID 32 ·
//     sensor type 4 (1 station, 2 T/H, 3 pool, 4 soil) + startup + channel 3 · gust / average wind (inverted BCD, 0.1 m/s)
//     · direction 3 BCD · temperature 3 BCD (0.1 °C, sign bit) + battery bit · humidity BCD — or, when the flag bit of
//     byte 16 is set, rain (inverted 6 BCD digits, 0.1 mm) · UV (inverted BCD) · byte sum of 2–17 = 0xFF.
//   • 7-in-1 family (25 bytes, whitened with 0xAA): the digest (generator 0x8810, key 0xBA95) XOR 0x6DF1 = bytes 0–1 ·
//     ID 16 · direction 3 BCD · type (byte 6 high nibble, before de-whitening: 1 / 12 / 13 weather, 8 PM, 10 CO2, 11
//     HCHO/VOC) · gust, average (BCD, 0.1 m/s) · rain 6 BCD (0.1 mm) · temperature 3 BCD (0.1 °C, > 60.0 = − 100) ·
//     humidity · light 6 BCD (lux) · UV 3 BCD.
//   • 5-in-1 (26 bytes): bytes 0–12 the bitwise inverse of 13–25 · ID · type · gust 12 bits (0.1 m/s, its high nibble
//     out of order) + direction nibble · average 3 BCD · temperature 3 BCD (sign nibble in byte 25) · humidity BCD ·
//     rain 4 BCD (0.1 mm) · battery.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool bresser(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : wx::fskRows(b, 124, 40, 120)) {
        for (size_t s : wx::findAll(row, 0xAA2DD4, 24)) {
            const size_t p = s + 24, avail = row.size() > p ? (row.size() - p) / 8 : 0;
            if (avail < 18) continue;
            auto m = bytesAt(row, p, 26);
            // 6-in-1
            if (uint16_t(m[0] << 8 | m[1]) == wx::lfsrDigest16(m.data() + 2, 15, 0x8810, 0x5412) &&
                (wx::addBytes(m.data() + 2, 16) & 0xFF) == 0xFF) {
                const int stype = m[6] >> 4;
                Reading r = wx::wxReading("Bresser", stype == 2 ? "6-in-1 family T/H" : stype == 3 ? "pool thermometer" :
                                          stype == 4 ? "soil probe" : "6-in-1 / 5-in-1 (new)",
                                          wx::hex(uint32_t(m[2]) << 24 | m[3] << 16 | m[4] << 8 | m[5], 8));
                r.channel = m[6] & 7;
                const bool rain = m[16] & 1;
                if (!rain) {
                    r.battOk = (m[13] >> 1) & 1;
                    if (m[12] <= 0x99 && (m[13] & 0xF0) <= 0x90) {
                        const int t = (m[12] >> 4) * 100 + (m[12] & 0xF) * 10 + (m[13] >> 4);
                        double tc = (m[13] & 8) ? (t - 1000) / 10.0 : t / 10.0;
                        if (tc < -50) tc = -t / 10.0;
                        r.tempC = tc;
                        const int h = bcd2(m[14]);
                        if (stype == 4) { if (h >= 1 && h <= 16) { static const uint8_t mm[16] = {0, 7, 13, 20, 27, 33, 40, 47, 53, 60, 67, 73, 80, 87, 93, 99};
                                                                    r.extra = "soil moisture " + std::to_string(mm[h - 1]) + " %"; } }
                        else if (h <= 100) r.hum = h;
                    }
                } else {
                    const uint8_t a = uint8_t(~m[12]), c = uint8_t(~m[13]), d = uint8_t(~m[14]);
                    r.rainMm = (bcd2(a) * 10000 + bcd2(c) * 100 + bcd2(d)) * 0.1;
                }
                if (stype != 2 && stype != 4) {
                    const uint8_t g0 = uint8_t(~m[7]), g1 = uint8_t(~m[8]), g2 = uint8_t(~m[9]);
                    if (g0 <= 0x99 && g1 <= 0x99 && g2 <= 0x99) {
                        r.gustKmh = ((g0 >> 4) * 100 + (g0 & 0xF) * 10 + (g1 >> 4)) * 0.1 * 3.6;
                        r.windKmh = ((g2 >> 4) * 100 + (g2 & 0xF) * 10 + (g1 & 0xF)) * 0.1 * 3.6;
                        r.windDir = (m[10] >> 4) * 100 + (m[10] & 0xF) * 10 + (m[11] >> 4);
                    }
                    const uint8_t u0 = uint8_t(~m[15]), u1 = uint8_t(~m[16]);
                    if ((m[16] & 0xF) == 0 && u0 <= 0x99 && (u1 & 0xF0) <= 0x90) r.uv = ((u0 >> 4) * 100 + (u0 & 0xF) * 10 + (u1 >> 4)) * 0.1;
                }
                if (!std::isnan(r.tempC) && !wx::tempOk(r.tempC)) continue;
                out.push_back(r);
                return true;
            }
            // 7-in-1
            if (avail >= 22) {
                const int stype = m[6] >> 4;
                auto w = m; for (auto& v : w) v ^= 0xAA;
                if (m[21] != 0 && (uint16_t(w[0] << 8 | w[1]) ^ wx::lfsrDigest16(w.data() + 2, 23, 0x8810, 0xBA95)) == 0x6DF1) {
                    Reading r = wx::wxReading("Bresser", "", wx::hex(unsigned(w[2] << 8 | w[3]), 4));
                    r.channel = m[6] & 7;
                    r.battOk = ((w[15] & 6) == 6) ? 0 : 1;
                    if (stype == 1 || stype == 12 || stype == 13) {
                        r.model = stype == 12 ? "3-in-1 Professional" : stype == 13 ? "8-in-1" : "7-in-1";
                        const int t = (w[14] >> 4) * 100 + (w[14] & 0xF) * 10 + (w[15] >> 4);
                        r.tempC = t > 600 ? (t - 1000) / 10.0 : t / 10.0;
                        r.hum = bcd2(w[16]);
                        r.rainMm = (bcd2(w[10]) * 10000 + bcd2(w[11]) * 100 + bcd2(w[12])) * 0.1;
                        if (stype != 12) {
                            r.windDir = (w[4] >> 4) * 100 + (w[4] & 0xF) * 10 + (w[5] >> 4);
                            r.gustKmh = ((w[7] >> 4) * 100 + (w[7] & 0xF) * 10 + (w[8] >> 4)) * 0.1 * 3.6;
                            r.windKmh = ((w[8] & 0xF) * 100 + (w[9] >> 4) * 10 + (w[9] & 0xF)) * 0.1 * 3.6;
                            r.uv = ((w[20] >> 4) * 100 + (w[20] & 0xF) * 10 + (w[21] >> 4)) * 0.1;
                            const int lux = bcd2(w[17]) * 10000 + bcd2(w[18]) * 100 + bcd2(w[19]);
                            r.extra = std::to_string(lux) + " lux";
                        }
                        if (!wx::tempOk(r.tempC) || r.hum > 100) continue;
                    } else if (stype == 8) {
                        r.kind = "sensor"; r.model = "air quality PM2.5 / PM10";
                        const int pm25 = (w[10] & 0xF) * 1000 + bcd2(w[11]) * 10 + (w[12] >> 4);
                        const int pm10 = (w[12] & 0xF) * 1000 + bcd2(w[13]) * 10 + (w[14] >> 4);
                        r.extra = "PM2.5 " + std::to_string(pm25) + " µg/m³ · PM10 " + std::to_string(pm10) + " µg/m³";
                    } else if (stype == 10) {
                        r.kind = "sensor"; r.model = "CO2";
                        r.extra = "CO2 " + std::to_string(bcd2(w[4]) * 100 + bcd2(w[5])) + " ppm";
                    } else if (stype == 11) {
                        r.kind = "sensor"; r.model = "HCHO / VOC";
                        r.extra = "HCHO " + std::to_string(bcd2(w[4]) * 100 + bcd2(w[5])) + " ppb · VOC level " + std::to_string(w[22] & 0xF);
                    } else continue;
                    out.push_back(r);
                    return true;
                }
            }
            // 5-in-1
            if (avail >= 26) {
                bool inv = true;
                for (int k = 0; k < 13 && inv; ++k) inv = (m[size_t(k)] ^ m[size_t(k + 13)]) == 0xFF;
                if (!inv) continue;
                Reading r = wx::wxReading("Bresser", "5-in-1", wx::hex(m[14], 2));
                if ((m[20] & 0xF) <= 9) {
                    int t = (m[20] & 0xF) + (m[20] >> 4) * 10 + (m[21] & 0xF) * 100;
                    if (m[25] & 0xF) t = -t;
                    r.tempC = t / 10.0;
                }
                if ((m[22] & 0xF) <= 9) r.hum = bcd2(m[22]);
                r.windDir = (m[17] >> 4) * 22.5;
                r.gustKmh = (((m[17] & 0xF) << 8) + m[16]) * 0.1 * 3.6;
                r.windKmh = ((m[18] & 0xF) + (m[18] >> 4) * 10 + (m[19] & 0xF) * 100) * 0.1 * 3.6;
                r.rainMm = ((m[23] & 0xF) + (m[23] >> 4) * 10 + (m[24] & 0xF) * 100 + (m[24] >> 4) * 1000) * 0.1;
                r.battOk = (m[25] & 0x80) ? 0 : 1;
                if (!std::isnan(r.tempC) && !wx::tempOk(r.tempC)) continue;
                out.push_back(r);
                return true;
            }
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  MAVERICK ET-732 / ET-733 BBQ — OOK Manchester ("first edge = 0"), 230 µs half-bit; 104 raw bits starting 55 66 6A,
//  then Manchester again (01 = 1): 0xFA8 · state 4 · probe 1 and 2, 10 bits each (°C + 532) · a 16-bit digest that
//  includes a hidden per-reset ID (so it cannot be checked: the raw pattern and repeats are the check).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool maverick(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : wx::mcZero(b.ook, 230, 4000)) {
        if (row.size() != 104 || bitsAt(row, 0, 24) != 0x55666A) continue;
        Bits d;
        for (size_t k = 0; k + 1 < 104; k += 2) { if (row[k] == row[k + 1]) break; d.push_back(row[k + 1]); }
        if (d.size() < 52 || bitsAt(d, 0, 12) != 0xFA8) continue;
        const int t1 = int(bitsAt(d, 16, 10)) - 532, t2 = int(bitsAt(d, 26, 10)) - 532;
        if (t1 < -50 || t1 > 400 || t2 < -50 || t2 > 400) continue;
        Reading r = wx::wxReading("Maverick", "ET-732 / ET-733 BBQ", "");
        r.kind = "sensor";
        r.tempC = t1;
        char e[32]; std::snprintf(e, sizeof e, "probe 2 %d °C", t2); r.extra = e;
        r.confirm = true;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  COMPANION WTR001 — OOK PWM, 2928 µs period: 2196 µs mark = 1, 732 µs = 0 (after inversion), 1464 µs sync; 14 bits ×5:
//  tenths 5 bits (LSB first, + 10) · 0 · whole degrees 7 bits in a scrambled order (+ 41) · odd parity.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool companion(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::pwm(b.ook, 732, 2196, 4000, 1, 1464);
    const int i = wx::repeatedRow(rows, 3, 14, 14);
    if (i < 0) return false;
    uint8_t by[2] = {uint8_t(~bitsAt(rows[size_t(i)], 0, 8)), uint8_t(~(bitsAt(rows[size_t(i)], 8, 6) << 2) & 0xFC)};
    if (by[0] & 0x04) return false;
    if (!(wx::parity8(by[0]) ^ wx::parity8(by[1]))) return false;
    int tenth = wx::rev8(by[0] & 0xF8);
    if (tenth < 10 || tenth > 19) return false;
    tenth -= 10;
    const int whole = wx::rev8(by[1] & 0xF0) | wx::rev8(by[0] & 0x03) >> 2 | (by[1] & 0x08) << 3;
    if (whole < 11 || whole > 111) return false;
    Reading r = wx::wxReading("Companion", "WTR001", "");
    r.tempC = whole + tenth / 10.0 - 41.0;
    r.confirm = true;
    out.push_back(r);
    return true;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (bresser(b, out)) return true;
    if (maverick(b, out)) return true;
    if (companion(b, out)) return true;
    return false;
}

} // namespace wx10
} // namespace vibeism
