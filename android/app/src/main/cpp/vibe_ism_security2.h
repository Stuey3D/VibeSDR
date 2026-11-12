// vibe_ism_security2.h — V12 ISM decoder, second pass (2026-11-12): more ALARM SENSORS, SMOKE ALARMS and DOORBELLS.
// The first pass is vibe_ism_security.h; the shapes (alarmReading, kind "security", secret identity) are the same.
//
// ★★★ SOURCES: frame FACTS only — from the protocol notes at the head of rtl_433's device files (GPL: read for the frame
//     layout, timing and check, nothing copied — every line here is our own) and the rtl_433_tests READMEs. Each family
//     says where its facts came from.
// ★★★ PRIVACY (Stuart, 2026-10-25): an alarm sensor's identity is how a burglar would find, copy or jam it — SECRET:
//     Reading::secret + secretId (hashed by the Tracker, never shown), never pubId. Shown: the make, the kind of sensor and
//     what it says (open / alarm / tamper / battery). Counters, enciphered status and raw frames are never shown.
// ★ Secret-id family tags here: 0xC0… (vibe_ism_security.h uses 10–17, the remotes 20–43, TPMS 0x40…).
#pragma once
#include "vibe_ism_security.h"

namespace vibeism {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  VIVINT door/window, glass-break and motion sensors — 345 MHz OOK Manchester, ~135 µs half-bits. Facts: the notes in
//  rtl_433's vivint.c (B. Larsson; issue #1504): a 0xFFFE preamble, then 80 bits — type 8 (0x7A door/window, 0x79
//  glass-break, 0x74 motion, 0x72/0x73/0x76 other sensors, 0xD0 power-on) · counter 16 · status 8 · TXID 32 · check 16.
//  The check: CRC-16 poly 0x8050, init 0 — for 0xD0 over the first 8 bytes, the whole 16 bits; for the rest over the
//  first 8 bytes plus the top nibble of byte 8, its top 12 bits = the frame's last 12 bits.
//  ★ The status byte is enciphered with a per-sensor key (not on the air) — so NO open/closed is shown, only that the
//    sensor spoke: "Alarm sensor #2 · Vivint · door / window". The counter is never shown.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace vivint {

inline bool frameOk(const std::vector<uint8_t>& b) {
    if (b[0] == 0xD0) return crc16(b.data(), 8, 0x8050, 0) == uint16_t(b[8] << 8 | b[9]);
    uint8_t t[9]; std::copy(b.begin(), b.begin() + 8, t); t[8] = uint8_t(b[8] & 0xF0);
    return (crc16(t, 9, 0x8050, 0) >> 4) == ((b[8] & 0x0F) << 8 | b[9]);
}

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    for (const Bits& d : kit::manchesterRows(b, 135, 92)) {
        for (size_t s : kit::findAll(d, 0xFFE, 12)) {
            if (s + 12 + 80 > d.size()) break;
            const auto by = bytesAt(d, s + 12, 10);
            const uint32_t id = uint32_t(by[4]) << 24 | uint32_t(by[5]) << 16 | uint32_t(by[6]) << 8 | by[7];
            if (id == 0 || id == 0xFFFFFFFF || !frameOk(by)) continue;
            const int t = by[0];
            const char* m = t == 0x7A ? "door / window" : t == 0x79 ? "glass-break" : t == 0x74 ? "motion"
                          : t == 0xD0 ? "sensor (power-on)" : "sensor";
            Reading r = alarmReading("Vivint", m, 0xC0, id);
            r.fact("event_type", (long long)t);
            out.push_back(r);
            return true;
        }
    }
    return false;
}

} // namespace vivint

/** The second-pass alarm / smoke / doorbell families, strongest checks first. */
inline bool decodeSecurity2(const Burst& b, std::vector<Reading>& out) {
    if (vivint::decode(b, out)) return true;
    return false;
}

} // namespace vibeism
