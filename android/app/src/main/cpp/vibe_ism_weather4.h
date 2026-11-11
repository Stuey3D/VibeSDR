// vibe_ism_weather4.h — V12 ISM decoder, weather pass 2026-11-11: ACURITE (Chaney / Primex), beyond the Ray Wang
// families in vibe_ism_protocols2.h. Sources: Ray Wang's rayshobby.net write-ups (the 592TX framing), the wxforum.net
// 5-in-1 thread (wind scale), and rtl_433's acurite.c field tables — FACTS only (layouts, timings, checks, scaling).
// Our own code throughout.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace acurite2 {

inline double fToC(double f) { return (f - 32.0) / 1.8; }
inline const char* chanName(uint8_t b0) { static const char* k[4] = {"C", "?", "B", "A"}; return k[b0 >> 6]; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE "TXR" FAMILY — 592TXR / 592TX / 06002RM tower, 5-in-1 (Iris), 3-in-1 (Notos), Atlas (7-in-1, ± lightning),
//  Optimus 6-in-1, 515 fridge/freezer, 6045M lightning, 899 rain, 1190/1192 leak. 433.92 MHz OOK PWM: ~408 µs mark = 1,
//  ~220 µs = 0, ~612 µs period; ~620 µs sync pulses (+ ~600 µs gap) before each of 3 copies. Bytes: channel 2 + ID ·
//  ID · parity + battery OK + message type 6 · data bytes (bit 7 = even parity) · byte sum of all before. Lengths by
//  type: tower 0x04 = 7, 515 0x08/0x09 = 6, 6045M 0x2F = 9, 5-in-1 0x31/0x38 = 8, 3-in-1 0x20 = 8 (no parity), 899
//  0x30 = 8, Atlas 0x05–07 = 8 / 0x25–27 (lightning) = 10, Optimus 0x3A/0x3B = 10, 1190 0x01 = 7.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline int txrLen(int type) {
    switch (type) {
        case 0x04: case 0x01: return 7;
        case 0x08: case 0x09: return 6;
        case 0x2F: return 9;
        case 0x31: case 0x38: case 0x20: case 0x30: case 0x05: case 0x06: case 0x07: return 8;
        case 0x25: case 0x26: case 0x27: case 0x3A: case 0x3B: return 10;
        default: return 0;
    }
}
/** The 5-in-1's wind vane code → sixteenths of a circle (the vane's 4-bit code is not in compass order). */
inline double vane5n1(int code) { static const uint8_t k[16] = {14, 11, 13, 12, 15, 10, 0, 9, 3, 6, 4, 5, 2, 7, 1, 8}; return k[code & 15] * 22.5; }

inline bool txrFrame(const std::vector<uint8_t>& bb, std::vector<Reading>& out) {
    const int type = bb[2] & 0x3F, n = txrLen(type);
    if (!n) return false;
    if ((wx::addBytes(bb.data(), size_t(n - 1)) & 0xFF) != bb[size_t(n - 1)]) return false;
    if (type != 0x20) for (int i = 2; i < n - 1; ++i) if (wx::parity8(bb[size_t(i)])) return false;
    if (type != 0x30 && (bb[0] >> 6) == 1) return false;
    if (bb[0] == 0 && bb[1] == 0 && bb[2] == 0) return false;
    const int batt = (bb[2] & 0x40) ? 1 : 0;
    Reading r; r.kind = "weather"; r.make = "AcuRite"; r.battOk = batt;
    auto id14 = [&]() { return wx::hex(unsigned((bb[0] & 0x3F) << 8 | bb[1]), 4); };
    const int chan = (bb[0] >> 6) == 3 ? 1 : (bb[0] >> 6) == 2 ? 2 : 3;   // A, B, C
    switch (type) {
        case 0x04: {   // tower
            r.model = "592TXR tower"; r.pubId = id14(); r.channel = chan;
            const int h = bb[3] & 0x7F, t = (bb[4] & 0x7F) << 7 | (bb[5] & 0x7F);
            r.tempC = (t - 1000) / 10.0;
            if (!wx::tempOk(r.tempC) || (h > 100 && h != 127)) return false;
            if (h <= 100) r.hum = h;
            break;
        }
        case 0x01: {
            r.kind = "sensor"; r.model = "1190 / 1192 leak detector"; r.pubId = id14(); r.channel = chan;
            r.extra = (bb[3] & 0x10) ? "WET" : "dry";
            break;
        }
        case 0x08: case 0x09: {
            r.model = type == 8 ? "515 refrigerator" : "515 freezer"; r.pubId = id14(); r.channel = chan;
            const double f = (((bb[3] & 0x7F) << 7 | (bb[4] & 0x7F)) - 1480) / 10.0;
            if (f < -40 || f > 158) return false;
            r.tempC = fToC(f);
            break;
        }
        case 0x2F: {
            r.model = "6045M lightning"; r.pubId = id14(); r.channel = chan;
            const int h = bb[3] & 0x7F;
            const double f = (((bb[4] & 0x1F) << 7 | (bb[5] & 0x7F)) - 1480) / 10.0;
            if (h > 100 || f < -40 || f > 158) return false;
            r.tempC = fToC(f); r.hum = h;
            const int strikes = (bb[6] & 0x7F) << 1 | (bb[7] & 0x40) >> 6, dist = bb[7] & 0x1F;
            char e[64]; std::snprintf(e, sizeof e, "lightning: %d strikes · storm %d mi%s%s", strikes, dist,
                                      (bb[4] & 0x40) ? "" : " · standby", (bb[7] & 0x20) ? " · interference" : "");
            r.extra = e;
            break;
        }
        case 0x31: case 0x38: {   // 5-in-1
            r.model = "5-in-1"; r.pubId = wx::hex(unsigned((bb[0] & 0x0F) << 8 | bb[1]), 3); r.channel = chan;
            const int ws = (bb[3] & 0x1F) << 3 | (bb[4] & 0x70) >> 4;
            r.windKmh = ws > 0 ? ws * 0.8278 + 1.0 : 0;
            if (type == 0x31) {
                r.windDir = vane5n1(bb[4] & 0xF);
                r.rainMm = ((bb[5] & 0x7F) << 7 | (bb[6] & 0x7F)) * 0.254;
            } else {
                const double f = (((bb[4] & 0x0F) << 7 | (bb[5] & 0x7F)) - 400) / 10.0;
                const int h = bb[6] & 0x7F;
                if (f < -40 || f > 158 || h > 100) return false;
                r.tempC = fToC(f); r.hum = h;
            }
            break;
        }
        case 0x20: {   // 3-in-1
            r.model = "3-in-1"; r.pubId = id14(); r.channel = chan;
            const int h = bb[3] & 0x7F;
            const double f = (((bb[4] & 0x1F) << 7 | (bb[5] & 0x7F)) - 1480) / 10.0;
            if (h > 100 || f < -40 || f > 158) return false;
            r.tempC = fToC(f); r.hum = h;
            r.windKmh = (bb[6] & 0x7F) * 1.609344;
            break;
        }
        case 0x30: {   // 899 rain
            r.model = "899 rain gauge"; r.pubId = id14(); r.channel = (bb[0] >> 6) + 1;
            r.rainMm = ((bb[5] & 0x7F) << 7 | (bb[6] & 0x7F)) * 0.254;
            break;
        }
        case 0x3A: case 0x3B: {   // Optimus
            r.model = "Optimus 6-in-1"; r.pubId = wx::hex(unsigned((bb[0] & 0x0F) << 8 | bb[1]), 3); r.channel = chan;
            const int mph = (bb[3] & 0x7F) << 1 | (bb[4] & 0x40) >> 6;
            if (mph > 200) return false;
            r.windKmh = mph * 1.609344;
            if (type == 0x3B) {
                const double f = (((bb[4] & 0x0F) << 7 | (bb[5] & 0x7F)) - 400) / 10.0;
                const int h = bb[6] & 0x7F;
                if (f < -40 || f > 158 || h > 100) return false;
                r.tempC = fToC(f); r.hum = h;
            } else {
                r.windDir = vane5n1(bb[4] & 0xF);
                r.rainMm = ((bb[5] & 0x03) << 7 | (bb[6] & 0x7F)) * 0.254;
            }
            break;
        }
        default: {   // Atlas 0x05–07, 0x25–27
            r.model = (type & 0x20) ? "Atlas (lightning)" : "Atlas";
            r.pubId = wx::hex(unsigned((bb[0] & 0x03) << 8 | bb[1]), 3); r.channel = chan;
            const int mph = (bb[3] & 0x7F) << 1 | (bb[4] & 0x40) >> 6;
            if (mph > 200) return false;
            r.windKmh = mph * 1.609344;
            const int sub = type & 0x1F;
            if (sub == 5) {
                const double f = (((bb[4] & 0x0F) << 7 | (bb[5] & 0x7F)) - 400) / 10.0;
                const int h = bb[6] & 0x7F;
                if (f < -40 || f > 158 || h > 100) return false;
                r.tempC = fToC(f); r.hum = h;
            } else if (sub == 6) {
                const int dir = (bb[4] & 0x1F) << 5 | (bb[5] & 0x7C) >> 2;
                if (dir > 360) return false;
                r.windDir = dir;
                r.rainMm = ((bb[5] & 0x03) << 7 | (bb[6] & 0x7F)) * 0.254;
            } else {
                const int lux = (bb[5] & 0x7F) << 7 | (bb[6] & 0x7F);
                if (lux > 12000) return false;
                r.uv = bb[4] & 0x0F;
                char e[32]; std::snprintf(e, sizeof e, "%d lux", lux * 10); r.extra = e;
            }
            if (type & 0x20) {
                const int strikes = (bb[7] & 0x7F) << 2 | (bb[8] & 0x60) >> 5, dist = bb[8] & 0x1F;
                char e[48]; std::snprintf(e, sizeof e, "%s%slightning %d strikes · %d mi", r.extra.c_str(), r.extra.empty() ? "" : " · ", strikes, dist);
                r.extra = e;
            }
            break;
        }
    }
    out.push_back(r);
    return true;
}

inline bool txr(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : wx::pwm(b.ook, 220, 408, 500, 0, 620)) {
        if (w.size() < 48 || w.size() > 90) continue;
        const auto bb = bytesAt(w, 0, 10);
        if (txrLen(bb[2] & 0x3F) * 8 > int(w.size())) continue;
        if (txrFrame(bb, out)) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  609TXC T/H — OOK PPM, ~1 ms space = 0, ~2 ms = 1, 40 bits: ID · status 4 (bit 3 = battery low) + temperature 12 bits
//  signed × 0.1 °C · humidity · byte sum of the first four.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool th609(const Burst& b, std::vector<Reading>& out) {
    // ★★ an 8-bit sum is all the check there is (the corpus: an SL109H's glitchy slice read as a 609): the ~500 µs marks
    //    are enforced, and the 40 bits must come twice alike (the sensor sends ~6 copies)
    const auto rows = wx::ppm(b.ook, 1000, 2000, 3000, 0, 900, 250);
    for (const Bits& w : rows) {
        if (w.size() != 40) continue;
        int same = 0; for (const Bits& o : rows) if (o == w) ++same;
        if (same < 2) continue;
        const auto by = bytesAt(w, 0, 5);
        const unsigned sum = wx::addBytes(by.data(), 4);
        if (sum == 0 || (sum & 0xFF) != by[4] || by[3] > 100) continue;
        int t = (by[1] & 0xF) << 8 | by[2]; if (t & 0x800) t -= 0x1000;
        Reading r = wx::wxReading("AcuRite", "609TXC T/H", wx::hex(by[0], 2));
        r.tempC = t / 10.0;
        if (!wx::tempOk(r.tempC)) continue;
        r.hum = by[3];
        r.battOk = (by[1] & 0x80) ? 0 : 1;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  606TX / Technoline TX960 temperature — OOK PPM, ~2 ms space = 0, ~4 ms = 1, 32 bits sent ~6 times: ID · battery OK 1
//  · button 1 · channel 2 · temperature 12 bits signed × 0.1 °C · LFSR digest (generator 0x98, key 0xF1) of the first 3.
//  590TX: OOK PPM, ~500 µs space = 0, ~1500 µs = 1, 25 bits after a 3 ms sync, 14 copies: ID 7 + battery 1 ·
//  parity 1 + channel 2 + … temperature-or-humidity 12 bits signed ((raw − 500) / 10 °C; 0–100 = humidity) · a trailing
//  0. Odd parity over bits 0–9.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool tx606(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2000, 4000, 7000);
    const int i = wx::repeatedRow(rows, 3, 32, 33);
    if (i < 0) return false;
    const auto by = bytesAt(rows[size_t(i)], 0, 4);
    if ((by[0] | by[1] | by[2] | by[3]) == 0 || wx::lfsrDigest8(by.data(), 3, 0x98, 0xF1) != by[3]) return false;
    int t = (by[1] & 0xF) << 8 | by[2]; if (t & 0x800) t -= 0x1000;
    Reading r = wx::wxReading("AcuRite", "606TX / TX960", wx::hex(by[0], 2));
    r.tempC = t / 10.0;
    if (!wx::tempOk(r.tempC)) return false;
    r.battOk = (by[1] & 0x80) ? 1 : 0;
    r.channel = ((by[1] >> 4) & 3) + 1;
    out.push_back(r);
    return true;
}

inline bool tx590(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 500, 1500, 2000);
    const int i = wx::repeatedRow(rows, 3, 25, 25);
    if (i < 0) return false;
    const Bits& w = rows[size_t(i)];
    if (w[24] != 0 || bitsAt(w, 0, 24) == 0) return false;
    int par = 0; for (int k = 0; k < 10; ++k) par += w[size_t(k)];
    if (!(par & 1)) return false;
    const auto by = bytesAt(w, 0, 3);
    int raw = (by[1] & 0xF) << 8 | by[2]; if (raw & 0x800) raw -= 0x1000;
    Reading r = wx::wxReading("AcuRite", "590TX", wx::hex(by[0] & 0xFE, 2));
    r.battOk = by[0] & 1;
    r.channel = (by[1] >> 4) & 3;
    if (raw >= 0 && raw <= 100) r.hum = raw;
    else { r.tempC = (raw - 500) / 10.0; if (!wx::tempOk(r.tempC)) return false; }
    r.confirm = true;   // ★ one parity bit and repeats
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  986 / 985 refrigerator–freezer thermometers — OOK PPM (986: ~520 / 880 µs spaces; 985: ~556 / 1104 µs, a ~3 ms sync).
//  Bytes are sent LSB first. 986: temperature °F (sign + magnitude) · ID 16 · status (bit 0 = sensor 2, bit 1 = battery
//  low) · CRC-8 poly 0x07 (reflected) over the four. 985: two lead-in bytes, then the same, with battery bits 1 / 2 per
//  sensor. (The last bit is often lost: a CRC that matches with it set is accepted.)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline uint8_t rev8(uint8_t v) { v = uint8_t((v & 0xF0) >> 4 | (v & 0x0F) << 4); v = uint8_t((v & 0xCC) >> 2 | (v & 0x33) << 2); return uint8_t((v & 0xAA) >> 1 | (v & 0x55) << 1); }
inline bool fridge(const Burst& b, std::vector<Reading>& out) {
    struct V { double s, l, gap; size_t lo, hi, skip; const char* model; };
    static const V vs[2] = {{520, 880, 1280, 39, 43, 0, "986 refrigerator / freezer"}, {556, 1104, 2500, 55, 59, 2, "985 refrigerator / freezer"}};
    for (const V& v : vs)
        for (const Bits& w : wx::ppm(b.ook, v.s, v.l, v.gap, 0, 0, 150)) {
            if (w.size() < v.lo || w.size() > v.hi) continue;
            std::vector<uint8_t> br(7);
            for (size_t k = 0; k < 7; ++k) br[k] = rev8(uint8_t(bitsAt(w, 8 * k, 8)));
            const uint8_t* p = br.data() + v.skip;
            if ((p[0] | p[1] | p[2] | p[3]) == 0 || (v.skip == 0 && p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF)) continue;
            const uint8_t c = wx::crc8le(p, 4, 0xE0, 0);
            if (c != p[4] && c != (p[4] | 0x80)) continue;
            if (c != p[4]) {   // ★ the lost-last-bit allowance doubles the chance of noise passing: then a second copy must agree
                int same = 0; for (const Bits& o : wx::ppm(b.ook, v.s, v.l, v.gap, 0, 0, 150)) if (o == w) ++same;
                if (same < 2) continue;
            }
            const int tf = (p[0] & 0x80) ? -(p[0] & 0x7F) : p[0];
            if (tf < -40 || tf > 104) continue;
            Reading r = wx::wxReading("AcuRite", v.model, wx::hex(unsigned(p[1] << 8 | p[2]), 4));
            const int sensor = (p[3] & 1) + 1;
            r.channel = sensor;
            r.tempC = fToC(tf);
            r.battOk = v.skip ? ((p[3] & (sensor == 2 ? 4 : 2)) ? 0 : 1) : ((p[3] & 2) ? 0 : 1);
            r.note = sensor == 2 ? "freezer" : "refrigerator";
            out.push_back(r);
            return true;
        }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  00275RM / 00276RM room monitor (+ optional probe) — OOK PWM, ~420 µs = 1, ~232 µs = 0, ~632 µs sync; 88 bits sent 3
//  times: ID 16 · flags (bit 6 battery OK, bit 0 = 00275 vs 00276) · ID 8 · temperature 12 bits ((raw − 1000) / 10 °C) ·
//  probe type 2 · humidity 7 · probe data · CRC-16 (reflected poly 0xB2, init 0xD0) over all 11 bytes = 0. A majority of
//  the three copies when none checks alone.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool rm275(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::pwm(b.ook, 232, 420, 520, 0, 632);
    std::vector<std::vector<uint8_t>> cand;
    for (const Bits& w : rows) if (w.size() == 88) cand.push_back(bytesAt(w, 0, 11));
    if (cand.size() >= 3) {
        std::vector<uint8_t> m(11);
        for (size_t k = 0; k < 11; ++k) m[k] = uint8_t((cand[0][k] & cand[1][k]) | (cand[1][k] & cand[2][k]) | (cand[2][k] & cand[0][k]));
        cand.push_back(m);
    }
    for (const auto& by : cand) {
        if (wx::crc16le(by.data(), 11, 0xB2, 0xD0) != 0) continue;
        Reading r = wx::wxReading("AcuRite", (by[2] & 1) ? "00275RM" : "00276RM",
                                  wx::hex(unsigned(by[0] << 16 | by[1] << 8 | by[3]), 6));
        r.battOk = (by[2] & 0x40) ? 1 : 0;
        r.tempC = ((by[4] << 4 | by[5] >> 4) - 1000) / 10.0;
        r.hum = (by[6] & 0x1F) << 2 | by[7] >> 6;
        if (!wx::tempOk(r.tempC) || r.hum > 100) continue;
        const int probe = by[5] & 3;
        if (probe == 1) r.extra = (by[7] & 0xF) == 15 ? "probe: WET" : "probe: dry";
        else if (probe >= 2) { char e[32]; std::snprintf(e, sizeof e, "probe %.1f °C", (((by[7] & 0xF) << 8 | by[8]) - 1000) / 10.0); r.extra = e; }
        out.push_back(r);
        return true;
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (txr(b, out)) return true;
    if (rm275(b, out)) return true;
    if (th609(b, out)) return true;
    if (tx606(b, out)) return true;
    if (fridge(b, out)) return true;
    if (tx590(b, out)) return true;
    return false;
}

} // namespace acurite2
} // namespace vibeism
