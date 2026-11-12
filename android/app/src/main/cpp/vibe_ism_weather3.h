// vibe_ism_weather3.h — V12 ISM decoder, weather pass 2026-11-11: LA CROSSE TECHNOLOGY (and its TFA Dostmann twins).
// Sources as vibe_ism_weather2.h: public write-ups where they exist (cited), and rtl_433's device-file field tables for
// FACTS only (lacrosse_tx141x.c, lacrossews.c, lacrosse.c, lacrosse_tx22uit.c). Our own code throughout.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace lacrosse {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TX141 family — TX141-Bv2 / -Bv3 (temperature), TX141TH-Bv2 (T/H), TX141W / TX145wsdth (T/H or wind), TFA 30.3221 /
//  30.3222 / 30.3243 / 30.3249 / 30.3251 twins. 433.92 MHz OOK PWM, 625 µs bit period: 417 µs high = 1, 208 µs = 0;
//  four ~833 µs sync pulses before each copy; 12 copies (Bv3: 4). (TX141W: 500 / 256 µs, sync ~750 µs.)
//   • 40 bits (TH): ID 8 · battery low 1 · test 1 · channel 2 · temperature 12 ((raw − 500) / 10 °C) · humidity 8 ·
//     check = the reflected LFSR digest (generator 0x31, key 0xF4) of the first 4 bytes. 41 bits: the same (TH-Bv3).
//   • 37 / 33 / 32 bits (temperature only): the same first 3 bytes, the battery bit inverted; no check — the copies must
//     agree (≥ 5 of 12, or 3 of 4).
//   • 65 bits (TX141W): 5 bits 00001 · ID 19 · battery 1 · test 1 · channel 2 · type 4 · value 12 · value 12 · CRC-8
//     0x31 over the first 7 bytes. Type 1 = temperature ((raw − 500) / 10) + humidity, type 2 = wind (0.1 km/h) +
//     direction (degrees).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tx141w(const std::vector<Bits>& rows, std::vector<Reading>& out) {
    for (const Bits& w : rows) {
        if (w.size() < 64 || w.size() > 66 || bitsAt(w, 0, 5) != 1) continue;
        const auto b = bytesAt(w, 0, 8);
        if (crc8(b.data(), 7, 0x31, 0) != b[7]) continue;
        const int type = b[3] & 0xF, v1 = b[4] << 4 | b[5] >> 4, v2 = (b[5] & 0xF) << 8 | b[6];
        if (type != 1 && type != 2) continue;
        Reading r = wx::wxReading("La Crosse", type == 1 ? "TX141W T/H" : "TX141W / TX145wsdth wind",
                                  wx::hex(uint32_t(b[0] & 7) << 16 | b[1] << 8 | b[2], 5));
        r.battOk = (b[3] & 0x80) ? 0 : 1;
        r.channel = (b[3] >> 4) & 3;
        if (type == 1) {
            r.tempC = (v1 - 500) / 10.0;
            if (!wx::tempOk(r.tempC) || v2 > 100) continue;
            r.hum = v2;
        } else {
            r.windKmh = v1 / 10.0;
            if (v2 < 360) r.windDir = v2;
        }
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool tx141(const Burst& b, std::vector<Reading>& out) {
    {   // the TX141W's faster timing first: its rows are 65 bits, never confused with the 40s below
        std::vector<Bits> rows = wx::pwm(b.ook, 256, 500, 600, 0, 750);
        if (tx141w(rows, out)) return true;
    }
    const std::vector<Bits> rows = wx::pwm(b.ook, 208, 417, 625, 0, 833);
    if (rows.empty()) return false;
    // 40 / 41 bits: the digest is strong enough for one copy
    for (const Bits& w : rows) {
        if (w.size() != 40 && w.size() != 41) continue;
        const auto by = bytesAt(w, 0, 5);
        if (wx::lfsrDigest8Refl(by.data(), 4, 0x31, 0xF4) != by[4]) continue;
        const int traw = (by[1] & 0xF) << 8 | by[2];
        const double t = (traw - 500) / 10.0;
        if (by[0] == 0 || by[3] == 0 || by[3] > 100 || !wx::tempOk(t)) continue;
        Reading r = wx::wxReading("La Crosse", w.size() == 41 ? "TX141TH-Bv3" : "TX141TH-Bv2", wx::hex(by[0], 2));
        r.battOk = (by[1] & 0x80) ? 0 : 1;
        r.channel = ((by[1] >> 4) & 3) + 1;
        r.tempC = t; r.hum = by[3];
        out.push_back(r);
        return true;
    }
    // temperature only: no check — repeats
    const int need = rows.size() > 5 ? 5 : 3;
    for (size_t len : {size_t(37), size_t(33), size_t(32)}) {
        const int i = wx::repeatedRow(rows, need, len, len);
        if (i < 0) continue;
        const auto by = bytesAt(rows[size_t(i)], 0, 3);
        const double t = (((by[1] & 0xF) << 8 | by[2]) - 500) / 10.0;
        if (by[0] == 0 || !wx::tempOk(t)) continue;
        Reading r = wx::wxReading("La Crosse", len == 37 ? "TX141-Bv2" : len == 33 ? "TX141-Bv3" : "TX141B",
                                  wx::hex(by[0], 2));
        r.battOk = (by[1] & 0x80) ? 1 : 0;
        r.channel = ((by[1] >> 4) & 3) + 1;
        r.tempC = t;
        r.confirm = true;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WS-2310 / WS-3600 stations — 433.92 MHz OOK PWM: ~368 µs mark = 1, ~1464 µs = 0, ~1336 µs fixed gap. 52 bits =
//  13 nibbles: 0 · station (9 = WS-2310, 6 = WS-3600) · type (bit 2 = gust flag, bit 1 parity, bits 1–0 type: 0 temp,
//  1 humidity, 2 rain, 3 wind) · ID 2 nibbles · flags 2 · value 3 nibbles · the first two value nibbles inverted ·
//  4-bit sum of nibbles 0–11. Odd parity over bit 9 and bits 27–39. Temperature BCD × 0.1 − 30 (WS-3600: − 40);
//  humidity 2 BCD digits; rain 12 bits × 0.518 mm; wind 8 bits × 0.1 m/s + direction nibble × 22.5°.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool ws2310(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : wx::pwm(b.ook, 368, 1464, 2500, 1)) {
        if (w.size() != 52) continue;
        int nb[13]; for (int i = 0; i < 13; ++i) nb[i] = int(bitsAt(w, size_t(4 * i), 4));
        if (nb[0] != 0 || (nb[1] != 9 && nb[1] != 6)) continue;
        unsigned sum = 0; for (int i = 0; i < 12; ++i) sum += unsigned(nb[i]);
        int par = w[9]; for (int i = 27; i <= 39; ++i) par += w[size_t(i)];
        if ((sum & 0xF) != unsigned(nb[12]) || !(par & 1) || nb[7] != (nb[10] ^ 0xF) || nb[8] != (nb[11] ^ 0xF)) continue;
        const int type = ((nb[2] >> 1) & 4) | (nb[2] & 3);
        Reading r = wx::wxReading("La Crosse", nb[1] == 6 ? "WS-3600" : "WS-2310", wx::hex(unsigned(nb[3] << 4 | nb[4]), 2));
        const int bcd3 = nb[7] * 100 + nb[8] * 10 + nb[9], bin = nb[7] << 8 | nb[8] << 4 | nb[9];
        if (type == 0) { r.tempC = (bcd3 - (nb[1] == 6 ? 400 : 300)) / 10.0; if (!wx::tempOk(r.tempC)) continue; }
        else if (type == 1) { if (nb[7] == 0xA) continue; r.hum = nb[7] * 10 + nb[8]; if (r.hum > 100) continue; }
        else if (type == 2) r.rainMm = bin * 0.518;
        else if (type == 3 || type == 7) {
            if (nb[7] == 0xF && nb[8] == 0xE) continue;   // no wind sensor
            const double kmh = (nb[7] * 16 + nb[8]) * 0.1 * 3.6;
            if (type == 3) r.windKmh = kmh; else r.gustKmh = kmh;
            r.windDir = nb[9] * 22.5;
        } else continue;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TX-3 / TX-4 / TX-6U / TX-7U / TX5U (and TFA 30.3120.90 / 30.3121) — 433.92 MHz OOK PWM: ~550 µs mark = 1,
//  ~1400 µs = 0, ~1000 µs gap. Source: "TX3 temperature/humidity sensor" protocol page, f6fbb.org/domo/sensors/tx3_th.php.
//  44 bits = 11 nibbles: 0x0 · 0xA (length) · type (0 temperature, E humidity, A rain) · ID 7 bits + parity 1 · value
//  3 BCD digits (V.V.V) · the first two repeated · 4-bit sum of nibbles 0–9. Even parity over the value's 12 bits and the
//  parity bit. Temperature = value − 50 °C; rain (TX5U) = the 12-bit binary count × 0.2667 mm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tx3(const Burst& b, std::vector<Reading>& out) {
    // ★ the strict slicer (every pulse within 35 % of its width): the check is only 5 bits
    for (const Bits& w : slicePwm(b.ook, 550, 1400, 3000, true, 0.35)) {
        if (w.size() != 44 || bitsAt(w, 0, 8) != 0x0A) continue;
        int nb[11]; for (int i = 0; i < 11; ++i) nb[i] = int(bitsAt(w, size_t(4 * i), 4));
        unsigned sum = 0; for (int i = 0; i < 10; ++i) sum += unsigned(nb[i]);
        int par = nb[4] & 1; for (int i = 20; i < 32; ++i) par += w[size_t(i)];
        if ((sum & 0xF) != unsigned(nb[10]) || (par & 1) || nb[5] != nb[8] || nb[6] != nb[9]) continue;
        const int type = nb[2];
        Reading r = wx::wxReading("La Crosse", "TX-3 / TX-6U / TX-7U", wx::hex(unsigned(nb[3] << 3 | nb[4] >> 1), 2));
        const double v = nb[5] * 10 + nb[6] + nb[7] * 0.1;
        if (type == 0) { if (nb[5] > 9 || nb[6] > 9 || nb[7] > 9) continue; r.tempC = v - 50; if (!wx::tempOk(r.tempC)) continue; }
        else if (type == 0xE) { if (nb[5] > 9 || nb[6] > 9 || v > 100) continue; r.hum = std::floor(v); }
        else if (type == 0xA) { r.model = "TX5U rain"; r.rainMm = (nb[5] << 8 | nb[6] << 4 | nb[7]) * 0.2667; }
        else continue;
        r.confirm = true;   // ★ a 4-bit sum and one parity bit: shown once heard twice
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TX22U-IT (WS-1611-IT) — 915 MHz FSK NRZ, 116 µs bits (an OOK edition: 120 µs, inverted). Preamble AA AA, sync 2D D4,
//  ID · flags (low 3 bits = how many 2-byte "quartets" follow: 1–3, or 5 while pairing) · quartets · CRC-8 0x31 over
//  ID … last quartet. Quartet: type nibble + 12 bits — 0 temperature (3 BCD, (v − 400) / 10 °C), 1 humidity (3 BCD),
//  2 rain (binary × 0.518 mm), 3 wind (direction nibble × 22.5°, speed byte × 0.1 km/h), 4 gust (binary × 0.1 km/h).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tx22u(const Burst& b, std::vector<Reading>& out) {
    for (double bit : {116.0, 120.0}) {
        for (const Bits& row : wx::fskRows(b, bit, 40, 64)) {
            for (size_t s : wx::findAll(row, 0xAAAA2DD4, 32)) {
                const auto by = bytesAt(row, s + 32, 14);
                const size_t avail = (row.size() - s - 32) / 8;
                for (int n : {5, 3, 2, 1}) {
                    const size_t len = size_t(2 + 2 * n);
                    if (len + 1 > avail || crc8(by.data(), len, 0x31, 0) != by[len] || (by[1] & 7) != n) continue;
                    Reading r = wx::wxReading("La Crosse", "TX22U-IT", wx::hex(by[0], 2));
                    int known = 0;
                    for (int q = 0; q < n; ++q) {
                        const uint8_t hi = by[size_t(2 + 2 * q)], lo = by[size_t(3 + 2 * q)];
                        const int d1 = hi & 0xF, d2 = lo >> 4, d3 = lo & 0xF, v = (hi & 0xF) << 8 | lo;
                        switch (hi >> 4) {
                            case 0: r.tempC = (d1 * 100 + d2 * 10 + d3 - 400) / 10.0; ++known; break;
                            case 1: r.hum = d1 * 100 + d2 * 10 + d3; ++known; break;
                            case 2: r.rainMm = v * 0.518; ++known; break;
                            case 3: r.windDir = d1 * 22.5; r.windKmh = lo * 0.1; ++known; break;
                            case 4: r.gustKmh = v * 0.1; ++known; break;
                            default: break;
                        }
                    }
                    if (!known) continue;
                    if (!std::isnan(r.tempC) && !wx::tempOk(r.tempC)) continue;
                    out.push_back(r);
                    return true;
                }
            }
        }
    }
    return false;
}

inline bool decodeWx3(const Burst& b, std::vector<Reading>& out) {
    if (tx22u(b, out)) return true;
    if (tx141(b, out)) return true;
    if (ws2310(b, out)) return true;
    if (tx3(b, out)) return true;
    return false;
}

} // namespace lacrosse
} // namespace vibeism
