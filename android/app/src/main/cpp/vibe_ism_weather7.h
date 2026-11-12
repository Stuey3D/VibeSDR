// vibe_ism_weather7.h — V12 ISM decoder, weather pass 2026-11-11: ELV / La Crosse WS 2000 / WS 7000 / WS 2500 sensors,
// Ambient Weather F007TH family, Ambient Weather TX-8300 (TFA 30.3211.02).
// Sources: the ELV WS 2000 / WS 7000 sensor protocol as described publicly (and rtl_433 lacrosse_ws7000.c's field
// table), rtl_433 ambient_weather.c and ambientweather_tx8300.c field tables — FACTS only. Our own code throughout.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx7 {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WS 7000 / WS 2500 (ELV, La Crosse, Conrad) — OOK PWM, 1200 µs bit: ~400 µs mark = 1, ~800 µs = 0. Ten 0s and a 1,
//  then nibbles sent LSB first, each followed by a 1: type (0 thermo, 1 T/H, 2 rain, 3 wind, 4 T/H/pressure,
//  5 brightness) · address 3 bits + sign · BCD data (3–10 nibbles; rain = a 12-bit count × 0.3 mm) · XOR of the nibbles before · sum of all + 5.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool ws7000(const Burst& b, std::vector<Reading>& out) {
    static const uint8_t dataLen[6] = {3, 6, 3, 6, 10, 7};
    for (const Bits& row : wx::pwm(b.ook, 400, 800, 1100, 1)) {
        const size_t s0 = findBits(row, 0x01, 8);
        if (s0 == size_t(-1)) continue;
        std::vector<uint8_t> n;
        for (size_t p = s0 + 8; p + 5 <= row.size() && n.size() < 14; p += 5) {
            if (!row[p + 4]) break;
            n.push_back(uint8_t(bitsAtLsb(row, p, 4)));
        }
        if (n.size() < 7) continue;
        const int type = n[0];
        if (type > 5 || n.size() < size_t(dataLen[type] + 4)) continue;
        const size_t len = size_t(dataLen[type] + 4);   // type, address, data, XOR, sum
        unsigned x = 0, sum = 0;
        for (size_t k = 0; k + 1 < len; ++k) { if (k + 2 < len) x ^= n[k]; sum += n[k]; }
        if (x != n[len - 2] || ((sum + 5) & 0xF) != n[len - 1]) continue;
        Reading r = wx::wxReading("ELV / La Crosse", "", wx::hex(unsigned(type << 4 | (n[1] & 7)), 2));
        const int sign = (n[1] & 8) ? -1 : 1;
        auto bcd3 = [&](int a) { return n[size_t(a + 2)] * 10 + n[size_t(a + 1)] + n[size_t(a)] * 0.1; };
        switch (type) {
            case 0: r.model = "WS 7000-27/28 thermo"; r.tempC = sign * bcd3(2); break;
            case 1: r.model = "WS 7000-22/25 T/H"; r.tempC = sign * bcd3(2); r.hum = std::floor(bcd3(5)); break;
            case 2: r.model = "WS 7000-16 rain"; r.rainMm = (n[4] << 8 | n[3] << 4 | n[2]) * 0.3; break;
            case 3: r.model = "WS 7000-15 wind"; r.windKmh = bcd3(2); r.windDir = (n[7] >> 2) * 100 + n[6] * 10 + n[5]; break;
            case 4: r.model = "WS 7000-20 T/H/pressure"; r.tempC = sign * bcd3(2); r.hum = std::floor(bcd3(5));
                    r.baroHpa = n[10] * 100 + n[9] * 10 + n[8] + 200; break;
            default: {
                r.model = "WS 2500-19 brightness";
                unsigned lux = unsigned(n[4] * 100 + n[3] * 10 + n[2]);
                for (int e = n[5]; e > 0; --e) lux *= 10;
                r.extra = std::to_string(lux) + " lux";
            }
        }
        if (!std::isnan(r.tempC) && !wx::tempOk(r.tempC)) continue;
        if (!std::isnan(r.hum) && r.hum > 100) continue;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AMBIENT WEATHER F007TH / F012TH, TFA 30.3208.02, SwitchDoc F016TH — OOK Manchester ("first edge = 0"), 500 µs half
//  bit; after a 0x01 lead-in, 0x45 (model) · ID · battery low + channel 3 + temperature 12 bits ((raw − 400) / 10 °F) ·
//  humidity · LFSR digest (generator 0x98, key 0x3E) XOR 0x64 of the five bytes. Sometimes received inverted.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool f007th(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row0 : wx::mcZero(b.ook, 500, 2400)) {
        for (int inv = 0; inv < 2; ++inv) {
            const Bits row = inv ? invert(row0) : row0;
            for (size_t s : wx::findAll(row, 0x145, 12)) {
                if (s + 8 + 48 > row.size()) break;
                const auto by = bytesAt(row, s + 8, 6);
                if ((wx::lfsrDigest8(by.data(), 5, 0x98, 0x3E) ^ 0x64) != by[5]) continue;
                const double f = (((by[2] & 0xF) << 8 | by[3]) - 400) / 10.0;
                if (by[4] > 100 || f < -40 || f >= 344) continue;
                Reading r = wx::wxReading("Ambient Weather", "F007TH / F012TH", wx::hex(by[1], 2));
                r.battOk = (by[2] & 0x80) ? 0 : 1;
                r.channel = ((by[2] >> 4) & 7) + 1;
                r.tempC = (f - 32) / 1.8;
                r.hum = by[4];
                out.push_back(r);
                return true;
            }
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AMBIENT WEATHER TX-8300 / TFA 30.3211.02 — OOK PPM, ~2 ms marks, ~1.9 ms space = 0, ~3.9 ms = 1; 74 bits: 2 lead-in ·
//  humidity 2 BCD · flags 2 + channel 2 + sign + ID 7 · temperature 3 BCD (× 0.1 °C) · the same 4 bytes inverted · a
//  Fletcher-like check (complemented nibble sums of all bits and of bits 0 and 2 of each nibble).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tx8300(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : wx::ppm(b.ook, 1950, 3900, 6500, 0, 2800, 1200)) {
        if (w.size() != 74) continue;
        auto by = bytesAt(w, 2, 9);
        for (int k = 4; k < 8; ++k) by[size_t(k)] ^= 0xFF;
        by[0] = uint8_t((by[0] & 0x7F) | (by[4] & 0x80));
        if (by[0] != by[4] || by[1] != by[5] || by[2] != by[6] || by[3] != by[7]) continue;
        unsigned x = 0, y = 0;
        for (int k = 0; k < 4; ++k) { x += (by[size_t(k)] & 0xF) + (by[size_t(k)] >> 4); y += (by[size_t(k)] & 0x5) + ((by[size_t(k)] & 0x50) >> 4); }
        if ((((~x & 0xF) << 4) | (~y & 0xF)) != by[8]) continue;
        const int d1 = by[2] & 0xF, d2 = by[3] >> 4, d3 = by[3] & 0xF;
        if (d1 > 9 || d2 > 9 || d3 > 9) continue;
        Reading r = wx::wxReading("Ambient Weather", "TX-8300 / TFA 30.3211", wx::hex(unsigned((by[1] & 7) << 4 | by[2] >> 4), 2));
        r.channel = (by[1] >> 4) & 3;
        r.tempC = ((by[1] & 8) ? -1 : 1) * (d1 * 10 + d2 + d3 * 0.1);
        if ((by[0] >> 4) <= 9 && (by[0] & 0xF) <= 9) r.hum = (by[0] >> 4) * 10 + (by[0] & 0xF);
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (f007th(b, out)) return true;
    if (tx8300(b, out)) return true;
    if (ws7000(b, out)) return true;
    return false;
}

} // namespace wx7
} // namespace vibeism
