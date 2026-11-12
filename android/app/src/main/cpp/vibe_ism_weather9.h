// vibe_ism_weather9.h — V12 ISM decoder, weather pass 2026-11-11: Eurochron EFTH-800, ThermoPro TP11 / TP08 / TP12 /
// TP20 (BBQ / meat), Rubicson (Kjell & Co; TFA 30.3197; inFactory ZX-7382 / PT-310), Bresser 3CH (Renkforce DM-7511),
// WT450 (WT260H / WT405H).
// Sources: Jaakko Ala-Paavola's WT450H page (ala-paavola.fi/jaakko/doku.php?id=wt450h) for the WT450; rtl_433
// device-file field tables (efth800.c, thermopro_tp11.c, thermopro_tp12.c, rubicson.c, bresser_3ch.c, wt450.c) for
// FACTS only. Our own code throughout.
#pragma once
#include "vibe_ism_wxkit.h"
#include <array>

namespace vibeism {
namespace wx9 {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  EUROCHRON EFTH-800 — OOK PWM, ~500 µs mark = 1, ~250 µs = 0 (after inversion), four ~750 µs sync pulses; 48 bits,
//  two copies: ? + channel 3 · ID 12 · battery low · ? · temperature 10 bits signed × 0.1 °C · ? 4 · humidity 2 BCD ·
//  CRC-8 0x31 over all six = 0.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool efth800(const Burst& b, std::vector<Reading>& out) {
    /* ★ weather pass 2 (2026-11-12): a strong close sensor's marks come out stretched by ~190 µs (the corpus: 440 / 650
     *  µs, syncs 880, the period unchanged at ~730 µs) — every mark then sat above the nominal midpoint and read as a 1.
     *  The stretched timing is tried second; the CRC decides. */
  for (const auto& tm : {std::array<double, 3>{250, 500, 750}, std::array<double, 3>{440, 650, 880}})
    for (const Bits& w : wx::pwm(b.ook, tm[0], tm[1], 900, 0, tm[2])) {
        if (w.size() != 48 && w.size() != 49) continue;   // (a trailing bit is common)
        const auto by = bytesAt(w, 0, 6);
        if (crc8(by.data(), 6, 0x31, 0) != 0 || (!by[0] && !by[1] && !by[2] && !by[4])) continue;
        int t = (by[2] & 0x3F) << 4 | by[3] >> 4; if (t & 0x200) t -= 0x400;
        const int h1 = by[4] >> 4, h0 = by[4] & 0xF;
        if (h1 > 9 || h0 > 9) continue;
        Reading r = wx::wxReading("Eurochron", "EFTH-800", wx::hex(unsigned((by[0] & 0xF) << 8 | by[1]), 3));
        r.channel = ((by[0] >> 4) & 7) + 1; r.battOk = (by[2] & 0x80) ? 0 : 1; r.tempC = t / 10.0; r.hum = h1 * 10 + h0;
        if (!wx::tempOk(r.tempC)) continue;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THERMOPRO TP11 — OOK PPM, ~500 µs space = 0, ~1500 µs = 1; 32 bits, repeated: ID 12 · temperature 12 ((raw − 200) /
//  10 °C) · reflected LFSR digest (generator 0x51, key 0x04) of the three bytes.
//  TP08 / TP12 / TP20 (two probes) — 41 bits: ID · probe 1 low 8 · the two high nibbles · probe 2 low 8 · the digest of
//  the four bytes.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool thermopro(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 500, 1500, 2000, 0, 900);
    for (const Bits& w : rows) {
        if (w.size() == 41) {
            const auto by = bytesAt(w, 0, 5);
            if (!(by[1] | by[2] | by[3]) || wx::lfsrDigest8Refl(by.data(), 4, 0x51, 0x04) != by[4]) continue;   // both probes raw 0: a frame of nothing
            // ★ an 8-bit digest: the copies must agree (the corpus: a 345 MHz Honeywell read once as a TP12)
            int same = 0; for (const Bits& o : rows) if (o.size() >= 40 && bitsAt(o, 0, 32) == bitsAt(w, 0, 32) && bitsAt(o, 32, 8) == bitsAt(w, 32, 8)) ++same;
            if (same < 2) continue;
            Reading r = wx::wxReading("ThermoPro", "TP08 / TP12 / TP20", wx::hex(by[0], 2));
            r.kind = "sensor";
            r.tempC = ((((by[2] & 0xF0) << 4) | by[1]) - 200) / 10.0;
            const double t2 = ((((by[2] & 0x0F) << 8) | by[3]) - 200) / 10.0;
            char e[32]; std::snprintf(e, sizeof e, "probe 2 %.1f °C", t2); r.extra = e;
            out.push_back(r);
            return true;
        }
        if (w.size() == 32 || w.size() == 33) {
            const auto by = bytesAt(w, 0, 4);
            if ((!(by[0] | by[1] | by[2] | by[3])) || (by[0] & by[1] & by[2] & by[3]) == 0xFF) continue;
            if (wx::lfsrDigest8Refl(by.data(), 3, 0x51, 0x04) != by[3]) continue;
            int same = 0; for (const Bits& o : rows) if (o.size() >= 32 && bitsAt(o, 0, 32) == bitsAt(w, 0, 32)) ++same;
            if (same < 2) continue;
            Reading r = wx::wxReading("ThermoPro", "TP11", wx::hex(unsigned(by[0] << 4 | by[1] >> 4), 3));
            r.kind = "sensor";
            r.tempC = (((by[1] & 0xF) << 8 | by[2]) - 200) / 10.0;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  RUBICSON / TFA 30.3197 / inFactory — OOK PPM, ~500 µs marks, ~1 ms = 0, ~2 ms = 1; 36 bits × 12 (the pool sensor
//  37–38): ID · battery OK · 0 · channel 2 · temperature 12 bits signed × 0.1 °C · F · CRC-8 (0x31, init 0x6C) over the
//  nibbles with the F padded. (The Nexus family shares the timing and length, without a check — this runs first.)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool rubicson(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 1000, 2000, 3000, 0, 1000, 0, 100);
    for (const Bits& w : rows) {
        if (w.size() < 36 || w.size() > 38) continue;
        const auto by = bytesAt(w, 0, 5);
        if ((by[3] & 0xF0) != 0xF0) continue;
        const uint8_t t5[5] = {by[0], by[1], by[2], uint8_t(by[3] & 0xF0), uint8_t((by[3] & 0x0F) << 4 | by[4] >> 4)};
        if (crc8(t5, 5, 0x31, 0x6C) != 0) continue;
        int same = 0; for (const Bits& o : rows) if (o.size() >= 36 && bitsAt(o, 0, 32) == bitsAt(w, 0, 32) && bitsAt(o, 32, 4) == bitsAt(w, 32, 4)) ++same;
        if (same < 2) continue;
        int t = (by[1] & 0xF) << 8 | by[2]; if (t & 0x800) t -= 0x1000;
        Reading r = wx::wxReading("Rubicson / TFA / inFactory", "temperature", wx::hex(by[0], 2));
        r.battOk = (by[1] & 0x80) ? 1 : 0; r.channel = ((by[1] >> 4) & 3) + 1; r.tempC = t / 10.0;
        if (!wx::tempOk(r.tempC)) continue;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  BRESSER THERMO-/HYGRO-SENSOR 3CH (Renkforce DM-7511) — OOK PWM, ~500 µs = 1, ~250 µs = 0, 4 × 750 µs sync; 40 bits
//  × 15, inverted: ID · battery low · ? · channel 2 · temperature 12 ((raw − 900) / 10 °F) · humidity · byte sum.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool bresser3ch(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::pwm(b.ook, 250, 500, 625, 1, 750);
    const int i = wx::repeatedRow(rows, 3, 40, 42);
    if (i < 0) return false;
    auto by = bytesAt(rows[size_t(i)], 0, 5);
    for (auto& v : by) v = uint8_t(~v);
    if (((by[0] + by[1] + by[2] + by[3] - by[4]) & 0xFF) != 0) return false;
    const int ch = (by[1] >> 4) & 3;
    const double f = (((by[1] & 0xF) << 8 | by[2]) - 900) / 10.0;
    if (ch == 0 || by[3] > 100 || f < -20 || f > 160) return false;
    Reading r = wx::wxReading("Bresser", "Thermo-/Hygro-Sensor 3CH", wx::hex(by[0], 2));
    r.channel = ch; r.battOk = (by[1] & 0x80) ? 0 : 1; r.tempC = (f - 32) / 1.8; r.hum = by[3];
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WT450 / WT260H / WT405H — OOK biphase, 2 ms bit (a level change mid-bit = 1); 36 bits: 1100 · house code 4 ·
//  channel 2 · 2 · battery low · humidity 7 · temperature 8 + 4 (whole − 50, sixteenths) · sequence 2 · even-bit
//  parity · odd-bit parity.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool wt450(const Burst& b, std::vector<Reading>& out) {
    for (Bits w : wx::dmc(b.ook, 976, 250)) {
        if (w.size() == 35) w.push_back(0);   // ★ a last 0 sent at the low level is a space that runs into the gap: invisible
        if (w.size() != 36 || bitsAt(w, 0, 4) != 0xC) continue;
        const auto by = bytesAt(w, 0, 5);
        unsigned p = wx::xorBytes(by.data(), 5); p ^= p >> 4; p ^= p >> 2;
        if (p & 3) continue;
        const int h = ((by[1] & 7) << 4) | by[2] >> 4;
        const double t = (((by[2] << 4) | (by[3] >> 4)) & 0xFF) - 50.0 + (by[3] & 0xF) / 16.0;
        if (h > 100 || t < -35 || t > 75) continue;
        Reading r = wx::wxReading("WT450-type", "WT450 / WT260H / WT405H T/H", wx::hex(by[0] & 0xF, 1));
        r.channel = (by[1] >> 6) + 1; r.battOk = (by[1] & 8) ? 0 : 1; r.tempC = t; r.hum = h;
        r.confirm = true;   // ★ two parity bits
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (efth800(b, out)) return true;
    if (thermopro(b, out)) return true;
    if (rubicson(b, out)) return true;
    if (wt450(b, out)) return true;
    // (bresser3ch: called after the La Crosse TX141, whose PWM it shares — see decodeOne)
    return false;
}

} // namespace wx9
} // namespace vibeism
