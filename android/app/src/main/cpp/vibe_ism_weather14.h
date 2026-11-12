// vibe_ism_weather14.h — V12 ISM decoder, weather pass 2 (2026-11-12): AOK / Holman iWeather WS5029 (FSK PCM and
// PWM), TS-FT002 tank level + temperature, Emos TTX201 / IROX ETS69, Hyundai WS SENZOR.
// Sources: the makers' product pages named per family and the protocol notes at the head of rtl_433's
// holman_ws5029.c, ts_ft002.c, ttx201.c, wssensor.c — FACTS only (layout, timing, check, scaling); our own code on our
// own slicers (vibe_ism_wxkit.h). Scored on the rtl_433_tests recordings (scripts/ism-corpus.mjs).
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx14 {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AOK ELECTRONIC station (Holman iWeather WS5029, Conrad Renkforce AOK-5056, Optex 990018), FSK PCM form — 917 MHz,
//  100 µs bits, three copies 1 ms apart: AA × 6, header 98 F3 A5, then ID 16 · temperature 12 (signed, / 10 °C) ·
//  humidity 8 · rain 12 · wind 8 (km/h) · direction 4 (× 22.5°) · … · byte 12 = a digest of the XOR of bytes 0–11
//  (each 1 bit of that XOR at position i contributes 0x31 << i, kept to 8 bits). The model without the UV / light
//  sensor counts rain in 0.79 mm; the one with it in 1 mm, and carries UV 4 (bits 2–0 of byte 7 + bit 7 of byte 8),
//  light 17 (lux), a battery pair and a 12-bit counter in bytes 10–11 (zero on the other model).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint8_t aokDigest(uint8_t x) { uint8_t s = 0; for (int i = 0; i < 8; ++i) if ((x >> i) & 1) s ^= uint8_t(0x31 << i); return s; }
inline bool aokPcm(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty() && b.ook.size() < 40) return false;
    // ★ up to 80 bits of one tone: the zero bytes 8–11 of the model without light run longer than the usual row limit
    for (const Bits& row : wx::fskRows(b, 100, 80, 150)) {
        for (size_t s : wx::findAll(row, 0xAA98F3A5u, 32)) {
            const size_t at = s + 32;
            if (at + 13 * 8 > row.size() + 8) break;
            const auto m = bytesAt(row, at, 13);
            if (aokDigest(uint8_t(wx::xorBytes(m.data(), 12))) != m[12]) continue;
            if ((m[0] | m[1] | m[2] | m[3]) == 0) continue;
            const int lux = (m[8] & 0x7F) << 10 | m[9] << 2 | m[10] >> 6;
            // ★ told apart by content, not row length (a row's end depends on the gap): only the UV model fills light,
            //   battery and its 12-bit counter (bytes 8–11); the other leaves them 0
            const bool uvModel = lux != 0 || (m[10] & 0x3F) != 0 || m[11] != 0;
            Reading r = wx::wxReading("AOK", uvModel ? "WS5029-style station (UV / light)" : "Holman WS5029 / AOK-5056 station",
                                      wx::hex(unsigned(m[0] << 8 | m[1]), 4));
            r.tempC = (int16_t(m[2] << 8 | (m[3] & 0xF0)) >> 4) / 10.0;
            const int h = (m[3] & 0xF) << 4 | m[4] >> 4;
            if (h > 100 || !wx::tempOk(r.tempC)) continue;
            r.hum = h;
            r.rainMm = ((m[4] & 0xF) << 8 | m[5]) * (uvModel ? 1.0 : 0.79);
            r.windKmh = m[6];
            r.windDir = (m[7] >> 4) * 22.5;
            if (uvModel) {
                r.uv = (m[7] & 7) << 1 | m[8] >> 7;
                char e[32]; std::snprintf(e, sizeof e, "%d lux", lux); r.extra = e;
                r.battOk = ((m[10] >> 4) & 3) ? 0 : 1;
            }
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  HOLMAN WS5029, PWM form — 433.92 MHz FSK, each bit one tone ~488 µs or ~976 µs (the other tone the rest), 96 bits
//  sent three times; INVERTED: AA A5 98 · ID · battery low (bit 7) + temperature 12 (signed, / 10 °C) · humidity % ·
//  rain 12 (× 0.79 mm) · wind 8 (km/h) · direction 4 (× 22.5°) · check · 00 / 80. The check: XOR the even bytes
//  (0, 2, … 8) into A, the odd ones into B; check = A ^ B ^ (A << 4) ^ (A << 5) (the notes' "shift key 0x18").
//  The two tones are often heard as two OOK channels (their spacing is wide): each is read, in both senses.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint8_t holmanCheck(const uint8_t* d, int n) {
    uint8_t a = 0, c = 0;
    for (int i = 0; i < n; ++i) (i & 1 ? c : a) ^= d[i];
    return uint8_t(a ^ c ^ uint8_t(a << 4) ^ uint8_t(a << 5));
}
inline bool holmanPwm(const Burst& b, std::vector<Reading>& out) {
    const Runs* src[2] = {&b.ook, &b.fsk};
    for (const Runs* rs : src) {
        if (rs->size() < 150) continue;
        for (int sense = 0; sense < 2; ++sense) {
            const auto rows = wx::pwm(*rs, 488, 976, 2000, sense);
            const int i = wx::repeatedRow(rows, 2, 95, 96);   // (the trailing 00 / 80 byte may lose its last bit)
            if (i < 0) continue;
            const auto m = bytesAt(rows[size_t(i)], 0, 12);
            if (m[0] != 0xAA || m[1] != 0xA5 || m[2] != 0x98 || holmanCheck(m.data(), 10) != m[10]) continue;
            Reading r = wx::wxReading("Holman", "WS5029 station", wx::hex(m[3], 2));
            r.battOk = (m[4] & 0x80) ? 0 : 1;
            r.tempC = (int16_t((m[4] & 0xF) << 12 | m[5] << 4) >> 4) / 10.0;
            if (m[6] > 100 || !wx::tempOk(r.tempC)) continue;
            r.hum = m[6];
            r.rainMm = (m[7] << 4 | m[8] >> 4) * 0.79;
            r.windKmh = (m[8] & 0xF) << 4 | m[9] >> 4;
            r.windDir = (m[9] & 0xF) * 22.5;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TS-FT002 ultrasonic tank level + temperature — OOK PPM: ~500 µs marks, 464 µs space = 0, 948 µs = 1, ~1.9 ms between
//  the two packets; 9 bytes sent LSB first: sync 0xFA · ID · type 0x11 · depth 12 (cm; 0x5DC = no echo) · battery 4 ·
//  interval 4 · temperature 12 ((raw − 400) / 10 °C; 0x3E8 = none) · rain (unused) · XOR of all nine = 0.
//  Nibble order after reflecting each byte: depth H M (byte 3) L (low nibble of byte 4); temperature H M (byte 6) L (low
//  nibble of byte 5).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tsft002(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& raw : wx::ppm(b.ook, 464, 948, 1300, 0, 800, 250, 120)) {
        if (raw.size() < 70) continue;
        // ★ the sync's leading 0 (or 01) is often lost: it follows the long packet gap, where the slicer has no mark yet
      for (int lost = 0; lost < 3; ++lost) {
        Bits row = raw;
        if (lost == 1) row.insert(row.begin(), uint8_t(0));
        if (lost == 2) { row.insert(row.begin(), uint8_t(1)); row.insert(row.begin(), uint8_t(0)); }
        for (size_t s : wx::findAll(row, 0x5F, 8)) {
            if (s + 72 > row.size() + 2) break;
            auto m = bytesAt(row, s, 9);
            if (wx::xorBytes(m.data(), 9) != 0) continue;
            for (auto& v : m) v = wx::rev8(v);
            if (m[2] != 0x11) continue;
            const int depth = m[3] << 4 | (m[4] & 0xF), traw = m[6] << 4 | (m[5] & 0xF);
            Reading r;
            r.kind = "sensor"; r.make = "TS-FT002"; r.model = "ultrasonic tank level"; r.pubId = wx::hex(m[1], 2);
            if (traw != 0x3E8) { r.tempC = (traw - 400) / 10.0; if (!wx::tempOk(r.tempC)) continue; }
            if (depth != 0x5DC) { char e[32]; std::snprintf(e, sizeof e, "depth %d cm", depth); r.extra = e; r.fact("depth_cm", (long long)depth); }
            else r.note = "no echo";
            r.fact("id", (long long)m[1]);
            out.push_back(r);
            return true;
        }
      }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  EMOS TTX201 (Ewig TTX201M, FCC N9ZTTX201M) / IROX ETS69 — OOK Manchester (first edge = 0), 500 µs half-bits, 1.5 ms
//  between packets: a 17-bit zero preamble, then 8 packets of 54 bits (the last 50): 4 zero bits · 6-bit sum of nibbles
//  3–12 · ID 8 · startup · data type 3 (0 temperature, 5 date/time) · battery low · channel 3 (0-based) · 0 · packet
//  index 3 · temperature 12 (signed, / 10 °C) · postmark 8 (0x14 TTX201, 0x00 ETS69) · F. Taken from two packets that
//  agree (each with a good sum).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool ttx201(const Burst& b, std::vector<Reading>& out) {
    std::vector<std::vector<uint8_t>> good;
    for (const Bits& row : wx::mcZero(b.ook, 510, 1300)) {
        if (row.size() < 49 || row.size() > 55) continue;
        // ★ the packet's leading zero bits are where the slicer's first-edge rule lands: one may be lost or gained, so
        //   the 6-bit sum is looked for two, one or three bits in
        for (size_t at : {size_t(2), size_t(1), size_t(3)}) {
            const auto m = bytesAt(row, at, 7);   // LL KKKKKK · ID · flags · index + T · T · postmark
            const unsigned sum = wx::addNibbles(m.data() + 1, 5);
            if (sum == 0 || (m[0] & 0x3F) != (sum & 0x3F) || (m[0] & 0xC0)) continue;
            if (m[5] != 0x14 && m[5] != 0x00) continue;
            if (((m[2] >> 4) & 7) != 0) continue;   // temperature packets only (the hourly clock packet is not read)
            if (good.size() < 16) good.push_back(m);
            break;
        }
    }
    for (size_t i = 0; i < good.size(); ++i)
        for (size_t j = i + 1; j < good.size(); ++j) {
            const auto& m = good[i]; const auto& o = good[j];
            if (m[1] != o[1] || m[2] != o[2] || (m[3] & 0xF) != (o[3] & 0xF) || m[4] != o[4]) continue;
            Reading r = wx::wxReading("Emos", m[5] == 0x14 ? "TTX201" : "ETS69 (IROX)", wx::hex(m[1], 2));
            r.battOk = (m[2] & 0x08) ? 0 : 1;
            r.channel = (m[2] & 7) + 1;
            r.tempC = (int16_t((m[3] & 0xF) << 12 | m[4] << 4) >> 4) / 10.0;
            if (!wx::tempOk(r.tempC)) continue;
            out.push_back(r);
            return true;
        }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  HYUNDAI WS SENZOR — OOK PPM: ~224 µs marks, 1032 µs space = 0, 1992 µs = 1, ~4 ms between; 24 bits sent 23 times:
//  temperature 12 (signed, / 10 °C) · battery OK · startup · channel 2 (0-based) · ID 8. No check: 4 identical rows,
//  never all 0 / all 1, and confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool wsSenzor(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 1032, 1992, 3000, 0, 600, 100, 80);
    const int i = wx::repeatedRow(rows, 4, 24, 24);
    if (i < 0) return false;
    const auto m = bytesAt(rows[size_t(i)], 0, 3);
    if ((m[0] | m[1] | m[2]) == 0 || (m[0] & m[1] & m[2]) == 0xFF || (m[1] & 3) == 3) return false;
    Reading r = wx::wxReading("Hyundai", "WS SENZOR", wx::hex(m[2], 2));
    r.tempC = (int16_t(m[0] << 8 | (m[1] & 0xF0)) >> 4) / 10.0;
    if (!wx::tempOk(r.tempC)) return false;
    r.battOk = (m[1] & 0x08) ? 1 : 0;
    r.channel = (m[1] & 3) + 1;
    r.confirm = true;
    out.push_back(r);
    return true;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (aokPcm(b, out)) return true;
    if (holmanPwm(b, out)) return true;
    if (tsft002(b, out)) return true;
    if (ttx201(b, out)) return true;
    return false;
}
inline bool decodeWeak(const Burst& b, std::vector<Reading>& out) {
    if (wsSenzor(b, out)) return true;
    return false;
}

} // namespace wx14
} // namespace vibeism
