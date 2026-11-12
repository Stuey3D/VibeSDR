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
    for (const Bits& row : slicePpm(b.ook, 122, 244, 500, false, 0.35)) {
        if (row.size() < 57 || row.size() > 64) continue;
        const size_t s = findBits(row, 0x01, 8);
        if (s == size_t(-1) || s + 8 + 46 > row.size()) continue;
        const auto m = bytesAt(row, s + 8, 6);   // 46 bits; the last byte's low 2 bits are padding
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

/** The alarm-sensor families, strongest checks first. */
inline bool decodeSecurity(const Burst& b, std::vector<Reading>& out) {
    if (interlogix::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
