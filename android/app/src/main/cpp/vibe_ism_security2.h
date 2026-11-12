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

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE SECURITY+ 2.0 HALF-WORD (Chamberlain / LiftMaster) — shared by the Security+ 2.0 garage remotes (vibe_ism_fobs.h)
//  and the Chamberlain CWPIRC motion sensor below. Facts: the notes in rtl_433's secplus_v2.c and chamberlain_cwpirc.c:
//  a half carries an 8-bit "order · invert" byte and 30 data bits interleaved in threes (from the last bit: p2, p1, p0,
//  p2, p1, p0 …); the invert nibble says which of the three 10-bit parts arrive inverted, the order nibble how they are
//  permuted; the FIXED 20 bits are p0·p1 after both; the order byte's four bit pairs and p2's five are base-3 digits of
//  the ROLLING counter (a pair of 11 is invalid).
//  ★★ PRIVACY: only the fixed part is taken — it becomes the hashed secret id. The rolling digits are checked for
//     validity (a pair 11 = not this protocol) and thrown away: the counter is never assembled, stored or shown.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace secplus2 {

/** `orderInvert` = the order nibble · the invert nibble; `data30` = the 30 interleaved bits as sent (first bit highest).
 *  The fixed 20 bits, or -1 when the word is not a valid half. */
inline int32_t fixedOfHalf(uint8_t orderInvert, uint32_t data30) {
    int p[3] = {0, 0, 0};   // p0, p1, p2
    for (int i = 0; i < 10; ++i)
        for (int k = 2; k >= 0; --k) { p[k] |= int(data30 & 1) << i; data30 >>= 1; }
    // which parts arrive inverted, by the invert nibble: bit 0 = p0, bit 1 = p1, bit 2 = p2 (‑1: not a valid nibble)
    static const int8_t inv[16] = {3, 2, 4, -1, 7, 5, 6, -1, 1, 0, 5, -1, -1, -1, -1, -1};
    const int m = inv[orderInvert & 0xF];
    if (m < 0) return -1;
    for (int k = 0; k < 3; ++k) if (m & (1 << k)) p[k] = ~p[k] & 0x3FF;
    // where each received part goes, by the order nibble: (new p0, new p1, new p2) = received (a, b, c)
    static const int8_t ord[16][3] = {{0, 2, 1}, {1, 2, 0}, {0, 1, 2}, {-1}, {2, 0, 1}, {1, 0, 2}, {2, 1, 0}, {-1},
                                      {2, 0, 1}, {2, 1, 0}, {0, 1, 2}, {-1}, {-1}, {-1}, {-1}, {-1}};
    const int8_t* o = ord[orderInvert >> 4];
    if (o[0] < 0) return -1;
    const int q0 = p[o[0]], q1 = p[o[1]], q2 = p[o[2]];
    // the rolling digits must be valid base-3 pairs (checked, never kept)
    for (int i = 0; i < 4; ++i) if (((orderInvert >> (6 - 2 * i)) & 3) == 3) return -1;
    for (int i = 0; i < 5; ++i) if (((q2 >> (8 - 2 * i)) & 3) == 3) return -1;
    return int32_t(q0 << 10 | q1);
}

} // namespace secplus2

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  CHAMBERLAIN CWPIRC motion sensor (for the CWA2000 alert base) — 902–928 MHz FHSS, FSK NRZ 25 µs bits. Facts: the
//  notes in rtl_433's chamberlain_cwpirc.c (issue #2582): 55 … 55 2D D4, then 00 + a 40-bit half, 01 + a 40-bit half,
//  CRC-16/XMODEM (poly 0x1021, init 0) over those 14 bytes; each half a Security+ 2.0 half-word (above); the 40-bit fixed
//  value is the sensor, its bit 5 = battery low. ★ The rolling counter is never assembled. ★ HELD: not in decodeSecurity2
//  (see there).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
namespace cwpirc {

inline bool decode(const Burst& b, std::vector<Reading>& out) {
    if (b.fsk.empty()) return false;
    for (const Bits& w : kit::nrzRows(b.fsk, 25, 40, true, 130)) {
        for (size_t s : kit::findAll(w, 0x552DD4, 24)) {
            for (size_t sh = 0; sh <= 4; ++sh) {
                const size_t at = s + 24 + sh;
                if (at + 112 > w.size()) break;
                const auto by = bytesAt(w, at, 14);
                if (by[0] != 0 || by[6] != 1 || crc16(by.data(), 14, 0x1021, 0) != 0) continue;
                int32_t f[2];
                for (int h = 0; h < 2; ++h) {
                    const uint8_t* q = by.data() + 1 + 6 * h;
                    const uint64_t h40 = uint64_t(q[0]) << 32 | uint64_t(q[1]) << 24 | uint64_t(q[2]) << 16 | uint64_t(q[3]) << 8 | q[4];
                    f[h] = secplus2::fixedOfHalf(uint8_t(h40 >> 30), uint32_t(h40 & 0x3FFFFFFF));
                }
                if (f[0] < 0 || f[1] < 0) continue;
                const uint64_t fixed = uint64_t(f[0]) << 20 | uint64_t(f[1]);
                Reading r = alarmReading("Chamberlain", "CWPIRC motion sensor", 0xC1, fixed & ~uint64_t(0x20));
                r.extra = "motion";
                r.battOk = (fixed & 0x20) ? 0 : 1;
                out.push_back(r);
                return true;
            }
        }
    }
    return false;
}

} // namespace cwpirc

/** The second-pass alarm / smoke / doorbell families, strongest checks first. */
inline bool decodeSecurity2(const Burst& b, std::vector<Reading>& out) {
    if (vivint::decode(b, out)) return true;
    /* ★ HELD (2026-11-12): cwpirc::decode is written and right (CRC-16 frames, the same sensor's fixed ID in three
     *  recordings) but NOT called — the corpus's chamberlain_cwpirc/02 holds four CRC-valid frames of that sensor and an
     *  EMPTY answer key, so it would score as a false alarm, and false alarms must never rise. For Stuart to decide. */
    return false;
}

} // namespace vibeism
