// vibe_ism_weather8.h — V12 ISM decoder, weather pass 2026-11-11: Globaltronics GT-TMBBQ-05 (Aldi BBQ), TFA Twin Plus
// 30.3049 (Conrad KW9010, Ea2 BL999), Philips AJ7010 / AJ3650 clock-radio outdoor sensors, TFA 30.3196 (868 MHz).
// Sources: rtl_433 device-file field tables (gt_tmbbq05.c, tfa_twin_plus_30.3049.c — itself from iotzo's reverse
// engineering, philips_aj7010.c, philips_aj3650.c, tfa_30_3196.c — documented by Ekkehart Tessmer) for FACTS only.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx8 {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  GT-TMBBQ-05 — OOK PPM, ~2 ms space = 0, ~4 ms = 1; 33 bits × 8: 0 · static 8 · temperature low 8 · static 8 ·
//  temperature high 2 · ? · odd parity over the 7 nibbles before · 4-bit sum of those nibbles. °F + 90, whole degrees.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tmbbq05(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2000, 4000, 4200, 0, 1200, 0, 120);
    const int i = wx::repeatedRow(rows, 4, 33, 33);
    if (i < 0) return false;
    auto by = bytesAt(rows[size_t(i)], 1, 4);
    if (!(by[0] | by[1] | by[2] | by[3])) return false;
    int par = 0; for (int k = 0; k < 3; ++k) par ^= wx::parity8(by[size_t(k)]); par ^= wx::parity8(by[3] & 0xF0);
    if (par != 1) return false;
    if (((wx::addNibbles(by.data(), 3) + (by[3] >> 4)) & 0xF) != (by[3] & 0xF)) return false;
    const int f = (((by[3] & 0xC0) << 2) | by[1]) - 90;
    Reading r = wx::wxReading("Globaltronics", "GT-TMBBQ-05 BBQ", wx::hex(unsigned(by[0] << 8 | by[2]), 4));
    r.kind = "sensor";
    r.tempC = (f - 32) / 1.8;
    r.confirm = true;
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TFA TWIN PLUS 30.3049 / Conrad KW9010 / Ea2 BL999 — OOK PPM, ~2 ms space = 0, ~4 ms = 1; 36 bits, LSB first per
//  byte: ID / channel · battery · temperature 9 bits + 3 sign bits (negative = 512 − raw) · humidity 7 (+ 28) and a 1 ·
//  4-bit sum of the eight nibbles. Shares the AlectoV1 / Prologue timing — runs after them.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool twinPlus(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2000, 4000, 6000, 0, 1200, 0, 120);
    const int i = wx::repeatedRow(rows, 2, 36, 36);
    if (i < 0) return false;
    const auto by = bytesAt(rows[size_t(i)], 0, 5);
    if (!(by[0] | by[1] | by[2] | by[3] | by[4])) return false;
    uint8_t rb[5]; for (int k = 0; k < 5; ++k) rb[k] = wx::rev8(by[size_t(k)]);
    if ((wx::addNibbles(rb, 4) & 0xF) != (rb[4] & 0xF) || !(by[3] & 1)) return false;
    const int neg = by[2] & 7, t = ((rb[2] & 0x1F) << 4) | (rb[1] >> 4), h = (rb[3] & 0x7F) - 28;
    if (neg != 0 && neg != 7) return false;
    const double tc = (neg ? -(512 - t) : t) / 10.0;
    if (!wx::tempOk(tc) || h < 0 || h > 100) return false;
    Reading r = wx::wxReading("TFA Dostmann", "Twin Plus 30.3049 / KW9010", wx::hex(unsigned((rb[0] & 0x0F) | (rb[0] & 0xC0) >> 2), 2));
    r.battOk = (by[1] & 0x80) ? 0 : 1; r.channel = ((by[0] >> 2) & 3) + 1; r.tempC = tc; r.hum = h;
    r.confirm = true;
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  PHILIPS AJ7010 — OOK PWM, ~6 ms mark = 1, ~2 ms = 0 (after inversion), three ~1 ms sync pulses; 40 bits: 0x00 · channel
//  code (0x5A / 0x45 / 0x36 = 1 / 2 / 3) · temperature 14 bits as an ADC value (°C ≈ raw / 353 − 9.2 — ASSUMED, the only
//  published fit) · XOR (of all four, or of the first three on alternate packets).
//  PHILIPS AJ3650 (AJ260 …) — the same PWM; each packet (≈ 29 ms apart, so a burst each here) a 0 separator nibble +
//  32 bits: 0001 · channel (0 = 2, 2 = 1, 4 = 3) · temperature 10 bits ((raw − 500) / 10 °C) · 000000 0 · battery low ·
//  ? · CRC-4 (poly 9, init 1) over all four bytes = 0.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool philips(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : wx::pwm(b.ook, 2000, 6000, 12000, 0, 1000, 9000)) {
        if (w.size() == 40) {
            const auto by = bytesAt(w, 0, 5);
            const int ch = by[1] == 0x5A ? 1 : by[1] == 0x45 ? 2 : by[1] == 0x36 ? 3 : 0;
            if (by[0] == 0 && ch && (wx::xorBytes(by.data(), 5) == 0 || wx::xorBytes(by.data(), 3) == by[4])) {
                Reading r = wx::wxReading("Philips", "AJ7010 outdoor", "");
                r.channel = ch;
                r.tempC = (((by[3] & 0x3F) << 8) | by[2]) / 353.0 - 9.2;
                if (!wx::tempOk(r.tempC)) continue;
                r.confirm = true;
                out.push_back(r);
                return true;
            }
        }
        if (w.size() >= 36 && w.size() <= 40) {
            const auto p = bytesAt(w, w.size() - 32, 4);
            if ((p[0] >> 4) != 1 || (p[2] & 0x3F) || (p[3] & 0x80) || wx::crc4(p.data(), 4, 9, 1) != 0) continue;
            static const int cmap[5] = {2, 0, 1, 0, 3};
            const int c = p[0] & 0xF;
            if (c > 4 || !cmap[c]) continue;
            Reading r = wx::wxReading("Philips", "AJ3650 outdoor", "");
            r.channel = cmap[c];
            r.tempC = ((p[1] << 2 | p[2] >> 6) - 500) / 10.0;
            r.battOk = (p[3] & 0x40) ? 0 : 1;
            if (!wx::tempOk(r.tempC)) continue;
            r.confirm = true;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TFA 30.3196 (868.33 MHz, "Modus Plus") — FSK, Manchester twice: 245 µs half-bits ("first edge = 0" layer), then
//  0x5556 and a second Manchester layer (01 = 1, 10 = 0) giving 48 bits: 0xA8 · ? 2 + channel 2 + temperature 12
//  ((raw − 400) / 10 °C) · battery low + humidity 7 · LFSR-16 digest (generator 0x8810, key 0x22D0) of the four bytes.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tfa303196(const Burst& b, std::vector<Reading>& out) {
    const Runs* src[2] = {&b.fsk, &b.ook};
    for (const Runs* rs : src) {
        if (rs->size() < 100) continue;
        for (int pol = 0; pol < 2; ++pol) {
            Runs rr = *rs; if (pol) for (auto& d : rr) d = -d;
            for (const Bits& row : wx::mcZero(rr, 245, 2000)) {
                if (row.size() < 108) continue;
                for (size_t s : wx::findAll(row, 0x5556, 16)) {
                    const size_t st = s + 12;
                    if (st + 96 > row.size()) break;
                    Bits d;
                    for (size_t k = 0; k < 48; ++k) { const uint8_t a = row[st + 2 * k], c = row[st + 2 * k + 1]; if (a == c) break; d.push_back(c); }
                    if (d.size() < 48) continue;
                    const auto by = bytesAt(d, 0, 6);
                    if (by[0] != 0xA8 || wx::lfsrDigest16(by.data(), 4, 0x8810, 0x22D0) != uint16_t(by[4] << 8 | by[5])) continue;
                    Reading r = wx::wxReading("TFA Dostmann", "30.3196 T/H", "");
                    r.channel = (by[1] >> 4) + 1;   // as the field table counts it (the top two bits are unknown)
                    r.tempC = (((by[1] & 0xF) << 8 | by[2]) - 400) / 10.0;
                    r.battOk = (by[3] & 0x80) ? 0 : 1;
                    r.hum = by[3] & 0x7F;
                    if (!wx::tempOk(r.tempC) || r.hum > 100) continue;
                    out.push_back(r);
                    return true;
                }
            }
        }
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (tfa303196(b, out)) return true;
    if (philips(b, out)) return true;
    if (tmbbq05(b, out)) return true;
    return false;
}

} // namespace wx8
} // namespace vibeism
