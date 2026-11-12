// vibe_ism_weather6.h — V12 ISM decoder, weather pass 2026-11-11: Lidl Auriol (AFW 2 A1, AFT 77 B2, AHFL, HG02832 /
// Rubicson 48957), Vauno EN8822C (Auriol AFT 77 A1, Esun T21), AlectoV1 (Alecto WS3500/4500, Ventus, Unitec — the
// "Auriol protocol v2.0" PDF at tfd.hu), Prologue (Clas Ohlson; FreeTec NC-7104; Pearl NC-7159).
// Sources: the tfd.hu Auriol/Alecto protocol PDF where it covers a family; rtl_433 device-file field tables
// (auriol_*.c, vauno_en8822c.c, alecto.c, prologue.c) for FACTS only. Our own code throughout.
#pragma once
#include "vibe_ism_wxkit.h"

namespace vibeism {
namespace wx6 {

/** Two's-complement 12-bit temperature × 0.1 °C. */
inline double t12(unsigned raw) { int t = int(raw & 0xFFF); if (t & 0x800) t -= 0x1000; return t / 10.0; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AURIOL AFW 2 A1 — OOK PPM, ~576 µs space = 0, ~1536 µs = 1; 36 bits sent 12 times: ID 8 · battery OK · button ·
//  channel 2 · temperature 12 bits signed × 0.1 °C · 0xA · humidity 8. No check but the fixed nibble — so the copies must
//  agree (≥ 6) and the reading is shown once heard twice.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool afw2a1(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 576, 1536, 2012, 0, 1000, 0, 100);
    const int i = wx::repeatedRow(rows, 6, 36, 36);
    if (i < 0) return false;
    const Bits& w = rows[size_t(i)];
    if (bitsAt(w, 24, 4) != 0xA) return false;
    const int h = int(bitsAt(w, 28, 8));
    const double t = t12(bitsAt(w, 12, 12));
    if (h > 100 || t < -51.1 || t > 76.7 || bitsAt(w, 0, 8) == 0) return false;
    Reading r = wx::wxReading("Auriol", "AFW 2 A1", wx::hex(bitsAt(w, 0, 8), 2));
    r.battOk = w[8]; r.channel = int(bitsAt(w, 10, 2)) + 1; r.tempC = t; r.hum = h;
    r.confirm = true;
    out.push_back(r);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AURIOL AFT 77 B2 — OOK PPM, ~488 µs marks, ~488 µs space = 0, ~976 µs = 1; 68 bits = 17 nibbles: A · 5 · C · ID 8 ·
//  0 · flags (bit 3 = negative) · temperature 3 BCD digits × 0.1 °C · 00 · byte sum · LFSR digest (generator 0x83, key
//  0xEC) — both over the 6 bytes after the A nibble.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool aft77b2(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : wx::ppm(b.ook, 500, 920, 1104, 0, 800)) {
        if (w.size() != 68 || bitsAt(w, 0, 8) != 0xA5) continue;
        const auto f = bytesAt(w, 4, 8);
        if ((wx::addBytes(f.data(), 6) & 0xFF) != f[6] || wx::lfsrDigest8(f.data(), 6, 0x83, 0xEC) != f[7]) continue;
        const int d1 = int(bitsAt(w, 32, 4)), d2 = int(bitsAt(w, 36, 4)), d3 = int(bitsAt(w, 40, 4));
        if (d1 > 9 || d2 > 9 || d3 > 9) continue;
        Reading r = wx::wxReading("Auriol", "AFT 77 B2", wx::hex(f[1], 2));
        r.tempC = (bitsAt(w, 28, 4) & 8 ? -1 : 1) * (d1 * 100 + d2 * 10 + d3) / 10.0;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AURIOL AHFL 433B2 — OOK PPM, ~2.1 ms space = 0, ~4.15 ms = 1; 42 bits: ID · battery OK · button · channel 2 ·
//  temperature 12 bits signed × 0.1 °C · humidity 7 + 0 · 4 · 6-bit sum of the nine nibbles before.
//  VAUNO EN8822C (Auriol AFT 77 A1, Esun T21) — the same shape at 2 / 4 ms: ID · battery · ? · channel 2 · temperature
//  12 bits signed · humidity 7 · flags 4 · battery · 6-bit sum of nine nibbles. (The S3318P family shares the timing
//  and length; its frames start 00 and carry a CRC-4 — tried after these.)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool ahflVauno(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2050, 4100, 4600, 0, 1200, 0, 120);
    for (int need : {3, 2}) {
        const int i = wx::repeatedRow(rows, need, 42, 42);
        if (i < 0) continue;
        const auto by = bytesAt(rows[size_t(i)], 0, 6);
        unsigned sum = 0; for (int k = 0; k < 9; ++k) sum += wx::nib(by, k);
        const unsigned chk = (by[4] & 0xF) << 2 | by[5] >> 6;
        if (sum == 0 || (sum & 0x3F) != chk) continue;
        {   // ★ an S3318P-family frame (00 + CRC-4) is theirs, whatever the sum says
            const Bits& w = rows[size_t(i)];
            const auto s3 = bytesAt(w, 2, 5);
            if (bitsAt(w, 0, 2) == 0 && (wx::crc4(s3.data(), 4, 3, 0) ^ (s3[4] >> 4)) == (s3[4] & 0xF)) continue;
        }
        const bool ahfl = (by[4] & 0xF0) == 0x40 && !(by[3] & 1);
        Reading r = wx::wxReading(ahfl ? "Auriol" : "Vauno", ahfl ? "AHFL 433B2" : "EN8822C / AFT 77 A1", wx::hex(by[0], 2));
        r.channel = ((by[1] >> 4) & 3) + 1;
        r.tempC = t12(unsigned(by[1] & 0xF) << 8 | by[2]);
        r.hum = by[3] >> 1;
        if (ahfl) r.battOk = by[1] >> 7; else r.battOk = (by[4] & 0x10) ? 0 : 1;
        if (!wx::tempOk(r.tempC) || r.hum > 100) continue;
        r.confirm = need < 3;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  AURIOL HG02832 / HG05124A-DCF / Rubicson 48957 — OOK PWM, ~612 µs = 1, ~252 µs = 0 (after inversion), ~860 µs sync;
//  40 bits: ID · humidity · battery low · button · channel 2 · temperature 12 bits signed × 0.1 °C · check = CRC-8
//  (0x31, init 0x53) of the XOR of the first four bytes, XOR the fifth.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool hg02832(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : wx::pwm(b.ook, 252, 612, 750, 0, 860)) {
        if (w.size() != 40) continue;
        const auto by = bytesAt(w, 0, 5);
        const uint8_t d0 = uint8_t(by[0] ^ by[1] ^ by[2] ^ by[3]);
        if ((crc8(&d0, 1, 0x31, 0x53) ^ by[4]) != 0 || by[1] > 100) continue;
        Reading r = wx::wxReading("Auriol", "HG02832 / Rubicson 48957", wx::hex(by[0], 2));
        r.hum = by[1]; r.battOk = (by[2] & 0x80) ? 0 : 1; r.channel = ((by[2] >> 4) & 3) + 1;
        r.tempC = t12(unsigned(by[2] & 0xF) << 8 | by[3]);
        if (!wx::tempOk(r.tempC)) continue;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ALECTO V1 (WS3500 / WS4500, Ventus W155 / W044, Unitec W186-F) — OOK PPM, ~500 µs marks, ~2 ms = 0, ~4 ms = 1,
//  ~9 ms sync; 36-bit messages, bytes sent LSB first. ID 8 (2 of them the channel) · battery · type 2 (11 = wind / rain)
//  · button · 12 bits temperature signed × 0.1 °C + humidity BCD — or rain 16 bits × 0.25 mm, or wind 0.2 m/s, or
//  direction 9 bits + gust 0.2 m/s · a 4-bit check (the nibble sum, complemented, or + 7 for the rain type). A wind
//  transmission is two groups of four: speed, then direction + gust.
//  PROLOGUE (and FreeTec NC-7104, Pearl NC-7159, TFA 30.3240) — the same timing, no check: type 9 or 5 · ID 8 ·
//  battery OK · button · channel 2 · temperature 12 bits signed × 0.1 °C · humidity (0xCC = none). Seven copies.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool alectoCk(const std::vector<uint8_t>& b) {
    int cs = 0;
    for (int i = 0; i < 4; ++i) { const uint8_t t = wx::rev8(b[size_t(i)]); cs += (t & 0xF) + (t >> 4); }
    cs = ((b[1] & 0x7F) == 0x6C) ? cs + 7 : 0xF - cs;
    return wx::rev8(uint8_t((cs & 0xF) << 4)) == (b[4] >> 4);
}
inline bool alectoProlog(const Burst& b, std::vector<Reading>& out) {
    const auto rows = wx::ppm(b.ook, 2000, 4000, 7000, 0, 1200, 0, 120);
    std::vector<std::vector<uint8_t>> msgs;
    for (const Bits& w : rows) if (w.size() == 36 || w.size() == 37) msgs.push_back(bytesAt(w, 0, 5));
    if (msgs.size() < 2) return false;
    // AlectoV1: two checked messages that agree
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        if ((m[4] & 0xF) || !alectoCk(m) || m[0] == 0) continue;
        int same = 0; for (const auto& o : msgs) if (o == m) ++same;
        if (same < 2) continue;
        Reading r = wx::wxReading("Alecto", "V1", wx::hex(wx::rev8(m[0]), 2));
        r.channel = (m[0] >> 2) & 3;
        r.battOk = (m[1] & 0x80) ? 0 : 1;
        const int type = (m[1] & 0x60) >> 5;
        const bool rain = (m[1] & 0x0F) == 0x0C;
        if (type == 3 && rain) { r.model = "V1 rain"; r.rainMm = (wx::rev8(m[3]) << 8 | wx::rev8(m[2])) * 0.25; }
        else if (type == 3) {
            if ((m[1] & 0xE) == 0x8 && m[2] == 0) {
                r.model = "V1 wind"; r.windKmh = wx::rev8(m[3]) * 0.2 * 3.6;
                // the direction + gust group, if it is in this burst too
                for (const auto& o : msgs)
                    if (o[0] == m[0] && (o[1] & 0xE) == 0xE && !(o[4] & 0xF) && alectoCk(o)) {
                        r.gustKmh = wx::rev8(o[3]) * 0.2 * 3.6;
                        const int dir = wx::rev8(o[2]) << 1 | (o[1] & 1);
                        if (dir < 360) r.windDir = dir;
                        break;
                    }
            } else if ((m[1] & 0xE) == 0xE) {
                r.model = "V1 wind"; r.gustKmh = wx::rev8(m[3]) * 0.2 * 3.6;
                const int dir = wx::rev8(m[2]) << 1 | (m[1] & 1);
                if (dir < 360) r.windDir = dir;
            } else continue;
        } else {
            int t = (wx::rev8(m[1]) & 0xF0) | (wx::rev8(m[2]) << 8); if (t & 0x8000) t -= 0x10000;
            r.tempC = (t >> 4) / 10.0;
            const uint8_t hb = wx::rev8(m[3]);
            const int h = (hb >> 4) * 10 + (hb & 0xF);
            if (!wx::tempOk(r.tempC) || h > 100) continue;
            r.model = "V1 T/H"; if (h) r.hum = h;
        }
        out.push_back(r);
        return true;
    }
    // Prologue: three copies alike (the AlectoV1's check was tried first), type 9 or 5. ★ Two sensors in one burst are
    //  common (the corpus: a second Prologue's copies interleaved with the first's) — each is read.
    bool any = false;
    std::vector<std::vector<uint8_t>> done;
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        if ((m[0] >> 4) != 9 && (m[0] >> 4) != 5) continue;
        if (std::find(done.begin(), done.end(), m) != done.end()) continue;
        int same = 0; for (const auto& o : msgs) if (o == m) ++same;
        if (same < 3) continue;
        done.push_back(m);
        const double t = t12(unsigned(m[2]) << 4 | m[3] >> 4);
        const int h = (m[3] & 0xF) << 4 | m[4] >> 4;
        if (!wx::tempOk(t)) continue;
        Reading r = wx::wxReading("Prologue", "T/H (NC-7104 / NC-7159)", wx::hex(unsigned((m[0] & 0xF) << 4 | m[1] >> 4), 2));
        r.battOk = (m[1] & 8) ? 1 : 0; r.channel = (m[1] & 3) + 1; r.tempC = t;
        if (h != 0xCC && h <= 100) r.hum = h;
        r.confirm = true;
        out.push_back(r);
        any = true;
    }
    return any;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (aft77b2(b, out)) return true;
    if (hg02832(b, out)) return true;
    if (alectoProlog(b, out)) return true;
    if (ahflVauno(b, out)) return true;
    if (afw2a1(b, out)) return true;
    return false;
}

} // namespace wx6
} // namespace vibeism
