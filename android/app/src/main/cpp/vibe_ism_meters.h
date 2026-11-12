// vibe_ism_meters.h — V12 ISM decoder: METERS AND MONITORS — oil-tank gauges, electricity monitors, utility meters.
//
// ★★★ SOURCES (2026-11-11, Stuart: "we dont want to copy but if we can take inspiration"). Frame FACTS — layout, timing,
//     line code, sync word, check polynomial, scaling — were learned from public protocol descriptions; where the only
//     description is the header comment of a GPL decoder (rtl_433, src/devices/*.c), that comment was read for the facts
//     and nothing else: no code, table, comment or identifier is copied, every decoder here is written in our own style
//     on our own slicers. Each family cites where its facts come from. Real-signal checked on the rtl_433_tests
//     recordings (test INPUT only — scripts/ism-corpus.mjs; never in the repository).
// ★ Meters may show their serial (Stuart, 2026-10-25); an encrypted payload stays encrypted.
#pragma once
#include "vibe_ism_kit.h"

namespace vibeism {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  OIL-TANK GAUGES (ultrasonic, on the tank lid: the distance down to the oil) — Kingspan / Sensor Systems Watchman
//  Sonic, Apollo Ultrasonic (Smart / Standard), Tekelek TEK377, Beckett Rocket. FSK (the TEK377A on 915 MHz: OOK).
//  Facts: the protocol notes in rtl_433's oil_watchman.c (D. Woodhouse), oil_smart.c and oil_standard.c (C. Zuckschwerdt,
//  device analysis by StarMonkey1 / havochome) and oil_watchman_advanced.c — read for the frame facts only.
//   • WATCHMAN SONIC: NRZ chips of 1 ms; a 111000 start, then 64 Manchester bits (01 = 1): ID 32 · flags 8 (bit 0 =
//     rebinding, bit 3 = leak/theft alarm) · a 6-bit temperature code (T = (145 − 5·code) / 3 °C) + depth 10 bits (cm;
//     0 = no reading) — while rebinding the depth byte is a countdown · CRC-8 0x31 reflected, init 0, over bytes 0–6;
//     after the data the last chip repeats twice (the frame's end).
//   • APOLLO SMART: 500 µs chips; preamble ending 0x5558, 64 Manchester bits: ID 32 · status 8 (0x40 rapid, 0x04
//     battery OK) · counter 4 · 3 unknown · depth 9 bits (cm) · CRC-8 0x31 reflected over all 8 bytes = 0.
//   • APOLLO STANDARD / TEK377: 500 µs chips; preamble 0x555D or 0x5562 whose last two chips are the first Manchester
//     pair; 32 Manchester bits (the TEK377E adds 8 zero bits): ID 16 · flags 8 (bit 0 rebinding, bit 1 depth's 9th bit,
//     bit 3 alarm) · depth 8 (cm, ≤ 305). NO CHECK: the 32 clean Manchester pairs, the violation that ends them, a
//     plausible depth — and heard twice alike before it is shown (Reading::confirm).
//   • WATCHMAN SONIC ADVANCED / TEK608: 500 µs chips, AA… 2D D4, length 0x0E, model 0401 / 0106 / 0101, serial 24 (the
//     label's), status, temperature (byte − 0x48) / 2 °C, depth 12 bits (cm), version 4 bytes, CRC-16 0x8005 init 0 over
//     the 15 bytes from the length (some units send one stray bit before the CRC).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace oiltank {

inline void depthWords(Reading& r, int cm, bool binding) {
    if (binding) r.extra = "pairing";
    else if (cm == 0) r.extra = "no echo";
    else r.extra = std::to_string(cm) + " cm to the oil";
}

inline bool sonic(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : kit::nrzRowsAny(b, 1000, 12, 80)) {
        for (size_t s : kit::findAll(row, 0x38, 6)) {
            size_t end = 0;
            const Bits d = kit::manchesterAt(row, s + 6, 64, true, &end);
            if (d.size() != 64 || end + 2 > row.size()) continue;
            const auto by = bytesAt(d, 0, 8);
            if (row[end] != (by[7] & 1) || row[end + 1] != (by[7] & 1)) continue;   // the end: the last chip twice more
            if (kit::crc8r(by.data(), 7, 0x31, 0) != by[7]) continue;
            const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
            const int code = by[5] >> 2;
            const bool bind = by[4] & 1;
            const int depth = bind ? 0 : ((by[5] & 3) << 8 | by[6]);
            Reading r;
            r.kind = "sensor"; r.make = "Oil tank gauge"; r.model = "Watchman Sonic";
            char ib[12]; std::snprintf(ib, sizeof ib, "%08X", id); r.pubId = ib;
            r.tempC = (145.0 - 5.0 * code) / 3.0;
            depthWords(r, depth, bind);
            if (by[4] & 0x08) r.extra += r.extra.empty() ? "leak / theft alarm" : " \xc2\xb7 leak / theft alarm";
            r.fact("id", (long long)id); r.fact("depth_cm", (long long)depth); r.fact("flags", (long long)by[4]);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

inline bool smart(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : kit::nrzRowsAny(b, 500, 12, 140)) {
        for (size_t s : kit::findAll(row, 0x5558, 16)) {
            const Bits d = kit::manchesterAt(row, s + 16, 64);
            if (d.size() != 64) continue;
            const auto by = bytesAt(d, 0, 8);
            if (kit::crc8r(by.data(), 8, 0x31, 0) != 0) continue;
            if ((by[0] | by[1] | by[2] | by[3]) == 0) continue;
            const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
            const int depth = (by[5] & 1) << 8 | by[6];
            Reading r;
            r.kind = "sensor"; r.make = "Oil tank gauge"; r.model = "Apollo Ultrasonic Smart";
            char ib[12]; std::snprintf(ib, sizeof ib, "%08X", id); r.pubId = ib;
            depthWords(r, depth, false);
            r.battOk = (by[4] & 0x04) ? 1 : 0;
            r.fact("id", (long long)id); r.fact("depth_cm", (long long)depth);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

inline bool standardRows(const std::vector<Bits>& rows, std::vector<Reading>& out) {
    for (const Bits& row : rows) {
        for (uint32_t pre : {0x555Du, 0x5562u}) {
            for (size_t s : kit::findAll(row, pre, 16)) {
                // ★ a clipped Apollo SMART frame carries a STANDARD-looking sync inside its 555558 preamble: not ours
                if (s >= 8 && bitsAt(row, s - 8, 24) == 0x555558) continue;
                size_t end = 0;
                const Bits d = kit::manchesterAt(row, s + 14, 41, true, &end);
                if (d.size() < 32 || d.size() > 40 || bitsAt(d, 32, int(d.size()) - 32) != 0) continue;
                if (d.size() == 41 || end + 1 >= row.size()) continue;   // ★ it must END in a violation, not run on
                const auto by = bytesAt(d, 0, 4);
                const bool bind = by[2] & 1;
                const int depth = bind ? 0 : ((by[2] & 0x02) << 7 | by[3]);
                if (depth > 305) continue;
                if (by[0] == 0 && by[1] == 0) continue;
                Reading r;
                r.kind = "sensor"; r.make = "Oil tank gauge"; r.model = "Apollo Ultrasonic / Tekelek TEK377";
                char ib[8]; std::snprintf(ib, sizeof ib, "%04X", by[0] << 8 | by[1]); r.pubId = ib;
                depthWords(r, depth, bind);
                if (by[2] & 0x08) r.extra += " \xc2\xb7 alarm";
                r.confirm = true;   // ★ no checksum in the frame
                r.fact("id", (long long)(by[0] << 8 | by[1])); r.fact("depth_cm", (long long)depth);
                out.push_back(r);
                return true;
            }
        }
    }
    return false;
}
inline bool standard(const Burst& b, std::vector<Reading>& out) {
    return standardRows(kit::nrzRowsAny(b, 500, 12, 78), out);
}

inline bool advanced(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : kit::nrzRowsAny(b, 500, 24, 150)) {
        for (size_t s : kit::findAll(row, 0x2DD40E, 24)) {
            if (s + 16 + 8 * 17 > row.size() + 1) continue;
            const auto by = bytesAt(row, s + 16, 17);   // length … CRC (+ the stray bit's byte)
            const uint16_t c = crc16(by.data(), 15, 0x8005, 0);
            const uint16_t m1 = uint16_t(by[15] << 8 | by[16]);
            const uint16_t m2 = uint16_t(bitsAt(row, s + 16 + 15 * 8 + 1, 16));
            if (c != m1 && c != m2) continue;
            const int model = by[1] << 8 | by[2];
            if (model != 0x0401 && model != 0x0106 && model != 0x0101) continue;
            const uint32_t serial = uint32_t(by[3]) << 16 | uint32_t(by[4]) << 8 | by[5];
            const int depth = (by[9] & 0x0F) << 8 | by[10];
            Reading r;
            r.kind = "sensor"; r.make = "Oil tank gauge"; r.model = model == 0x0401 ? "Watchman Sonic Advanced" : "Tekelek";
            r.pubId = std::to_string(serial);
            r.tempC = (int(by[7]) - 0x48) / 2.0;
            depthWords(r, depth, false);
            r.fact("id", (long long)serial); r.fact("depth_cm", (long long)depth);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    return sonic(b, out) || smart(b, out) || advanced(b, out) || standard(b, out);
}

} // namespace oiltank

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  OPENENERGYMONITOR emonTx (and any JeeLib RF12 node) — FSK NRZ at ~49.2 kbit/s (RFM12B 49.23 k, RFM69 49.26 k)
//  Source: Jean-Claude Wippler, "RF12 packet format and design", jeelabs.org, 2011-06-09 (the packet: preamble AA AA AA,
//  SYN 2D, group byte, header (C, D, A flags + 5-bit node), length, data, CRC-16 over group..data — the standard
//  CRC-16/IBM reflected, poly 0xA001, init 0xFFFF, sent low byte first); the emonTx v3 payload from openenergymonitor.org
//  (13 little-endian words: CT1–CT4 watts · Vrms × 100 · 6 temperatures × 10 (300.0 = none) · a 32-bit pulse count).
//  The facts were also checked against rtl_433's emontx.c notes. Group 0xD2 (210) is the emonTx default.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace emontx {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (const Bits& row : kit::nrzRows(b.fsk, 2e6 / (49230 + 49261), 160, true, 200)) {   // ★ 160: the zero words run long
        for (size_t s : kit::findAll(row, 0x2DD2, 16)) {
            if (s + 8 + 8 * 31 > row.size()) break;
            const auto by = bytesAt(row, s + 8, 31);   // group, header, length, 26 data, CRC lo, CRC hi
            if ((by[1] & 0xE0) != 0 || by[2] != 26) continue;
            const uint16_t crc = kit::crc16r(by.data(), 29, 0xA001, 0xFFFF);
            if (crc != uint16_t(by[29] | by[30] << 8)) continue;
            auto w = [&](int i) { return int(int16_t(uint16_t(by[size_t(3 + 2 * i)] | by[size_t(4 + 2 * i)] << 8))); };
            const int node = by[1] & 0x1F;
            Reading r;
            r.kind = "energy"; r.make = "OpenEnergyMonitor"; r.model = "emonTx";
            r.pubId = std::to_string(node);
            const int ct[4] = {w(0), w(1), w(2), w(3)};
            r.watts = ct[0];
            const double vrms = (w(4) & 0xFFFF) / 100.0;
            std::string ex;
            for (int i = 1; i < 4; ++i) if (ct[i]) ex += (ex.empty() ? "" : " \xc2\xb7 ") + std::string("CT") + char('1' + i) + " " + std::to_string(ct[i]) + " W";
            if (vrms > 0) ex += (ex.empty() ? "" : " \xc2\xb7 ") + kit::fmt1v(vrms) + " V";
            r.extra = ex;
            for (int i = 0; i < 6; ++i) if (w(5 + i) != 3000) { r.tempC = w(5 + i) / 10.0; break; }
            const uint32_t pulse = uint32_t(w(11) & 0xFFFF) | uint32_t(w(12) & 0xFFFF) << 16;
            r.fact("node", (long long)node);
            r.fact("ct1", (long long)ct[0]); r.fact("ct2", (long long)ct[1]); r.fact("ct3", (long long)ct[2]); r.fact("ct4", (long long)ct[3]);
            r.fact("batt_Vrms", vrms); r.fact("pulse", (long long)pulse);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace emontx

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  EFERGY e2 classic / e2CT clamp and EFERGY optical (meter-LED) transmitters — ~433.55 MHz FSK, PWM on the mark:
//  a ~500 µs sync mark, then marks of ~64 µs = 1 and ~136 µs = 0, ~200 µs a bit (the line can arrive inverted: the
//  frame always opens with 0000, so 1111 means invert it).
//  Facts: the protocol notes in rtl_433's efergy_e2_classic.c (R. Högberg; the power arithmetic credited there to
//  N. Elijah's EfergyRPI) and efergy_optical.c — read for the frame facts only.
//   • e2 classic: 8 bytes — start 00 · ID 16 (low byte first) · flags (bit 7 learn, bit 6 battery OK, bits 5–4 =
//     interval (n + 1) × 6 s) · current mantissa 16 · exponent byte e (amps = mantissa / 2^(15 − (int8)e)) · sum of
//     bytes 0–6 (mod 256).
//   • optical: 12 bytes — ID 24 · byte 3 bits 5–4 = interval (n + 1) × 30 s · byte 8 = LED pulses counted · CRC-16
//     (CCITT poly 0x1021, init 0) over bytes 0–9. kWh per pulse depends on the meter (the transmitter does not say).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace efergy {

/** The PWM bits after each sync mark (≥ 350 µs), from both FSK polarities and the envelope. */
inline std::vector<Bits> rows(const Burst& b) {
    std::vector<Bits> out;
    auto scan = [&](const Runs& r, bool flip) {
        for (size_t i = 0; i < r.size(); ++i) {
            const int m = flip ? -r[i] : r[i];
            if (m < 350 || m > 900) continue;
            Bits row;
            for (size_t k = i + 1; k + 1 < r.size(); k += 2) {
                const int sp = flip ? r[k] : -r[k], mk = flip ? -r[k + 1] : r[k + 1];
                if (sp <= 0 || sp > 300 || mk <= 0) break;
                if (mk >= 30 && mk < 100) row.push_back(1); else if (mk >= 100 && mk < 220) row.push_back(0); else break;
            }
            if (row.size() >= 60) out.push_back(row);
        }
    };
    for (int f = 0; f < 2; ++f) scan(b.fsk, f);
    scan(b.ook, false);
    return out;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (Bits row : rows(b)) {
        if (bitsAt(row, 0, 4) == 0xF) row = invert(row);
        if (bitsAt(row, 0, 4) != 0) continue;
        if (row.size() >= 96 && row.size() <= 100) {          // optical
            const auto by = bytesAt(row, 0, 12);
            if ((by[8] | by[9] | by[10] | by[11]) == 0) continue;
            if (crc16(by.data(), 10, 0x1021, 0) != uint16_t(by[10] << 8 | by[11])) continue;
            const uint32_t id = uint32_t(by[0]) << 16 | uint32_t(by[1]) << 8 | by[2];
            const int secs = (((by[3] & 0x30) >> 4) + 1) * 30;
            Reading r;
            r.kind = "energy"; r.make = "Efergy"; r.model = "optical sensor";
            r.pubId = std::to_string(id);
            r.extra = std::to_string(by[8]) + " meter pulses in " + std::to_string(secs) + " s";
            r.fact("id", (long long)id); r.fact("pulsecount", (long long)by[8]);
            out.push_back(r);
            return true;
        }
        if (row.size() >= 64 && row.size() <= 65) {           // e2 classic
            const auto by = bytesAt(row, 0, 8);
            int zeros = 0; for (uint8_t v : by) zeros += v == 0;
            if (zeros > 5) continue;
            const unsigned sum = kit::sumBytes(by.data(), 7);
            if (sum == 0 || (sum & 0xFF) != by[7]) continue;
            const int fact = 15 - int(int8_t(by[6]));
            if (fact < 7 || fact > 23) continue;
            const double amps = double(by[4] << 8 | by[5]) / double(1 << fact);
            const int id = by[2] << 8 | by[1];
            Reading r;
            r.kind = "energy"; r.make = "Efergy"; r.model = "e2 classic";
            r.pubId = std::to_string(id);
            r.extra = kit::fmt1v(amps, 2) + " A";
            if (by[3] & 0x80) r.extra += " \xc2\xb7 pairing";
            r.battOk = (by[3] & 0x40) ? 1 : 0;
            r.fact("id", (long long)id); r.fact("current", amps); r.fact("interval", (long long)((((by[3] & 0x30) >> 4) + 1) * 6));
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace efergy

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  ITRON ERT (encoder receiver transmitter) utility meters, 902–928 MHz — SCM and SCM+. OOK Manchester, ~30.5 µs chips
//  (16.384 kbit/s). Sources: bemasher, rtlamr wiki "Protocol" (github.com/bemasher/rtlamr/wiki/Protocol): SCM = a 21-bit
//  preamble 0x1F2A60, then ID high bits · tamper · ERT type · encoder tamper · 24-bit consumption · ID low 24 bits ·
//  BCH CRC, generator 0x6F63, over the 10 bytes from the third (0 when right); SCM+ = sync 0x16A31E, protocol ID ·
//  endpoint type · 32-bit endpoint ID · 32-bit consumption · 16-bit tamper · CRC-16 (CCITT poly; rtl_433's scmplus.c
//  gives the 0x0971 start that matches it) over bytes 2–13. Meter type from the endpoint type's low nibble (rtlamr's
//  "Compatible meters" page). IDM / NetIDM (interval data): sync 0x16A31C, 92 bytes — endpoint type byte 6, ID bytes 7–10,
//  last consumption bytes 27–30, CRC-16 (CCITT poly, start 0xD895 per rtl_433's ert_idm.c) over bytes 2–87 = bytes 88–89.
//  ★ The meter's serial is shown (Stuart, 2026-10-25).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace ert {

inline const char* meterType(int t) {
    switch (t & 0xF) {
        case 4: case 5: case 7: case 8: return "Electric";
        case 0: case 1: case 2: case 9: case 12: return "Gas";
        case 3: case 11: case 13: return "Water";
        default: return nullptr;
    }
}

/** Manchester bit rows of a burst at ~30.5 µs chips: both slices, both polarities, both pair phases, both conventions. */
inline std::vector<Bits> rows(const Burst& b) {
    std::vector<Bits> out;
    for (const Bits& chips : kit::nrzRowsAny(b, 30.52, 8, 120))
        for (int ph = 0; ph < 2; ++ph) {
            // a row may hold several Manchester stretches: restart after each violation
            size_t i = size_t(ph);
            while (i + 1 < chips.size()) {
                size_t end = 0;
                Bits d = kit::manchesterAt(chips, i, 4000, true, &end);
                if (d.size() >= 60) { out.push_back(d); out.push_back(invert(d)); }
                i = end + 1;
            }
        }
    return out;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const auto rs = rows(b);
    for (const Bits& d : rs) {
        for (size_t s : kit::findAll(d, 0x1F2A60, 21)) {      // SCM
            if (s + 96 > d.size()) break;
            const auto by = bytesAt(d, s, 12);
            if (!by[0] && !by[1] && !by[2] && !by[3]) continue;
            if (crc16(by.data() + 2, 10, 0x6F63, 0) != 0) continue;
            const uint32_t id = uint32_t(by[2] & 0x06) << 23 | uint32_t(by[7]) << 16 | uint32_t(by[8]) << 8 | by[9];
            const int type = (by[3] >> 2) & 0xF;
            const uint32_t cons = uint32_t(by[4]) << 16 | uint32_t(by[5]) << 8 | by[6];
            Reading r;
            r.kind = "energy"; r.make = "Itron ERT"; r.model = std::string("SCM ") + (meterType(type) ? meterType(type) : "meter");
            r.pubId = std::to_string(id);
            r.extra = "reading " + std::to_string(cons);
            if (by[3] & 0xC0) r.extra += " \xc2\xb7 tamper";
            r.fact("id", (long long)id); r.fact("ert_type", (long long)type); r.fact("consumption_data", (long long)cons);
            r.fact("physical_tamper", (long long)((by[3] & 0xC0) >> 6)); r.fact("encoder_tamper", (long long)(by[3] & 3));
            out.push_back(r);
            return true;
        }
        for (size_t s : kit::findAll(d, 0x16A31E, 24)) {      // SCM+
            if (s + 128 > d.size()) break;
            const auto by = bytesAt(d, s, 16);
            if (crc16(by.data() + 2, 12, 0x1021, 0x0971) != uint16_t(by[14] << 8 | by[15])) continue;
            const uint32_t id = uint32_t(by[4]) << 24 | uint32_t(by[5]) << 16 | uint32_t(by[6]) << 8 | by[7];
            const uint32_t cons = uint32_t(by[8]) << 24 | uint32_t(by[9]) << 16 | uint32_t(by[10]) << 8 | by[11];
            const char* mt = meterType(by[3]);
            Reading r;
            r.kind = "energy"; r.make = "Itron ERT"; r.model = std::string("SCM+ ") + (mt ? mt : "meter");
            r.pubId = std::to_string(id);
            r.extra = "reading " + std::to_string(cons);
            r.fact("id", (long long)id); r.fact("Consumption", (long long)cons);
            char et[8]; std::snprintf(et, sizeof et, "0x%02X", by[3]); r.fact("EndpointType", et);
            out.push_back(r);
            return true;
        }
        for (size_t s : kit::findAll(d, 0x16A31C, 24)) {      // IDM / NetIDM: 92 bytes
            if (s + 90 * 8 > d.size()) break;              // to the end of the CRC (the trailer is not needed)
            const auto by = bytesAt(d, s, 90);
            if (crc16(by.data() + 2, 86, 0x1021, 0xD895) != uint16_t(by[88] << 8 | by[89])) continue;
            const uint32_t id = uint32_t(by[7]) << 24 | uint32_t(by[8]) << 16 | uint32_t(by[9]) << 8 | by[10];
            const uint32_t last = uint32_t(by[27]) << 24 | uint32_t(by[28]) << 16 | uint32_t(by[29]) << 8 | by[30];
            const char* mt = meterType(by[6]);
            Reading r;
            r.kind = "energy"; r.make = "Itron ERT"; r.model = std::string("IDM ") + (mt ? mt : "meter");
            r.pubId = std::to_string(id);
            r.extra = "reading " + std::to_string(last);
            r.fact("id", (long long)id); r.fact("ERTType", (long long)by[6]); r.fact("LastConsumptionCount", (long long)last);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace ert

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  NEPTUNE R900 water-meter radios (E-CODER / MACH 10), 902–928 MHz — OOK, 30 µs chips. Sources: bemasher, rtlamr wiki
//  "Protocol" § R900 consumption message (ID 32 · unknown 8 · no-use 6 · backflow · consumption 24 · leak · leak now) and
//  issue #29; the 4-chip → base-6 symbols (0011 0, 0101 1, 0110 2, 1100 3, 1010 4, 1001 5), the 5-bit packing of each
//  digit pair and the preamble …55 55 55 A9 66 69 65 from rtl_433's neptune_r900.c notes. The 42 symbols, every one of
//  six valid 4-chip codes, are the check. Consumption: plain binary with 3 wrap bits above it, or six BCD digits on some
//  meters (T-10) — the frame does not say which, so a value that is valid BCD is shown both ways.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace r900 {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    static const int sym[16] = {-1, -1, -1, 0, -1, 1, 2, -1, -1, 5, 4, -1, 3, -1, -1, -1};
    for (const Bits& row : kit::nrzRowsAny(b, 30.0, 12, 200)) {
        for (size_t s : kit::findAll(row, 0xA9666965, 32)) {
            if (s < 24 || bitsAt(row, s - 24, 24) != 0x555555 || s + 32 + 168 > row.size()) continue;
            Bits pk; bool ok = true;
            for (size_t k = 0; k < 21 && ok; ++k) {
                const uint32_t by = bitsAt(row, s + 32 + 8 * k, 8);
                const int hi = sym[by >> 4], lo = sym[by & 0xF];
                if (hi < 0 || lo < 0) { ok = false; break; }
                const int v = 6 * hi + lo;
                for (int q = 4; q >= 0; --q) pk.push_back((v >> q) & 1);
            }
            if (!ok) continue;
            const auto x = bytesAt(pk, 0, 13);
            const uint32_t id = uint32_t(x[0]) << 24 | uint32_t(x[1]) << 16 | uint32_t(x[2]) << 8 | x[3];
            const uint32_t raw = uint32_t(x[6]) << 16 | uint32_t(x[7]) << 8 | x[8];
            const uint32_t bin = uint32_t(x[9] >> 5) << 24 | raw;
            bool isBcd = true; uint32_t bcd = 0;
            for (int i = 5; i >= 0; --i) { const uint32_t dgt = (raw >> (4 * i)) & 0xF; if (dgt > 9) isBcd = false; bcd = bcd * 10 + dgt; }
            const int leak = ((x[9] >> 1) & 0x0F) >> 1, leakNow = x[9] & 3, back = x[5] & 3;
            Reading r;
            r.kind = "energy"; r.make = "Neptune"; r.model = "R900 water meter";
            r.pubId = std::to_string(id);
            r.extra = "reading " + std::to_string(bin) + (isBcd ? " (or " + std::to_string(bcd) + ")" : "");
            if (leakNow) r.extra += leakNow == 2 ? " \xc2\xb7 continuous leak" : " \xc2\xb7 intermittent leak";
            if (back) r.extra += " \xc2\xb7 backflow";
            r.fact("id", (long long)id); r.fact("metertype", (long long)(x[4] & 0xF));
            r.fact("leak", (long long)leak); r.fact("leaknow", (long long)leakNow); r.fact("backflow", (long long)back);
            if (!isBcd) r.fact("consumption", (long long)bin);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace r900

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  RADIOHEAD ASK packets (Arduino / hobby links, and the Sensible Living plant-moisture sensor built on them) — 433 OOK
//  NRZ at 2000 bit/s (500 µs; Sensible Living 1000 µs). Source: the RadioHead library documentation (airspayce.com,
//  RH_ASK): a 36-bit 0101… training preamble, the 12-bit start symbol 0xB38 (sent LSB first), then every byte as two
//  4-to-6 symbols (each 6-bit symbol three 1s and three 0s; sent LSB first, high nibble first): length · to · from · id ·
//  flags · data · CRC-16 (CCITT reflected, init 0xFFFF, complemented, low byte first) over everything before it. The
//  Sensible Living fields (house, module, sensor type / count, alarms, value, battery × 10 mV) from rtl_433's notes.
//  A hobby link's data are its owner's: shown as a length only, never the bytes.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace radiohead {

inline int nib6(uint32_t sym) {
    static const uint8_t k[16] = {0x0D, 0x0E, 0x13, 0x15, 0x16, 0x19, 0x1A, 0x1C, 0x23, 0x25, 0x26, 0x29, 0x2A, 0x2C, 0x32, 0x34};
    for (int i = 0; i < 16; ++i) if (k[i] == sym) return i;
    return -1;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (double bit : {500.0, 1000.0}) {
        for (const Bits& row : kit::nrzRows(b.ook, bit, 6, false, 60)) {
            for (size_t s : kit::findAll(row, 0x5551CD, 24)) {
                if (s < 16 || bitsAt(row, s - 16, 16) != 0x5555) continue;
                std::vector<uint8_t> p;
                size_t pos = s + 24;
                int len = 0;
                while (pos + 12 <= row.size()) {
                    const int hi = nib6(bitsAtLsb(row, pos, 6)), lo = nib6(bitsAtLsb(row, pos + 6, 6));
                    if (hi < 0 || lo < 0) break;
                    p.push_back(uint8_t(hi << 4 | lo));
                    pos += 12;
                    if (p.size() == 1) { len = p[0]; if (len < 7 || len > 60) break; }
                    if (len && int(p.size()) == len) break;
                }
                if (len < 7 || int(p.size()) != len) continue;
                const uint16_t c = uint16_t(~kit::crc16r(p.data(), size_t(len - 2), 0x8408, 0xFFFF));
                if (c != uint16_t(p[size_t(len - 1)] << 8 | p[size_t(len - 2)])) continue;
                Reading r;
                if (bit == 1000.0 && len >= 13) {
                    r.kind = "sensor"; r.make = "Sensible Living"; r.model = "plant moisture sensor";
                    r.pubId = std::to_string(p[2] << 8 | p[3]);
                    r.extra = "moisture " + std::to_string(p[7] << 8 | p[8]);
                    r.fact("house_id", (long long)p[1]); r.fact("module_id", (long long)(p[2] << 8 | p[3]));
                    r.fact("sensor_value", (long long)(p[7] << 8 | p[8])); r.fact("battery_mV", (long long)((p[9] << 8 | p[10]) * 10));
                } else {
                    r.kind = "sensor"; r.make = "RadioHead"; r.model = "ASK packet";
                    r.pubId = std::to_string(p[2]);
                    r.extra = std::to_string(len - 7) + " data bytes, from " + std::to_string(p[2]) + " to " + std::to_string(p[1]);
                    r.fact("len", (long long)(len - 7)); r.fact("to", (long long)p[1]); r.fact("from", (long long)p[2]);
                    r.fact("flags", (long long)p[4]);
                }
                out.push_back(r);
                return true;
            }
        }
    }
    return false;
}

} // namespace radiohead

/** The meter and monitor families, strongest checks first. */
inline bool decodeMeters(const Burst& b, std::vector<Reading>& out) {
    if (oiltank::decode(b, out)) return true;
    if (emontx::decode(b, out)) return true;
    if (efergy::decode(b, out)) return true;
    if (ert::decode(b, out)) return true;
    if (r900::decode(b, out)) return true;
    if (radiohead::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
