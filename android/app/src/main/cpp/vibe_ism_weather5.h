// vibe_ism_weather5.h — V12 ISM decoder, weather pass 2026-11-11: supermarket / own-brand thermo-hygrometers and
// stations — the S3318P family (Conrad, Esperanza, Kedsum, Pearl), Digitech XC-0324, Hideki (Cresta, Bresser 5CH,
// TFA, Mebus, Sigma), Globaltronics GT-WT-02 / -03, WT0122/0124 pool, Cotech 36-7959 (Sainlogic, SwitchDoc), TFA pool.
// Sources as vibe_ism_weather2.h: rtl_433 device-file field tables (esperanza_ews.c, s3318p.c, kedsum.c,
// digitech_xc0324.c + its wiki tutorial, hideki.c, gt_wt_02.c, gt_wt_03.c, wt0124.c, cotech_36_7959.c,
// tfa_pool_thermometer.c) for FACTS only. Our own code throughout.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx5 {

inline double fToC(double f) { return (f - 32.0) / 1.8; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  S3318P family (Conrad S3318P, Esperanza EWS-103, Kedsum, Pearl NC-5849 / NC-7415, ORIA ST389) — OOK PPM, ~500 µs
//  marks, ~2 ms space = 0, ~4 ms = 1; 42-bit rows (2 leading 0s), repeated. ID 8 · flags 2 (Kedsum: battery) +
//  channel 2 · temperature 12 bits in nibble order 3-1-2 ((raw − 900) / 10 °F) · humidity (nibbles swapped) · flags 4
//  (S3318P: button, battery low) · CRC-4 (poly 3) of the first 4 bytes XOR the flags nibble.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool s3318(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 1950, 3900, 4400, 0, 1200, 0, 120);
    const int i = wx::repeatedRow(rows, 3, 42, 42);
    if (i < 0) return false;
    const auto by = bytesAt(rows[size_t(i)], 2, 5);
    if (!(by[0] | by[1] | by[2] | by[3])) return false;
    if ((wx::crc4(by.data(), 4, 3, 0) ^ (by[4] >> 4)) != (by[4] & 0xF)) return false;
    const int traw = (by[2] & 0x0F) << 8 | (by[2] & 0xF0) | (by[1] & 0x0F);
    const int h = (by[3] & 0x0F) << 4 | by[3] >> 4;
    const double f = (traw - 900) / 10.0;
    if (f < -40 || f > 158 || h > 100) return false;
    Reading r = wx::wxReading("Conrad / Esperanza / Kedsum", "S3318P-type T/H", wx::hex(by[0], 2));
    r.channel = ((by[1] >> 4) & 3) + 1;
    r.tempC = fToC(f);
    if (h > 0) r.hum = h;
    r.battOk = (by[4] & 0x40) ? 0 : 1;
    r.confirm = true;   // ★ a 4-bit CRC: shown once heard twice
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  Digitech XC-0324 / Ambient Weather FT005TH — OOK PPM, ~400 µs marks, ~520 µs space = 0, ~1000 µs = 1; three 48-bit
//  messages per row: 0x5F · ID · temperature 12 bits sent LSB first ((raw − 400) / 10 °C) · humidity (LSB first) · XOR
//  of all six bytes = 0.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool xc0324(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : wx::ppm(b.ook, 520, 1000, 2500, 0, 800)) {
        if (row.size() < 96) continue;
        std::vector<std::vector<uint8_t>> ok;
        for (size_t s : wx::findAll(row, 0x5F, 8)) {
            if (s + 48 > row.size()) break;
            const auto by = bytesAt(row, s, 6);
            if (wx::xorBytes(by.data(), 6) != 0) continue;
            ok.push_back(by);
        }
        // ★ an XOR byte: two of the three copies must agree
        for (size_t a = 0; a < ok.size(); ++a)
            for (size_t c = a + 1; c < ok.size(); ++c) {
                if (ok[a] != ok[c]) continue;
                const auto& by = ok[a];
                const int t = (wx::rev8(by[3]) & 0x0F) << 8 | wx::rev8(by[2]);
                Reading r = wx::wxReading("Digitech", "XC-0324 / FT005TH", wx::hex(by[1], 2));
                r.tempC = (t - 400) / 10.0;
                const int h = wx::rev8(by[4]);
                if (!wx::tempOk(r.tempC) || h > 100) continue;
                if (h > 0) r.hum = h;
                out.push_back(r);
                return true;
            }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  HIDEKI (TS04 / Cresta / Bresser 5CH / TFA / Mebus / Sigma) — OOK biphase ("DMC"), 520 µs half-bit; bits inverted;
//  every byte followed by a parity bit (even); after a 9-bit sync (000001101), 7–13 bytes: channel 3 + rolling code 4 ·
//  battery / length 5 · sequence + type · BCD values (LSB-first after the checks) · XOR of all before = the next byte ·
//  CRC-8 (poly 0x07) over everything = 0. Types by length: 8 bytes temperature, 10 T/H, 9 rain (× 0.7 mm), 14 wind.
//  Temperature: BCD 0.1 °C, sign bit 0 = negative. Wind / gust 0.1 mph, direction a 4-bit code.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool hideki(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row0 : wx::dmc(b.ook, 520, 240)) {
        const int total = int((row0.size() + 4) / 9);
        if (total != 8 && total != 9 && total != 10 && total != 14) continue;
        int start = -1;
        for (int k = 0; k < 4; ++k) if (bitsAt(row0, 0, 9 - k) == (0x0Du >> k)) { start = 9 - k; break; }
        if (start < 0) continue;
        const int n = total - 1;
        if (size_t(start + 9 * n) > row0.size() + 1) continue;
        std::vector<uint8_t> p(static_cast<size_t>(n));
        bool ok = true;
        for (int k = 0; k < n && ok; ++k) {
            const size_t at = size_t(start + 9 * k);
            const uint8_t v = uint8_t(~bitsAt(row0, at, 8) & 0xFF);
            const int par = at + 8 < row0.size() ? !row0[at + 8] : 0;
            if (par != wx::parity8(v)) ok = false;
            p[size_t(k)] = v;
        }
        if (!ok || wx::xorBytes(p.data(), size_t(n - 1)) != 0 || crc8(p.data(), size_t(n), 0x07, 0) != 0) continue;
        for (auto& v : p) v = wx::rev8(v);
        if (((p[1] >> 1) & 0x1F) + 2 != n) continue;
        int ch = (p[0] >> 5) & 7; if (ch >= 5) ch -= 1;
        Reading r = wx::wxReading("Hideki", "", wx::hex(p[0] & 0xF, 1));
        r.channel = ch;
        auto temp = [&]() { const int t = (p[4] & 0xF) * 100 + (p[3] >> 4) * 10 + (p[3] & 0xF); return ((p[4] >> 7) & 1 ? 1 : -1) * t / 10.0; };
        if (total == 10) {
            r.model = "TS04 T/H"; r.tempC = temp(); r.battOk = (p[4] >> 6) & 1;
            r.hum = (p[5] >> 4) * 10 + (p[5] & 0xF);
        } else if (total == 8) {
            r.model = "temperature"; r.tempC = temp(); r.battOk = (p[4] >> 6) & 1;
        } else if (total == 9) {
            r.model = "rain"; r.rainMm = (p[4] << 8 | p[3]) * 0.7; r.battOk = (p[1] >> 6) & 1;
        } else {
            static const uint8_t wd[16] = {0, 15, 13, 14, 9, 10, 12, 11, 1, 2, 4, 3, 8, 7, 5, 6};
            r.model = "wind"; r.tempC = temp(); r.battOk = (p[4] >> 6) & 1;
            r.windDir = wd[p[10] >> 4] * 22.5;
            r.windKmh = ((p[8] & 0xF) * 100 + (p[7] >> 4) * 10 + (p[7] & 0xF)) * 0.1 * 1.609344;
            r.gustKmh = ((p[9] >> 4) * 100 + (p[9] & 0xF) * 10 + (p[8] >> 4)) * 0.1 * 1.609344;
        }
        if (!std::isnan(r.tempC) && !wx::tempOk(r.tempC)) continue;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  GLOBALTRONICS GT-WT-02 (Aldi / Lidl) — OOK PPM, ~500 µs marks, ~2–3 ms = 0, ~4–6 ms = 1, ~9–10 ms sync; 37 bits
//  (39 on a later PCB: 2 extra in front): ID · battery · button · channel 2 · temperature 12 bits signed × 0.1 °C ·
//  humidity 7 (10 = "LL", 110 = "HH") · 6-bit sum of the nine nibbles before (the last with its low bit cleared).
//  GT-WT-03 — OOK PWM, ~625 µs = 1 (after inversion), ~256 µs = 0, ~855 µs sync; 41 bits: ID · humidity · battery ·
//  button · channel 2 · temperature 12 bits signed × 0.1 °C · check = a per-byte rolling XOR (key 0x3100 shifted per
//  bit) XOR 0x2D.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool gtwt02(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2250, 4500, 7500, 0, 1200, 0, 150);
    if (rows.size() < 2) return false;
    for (const Bits& w : rows) {
        size_t off = 0;
        if (w.size() == 39) off = 2; else if (w.size() != 37) continue;
        const auto by = bytesAt(w, off, 5);
        if (!(by[0] | by[1] | by[2] | by[3] | by[4])) continue;
        const int sum = (by[0] >> 4) + (by[0] & 0xF) + (by[1] >> 4) + (by[1] & 0xF) + (by[2] >> 4) + (by[2] & 0xF) + (by[3] >> 4) + (by[3] & 0xE);
        if ((sum & 0x3F) != (((by[3] & 1) << 5) | (by[4] >> 3))) continue;
        const int ch = (by[1] >> 4) & 3;
        int t = (by[1] & 0xF) << 8 | by[2]; if (t & 0x800) t -= 0x1000;
        const int h = by[3] >> 1;
        if (ch > 2 || t < -200 || t > 600 || (h != 10 && h != 110 && (h < 20 || h > 90))) continue;
        Reading r = wx::wxReading("Globaltronics", "GT-WT-02", wx::hex(by[0], 2));
        r.channel = ch + 1; r.tempC = t / 10.0; r.hum = h == 10 ? 0 : h == 110 ? 100 : h;
        r.battOk = (by[1] & 0x80) ? 0 : 1;
        r.confirm = true;   // ★ a 6-bit sum
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool gtwt03(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::pwm(b.ook, 256, 625, 1000, 0, 855);
    const int i = wx::repeatedRow(rows, rows.size() > 2 ? 2 : 1, 41, 41);
    if (i < 0) return false;
    const auto by = bytesAt(rows[size_t(i)], 0, 5);
    if (!(by[0] | by[1] | by[2] | by[3] | by[4])) return false;
    uint8_t sum = 0;
    for (int k = 0; k < 4; ++k) { uint16_t key = 0x3100; for (int bit = 7; bit >= 0; --bit) { if ((by[size_t(k)] >> bit) & 1) sum ^= uint8_t(key); key >>= 1; } }
    if ((sum ^ by[4] ^ 0x2D) != 0) return false;
    int t = (by[2] & 0xF) << 8 | by[3]; if (t & 0x800) t -= 0x1000;
    const int h = by[1];
    if (t <= -502 || t >= 702 || (h != 10 && h != 110 && (h < 20 || h > 95))) return false;
    Reading r = wx::wxReading("Globaltronics", "GT-WT-03", wx::hex(by[0], 2));
    r.channel = ((by[2] >> 4) & 3) + 1; r.tempC = t / 10.0; r.hum = h == 10 ? 0 : h == 110 ? 100 : h;
    r.battOk = (by[2] & 0x80) ? 0 : 1;
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WT0122 / WT0124 pool thermometer — OOK PWM, ~680 µs = 1, ~1850 µs = 0, a ~10 ms sync pulse; 49 bits: 5 · ID 8 ·
//  temperature 12 bits ((raw − 0x990) / 10 °C) · flags 2 · channel 2 · F · XOR of bytes 0–3 · end-around-carry sum.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool wt0124(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : wx::pwm(b.ook, 680, 1850, 4000, 1, 10000, 4000)) {
        if (w.size() != 49) continue;
        const auto by = bytesAt(w, 0, 6);
        if ((by[0] >> 4) != 5 || wx::xorBytes(by.data(), 4) != by[4]) continue;
        unsigned sum = wx::addBytes(by.data(), 4); sum += sum >> 8; sum += by[4];
        if ((sum & 0xFF) != by[5]) continue;
        Reading r = wx::wxReading("WT0124", "pool thermometer", wx::hex(unsigned((by[0] & 0xF) << 4 | (by[1] & 0xF)), 2));
        r.tempC = (((by[1] & 0xF) << 8 | by[2]) - 0x990) / 10.0;
        if (!wx::tempOk(r.tempC)) continue;
        r.channel = (by[3] >> 4) & 3;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  COTECH 36-7959 (SwitchDoc FT020T, Sainlogic WS019T / FT0300, Ragova FT-0310, NicetyMeter 0366) — OOK Manchester
//  ("first edge = 0" convention), 500 µs half-bit; after a 12-bit 0x014 preamble, 112 bits: type 4 · ID 8 · battery ·
//  MSBs of direction / gust / wind · wind 0.1 m/s · gust 0.1 m/s · direction° · ? 4 + rain 12 bits (0.1 mm) · flags 4 +
//  temperature 12 bits ((raw − 400) / 10 °F) · humidity · light 16 (+1) lux · UV index · CRC-8 0x31 init 0xC0 = 0.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool cotech(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : wx::mcZero(b.ook, 500, 1200)) {
        if (row.size() < 124) continue;
        for (size_t s : wx::findAll(row, 0x014, 12)) {
            if (s + 12 + 112 > row.size()) break;
            const auto by = bytesAt(row, s + 12, 14);
            if (crc8(by.data(), 14, 0x31, 0xC0) != 0) continue;
            Reading r = wx::wxReading("Cotech", "36-7959 / FT020T / Sainlogic", wx::hex(unsigned((by[0] & 0xF) << 4 | by[1] >> 4), 2));
            r.battOk = (by[1] & 0x08) ? 0 : 1;
            r.windKmh = ((by[1] & 1) << 8 | by[2]) * 0.1 * 3.6;
            r.gustKmh = ((by[1] & 2) << 7 | by[3]) * 0.1 * 3.6;
            r.windDir = (by[1] & 4) << 6 | by[4];
            r.rainMm = ((by[5] & 0xF) << 8 | by[6]) * 0.1;
            r.tempC = fToC((((by[7] & 0xF) << 8 | by[8]) - 400) / 10.0);
            r.hum = by[9];
            if (by[12] <= 150) {
                r.uv = by[12];
                char e[32]; std::snprintf(e, sizeof e, "%d lux", (by[10] << 8 | by[11]) | ((by[7] & 0x80) << 9)); r.extra = e;
            }
            if (!wx::tempOk(r.tempC) || r.hum > 100) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TFA 30.3160 pool thermometer — OOK PPM, ~2 ms space = 0, ~4.6 ms = 1; 28 bits sent ~10 times: checksum 4 (the sum of
//  the next six nibbles − 1) · ID 8 · temperature 12 bits signed × 0.1 °C · channel 2 · battery 1 · first-send 1.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tfaPool(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2000, 4600, 7800, 0, 1500, 0, 150);
    const int i = wx::repeatedRow(rows, 5, 28, 28);
    if (i < 0) return false;
    const Bits& w = rows[size_t(i)];
    unsigned sum = 0; for (int k = 1; k < 7; ++k) sum += bitsAt(w, size_t(4 * k), 4);
    if (((sum - 1) & 0xF) != bitsAt(w, 0, 4)) return false;
    int t = int(bitsAt(w, 12, 12)); if (t > 2048) t -= 4096;
    Reading r = wx::wxReading("TFA Dostmann", "30.3160 pool thermometer", wx::hex(bitsAt(w, 4, 8), 2));
    r.tempC = t / 10.0;
    if (!wx::tempOk(r.tempC, -40, 60)) return false;
    r.channel = int(bitsAt(w, 24, 2));
    r.confirm = true;
    out.push_back(r);
    return true;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (cotech(b, out)) return true;
    if (hideki(b, out)) return true;
    if (gtwt03(b, out)) return true;
    if (wt0124(b, out)) return true;
    if (xc0324(b, out)) return true;
    if (s3318(b, out)) return true;
    if (gtwt02(b, out)) return true;
    if (tfaPool(b, out)) return true;
    return false;
}

} // namespace wx5
} // namespace vibeism
