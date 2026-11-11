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
        const double w = fsk ? wFsk_ : wOok_;
        if (w < 0.75 * nominal || w > 1.33 * nominal) return out;
        const Runs& r = fsk ? b_.fsk : b_.ook;
        out = chipRows(r, w, 24, fsk ? 0 : 6);
        if (fsk) { const size_t n = out.size(); for (size_t i = 0; i < n; ++i) out.push_back(invert(out[i])); }
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

/** Every TPMS family, in turn. */
inline bool decode(const Burst& b, std::vector<Reading>& out) {
    Chips ch(b);
    if (schraderGG4(ch, out)) return true;
    if (porsche987(ch, out)) return true;
    return false;
}

} // namespace tyre
} // namespace vibeism
