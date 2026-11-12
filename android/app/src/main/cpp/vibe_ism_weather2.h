// vibe_ism_weather2.h — V12 ISM decoder, weather pass 2 (2026-11-11): Oregon Scientific SL109H and the Fine Offset /
// Ecowitt FSK family (WH24 / WH65 / WS69, WH25 / WH32 / WH32B, WH0290 air quality, WH51 soil moisture).
//
// ★★★ SOURCES. Frame FACTS (layout, timing, sync, check polynomial and coverage, scaling) were taken from public
//     descriptions: the documents cited per family, and — from this pass on, with Stuart's agreement — the comment
//     blocks and field tables of rtl_433's device files (github.com/merbanan/rtl_433, src/devices/*.c), read for facts
//     only. No rtl_433 code, table or identifier is copied: every decoder below is our own, built on the shared slicers
//     (vibe_ism_wxkit.h). Each was scored on the rtl_433_tests recordings (scripts/ism-corpus.mjs) — test input only.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  OREGON SCIENTIFIC SL109H (remote thermo-hygro for the SL109 clock) — 433.92 MHz OOK PPM
//  Sources: "Oregon Scientific RF Protocols II" (osengr.org WxShield downloads); rtl_433 oregon_scientific_sl109h.c
//  (field table). ~500 µs marks; the space is the bit: ~2 ms = 0, ~4 ms = 1; ~8.9 ms between repeats. 38 bits:
//  checksum 4 · channel 2 (1, 2; 0 = channel 3) · humidity 2 BCD digits · temperature 12 bits signed × 0.1 °C · status 4
//  · ID 8 (new at every battery change). Checksum = the 4-bit sum of the nine nibbles after it (the channel's two bits
//  as a nibble 00cc). A 4-bit check is weak: the row must repeat identically, and the reading is shown once heard twice.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace sl109h {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2000, 4000, 5500, 0, 1500, 0, 120);   // ★ blips inside a 2–4 ms space are noise
    const int i = wx::repeatedRow(rows, 2, 38, 38);
    if (i < 0) return false;
    const Bits& w = rows[size_t(i)];
    if (bitsAt(w, 0, 32) == 0) return false;
    unsigned sum = bitsAt(w, 4, 2);
    for (int k = 0; k < 8; ++k) sum += bitsAt(w, size_t(6 + 4 * k), 4);
    if ((sum & 0xF) != bitsAt(w, 0, 4)) return false;
    const int cc = int(bitsAt(w, 4, 2));
    if (cc == 3) return false;
    const int h10 = int(bitsAt(w, 6, 4)), h1 = int(bitsAt(w, 10, 4));
    if (h10 > 9 || h1 > 9) return false;
    int t = int(bitsAt(w, 14, 12)); if (t & 0x800) t -= 0x1000;
    const double tc = t / 10.0;
    if (tc < -20 || tc > 60) return false;
    Reading r = wx::wxReading("Oregon Scientific", "SL109H", wx::hex(bitsAt(w, 30, 8), 2));
    r.channel = cc ? cc : 3;
    r.tempC = tc; r.hum = h10 * 10 + h1;
    r.confirm = true;
    out.push_back(r);
    return true;
}

} // namespace sl109h

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FINE OFFSET / ECOWITT FSK SENSORS (433 / 868 / 915 MHz, 58 µs NRZ — 17.24 kbps)
//  Source: rtl_433 fineoffset.c field tables (WH24/WH65/WS69, WH25/WH32/WH32B, WH0290, WH51). Preamble AA…, sync 2D D4,
//  then a family code:
//   • 0x24 WH24 / WH65 (outdoor all-in-one): 17 bytes — family · ID · wind direction low 8 · flags (bit 7 direction bit 8,
//     bit 4 wind bit 8, bit 3 battery low) + temperature 11 bits ((raw − 400) / 10 °C) · humidity · wind (raw / 8 ×
//     factor) · gust (raw × factor) · rain counter 16 · UV 16 · light 24 (× 0.1 lux) · CRC-8 0x31 over bytes 0–14 ·
//     byte sum of 0–15. Factor 1.12 m/s and 0.3 mm a tip on the WH24; 0.51 m/s and 0.254 mm on the WH65 (told apart by
//     its longer preamble). WS69: the same, then 8 more bytes (pressure 17 bits × 0.01 hPa, its own CRC and sum).
//   • 0xE_ WH25 (T/H/P indoor) and 0xD_ / short frame WH32 (T/H): model nibble · ID 8 · battery 1 · invalid 1 ·
//     temperature 10 bits ((raw − 400) / 10) · humidity · pressure 16 (× 0.1 hPa) · byte sum of 0–5 · (WH25 only) XOR of
//     0–5 with its nibbles swapped.
//   • 0x41 WH0290 / WH41 PM2.5: family · ID · flags + PM2.5 14 bits · flags + PM10 14 bits · CRC-8 0x31 over 0–5 · byte
//     sum of 0–6. Shown in words (µg/m³).
//   • 0x51 WH51 soil moisture: family · ID 24 · boost 3 + battery 5 (× 0.1 V) · 0x7F · moisture % · AD value · 3 × FF ·
//     CRC-8 0x31 over 0–11 · byte sum of 0–12. Shown in words.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace fofsk {

inline bool parseAt(const Bits& row, size_t p, size_t preBits, std::vector<Reading>& out) {
    const auto b = bytesAt(row, p, 25);   // (25: the longest frame, the WS69's)
    const size_t avail = row.size() > p ? (row.size() - p) / 8 : 0;
    const uint8_t fam = b[0];
    if (fam == 0x24 && avail >= 17) {
        if (crc8(b.data(), 15, 0x31, 0) != b[15] || (wx::addBytes(b.data(), 16) & 0xFF) != b[16]) return false;
        const bool wh65 = preBits >= 43;
        const double wf = wh65 ? 0.51 : 1.12, rf = wh65 ? 0.254 : 0.3;
        bool ws69 = false;
        double hpa = NAN;
        if (avail >= 25 && crc8(b.data(), 23, 0x31, 0) == b[23] && (wx::addBytes(b.data(), 24) & 0xFF) == b[24]) {
            ws69 = true;
            const int praw = (b[17] << 16 | b[18] << 8 | b[19]) & 0x1FFFF;
            if (praw < 0x1FFFF) hpa = praw * 0.01;
        }
        Reading r = wx::wxReading("Fine Offset", ws69 ? "WS69" : wh65 ? "WH65" : "WH24", wx::hex(b[1], 2));
        const int traw = (b[3] & 0x07) << 8 | b[4];
        if (traw != 0x7FF) r.tempC = (traw - 400) / 10.0;
        if (b[5] != 0xFF) r.hum = b[5];
        const int dir = b[2] | (b[3] & 0x80) << 1;
        if (dir <= 359) r.windDir = dir;
        const int ws = b[6] | (b[3] & 0x10) << 4;
        if (ws != 0x1FF) r.windKmh = ws / 8.0 * wf * 3.6;
        if (b[7] != 0xFF) r.gustKmh = b[7] * wf * 3.6;
        r.rainMm = (b[8] << 8 | b[9]) * rf;
        const int uv = b[10] << 8 | b[11];
        if (uv != 0xFFFF) {
            static const int top[13] = {432, 851, 1210, 1570, 2017, 2450, 2761, 3100, 3512, 3918, 4277, 4650, 5029};
            int k = 0; while (k < 13 && uv > top[k]) ++k;
            r.uv = k;
        }
        const int lux = b[12] << 16 | b[13] << 8 | b[14];
        if (lux != 0xFFFFFF) { char e[32]; std::snprintf(e, sizeof e, "%.0f lux", lux * 0.1); r.extra = e; }
        r.baroHpa = hpa;
        r.battOk = (b[3] & 0x08) ? 0 : 1;
        if (!std::isnan(r.tempC) && !wx::tempOk(r.tempC, -40, 60)) return false;
        out.push_back(r);
        return true;
    }
    if (fam == 0x41 && avail >= 8) {
        if (crc8(b.data(), 6, 0x31, 0) != b[6] || (wx::addBytes(b.data(), 7) & 0xFF) != b[7]) return false;
        Reading r = wx::wxReading("Fine Offset", "WH0290 air quality", wx::hex(b[1], 2));
        r.kind = "sensor";
        const int pm25 = (b[2] & 0x3F) << 8 | b[3], pm10 = (b[4] & 0x3F) << 8 | b[5];
        char e[48]; std::snprintf(e, sizeof e, "PM2.5 %d µg/m³ · PM10 %d µg/m³", pm25, pm10); r.extra = e;
        out.push_back(r);
        return true;
    }
    if (fam == 0x51 && avail >= 14) {
        if (crc8(b.data(), 12, 0x31, 0) != b[12] || (wx::addBytes(b.data(), 13) & 0xFF) != b[13]) return false;
        Reading r = wx::wxReading("Fine Offset", "WH51 soil moisture", wx::hex(b[1] << 16 | b[2] << 8 | b[3], 6));
        r.kind = "sensor";
        const int mv = (b[4] & 0x1F) * 100;
        r.battOk = mv >= 1300 ? 1 : 0;
        char e[40]; std::snprintf(e, sizeof e, "soil moisture %d %%", b[6]); r.extra = e;
        out.push_back(r);
        return true;
    }
    /* ★ Ecowitt / Ambient Weather on the same sync (field facts: rtl_433 ambientweather_wh31e.c): every one checks a
     *  CRC-8 0x31 AND a byte sum, at its own length.
     *   0x30 / 0x37 WH31E / WH31B T/H: ID · 1 + channel 3 + 0 + battery low + temperature 10 bits ((raw − 400) / 10) ·
     *   humidity · CRC over 0–4 · sum over 0–5.   0x40 WH40 rain: ID 20 bits · battery (× 0.1 V) · tips 16 (0.1 mm) · CRC
     *   over 0–6 · sum over 0–7.   0x20 WN20 rain: 0 · ID 16 · battery (× 0.02 V) · tips 16 (0.1 mm) · ? · CRC over 0–7 ·
     *   sum over 0–8.   0x68 WS68 wind / light / UV: ? · ID 16 · lux / 10 · battery · MSBs + flags · 2 · speed, direction,
     *   gust (0.1 m/s, °) · UV × 10 · CRC over 0–13 · sum over 0–14.   0x52 the clock frame (BCD date and time). */
    auto crcSum = [&](int n) { return avail >= size_t(n + 2) && crc8(b.data(), size_t(n), 0x31, 0) == b[size_t(n)] &&
                                      (wx::addBytes(b.data(), size_t(n + 1)) & 0xFF) == b[size_t(n + 1)]; };
    if ((fam == 0x30 || fam == 0x37) && crcSum(5)) {
        Reading r = wx::wxReading("Ecowitt / Ambient", fam == 0x30 ? "WH31E" : "WH31B", wx::hex(b[1], 2));
        r.channel = ((b[2] >> 4) & 7) + 1;
        r.battOk = (b[2] & 0x04) ? 0 : 1;
        r.tempC = (((b[2] & 3) << 8 | b[3]) - 400) / 10.0;
        if (!wx::tempOk(r.tempC, -40, 60) || b[4] > 100) return false;
        r.hum = b[4];
        out.push_back(r);
        return true;
    }
    if (fam == 0x40 && crcSum(7)) {
        Reading r = wx::wxReading("Ecowitt", "WH40 rain", wx::hex(unsigned(b[1] & 0xF) << 16 | b[2] << 8 | b[3], 5));
        r.rainMm = (b[5] << 8 | b[6]) * 0.1;
        out.push_back(r);
        return true;
    }
    if (fam == 0x20 && crcSum(8)) {
        Reading r = wx::wxReading("Ecowitt", "WN20 rain", wx::hex(unsigned(b[2] << 8 | b[3]), 4));
        r.rainMm = (b[5] << 8 | b[6]) * 0.1;
        out.push_back(r);
        return true;
    }
    if (fam == 0x68 && crcSum(14)) {
        Reading r = wx::wxReading("Ecowitt", "WS68 wind / light", wx::hex(unsigned(b[2] << 8 | b[3]), 4));
        r.windKmh = (((b[7] & 0x10) << 4) | b[10]) * 0.1 * 3.6;
        r.windDir = ((b[7] & 0x20) << 3) | b[11];
        r.gustKmh = (((b[7] & 0x40) << 2) | b[12]) * 0.1 * 3.6;
        r.uv = int(b[13] * 0.1);
        char e[32]; std::snprintf(e, sizeof e, "%d lux", (b[4] << 8 | b[5]) * 10); r.extra = e;
        out.push_back(r);
        return true;
    }
    if (fam == 0x52 && crcSum(9)) {
        Reading r = wx::wxReading("Ecowitt / Ambient", "WH31E clock", wx::hex(b[1], 2));
        char e[48]; std::snprintf(e, sizeof e, "WWVB time 20%02X-%02X-%02X %02X:%02X:%02X", b[3], b[4] & 0x1F, b[5] & 0x3F, b[6] & 0x3F, b[7] & 0x7F, b[8] & 0x7F);
        r.extra = e;
        out.push_back(r);
        return true;
    }
    if (((fam >> 4) == 0xE || (fam >> 4) == 0xD) && avail >= 7) {
        if ((wx::addBytes(b.data(), 6) & 0xFF) != b[6]) return false;
        const unsigned x = wx::xorBytes(b.data(), 6), xs = ((x & 0xF) << 4) | (x >> 4);
        const bool wh25 = (fam >> 4) == 0xE && avail >= 8 && xs == b[7];
        // ★ an 8-bit sum alone is weak: a frame without the XOR byte must be heard twice
        Reading r = wx::wxReading("Fine Offset", wh25 ? "WH25" : (fam >> 4) == 0xD ? "WH32" : "WH32B",
                                  wx::hex(((b[0] & 0xF) << 4) | (b[1] >> 4), 2));
        const int traw = (b[1] & 0x03) << 8 | b[2];
        r.tempC = (traw - 400) / 10.0;
        if (!wx::tempOk(r.tempC, -40, 60) || b[3] > 100) return false;
        r.hum = b[3];
        const int praw = b[4] << 8 | b[5];
        if ((fam >> 4) == 0xE && praw != 0xFFFF) r.baroHpa = praw * 0.1;
        if (!std::isnan(r.baroHpa) && (r.baroHpa < 300 || r.baroHpa > 1100)) return false;
        r.battOk = (b[1] & 0x08) ? 0 : 1;
        if (!wh25) r.confirm = true;
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : wx::fskRows(b, 58.5, 40, 80)) {
        for (size_t s : wx::findAll(row, 0xAA2DD4, 24)) {
            size_t pre = 0;                                         // the preamble's length before the sync
            while (s >= pre + 1 && row[s - pre - 1] == ((pre & 1) ? 1 : 0)) ++pre;
            if (parseAt(row, s + 24, pre + 8, out)) return true;
        }
    }
    return false;
}

} // namespace fofsk

} // namespace vibeism
