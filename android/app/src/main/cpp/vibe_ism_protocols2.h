// vibe_ism_protocols2.h — V12 ISM decoder: the weather / sensor / energy families, and decodeAll.
// ★★★ CLEAN ROOM — the same rule as vibe_ism_protocols.h: every decoder cites the published description it was written
//     from (docs/v12/ISM-DECODER.md §Protocols has the table); no GPL decoder source was read. Where a write-up left a
//     detail open, the choice made is marked ASSUMED and the doc lists it for a real-signal check.
#pragma once
#include "vibe_ism_protocols.h"

namespace vibeism {

/** FSK NRZ rows at `bitUs`, both polarities (which tone is "1" depends on the radio's spectrum orientation). */
inline std::vector<Bits> fskNrzRows(const Burst& b, double bitUs) {
    std::vector<Bits> out;
    if (b.fsk.empty() || bitUs <= 0) return out;
    for (int pol = 0; pol < 2; ++pol) {
        Runs rr = b.fsk;
        if (pol) for (auto& d : rr) d = -d;
        for (auto& row : sliceNrz(rr, bitUs, 40)) if (row.size() >= 40) out.push_back(row);
    }
    return out;
}
inline int evenParityOk(uint8_t v) { int n = 0; for (int i = 0; i < 8; ++i) n += (v >> i) & 1; return (n & 1) == 0; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FINE OFFSET WH1080 / WH1081 (and the many stations sold under other names) — OOK version
//  Source: Kevin Sangeelee, "Raspberry Pi reading WH1081 weather sensors using an RFM01 and RFM12b", SusaNET, 2012-08-17
//  (susa.net; web.archive.org copy). OOK PWM: narrow ~500 µs pulse = 1, wide ~1500 µs = 0, a ~1 ms gap after each;
//  preamble 0xFF then 10 bytes: type nibble (0xA sensor, 0xB DCF77 time) · 8-bit station ID · temperature 12 bits,
//  (raw − 400) / 10 °C · humidity % · average and gust wind × 0.34 m/s · 4 unknown · rain 12 bits × 0.3 mm · battery
//  nibble (non-zero = low) · direction 0–15 × 22.5° · CRC-8 poly 0x31 init 0 over bytes 0–8 (checked on the article's
//  three messages in test-ism).
//  FSK version (2012 on, 868.3 / 916 MHz): Frank, "WH1080 protocol V2 – FSK", sevenwatt.com, 2013-03-02 — preamble
//  AA AA AA, sync 2D D4, the same 10 bytes except the temperature: 12 bits ×10 °C, MSB = sign, no offset; status bit 3 =
//  battery low. Bit rate not published: measured from the preamble (shortest run). Wind scale ASSUMED as the OOK one.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace fineoffset {

inline bool wh1080Fields(const std::vector<uint8_t>& by, bool fsk, Reading& r) {
    if ((by[0] >> 4) != 0xA) return false;
    if (crc8(by.data(), 9, 0x31, 0) != by[9]) return false;
    r.kind = "weather"; r.make = "Fine Offset"; r.model = fsk ? "WH1080 (FSK)" : "WH1080";
    char id[4]; std::snprintf(id, sizeof id, "%02X", ((by[0] & 0xF) << 4) | (by[1] >> 4)); r.pubId = id;
    const int raw = ((by[1] & 0xF) << 8) | by[2];
    r.tempC = fsk ? ((raw & 0x800) ? -1 : 1) * (raw & 0x7FF) / 10.0 : (raw - 400) / 10.0;
    r.hum = fsk ? (by[3] & 0x7F) : by[3];
    r.windKmh = by[4] * 0.34 * 3.6;
    r.gustKmh = by[5] * 0.34 * 3.6;
    r.rainMm = (((by[6] & 0xF) << 8) | by[7]) * 0.3;
    const int st = by[8] >> 4;
    r.battOk = fsk ? ((st & 0x8) ? 0 : 1) : (st ? 0 : 1);
    r.windDir = (by[8] & 0xF) * 22.5;
    if (r.tempC < -40 || r.tempC > 70 || r.hum > 100) return false;
    return true;
}

inline bool decodeWh1080(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : slicePwm(b.ook, 500, 1500, 4000, true)) {
        for (size_t s = 0; s + 88 <= row.size(); ++s) {
            if (bitsAt(row, s, 8) != 0xFF) continue;
            const auto by = bytesAt(row, s + 8, 10);
            Reading r;
            if (wh1080Fields(by, false, r)) { out.push_back(r); return true; }
        }
    }
    const double bit = tpms::halfEstimate(b.fsk);
    for (const Bits& row : fskNrzRows(b, bit)) {
        for (size_t s = findBits(row, 0x2DD4, 16); s != size_t(-1); s = findBits(row, 0x2DD4, 16, s + 1)) {
            if (s + 16 + 80 > row.size()) break;
            const auto by = bytesAt(row, s + 16, 10);
            Reading r;
            if (wh1080Fields(by, true, r)) { out.push_back(r); return true; }
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FINE OFFSET WH2 temperature/humidity (Telldus, Agimex, Cresta …)
//  Source: Luc Small, "Hacking the WH2 Wireless Weather Station Outdoor Sensor – Part 2: Protocol Specification",
//  lucsmall.com, 2012-04-29. OOK, 500 µs pulse = 1, 1500 µs = 0, 1 ms gap after every pulse; 8 preamble 1s, then 12-bit
//  ID (its MSB always 0 — the end of the preamble), 12-bit temperature in tenths (MSB = sign, 1 = negative), 8-bit
//  humidity, CRC-8 x⁸+x⁵+x⁴+1 (0x31) over the 4 payload bytes — init ASSUMED 0 (as the WH1080's).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool decodeWh2(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : slicePwm(b.ook, 500, 1500, 4000, true)) {
        for (size_t s = 6; s + 40 <= row.size(); ++s) {
            if (row[s] != 0 || bitsAt(row, s - 6, 6) != 0x3F) continue;
            const auto by = bytesAt(row, s, 5);
            if (crc8(by.data(), 4, 0x31, 0) != by[4]) continue;
            const int raw = ((by[1] & 0xF) << 8) | by[2];
            Reading r;
            r.kind = "weather"; r.make = "Fine Offset"; r.model = "WH2";
            char id[5]; std::snprintf(id, sizeof id, "%03X", (by[0] << 4) | (by[1] >> 4)); r.pubId = id;
            r.tempC = ((raw & 0x800) ? -1 : 1) * (raw & 0x7FF) / 10.0;
            r.hum = by[3];
            if (r.tempC < -40 || r.tempC > 70 || r.hum > 100) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace fineoffset

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  NEXUS-STYLE temperature/humidity (Nexus, Sencor, Digoo, Rubicson-alikes, many supermarket stations)
//  Source: the README of aquaticus/nexus433 (prose only; the repository's code was not read). OOK PPM: each bit a
//  500 µs high then 1000 µs low = 0 / 2000 µs = 1; a 4000 µs low before every 36-bit frame; 12 frames per transmission.
//  ID 8 · battery 1 (1 = OK) · 0 · channel 2 (0-based) · temperature 12 (signed, °C × 10 — two's complement ASSUMED;
//  the README's wording is ambiguous) · 1111 · humidity 8. No checksum: two identical frames and the 1111 nibble.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace nexus {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const auto rows = slicePpm(b.ook, 1000, 2000, 3000, false);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].size() != 36) continue;
        int same = 0;
        for (size_t j = 0; j < rows.size(); ++j) if (j != i && rows[j] == rows[i]) ++same;
        if (same < 1) continue;
        const Bits& w = rows[i];
        if (bitsAt(w, 24, 4) != 0xF || w[9] != 0) continue;
        int t = int(bitsAt(w, 12, 12)); if (t & 0x800) t -= 0x1000;
        const int h = int(bitsAt(w, 28, 8));
        if (h > 100 || t < -400 || t > 700) continue;
        Reading r;
        r.kind = "weather"; r.make = "Nexus-type"; r.model = "temperature/humidity";
        char id[4]; std::snprintf(id, sizeof id, "%02X", bitsAt(w, 0, 8)); r.pubId = id;
        r.battOk = w[8] ? 1 : 0;
        r.channel = int(bitsAt(w, 10, 2)) + 1;
        r.tempC = t / 10.0;
        if (h > 0) r.hum = h;
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace nexus

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  LA CROSSE TECHNOLOGY IT+ (TX29-IT, TX29DTH-IT, TX35, TFA Dostmann sensors) — 868.3 MHz FSK, 17.241 kbps NRZ
//  Sources: "Fred", "TX29 Protocol", fredboboss.free.fr/articles/tx29.php, 2011-06-11 (40-bit layout, CRC); the
//  sevenwatt.com WH1080 V2 page and its comments (AA preamble, 2D D4 sync, "the same CRC as IT+"). 40 bits: length
//  nibble 9 · ID 6 · new battery 1 · unused 1 · temperature 12 bits = BCD of (T + 40) × 10 · weak battery 1 ·
//  humidity 7 (106 = no hygrometer) · CRC-8 0x31 init 0 over the first 4 bytes.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace lacrosse {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : fskNrzRows(b, 1e6 / 17241.0)) {
        for (size_t s = findBits(row, 0x2DD4, 16); s != size_t(-1); s = findBits(row, 0x2DD4, 16, s + 1)) {
            if (s + 16 + 40 > row.size()) break;
            const auto by = bytesAt(row, s + 16, 5);
            if ((by[0] >> 4) != 9 || crc8(by.data(), 4, 0x31, 0) != by[4]) continue;
            const int d1 = by[1] & 0xF, d2 = by[2] >> 4, d3 = by[2] & 0xF;
            if (d1 > 9 || d2 > 9 || d3 > 9) continue;
            Reading r;
            r.kind = "weather"; r.make = "La Crosse"; r.model = "IT+ (TX29/TX35)";
            const int id = ((by[0] & 0xF) << 2) | (by[1] >> 6);
            char ib[4]; std::snprintf(ib, sizeof ib, "%02X", id); r.pubId = ib;
            r.tempC = (d1 * 100 + d2 * 10 + d3) / 10.0 - 40.0;
            const int h = by[3] & 0x7F;
            if (h <= 100) r.hum = h;
            r.battOk = (by[3] & 0x80) ? 0 : 1;
            if ((by[1] >> 5) & 1) r.extra = "new battery";
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace lacrosse

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ACURITE — Ray Wang, "Reverse Engineer Wireless Temperature / Humidity / Rain Sensors", Parts 1–3, rayshobby.net,
//  2014 (CC BY-SA 3.0 text). Facts only; the checksums the author left open were identified by us on his examples.
//   • 00592TX-family T/H (Part 2): sync 4 square waves of ~1.2 ms; PWM 1 = 400 µs H + 225 L, 0 = 250 H + 400 L; 56 bits,
//     3 repeats. Bytes 0–1 ID/channel, byte 3 humidity (bit 7 parity), byte 4 low nibble + byte 5 low 7 bits = temperature
//     raw, °C = (raw − 1000) / 10 (offset ASSUMED — the author's empirical formula gives 1005), byte 6 = sum of bytes
//     0–5 (ours, holds for all three examples); bytes 2–5 even parity.
//   • 00899 rain gauge (Part 3): the same PWM and sync; 64 bits; bytes 0–2 ID, bytes 3–6 tip count (7 bits each, bit 7
//     parity), byte 7 = sum of bytes 0–6. The mm per tip is not published — shown as tips.
//   • 00782 temperature (Part 1): PPM — 540 µs high, then 2.1 ms low = 0 / 4.1 ms = 1, 9 ms sync; 32 bits, 8 repeats;
//     byte 0 ID, temperature 12-bit two's complement × 0.1 °C (byte 1 low nibble + byte 2); byte 3 unknown — no check,
//     so three identical frames are required.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace acurite {

inline bool decodePwm(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : slicePwm(b.ook, 240, 400, 1000, false, 0.35)) {
        // the rain gauge first: 64 bits
        for (size_t s = 0; s + 64 <= row.size(); ++s) {
            const auto by = bytesAt(row, s, 8);
            unsigned sum = 0; for (int i = 0; i < 7; ++i) sum += by[size_t(i)];
            if ((sum & 0xFF) != by[7]) continue;
            bool par = true; for (int i = 3; i < 7; ++i) par = par && evenParityOk(by[size_t(i)]);
            if (!par || (by[0] == 0 && by[1] == 0 && by[2] == 0)) continue;
            const uint32_t tips = uint32_t(by[3] & 0x7F) << 21 | uint32_t(by[4] & 0x7F) << 14 | uint32_t(by[5] & 0x7F) << 7 | (by[6] & 0x7F);
            Reading r;
            r.kind = "weather"; r.make = "AcuRite"; r.model = "00899 rain gauge";
            char id[8]; std::snprintf(id, sizeof id, "%02X%02X%02X", by[0], by[1], by[2]); r.pubId = id;
            r.extra = std::to_string(tips) + (tips == 1 ? " bucket tip" : " bucket tips");
            out.push_back(r);
            return true;
        }
        for (size_t s = 0; s + 56 <= row.size(); ++s) {
            const auto by = bytesAt(row, s, 7);
            unsigned sum = 0; for (int i = 0; i < 6; ++i) sum += by[size_t(i)];
            if ((sum & 0xFF) != by[6]) continue;
            bool par = true; for (int i = 2; i < 6; ++i) par = par && evenParityOk(by[size_t(i)]);
            if (!par) continue;
            const int h = by[3] & 0x7F;
            const int raw = ((by[4] & 0x0F) << 7) | (by[5] & 0x7F);
            const double t = (raw - 1000) / 10.0;
            if (h > 100 || t < -40 || t > 70) continue;
            Reading r;
            r.kind = "weather"; r.make = "AcuRite"; r.model = "592TX-type T/H";
            char id[8]; std::snprintf(id, sizeof id, "%02X%02X", by[0], by[1]); r.pubId = id;
            r.tempC = t; r.hum = h;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

inline bool decode782(const Burst& b, std::vector<Reading>& out) {
    const auto rows = slicePpm(b.ook, 2100, 4100, 6500, false, 0.3);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].size() != 32) continue;
        int same = 0;
        for (size_t j = 0; j < rows.size(); ++j) if (j != i && rows[j] == rows[i]) ++same;
        if (same < 2) continue;
        int t = int(bitsAt(rows[i], 12, 12)); if (t & 0x800) t -= 0x1000;
        if (t < -400 || t > 700) continue;
        Reading r;
        r.kind = "weather"; r.make = "AcuRite"; r.model = "00782 temperature";
        char id[4]; std::snprintf(id, sizeof id, "%02X", bitsAt(rows[i], 0, 8)); r.pubId = id;
        r.tempC = t / 10.0;
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace acurite

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  CURRENT COST energy transmitters (EnviR / Envi clip-on sensors) — 433.9 MHz FSK, 3918 bps
//  Source: Jack Kelly, "Technical details of Current Cost RF protocol", rfm_edf_ecomanager wiki (GitHub wiki prose).
//  Preamble 55/AA, sync 2D D4, then 16 Manchester-coded bytes (10 = 1, 01 = 0; 00/11 illegal — the integrity check) =
//  8 data bytes: byte 0 bit 7 pairing, bit 6 type (0 = electricity), bits 3–0 address 11–8 · byte 1 address 7–0 ·
//  three 16-bit words, MSB = valid, 15-bit watts.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace currentcost {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : fskNrzRows(b, 1e6 / 3918.0)) {
        for (size_t s = findBits(row, 0x2DD4, 16); s != size_t(-1); s = findBits(row, 0x2DD4, 16, s + 1)) {
            if (s + 16 + 128 > row.size()) break;
            Bits d; bool ok = true;
            for (size_t k = 0; k < 64 && ok; ++k) {
                const uint8_t a = row[s + 16 + 2 * k], c = row[s + 17 + 2 * k];
                if (a == c) ok = false; else d.push_back(a);   // 10 = 1, 01 = 0
            }
            if (!ok) continue;
            const auto by = bytesAt(d, 0, 8);
            if (by[0] & 0x40) continue;                         // a counter packet (gas / water / impulses): not shown yet
            Reading r;
            r.kind = "energy"; r.make = "Current Cost"; r.model = "transmitter";
            char id[5]; std::snprintf(id, sizeof id, "%03X", ((by[0] & 0xF) << 8) | by[1]); r.pubId = id;
            double w = 0; int valid = 0;
            for (int k = 0; k < 3; ++k) {
                const int v = (by[size_t(2 + 2 * k)] << 8) | by[size_t(3 + 2 * k)];
                if (v & 0x8000) { w += v & 0x7FFF; ++valid; }
            }
            if (!valid) continue;
            r.watts = w;
            if (by[0] & 0x80) r.extra = "pairing";
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace currentcost

/** Every decoder, in turn, on every smoothing of the burst (finest first). The first that accepts it wins (a burst is one
 *  transmitter). */
inline void decodeOne(const Burst& b, std::vector<Reading>& out);
inline void decodeAll(const Burst& b, std::vector<Reading>& out) {
    if (b.ookAll.empty() && b.fskAll.empty()) { decodeOne(b, out); return; }   // a hand-built burst (the tests)
    const size_t n = std::max(b.ookAll.size(), b.fskAll.size());
    Burst v = b;
    for (size_t i = 0; i < n; ++i) {
        v.ook = i < b.ookAll.size() ? b.ookAll[i] : Runs();
        v.fsk = i < b.fskAll.size() ? b.fskAll[i] : Runs();
        const size_t before = out.size();
        decodeOne(v, out);
        if (out.size() > before) return;
    }
}
inline void decodeOne(const Burst& b, std::vector<Reading>& out) {
    if (oregon::decode(b, out)) return;
    if (fineoffset::decodeWh1080(b, out)) return;
    if (fineoffset::decodeWh2(b, out)) return;
    if (nexus::decode(b, out)) return;
    if (acurite::decodePwm(b, out)) return;
    if (acurite::decode782(b, out)) return;
    if (lacrosse::decode(b, out)) return;
    if (currentcost::decode(b, out)) return;
    if (keeloq::decode(b, out)) return;
    if (fixedcode::decode(b, out)) return;
    if (tpms::decodeA(b, out)) return;
    if (tpms::decodeB(b, out)) return;
}

} // namespace vibeism
