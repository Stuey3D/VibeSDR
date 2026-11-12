// vibe_ism_weather15.h — V12 ISM decoder, weather pass 2 (2026-11-12): inFactory / nor-tec / DAY T/H, Esun EN2053 and
// EN2058 BBQ, TFA Marbella pool, Fuzhou Emax W6 family, Rubicson 48942 pool; and the unchecked OOK ones — Eurochron T/H, Baldr E0666TH,
// Missil ML0757.
// Sources: the makers' pages / FCC IDs named per family and the protocol notes at the head of rtl_433's infactory.c,
// esun_en2053.c (issue #1478), en2058.c, rubicson_pool_48942.c, tfa_marbella.c (TFA's 30.3238.06 manual), emax.c (issues #2000 … #3463),
// eurochron.c, baldr_therm.c, missil_ml0757.c — FACTS only (layout, timing, check, scaling); our own code on our own
// slicers (vibe_ism_wxkit.h). Scored on the rtl_433_tests recordings (scripts/ism-corpus.mjs).
#pragma once
#include "vibe_ism_wxkit.h"
#include <array>

namespace vibeism {
namespace wx15 {

inline double fToC(double f) { return (f - 32) / 1.8; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  inFACTORY T/H (NC-3982-913 / NX-5817-902 / NC-3982-675, nor-tec 73383, DAY 73365, FreeTec, Pearl FWS-686) — OOK
//  PPM: ~500 µs marks, ~2 ms space = 0, ~4 ms = 1, a ~8 ms sync before and ~16 ms after; 40 bits (some units 42), six
//  times a minute: ID 8 · CRC-4 · button · battery low · 2 spare · temperature 12 ((raw − 900) / 10 °F) · humidity 2 BCD
//  digits (A0 = 100) · 2 spare · channel 2 (1–3). The CRC-4 (x⁴ + x + 1, init 0) runs over bytes 0–3 with the channel
//  bits in the CRC's place, then the last nibble (humidity units) is XORed in.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool infactory(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2000, 4000, 6000, 0, 1200, 200, 120);
    /* ★ a 4-bit CRC on a line code the Auriol AHFL / Vauno share exactly (2 / 4 ms, 8 ms sync, 16 ms gap): the burst is
     *  claimed only when every data-length row in it is the same frame, at least twice — a burst that also holds rows of
     *  another device (the corpus: an Alecto WS-1200 behind one) is left alone */
    const Bits* first = nullptr;
    int copies = 0;
    for (const Bits& w : rows) {
        if (w.size() < 24) continue;
        if (w.size() > 42 || w.size() < 40) return false;
        if (!first) first = &w;
        else if (!std::equal(w.begin(), w.begin() + 40, first->begin())) return false;
        ++copies;
    }
    if (copies < 2) return false;
    for (const Bits& w : rows) {
        if (w.size() < 40 || w.size() > 42) continue;
        const auto m = bytesAt(w, 0, 5);
        const int ch = m[4] & 3;
        if (!ch) continue;
        uint8_t c[4] = {m[0], uint8_t((m[1] & 0x0F) | (m[4] & 0x0F) << 4), m[2], m[3]};
        if (uint8_t(wx::crc4(c, 4, 0x3, 0) ^ (m[4] >> 4)) != (m[1] >> 4)) continue;
        const int hum = (m[3] & 0xF) * 10 + (m[4] >> 4);
        const double tf = ((m[2] << 4 | m[3] >> 4) - 900) / 10.0;
        if (hum > 100 || tf < -40 || tf > 158) continue;
        Reading r = wx::wxReading("inFactory", "T/H (NC-3982 / nor-tec / DAY)", wx::hex(m[0], 2));
        r.channel = ch; r.battOk = (m[1] & 0x04) ? 0 : 1;
        r.tempC = fToC(tf); r.hum = hum;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ESUN EN2053 two-probe BBQ (FCC 2APN2-EN2053) — OOK PPM: ~436 µs marks, 1024 µs = 0, 2000 µs = 1, ~3.95 ms between 9
//  copies; 40 bits: 0xC0 · probe 1 12 · probe 2 12 (whole °F; 0xFD6 = no probe) · check: bits 0–2 the sum of bytes 0–3
//  mod 8, bit 3 0, bits 4–7 set where byte 0 / 1 / 2 / 3 has an even number of 1 bits. Two copies alike.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool esun(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 1024, 2000, 3000, 0, 900, 150, 120);
    const int i = wx::repeatedRow(rows, 2, 40, 40);
    if (i < 0) return false;
    const auto m = bytesAt(rows[size_t(i)], 0, 5);
    if (m[0] != 0xC0) return false;
    unsigned c = (m[0] + m[1] + m[2] + m[3]) & 7;
    for (int k = 0; k < 4; ++k) c |= unsigned(1 ^ wx::parity8(m[size_t(k)])) << (4 + k);
    if (c != m[4]) return false;
    const int p1 = m[1] << 4 | m[2] >> 4, p2 = (m[2] & 0xF) << 8 | m[3];
    Reading r = wx::wxReading("Esun", "EN2053 BBQ", "");
    if (p1 != 0xFD6) { r.tempC = fToC(p1); r.fact("temperature_1_F", (long long)p1); }
    if (p2 != 0xFD6) { char e[40]; std::snprintf(e, sizeof e, "probe 2 %.1f °C", fToC(p2)); r.extra = e; r.fact("temperature_2_F", (long long)p2); }
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  EN2058 four-probe BBQ — FSK NRZ 100 µs; 15 ones, 15 zeros, AA AA AA AA AA CA CA, then ID 24 · four temperatures 16
//  ((raw − 900) / 10 °F) · AA · check = 0x56 + the ID's bytes + the temperatures' bytes · a counting filler; nine times
//  back to back.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool en2058(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty() && b.ook.size() < 100) return false;
    for (const Bits& row : wx::fskRows(b, 100, 40, 150)) {
        for (size_t s : wx::findAll(row, 0xAAAACACAu, 32)) {
            if (s + 32 + 13 * 8 > row.size()) break;
            const auto m = bytesAt(row, s + 32, 13);
            if (m[11] != 0xAA || uint8_t(0x56 + wx::addBytes(m.data(), 11)) != m[12]) continue;
            Reading r = wx::wxReading("EN2058", "four-probe BBQ", wx::hex(unsigned(m[0] << 16 | m[1] << 8 | m[2]), 6));
            double t[4];
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
                const int raw = m[size_t(3 + 2 * k)] << 8 | m[size_t(4 + 2 * k)];
                t[k] = (raw - 900) / 10.0;
                if (raw >= 0xFF && (t[k] < -4 || t[k] > 572)) ok = false;
            }
            if (!ok) continue;
            r.tempC = fToC(t[0]);
            char e[80]; std::snprintf(e, sizeof e, "probes 2–4 %.1f / %.1f / %.1f °C", fToC(t[1]), fToC(t[2]), fToC(t[3])); r.extra = e;
            r.fact("temperature_1_F", t[0]); r.fact("temperature_2_F", t[1]); r.fact("temperature_3_F", t[2]); r.fact("temperature_4_F", t[3]);
            r.fact("id", (long long)(m[0] << 16 | m[1] << 8 | m[2]));
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TFA DOSTMANN MARBELLA 30.3238.06 pool thermometer (868 MHz) — FSK NRZ 105 µs, AA 2D D4, then serial 24 · flags
//  (bit 7 battery low, bits 3–1 a rolling counter) · temperature 12 ((raw − 400) / 10 °C) · A · AA · a reflected LFSR
//  digest (generator 0x31, key 0x31) of the seven bytes from the serial.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool marbella(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty() && b.ook.size() < 40) return false;
    for (const Bits& row : wx::fskRows(b, 105, 40, 80)) {
        for (size_t s : wx::findAll(row, 0xAA2DD4, 24)) {
            if (s + 24 + 8 * 8 > row.size() + 4) break;
            const auto m = bytesAt(row, s + 24, 8);
            if (m[6] != 0xAA || (m[5] & 0xF) != 0xA || wx::lfsrDigest8Refl(m.data(), 7, 0x31, 0x31) != m[7]) continue;
            Reading r = wx::wxReading("TFA Dostmann", "Marbella 30.3238 pool", wx::hex(unsigned(m[0] << 16 | m[1] << 8 | m[2]), 6));
            r.battOk = (m[3] & 0x80) ? 0 : 1;
            r.tempC = ((m[4] << 4 | m[5] >> 4) - 400) / 10.0;
            if (!wx::tempOk(r.tempC, -40, 80)) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FUZHOU EMAX W6 family (Emax WEC-W6 / EM3390W6 / EM3551H, Altronics X7063 / X7064 / X7064A, Optex 990040 / 990050 /
//  SM-040, inFactory FWS-1200, Newentor Q9, Otio 810025, Orium, Protmex PT3390A, Jula Marquant, TechniSat IMETEO X6,
//  Auriol HG11911, La Crosse WS6262) — FSK NRZ 90 µs; AA AA CA CA 54, then 32 bytes, the last the byte sum of the 31
//  before (the X7064A: the sum − 0x9A). Byte 1: kind 4 (A = T/H sensor, 0 = the rain / wind station) · channel 4; ID 12
//  · flags 4 (battery low, pairing, subtype 2). T/H sensor: temperature 12 in a split layout (low nibble of byte 4,
//  high nibble of byte 5, low nibble of byte 6) — subtype 1: (raw − 900) / 10 °F, subtype 3: (raw − 500) / 10 °C ·
//  humidity byte 7. Station: temperature 12 (low nibble of 4, byte 5; °F as above) · humidity byte 6 · wind 16, direction
//  12, rain 16 — each byte stored + 1 (0x01 = 0) — wind and rain × 0.2 (km/h, mm). UV / light / gust are not read.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool emax(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty() && b.ook.size() < 100) return false;
    for (const Bits& row : wx::fskRows(b, 90, 40, 200)) {
        for (size_t s : wx::findAll(row, 0xAACACA54u, 32)) {
            if (s + 32 + 32 * 8 > row.size() + 8) break;
            const auto m = bytesAt(row, s + 32, 32);
            const int kind = m[1] >> 4, sub = m[3] & 3;
            unsigned sum = wx::addBytes(m.data(), 31);
            if (kind != 0 && sub == 3) sum -= 0x9A;
            if ((sum & 0xFF) != m[31]) continue;
            Reading r = wx::wxReading("Emax", kind ? "W6-family T/H sensor" : "W6-family station", wx::hex(unsigned(m[2] << 4 | m[3] >> 4), 3));
            r.channel = m[1] & 0xF;
            r.battOk = (m[3] & 0x08) ? 0 : 1;
            if (m[3] & 0x04) r.note = "pairing";
            if (kind) {
                const int raw = (m[4] & 0xF) << 8 | (m[5] & 0xF0) | (m[6] & 0xF);
                if (sub == 1) r.tempC = fToC((raw - 900) / 10.0);
                else if (sub == 3) r.tempC = (raw - 500) / 10.0;
                if (m[7] <= 100) r.hum = m[7];
            } else {
                r.tempC = fToC((((m[4] & 0xF) << 8 | m[5]) - 900) / 10.0);
                if (m[6] <= 100) r.hum = m[6];
                const auto less1 = [&](int i) { return (m[size_t(i)] - 1) & 0xFF; };
                r.windKmh = (less1(7) << 8 | less1(8)) * 0.2;
                const int dir = (less1(9) & 0xF) << 8 | less1(10);
                if (dir < 360) r.windDir = dir;
                r.rainMm = (less1(11) << 8 | less1(12)) * 0.2;
            }
            if (r.tempC == r.tempC && !wx::tempOk(r.tempC)) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  The unchecked OOK PPM ones (~1 ms = 0, ~2 ms = 1): tried after every family with a check, each with its own fixed
//  bits, several copies alike, and confirm.
//  EUROCHRON T/H — 36 bits, ≥ 3 alike: ID 8 · battery low · 0 0 · button · 0000 · humidity % · temperature 12 (signed,
//  / 10 °C).
//  BALDR E0666TH — 64 bits × 8 (≥ 4 alike): ID 8 · battery OK · 0 · channel 2 · temperature 12 (signed, / 10 °C) · F ·
//  humidity 8 · 0000 · 0 · startup + 000 · ID 8.
//  MISSIL ML0757 — 40 bits × 9 (≥ 5 alike): ID 8 · flags (bit 7 battery low, bit 2 = a rain + wind packet) · field 12 ·
//  field 8 · 1111. Temperature packet: field 12 = signed °C × 10. Rain + wind: field 12 = 0.45 mm steps, field 8 = wind
//  (0x00 → 0, 0x80 → 1.4, 0xC0 → 2.8, else (v + 2) × 1.4 km/h — the notes' measured table).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline double s12(int v) { return (v & 0x800) ? v - 0x1000 : v; }
inline bool eurochron(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 1016, 2024, 3000, 0, 900, 150, 120);
    const int i = wx::repeatedRow(rows, 3, 36, 36);
    if (i < 0) return false;
    const auto m = bytesAt(rows[size_t(i)], 0, 5);
    if ((m[1] & 0x6F) || m[2] > 100 || (m[0] | m[2] | m[3]) == 0) return false;
    const double t = s12(m[3] << 4 | m[4] >> 4) / 10.0;
    if (!wx::tempOk(t)) return false;
    Reading r = wx::wxReading("Eurochron", "T/H", wx::hex(m[0], 2));
    r.battOk = (m[1] & 0x80) ? 0 : 1; r.tempC = t; r.hum = m[2]; r.confirm = true;
    out.push_back(r);
    return true;
}
inline bool baldr(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 1000, 2000, 3000, 0, 900, 150, 120);
    const int i = wx::repeatedRow(rows, 4, 64, 65);
    if (i < 0) return false;
    const auto m = bytesAt(rows[size_t(i)], 0, 8);
    if ((m[1] & 0x40) || (m[3] & 0xF0) != 0xF0 || (m[4] & 0x0F) || m[5] || (m[6] & 0xF7)) return false;
    const double t = s12((m[1] & 0xF) << 8 | m[2]) / 10.0;
    const int h = (m[3] & 0xF) << 4 | m[4] >> 4;
    if (!wx::tempOk(t) || h > 100) return false;
    Reading r = wx::wxReading("Baldr", "E0666TH", wx::hex(unsigned(m[0] << 8 | m[7]), 4));
    r.battOk = (m[1] & 0x80) ? 1 : 0; r.channel = ((m[1] >> 4) & 3) + 1; r.tempC = t; r.hum = h; r.confirm = true;
    out.push_back(r);
    return true;
}
inline bool missil(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 975, 1950, 2600, 0, 900, 150, 120);
    const int i = wx::repeatedRow(rows, 5, 40, 40);
    if (i < 0) return false;
    const auto m = bytesAt(rows[size_t(i)], 0, 5);
    if ((m[4] & 0xF) != 0xF || (m[0] == 0 && m[1] == 0)) return false;
    const int f12 = m[2] << 4 | m[3] >> 4, f8 = (m[3] & 0xF) << 4 | m[4] >> 4;
    Reading r = wx::wxReading("Missil", "ML0757 station", wx::hex(m[0], 2));
    r.battOk = (m[1] & 0x80) ? 0 : 1;
    if (m[1] & 0x04) {
        r.rainMm = f12 * 0.45;
        r.windKmh = f8 == 0 ? 0 : f8 == 0x80 ? 1.4 : f8 == 0xC0 ? 2.8 : (f8 + 2) * 1.4;
    } else {
        r.tempC = s12(f12) / 10.0;
        if (!wx::tempOk(r.tempC)) return false;
    }
    r.confirm = true;
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  RUBICSON 48942 pool thermometer — OOK PWM at a fixed ~720 µs period: ~240 µs mark = 0, ~480 µs = 1, after a preamble
//  and four ~730 µs sync pulses; 41 bits, sent twice: channel − 1 4 · random ID 10 · 1 0 · battery low · temperature 11
//  ((raw − 1024) / 10 °C) · 0000 · CRC-8 (0x31, init 0) of bytes 0–3 · 0.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool rubicsonPool(const Burst& b, std::vector<Reading>& out) {
    for (const auto& tm : {std::array<double, 3>{240, 480, 730}, std::array<double, 3>{430, 650, 880}}) {
        const auto rows = wx::pwm(b.ook, tm[0], tm[1], 900, 0, tm[2]);
        const int i = wx::repeatedRow(rows, 2, 41, 41);
        if (i < 0) continue;
        const auto m = bytesAt(rows[size_t(i)], 0, 6);
        if ((m[3] & 0xF) || m[5] || (m[1] & 3) != 2 || (m[0] == 0 && m[2] == 0 && m[4] == 0)) continue;
        if (crc8(m.data(), 4, 0x31, 0) != m[4]) continue;
        Reading r = wx::wxReading("Rubicson", "48942 pool", wx::hex(unsigned((m[0] & 0xF) << 6 | m[1] >> 2), 3));
        r.channel = (m[0] >> 4) + 1;
        r.battOk = (m[2] & 0x80) ? 0 : 1;
        r.tempC = (((m[2] & 0x7F) << 4 | m[3] >> 4) - 1024) / 10.0;
        if (!wx::tempOk(r.tempC)) continue;
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (en2058(b, out)) return true;
    if (rubicsonPool(b, out)) return true;
    if (emax(b, out)) return true;
    if (marbella(b, out)) return true;
    /* ★ HELD (2026-11-12): infactory() reads its corpus folders 6/6, but alecto_ws_1200/03/g000 holds a second sensor
     *  in exactly its format (7 copies, CRC-4 good, framing right) that the recording's answer key does not list, while
     *  the front end misses that recording's keyed Alecto (a strong 0.45 s signal at 1.05 s): the bench counts it WRONG.
     *  Hook it back once the front end reads that Alecto (WRONG must never rise). test-ism exercises it directly. */
    if (esun(b, out)) return true;
    return false;
}
inline bool decodeWeak(const Burst& b, std::vector<Reading>& out) {
    if (baldr(b, out)) return true;
    if (missil(b, out)) return true;
    if (eurochron(b, out)) return true;
    return false;
}

} // namespace wx15
} // namespace vibeism
