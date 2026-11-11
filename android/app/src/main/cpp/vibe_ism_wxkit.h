// vibe_ism_wxkit.h — V12 ISM decoder: the small toolkit the weather / environment families (vibe_ism_weather*.h) share.
//
// ★★ WHY A SECOND SET OF SLICERS. vibe_ism.h's slicePwm / slicePpm read a run as a bit only when it sits within a
//    fraction of the nominal width, and break the row otherwise — right for a clean signal, but a real sensor's pulses
//    drift (temperature, a weak battery, the envelope smoothing widening marks by ~20 µs), and one stretched pulse cost
//    the whole frame. These are the lenient forms every receiver of these sensors uses: a run is SHORT or LONG by which
//    side of the midpoint it falls, and only a run past the gap limit ends a row. The check (CRC / checksum / repeats)
//    is what tells a frame from noise — never the slicer.
// ★★ Written from the textbook definitions of PWM / PPM / NRZ / RZ / Manchester; nothing here knows a protocol.
#pragma once
#include "vibe_ism_protocols.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace vibeism {
namespace wx {

/** A mark and the space after it, µs (the space is "forever" after the burst's last mark). */
struct MarkSpace { int mark, space; };
/** `sliverUs` > 0: a mark narrower than that is noise inside a space — it and the space after it join the space before
 *  (a weak PPM burst's long spaces are often split by a one-sample blip). */
inline std::vector<MarkSpace> markSpaces(const Runs& r, int sliverUs = 0) {
    std::vector<MarkSpace> o;
    o.reserve(r.size() / 2 + 1);
    for (size_t i = 0; i < r.size(); ++i) {
        if (r[i] <= 0) continue;
        int mk = r[i];
        while (i + 1 < r.size() && r[i + 1] > 0) mk += r[++i];          // two marks in a row: one
        const int sp = (i + 1 < r.size()) ? -r[i + 1] : 1 << 28;
        if (mk < sliverUs && !o.empty() && o.back().space < (1 << 27)) o.back().space += mk + sp;
        else o.push_back({mk, sp});
        if (i + 1 < r.size()) ++i;
    }
    return o;
}

inline void newRow(std::vector<Bits>& rows) { if (!rows.back().empty()) rows.emplace_back(); }
inline void trimRows(std::vector<Bits>& rows) { if (!rows.empty() && rows.back().empty()) rows.pop_back(); }

/** PPM — the SPACE after each mark carries the bit: under the short/long midpoint = `shortBit`, up to `gapLimitUs` =
 *  the other; a longer space ends the row. Marks wider than `maxMarkUs` (when > 0) or narrower than `minMarkUs` end
 *  the row too (a family whose marks are all one width says so — noise slivers then never read as bits). */
inline std::vector<Bits> ppm(const Runs& r, double shortUs, double longUs, double gapLimitUs, int shortBit = 0,
                             double maxMarkUs = 0, double minMarkUs = 0, int sliverUs = 0) {
    std::vector<Bits> rows(1);
    const double mid = 0.5 * (shortUs + longUs);
    for (const auto& ms : markSpaces(r, sliverUs)) {
        if ((maxMarkUs > 0 && ms.mark > maxMarkUs) || ms.mark < minMarkUs) { newRow(rows); continue; }
        if (ms.space > gapLimitUs) { newRow(rows); continue; }
        rows.back().push_back(uint8_t(ms.space < mid ? shortBit : !shortBit));
    }
    trimRows(rows);
    return rows;
}

/** PWM — the MARK carries the bit: under the short/long midpoint = `shortBit`, up to `maxMarkUs` (default 2× long) = the
 *  other. A space over `gapLimitUs` ends the row; a mark too long ends it too; a sliver (< a third of short) is ignored.
 *  `syncUs` > 0: a mark nearer that width than to long ends the row (a sync pulse). */
inline std::vector<Bits> pwm(const Runs& r, double shortUs, double longUs, double gapLimitUs, int shortBit = 1,
                             double syncUs = 0, double maxMarkUs = 0) {
    std::vector<Bits> rows(1);
    const double mid = 0.5 * (shortUs + longUs);
    const double maxM = maxMarkUs > 0 ? maxMarkUs : 2.0 * longUs;
    for (const auto& ms : markSpaces(r)) {
        const double m = ms.mark;
        if (m < shortUs / 3) { if (ms.space > gapLimitUs) newRow(rows); continue; }
        if (syncUs > 0 && std::fabs(m - syncUs) < std::fabs(m - longUs) && std::fabs(m - syncUs) < std::fabs(m - shortUs)) newRow(rows);
        else if (m > maxM) newRow(rows);
        else rows.back().push_back(uint8_t(m < mid ? shortBit : !shortBit));
        if (ms.space > gapLimitUs) newRow(rows);
    }
    trimRows(rows);
    return rows;
}

/** PCM. NRZ (`shortUs` == `longUs` = the bit): each run is round(run / bit) bits of its level. RZ (`shortUs` < `longUs`):
 *  a mark of `shortUs` is a 1, each further `longUs` of space a 0. A space over `gapLimitUs` ends the row. */
inline std::vector<Bits> pcm(const Runs& r, double shortUs, double longUs, double gapLimitUs) {
    std::vector<Bits> rows(1);
    const bool nrz = std::fabs(shortUs - longUs) < 1;
    bool started = false;
    for (int d : r) {
        if (d < 0 && -d > gapLimitUs) { newRow(rows); started = false; continue; }
        if (d < 0 && !started) continue;
        started = true;
        if (nrz) {
            const int n = int(std::lround(std::abs(d) / shortUs));
            for (int k = 0; k < n; ++k) rows.back().push_back(d > 0 ? 1 : 0);
        } else if (d > 0) {
            const int n = std::max(1, int(std::lround(d / longUs)));   // ★ two 1s back to back can merge into one mark
            for (int k = 0; k < n; ++k) rows.back().push_back(1);
        } else {
            const int n = int(std::lround((-d + shortUs - longUs) / longUs));
            for (int k = 0; k < n; ++k) rows.back().push_back(0);
        }
    }
    trimRows(rows);
    return rows;
}

/** Runs → half-bit levels at `halfUs` (each run = round(run / half) halves, 1–2 expected), a run over `gapLimitUs` ends
 *  the row. For Manchester variants the decoder pairs them itself. */
inline std::vector<Bits> halves(const Runs& r, double halfUs, double gapLimitUs) {
    std::vector<Bits> rows(1);
    for (int d : r) {
        const double a = std::abs(d);
        if (a > gapLimitUs) { if (!rows.back().empty() && d > 0) rows.back().push_back(1); newRow(rows); continue; }
        int n = int(std::lround(a / halfUs));
        if (n < 1) n = 1;
        if (n > 4) { newRow(rows); continue; }
        for (int k = 0; k < n; ++k) rows.back().push_back(d > 0 ? 1 : 0);
    }
    trimRows(rows);
    return rows;
}

/** Manchester with the first half-bit pair taken where a mark STARTS the row (a frame begins with a rising edge):
 *  (high, low) = `hiLoBit`. Stops at the first violation. */
inline Bits manchesterFrom(const Bits& h, size_t from, int hiLoBit) {
    Bits o;
    for (size_t i = from; i + 1 < h.size(); i += 2) {
        if (h[i] == h[i + 1]) break;
        o.push_back(uint8_t(h[i] == 1 ? hiLoBit : !hiLoBit));
    }
    return o;
}

/** "Zero-bit" Manchester (the convention of Oregon-style OOK sensors, and many others since): the first rising edge is
 *  a 0; afterwards a FALLING edge more than 1.5 half-bits after the last bit is a 1, a RISING edge likewise a 0. A run
 *  over 2 bits + tolerance ends the row. */
inline std::vector<Bits> mcZero(const Runs& r, double halfUs, double gapLimitUs) {
    std::vector<Bits> rows(1);
    double since = 0;
    bool fresh = true;
    for (int d : r) {
        const double a = std::abs(d);
        if (fresh) { if (d < 0) continue; rows.back().push_back(0); fresh = false; since = 0; }
        if (a > gapLimitUs || a > 2.6 * halfUs) {
            if (d > 0 && a > 1.5 * halfUs) rows.back().push_back(1);     // a long last mark: its falling edge is a 1
            newRow(rows); fresh = true; since = 0;
            if (d > 0) { /* this long mark begins nothing */ }
            continue;
        }
        if (a + since > 1.5 * halfUs) { rows.back().push_back(d > 0 ? 1 : 0); since = 0; }
        else since += a;
    }
    trimRows(rows);
    return rows;
}

/** Biphase "DMC" (a 0 is one full-bit run, a 1 two half-bit runs — every bit boundary a transition): runs of ~`halfUs`
 *  come in pairs = 1, a run of ~2 × `halfUs` = 0; anything else (beyond `tolUs`) ends the row. */
inline std::vector<Bits> dmc(const Runs& r, double halfUs, double tolUs) {
    std::vector<Bits> rows(1);
    for (size_t i = 0; i < r.size(); ++i) {
        const double a = std::abs(r[i]);
        if (std::fabs(a - halfUs) < tolUs) {
            if (i + 1 < r.size() && std::fabs(std::abs(r[i + 1]) - halfUs) < tolUs) { rows.back().push_back(1); ++i; }
            else if (i + 1 >= r.size()) rows.back().push_back(1);
            else newRow(rows);
        } else if (std::fabs(a - 2 * halfUs) < tolUs) rows.back().push_back(0);
        else newRow(rows);
    }
    trimRows(rows);
    return rows;
}

/** CRC-4 MSB-first over whole bytes (the 4-bit remainder, poly as a nibble). */
inline uint8_t crc4(const uint8_t* d, size_t n, uint8_t poly, uint8_t init) {
    unsigned rem = unsigned(init) << 4;
    const unsigned p = unsigned(poly) << 4;
    for (size_t i = 0; i < n; ++i) { rem ^= d[i]; for (int k = 0; k < 8; ++k) rem = (rem & 0x80) ? ((rem << 1) ^ p) & 0xFF : (rem << 1) & 0xFF; }
    return uint8_t(rem >> 4);
}
inline uint8_t rev8(uint8_t v) { v = uint8_t((v & 0xF0) >> 4 | (v & 0x0F) << 4); v = uint8_t((v & 0xCC) >> 2 | (v & 0x33) << 2); return uint8_t((v & 0xAA) >> 1 | (v & 0x55) << 1); }

/** A row that appears at least `minCount` times (with `minBits` ≤ length ≤ `maxBits`); -1 if none. */
inline int repeatedRow(const std::vector<Bits>& rows, int minCount, size_t minBits, size_t maxBits = 100000) {
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].size() < minBits || rows[i].size() > maxBits) continue;
        int n = 1;
        for (size_t j = i + 1; j < rows.size(); ++j) if (rows[j] == rows[i]) ++n;
        if (n >= minCount) return int(i);
    }
    return -1;
}

/** Bytes from bit `pos`, MSB first; bits past the row's end read 0. */
inline std::vector<uint8_t> bytes(const Bits& b, size_t pos, int n) { return bytesAt(b, pos, n); }
inline unsigned nib(const std::vector<uint8_t>& b, int i) { return (i & 1) ? b[size_t(i / 2)] & 0xF : b[size_t(i / 2)] >> 4; }

inline unsigned addBytes(const uint8_t* d, size_t n) { unsigned s = 0; for (size_t i = 0; i < n; ++i) s += d[i]; return s; }
inline unsigned xorBytes(const uint8_t* d, size_t n) { unsigned s = 0; for (size_t i = 0; i < n; ++i) s ^= d[i]; return s; }
inline unsigned addNibbles(const uint8_t* d, size_t n) { unsigned s = 0; for (size_t i = 0; i < n; ++i) s += (d[i] >> 4) + (d[i] & 0xF); return s; }
inline int parity8(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return v & 1; }

/** CRC-8, LSB-first (reflected) with a reflected polynomial (0x8C = Dallas/Maxim 1-Wire). */
inline uint8_t crc8le(const uint8_t* d, size_t n, uint8_t polyRefl, uint8_t init = 0) {
    uint8_t c = init;
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = uint8_t(c & 1 ? (c >> 1) ^ polyRefl : c >> 1); }
    return c;
}
/** CRC-16, LSB-first (reflected) with a reflected polynomial. */
inline uint16_t crc16le(const uint8_t* d, size_t n, uint16_t polyRefl, uint16_t init) {
    uint16_t c = init;
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = uint16_t(c & 1 ? (c >> 1) ^ polyRefl : c >> 1); }
    return c;
}
/** The "LFSR digest" many 433 MHz sensors use as a message check (a Galois LFSR stepped once per data bit, MSB first;
 *  the key XORed into the sum on every 1 bit): 8-bit form. `gen` the feedback, `key` the starting key. */
inline uint8_t lfsrDigest8(const uint8_t* d, size_t n, uint8_t gen, uint8_t key) {
    uint8_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        for (int k = 7; k >= 0; --k) {
            if ((d[i] >> k) & 1) sum ^= key;
            key = uint8_t((key & 1) ? (key >> 1) ^ gen : key >> 1);
        }
    return sum;
}
/** The same digest stepping the key LEFT (the reflected form some families use). */
inline uint8_t lfsrDigest8Refl(const uint8_t* d, size_t n, uint8_t gen, uint8_t key) {
    uint8_t sum = 0;
    for (size_t i = n; i-- > 0;)
        for (int k = 0; k < 8; ++k) {
            if ((d[i] >> k) & 1) sum ^= key;
            key = uint8_t((key & 0x80) ? (key << 1) ^ gen : key << 1);
        }
    return sum;
}
inline uint16_t lfsrDigest16(const uint8_t* d, size_t n, uint16_t gen, uint16_t key) {
    uint16_t sum = 0;
    for (size_t i = 0; i < n; ++i)
        for (int k = 7; k >= 0; --k) {
            if ((d[i] >> k) & 1) sum ^= key;
            key = uint16_t((key & 1) ? (key >> 1) ^ gen : key >> 1);
        }
    return sum;
}

/** FSK NRZ rows at `bitUs` from BOTH demodulations — the discriminator runs and, when the two tones sit so far apart that
 *  the detector opened a channel on one of them, that channel's envelope (its marks are one tone, its spaces the other) —
 *  each in both polarities. `maxBits` per run before the row breaks. */
inline std::vector<Bits> fskRows(const Burst& b, double bitUs, int maxBits = 40, size_t minLen = 32) {
    std::vector<Bits> out;
    const Runs* src[2] = {&b.fsk, &b.ook};
    for (const Runs* rs : src) {
        if (rs->empty()) continue;
        for (int pol = 0; pol < 2; ++pol) {
            Runs rr = *rs;
            if (pol) for (auto& d : rr) d = -d;
            for (auto& row : sliceNrz(rr, bitUs, maxBits)) if (row.size() >= minLen) out.push_back(std::move(row));
        }
    }
    return out;
}

/** Every start of `pat` (`n` bits) in `row`. */
inline std::vector<size_t> findAll(const Bits& row, uint32_t pat, int n) {
    std::vector<size_t> o;
    for (size_t s = findBits(row, pat, n); s != size_t(-1); s = findBits(row, pat, n, s + 1)) o.push_back(s);
    return o;
}

inline std::string hex(unsigned v, int digits) { char b[12]; std::snprintf(b, sizeof b, "%0*X", digits, v); return b; }
inline Reading wxReading(const char* make, const char* model, const std::string& id) {
    Reading r; r.kind = "weather"; r.make = make; r.model = model; r.pubId = id; return r;
}
inline bool tempOk(double c, double lo = -40, double hi = 70) { return c >= lo && c <= hi; }

} // namespace wx
} // namespace vibeism
