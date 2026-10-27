// vibe_wmbus.h — V12 Wireless M-Bus meters in ISM mode (docs/v12/WMBUS-DECODER.md): water, heat, gas and electricity
// meters at 868.3 / 868.95 MHz — modes T1, C1 (frame A and B) and S1 — decoded from the ISM channel's SAMPLES.
//
// ★★★ BUILT FROM THE PUBLISHED STANDARDS. EN 13757-4 (radio modes, coding, frames, CRC), EN 13757-3 (DIF/VIF, device
//     types), EN 13757-7 (transport header, security modes), as summarised in the design doc and checked on 2026-10-26
//     against: TI AN067 / SWRA234 (S and T preambles and sync words, frame format A, the CRC polynomial, init 0 and the
//     final complement; TI's own reference tables for 3-out-of-6 — the 16 code words below match them one for one),
//     Infineon's TDA5340 wM-Bus application note (Table 2-1: every mode's preamble and sync in chips, incl. C mode
//     0101010000111101 then 0101010011001101 (frame A) / 0101010000111101 (frame B), n = 16; C1 deviation 43-47 kHz),
//     and the CRC catalogue (CRC-16/EN-13757, check 0xC2B7). No GPL decoder source was read or copied.
// ★★★ NEVER DECRYPT (Stuart). No AES, no key, no key entry. An encrypted payload's bytes never leave decode(): they are
//     not in the Meter, the Reading, the JSON, the gallery or any log — only "encrypted (AES)" and how many blocks.
// ★★★ SERIALS ARE SHOWN (Stuart, 2026-10-25): a meter is bolted at one address, its serial is printed on it, and joining
//     the two needs the water company's records. So unlike a TPMS the ID is the device's public name. Readings of a meter
//     that sends them in the clear are shown too.
// ★★ THE DEMODULATOR WORKS ON SAMPLES, not ISM's run lengths: a T1/C1 chip is 10 µs. Per burst (only bursts at 868.30
//    or 868.95, so nothing else pays for it): a delay-and-multiply discriminator (SIMD), the carrier from the burst's own
//    phase drift, the chip period and phase measured off the 0101… preamble's zero crossings (no rate grid to search),
//    then every chip as the integral of the discriminator over that chip, tracked by a small timing loop. Decisions are
//    SOFT where the code allows: a 3-out-of-6 word is the code word that best matches its six soft chips (maximum
//    likelihood — a hard slicer would call it a code error and lose the frame), a Manchester bit is the sign of its two
//    halves' difference. The 16-bit CRC of every block then decides.
// ★★ Header-only and links nothing, like vibe_ism.h.
#pragma once

#include "vibe_ism.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#if defined(__ARM_NEON) && !defined(VIBE_FORCE_SCALAR)
#include <arm_neon.h>
#define VIBE_WMBUS_NEON 1
#elif (defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)) && !defined(VIBE_FORCE_SCALAR)
#include <emmintrin.h>
#define VIBE_WMBUS_SSE 1
#endif

namespace vibeism {
namespace wmbus {

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  CHECKS AND CODES
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** CRC-16/EN-13757: poly 0x3D65, init 0, MSB first, not reflected, final complement. Sent high byte first. */
inline uint16_t crc(const uint8_t* d, size_t n) { return uint16_t(~crc16(d, n, 0x3D65, 0)); }

/** 3-out-of-6 (EN 13757-4; TI SWRA234's tables agree): nibble → 6 chips, sent MSB first, the high nibble's word first. */
static constexpr uint8_t kCode36[16] = {0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13,
                                        0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29};
/** The inverse: 6-chip word → nibble, −1 = not a code word. */
inline const int8_t* decode36Table() {
    static const auto t = [] { std::vector<int8_t> v(64, -1); for (int i = 0; i < 16; ++i) v[kCode36[i]] = int8_t(i); return v; }();
    return t.data();
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  LINK LAYER FIELDS
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** The M-field's three letters: (c1−64)·1024 + (c2−64)·32 + (c3−64). Bit 15 (a "soft" code) is ignored for the letters. */
inline std::string mfrLetters(uint16_t m) {
    std::string s(3, '?');
    const int c[3] = {(m >> 10) & 31, (m >> 5) & 31, m & 31};
    for (int i = 0; i < 3; ++i) s[size_t(i)] = c[i] >= 1 && c[i] <= 26 ? char(64 + c[i]) : '?';
    return s;
}
/** ★ A small table of meter makers likely in the UK and Europe, by their DLMS UA FLAG ID. ★★ ASSUMED — the FLAG directory
 *  (dlms.com/flag-id-directory) is a script-driven page that could not be read on 2026-10-26; these are the codes these
 *  makers' meters are known by in the wM-Bus literature. Anything else shows its three letters ("Meter · ABC · …") —
 *  a wrong name is worse than three letters. */
inline const char* mfrName(const std::string& l) {
    static const struct { const char* code; const char* name; } k[] = {
        {"SEN", "Sensus"}, {"KAM", "Kamstrup"}, {"ITW", "Itron"}, {"ELS", "Elster"}, {"HYD", "Diehl"}, {"DME", "Diehl"},
        {"ZRI", "Zenner"}, {"APA", "Apator"}, {"TCH", "Techem"}, {"IST", "ista"}, {"QDS", "Qundis"}, {"EFE", "Engelmann"},
        {"BMT", "B Meters"}, {"MAD", "Maddalena"}, {"SON", "Sontex"}, {"LUG", "Landis+Gyr"},
    };
    for (const auto& e : k) if (l == e.code) return e.name;
    return nullptr;
}
/** "12345678" from the A-field's 4 identification bytes (BCD, least significant byte first). A non-BCD nibble is printed
 *  as its hex digit, never rejected (some makers use them). */
inline std::string idText(const uint8_t* a) {
    static const char* hx = "0123456789ABCDEF";
    std::string s;
    for (int i = 3; i >= 0; --i) { s += hx[a[i] >> 4]; s += hx[a[i] & 15]; }
    return s;
}
/** EN 13757-3 / OMS device types. nullptr = not in the table (shown as "Meter (type 0xNN)"). */
inline const char* devTypeName(int t) {
    switch (t) {
        case 0x00: return "Meter";
        case 0x01: return "Oil meter";
        case 0x02: return "Electricity meter";
        case 0x03: return "Gas meter";
        case 0x04: return "Heat meter";
        case 0x05: return "Steam meter";
        case 0x06: return "Warm water meter";
        case 0x07: return "Water meter";
        case 0x08: return "Heat cost allocator";
        case 0x09: return "Compressed air meter";
        case 0x0A: return "Cooling meter";
        case 0x0B: return "Cooling meter (inlet)";
        case 0x0C: return "Heat meter (inlet)";
        case 0x0D: return "Heat / cooling meter";
        case 0x0E: return "Bus / system component";
        case 0x0F: return "Meter (unknown medium)";
        case 0x15: return "Hot water meter";
        case 0x16: return "Cold water meter";
        case 0x17: return "Hot / cold water meter";
        case 0x18: return "Pressure meter";
        case 0x1A: return "Smoke detector";
        case 0x1B: return "Room sensor";
        case 0x1C: return "Gas detector";
        case 0x21: return "Valve";
        case 0x25: return "Display unit";
        case 0x28: return "Waste water meter";
        case 0x31: case 0x32: case 0x33: return "OMS collector / repeater";
        case 0x36: case 0x37: return "Radio converter";
        default: return nullptr;
    }
}
inline std::string typeText(int t) {
    if (const char* n = devTypeName(t)) return n;
    char b[32]; std::snprintf(b, sizeof b, "Meter (type 0x%02X)", t & 0xFF); return b;
}
/** Heat and cooling meters lead with energy; an allocator with its units; everything else with volume. */
inline bool energyType(int t) { return t == 0x04 || t == 0x0A || t == 0x0B || t == 0x0C || t == 0x0D; }
/** The configuration word's security mode in words (EN 13757-7). */
inline std::string secText(int mode) {
    switch (mode) {
        case 0: return "not encrypted";
        case 2: case 3: return "encrypted (DES)";
        case 4: return "encrypted (AES, mode 4)";
        case 5: return "encrypted (AES)";
        case 7: return "encrypted (AES, KDF)";
        case 8: case 9: case 10: return "encrypted (mode " + std::to_string(mode) + ")";
        case 13: return "encrypted (TLS)";
        default: return "security mode " + std::to_string(mode);
    }
}
/** C-fields a METER sends (SND-NR, SND-IR, ACC-NR, ACC-DMD, RSP-UD). Anything else is a collector talking to a meter. */
inline bool meterTalking(uint8_t c) {
    return c == 0x44 || c == 0x46 || c == 0x47 || c == 0x48 || c == 0x08 || c == 0x18 || c == 0x28 || c == 0x38;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  WHAT A FRAME SAID
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
struct Record {
    std::string q;        ///< "volume", "energy", "flow", "date", "error flags", … ("" = not named)
    double v = NAN;       ///< the value in `u`
    std::string u;        ///< "m³", "kWh", "GJ", "m³/h", "°C" …
    std::string t;        ///< text: a date ("2026-09-30"), plain text
    int st = 0;           ///< storage number
    int fn = 0;           ///< 0 instantaneous, 1 maximum, 2 minimum, 3 during error
    int tariff = 0, sub = 0;
    bool qualified = false;   ///< a VIFE (or an extension) not understood: kept OUT of the line
    int dec = 3;          ///< decimals worth showing
};
struct Meter {
    std::string wm;           ///< "T1" | "C1-A" | "C1-B" | "S1"
    uint8_t c = 0;
    std::string mfr;          ///< "SEN"
    std::string id;           ///< the serial, 8 characters
    int ver = 0, type = 0;
    std::string via;          ///< "KAM 00123456" when the link layer was a repeater / converter (CI 0x72)
    int ci = -1;
    int acc = -1, sts = -1, cw = -1, sec = -1, encBlocks = 0;
    bool ellEnc = false, authenticated = false;
    int payloadLen = 0;       ///< the application payload's length (encrypted or not) — a number only
    bool damaged = false;     ///< block 1 good, a later block bad: the meter is heard, its readings are not
    bool accOk = false;       ///< the transport header (ACC/STS/CW) came from a block whose CRC passed
    std::string note;         ///< "manufacturer format 0xA2", "compact frame", "CI 0x51", "data not understood"
    std::vector<Record> recs;
    float carrierHz = 0;      ///< the demodulator's carrier, relative to the burst's channel centre
};

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  FRAMES: blocks and CRCs → the frame's bytes without CRCs
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** Bytes on air for a format-A frame of L-field `L`: 1 + L + 2 × (1 + ceil((L − 9) / 16)). */
inline size_t lenA(int L) { return L < 9 ? 0 : size_t(1 + L + 2 * (1 + (L - 9 + 15) / 16)); }
/** Format A. `out` = L … end without CRCs, as far as the blocks were good. `good` = bytes of `out` proven by a CRC
 *  (10 = block 1 only). True = every block good. */
inline bool frameA(const uint8_t* f, size_t n, std::vector<uint8_t>& out, size_t& good) {
    out.clear(); good = 0;
    if (n < 12) return false;
    const int L = f[0];
    if (L < 9 || n < lenA(L)) return false;
    size_t p = 0;
    size_t rem = size_t(L) - 9;
    size_t blk = 10;
    while (true) {
        const uint16_t want = uint16_t(f[p + blk] << 8 | f[p + blk + 1]);
        if (crc(f + p, blk) != want) return false;
        out.insert(out.end(), f + p, f + p + blk);
        good = out.size();
        p += blk + 2;
        if (!rem) return true;
        blk = std::min<size_t>(16, rem);
        rem -= blk;
    }
}
/** Format B: L counts every byte after itself INCLUDING the CRCs. Block 1 (10 bytes) has no CRC of its own; block 2
 *  (≤ 118 bytes incl. its CRC) is checked over blocks 1 + 2; a block 3 (only when L > 127) over itself.
 *  ★ ASSUMED from the standard's block sizes (10 + 118 = 128) — a real C1-B capture with L > 127 confirms block 3. */
inline bool frameB(const uint8_t* f, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n < 1) return false;
    const size_t L = f[0];
    if (L < 12 || n < 1 + L) return false;
    const size_t end = 1 + L;
    const size_t e2 = std::min<size_t>(end, 128);               // block 1 + 2 end, incl. CRC
    if (crc(f, e2 - 2) != uint16_t(f[e2 - 2] << 8 | f[e2 - 1])) return false;
    out.assign(f, f + e2 - 2);
    if (end > 128) {
        if (end - 128 < 3) return false;
        if (crc(f + 128, end - 130) != uint16_t(f[end - 2] << 8 | f[end - 1])) return false;
        out.insert(out.end(), f + 128, f + end - 2);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  DATA RECORDS (EN 13757-3) — only ever on a payload that was sent in the clear
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** One record's value. False = the data could not be read (not BCD, ran off the end). */
inline bool readValue(int code, const uint8_t* d, size_t len, double& v) {
    auto ints = [&](size_t nb) {
        uint64_t u = 0; for (size_t i = 0; i < nb; ++i) u |= uint64_t(d[i]) << (8 * i);
        if (nb < 8 && (u >> (8 * nb - 1)) & 1) u |= ~uint64_t(0) << (8 * nb);    // sign-extend
        v = double(int64_t(u)); return true;
    };
    auto bcd = [&](size_t nb) {
        double x = 0, m = 1; bool neg = false;
        for (size_t i = 0; i < nb; ++i) {
            const int lo = d[i] & 15, hi = d[i] >> 4;
            if (lo > 9) return false;
            x += lo * m; m *= 10;
            if (hi > 9) { if (i == nb - 1 && hi == 0xF) { neg = true; break; } return false; }
            x += hi * m; m *= 10;
        }
        v = neg ? -x : x; return true;
    };
    switch (code) {
        case 1: return len >= 1 && ints(1);
        case 2: return len >= 2 && ints(2);
        case 3: return len >= 3 && ints(3);
        case 4: return len >= 4 && ints(4);
        case 6: return len >= 6 && ints(6);
        case 7: return len >= 8 && ints(8);
        case 5: { if (len < 4) return false; uint32_t u = uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24;
                  float f; std::memcpy(&f, &u, 4); v = f; return std::isfinite(f); }
        case 9: return len >= 1 && bcd(1);
        case 10: return len >= 2 && bcd(2);
        case 11: return len >= 3 && bcd(3);
        case 12: return len >= 4 && bcd(4);
        case 14: return len >= 6 && bcd(6);
        default: return false;
    }
}
inline int dataLen(int code) { static const int k[16] = {0, 1, 2, 3, 4, 4, 6, 8, 0, 1, 2, 3, 4, -1, 6, -1}; return k[code & 15]; }

/** Parse records from `p`. False = the payload could not be followed to its end ("data not understood" if nothing came
 *  out). Records parsed before a problem are kept. */
inline bool parseRecords(const uint8_t* p, size_t n, std::vector<Record>& out) {
    size_t i = 0;
    while (i < n) {
        const uint8_t dif = p[i++];
        if (dif == 0x2F) continue;                                  // idle filler
        if ((dif & 0x0F) == 0x0F) return true;                      // 0x0F / 0x1F manufacturer data, or a readout request: stop
        Record r;
        const int code = dif & 0x0F;
        r.fn = (dif >> 4) & 3;
        r.st = (dif >> 6) & 1;
        bool ext = dif & 0x80;
        int k = 0;
        while (ext) {
            if (i >= n || k >= 10) return false;
            const uint8_t dife = p[i++];
            r.st |= (dife & 15) << (1 + 4 * k);
            r.tariff |= ((dife >> 4) & 3) << (2 * k);
            r.sub |= ((dife >> 6) & 1) << k;
            ext = dife & 0x80; ++k;
        }
        if (i >= n) return false;
        const uint8_t vif = p[i++];
        int ext1 = -1;                                              // 0xFD / 0xFB: the extension table's code
        std::string plainUnit;
        bool more = vif & 0x80, manuf = (vif & 0x7F) == 0x7F;
        if (vif == 0xFD || vif == 0xFB) {
            if (i >= n) return false;
            const uint8_t e = p[i++];
            ext1 = (vif == 0xFB ? 0x100 : 0) | (e & 0x7F);
            more = e & 0x80;
        }
        if ((vif & 0x7F) == 0x7C) {                                 // plain-text unit: length + ASCII (sent reversed)
            if (i >= n) return false;
            const size_t ln = p[i++];
            if (i + ln > n) return false;
            for (size_t j = 0; j < ln; ++j) plainUnit.insert(plainUnit.begin(), char(p[i + j]));
            i += ln;
        }
        int vifes = 0;
        while (more) { if (i >= n || ++vifes > 10) return false; more = p[i++] & 0x80; r.qualified = true; }
        // the data
        int len = dataLen(code);
        uint8_t lvar = 0;
        if (code == 13) {
            if (i >= n) return false;
            lvar = p[i++];
            if (lvar < 0xC0) len = lvar;
            else if (lvar >= 0xC0 && lvar <= 0xC9) len = lvar - 0xC0;
            else if (lvar >= 0xD0 && lvar <= 0xD9) len = lvar - 0xD0;
            else if (lvar >= 0xE0 && lvar <= 0xEF) len = lvar - 0xE0;
            else return false;
        }
        if (len < 0 || i + size_t(len) > n) return false;
        const uint8_t* d = p + i;
        i += size_t(len);
        if (manuf) continue;                                        // manufacturer-specific: skipped
        if (code == 13) {
            if (lvar < 0xC0) { r.q = "text"; for (int j = len - 1; j >= 0; --j) r.t += char(d[j] >= 0x20 && d[j] < 0x7F ? d[j] : '?'); r.qualified = true; out.push_back(r); }
            continue;
        }
        if (code == 0 || code == 8) continue;                       // no data
        double raw = NAN;
        const bool ok = readValue(code, d, size_t(len), raw);
        const int v7 = vif & 0x7F;
        auto scaled = [&](const char* q, const char* u, int pw, int dec) { r.q = q; r.u = u; r.v = raw * std::pow(10.0, pw); r.dec = std::max(0, dec); };
        if (ext1 >= 0) {
            if (ext1 == 0x17) { r.q = "error flags"; r.v = raw; r.dec = 0; }
            else if (ext1 == 0x0E || ext1 == 0x0F) { r.q = ext1 == 0x0E ? "firmware version" : "software version"; r.v = raw; r.dec = 0; }
            else if (ext1 == 0x74) { r.q = "battery"; r.u = "days"; r.v = raw; r.dec = 0; }
            else { char b[24]; std::snprintf(b, sizeof b, "ext 0x%s%02X", ext1 & 0x100 ? "FB " : "FD ", ext1 & 0x7F); r.q = b; r.v = raw; r.dec = 0; r.qualified = true; }
            // ★ ASSUMED: these 0xFD codes are from the design's reading of EN 13757-3 table 2 — check before naming more
        } else if (v7 <= 0x07) scaled("energy", "kWh", (v7 & 7) - 6, 6 - (v7 & 7));
        else if (v7 <= 0x0F) scaled("energy", "GJ", (v7 & 7) - 9, 9 - (v7 & 7));
        else if (v7 <= 0x17) scaled("volume", "m\xc2\xb3", (v7 & 7) - 6, 3);
        else if (v7 <= 0x1F) scaled("mass", "kg", (v7 & 7) - 3, 3 - (v7 & 7));
        else if (v7 <= 0x27) { static const char* u[4] = {"s", "min", "h", "days"}; r.q = v7 <= 0x23 ? "on time" : "operating time"; r.u = u[v7 & 3]; r.v = raw; r.dec = 0; }
        else if (v7 <= 0x2F) scaled("power", "W", (v7 & 7) - 3, 3 - (v7 & 7));
        else if (v7 <= 0x37) scaled("power", "J/h", v7 & 7, 0);
        else if (v7 <= 0x3F) scaled("flow", "m\xc2\xb3/h", (v7 & 7) - 6, 3);
        else if (v7 <= 0x47) scaled("flow", "m\xc2\xb3/min", (v7 & 7) - 7, 7 - (v7 & 7));
        else if (v7 <= 0x4F) scaled("flow", "m\xc2\xb3/s", (v7 & 7) - 9, 9 - (v7 & 7));
        else if (v7 <= 0x57) scaled("mass flow", "kg/h", (v7 & 7) - 3, 3 - (v7 & 7));
        else if (v7 <= 0x5B) scaled("flow temperature", "\xc2\xb0" "C", (v7 & 3) - 3, 3 - (v7 & 3));
        else if (v7 <= 0x5F) scaled("return temperature", "\xc2\xb0" "C", (v7 & 3) - 3, 3 - (v7 & 3));
        else if (v7 <= 0x63) scaled("temperature difference", "K", (v7 & 3) - 3, 3 - (v7 & 3));
        else if (v7 <= 0x67) scaled("external temperature", "\xc2\xb0" "C", (v7 & 3) - 3, 3 - (v7 & 3));
        else if (v7 <= 0x6B) scaled("pressure", "bar", (v7 & 3) - 3, 3 - (v7 & 3));
        else if (v7 == 0x6C || v7 == 0x6D) {
            if (!ok || (code != 2 && code != 4)) { r.q = "date"; r.qualified = true; out.push_back(r); continue; }
            const uint32_t u = uint32_t(int64_t(raw)) & (code == 2 ? 0xFFFFu : 0xFFFFFFFFu);
            int day, mon, yr, hh = -1, mm = -1;
            if (v7 == 0x6C) { day = u & 31; mon = (u >> 8) & 15; yr = int((u >> 5) & 7) | int((u >> 12) & 15) << 3; }
            else { mm = u & 63; hh = (u >> 8) & 31; day = (u >> 16) & 31; mon = (u >> 24) & 15; yr = int((u >> 21) & 7) | int((u >> 28) & 15) << 3; }
            r.q = "date";
            if (day < 1 || day > 31 || mon < 1 || mon > 12 || (hh > 23) || (mm > 59)) { r.t = "invalid"; r.qualified = true; }
            else {
                char b[32];
                if (hh >= 0) std::snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d", 2000 + yr, mon, day, hh, mm);
                else std::snprintf(b, sizeof b, "%04d-%02d-%02d", 2000 + yr, mon, day);
                r.t = b;
            }
            out.push_back(r);
            continue;
        }
        else if (v7 == 0x6E) { r.q = "HCA units"; r.v = raw; r.dec = 0; }
        else if (v7 >= 0x74 && v7 <= 0x77) { static const char* u[4] = {"s", "min", "h", "days"}; r.q = "actuality"; r.u = u[v7 & 3]; r.v = raw; r.dec = 0; }
        else if (v7 == 0x78) { r.q = "fabrication number"; r.v = raw; r.dec = 0; }
        else if (v7 == 0x7C) { r.q = plainUnit.empty() ? "value" : plainUnit; r.v = raw; r.dec = 0; r.qualified = true; }
        else { char b[16]; std::snprintf(b, sizeof b, "VIF 0x%02X", v7); r.q = b; r.v = raw; r.dec = 0; r.qualified = true; }
        if (!ok) { r.v = NAN; r.qualified = true; }
        out.push_back(r);
    }
    return true;
}

/** The link layer, the CI field and the transport header from a CRC-checked frame `f` (L … without CRCs); `good` = the
 *  bytes a CRC proved. False = not a meter talking (a collector's command, or too short). */
inline bool parseFrame(const std::vector<uint8_t>& f, size_t good, Meter& m) {
    if (f.size() < 10 || good < 10) return false;
    m.c = f[1];
    if (!meterTalking(m.c)) return false;
    const uint16_t M = uint16_t(f[2] | f[3] << 8);
    m.mfr = mfrLetters(M);
    m.id = idText(&f[4]);
    m.ver = f[8]; m.type = f[9];
    m.damaged = good < f.size() || size_t(f[0]) + 1 > f.size();
    if (good <= 10) { m.damaged = size_t(f[0]) > 9; return true; }
    size_t p = 10;
    const size_t end = good;
    for (int hop = 0; hop < 4 && p < end; ++hop) {
        const uint8_t ci = f[p];
        m.ci = ci;
        if (ci == 0x8C) { if (p + 3 > end) return true; m.acc = f[p + 2]; p += 3; continue; }              // ELL short: CC, ACC
        if (ci == 0x8D) {                                                                                   // ELL long
            if (p + 9 > end) return true;
            m.acc = f[p + 2];
            const uint32_t sn = uint32_t(f[p + 3]) | uint32_t(f[p + 4]) << 8 | uint32_t(f[p + 5]) << 16 | uint32_t(f[p + 6]) << 24;
            // ★ ASSUMED (EN 13757-4 ELL session number): its top 3 bits say how what follows is encrypted; 0 = in the clear
            if ((sn >> 29) & 7) { m.ellEnc = true; m.payloadLen = int(end - (p + 9)); return true; }
            p += 9; continue;
        }
        if (ci == 0x90) {                                                                                   // AFL: AFLL, then its fields
            // ★ ASSUMED (EN 13757-7): the byte after the CI is the AFL's length
            if (p + 2 > end) return true;
            m.authenticated = true; p += 2 + f[p + 1]; continue;
        }
        if (ci == 0x8E || ci == 0x8F) { m.note = "extended link layer"; return true; }
        break;
    }
    if (p >= end) return true;
    const uint8_t ci = f[p];
    m.ci = ci;
    size_t q = p + 1;
    auto header = [&](size_t at) {
        m.acc = f[at]; m.sts = f[at + 1]; m.cw = f[at + 2] | f[at + 3] << 8;
        m.sec = (m.cw >> 8) & 0x1F;
        if (m.sec == 5 || m.sec == 7) m.encBlocks = (m.cw >> 4) & 15;
        m.accOk = true;
    };
    if (ci == 0x7A) {
        if (q + 4 > end) return true;
        header(q); q += 4;
    } else if (ci == 0x72) {
        if (q + 12 > end) return true;
        // ★ the meter's own address is HERE; the link layer's was a repeater / radio converter
        char via[40]; std::snprintf(via, sizeof via, "%s %s", m.mfr.c_str(), m.id.c_str()); m.via = via;
        m.id = idText(&f[q]);
        m.mfr = mfrLetters(uint16_t(f[q + 4] | f[q + 5] << 8));
        m.ver = f[q + 6]; m.type = f[q + 7];
        header(q + 8); q += 12;
    } else if (ci == 0x78) {
        m.sec = 0;
    } else if (ci == 0x79) { m.note = "compact frame"; return true; }
    else if (ci >= 0xA0 && ci <= 0xB7) { char b[40]; std::snprintf(b, sizeof b, "manufacturer format 0x%02X", ci); m.note = b; return true; }
    else { char b[16]; std::snprintf(b, sizeof b, "CI 0x%02X", ci); m.note = b; return true; }
    if ((m.sec == 7 || m.sec == 13) && q < end) ++q;     // ★ ASSUMED: a one-byte configuration-field extension (KDF / key)
    m.payloadLen = q < f.size() ? int(f.size() - q) : 0;
    if (m.sec != 0 || m.damaged) return true;            // ★★ encrypted: nothing of the payload is read, kept or sent
    if (q < end && !parseRecords(&f[q], end - q, m.recs) && m.recs.empty()) m.note = "data not understood";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE READABLE LINE
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
static const char* const kDot = " \xc2\xb7 ";
/** "1 234.567" — thousands grouped with a space, as the design writes them. */
inline std::string grouped(double v, int dec) {
    char b[64]; std::snprintf(b, sizeof b, "%.*f", dec, std::fabs(v));
    std::string s = b; const size_t dot = s.find('.');
    std::string ip = s.substr(0, dot), fp = dot == std::string::npos ? "" : s.substr(dot);
    std::string o;
    for (size_t i = 0; i < ip.size(); ++i) { if (i && (ip.size() - i) % 3 == 0) o += ' '; o += ip[i]; }
    return (v < 0 ? "-" : "") + o + fp;
}
inline std::string recValue(const Record& r) { return grouped(r.v, r.q == "volume" || r.q == "flow" ? 3 : r.dec) + (r.u.empty() ? "" : " " + r.u); }
inline const Record* findRec(const Meter& m, const char* q, int st, const char* unit = nullptr) {
    for (const auto& r : m.recs)
        if (!r.qualified && r.q == q && r.st == st && r.fn == 0 && r.tariff == 0 && r.sub == 0 && r.v == r.v && (!unit || r.u == unit)) return &r;
    return nullptr;
}
inline const Record* findDate(const Meter& m, int st) {
    for (const auto& r : m.recs) if (!r.qualified && r.q == "date" && r.st == st && r.tariff == 0 && r.sub == 0) return &r;
    return nullptr;
}
/** The readings for the line, in the design's order: the lead value, flow, the billing-day value with its date, error
 *  flags. Empty = nothing worth a line. */
inline std::vector<std::string> readings(const Meter& m) {
    std::vector<std::string> p;
    const char* lead = m.type == 0x08 ? "HCA units" : energyType(m.type) ? "energy" : "volume";
    const Record* now = findRec(m, lead, 0);
    if (now) p.push_back(m.type == 0x08 ? grouped(now->v, 0) + " units" : recValue(*now));
    if (const Record* fl = findRec(m, "flow", 0, "m\xc2\xb3/h"); fl && fl->v != 0) p.push_back("flow " + recValue(*fl));
    if (const Record* s1 = findRec(m, lead, 1)) {
        const Record* d = findDate(m, 1);
        static const char* mon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        std::string when;
        int y, mo, dd;
        if (d && std::sscanf(d->t.c_str(), "%d-%d-%d", &y, &mo, &dd) == 3 && mo >= 1 && mo <= 12) when = std::to_string(dd) + " " + mon[mo - 1] + " ";
        const std::string v = m.type == 0x08 ? grouped(s1->v, 0) + " units" : recValue(*s1);
        p.push_back(when.empty() ? "stored " + v : when + v);
    }
    for (const auto& r : m.recs)
        if (!r.qualified && r.q == "error flags" && r.st == 0 && r.v == r.v) {
            char b[32];
            if (r.v == 0) p.push_back("no alarms"); else { std::snprintf(b, sizeof b, "alarm 0x%04llX", (unsigned long long)int64_t(r.v)); p.push_back(b); }
            break;
        }
    return p;
}
inline std::string statusText(int sts) {
    if (sts < 0) return "";
    std::vector<std::string> p;
    if (sts & 0x04) p.push_back("battery low");
    if (sts & 0x08) p.push_back("permanent error");
    if (sts & 0x10) p.push_back("error");
    if ((sts & 3) == 2 && !(sts & 0x18)) p.push_back("error");
    if ((sts & 3) == 3) p.push_back("alarm");
    std::string s; for (size_t i = 0; i < p.size(); ++i) { if (i) s += kDot; s += p[i]; }
    return s;
}
/** The maker as shown: its name, or its three letters. */
inline std::string makeText(const Meter& m) { const char* n = mfrName(m.mfr); return n ? n : m.mfr; }
/** "Water meter · Sensus · 12345678" — the device's name. */
inline std::string nameOf(const Meter& m) { return typeText(m.type) + kDot + makeText(m) + kDot + m.id; }
/** The security state, or the readings: what the line says about the payload. */
inline std::vector<std::string> payloadWords(const Meter& m) {
    if (m.ellEnc) return {"encrypted (AES, link layer)"};
    if (!m.note.empty()) return {m.note};
    if (m.sec > 0) return {secText(m.sec)};
    if (m.sec < 0) return {};
    std::vector<std::string> r = readings(m);
    if (r.empty()) r.push_back("not encrypted");
    return r;
}
/** THE CORE LINE — everything that is the meter's own news; the list and the gallery compare this (signal, interval and
 *  access number are not news). "Water meter · Sensus · 12345678 · v0x68 · encrypted (AES) · battery low". */
inline std::string coreLine(const Meter& m) {
    std::string s = nameOf(m);
    char v[16]; std::snprintf(v, sizeof v, "v0x%02X", m.ver & 0xFF); s += kDot; s += v;
    for (const auto& w : payloadWords(m)) { s += kDot; s += w; }
    const std::string st = statusText(m.sts);
    if (!st.empty()) { s += kDot; s += st; }
    if (!m.via.empty()) { s += kDot; s += "via " + m.via; }
    return s;
}
/** "every 16 s" / "every 2 min" / "every 3 h". */
inline std::string everyText(double sec) {
    if (!(sec > 0)) return "";
    if (sec < 90) return "every " + std::to_string(int(std::lround(sec))) + " s";
    if (sec < 90 * 60) return "every " + std::to_string(int(std::lround(sec / 60))) + " min";
    return "every " + std::to_string(int(std::lround(sec / 3600))) + " h";
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
//  THE DEMODULATOR
// ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** z[k] = x[k+1] · conj(x[k]) as two float arrays (re, im), k = 0 … n−2. ★ The burst's one pass at its full sample rate:
 *  NEON (ARMv7 and AArch64) / SSE2 four at a time, scalar for the tail. */
inline void products(const cf* x, size_t n, float* zr, float* zi) {
    if (n < 2) return;
    const size_t m = n - 1;
    const float* f = reinterpret_cast<const float*>(x);
    size_t k = 0;
#if VIBE_WMBUS_NEON
    for (; k + 4 <= m; k += 4) {
        const float32x4x2_t a = vld2q_f32(f + 2 * (k + 1));   // x[k+1 … k+4]
        const float32x4x2_t b = vld2q_f32(f + 2 * k);         // x[k   … k+3]
        const float32x4_t re = vmlaq_f32(vmulq_f32(a.val[0], b.val[0]), a.val[1], b.val[1]);
        const float32x4_t im = vmlsq_f32(vmulq_f32(a.val[1], b.val[0]), a.val[0], b.val[1]);
        vst1q_f32(zr + k, re); vst1q_f32(zi + k, im);
    }
#elif VIBE_WMBUS_SSE
    for (; k + 4 <= m; k += 4) {
        const __m128 a0 = _mm_loadu_ps(f + 2 * (k + 1)), a1 = _mm_loadu_ps(f + 2 * (k + 1) + 4);
        const __m128 b0 = _mm_loadu_ps(f + 2 * k), b1 = _mm_loadu_ps(f + 2 * k + 4);
        const __m128 ar = _mm_shuffle_ps(a0, a1, _MM_SHUFFLE(2, 0, 2, 0)), ai = _mm_shuffle_ps(a0, a1, _MM_SHUFFLE(3, 1, 3, 1));
        const __m128 br = _mm_shuffle_ps(b0, b1, _MM_SHUFFLE(2, 0, 2, 0)), bi = _mm_shuffle_ps(b0, b1, _MM_SHUFFLE(3, 1, 3, 1));
        _mm_storeu_ps(zr + k, _mm_add_ps(_mm_mul_ps(ar, br), _mm_mul_ps(ai, bi)));
        _mm_storeu_ps(zi + k, _mm_sub_ps(_mm_mul_ps(ai, br), _mm_mul_ps(ar, bi)));
    }
#endif
    for (; k < m; ++k) {
        const cf a = x[k + 1], b = x[k];
        zr[k] = a.real() * b.real() + a.imag() * b.imag();
        zi[k] = a.imag() * b.real() - a.real() * b.imag();
    }
}

/** Counters the service shows. */
struct Stats { uint64_t tried = 0, frames = 0, damaged = 0, collector = 0; };

/** One burst's discriminator, rotated onto its carrier, as a running sum — so any chip's integral is two lookups. */
class Disc {
public:
    Disc(const cf* x, size_t n) : zr_(n > 1 ? n - 1 : 0), zi_(n > 1 ? n - 1 : 0) { products(x, n, zr_.data(), zi_.data()); }
    /** The carrier's phase step from samples [a, b): the angle of the summed products (balanced FSK averages out). */
    double carrier(size_t a, size_t b) const {
        double sr = 0, si = 0;
        b = std::min(b, zr_.size());
        for (size_t k = a; k < b; ++k) { sr += zr_[k]; si += zi_[k]; }
        return std::atan2(si, sr);
    }
    void rotate(double th) {
        const float c = float(std::cos(th)), s = float(std::sin(th));
        P_.assign(zr_.size() + 1, 0.0);
        double acc = 0;
        for (size_t k = 0; k < zr_.size(); ++k) { acc += double(zi_[k] * c - zr_[k] * s); P_[k + 1] = acc; }
        th_ = th;
    }
    size_t size() const { return P_.size(); }
    double P(double t) const {
        if (t <= 0) return P_.front();
        const double last = double(P_.size() - 1);
        if (t >= last) return P_.back();
        const size_t i = size_t(t); const double f = t - double(i);
        return P_[i] + f * (P_[i + 1] - P_[i]);
    }
    double Pi(size_t i) const { return P_[std::min(i, P_.size() - 1)]; }
    double theta() const { return th_; }
private:
    std::vector<float> zr_, zi_;
    std::vector<double> P_;
    double th_ = 0;
};

enum Family { kTC, kS };
struct ModeSpec { double chipHz, lo, hi; int minCross; };
inline ModeSpec spec(Family f) {
    // ★ T: 90-110 kchip/s (AN067 Table 6; receivers allow 88-112); C: 100 k (taken as T's tolerance); S: 32.768 k ±2 %
    return f == kTC ? ModeSpec{100e3, 86e3, 114e3, 16} : ModeSpec{32768, 31.5e3, 34e3, 16};
}

/** Zero crossings of the one-chip integral (the chip boundaries' "centres" minus L/2), with hysteresis. */
inline std::vector<double> crossings(const Disc& d, int L) {
    std::vector<double> cr;
    const size_t N = d.size();
    if (N <= size_t(L) + 2) return cr;
    const size_t M = N - size_t(L);
    std::vector<float> mag; mag.reserve(M / 4 + 1);
    for (size_t i = 0; i < M; i += 4) mag.push_back(float(std::fabs(d.Pi(i + size_t(L)) - d.Pi(i))));
    std::nth_element(mag.begin(), mag.begin() + long(mag.size() * 9 / 10), mag.end());
    const double h = 0.2 * mag[mag.size() * 9 / 10];
    int st = 0; double zc = 0; double prev = d.Pi(size_t(L)) - d.Pi(0);
    for (size_t i = 1; i < M; ++i) {
        const double v = d.Pi(i + size_t(L)) - d.Pi(i);
        if ((v >= 0) != (prev >= 0)) zc = double(i - 1) + prev / (prev - v);   // the latest sign change, interpolated
        if (st >= 0 && v < -h) { if (st > 0) cr.push_back(zc); st = -1; }
        else if (st <= 0 && v > h) { if (st < 0) cr.push_back(zc); st = 1; }
        prev = v;
    }
    return cr;
}
struct Run { size_t a = 0, b = 0; double T = 0, phi = 0; };   // crossings [a, b], the fitted period and first boundary
/** Runs of crossings one chip apart — preambles — EARLIEST first, at most `maxRuns` (a frame's own data can hold a long
 *  0101… stretch — Manchester 0x00 bytes, NRZ 0x55 — that is longer than a short preamble; the preamble comes first).
 *  ★ Judged by PAIRS of spacings (two chips): a carrier estimate biased by unbalanced data shifts every other crossing
 *    one way and the next the other, so single spacings alternate short/long while each pair stays exactly 2T. */
inline std::vector<Run> preambles(const std::vector<double>& cr, double R, const ModeSpec& sp, int L, size_t maxRuns) {
    std::vector<Run> out;
    const double tlo = R / sp.hi, thi = R / sp.lo;
    size_t i = 0;
    while (i + 2 < cr.size() && out.size() < maxRuns) {
        size_t j = i;
        double sum = 0;
        while (j + 1 < cr.size()) {
            const double s = cr[j + 1] - cr[j];
            if (s < tlo * 0.4 || s > thi * 1.6) break;
            if (j > i) {
                const double pair = s + (cr[j] - cr[j - 1]);
                if (pair < 2 * tlo * 0.8 || pair > 2 * thi * 1.2) break;
                if (j > i + 4 && std::fabs(pair - 2 * sum / double(j - i)) > 0.2 * 2 * sum / double(j - i)) break;
            }
            sum += s; ++j;
        }
        if (int(j - i) >= sp.minCross) {
            // least squares c_k = c0 + k·T over the run
            const double K = double(j - i + 1);
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (size_t k = i; k <= j; ++k) { const double x = double(k - i), y = cr[k]; sx += x; sy += y; sxx += x * x; sxy += x * y; }
            Run r; r.a = i; r.b = j;
            r.T = (K * sxy - sx * sy) / (K * sxx - sx * sx);
            r.phi = (sy - r.T * sx) / K + 0.5 * L;
            if (r.T >= tlo * 0.97 && r.T <= thi * 1.03) out.push_back(r);
        }
        i = std::max(j, i + 1);
    }
    return out;
}
/** ★ `start`, never `B0`: Android's termios headers define B0 as a macro, and it broke the app's native build. Every chip from `start` on as a soft value (+ = the higher tone), with a timing loop on the transitions. */
inline std::vector<float> readChips(const Disc& d, double start, double T, int L, size_t maxChips) {
    std::vector<float> s;
    const double end = double(d.size() - 1);
    double B = start, Tc = T;
    float prev = 0;
    const double T0 = T;
    while (B + Tc <= end && s.size() < maxChips) {
        const float v = float(d.P(B + Tc) - d.P(B));
        if (!s.empty() && (v >= 0) != (prev >= 0)) {
            // a transition at B: where does the one-chip integral cross zero near B − L/2?
            const double c = B - 0.5 * L;
            const long lo = long(std::floor(c - 0.5 * Tc)), hi = long(std::ceil(c + 0.5 * Tc));
            double best = 1e9;
            for (long i = std::max(0L, lo); i < hi && size_t(i) + size_t(L) + 1 < d.size(); ++i) {
                const double a = d.Pi(size_t(i) + size_t(L)) - d.Pi(size_t(i));
                const double b = d.Pi(size_t(i) + 1 + size_t(L)) - d.Pi(size_t(i) + 1);
                if ((a >= 0) != (b >= 0)) { const double z = double(i) + a / (a - b); if (std::fabs(z - c) < std::fabs(best - c)) best = z; }
            }
            if (best < 1e8) {
                const double e = best - c;                         // + = the boundary is later than predicted
                B += 0.25 * e;
                Tc = std::min(T0 * 1.03, std::max(T0 * 0.97, Tc + 0.01 * e));
                // re-read this chip from the corrected boundary
                const float v2 = float(d.P(B + Tc) - d.P(B));
                s.push_back(v2); prev = v2; B += Tc; continue;
            }
        }
        s.push_back(v); prev = v; B += Tc;
    }
    return s;
}
/** Find `pat` (n chips, MSB first) in the hard chips of `s` from `from` to `to`; ≤ maxErr errors; sign ±1 = polarity. */
inline long findSync(const std::vector<float>& s, uint32_t pat, int n, long from, long to, int maxErr, int& pol) {
    for (int err = 0; err <= maxErr; ++err)
        for (long i = std::max(0L, from); i <= to && i + n <= long(s.size()); ++i)
            for (int p = 0; p < 2; ++p) {
                int e = 0;
                for (int k = 0; k < n && e <= err; ++k) {
                    const bool bit = (pat >> (n - 1 - k)) & 1;
                    const bool hard = p ? s[size_t(i + k)] < 0 : s[size_t(i + k)] >= 0;
                    if (bit != hard) ++e;
                }
                if (e == err) { pol = p ? -1 : 1; return i; }
            }
    return -1;
}

/** Chips → bytes. */
inline std::vector<uint8_t> bytesT(const std::vector<float>& s, size_t at, int pol, size_t maxBytes) {
    std::vector<uint8_t> o;
    while (o.size() < maxBytes && at + 12 <= s.size()) {
        int nib[2];
        for (int h = 0; h < 2; ++h) {
            float best = -1e30f; int bi = 0;
            for (int c = 0; c < 16; ++c) {
                float sc = 0;
                for (int k = 0; k < 6; ++k) sc += ((kCode36[c] >> (5 - k)) & 1 ? 1.0f : -1.0f) * s[at + size_t(k)] * float(pol);
                if (sc > best) { best = sc; bi = c; }
            }
            nib[h] = bi; at += 6;
        }
        o.push_back(uint8_t(nib[0] << 4 | nib[1]));
        if (o.size() == 1) maxBytes = std::min(maxBytes, lenA(o[0]) ? lenA(o[0]) : size_t(1));
    }
    return o;
}
inline std::vector<uint8_t> bytesNrz(const std::vector<float>& s, size_t at, int pol, size_t maxBytes, bool formatB) {
    std::vector<uint8_t> o;
    while (o.size() < maxBytes && at + 8 <= s.size()) {
        uint8_t v = 0;
        for (int k = 0; k < 8; ++k) v = uint8_t(v << 1 | (s[at + size_t(k)] * float(pol) >= 0 ? 1 : 0));
        at += 8; o.push_back(v);
        if (o.size() == 1) maxBytes = std::min(maxBytes, formatB ? size_t(1) + v : (lenA(v) ? lenA(v) : size_t(1)));
    }
    return o;
}
inline std::vector<uint8_t> bytesManchester(const std::vector<float>& s, size_t at, int pol, bool hiFirstIsOne, size_t maxBytes) {
    std::vector<uint8_t> o;
    while (o.size() < maxBytes && at + 16 <= s.size()) {
        uint8_t v = 0;
        for (int k = 0; k < 8; ++k) {
            const float d = (s[at] - s[at + 1]) * float(pol);
            v = uint8_t(v << 1 | ((d >= 0) == hiFirstIsOne ? 1 : 0));
            at += 2;
        }
        o.push_back(v);
        if (o.size() == 1) maxBytes = std::min(maxBytes, lenA(v) ? lenA(v) : size_t(1));
    }
    return o;
}

/** A frame's bytes (CRCs still in) → a Meter, by its format. 1 = a meter, 2 = a collector frame, 0 = nothing. */
inline int fromBytes(const std::vector<uint8_t>& raw, bool formatB, const char* wm, Meter& m) {
    std::vector<uint8_t> f; size_t good = 0;
    if (formatB) {
        if (!frameB(raw.data(), raw.size(), f)) return 0;
        good = f.size();
        f[0] = uint8_t(f.size() - 1);       // ★ as format A counts it (no CRCs), so "complete" reads the same below
    }
    else {
        frameA(raw.data(), raw.size(), f, good);
        if (good < 10) return 0;                                   // ★ block 1 failed its CRC: not a meter
        // the rest of the frame, as far as it came: so parseFrame knows the L-field promised more
        if (good < f.size()) f.resize(good);
    }
    Meter mm; mm.wm = wm;
    if (!parseFrame(f, good, mm)) return meterTalking(f[1]) ? 0 : 2;
    m = std::move(mm);
    return 1;
}

/** ★ The decoder: one burst → at most one meter. 1 = `out` is a meter, 2 = a collector's frame (counted, not listed),
 *  0 = not a wM-Bus frame. Only bursts with their baseband, at 868.95 (T/C) or 868.30 (S), get past the first lines. */
inline int decodeBurst(const Burst& b, double hwCentreHz, Meter& out, Stats* st = nullptr) {
    if (!b.bb || b.bbRate <= 0) return 0;
    const double f = hwCentreHz + b.offHz + b.carrierHz;
    const bool tc = std::fabs(f - 868.95e6) < 130e3, sm = std::fabs(f - 868.30e6) < 130e3;
    if (!tc && !sm) return 0;
    const std::vector<cf>& x = *b.bb;
    const double R = b.bbRate, durS = double(x.size()) / R;
    if (durS < 0.002 || durS > 0.25) return 0;
    if (st) ++st->tried;
    Disc d(x.data(), x.size());
    const Family fam = tc ? kTC : kS;
    const ModeSpec sp = spec(fam);
    const int L = std::max(2, int(std::lround(R / sp.chipHz)));
    d.rotate(d.carrier(0, x.size()));
    const std::vector<double> cr0 = crossings(d, L);
    const auto runs0 = preambles(cr0, R, sp, L, 4);
    for (const Run& r0 : runs0) {
        // ★ the carrier again, from the preamble alone (0101… is exactly balanced; NRZ data need not be)
        Disc& dd = d;
        const size_t a = size_t(std::max(0.0, r0.phi - 0.5 * L)), e = size_t(std::max(0.0, r0.phi + double(r0.b - r0.a) * r0.T));
        dd.rotate(dd.carrier(a, e));
        /* ★★ A SIDE CHANNEL IS NOT THE METER. A T1/C1 burst's spectrum is ~200 kHz wide and its skirts open channels of
         *  their own ±200 kHz away; in one of those the signal sits far off the channel's centre. The channel the meter
         *  is centred in decodes it (with the right frequency); this one gives up. */
        if (std::fabs(dd.theta() * R / (2 * M_PI)) > 90e3) continue;
        const auto cr = crossings(dd, L);
        auto runs = preambles(cr, R, sp, L, 64);
        const Run* r = nullptr;
        for (const Run& c : runs) if (std::fabs(c.phi - r0.phi) < double(r0.b - r0.a) * r0.T) { r = &c; break; }
        if (!r) continue;
        const double T = r->T;
        const size_t pre = r->b - r->a;                     // ≈ alternating chips seen
        const size_t maxChips = size_t((double(dd.size()) - r->phi) / T) + 2;
        const std::vector<float> s = readChips(dd, r->phi, T, L, maxChips);
        int pol = 1;
        if (fam == kTC) {
            const long at = findSync(s, 0x543D, 16, long(pre) - 14, long(pre) + 12, 1, pol);
            if (at < 0) continue;
            size_t j = size_t(at) + 16;
            int p2 = pol;
            // C: the next 8 chips are 01010100; then 11001101 (frame A) or 00111101 (frame B)
            bool isC = false, isB = false;
            if (j + 16 <= s.size()) {
                long c1 = findSync(s, 0x54CD, 16, long(j), long(j), 1, p2);
                if (c1 >= 0 && p2 == pol) isC = true;
                else { p2 = pol; c1 = findSync(s, 0x543D, 16, long(j), long(j), 1, p2); if (c1 >= 0 && p2 == pol) { isC = true; isB = true; } }
            }
            int res = 0; Meter m;
            if (isC) res = fromBytes(bytesNrz(s, j + 16, pol, 300, isB), isB, isB ? "C1-B" : "C1-A", m);
            if (!res) res = fromBytes(bytesT(s, j, pol, 300), false, "T1", m);
            if (res) {
                if (st) { if (res == 2) ++st->collector; else { ++st->frames; if (m.damaged) ++st->damaged; } }
                m.carrierHz = float(dd.theta() * R / (2 * M_PI));
                out = std::move(m);
                return res;
            }
        } else {
            // S: the preamble's last 10 chips + the 18-chip sync
            const uint32_t pat = 0x5547696;                   // 0101010101 · 000111011010010110 (28 chips)
            const long at = findSync(s, pat, 28, long(pre) - 16, long(pre) + 12, 1, pol);
            if (at < 0) continue;
            const size_t j = size_t(at) + 28;
            for (int conv = 0; conv < 2; ++conv) {
                Meter m;
                const int res = fromBytes(bytesManchester(s, j, pol, conv == 0, 300), false, "S1", m);
                if (res) {
                    if (st) { if (res == 2) ++st->collector; else { ++st->frames; if (m.damaged) ++st->damaged; } }
                    m.carrierHz = float(dd.theta() * R / (2 * M_PI));
                    out = std::move(m);
                    return res;
                }
            }
        }
    }
    return 0;
}

} // namespace wmbus
} // namespace vibeism
