// vibe_ism_tpms.h — V12 ISM decoder: TYRE-PRESSURE SENSORS (TPMS), make by make.
//
// ★★★ WHERE THE FRAME FACTS COME FROM (2026-11-11, Stuart: "take inspiration … we dont want to copy"). A tyre sensor's
//     frame is published almost nowhere outside reverse-engineering notes. Each family below names its source: the
//     public write-ups where they exist (FCC filings, the IHP/DLR paper, issue threads), and otherwise the protocol
//     descriptions in rtl_433's device notes (github.com/merbanan/rtl_433, src/devices/tpms_*.c, schraeder.c). Those
//     notes were READ FOR FACTS ONLY — bit timing, modulation, preamble, field layout, check polynomial and scaling.
//     No code, table, comment or identifier was copied: the framing engine, the searches and every decoder here are our
//     own, written to this file's own shape, and checked against the real recordings of the rtl_433_tests corpus (test
//     INPUT only, never in this repository — scripts/ism-corpus.mjs).
// ★★★ PRIVACY (Stuart, 2026-10-25) — exactly as vibe_ism_protocols.h: a tyre's ID goes to Reading::secretId and nowhere
//     else; the Tracker hashes it into a per-server nickname ("TPMS #2") and clears it. Never pubId, never raw bits.
// ★★ HOW STRONG IS A FRAME? Reading::confirm decides whether one frame is enough (vibe_ism_service.h, Tracker::accept).
//     STRONG (shown at once): a CRC-8 or CRC-16 behind an exact sync of 12+ chips, or a CRC plus a redundant field (an
//     inverted copy of the pressure). WEAK (shown once heard twice alike within 90 s): an 8-bit sum or XOR, a 2-bit
//     check, or a frame with no check at all — noise passes an 8-bit sum one time in 256, and a busy 433 band offers
//     thousands of bursts a day.
#pragma once

#include "vibe_ism_protocols.h"
#include <cstring>
#include <map>
#include <utility>

namespace vibeism {
namespace tyre {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE FRAMING ENGINE — a burst's runs as fixed-width CHIPS (one per half-bit, or per PCM bit), then Manchester or
//  differential Manchester read from a sync pattern found in the chips. Every TPMS family here is a chip sync + a line
//  code + a check, at one of a handful of chip widths (25, 50, 100, 120, 167 µs).
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────

/** Runs → rows of chips `w` µs wide (mark / high tone = 1). A run under a third of a chip is a glitch and ends the row; a
 *  run of more than `maxRun` chips ends it too, keeping `maxRun` of them on both sides (a long preamble tone or a gap
 *  still borders the frame). `pad`: space chips added at both ends of every row — for OOK, where the carrier-off before
 *  and after a frame IS a space the run list does not hold. */
inline std::vector<Bits> chipRows(const Runs& r, double w, int maxRun, int pad) {
    std::vector<Bits> rows(1);
    auto padRow = [&](Bits& b) { if (pad > 0) b.insert(b.begin(), size_t(pad), 0); };
    padRow(rows.back());
    for (int d : r) {
        const double a = std::abs(d);
        const uint8_t lv = d > 0 ? 1 : 0;
        if (a < w / 3) {   // a glitch: the row's timing is lost here
            if (rows.back().size() > size_t(pad)) { rows.back().insert(rows.back().end(), size_t(pad), 0); rows.emplace_back(); padRow(rows.back()); }
            continue;
        }
        const long n = std::max(1L, std::lround(a / w));
        if (n > maxRun) {
            rows.back().insert(rows.back().end(), size_t(maxRun), lv);
            rows.back().insert(rows.back().end(), size_t(pad), 0);
            rows.emplace_back(); padRow(rows.back());
            rows.back().insert(rows.back().end(), size_t(maxRun), lv);
            continue;
        }
        rows.back().insert(rows.back().end(), size_t(n), lv);
    }
    rows.back().insert(rows.back().end(), size_t(pad), 0);
    std::vector<Bits> out;
    for (auto& b : rows) if (b.size() >= size_t(2 * pad + 24)) out.push_back(std::move(b));
    return out;
}

/** Find a chip pattern written as text ("0101 1001": spaces ignored) in `c` at or after `from`; npos if absent. */
inline size_t findChips(const Bits& c, const char* pat, size_t from = 0) {
    char p[160]; size_t n = 0;
    for (const char* q = pat; *q && n < sizeof p; ++q) if (*q == '0' || *q == '1') p[n++] = *q;
    if (c.size() < n) return size_t(-1);
    for (size_t i = from; i + n <= c.size(); ++i) {
        size_t k = 0;
        while (k < n && c[i + k] == uint8_t(p[k] - '0')) ++k;
        if (k == n) return i;
    }
    return size_t(-1);
}
inline size_t patLen(const char* pat) { size_t n = 0; for (const char* q = pat; *q; ++q) n += (*q == '0' || *q == '1'); return n; }

/** `n` Manchester bits from the chips at `pos`: a (1,0) pair is 1 when `hiLoIsOne`, else (0,1) is 1. False at a
 *  violation (two equal halves) or the row's end — every bit of a frame must be a clean pair. */
inline bool mcRead(const Bits& c, size_t pos, int n, bool hiLoIsOne, Bits& out) {
    out.clear();
    if (pos + size_t(2 * n) > c.size()) return false;
    for (int i = 0; i < n; ++i) {
        const uint8_t a = c[pos + size_t(2 * i)], b = c[pos + size_t(2 * i) + 1];
        if (a == b) return false;
        out.push_back(uint8_t((a == 1) == hiLoIsOne));
    }
    return true;
}
/** `n` differential-Manchester bits from the chips at `pos` (pos ≥ 1): every bit is a pair with a change in its middle;
 *  the bit is whether the level also changes at its START (against the chip before it) — `changeIsOne` says which way
 *  round the family counts it. */
inline bool dmcRead(const Bits& c, size_t pos, int n, bool changeIsOne, Bits& out) {
    out.clear();
    if (pos < 1 || pos + size_t(2 * n) > c.size()) return false;
    uint8_t prev = c[pos - 1];
    for (int i = 0; i < n; ++i) {
        const uint8_t a = c[pos + size_t(2 * i)], b = c[pos + size_t(2 * i) + 1];
        if (a == b) return false;
        out.push_back(uint8_t((a != prev) == changeIsOne));
        prev = b;
    }
    return true;
}
inline std::vector<uint8_t> pack(const Bits& b, size_t pos = 0) {
    std::vector<uint8_t> o((b.size() - pos) / 8);
    for (size_t i = 0; i < o.size(); ++i) o[i] = uint8_t(bitsAt(b, pos + 8 * i, 8));
    return o;
}
inline unsigned sum8(const uint8_t* d, size_t n) { unsigned s = 0; for (size_t i = 0; i < n; ++i) s += d[i]; return s & 0xFF; }
inline unsigned xor8(const uint8_t* d, size_t n) { unsigned s = 0; for (size_t i = 0; i < n; ++i) s ^= d[i]; return s; }

/** One burst's chip rows, made once per (modulation, nominal width) and shared by every family that asks. The width is
 *  MEASURED (the shortest-run cluster, tpms::halfEstimate) and used when it lies within ±25 % of the family's nominal —
 *  a tyre sensor's clock is an RC oscillator, several per cent off — and a burst whose measured width is nowhere near
 *  a family's is not offered to it at all (that is most of the CPU saved). FSK rows come in both polarities: which
 *  tone is "1" depends on the radio's spectrum orientation. */
class Chips {
public:
    explicit Chips(const Burst& b) : b_(b) {
        wOok_ = tpms::halfEstimate(b.ook);
        wFsk_ = b.fsk.empty() ? 0 : tpms::halfEstimate(b.fsk);
    }
    const std::vector<Bits>& rows(bool fsk, double nominal) {
        const int key = (fsk ? 100000 : 0) + int(nominal);
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        std::vector<Bits>& out = cache_[key];
        auto fits = [&](double w) { return w >= 0.75 * nominal && w <= 1.33 * nominal; };
        auto both = [&](std::vector<Bits> rs) { for (auto& r : rs) { out.push_back(invert(r)); out.push_back(std::move(r)); } };
        if (!fsk) { if (fits(wOok_)) out = chipRows(b_.ook, wOok_, 24, 6); return out; }
        if (fits(wFsk_)) both(chipRows(b_.fsk, wFsk_, 24, 0));
        /* ★★ AN FSK SENSOR WITH A WIDE DEVIATION ARRIVES AS TWO "OOK" BURSTS (the corpus' older Renault, Ford and Toyota
         *  recordings: tones ±60-75 kHz apart). The detector opens a channel on each tone, and each channel's ENVELOPE is
         *  the frame's chips — the tone on or off. So an FSK family is offered the envelope's chips too, both ways up. */
        if (fits(wOok_)) both(chipRows(b_.ook, wOok_, 24, 6));
        return out;
    }
private:
    const Burst& b_;
    double wOok_ = 0, wFsk_ = 0;
    std::map<int, std::vector<Bits>> cache_;
};

/** A tyre reading's common part. `tag` keeps two makes' IDs apart in the Tracker's hash. */
inline Reading tyre(const char* make, const char* model, unsigned tag, uint64_t id, bool weak) {
    Reading r;
    r.kind = "tpms"; r.make = make; r.model = model;
    r.secret = true;
    r.secretId = (uint64_t(0x40 + tag) << 40) | (id & 0xFFFFFFFFFFull);
    r.confirm = weak;
    return r;
}
/** Values no road tyre reports: a frame that says them passed its check by chance. */
inline bool plausible(double kpa, double tC) {
    if (kpa == kpa && (kpa < 0 || kpa > 1400)) return false;   // 14 bar: a truck tyre is 9-10
    if (tC == tC && (tC < -45 || tC > 130)) return false;
    return true;
}
inline bool allSame(const uint8_t* d, size_t n) { for (size_t i = 1; i < n; ++i) if (d[i] != d[0]) return false; return true; }

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SCHRADER "GG4" (the long-running Schrader frame: FCC ID MRXGG4 — many European and US OE fitments)
//  Facts: rtl_433 schraeder.c notes. OOK Manchester, 120 µs half-bit, a mark-then-space half pair = 1. A ~500 µs wake
//  pulse, then alternating chips into a fixed 0xF nibble; then flags (8 bits), ID (28), pressure (8, × 2.5 kPa),
//  temperature (8, °C + 50), CRC-8 poly 0x07 init 0xF0 over the seven bytes from the 0xF nibble on.
//  ★ OUR FRAMING (checked on the corpus' 54 recordings): the alternating run-in is phase-ambiguous, so the frame is
//  anchored where the run-in ENDS — the chips "…0101 1010" (the last two 1s of the 0xF nibble) — and the nibble's two
//  leading 1s are taken as read.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool schraderGG4(Chips& ch, std::vector<Reading>& out) {
    static const char* kSync = "1 0101 0101 01 1010";   // the wake pulse's last chip, run-in, then 1 1 of the 0xF nibble
    for (const Bits& c : ch.rows(false, 120)) {
        for (size_t p = findChips(c, kSync); p != size_t(-1); p = findChips(c, kSync, p + 1)) {
            Bits d;
            if (!mcRead(c, p + patLen(kSync), 60, true, d)) continue;
            std::vector<uint8_t> by(8);
            by[0] = uint8_t(0xF0 | bitsAt(d, 0, 4));
            for (int i = 1; i < 8; ++i) by[size_t(i)] = uint8_t(bitsAt(d, size_t(4 + 8 * (i - 1)), 8));
            if (crc8(by.data(), 7, 0x07, 0xF0) != by[7]) continue;
            const uint32_t id = (uint32_t(by[1] & 0x0F) << 24) | (uint32_t(by[2]) << 16) | (uint32_t(by[3]) << 8) | by[4];
            if (id == 0 || id == 0x0FFFFFFF) continue;
            Reading r = tyre("Schrader", "Schrader", 1, id, false);
            r.kpa = by[5] * 2.5;
            r.tempC = int(by[6]) - 50;
            if (!plausible(r.kpa, r.tempC)) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  PORSCHE 987 (Boxster / Cayman, second generation) — our former "TPMS type A"
//  Facts: the IHP/DLR paper (Krstic, Savic, Kraemer & Junghans, "Applying Tire Pressure Monitoring Devices for Traffic
//  Management Purposes", elib.dlr.de/81155: its "Sensor A" — FSK, differential Manchester, ID 4 bytes, pressure,
//  temperature °C + 40, two status bytes, CRC-16-CCITT) and rtl_433's tpms_porsche.c notes, which name the car and give
//  the rest: ~52 µs chips, a run-in of chip pairs "0011…" ending "0010", CRC-16 poly 0x1021 init 0xFFFF over the eight
//  data bytes, pressure raw × 2.5 − 100 kPa. Checked on that note's two worked frames in test-ism: the bit after the
//  run-in's last chip is the first data bit, and a bit is 1 when the level does NOT change at its start.
//  STRONG: CRC-16 behind a 20-chip sync.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool porsche987(Chips& ch, std::vector<Reading>& out) {
    static const char* kSync = "0011 0011 0011 0011 0010";
    for (const Bits& c : ch.rows(true, 52)) {
        for (size_t p = findChips(c, kSync); p != size_t(-1); p = findChips(c, kSync, p + 1)) {
            Bits d;
            if (!dmcRead(c, p + patLen(kSync) + 1, 80, false, d)) continue;
            const auto by = pack(d);
            if (crc16(by.data(), 8, 0x1021, 0xFFFF) != uint16_t(by[8] << 8 | by[9])) continue;
            if (allSame(by.data(), 8)) continue;
            const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
            if (id == 0 || id == 0xFFFFFFFF) continue;
            Reading r = tyre("Porsche", "Porsche 987", 2, id, false);
            r.kpa = std::max(0.0, by[4] * 2.5 - 100);
            r.tempC = int(by[5]) - 40;
            if (!plausible(r.kpa, r.tempC)) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

constexpr double kPaPerPsi = 6.894757;

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TOYOTA — Pacific Industrial PMV-C210 (Auris / Corolla and others; 433.92 MHz)
//  Facts: rtl_433 tpms_toyota.c notes and its issue #706. FSK, ~52 µs chips, a run-in "0101 0101 0011 11", then 72
//  differential-Manchester bits: ID (32), a status bit, pressure (8: raw / 4 − 7 PSI), temperature (8: °C + 40), 7 more
//  status bits, the pressure again INVERTED (8), CRC-8 poly 0x07 init 0x80 over the eight bytes before it.
//  STRONG: a CRC-8 and a redundant pressure byte (16 bits of check) behind a 12-chip sync.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool toyotaC210(Chips& ch, std::vector<Reading>& out) {
    static const char* kSync = "1010 1001 1110";
    for (const Bits& c : ch.rows(true, 52)) {
        for (size_t p = findChips(c, kSync); p != size_t(-1); p = findChips(c, kSync, p + 1)) {
            Bits d;
            if (!dmcRead(c, p + patLen(kSync), 72, false, d)) continue;
            const auto by = pack(d);
            if (crc8(by.data(), 8, 0x07, 0x80) != by[8]) continue;
            const int pr = ((by[4] & 0x7F) << 1) | (by[5] >> 7), tr = ((by[5] & 0x7F) << 1) | (by[6] >> 7);
            if (pr != (by[7] ^ 0xFF)) continue;
            const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
            Reading r = tyre("Toyota", "Toyota PMV-C210", 3, id, false);
            r.kpa = std::max(0.0, (pr * 0.25 - 7) * kPaPerPsi);
            r.tempC = tr - 40;
            if (!plausible(r.kpa, r.tempC)) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  TOYOTA — Pacific Industrial PMV-107J (US Toyota / Lexus, 315 MHz)
//  Facts: the FCC filing (fccid.io/PAXPMV107J: 314.98 MHz, F2D FSK, ±40 kHz) and rtl_433 tpms_pmv107j.c notes (after
//  Werner Johansson's work; github.com/xnk/pacific-tpms). ~100 µs chips, a run-in ending "1111 10", then 66
//  differential-Manchester bits: ID (28), flags (6: battery low, a 2-bit repeat counter, unknown, rapid change, failed),
//  pressure (8: (raw − 40) × 2.48 kPa), the pressure INVERTED, temperature (8: °C + 40), CRC-8 poly 0x13 init 0 over the
//  first 64 bits taken as 8 bytes after six leading zero bits.
//  STRONG: a CRC-8 and the inverted pressure.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool toyota107J(Chips& ch, std::vector<Reading>& out) {
    static const char* kSync = "111110";
    for (const Bits& c : ch.rows(true, 100)) {
        for (size_t p = findChips(c, kSync); p != size_t(-1); p = findChips(c, kSync, p + 1)) {
            // ★ the run-in's last space measured 1½ chips on the corpus' recordings (a desync mark): the first data pair
            //   starts one or two chips after "111110" — both are tried, the CRC and the inverted pressure decide
            std::vector<uint8_t> by;
            for (int slack = 1; slack <= 2 && by.empty(); ++slack) {
                Bits d;
                if (!dmcRead(c, p + patLen(kSync) + size_t(slack), 66, false, d)) continue;
                Bits al(6, 0); al.insert(al.end(), d.begin(), d.end());   // 72 bits: the CRC's byte grid
                auto t = pack(al);
                if (crc8(t.data(), 8, 0x13, 0x00) == t[8] && t[5] == (t[6] ^ 0xFF)) by = t;
            }
            if (by.empty()) continue;
            const uint32_t id = uint32_t(by[0]) << 26 | uint32_t(by[1]) << 18 | uint32_t(by[2]) << 10 | uint32_t(by[3]) << 2 | (by[4] >> 6);
            Reading r = tyre("Toyota", "Toyota PMV-107J", 4, id, false);
            r.kpa = std::max(0.0, (int(by[5]) - 40) * 2.48);
            r.tempC = int(by[7]) - 40;
            r.battOk = (by[4] & 0x20) ? 0 : 1;
            if (!plausible(r.kpa, r.tempC)) continue;
            out.push_back(r);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SCHRADER EG53MA4 — Opel / Vauxhall / Saab / Chevrolet OE (Opel 13348393, GM 13540600); the corpus' "PA66GF35"
//  sensor sends it too. Facts: rtl_433 schraeder.c notes (Leonardo Hamada). OOK Manchester, ~123 µs half-bit, 120 bits:
//  a 40-bit run-in, then 10 bytes — flags/status (4), ID (24), pressure (× 2.5 kPa), temperature (°F), and the sum of
//  the nine before it.
//  ★ OUR FRAMING: the run-in is Manchester zeros in the data's own phase, and the frame's first bits may be zeros too,
//  so the frame is anchored at its END — the last 80 bits of an unbroken Manchester run that starts with ≥ 32 zeros.
//  WEAK: an 8-bit sum — confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool schraderEG53MA4(Chips& ch, std::vector<Reading>& out) {
    static const char* kRunIn = "0101 0101 0101 0101 0101 0101 0101 0101";   // 16 Manchester zeros, (1,0) = 1
    for (const Bits& c : ch.rows(false, 123)) {
        const size_t p0 = findChips(c, kRunIn);
        if (p0 == size_t(-1)) continue;
        // the longest clean run of pairs from the run-in on
        Bits d;
        for (size_t i = p0; i + 1 < c.size() && c[i] != c[i + 1]; i += 2) d.push_back(c[i]);   // (1,0) = 1
        if (d.size() < 80 + 32) continue;
        const size_t s = d.size() - 80;
        bool zeros = true; for (size_t i = s - 32; i < s; ++i) zeros = zeros && d[i] == 0;
        if (!zeros) continue;
        const auto by = pack(d, s);
        if (sum8(by.data(), 9) != by[9]) continue;
        if (!by[1] && !by[2] && !by[4] && !by[5] && !by[7] && !by[8]) continue;
        const uint32_t id = uint32_t(by[4]) << 16 | uint32_t(by[5]) << 8 | by[6];
        if (id == 0 || id == 0xFFFFFF) continue;
        Reading r = tyre("Schrader", "Schrader EG53MA4", 14, id, true);
        r.kpa = by[7] * 2.5;
        r.tempC = (by[8] - 32) / 1.8;
        if (!plausible(r.kpa, r.tempC)) continue;
        out.push_back(r);
        return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  SCHRADER SMD3MA4 (Subaru) and SCHRADER 3039 / MRXNIS315G3 (Nissan, Infiniti, Renault Koleos; Redi-Sensor
//  SE10001HP) — 315 MHz. Facts: rtl_433 schraeder.c notes (RonNiles; MotorvateDIY; github.com/JoeSc/Subaru-TPMS-
//  Spoofing; issue #1734). OOK, 120 µs chips: a 36-chip run-in 0xF5555555E, then 38 Manchester bits with (1,0) = 1 —
//  a fixed 1, flags (3: 0 learn, 3 pressure alarm, 5 wake, 7 driving), ID (24), pressure (8), a 2-bit check: all 19 bit
//  pairs summed mod 4 = 1. No temperature is sent.
//  ★ The two sensors send the SAME frame with different pressure scales (Subaru PSI × 5, Nissan PSI × 4) and nothing
//  in the bits tells them apart, so no pressure is shown — a number that is wrong for one of them half the time is
//  worse than none. WEAK: a 2-bit check — confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool schraderSMD3MA4(Chips& ch, std::vector<Reading>& out) {
    static const char* kSync = "0101 0101 0101 1110";
    for (const Bits& c : ch.rows(false, 120)) {
        for (size_t p = findChips(c, kSync); p != size_t(-1); p = findChips(c, kSync, p + 1)) {
            Bits d;
            if (!mcRead(c, p + 14, 38, true, d) || d[0] != 1) continue;
            unsigned sum = 0; for (size_t i = 0; i < 38; i += 2) sum += unsigned(d[i] << 1 | d[i + 1]);
            if ((sum & 3) != 1) continue;
            const uint32_t id = bitsAt(d, 4, 24);
            if (id == 0 || id == 0xFFFFFF) continue;
            const unsigned flags = bitsAt(d, 1, 3);
            Reading r = tyre("Schrader", "Schrader SMD3MA4 / 3039", 15, id, true);
            r.note = "pressure scale depends on the car (Subaru or Nissan)";
            if (flags == 0) r.extra = "learn";
            else if (flags == 3) r.extra = "pressure alarm";
            out.push_back(r);
            return true;
        }
    }
    return false;
}

/** Bytes read LSB-first from `b` at `pos` (some aftermarket chips send each byte least significant bit first). */
inline std::vector<uint8_t> packLsb(const Bits& b, size_t pos, int n) {
    std::vector<uint8_t> o(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) o[size_t(i)] = uint8_t(bitsAtLsb(b, pos + size_t(8 * i), 8));
    return o;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  STEELMATE TP-S15 (and R-Lake) — aftermarket. Facts: rtl_433 steelmate.c notes (issue #3200). FSK, 50 µs Manchester
//  half-bits. Read with (0,1) = 1 and every byte LSB first (checked on the corpus): 16 zeros, a 0x01 byte, ID (16),
//  pressure (raw × 3.125 kPa — bar × 32), temperature (°C + 50), battery (3.9 V − raw × 10 mV; 0xFF / 0xFE = fast /
//  slow leak alarm), and the sum of the six bytes from the 0x01 on. On the air: "10" × 16, "01", "10" × 7 — the lone
//  one is the only place the run-in's phase is certain, so it is the sync. WEAK: an 8-bit sum — confirm.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline bool steelmate(Chips& ch, std::vector<Reading>& out) {
    static const char* kSync = "1010 1010 1010 1010 01 10 1010 1010 1010";   // zeros, then the 0x01 byte: a 1 + 7 zeros
    for (const Bits& c : ch.rows(true, 50))
        for (size_t p = findChips(c, kSync); p != size_t(-1); p = findChips(c, kSync, p + 1)) {
            Bits d;
            if (!mcRead(c, p + patLen(kSync), 48, false, d)) continue;
            const auto by = packLsb(d, 0, 6);
            if (((0x01 + by[0] + by[1] + by[2] + by[3] + by[4]) & 0xFF) != by[5]) continue;
            const uint32_t id = uint32_t(by[0]) << 8 | by[1];
            if (id == 0 || id == 0xFFFF) continue;
            Reading r = tyre("Steelmate", "Steelmate", 16, id, true);
            r.kpa = by[2] * 3.125;
            r.tempC = int(by[3]) - 50;
            if (by[4] == 0xFF) r.extra = "fast leak";
            else if (by[4] == 0xFE) r.extra = "slow leak";
            if (!plausible(r.kpa, r.tempC)) continue;
            out.push_back(r);
            return true;
        }
    return false;
}

/** Every frame in a burst's chip rows (FSK or OOK at `nominal` µs) that follows `sync` and reads `nBits` clean
 *  Manchester bits ((1,0) = 1 when hiLoIsOne): `f(bytes)` decides; true stops the search. */
template <class F> bool eachMc(Chips& ch, bool fsk, double nominal, const char* sync, int nBits, bool hiLoIsOne, F&& f) {
    const size_t n = patLen(sync);
    for (const Bits& c : ch.rows(fsk, nominal))
        for (size_t p = findChips(c, sync); p != size_t(-1); p = findChips(c, sync, p + 1)) {
            Bits d;
            if (!mcRead(c, p + n, nBits, hiLoIsOne, d)) continue;
            if (f(pack(d), d)) return true;
        }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE "VDO" FAMILY OF FRAMES — Continental/VDO and the sensors that copied its air interface: FSK, ~52 µs chips (9.6
//  kbit/s Manchester), a run-in of alternating chips ending in a broken pair — "…1010 1001" seen one way up — and then
//  Manchester with (0,1) = 1. The makes differ in the payload and the check. Facts for each: rtl_433's tpms_*.c notes
//  as named; the VDO application list (vdo.com, 2017-05) for which cars carry which part.
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
constexpr const char* kVdoSync = "1010 1010 1010 1001";

/** RENAULT (Clio, Captur, Zoe; maybe Dacia) — tpms_renault.c notes: 9 bytes; flags (6 bits), pressure (10 bits ×
 *  0.75 kPa), temperature (°C + 30), ID (24 bits, LITTLE-endian), 2 unknown bytes, CRC-8 poly 0x07 init 0.
 *  STRONG: CRC-8 behind the 16-chip sync. */
inline bool renault(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, kVdoSync, 72, false, [&](const std::vector<uint8_t>& by, const Bits&) {
        if (crc8(by.data(), 8, 0x07, 0x00) != by[8] || allSame(by.data(), 9)) return false;
        const uint32_t id = uint32_t(by[5]) << 16 | uint32_t(by[4]) << 8 | by[3];
        Reading r = tyre("Renault", "Renault", 5, id, false);
        r.kpa = (((by[0] & 0x03) << 8) | by[1]) * 0.75;
        r.tempC = int(by[2]) - 30;
        if (!plausible(r.kpa, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}
/** RENAULT 0435R (VDO S180052064Z: Mégane III, Scénic III, Laguna III, Fluence, Zoe) — tpms_renault_0435r.c notes:
 *  9 bytes; ID (3), flags (seen only 0xC0), pressure (raw ÷ 0.75 kPa), temperature (°C + 50), centripetal acceleration,
 *  an XOR checksum of all nine = 0, and a tick byte (0x80 + count ≤ 30, or 0). WEAK: an 8-bit XOR — confirm, and the
 *  flags' high nibble and the tick rule as plausibility. */
inline bool renault0435R(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, kVdoSync, 72, false, [&](const std::vector<uint8_t>& by, const Bits&) {
        if (xor8(by.data(), 9) != 0 || allSame(by.data(), 9)) return false;
        if ((by[3] & 0xF0) != 0xC0) return false;
        if (by[8] && (!(by[8] & 0x80) || (by[8] & 0x7F) > 30)) return false;
        const uint32_t id = uint32_t(by[0]) << 16 | uint32_t(by[1]) << 8 | by[2];
        if (id == 0 || id == 0xFFFFFF) return false;
        Reading r = tyre("Renault", "Renault 0435R", 6, id, true);
        r.kpa = by[4] / 0.75;
        r.tempC = int(by[5]) - 50;
        if (!plausible(r.kpa, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}
/** HYUNDAI / KIA — VDO TG1C (i30 and others; FCC ID KR5TIS-01) — tpms_hyundai_vdo.c notes: 10 bytes after a 32-chip
 *  run-in; state, ID (4), flags + repeat counter, pressure (raw ÷ 5 PSI), temperature (°C + 50), battery?, CRC-8 poly
 *  0x07 init 0xAA over the nine before it. STRONG. */
inline bool hyundaiVdo(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, "1010 1010 1010 1010 1010 1010 1010 1001", 80, false, [&](const std::vector<uint8_t>& by, const Bits&) {
        if (crc8(by.data(), 9, 0x07, 0xAA) != by[9] || allSame(by.data(), 10)) return false;
        const uint32_t id = uint32_t(by[1]) << 24 | uint32_t(by[2]) << 16 | uint32_t(by[3]) << 8 | by[4];
        Reading r = tyre("Hyundai / Kia", "Hyundai / Kia (VDO)", 7, id, false);
        r.kpa = by[6] / 5.0 * kPaPerPsi;
        r.tempC = int(by[7]) - 50;
        if (!plausible(r.kpa, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}
/** SHENZHEN EGQ "Q85" (aftermarket kit) — tpms_abarth124.c notes (issue #2556): 12 bytes; ID (4), flags, pressure
 *  (raw × 3 kPa), temperature (°C + 55), status, XOR of the first nine = 0, a fixed 0x40, then CRC-16 poly 0x1021 init
 *  0xFFFF over the first ten, LITTLE-endian. STRONG (CRC-16). */
inline bool egqQ85(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, "1010 1010 1010 1010 1010 1001", 96, false, [&](const std::vector<uint8_t>& by, const Bits&) {
        if (crc16(by.data(), 10, 0x1021, 0xFFFF) != uint16_t(by[11] << 8 | by[10])) return false;
        if (xor8(by.data(), 9) != 0 || allSame(by.data(), 10)) return false;
        const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
        Reading r = tyre("Shenzhen EGQ", "EGQ Q85", 8, id, false);
        r.kpa = by[5] * 3.0;
        r.tempC = int(by[6]) - 55;
        if (!plausible(r.kpa, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}
/** ABARTH 124 SPIDER / FIAT 124 / MAZDA MX-5 ND — VDO TG1C — tpms_abarth124.c notes: 9 bytes; ID (4), flags, pressure
 *  (raw × 1.38 kPa), temperature (°C + 50), status, XOR of all nine = 0. WEAK: confirm. */
inline bool abarth124(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, "1010 1010 1010 1010 1010 1001", 72, false, [&](const std::vector<uint8_t>& by, const Bits&) {
        if (xor8(by.data(), 9) != 0 || allSame(by.data(), 9)) return false;
        const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
        if (id == 0 || id == 0xFFFFFFFF) return false;
        Reading r = tyre("Abarth / Fiat / Mazda", "Abarth 124 / MX-5 (VDO TG1C)", 9, id, true);
        r.kpa = by[5] * 1.38;
        r.tempC = int(by[6]) - 50;
        if (!plausible(r.kpa, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}
/** CITROËN / PEUGEOT (PSA; VDO) — tpms_citroen.c notes: 10 bytes; state (not checked), ID (4), flags + repeat counter,
 *  pressure (raw × 1.364 kPa), temperature (°C + 50), battery?, XOR of bytes 1-9 = 0. JEEP (2021 Grand Cherokee,
 *  Continental) sends the same frame — tpms_jeep.c notes say NOTHING in the bits tells them apart, and the Jeep's
 *  pressure byte counts double (× 2.728 kPa). So this reads "PSA / Jeep (VDO)" and shows no pressure (it would be
 *  wrong for one of them half the time) — the temperature is common to both. WEAK: confirm. */
inline bool citroenJeep(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, kVdoSync, 80, false, [&](const std::vector<uint8_t>& by, const Bits&) {
        if (xor8(by.data() + 1, 9) != 0 || allSame(by.data() + 1, 9)) return false;
        if (by[6] == 0 || by[7] == 0) return false;
        const uint32_t id = uint32_t(by[1]) << 24 | uint32_t(by[2]) << 16 | uint32_t(by[3]) << 8 | by[4];
        if (id == 0 || id == 0xFFFFFFFF) return false;
        Reading r = tyre("Citroën / Peugeot / Jeep", "PSA / Jeep (VDO)", 10, id, true);
        r.tempC = int(by[7]) - 50;
        r.note = "pressure scale differs by make";
        if (!plausible(NAN, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}
/** FORD (Fiesta, Focus, Kuga, Transit …; VDO S180084730Z) — tpms_ford.c notes: 8 bytes; ID (4), pressure (raw / 4 PSI,
 *  a ninth bit in byte 6 bit 5), temperature (°C + 56 when bit 7 is clear), flags (moving / at rest / learn), the sum of
 *  the first seven = byte 8. WEAK: confirm, and the flags must be one of the three known states. */
inline bool ford(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, kVdoSync, 64, false, [&](const std::vector<uint8_t>& by, const Bits&) {
        if (sum8(by.data(), 7) != by[7] || allSame(by.data(), 8)) return false;
        const int st = by[6] & 0x4C;
        if ((st != 0x08 && st != 0x04 && st != 0x44) || (by[6] & 0x90)) return false;
        const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
        if (id == 0 || id == 0xFFFFFFFF) return false;
        Reading r = tyre("Ford", "Ford (VDO)", 11, id, true);
        /* ★ A LEARN-TOOL FRAME'S PRESSURE IS NOT TRUSTED: the notes' fit is from moving / at-rest frames, and the one learn
         *  frame in the corpus — a multi-make aftermarket sensor (Redi-Sensor, printed serial 0CD6136D) answering a
         *  learn tool in Ford format as well as its own — said 41 PSI where its own frame said 26-33. Shown as heard. */
        if (st == 0x08) r.extra = "learn";
        else r.kpa = ((((by[6] & 0x20) << 3) | by[4]) * 0.25) * kPaPerPsi;
        if (!(by[5] & 0x80)) r.tempC = (by[5] & 0x7F) - 56;
        if (!plausible(r.kpa, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}
/** TRUCK SOLAR TPMS (unbranded, 6 wheels) — tpms_truck.c notes (issue #1893): a 232-chip run-in, 4 bits, then 9 bytes:
 *  ID (4), wheel, flags (4 bits: pressure alert, battery) + pressure (12 bits, kPa), temperature (signed °C), XOR of all
 *  nine = 0. WEAK: confirm. */
inline bool truck(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, "1010 1010 1010 1010 1010 1001", 76, false, [&](const std::vector<uint8_t>&, const Bits& d) {
        const auto by = pack(d, 4);
        if (by[0] == 0 && by[1] == 0 && by[2] == 0 && by[3] == 0) return false;
        if (xor8(by.data(), 9) != 0) return false;
        const uint32_t id = uint32_t(by[0]) << 24 | uint32_t(by[1]) << 16 | uint32_t(by[2]) << 8 | by[3];
        Reading r = tyre("Truck TPMS", "Truck solar TPMS", 12, id, true);
        r.kpa = ((by[5] & 0x0F) << 8) | by[6];
        r.tempC = int8_t(by[7]);
        const int fl = by[5] >> 4;
        r.battOk = (fl & 3) == 3 ? 1 : (fl & 3) == 0 ? 0 : -1;
        if (!plausible(r.kpa, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}
/** HYUNDAI ELANTRA 2012 / HONDA CIVIC — TRW GQ4-44T — tpms_elantra2012.c notes: FSK ~52 µs chips, sync "1110 0010 1010
 *  101" + one, 64 Manchester bits ((0,1) = 1): pressure (raw + 60 kPa), temperature (°C + 50), ID (4), flags (battery
 *  low = bit 1), CRC-8 poly 0x07 init 0 over all eight = 0. STRONG. */
inline bool elantra2012(Chips& ch, std::vector<Reading>& out) {
    return eachMc(ch, true, 52, "0111 0001 0101 0101", 64, false, [&](const std::vector<uint8_t>& by, const Bits&) {
        if (crc8(by.data(), 8, 0x07, 0x00) != 0 || allSame(by.data(), 8)) return false;
        const uint32_t id = uint32_t(by[2]) << 24 | uint32_t(by[3]) << 16 | uint32_t(by[4]) << 8 | by[5];
        if (id == 0 || id == 0xFFFFFFFF) return false;
        Reading r = tyre("Hyundai / Honda (TRW)", "Elantra 2012 / Civic (TRW)", 13, id, false);
        r.kpa = by[0] + 60;
        r.tempC = int(by[1]) - 50;
        r.battOk = (by[6] & 0x02) ? 0 : 1;
        if (!plausible(r.kpa, r.tempC)) return false;
        out.push_back(r);
        return true;
    });
}

/** Every TPMS family, in turn. */
inline bool decode(const Burst& b, std::vector<Reading>& out) {
    Chips ch(b);
    if (schraderGG4(ch, out)) return true;
    if (porsche987(ch, out)) return true;
    if (toyotaC210(ch, out)) return true;
    if (toyota107J(ch, out)) return true;
    if (renault(ch, out)) return true;
    if (hyundaiVdo(ch, out)) return true;
    if (egqQ85(ch, out)) return true;
    if (elantra2012(ch, out)) return true;
    // the weak checks last: a strong family's frame must never be taken by a weak one's 8-bit sum
    if (schraderEG53MA4(ch, out)) return true;
    if (schraderSMD3MA4(ch, out)) return true;
    if (steelmate(ch, out)) return true;
    // ★ the 0435R before the Abarth: both are nine bytes with an XOR of zero after a long run-in, and nothing else in the
    //   Abarth frame is fixed — the 0435R's flag nibble and tick rule make it the stricter test (test-ism caught it)
    if (renault0435R(ch, out)) return true;
    if (abarth124(ch, out)) return true;
    if (citroenJeep(ch, out)) return true;
    if (ford(ch, out)) return true;
    if (truck(ch, out)) return true;
    return false;
}

} // namespace tyre
} // namespace vibeism
