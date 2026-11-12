// vibe_ism_weather13.h — V12 ISM decoder, weather pass 2 (2026-11-12): ThermoPro's FSK sensors (TX-7B, TP211B, TP28b,
// TempSpike XR), WG-PB12V1, Thermor A6N-132TX and DG950, Maverick ET-73.
// Sources: the makers' product pages and FCC filings named per family, and the protocol notes at the head of rtl_433's
// thermopro_tx7b.c, thermopro_tp211b.c, thermopro_tp28b.c, thermopro_tp86xb.c, wg_pb12v1.c, thermor_a6n_132tx.c,
// thermor.c, maverick_et73.c — FACTS only (layout, bit time, sync, check, scaling); our own code on our own slicers
// (vibe_ism_wxkit.h). Scored on the rtl_433_tests recordings (scripts/ism-corpus.mjs).
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx13 {

/** Every start of a 24- or 32-bit sync in the FSK rows at `bitUs`, the frame's bytes after it handed to `fn` (true =
 *  taken). */
template <class F>
inline bool fskSync(const Burst& b, double bitUs, uint32_t sync, int syncBits, int nBytes, F fn) {
    if (b.fsk.empty() && b.ook.size() < 40) return false;
    for (const Bits& row : wx::fskRows(b, bitUs, 40, size_t(syncBits + 8 * nBytes - 8))) {
        for (size_t s : wx::findAll(row, sync, syncBits)) {
            if (s + size_t(syncBits) + size_t(8 * nBytes) > row.size() + 8) break;   // the last bits may run into the gap
            if (fn(bytesAt(row, s + size_t(syncBits), nBytes))) return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THERMOPRO TX-7B (TP260B / TP280B stations) — FSK NRZ 108 µs, sync (D2) 55 2D D4, 9 bytes: ID · battery low, button,
//  channel − 1 (2 bits), 4 flags · temperature 12 ((raw − 400) / 10 °C) · 4 spare · humidity % · AA 55 AA · an LFSR
//  digest (generator 0x98, key 0x25) of bytes 0–7 taken LAST byte first, each byte's bits MSB first, the key stepped
//  right. Then D2 D2 D2 … Facts: buythermopro.com TX-7B; rtl_433 notes (and its digest helper's definition — the order
//  the bits are taken in, which the notes' "bit reflect, byte reflect" leaves open).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint8_t digestLastFirst(const uint8_t* d, int n, uint8_t gen, uint8_t key) {
    uint8_t sum = 0;
    for (int i = n - 1; i >= 0; --i)
        for (int k = 7; k >= 0; --k) {
            if ((d[i] >> k) & 1) sum ^= key;
            key = uint8_t((key & 1) ? (key >> 1) ^ gen : key >> 1);
        }
    return sum;
}
inline bool tx7b(const Burst& b, std::vector<Reading>& out) {
    return fskSync(b, 108, 0x552DD4, 24, 9, [&](const std::vector<uint8_t>& m) {
        if (m[5] != 0xAA || m[6] != 0x55 || m[7] != 0xAA || digestLastFirst(m.data(), 8, 0x98, 0x25) != m[8]) return false;
        Reading r = wx::wxReading("ThermoPro", "TX-7B", wx::hex(m[0], 2));
        r.channel = ((m[1] >> 4) & 3) + 1;
        r.battOk = (m[1] & 0x80) ? 0 : 1;
        r.tempC = ((m[2] << 4 | m[3] >> 4) - 400) / 10.0;
        if (m[4] > 100 || !wx::tempOk(r.tempC)) return false;
        r.hum = m[4];
        out.push_back(r);
        return true;
    });
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THERMOPRO TP211B (915 MHz) — FSK NRZ 105 µs, sync 55 2D D4, 8 bytes: ID 24 · battery low (bit 7) + 3 spare ·
//  temperature 12 ((raw − 500) / 10 °C) · AA · a 16-bit check. The notes give the check as one 16-bit word per message
//  bit, XORed together with 0x411B; those words are one LFSR's successive states — taking each byte's bits LSB first,
//  the key starts at 0xC010 and steps left, XORing in 0x4003 when a 1 leaves the top (derived here from the notes' list;
//  checked on its two worked frames in test-ism).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint16_t tp211bCheck(const uint8_t* d, int n) {
    uint16_t key = 0xC010, s = 0x411B;
    for (int i = 0; i < n; ++i)
        for (int k = 0; k < 8; ++k) {
            if ((d[i] >> k) & 1) s ^= key;
            key = uint16_t((key << 1) ^ ((key & 0x8000) ? 0x4003 : 0));
        }
    return s;
}
inline bool tp211b(const Burst& b, std::vector<Reading>& out) {
    return fskSync(b, 105, 0x552DD4, 24, 8, [&](const std::vector<uint8_t>& m) {
        if (m[5] != 0xAA || tp211bCheck(m.data(), 6) != uint16_t(m[6] << 8 | m[7])) return false;
        if ((m[0] | m[1] | m[2] | m[3] | m[4]) == 0) return false;
        Reading r = wx::wxReading("ThermoPro", "TP211B", wx::hex(unsigned(m[0] << 16 | m[1] << 8 | m[2]), 6));
        r.battOk = (m[3] & 0x80) ? 0 : 1;
        r.tempC = (((m[3] & 0xF) << 8 | m[4]) - 500) / 10.0;
        if (!wx::tempOk(r.tempC, -50, 300)) return false;
        out.push_back(r);
        return true;
    });
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THERMOPRO TP28b BBQ (915 MHz) — FSK NRZ 105 µs, sync D2 AA 2D D4, 17 bytes: six little-endian BCD temperatures
//  (probe 1 now / high alarm / low alarm, probe 2 the same; LL HH → HH tens-and-hundreds, LL units-and-tenths, °C) ·
//  flags 16 · ID 16 · the byte sum of 0–15.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool bcdOk(uint8_t v) { return (v >> 4) < 10 && (v & 0xF) < 10; }
inline double bcdTemp(uint8_t lo, uint8_t hi) { return (hi >> 4) * 100 + (hi & 0xF) * 10 + (lo >> 4) + (lo & 0xF) * 0.1; }
inline bool tp28b(const Burst& b, std::vector<Reading>& out) {
    return fskSync(b, 105, 0xD2AA2DD4u, 32, 17, [&](const std::vector<uint8_t>& m) {
        if ((wx::addBytes(m.data(), 16) & 0xFF) != m[16]) return false;
        for (int i = 0; i < 12; ++i) if (!bcdOk(m[size_t(i)])) return false;
        if (wx::addBytes(m.data(), 16) == 0) return false;
        Reading r = wx::wxReading("ThermoPro", "TP28b BBQ", wx::hex(unsigned(m[14] << 8 | m[15]), 4));
        r.tempC = bcdTemp(m[0], m[1]);
        const double t2 = bcdTemp(m[6], m[7]);
        char e[40]; std::snprintf(e, sizeof e, "probe 2 %.1f °C", t2); r.extra = e;
        r.fact("temperature_1_C", r.tempC);
        r.fact("temperature_2_C", t2);
        r.fact("id", (long long)(m[14] << 8 | m[15]));
        out.push_back(r);
        return true;
    });
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THERMOPRO TEMPSPIKE XR (TP862b / TP863b) — FSK NRZ 104 µs, sync D2 55 2D D4, 9 bytes: ID · docked (bit 6), colour
//  (bit 4) · internal 12 · ambient 12 (both (raw − 500) / 10 °C) · booster (bits 7–6 of byte 5) · byte 6: probe battery
//  bars (5–4), probe (3–2), booster battery (1–0) · CRC-8 (poly 0x07, init 0) of 0–6 XOR 0xDB · its complement for a
//  probe (0 for a booster) — so bytes 7 and 8 share no 1 bit.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tempSpike(const Burst& b, std::vector<Reading>& out) {
    return fskSync(b, 104, 0xD2552DD4u, 32, 9, [&](const std::vector<uint8_t>& m) {
        if ((m[7] & m[8]) != 0 || uint8_t(crc8(m.data(), 7, 0x07, 0) ^ 0xDB) != m[7]) return false;
        const bool probe = (m[6] & 0x0C) == 0x0C;
        Reading r = wx::wxReading("ThermoPro", probe ? "TempSpike XR probe" : "TempSpike XR booster", wx::hex(m[0], 2));
        const double in = ((m[2] << 4 | m[3] >> 4) - 500) / 10.0, amb = (((m[3] & 0xF) << 8 | m[4]) - 500) / 10.0;
        if (!wx::tempOk(in, -50, 300) || !wx::tempOk(amb, -50, 300)) return false;
        r.tempC = in;
        char e[40]; std::snprintf(e, sizeof e, "ambient %.1f °C", amb); r.extra = e;
        if (probe) r.battOk = ((m[6] >> 4) & 3) ? 1 : 0;
        r.fact("temperature_int_C", in);
        r.fact("temperature_amb_C", amb);
        r.fact("id", (long long)m[0]);
        out.push_back(r);
        return true;
    });
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WG-PB12V1 temperature probe — the WH2's line code (OOK PWM, ~564 µs = 1, ~1476 µs = 0, ~960 µs gaps): 0xFF, then
//  type 3 (the WH2's is 4) · temperature 12 ((raw − 400) / 10 °C) · 3 spare + ID 5 · 0xFF (no humidity) · CRC-8 (0x31,
//  init 0) over the four bytes after the 0xFF.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool wgpb12(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : wx::pwm(b.ook, 564, 1476, 2500, 1)) {
        if (row.size() < 48 || row.size() > 52) continue;
        for (size_t s = 0; s + 48 <= row.size(); ++s) {
            const auto m = bytesAt(row, s, 6);
            if (m[0] != 0xFF || (m[1] >> 4) != 3 || m[4] != 0xFF || crc8(m.data() + 1, 4, 0x31, 0) != m[5]) continue;
            Reading r = wx::wxReading("WG", "PB12V1 probe", wx::hex(m[3] & 0x1F, 2));
            r.tempC = (((m[1] & 0xF) << 8 | m[2]) - 400) / 10.0;
            if (!wx::tempOk(r.tempC, -40, 120)) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THERMOR A6N-132TX (FCC A6N-132TX) — OOK PPM: ~520 µs marks, 1000 µs space = 0, 2000 µs = 1, ~4 ms between frames;
//  a 32-bit frame sent 11 times: ID 4 · channel 2 · 2 spare · temperature 16 (/ 10 °C) · check. The check's low nibble
//  is the sum of the three bytes' low nibbles; the high nibble depends on the ID (ID 4: the parity of the high nibbles'
//  sum plus the low sum's carry; otherwise bit 3 = the parity of byte 0's low nibble, byte 1's low nibble and byte 2,
//  bits 2–0 = (2 + the high sum's parity) XOR the carry). Weak: 4 identical frames, and confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool thermorA6n(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 1000, 2000, 3000, 0, 900, 250, 120);
    const int i = wx::repeatedRow(rows, 4, 32, 32);
    if (i < 0) return false;
    const auto m = bytesAt(rows[size_t(i)], 0, 4);
    const int lo = (m[0] & 0xF) + (m[1] & 0xF) + (m[2] & 0xF), carry = lo >> 4, hi = (m[0] >> 4) + (m[1] >> 4) + (m[2] >> 4);
    if ((lo & 0xF) != (m[3] & 0xF)) return false;
    const int id = m[0] >> 4;
    int want;
    if (id == 4) want = (hi + carry) & 1;
    else want = (wx::parity8(m[0] & 0xF) ^ wx::parity8(m[1] & 0xF) ^ wx::parity8(m[2])) << 3 | (((2 + (hi & 1)) ^ carry) & 7);
    if ((m[3] >> 4) != want) return false;
    const int raw = m[1] << 8 | m[2];
    if (raw > 2500) return false;
    Reading r = wx::wxReading("Thermor", "A6N-132TX", wx::hex(unsigned(id), 1));
    r.channel = (m[0] >> 2) & 3;
    r.tempC = raw / 10.0;
    r.confirm = true;
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THERMOR DG950 station (FCC S24DG950 / S24DG950R; issue #2879) — OOK PWM: ~750 µs = 1, ~2128 µs = 0, after 96 sync
//  pulses of ~1440 µs; then 13 words of 9 bits, each a 0 start bit and a byte sent LSB first. Bytes: ID (inverted) ·
//  temperature whole (+ 195) · rain rate (inverted) · temperature check = whole + tenths + 1 · direction nibble ×2
//  (0xFF = none) · wind A low, A high, B (all inverted) · their sum (inverted) · tenths (+ 245) · spare · rain rate + 7
//  (inverted). A pairing frame: byte 0 = 0xFF and the new ID (inverted) in bytes 1, 2, 4–8, 10. The wind speed is the
//  notes' empirical fit: raw = A (16 bits), ratio 0.155 − 0.0001746 × raw under 256 else 0.11, speed = ratio × (raw −
//  B + 45) km/h (≥ 0). Direction nibble → compass sector (×22.5°): 7 2 6 3 8 1 5 4 10 15 11 14 9 0 12 13.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool dg950(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::pwm(b.ook, 750, 2128, 2900, 1);
    for (size_t i = 0; i + 13 <= rows.size(); ++i) {
        uint8_t m[13];
        bool ok = true;
        for (size_t k = 0; k < 13 && ok; ++k) {
            const Bits& w = rows[i + k];
            if (w.size() != 9 || w[0] != 0) { ok = false; break; }
            uint8_t v = 0;
            for (int j = 0; j < 8; ++j) v = uint8_t(v | w[size_t(1 + j)] << j);   // LSB first
            m[k] = v;
        }
        if (!ok) continue;
        if (m[0] == 0xFF) {
            const uint8_t v = m[1];
            if (m[2] != v || m[4] != v || m[5] != v || m[6] != v || m[7] != v || m[8] != v || m[10] != v) continue;
            Reading r = wx::wxReading("Thermor", "DG950 station", wx::hex(uint8_t(~v), 2));
            r.extra = "pairing";
            r.fact("id", (long long)uint8_t(~v));
            r.fact("pairing", 1LL);
            out.push_back(r);
            return true;
        }
        if (uint8_t(m[1] + m[10] + 1) != m[3]) continue;
        if (uint8_t(~m[2]) != uint8_t(uint8_t(~m[12]) - 7)) continue;
        if (uint8_t(uint8_t(~m[6]) + uint8_t(~m[7]) + uint8_t(~m[8])) != uint8_t(~m[9])) continue;
        if (m[4] != 0xFF && m[5] != 0xFF && m[4] != m[5]) continue;
        Reading r = wx::wxReading("Thermor", "DG950 station", wx::hex(uint8_t(~m[0]), 2));
        r.tempC = (m[1] - 195) + (m[10] - 245) * 0.1;
        if (!wx::tempOk(r.tempC)) continue;
        if (m[4] != 0xFF && m[5] != 0xFF) {
            static const uint8_t sector[16] = {7, 2, 6, 3, 8, 1, 5, 4, 10, 15, 11, 14, 9, 0, 12, 13};
            r.windDir = sector[m[4] & 0xF] * 22.5;
        }
        if (m[8] != 0xFF) {
            const int raw = uint8_t(~m[6]) | uint8_t(~m[7]) << 8, coef = uint8_t(~m[8]);
            const double ratio = raw < 256 ? 0.155 - 0.0001746 * raw : 0.11;
            r.windKmh = std::max(0.0, ratio * (raw - coef + 45));
        }
        r.fact("id", (long long)uint8_t(~m[0]));
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  MAVERICK ET-73 BBQ (two probes) — OOK PPM: ~4 ms between 48-bit frames, each bit a mark (~240 µs; some units ~1.1 /
//  2.1 ms, the bit period ~3.16 ms) then a space;
//  a ~1 ms space = 0, ~2 ms = 1 (so a long mark = 0). ID 8 (random at power-up) · probe 1 temperature 12 · probe 2
//  temperature 12 (both two's complement, / 10 °C) · 16 unknown. No published check: 3 identical frames, never all-0 or
//  all-1, and confirm. Tried after every family with a check.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool et73(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 1050, 2050, 3000, 0, 2600, 150);
    const int i = wx::repeatedRow(rows, 3, 48, 48);
    if (i < 0) return false;
    const auto m = bytesAt(rows[size_t(i)], 0, 6);
    if ((m[0] | m[1] | m[2] | m[3]) == 0 || (m[0] & m[1] & m[2] & m[3]) == 0xFF) return false;
    const auto s12 = [](int v) { return (v & 0x800) ? v - 0x1000 : v; };
    const double t1 = s12(m[1] << 4 | m[2] >> 4) / 10.0, t2 = s12((m[2] & 0xF) << 8 | m[3]) / 10.0;
    if (!wx::tempOk(t1, -40, 300) || !wx::tempOk(t2, -40, 300)) return false;
    Reading r = wx::wxReading("Maverick", "ET-73 BBQ", wx::hex(m[0], 2));
    r.tempC = t1;
    char e[40]; std::snprintf(e, sizeof e, "probe 2 %.1f °C", t2); r.extra = e;
    r.confirm = true;
    r.fact("temperature_1_C", t1);
    r.fact("temperature_2_C", t2);
    r.fact("id", (long long)m[0]);
    out.push_back(r);
    return true;
}

/** The families with a CRC / digest / strong sum. */
inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (tx7b(b, out)) return true;
    if (tp211b(b, out)) return true;
    if (tempSpike(b, out)) return true;
    if (tp28b(b, out)) return true;
    if (wgpb12(b, out)) return true;
    if (dg950(b, out)) return true;
    return false;
}
/** The weak ones: after every family with a check. */
inline bool decodeWeak(const Burst& b, std::vector<Reading>& out) {
    if (thermorA6n(b, out)) return true;
    if (et73(b, out)) return true;
    return false;
}

} // namespace wx13
} // namespace vibeism
