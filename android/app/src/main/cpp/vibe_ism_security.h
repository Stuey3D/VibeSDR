// vibe_ism_security.h — V12 ISM decoder: ALARM SENSORS — door/window contacts, PIR, smoke, glass-break, panic buttons,
// alarm keypads and their fobs (Interlogix/GE, Honeywell 5800, DSC, Visonic, SimpliSafe, 2gig …).
//
// ★★★ SOURCES: as vibe_ism_meters.h — frame FACTS from public descriptions (patents, write-ups) and, where that is all
//     there is, the protocol notes at the head of rtl_433's device files (GPL — read for facts only; nothing copied).
// ★★★ PRIVACY (Stuart, 2026-10-25): an alarm sensor's identity is how a burglar would find, copy or jam it — it is SECRET:
//     Reading::secret + secretId (hashed by the Tracker, never shown), never pubId. Shown: "Alarm sensor #3 · Interlogix
//     · motion". What the sensor SAYS (open / closed / motion / tamper / battery) is shown — the owner wants it, and it
//     is no key.
#pragma once
#include "vibe_ism_kit.h"

namespace vibeism {

/** The common shape of an alarm reading: kind "security", secret, a family tag in the secret id's top bits. */
inline Reading alarmReading(const char* make, const std::string& model, uint64_t family, uint64_t id) {
    Reading r;
    r.kind = "security"; r.make = make; r.model = model;
    r.secret = true; r.secretId = (family << 40) | (id & 0xFFFFFFFFFFull);
    return r;
}
inline void addWord(std::string& s, const std::string& w) { if (!w.empty()) s += (s.empty() ? "" : " \xc2\xb7 ") + w; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  INTERLOGIX / GE / UTC (ITI) wireless sensors — 319.5 MHz OOK, PPM: ~120 µs marks, a short gap (~122 µs) = 0, a long
//  one (~244 µs) = 1. Source: US patent 5 761 206 (the frame: front porch, 12 zero sync bits, a start 1, sensor ID 20,
//  device type 4, trigger count 3, low battery, the five inputs' latch + debounced bits, an even parity over the odd
//  bits and an odd parity over the even bits — bits arrive LSB first); the device-type codes (2 smoke, 3 panic, 4 PIR,
//  6 heat, 9/D glass, A contact, E freeze, F key fob) and the fobs' buttons-in-the-trigger-count quirk from the notes in
//  rtl_433's interlogix.c.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace interlogix {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    // ★ marks of ~122 µs only (the 976 µs front porch and the 366 µs old stop bit end/start a row harmlessly): a
    //   Manchester sender's 1T/2T marks — TPMS at 120 µs halves — must not be read as gaps of a PPM frame
    for (const Bits& row : kit::ppmRows(b.ook, 122, 244, 500, 60, 200, 0.35)) {
        if (row.size() < 55 || row.size() > 64) continue;
        const size_t s = findBits(row, 0x0001, 12);   // ★ the patent's 12 sync zeros then the start 1 — at least 11 + 1 seen
        if (s == size_t(-1) || s + 12 + 42 > row.size()) continue;
        const auto m = bytesAt(row, s + 12, 6);   // 46 bits; the last byte's low 2 bits are padding
        if ((m[0] == 0 && m[1] == 0 && m[2] == 0) || (m[0] == 0xFF && m[1] == 0xFF && m[2] == 0xFF)) continue;
        if ((m[3] == 0 && m[4] == 0 && m[5] == 0) || (m[3] == 0xFF && m[4] == 0xFF && m[5] == 0xFF)) continue;
        int p = m[0] ^ m[1] ^ m[2] ^ m[3] ^ m[4];
        p = (p >> 4) ^ (p & 0xF); p = (p >> 2) ^ (p & 3); p ^= m[5] >> 6;
        if (p != 3) continue;                      // both parities odd
        const int type = kit::rev8(m[2]) >> 4;
        const char* sub = type == 0xA ? "contact" : type == 0xF ? "keyfob" : type == 0x4 ? "motion" : type == 0x6 ? "heat"
                        : (type == 0x9 || type == 0xD) ? "glass" : type == 0xE ? "freeze" : type == 0x2 ? "smoke" : type == 0x3 ? "panic" : nullptr;
        if (!sub) continue;
        const uint32_t serial = uint32_t(kit::rev8(m[2])) << 16 | uint32_t(kit::rev8(m[1])) << 8 | kit::rev8(m[0]);
        bool sw[5];
        Reading r = alarmReading("Interlogix", sub, 10, serial);
        r.confirm = true;   // ★ two parity bits are all the check there is: shown once heard twice alike
        if (type == 0xF) {
            const int k = m[3] & 0xE;
            sw[0] = k != 0x4; sw[1] = k != 0x8; sw[2] = k != 0xC; sw[3] = k != 0x2; sw[4] = k != 0xA;
            r.kind = "keyfob"; r.make = "Interlogix";
            r.button = k == 0x4 ? "1" : k == 0x8 ? "2" : k == 0xC ? "3" : k == 0x2 ? "4" : k == 0xA ? "1+2" : "";
        } else {
            sw[0] = m[3] & 0x04; sw[1] = m[3] & 0x01; sw[2] = m[4] & 0x40; sw[3] = m[4] & 0x10; sw[4] = m[4] & 0x04;
            r.battOk = (m[3] & 0x10) ? 0 : 1;
            std::string w;
            if (type == 0xA) w = sw[0] ? "open" : "closed";
            else if (type == 0x4) w = sw[0] ? "motion" : "quiet";
            else w = sw[0] ? "alarm" : "clear";
            if (sw[2] && type == 0xA) addWord(w, "cover open");
            r.extra = w;
        }
        r.fact("subtype", sub);
        static const char* key[5] = {"switch1", "switch2", "switch3", "switch4", "switch5"};
        for (int i = 0; i < 5; ++i) r.fact(key[i], sw[i] ? "OPEN" : "CLOSED");
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace interlogix

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  HONEYWELL / ADEMCO 5800 series (5811, 5816, 5820L, 5890PI …) and 2GIG / Resolution Products sensors — 345 MHz OOK,
//  Manchester, ~136 µs half-bits. Source: Logan Lamb, "Home Insecurity: No Alarms, False Alarms, and SIGINT", DEF CON 22
//  (2014) — preamble FF FE, then channel 4 · serial 20 · event 8 · CRC-16 over the first 4 bytes, poly 0x8005 (Honeywell,
//  channel 8) or 0x8050 (2GIG: channels 2, 4, 9, A, C), init 0; event bits contact · tamper · reed · alarm · battery low
//  · heartbeat. The raw preamble 55 55 56 and the per-channel polynomial as rtl_433's honeywell.c notes give them; the
//  DW11's longer frames carry the same first 6 bytes.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace honeywell {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& row : kit::nrzRowsAny(b, 136, 12, 72)) {
        for (size_t s : kit::findAll(row, 0x555556, 24)) {
            const Bits d = kit::manchesterAt(row, s + 24, 96);
            if (d.size() < 48) continue;
            const auto by = bytesAt(d, 0, 6);
            const int ch = by[0] >> 4;
            const uint32_t id = uint32_t(by[0] & 0xF) << 16 | uint32_t(by[1]) << 8 | by[2];
            const uint16_t crc = uint16_t(by[4] << 8 | by[5]);
            if (id == 0 && crc == 0) continue;
            const bool gig = ch == 0x2 || ch == 0x4 || ch == 0x9 || ch == 0xA || ch == 0xC;
            if (crc16(by.data(), 4, gig ? 0x8050 : 0x8005, 0) != crc) continue;
            const int ev = by[3];
            Reading r = alarmReading(gig ? "2GIG" : "Honeywell", gig ? "sensor" : "5800 series", 11, id);
            std::string w = (ev & 0x80) ? "open" : "closed";
            if (ev & 0x10) addWord(w, "alarm");
            if (ev & 0x40) addWord(w, "tamper");
            if (ev & 0x04) addWord(w, "check-in");
            r.extra = w;
            r.battOk = (ev & 0x08) ? 0 : 1;
            r.fact("channel", (long long)ch); r.fact("event", (long long)ev);
            r.fact("contact_open", (long long)((ev >> 7) & 1)); r.fact("tamper", (long long)((ev >> 6) & 1));
            r.fact("alarm", (long long)((ev >> 4) & 1)); r.fact("heartbeat", (long long)((ev >> 2) & 1));
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace honeywell

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  DSC (Digital Security Controls) contacts, motion, smoke, water — 433.92 MHz OOK, return-to-zero: 500 µs bits, a 250 µs
//  pulse in the second half = 1, none = 0 (the WS4945 / EV-DW4927 double every time). Source: the FCC test report for
//  FCC ID F5300NB912 (the timing and the 48-bit packet: 4 sync 1s, then a 1 before every further 8 bits, status 8 · ESN
//  24 · CRC 8); the CRC — reflected, poly 0xF5, init 0x3D over the 5 bytes, 0 when right — and the status bits (0x02
//  closed, 0x40 heartbeat, 0x08 battery low, 0x10 / ¬0x01 tamper) from rtl_433's dsc.c notes.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace dsc {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (double per : {500.0, 1072.0}) {
        for (Bits row : kit::rzRows(b.ook, per, per * 0.25, per * 0.8, per * 9.5)) {
            if (row.size() < 40 || row.size() > 48) continue;
            row.resize(48, 0);                                  // a CRC ending in zeros sends nothing at its end
            if (bitsAt(row, 0, 4) != 0xF || !row[12] || !row[21] || !row[30] || !row[39]) continue;
            uint8_t by[5] = {uint8_t(bitsAt(row, 4, 8)), uint8_t(bitsAt(row, 13, 8)), uint8_t(bitsAt(row, 22, 8)),
                             uint8_t(bitsAt(row, 31, 8)), uint8_t(bitsAt(row, 40, 8))};
            if (by[0] == 0xFF && by[1] == 0xFF && by[2] == 0xFF && by[3] == 0xFF) continue;
            if (kit::crc8r(by, 5, 0xF5, 0x3D) != 0) continue;
            const int st = by[0];
            const uint32_t esn = uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
            Reading r = alarmReading("DSC", "sensor", 12, esn);
            std::string w = (st & 0x02) ? "closed" : "open";
            const bool tamper = !(st & 0x01) || (st & 0x10);
            if (tamper) addWord(w, "tamper");
            if (st & 0x40) addWord(w, "check-in");
            r.extra = w;
            r.battOk = (st & 0x08) ? 0 : 1;
            r.fact("closed", (long long)((st >> 1) & 1)); r.fact("event", (long long)!(st & 0x40));
            r.fact("tamper", (long long)tamper); r.fact("status", (long long)st);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace dsc

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SIMPLISAFE (gen 1/2) sensors — 433.92 MHz OOK, every mark and gap a bit: ~500 µs = 0, ~1000 µs = 1; bytes LSB first.
//  Facts: the notes in rtl_433's simplisafe.c (A. Callis): a row arrives inverted, starting CC 5F; inverted, byte 2 =
//  0x88 for a sensor; bytes 3–7 a 5-character ID; byte 8 sequence; byte 9 state (1 contact open, 2 closed, 3 alarm
//  off); byte 10 = sequence + state. Two identical rows are required. (Keypad PIN frames are NEVER decoded here.)
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace simplisafe {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const auto rows = kit::piwmRows(b.ook, 500, 1000, 0.3);
    for (const Bits& row : rows) {
        if (row.size() != 92 || !kit::twice(rows, row)) continue;
        // ★ 2026-11-12: our PIWM slice (short = 0) IS the notes' "inverted" row — inverting it again read nothing (0/10)
        const Bits& inv = row;
        if (bitsAt(inv, 0, 16) != 0x33A0 || bitsAt(inv, 16, 8) != 0x88) continue;
        const auto by = bytesAt(inv, 0, 11);
        const int seq = kit::rev8(by[8]), state = kit::rev8(by[9]), cs = kit::rev8(by[10]);
        if (((seq + state) & 0xFF) != cs) continue;
        uint64_t id = 0; for (int k = 3; k <= 7; ++k) id = id << 8 | kit::rev8(by[size_t(k)]);
        Reading r = alarmReading("SimpliSafe", "sensor", 13, id);
        r.extra = state == 1 ? "open" : state == 2 ? "closed" : state == 3 ? "alarm off" : "state " + std::to_string(state);
        r.fact("state", (long long)state); r.fact("seq", (long long)seq);
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace simplisafe

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  GENERIC 433.3 MHz MOTION SENSOR / ALARM (off-brand kits) — OOK PWM: ~888 µs pulse = 1, ~1332–1784 µs = 0, gaps
//  ≤ 1.2 ms, ~1.7 ms between the 8 copies of a 20-bit code, the packets alternating 888/1332 and 1332/1784 µs marks with
//  472 / 920 µs gaps (a constant period). Facts: the notes in rtl_433's generic_motion.c (example codes 80042 arm, 80002
//  disarm, 80008 ping, 800a2 / 800c2 / 800e2 motion). No check: three copies alike, every gap agreeing with its mark.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace genericmotion {

/** Rows of 20 (mark, gap-before) bits: a short mark (~888 µs in one packet, ~1332 in the next) = 1, a long one = 0 —
 *  AND the gap before each mark must say the same (~920 µs before a 1, ~472 before a 0): mark + gap is one bit period,
 *  so a packet whose marks and gaps disagree is not this sender. */
inline std::vector<Bits> rows(const Runs& r) {
    std::vector<Bits> out;
    Bits row; double per = 0;
    auto close = [&] { if (!row.empty()) out.push_back(row); row.clear(); per = 0; };
    for (size_t i = 0; i + 1 < r.size(); ++i) {
        if (r[i] <= 0) continue;
        const int m = r[i];
        const int gapBefore = i ? -r[i - 1] : 99999;
        if (gapBefore > 1500 || row.empty()) {           // a packet's first mark: after the repeat gap
            close();
            if (m >= 700 && m <= 1500) { row.push_back(1); per = m + 920; }   // every packet opens with a 1
            continue;
        }
        const bool one = gapBefore >= 650 && gapBefore <= 1250, zero = gapBefore >= 300 && gapBefore < 650;
        if (!one && !zero) { close(); continue; }
        if (std::fabs(m + gapBefore - per) > 0.15 * per) { close(); continue; }
        row.push_back(one ? 1 : 0);
    }
    close();
    return out;
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const auto rs = rows(b.ook);
    for (const Bits& row : rs) {
        if (row.size() != 20) continue;
        int same = 0; for (const Bits& o : rs) same += o == row;
        if (same < 3) continue;
        const uint32_t code = bitsAt(row, 0, 20);
        if ((code & 0xFFF) == 0 || code == 0xFFFFF || (code >> 8) == 0xFFF) continue;
        Reading r = alarmReading("Generic", "motion sensor", 14, code >> 8);
        const int ev = code & 0xFF;
        r.extra = ev == 0x42 ? "arm" : ev == 0x02 ? "disarm" : ev == 0x08 ? "check-in" : (ev & 0x0F) == 0x02 ? "motion" : "event";
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace genericmotion

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  X10 SECURITY (DS10/DS18 door-window, MS10/MS18 motion, KR10/15/18 fobs; 310 MHz "A", 433 MHz "E") — the X10 RF line
//  (9 ms + 4.5 ms sync, 562 µs marks, 562 / 1687 µs gaps — read long = 1), 41 bits: ID 8 · its top nibble again · its low
//  nibble XORed · code 8 · ¬code · ID 8 more · an even parity over all 41. Facts: the notes in rtl_433's x10_sec.c
//  (A. Kava; W. Herzig's observations) — the codes: 0x00/0x80 open/closed (0x40 tamper, 0x04 delay, bit 0 battery low),
//  0x0C/0x8C motion / ready, 0x06 arm, 0x86 disarm, 0x26/0x88/0x98 panic, 0x42/0xC2 & 0x46/0xC6 lights A / B on / off.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace x10sec {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& w : kit::ppmRows(b.ook, 562, 1687, 3000, 300, 900, 0.35)) {
        if (w.size() < 41 || w.size() > 42) continue;
        const auto x = bytesAt(w, 0, 6);
        if ((x[0] ^ x[1]) != 0x0F || (x[2] ^ x[3]) != 0xFF) continue;
        int p = 0; for (size_t i = 0; i < 41; ++i) p ^= w[i];
        if (p) continue;
        const int code = x[2], c = code & 0xFE;
        const char* ev = "UNKNOWN"; const char* word = "event";
        bool fob = false;
        switch (c) {
            case 0x00: case 0x04: case 0x40: case 0x44: ev = "DOOR/WINDOW OPEN"; word = "open"; break;
            case 0x80: case 0x84: case 0xC0: case 0xC4: ev = "DOOR/WINDOW CLOSED"; word = "closed"; break;
            case 0x0C: case 0x4C: ev = "MOTION TRIPPED"; word = "motion"; break;
            case 0x8C: case 0xCC: ev = "MOTION READY"; word = "quiet"; break;
            case 0x06: ev = "KEY-FOB ARM"; word = "arm"; fob = true; break;
            case 0x86: ev = "KEY-FOB DISARM"; word = "disarm"; fob = true; break;
            case 0x82: ev = "SH624 SEC-REMOTE DISARM"; word = "disarm"; fob = true; break;
            case 0x26: ev = "KR18 PANIC"; word = "panic"; fob = true; break;
            case 0x88: ev = "KR15 PANIC"; word = "panic"; fob = true; break;
            case 0x98: ev = "KR15 PANIC-3SECOND"; word = "panic"; fob = true; break;
            case 0x42: ev = "KEY-FOB LIGHTS A ON"; word = "lights A on"; fob = true; break;
            case 0xC2: ev = "KEY-FOB LIGHTS A OFF"; word = "lights A off"; fob = true; break;
            case 0x46: ev = "KEY-FOB LIGHTS B ON"; word = "lights B on"; fob = true; break;
            case 0xC6: ev = "KEY-FOB LIGHTS B OFF"; word = "lights B off"; fob = true; break;
            default: break;
        }
        Reading r = alarmReading("X10", fob ? "security remote" : "security sensor", 15, uint64_t(x[0]) << 8 | x[4]);
        std::string w2 = word;
        const bool sensor = c == 0x00 || c == 0x04 || c == 0x40 || c == 0x44 || c == 0x80 || c == 0x84 || c == 0xC0 || c == 0xC4 || c == 0x0C || c == 0x4C || c == 0x8C || c == 0xCC;
        if (sensor && (code & 0x40)) addWord(w2, "tamper");
        if (!fob) r.battOk = (code & 1) ? 0 : 1;
        r.extra = w2;
        char cs[4]; std::snprintf(cs, sizeof cs, "%02x", code);
        r.fact("event", ev); r.fact("code", cs);
        out.push_back(r);
        return true;
    }
    return false;
}

} // namespace x10sec

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  PARADOX MAGELLAN (door contacts, PIR, smoke, remotes) — 433.92 OOK PWM: 200 µs = 1, 400 µs = 0, 600 µs a bit; an 800 µs
//  header, 12 × 1, then a 1200 µs start pulse and 32 bits (LSB first): serial 16 · event (type door 1 / motion 2 / smoke
//  3 / remote 4–7, state alarm · tamper bits, 8 reset, 9 battery low, A battery OK, B learn) · CRC-8 0x31 over the 3
//  bytes as sent. Facts: rtl_433's paradox_magellan.c notes (from the Flipper Zero firmware's protocol description, with
//  its worked example 37 AE 48 28 = serial 0x75EC, door, tamper — checked in test-ism).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace paradox {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    const Runs& r = b.ook;
    for (size_t i = 0; i + 64 < r.size(); ++i) {
        if (r[i] < 950 || r[i] > 1500) continue;                 // the start pulse
        // ≥ 10 short (1) pulses before it — the preamble
        int ones = 0; for (size_t k = i; k >= 2 && ones < 12; k -= 2) { if (r[k - 2] > 120 && r[k - 2] < 290) ++ones; else break; }
        if (ones < 10) continue;
        Bits w;
        for (size_t k = i + 2; k < r.size() && w.size() < 32; k += 2) {
            const int m = r[k];
            if (m > 120 && m < 290) w.push_back(1); else if (m >= 290 && m < 520) w.push_back(0); else break;
        }
        if (w.size() != 32) continue;
        const auto by = bytesAt(w, 0, 4);
        if (!by[0] && !by[1] && !by[2]) continue;
        if (crc8(by.data(), 3, 0x31, 0) != by[3]) continue;
        const int ev = kit::rev8(by[2]), type = ev >> 4, st = ev & 0xF;
        const uint32_t id = uint32_t(kit::rev8(by[1])) << 8 | kit::rev8(by[0]);
        const bool sensor = type >= 1 && type <= 3;
        const char* sub = type == 1 ? "door" : type == 2 ? "motion" : type == 3 ? "smoke" : (type >= 4 && type <= 7) ? "remote" : "sensor";
        Reading rd = alarmReading("Paradox", std::string("Magellan ") + sub, 16, id);
        if (!sensor) rd.kind = "keyfob";
        std::string w2;
        if (sensor && st <= 3) { w2 = (st & 1) ? (type == 1 ? "open" : "alarm") : (type == 1 ? "closed" : "clear"); if (st & 2) addWord(w2, "tamper"); }
        else if (st == 8) w2 = "reset"; else if (st == 0xB) w2 = "learn";
        if (st == 9) rd.battOk = 0; else if (st == 0xA) rd.battOk = 1;
        rd.extra = w2;
        char evs[4]; std::snprintf(evs, sizeof evs, "%02x", ev);
        rd.fact("event", (long long)ev);
        if (sensor && st <= 3) { rd.fact("alarm", (long long)(st & 1)); rd.fact("tamper", (long long)((st >> 1) & 1)); }
        out.push_back(rd);
        return true;
    }
    return false;
}

} // namespace paradox

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  HONEYWELL ACTIVLINK door chimes, PIR and door/window contacts (RCWL300A/330A, Series 3/5/9, Decor) — 916.8 MHz (US)
//  OOK / 868 MHz (EU) FSK, PWM ~160–175 µs = 1, ~320–340 µs = 0, 48 bits. Source: K. Lohner, "honeywell-wireless-doorbell"
//  (github.com/klohner/honeywell-wireless-doorbell): ID 20 bits · device type (1 PIR, 2 doorbell, 5 contact) · alert
//  2 bits · door open / closed · secret knock (tamper on a contact) · relay · battery low · an even parity over all 48.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace activlink {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    // ★ which FSK tone is the pulse matters: the other way round reads the complement — still even parity (48 bits),
    //   and a doorbell (type 010) becomes a contact (101). Lohner's map has 15 bits that are always 0 (byte 2's low
    //   nibble, byte 3's bit 7 and low nibble, byte 4's top six, byte 5's bit 7): the complement fails them. And twice
    //   alike: one parity bit is all the check there is.
    for (const Runs& rr : kit::keyedRuns(b)) {
        const auto rows = slicePwm(rr, 170, 330, 450, true, 0.3);
        for (const Bits& w : rows) {
            if (w.size() != 48 || !kit::twice(rows, w)) continue;
            const auto by = bytesAt(w, 0, 6);
            int ones = 0; for (uint8_t x : w) ones += x;
            if (ones & 1) continue;
            if ((by[0] | by[1] | by[2] | by[3] | by[4] | by[5]) == 0) continue;
            if (by[0] == 0xFF && by[2] == 0xFF && by[4] == 0xFF && by[5] == 0xFF) continue;
            if ((by[2] & 0x0F) || (by[3] & 0x8F) || (by[4] & 0xFC) || (by[5] & 0x80)) continue;
            const int type = (by[3] & 0x70) >> 4;
            const char* sub = type == 1 ? "PIR-Motion" : type == 2 ? "Doorbell" : type == 5 ? "Contact" : nullptr;
            if (!sub) continue;
            const uint32_t id = uint32_t(by[0]) << 12 | uint32_t(by[1]) << 4 | (by[2] >> 4);
            const int alertBits = by[4] & 3;
            const bool knockOrTamper = by[5] & 0x10, relay = by[5] & 0x08, low = by[5] & 0x02;
            const bool opened = by[5] & 0x20, closed = by[5] & 0x40;
            Reading r = type == 2 ? Reading() : alarmReading("Honeywell", "ActivLink", 17, id);
            if (type == 2) { r.kind = "doorbell"; r.make = "Honeywell"; r.model = "ActivLink doorbell"; r.secret = true; r.secretId = (uint64_t(17) << 40) | id; }
            else r.model = type == 1 ? "ActivLink PIR" : "ActivLink contact";
            std::string w2 = type == 5 ? (opened && !closed ? "open" : !opened && closed ? "closed" : "") : type == 1 ? "motion" : "";
            if (alertBits == 3) addWord(w2, "full alarm");
            if (type == 5 && knockOrTamper) addWord(w2, "tamper");
            if (type == 2 && knockOrTamper) addWord(w2, "secret knock");
            r.extra = w2;
            r.battOk = low ? 0 : 1;
            r.fact("subtype", sub); r.fact("alert", alertBits == 0 ? "Normal" : alertBits == 3 ? "Full" : "High");
            r.fact("secret_knock", (long long)(type != 5 && knockOrTamper)); r.fact("tampered", (long long)(type == 5 && knockOrTamper));
            r.fact("relay", (long long)relay); r.fact("open", (long long)(opened && !closed ? 1 : !opened && closed ? 0 : -1));
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace activlink

/** The alarm-sensor families, strongest checks first. */
inline bool decodeSecurity(const Burst& b, std::vector<Reading>& out) {
    if (honeywell::decode(b, out)) return true;
    if (interlogix::decode(b, out)) return true;
    if (dsc::decode(b, out)) return true;
    if (x10sec::decode(b, out)) return true;
    if (simplisafe::decode(b, out)) return true;
    if (genericmotion::decode(b, out)) return true;
    if (paradox::decode(b, out)) return true;
    if (activlink::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
