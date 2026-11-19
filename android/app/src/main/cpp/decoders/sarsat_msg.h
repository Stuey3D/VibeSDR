// decoders/sarsat_msg.h — V12 COSPAS-SARSAT 406 MHz first-generation beacon messages: the two BCH codes and the protocols, from
// C/S T.001 "Specification for Cospas-Sarsat 406 MHz Distress Beacons" (Issue 4 Rev. 7, March 2021 — §2.2, §3, Annex A, Annex
// B; read in full for this work). Our own code. docs/v12/SARSAT.md.
//
// ★★ BIT NUMBERS ARE THE SPECIFICATION'S: bit 1 is the first bit transmitted. 1–15 bit sync (all 1), 16–24 frame sync
//    (000101111; 011010000 = SELF-TEST), 25 format flag (0 short 112 bits, 1 long 144), 26 protocol flag, 27–36 country code,
//    25–85 PDF-1, 86–106 BCH-1 (82,61) t = 3, 107–112 short: non-protected; long: 107–132 PDF-2, 133–144 BCH-2 (38,26) t = 2.
// ★★ THE BCH CODES (Annex B): g1(x) = x²¹+x¹⁸+x¹⁷+x¹⁵+x¹⁴+x¹²+x¹¹+x⁸+x⁷+x⁶+x⁵+x+1 (1001101101100111100011),
//    g2(x) = x¹²+x¹⁰+x⁸+x⁵+x⁴+x³+1 (1010100111001). The first data bit is the highest power (Annex B's long division).
//    Decoding: the syndrome is the XOR of each set bit's own syndrome; up to t errors are found by search over the positions
//    (82 positions, t = 3: ≤ 91 964 patterns, a few hundred µs, once per burst) — exact, and no Galois-field tables.
// ★ The 15 Hex ID (§3.3) is bits 26–85, with a location protocol's position bits replaced by the default pattern (A3.2).
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace vibe {
namespace sarsat {

using Bits = std::array<uint8_t, 145>;   ///< [1..144] as the specification numbers them; [0] unused

inline uint32_t field(const Bits& b, int from, int to) {   // bits from..to, the lowest-numbered the most significant
    uint32_t v = 0;
    for (int i = from; i <= to; ++i) v = (v << 1) | (b[(size_t)i] & 1u);
    return v;
}
inline void setField(Bits& b, int from, int to, uint32_t v) {
    for (int i = to; i >= from; --i) { b[(size_t)i] = (uint8_t)(v & 1u); v >>= 1; }
}

// ── the BCH codes ─────────────────────────────────────────────────────────────────────────────────────────────────
/** A shortened binary BCH code: `n` bits (the first at bit `first`), `r` parity bits, generator `g` (r + 1 bits). */
class Bch {
public:
    Bch(int first, int n, int r, uint32_t g, int t) : first_(first), n_(n), r_(r), t_(t) {
        syn_.resize((size_t)n);
        // x^k mod g for k = 0 … n−1; position i (bit first+i) is degree n−1−i
        uint32_t s = 1;
        std::vector<uint32_t> pw((size_t)n);
        for (int k = 0; k < n; ++k) {
            pw[(size_t)k] = s;
            s <<= 1;
            if (s & (1u << r)) s ^= g;
        }
        for (int i = 0; i < n; ++i) syn_[(size_t)i] = pw[(size_t)(n - 1 - i)];
    }
    uint32_t syndrome(const Bits& b) const {
        uint32_t s = 0;
        for (int i = 0; i < n_; ++i) if (b[(size_t)(first_ + i)] & 1u) s ^= syn_[(size_t)i];
        return s;
    }
    /** The parity for the data bits already in `b` (bits first … first+n−r−1), written into the parity bits. */
    void encode(Bits& b) const {
        for (int i = n_ - r_; i < n_; ++i) b[(size_t)(first_ + i)] = 0;
        const uint32_t s = syndrome(b);   // remainder of data·x^r
        for (int i = 0; i < r_; ++i) b[(size_t)(first_ + n_ - 1 - i)] = (uint8_t)((s >> i) & 1u);
    }
    /** Corrects up to t errors in place. @return the number corrected, or −1 when the word is not within t of a codeword.
     *  `maxErr` < t asks for fewer (a stricter decoder). */
    int correct(Bits& b, int maxErr = -1) const {
        const int lim = maxErr < 0 ? t_ : std::min(maxErr, t_);
        const uint32_t s = syndrome(b);
        if (!s) return 0;
        const int n = n_;
        for (int i = 0; i < n && lim >= 1; ++i) if (syn_[(size_t)i] == s) { flip(b, i); return 1; }
        if (lim >= 2)
            for (int i = 0; i < n; ++i) { const uint32_t a = s ^ syn_[(size_t)i];
                for (int j = i + 1; j < n; ++j) if (syn_[(size_t)j] == a) { flip(b, i); flip(b, j); return 2; } }
        if (lim >= 3)
            for (int i = 0; i < n; ++i) { const uint32_t a = s ^ syn_[(size_t)i];
                for (int j = i + 1; j < n; ++j) { const uint32_t c = a ^ syn_[(size_t)j];
                    for (int k = j + 1; k < n; ++k) if (syn_[(size_t)k] == c) { flip(b, i); flip(b, j); flip(b, k); return 3; } } }
        return -1;
    }
private:
    void flip(Bits& b, int i) const { b[(size_t)(first_ + i)] ^= 1u; }
    int first_, n_, r_, t_;
    std::vector<uint32_t> syn_;
};
inline const Bch& bch1() { static const Bch c(25, 82, 21, 0x26D9E3u, 3); return c; }   // 1001101101100111100011
inline const Bch& bch2() { static const Bch c(107, 38, 12, 0x1539u, 2); return c; }    // 1010100111001

// ── the modified-Baudot code (Table A3) ───────────────────────────────────────────────────────────────────────────
inline char baudot(uint32_t c6) {
    static const struct { uint8_t c; char ch; } t[] = {
        { 0x38, 'A' }, { 0x33, 'B' }, { 0x2E, 'C' }, { 0x32, 'D' }, { 0x30, 'E' }, { 0x36, 'F' }, { 0x2B, 'G' }, { 0x25, 'H' },
        { 0x2C, 'I' }, { 0x3A, 'J' }, { 0x3E, 'K' }, { 0x29, 'L' }, { 0x27, 'M' }, { 0x26, 'N' }, { 0x23, 'O' }, { 0x2D, 'P' },
        { 0x3D, 'Q' }, { 0x2A, 'R' }, { 0x34, 'S' }, { 0x21, 'T' }, { 0x3C, 'U' }, { 0x2F, 'V' }, { 0x39, 'W' }, { 0x37, 'X' },
        { 0x35, 'Y' }, { 0x31, 'Z' }, { 0x24, ' ' }, { 0x18, '-' }, { 0x17, '/' }, { 0x0D, '0' }, { 0x1D, '1' }, { 0x19, '2' },
        { 0x10, '3' }, { 0x0A, '4' }, { 0x01, '5' }, { 0x15, '6' }, { 0x1C, '7' }, { 0x0C, '8' }, { 0x03, '9' } };
    for (const auto& e : t) if (e.c == c6) return e.ch;
    return '?';
}
inline uint32_t baudotCode(char ch) {
    for (uint32_t c = 0; c < 64; ++c) if (baudot(c) == ch) return c;
    return 0x24;
}
inline std::string baudotText(const Bits& b, int from, int chars, int width = 6) {
    std::string s;
    for (int k = 0; k < chars; ++k) {
        uint32_t c = field(b, from + k * width, from + k * width + width - 1);
        if (width == 5) c |= 0x20;   // the shortened form omits the leading 1 of a letter
        s += baudot(c);
    }
    // right-justified with spaces where no character exists
    const size_t a = s.find_first_not_of(' '), z = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, z - a + 1);
}

// ── the message ───────────────────────────────────────────────────────────────────────────────────────────────────
struct Message {
    bool longMsg = false;
    bool selfTest = false;        ///< frame sync 011010000 (the beacon's self-test)
    bool testProtocol = false;    ///< a test protocol (user 111, location 1110/1111, RLS/ELT(DT) test coding)
    bool userProto = false;       ///< P = 1
    int country = 0;              ///< bits 27–36 (an ITU MID)
    int protoCode = 0;            ///< 3 bits (user) or 4 bits (location)
    std::string protocol;         ///< "Maritime user", "Standard location (EPIRB-MMSI)" …
    std::string beaconType;       ///< "EPIRB", "ELT", "PLB", "Ship security", "ELT(DT)", "" (unknown / national)
    std::string identity;         ///< what the beacon names itself by ("MMSI 232…123", "G-ABCD", "serial 12345" …)
    std::string hexId;            ///< the 15 Hex ID
    std::string hexMsg;           ///< bits 25 … the end, hex
    int bch1Errors = -1, bch2Errors = -1;   ///< corrected (−1 = failed / not present)
    bool hasPosition = false;     ///< an encoded position that is not the default pattern
    double lat = NAN, lon = NAN;
    std::string posRes;           ///< "4 s", "4 min", "15 min" …
    std::string posSource;        ///< "internal GNSS", "external navigation device", ""
    bool homing121 = false;
    std::string extra;            ///< emergency code / activation / altitude in words
};

inline std::string hexOf(const Bits& b, int from, int to) {
    std::string s; char c[2] = { 0, 0 };
    int i = from;
    // pad on the left to a whole number of nibbles
    const int n = to - from + 1, pad = (4 - n % 4) % 4;
    uint32_t v = 0; int k = pad;
    for (; i <= to; ++i) {
        v = (v << 1) | (b[(size_t)i] & 1u);
        if (++k == 4) { c[0] = "0123456789ABCDEF"[v]; s += c; v = 0; k = 0; }
    }
    return s;
}

/** ★ Parse a message whose BCH-1 (and BCH-2 when long) has already been checked. */
inline Message parse(const Bits& in, bool selfTest) {
    Bits b = in;
    Message m;
    m.selfTest = selfTest;
    m.longMsg = b[25] == 1;
    m.userProto = b[26] == 1;
    m.country = (int)field(b, 27, 36);
    m.hexMsg = hexOf(b, 25, m.longMsg ? 144 : 112);
    Bits id = b;   // for the 15 Hex ID: position fields set to the defaults
    auto pos4min = [&](int at) {   // user-location PDF-2, bit `at` = 108
        const uint32_t ns = b[(size_t)at], deg = field(b, at + 1, at + 7), mn = field(b, at + 8, at + 11) * 4;
        const uint32_t ew = b[(size_t)(at + 12)], ldeg = field(b, at + 13, at + 20), lmn = field(b, at + 21, at + 24) * 4;
        if (deg == 127 || ldeg == 255 || deg > 90 || ldeg > 180 || mn >= 60 || lmn >= 60) return;
        m.lat = (ns ? -1 : 1) * (deg + mn / 60.0); m.lon = (ew ? -1 : 1) * (ldeg + lmn / 60.0);
        m.hasPosition = true; m.posRes = "4 min";
    };
    if (m.userProto) {
        m.protoCode = (int)field(b, 37, 39);
        const uint32_t pc = (uint32_t)m.protoCode;
        const uint32_t rl = field(b, 84, 85);
        m.homing121 = rl == 1;
        switch (pc) {
        case 2: {   // maritime user: MMSI or radio call sign, 6 modified-Baudot characters
            m.protocol = "Maritime user"; m.beaconType = "EPIRB";
            const std::string s = baudotText(b, 40, 6);
            bool digits = !s.empty(); for (char c : s) if (c < '0' || c > '9') digits = false;
            m.identity = digits ? "MMSI " + std::to_string(m.country) + s : "call sign " + s;
            m.identity += " · beacon " + std::string(1, baudot(field(b, 76, 81)));
            break; }
        case 6: {   // radio call sign user: 4 Baudot + 3 BCD
            m.protocol = "Radio call sign user"; m.beaconType = "EPIRB";
            std::string s = baudotText(b, 40, 4);
            for (int k = 0; k < 3; ++k) { const uint32_t d = field(b, 64 + 4 * k, 67 + 4 * k); if (d <= 9) s += (char)('0' + d); }
            m.identity = "call sign " + s + " · beacon " + std::string(1, baudot(field(b, 76, 81)));
            break; }
        case 1:     // aviation user: registration, 7 Baudot characters
            m.protocol = "Aviation user"; m.beaconType = "ELT";
            m.identity = "registration " + baudotText(b, 40, 7) + " · ELT " + std::to_string(field(b, 82, 83) + 1);
            break;
        case 3: {   // serial user
            m.protocol = "Serial user";
            const uint32_t tt = field(b, 40, 42), cflag = b[43];
            const char* type = tt == 0 ? "ELT" : tt == 1 ? "ELT" : tt == 2 ? "EPIRB" : tt == 4 ? "EPIRB" : tt == 6 ? "PLB" : tt == 3 ? "ELT" : "";
            m.beaconType = type;
            if (tt == 2) m.beaconType = "EPIRB (float-free)";
            if (tt == 4) m.beaconType = "EPIRB (non float-free)";
            char t[120];
            if (tt == 3) std::snprintf(t, sizeof t, "aircraft address %06X · ELT %u", field(b, 44, 67), field(b, 68, 73) + 1);
            else if (tt == 1) std::snprintf(t, sizeof t, "operator %s · serial %u", baudotText(b, 44, 3).c_str(), field(b, 62, 73));
            else std::snprintf(t, sizeof t, "serial %u", field(b, 44, 63));
            m.identity = t;
            if (cflag) { std::snprintf(t, sizeof t, " · type approval %u", field(b, 74, 83)); m.identity += t; }
            break; }
        case 7: m.protocol = "Test user"; m.testProtocol = true; break;
        case 0: m.protocol = "Orbitography"; break;
        case 4: m.protocol = "National user"; break;
        default: m.protocol = "Reserved (second generation)"; break;
        }
        if (m.longMsg && pc != 4 && pc != 0) {   // user-location protocols carry a 4-minute position in PDF-2
            m.protocol += "-location";
            m.posSource = b[107] ? "internal navigation device" : "external navigation device";
            pos4min(108);
        } else if (!m.longMsg) {
            const uint32_t nu = field(b, 107, 112);
            if (b[107] && (pc == 2 || pc == 6 || (pc == 3 && (field(b, 40, 42) == 2 || field(b, 40, 42) == 4)))) {
                static const char* const mar[16] = { "unspecified distress", "fire / explosion", "flooding", "collision", "grounding",
                    "listing, in danger of capsizing", "sinking", "disabled and adrift", "abandoning ship", "", "", "", "", "", "", "" };
                const uint32_t e = field(b, 109, 112);
                m.extra = std::string("emergency: ") + (e == 0 ? mar[0] : e <= 7 ? mar[e] : e == 8 ? mar[8] : "spare code");
            } else if (b[107]) {
                m.extra = std::string("emergency:") + (b[109] ? " fire" : "") + (b[110] ? " medical help required" : "") + (b[111] ? " disabled" : "");
                if (m.extra == "emergency:") m.extra = "emergency: no details";
            }
            m.extra += std::string(m.extra.empty() ? "" : " · ") + (b[108] ? "automatic or manual activation" : "manual activation only");
            (void)nu;
        }
    } else {
        m.protoCode = (int)field(b, 37, 40);
        const uint32_t pc = (uint32_t)m.protoCode;
        auto offset4s = [&](int at, int minBits, int signBit) {   // sign, minutes, seconds (4 s)
            const int s = b[(size_t)signBit] ? 1 : -1;
            const uint32_t mn = field(b, at, at + minBits - 1), sc = field(b, at + minBits, at + minBits + 3);
            if (sc == 15) return std::make_pair(false, 0.0);
            return std::make_pair(true, s * (mn / 60.0 + sc * 4 / 3600.0));
        };
        auto apply = [&](double lat, double lon, int ns, int ew, int latSign, int latMin, int minBits, int lonSign, int lonMin) {
            m.lat = (ns ? -1 : 1) * lat; m.lon = (ew ? -1 : 1) * lon; m.hasPosition = true;
            const auto dl = offset4s(latMin, minBits, latSign), dn = offset4s(lonMin, minBits, lonSign);
            if (m.longMsg && dl.first && dn.first) {
                // the offset is added in the direction of the coarse value (A3.3.1: 100° W + 30′ = 100° 30′ W)
                m.lat += (ns ? -1 : 1) * dl.second; m.lon += (ew ? -1 : 1) * dn.second; m.posRes = "4 s";
            }
        };
        if (pc == 2 || pc == 3 || (pc >= 4 && pc <= 7) || pc == 12 || pc == 14) {
            m.protocol = pc == 14 ? "Standard test location" : "Standard location";
            m.testProtocol = pc == 14;
            m.beaconType = pc == 2 ? "EPIRB" : pc == 3 ? "ELT" : pc == 4 ? "ELT" : pc == 5 ? "ELT" : pc == 6 ? "EPIRB" : pc == 7 ? "PLB" : pc == 12 ? "Ship security" : "";
            char t[120];
            if (pc == 2 || pc == 12) std::snprintf(t, sizeof t, "MMSI %d%06u", m.country, field(b, 41, 60));
            else if (pc == 3) std::snprintf(t, sizeof t, "aircraft address %06X", field(b, 41, 64));
            else if (pc == 5) std::snprintf(t, sizeof t, "operator %s · serial %u", baudotText(b, 41, 3, 5).c_str(), field(b, 56, 64));
            else if (pc == 14) std::snprintf(t, sizeof t, "test data %06X", field(b, 41, 64));
            else std::snprintf(t, sizeof t, "type approval %u · serial %u", field(b, 41, 50), field(b, 51, 64));
            m.identity = t;
            if (pc == 2) { std::snprintf(t, sizeof t, " · beacon %u", field(b, 61, 64)); m.identity += t; }
            const uint32_t latQ = field(b, 66, 74), lonQ = field(b, 76, 85);
            if (!(latQ == 511 && lonQ == 1023) && latQ <= 360 && lonQ <= 720) {
                m.posRes = "15 min";
                apply(latQ / 4.0, lonQ / 4.0, b[65], b[75], 113, 114, 5, 123, 124);
            }
            if (m.longMsg) { m.posSource = b[111] ? "internal navigation device" : "external navigation device"; m.homing121 = b[112]; }
            setField(id, 65, 74, 0x1FF); setField(id, 75, 85, 0x3FF);
        } else if (pc == 8 || pc == 10 || pc == 11 || pc == 15) {
            m.protocol = pc == 15 ? "National test location" : "National location";
            m.testProtocol = pc == 15;
            m.beaconType = pc == 8 ? "ELT" : pc == 10 ? "EPIRB" : pc == 11 ? "PLB" : "";
            m.identity = "national serial " + std::to_string(field(b, 41, 58));
            const uint32_t latD = field(b, 60, 66), latM = field(b, 67, 71) * 2, lonD = field(b, 73, 80), lonM = field(b, 81, 85) * 2;
            if (!(latD == 127 && lonD == 255) && latD <= 90 && lonD <= 180 && latM < 60 && lonM < 60) {
                m.posRes = "2 min";
                if (m.longMsg && b[110]) apply(latD + latM / 60.0, lonD + lonM / 60.0, b[59], b[72], 113, 114, 2, 120, 121);
                else { m.lat = (b[59] ? -1 : 1) * (latD + latM / 60.0); m.lon = (b[72] ? -1 : 1) * (lonD + lonM / 60.0); m.hasPosition = true; }
            }
            if (m.longMsg) { m.posSource = b[111] ? "internal navigation device" : "external navigation device"; m.homing121 = b[112]; }
            setField(id, 59, 71, 0x7Fu << 5); setField(id, 72, 85, 0xFFu << 5);
        } else if (pc == 13 || pc == 9) {
            const bool rls = pc == 13;
            m.protocol = rls ? "RLS location" : "ELT(DT) location";
            const uint32_t t2 = field(b, 41, 42);
            char t[120];
            if (rls) {
                const bool mmsi = field(b, 43, 46) == 15;
                m.beaconType = mmsi ? (t2 == 2 ? "PLB" : t2 == 3 ? "" : "EPIRB") : (t2 == 0 ? "ELT" : t2 == 1 ? "EPIRB" : t2 == 2 ? "PLB" : "");
                m.testProtocol = t2 == 3;
                if (mmsi) std::snprintf(t, sizeof t, "MMSI %d%06u", m.country, field(b, 47, 66));
                else std::snprintf(t, sizeof t, "RLS TAC %u%03u · serial %u", t2 + 1, field(b, 43, 52), field(b, 53, 66));
            } else {
                m.beaconType = "ELT(DT)";
                const uint32_t v = field(b, 43, 66);
                m.testProtocol = v == 0 || v == 0xFFFFFF;
                if (t2 == 0) std::snprintf(t, sizeof t, "aircraft address %06X", v);
                else if (t2 == 1) std::snprintf(t, sizeof t, "operator %s · serial %u", baudotText(b, 43, 3, 5).c_str(), field(b, 58, 66));
                else std::snprintf(t, sizeof t, "type approval %u · serial %u", field(b, 43, 52), field(b, 53, 66));
            }
            m.identity = t;
            const uint32_t latH = field(b, 68, 75), lonH = field(b, 77, 85);
            if (!(latH == 255 && lonH == 511) && latH <= 180 && lonH <= 360) {
                m.posRes = "30 min";
                const bool offs = rls || field(b, 113, 114) != 0;
                if (m.longMsg && offs) apply(latH / 2.0, lonH / 2.0, b[67], b[76], 115, 116, 4, 124, 125);
                else { m.lat = (b[67] ? -1 : 1) * latH / 2.0; m.lon = (b[76] ? -1 : 1) * lonH / 2.0; m.hasPosition = true; }
            }
            if (rls) { m.posSource = b[107] ? "internal navigation device" : "external navigation device"; m.homing121 = b[108]; }
            else {
                static const char* const act[4] = { "manual activation", "automatic activation by the beacon", "automatic activation by external means", "" };
                m.extra = act[field(b, 107, 108)];
                const uint32_t alt = field(b, 109, 112);
                static const int hi[15] = { 400, 800, 1200, 1600, 2200, 2800, 3400, 4000, 4800, 5600, 6600, 7600, 8800, 10000, 0 };
                if (alt < 14) { char a[60]; std::snprintf(a, sizeof a, " · altitude up to %d m", hi[alt]); m.extra += a; }
                else if (alt == 14) m.extra += " · altitude above 10 000 m";
            }
            setField(id, 67, 75, 0xFF); setField(id, 76, 85, 0x1FF);
        } else {
            m.protocol = "Spare location protocol";
        }
    }
    m.hexId = hexOf(id, 26, 85);
    return m;
}

/** ★ Plausibility: a real beacon's country code is an ITU MID (201–775). */
inline bool plausible(const Message& m) { return m.country >= 201 && m.country <= 775; }

} // namespace sarsat
} // namespace vibe
