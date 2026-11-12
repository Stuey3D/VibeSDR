// vibe_ism_kit.h — V12 ISM decoder: the small tools the HOME families share (meters, alarm sensors, remotes —
// vibe_ism_meters.h, vibe_ism_security.h, vibe_ism_remotes.h). Our own code: standard checks and line codes, each as the
// textbooks give them. No device knowledge lives here.
#pragma once
#include "vibe_ism_protocols.h"

namespace vibeism {
namespace kit {

inline uint8_t rev8(uint8_t v) { uint8_t r = 0; for (int i = 0; i < 8; ++i) r = uint8_t(r << 1 | ((v >> i) & 1)); return r; }

/** CRC-8, REFLECTED (LSB first in, LSB first out) — `poly` in its ordinary MSB-first form (0x31 = the Dallas/Maxim
 *  1-Wire CRC when init is 0). */
inline uint8_t crc8r(const uint8_t* d, size_t n, uint8_t poly, uint8_t init = 0) {
    const uint8_t p = rev8(poly);
    uint8_t c = rev8(init);
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = uint8_t(c & 1 ? (c >> 1) ^ p : c >> 1); }
    return c;
}
/** CRC-16, LSB first — `polyRev` already reflected (0xA001 = CRC-16/IBM's 0x8005, 0x8408 = CCITT's 0x1021). */
inline uint16_t crc16r(const uint8_t* d, size_t n, uint16_t polyRev, uint16_t init) {
    uint16_t c = init;
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = uint16_t(c & 1 ? (c >> 1) ^ polyRev : c >> 1); }
    return c;
}
inline std::string fmt1v(double v, int dp = 1) { char b[32]; std::snprintf(b, sizeof b, "%.*f", dp, v); return b; }
inline unsigned sumBytes(const uint8_t* d, size_t n) { unsigned s = 0; for (size_t i = 0; i < n; ++i) s += d[i]; return s; }
inline uint8_t xorBytes(const uint8_t* d, size_t n) { uint8_t s = 0; for (size_t i = 0; i < n; ++i) s ^= d[i]; return s; }
inline int parity(uint32_t v) { int p = 0; while (v) { p ^= int(v & 1); v >>= 1; } return p; }

/** Manchester pairs from `pos` of a half-bit (or NRZ-chip) row: (0,1) = 1, (1,0) = 0 — the second half is the bit
 *  (`secondIsBit` false: the first half). Stops at the first equal pair or after `maxBits`. `end` = where it stopped. */
inline Bits manchesterAt(const Bits& h, size_t pos, size_t maxBits, bool secondIsBit = true, size_t* end = nullptr) {
    Bits out;
    size_t i = pos;
    while (i + 1 < h.size() && out.size() < maxBits) {
        if (h[i] == h[i + 1]) break;
        out.push_back(secondIsBit ? h[i + 1] : h[i]);
        i += 2;
    }
    if (end) *end = i;
    return out;
}

/** NRZ rows of a run list at `bitUs`, both polarities (`both`), rows ended by a run of more than `maxBits`.
 *  ★ Unlike sliceNrz, the run that ends a row still gives the row ONE chip of its level: a frame's last chip is
 *    followed by the idle level, and a Manchester frame needs that last half-bit. */
inline std::vector<Bits> nrzRows(const Runs& r, double bitUs, int maxBits, bool both, size_t minLen = 16) {
    std::vector<Bits> out;
    if (r.empty() || bitUs <= 0) return out;
    for (int pol = 0; pol < (both ? 2 : 1); ++pol) {
        Bits row;
        auto close = [&] { if (row.size() >= minLen) out.push_back(row); row.clear(); };
        for (int d0 : r) {
            const int d = pol ? -d0 : d0;
            const int nb = int(std::lround(std::abs(d) / bitUs));
            if (nb == 0) { close(); continue; }
            if (nb > maxBits) { if (!row.empty()) row.push_back(d > 0); close(); continue; }
            for (int k = 0; k < nb; ++k) row.push_back(d > 0);
        }
        close();
    }
    return out;
}

/** NRZ rows from BOTH slices of a burst — the discriminator's, and the envelope's: a wide-deviation FSK sender can be
 *  detected as two channels, one per tone, each looking like OOK (the oil gauges' ±40 kHz do). Both polarities each. */
inline std::vector<Bits> nrzRowsAny(const Burst& b, double bitUs, int maxBits, size_t minLen = 16) {
    std::vector<Bits> out = nrzRows(b.fsk, bitUs, maxBits, true, minLen);
    for (auto& r : nrzRows(b.ook, bitUs, maxBits, true, minLen)) out.push_back(std::move(r));
    return out;
}

/** The run lists an OOK-defined family should try: the envelope's, and the discriminator's both ways round — the same
 *  keying sent as FSK (or a wide FSK pair recorded as one channel) reads as OOK from its tones. */
inline std::vector<Runs> keyedRuns(const Burst& b) {
    std::vector<Runs> v;
    v.push_back(b.ook);
    if (!b.fsk.empty()) {
        Runs a = b.fsk, c = b.fsk;
        for (auto& d : c) d = -d;
        for (Runs* r : {&a, &c}) { size_t k = 0; while (k < r->size() && (*r)[k] < 0) ++k; r->erase(r->begin(), r->begin() + long(k)); }
        v.push_back(std::move(a)); v.push_back(std::move(c));
    }
    return v;
}

/** PPM rows whose MARKS are checked too: a mark outside [markMin, markMax] ends the row (slicePpm reads only the gaps,
 *  so a Manchester sender's 1T/2T marks pass through it — and a PPM decoder with a weak check then reads them). */
inline std::vector<Bits> ppmRows(const Runs& r, double shortGap, double longGap, double rowGap, double markMin, double markMax,
                                 double tol = 0.35) {
    std::vector<Bits> rows(1);
    for (size_t i = 0; i < r.size(); ++i) {
        const int d = r[i];
        if (d > 0) { if ((d < markMin || d > markMax) && !rows.back().empty()) rows.emplace_back(); continue; }
        const double g = -d;
        if (g > rowGap) { if (!rows.back().empty()) rows.emplace_back(); continue; }
        if (std::fabs(g - shortGap) <= shortGap * tol) rows.back().push_back(0);
        else if (std::fabs(g - longGap) <= longGap * tol) rows.back().push_back(1);
        else if (!rows.back().empty()) rows.emplace_back();
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** RETURN-TO-ZERO rows: a mark in a bit slot = 1, an empty slot = 0; marks [markMin, markMax], slots of `periodUs`
 *  counted from mark START to mark START; a gap over `rowGapUs` (or a mark out of range) ends the row. Trailing zeros
 *  are not on the air: callers pad. */
inline std::vector<Bits> rzRows(const Runs& r, double periodUs, double markMin, double markMax, double rowGapUs) {
    std::vector<Bits> rows(1);
    double t = 0, lastStart = -1;
    for (size_t i = 0; i < r.size(); ++i) {
        const int d = r[i];
        if (d > 0) {
            if (d < markMin || d > markMax) { if (!rows.back().empty()) rows.emplace_back(); lastStart = -1; }
            else {
                if (lastStart >= 0) {
                    const double k = (t - lastStart) / periodUs;
                    const long n = std::lround(k);
                    if (n < 1 || std::fabs(k - double(n)) > 0.3) { if (!rows.back().empty()) rows.emplace_back(); }
                    else for (long z = 1; z < n; ++z) rows.back().push_back(0);
                }
                rows.back().push_back(1);
                lastStart = t;
            }
        } else if (-d > rowGapUs) { if (!rows.back().empty()) rows.emplace_back(); lastStart = -1; }
        t += std::abs(d);
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** PIWM: every run, mark AND gap, is one bit — ~shortUs = 0, ~longUs = 1 (`tol` relative); anything else ends a row. */
inline std::vector<Bits> piwmRows(const Runs& r, double shortUs, double longUs, double tol = 0.3) {
    std::vector<Bits> rows(1);
    for (int d0 : r) {
        const double d = std::abs(d0);
        if (std::fabs(d - shortUs) <= shortUs * tol) rows.back().push_back(0);
        else if (std::fabs(d - longUs) <= longUs * tol) rows.back().push_back(1);
        else if (!rows.back().empty()) rows.emplace_back();
    }
    if (rows.back().empty()) rows.pop_back();
    return rows;
}

/** Every position of `pattern` (`n` ≤ 32 bits, MSB first) in `b`. */
inline std::vector<size_t> findAll(const Bits& b, uint32_t pattern, int n) {
    std::vector<size_t> at;
    for (size_t s = findBits(b, pattern, n); s != size_t(-1); s = findBits(b, pattern, n, s + 1)) at.push_back(s);
    return at;
}

/** Bits → bytes, MSB first, from `pos` (the last byte zero-padded). */
inline std::vector<uint8_t> packBits(const Bits& b, size_t pos = 0, size_t nBits = size_t(-1)) {
    const size_t n = std::min(nBits, b.size() > pos ? b.size() - pos : 0);
    std::vector<uint8_t> o((n + 7) / 8, 0);
    for (size_t i = 0; i < n; ++i) if (b[pos + i]) o[i / 8] |= uint8_t(0x80 >> (i % 8));
    return o;
}

/** The rows of a burst that came twice alike — what a frame with no check, or a weak one, must show to be believed. */
inline bool twice(const std::vector<Bits>& rows, const Bits& w) {
    int n = 0;
    for (const Bits& r : rows) if (r == w && ++n >= 2) return true;
    return false;
}

} // namespace kit

/** ★ The home families that transmit only at 300–440 MHz (315 / 319.5 / 345 / 433.92): a reading of theirs at 868 or
 *  915 MHz passed its check by chance (an Insteon burst at 915 read as an Interlogix heat sensor). bandPlausible. */
inline bool homeLowBandOnly(const Reading& r) {
    static const char* const low[] = {"Interlogix"};
    for (const char* m : low) if (r.make == m) return true;
    return false;
}

} // namespace vibeism
