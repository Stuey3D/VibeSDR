// vibe_adsb_commb.h — Mode S Comm-B (DF20/21): the 56-bit MB field of a reply to a ground radar's question,
// read for the registers Enhanced Surveillance carries — BDS 1,0 / 1,7 / 2,0 / 3,0 / 4,0 / 4,4 / 5,0 / 6,0.
//
// ★★★ OUR OWN, FROM THE PUBLISHED DESCRIPTION. The register layouts and the inference checks follow Junzi Sun,
//     "The 1090 Megahertz Riddle" (2nd ed., TU Delft, 2021, open access, CC BY) chapters on Comm-B and Enhanced
//     Surveillance — the same book vibe_adsb.h is built from. GPL decoders are not copied.
//
// ★★★ AN MB FIELD DOES NOT SAY WHICH REGISTER IT IS. The radar asked for one; the reply only carries the 56 data
//     bits. Four of the registers start with their own number (1,0 / 1,7's shape / 2,0 / 3,0); the rest (4,0, 4,4,
//     5,0, 6,0) must be INFERRED. What we believe, in order:
//       1. Data parity (Annex 10 Vol IV): some radars ask the aircraft to overlay its parity with its address
//          XOR the requested register number in the top 8 bits. If the plain address is unknown but that XOR
//          gives a known aircraft for exactly one register, the reply has said which register it is.
//       2. Self-identifying registers (2,0, 3,0, 1,0, 1,7): their header and reserved bits are checked.
//       3. The rest: each register's status bits must agree with its data (a "not available" field is all zero),
//          its reserved bits must be zero and its values must be in range; then, where the aircraft's own ADS-B
//          gives a reference (ground speed, track, vertical rate, altitude), the register must agree with it.
//          A reply that passes for MORE than one register is thrown away (ambiguous) — never guessed.
//     And a register's values reach the JSON only when CONFIRMED: by the data-parity overlay, by agreeing with the
//     ADS-B reference, or by a second reply within 30 s that inferred the same register with close values. Until then
//     it is counted, not shown (Stuart's rule: never show a guessed register as fact).
//
// ★ Memory: a few hundred bytes per aircraft (the latest value of each register), inside vibe_adsb.h's table.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace vibeadsb {
namespace commb {

/** The MB field as a 56-bit number, MB bit 1 = the most significant (message bit 33). */
inline uint64_t mbOf(const uint8_t* m) {
    uint64_t v = 0;
    for (int i = 4; i < 11; ++i) v = (v << 8) | m[i];
    return v;
}
/** MB bits a..b inclusive, numbered 1..56 as the book numbers them. */
inline uint32_t mb(uint64_t v, int a, int b) { return uint32_t((v >> (56 - b)) & ((1ull << (b - a + 1)) - 1)); }
/** A sign bit then a magnitude of `n` bits, two's complement (the book's "sign + value" fields). */
inline int sgn(uint64_t v, int signBit, int a, int b) {
    const int n = b - a + 1;
    const int x = int(mb(v, a, b));
    return mb(v, signBit, signBit) ? x - (1 << n) : x;
}

enum Reg : uint8_t { R10 = 0, R17, R20, R30, R40, R44, R50, R60, kRegs };
inline const char* regName(int r) {
    static const char* n[] = {"1,0", "1,7", "2,0", "3,0", "4,0", "4,4", "5,0", "6,0"};
    return r >= 0 && r < kRegs ? n[r] : "?";
}
/** The register's number as it goes in the data-parity overlay (BDS1 << 4 | BDS2). */
inline uint8_t regCode(int r) {
    static const uint8_t c[] = {0x10, 0x17, 0x20, 0x30, 0x40, 0x44, 0x50, 0x60};
    return c[r];
}

// ── the registers ────────────────────────────────────────────────────────────────────────────────────────────
struct Bds40 { bool hasMcp = false, hasFms = false, hasBaro = false; int mcpFt = 0, fmsFt = 0; double baroHpa = 0;
               bool hasModes = false, vnav = false, altHold = false, approach = false; };
struct Bds50 { bool hasRoll = false, hasTrack = false, hasGs = false, hasRate = false, hasTas = false;
               double roll = 0, track = 0, rate = 0; int gs = 0, tas = 0; };
struct Bds60 { bool hasHdg = false, hasIas = false, hasMach = false, hasVrBaro = false, hasVrIns = false;
               double heading = 0, mach = 0; int ias = 0, vrBaro = 0, vrIns = 0; };
struct Bds44 { int fom = 0; bool hasWind = false, hasPress = false, hasHum = false, hasTurb = false;
               int windKt = 0; double windDir = 0, tempC = 0; int pressHpa = 0; double humidity = 0; int turb = 0; };
struct Bds30 { bool active = false, multiple = false, corrective = false, down = false, increase = false,
               reversal = false, crossing = false, positive = false, terminated = false; int rac = 0, tti = 0;
               uint32_t threat = 0; };
struct Bds17 { uint32_t caps = 0; };   ///< 24 flags, MB bit 1 first
struct Bds10 { bool continuation = false, overlay = false, acas = false; int version = 0; };

// ── decoders: each returns false when the MB field cannot be that register (status, reserved bits, ranges) ──
/** BDS 1,0 — data link capability. MB 1-8 = 0001 0000, MB 10-14 reserved (zero). */
inline bool dec10(uint64_t v, Bds10& o) {
    if (mb(v, 1, 8) != 0x10 || mb(v, 10, 14) != 0) return false;
    o.continuation = mb(v, 9, 9); o.overlay = mb(v, 15, 15); o.acas = mb(v, 16, 16); o.version = int(mb(v, 17, 23));
    return o.version <= 3;               // Mode S subnetwork versions defined so far: 0–3 (higher = noise)
}
/** BDS 1,7 — common-usage GICB capability report: MB 1-24 one flag per register, 25-56 reserved (zero).
 *  ★ An aircraft that answers GICB has 2,0 (bit 7) — a report without it is not this register. */
inline bool dec17(uint64_t v, Bds17& o) {
    if (mb(v, 25, 56) != 0) return false;
    o.caps = mb(v, 1, 24);
    return (o.caps & (1u << (24 - 7))) != 0;
}
/** One of the 24 capability flags of 1,7, by register code (0x05…0x60), or false. */
inline bool caps17(const Bds17& c, uint8_t code) {
    static const uint8_t order[24] = {0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x20, 0x21, 0x40, 0x41, 0x42, 0x43,
                                      0x44, 0x45, 0x48, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x5F, 0x60};
    for (int i = 0; i < 24; ++i) if (order[i] == code) return (c.caps >> (23 - i)) & 1;
    return false;
}
/** BDS 2,0 — aircraft identification: MB 1-8 = 0010 0000, then eight 6-bit characters (ICAO alphabet). */
inline bool dec20(uint64_t v, std::string& cs) {
    if (mb(v, 1, 8) != 0x20) return false;
    static const char* al = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
    cs.clear();
    for (int i = 0; i < 8; ++i) {
        const char c = al[mb(v, 9 + 6 * i, 14 + 6 * i)];
        if (c == '#') return false;        // ★ not a character the alphabet has: not an identification
        cs += c;
    }
    while (!cs.empty() && cs.back() == ' ') cs.pop_back();
    return !cs.empty() && cs[0] != ' ';
}
/** BDS 3,0 — ACAS resolution advisory: MB 1-8 = 0011 0000; TTI 3 is undefined. */
inline bool dec30(uint64_t v, Bds30& o) {
    if (mb(v, 1, 8) != 0x30) return false;
    o.tti = int(mb(v, 29, 30));
    if (o.tti == 3) return false;
    const uint32_t ara = mb(v, 9, 22);
    o.active = (ara >> 13) & 1;            // MB 9: one threat, the RA in MB 10-15
    o.multiple = !o.active && ((ara >> 12) & 1);
    o.corrective = (ara >> 12) & 1; o.down = (ara >> 11) & 1; o.increase = (ara >> 10) & 1;
    o.reversal = (ara >> 9) & 1; o.crossing = (ara >> 8) & 1; o.positive = (ara >> 7) & 1;
    if (mb(v, 16, 22) >= 48) return false; // ★ the ACAS III bits: never this many set on ACAS II
    o.rac = int(mb(v, 23, 26)); o.terminated = mb(v, 27, 27);
    o.threat = o.tti == 1 ? mb(v, 31, 54) : 0;
    return o.active || o.multiple || o.terminated;
}
/** A status bit and its field: status 0 must come with an all-zero field. */
inline bool st(uint64_t v, int s, int a, int b, bool& has) {
    has = mb(v, s, s);
    return has || mb(v, a, b) == 0;
}
/** BDS 4,0 — selected vertical intention. */
inline bool dec40(uint64_t v, Bds40& o) {
    if (!st(v, 1, 2, 13, o.hasMcp) || !st(v, 14, 15, 26, o.hasFms) || !st(v, 27, 28, 39, o.hasBaro)) return false;
    if (mb(v, 40, 47) != 0 || mb(v, 52, 53) != 0) return false;
    if (!o.hasMcp && !o.hasFms && !o.hasBaro) return false;
    o.mcpFt = int(mb(v, 2, 13)) * 16; o.fmsFt = int(mb(v, 15, 26)) * 16;
    o.baroHpa = mb(v, 28, 39) * 0.1 + 800.0;
    if (o.hasMcp && (o.mcpFt < 0 || o.mcpFt > 50000)) return false;
    if (o.hasFms && (o.fmsFt < 0 || o.fmsFt > 50000)) return false;
    if (o.hasBaro && (o.baroHpa < 900 || o.baroHpa > 1100)) return false;
    // ★ a selected altitude is set in 100 ft steps (some panels 50); at 16 ft resolution it lands within 16 of one
    auto round = [](int ft) { const int r = ft % 100; return r <= 16 || r >= 84 || (r >= 34 && r <= 66); };
    if (o.hasMcp && !round(o.mcpFt)) return false;
    o.hasModes = mb(v, 48, 48);
    if (!o.hasModes && mb(v, 49, 51)) return false;
    o.vnav = mb(v, 49, 49); o.altHold = mb(v, 50, 50); o.approach = mb(v, 51, 51);
    if (!mb(v, 54, 54) && mb(v, 55, 56)) return false;
    // round the 16-ft steps back to what was dialled
    if (o.hasMcp) o.mcpFt = int(std::lround(o.mcpFt / 50.0) * 50);
    if (o.hasFms) o.fmsFt = int(std::lround(o.fmsFt / 50.0) * 50);
    return true;
}
/** BDS 5,0 — track and turn report. */
inline bool dec50(uint64_t v, Bds50& o) {
    if (!st(v, 1, 2, 11, o.hasRoll) || !st(v, 12, 13, 23, o.hasTrack) || !st(v, 24, 25, 34, o.hasGs)
        || !st(v, 35, 36, 45, o.hasRate) || !st(v, 46, 47, 56, o.hasTas)) return false;
    if (!o.hasRoll && !o.hasTrack && !o.hasGs && !o.hasRate && !o.hasTas) return false;
    o.roll = sgn(v, 2, 3, 11) * 45.0 / 256.0;
    o.track = sgn(v, 13, 14, 23) * 90.0 / 512.0; if (o.track < 0) o.track += 360;
    o.gs = int(mb(v, 25, 34)) * 2;
    o.rate = sgn(v, 36, 37, 45) * 8.0 / 256.0;
    o.tas = int(mb(v, 47, 56)) * 2;
    if (o.hasRoll && std::fabs(o.roll) > 50) return false;
    if (o.hasGs && o.gs > 600) return false;
    if (o.hasTas && (o.tas > 500 || o.tas < 50)) return false;
    if (o.hasGs && o.hasTas && std::abs(o.gs - o.tas) > 200) return false;
    if (o.hasRate && std::fabs(o.rate) > 8) return false;   // 8 °/s is past any airliner's rate-one turn ×2
    return true;
}
/** BDS 6,0 — heading and speed report. */
inline bool dec60(uint64_t v, Bds60& o) {
    if (!st(v, 1, 2, 12, o.hasHdg) || !st(v, 13, 14, 23, o.hasIas) || !st(v, 24, 25, 34, o.hasMach)
        || !st(v, 35, 36, 45, o.hasVrBaro) || !st(v, 46, 47, 56, o.hasVrIns)) return false;
    if (!o.hasHdg && !o.hasIas && !o.hasMach && !o.hasVrBaro && !o.hasVrIns) return false;
    o.heading = sgn(v, 2, 3, 12) * 90.0 / 512.0; if (o.heading < 0) o.heading += 360;
    o.ias = int(mb(v, 14, 23));
    o.mach = mb(v, 25, 34) * 2.048 / 512.0;
    o.vrBaro = sgn(v, 36, 37, 45) * 32;
    o.vrIns = sgn(v, 47, 48, 56) * 32;
    if (o.hasIas && (o.ias > 500 || o.ias < 30)) return false;
    if (o.hasMach && o.mach > 1.0) return false;
    if (o.hasVrBaro && std::abs(o.vrBaro) > 6000) return false;
    if (o.hasVrIns && std::abs(o.vrIns) > 6000) return false;
    if (o.hasVrBaro && o.hasVrIns && std::abs(o.vrBaro - o.vrIns) > 2000) return false;
    return true;
}
/** BDS 4,4 — meteorological routine air report (MRAR). The temperature has no status bit of its own. */
inline bool dec44(uint64_t v, Bds44& o) {
    o.fom = int(mb(v, 1, 4));
    if (o.fom == 0 || o.fom > 4) return false;   // 1 INS, 2 GNSS, 3 DME/DME, 4 VOR/DME; 0 = invalid, 5+ reserved
    if (!st(v, 5, 6, 23, o.hasWind) || !st(v, 35, 36, 46, o.hasPress) || !st(v, 47, 48, 49, o.hasTurb)
        || !st(v, 50, 51, 56, o.hasHum)) return false;
    if (!o.hasWind) return false;                // ★ the useful part, and what makes the shape distinctive
    o.windKt = int(mb(v, 6, 14));
    o.windDir = mb(v, 15, 23) * 180.0 / 256.0;
    o.tempC = sgn(v, 24, 25, 34) * 0.25;
    o.pressHpa = int(mb(v, 36, 46));
    o.turb = int(mb(v, 48, 49));
    o.humidity = mb(v, 51, 56) * 100.0 / 64.0;
    if (o.windKt > 250) return false;
    if (o.tempC < -80 || o.tempC > 60) return false;
    if (o.hasPress && (o.pressHpa < 100 || o.pressHpa > 1100)) return false;
    return true;
}

// ── ISA, for the altitude checks ─────────────────────────────────────────────────────────────────────────────
/** ISA temperature (°C) at a pressure altitude (ft), troposphere then the isothermal layer. */
inline double isaTempC(double ft) { const double h = ft * 0.3048; return h < 11000 ? 15.0 - 0.0065 * h : -56.5; }
/** CAS (kt) → Mach at a pressure altitude (ft), ISA (subsonic compressible formulae). */
inline double casToMach(double cas, double ft) {
    const double h = ft * 0.3048;
    const double T = isaTempC(ft) + 273.15;
    const double p = h < 11000 ? 101325.0 * std::pow(T / 288.15, 5.25588) : 22632.06 * std::exp(-(h - 11000) / 6341.62);
    const double a0 = 340.294, p0 = 101325.0, vc = cas * 0.514444;
    const double qc = p0 * (std::pow(1 + 0.2 * (vc / a0) * (vc / a0), 3.5) - 1);
    return std::sqrt(5 * (std::pow(qc / p + 1, 2.0 / 7.0) - 1));
}
inline double angDiff(double a, double b) { double d = std::fmod(std::fabs(a - b), 360.0); return d > 180 ? 360 - d : d; }

/** What the aircraft's own ADS-B says, for the cross-checks — `has*` false where we do not know. */
struct Ref {
    bool hasGs = false, hasTrack = false, hasAlt = false, hasVr = false;
    double gs = 0, track = 0; int altFt = 0, vr = 0;
};

/** The outcome of one reply. */
struct Inference {
    int reg = -1;              ///< which register, or -1
    bool confirmed = false;    ///< by data parity or the ADS-B cross-check (a second agreeing reply is the caller's)
    bool ambiguous = false;    ///< passed for more than one: thrown away
    Bds10 b10; Bds17 b17; std::string b20; Bds30 b30; Bds40 b40; Bds44 b44; Bds50 b50; Bds60 b60;
};

/** Agreement with the ADS-B reference: the number of independent quantities that agree (0 = nothing to compare),
 *  -1 if any disagrees (so not this register). TWO agreeing confirm at once (infer); one only lets it be a candidate. */
inline int check50(const Bds50& b, const Ref& r) {
    int ok = 0;
    if (b.hasGs && r.hasGs) { if (std::fabs(b.gs - r.gs) > 20) return -1; ++ok; }
    if (b.hasTrack && r.hasTrack) { if (angDiff(b.track, r.track) > 8) return -1; ++ok; }
    return ok;
}
inline int check60(const Bds60& b, const Ref& r) {
    int ok = 0;
    // ★ heading is magnetic and the wind sets the aircraft off its track: 45° covers variation plus a strong crosswind
    if (b.hasHdg && r.hasTrack && r.hasGs && r.gs > 80) { if (angDiff(b.heading, r.track) > 45) return -1; ++ok; }
    if (b.hasVrBaro && r.hasVr) { if (std::abs(b.vrBaro - r.vr) > 800) return -1; ++ok; }
    if (b.hasIas && b.hasMach && r.hasAlt && r.altFt > 1000) {
        if (std::fabs(casToMach(b.ias, r.altFt) - b.mach) > 0.08) return -1;
        ++ok;
    }
    if (b.hasIas && r.hasGs && r.hasAlt && r.altFt < 10000 && std::fabs(b.ias - r.gs) > 150) return -1;
    return ok;
}
inline int check44(const Bds44& b, const Ref& r) {
    if (!r.hasAlt) return 0;
    return std::fabs(b.tempC - isaTempC(r.altFt)) > 30 ? -1 : 1;   // ★ ISA ± 30 °C covers every real day aloft
}
inline int check40(const Bds40& b, const Ref& r) {
    if (!r.hasAlt || !b.hasMcp) return 0;
    // ★ an aircraft climbing or descending is heading for its selected altitude: not away from it
    if (r.hasVr && std::abs(r.vr) > 1000) {
        const int to = b.mcpFt - r.altFt;
        if ((to > 0) != (r.vr > 0) && std::abs(to) > 1000) return -1;
        return 1;
    }
    return 0;
}

/** Which register this MB field is. `dpCode` >= 0 when the data-parity overlay already said (regCode of it). */
inline Inference infer(uint64_t v, const Ref& r, int dpCode = -1) {
    Inference in;
    if (v == 0) return in;
    if (dpCode >= 0) {
        // the radar asked for this register and the reply's parity proves it
        bool ok = false;
        switch (dpCode) {
            case 0x10: ok = dec10(v, in.b10); in.reg = R10; break;
            case 0x17: ok = dec17(v, in.b17); in.reg = R17; break;
            case 0x20: ok = dec20(v, in.b20); in.reg = R20; break;
            case 0x30: ok = dec30(v, in.b30); in.reg = R30; break;
            case 0x40: ok = dec40(v, in.b40); in.reg = R40; break;
            case 0x44: ok = dec44(v, in.b44); in.reg = R44; break;
            case 0x50: ok = dec50(v, in.b50); in.reg = R50; break;
            case 0x60: ok = dec60(v, in.b60); in.reg = R60; break;
            default: break;
        }
        if (!ok) in.reg = -1; else in.confirmed = true;
        return in;
    }
    // self-identifying first: their headers cannot be an accident of 4,0–6,0 status bits AND still pass their checks
    if (dec20(v, in.b20)) { in.reg = R20; return in; }
    if (dec30(v, in.b30)) { in.reg = R30; return in; }
    if (dec10(v, in.b10)) { in.reg = R10; return in; }
    int n = 0, pick = -1, conf = 0;
    auto cand = [&](int reg, bool pass, int agree) {
        if (!pass || agree < 0) return;
        ++n; pick = reg; conf = agree;
    };
    cand(R40, dec40(v, in.b40), check40(in.b40, r));
    cand(R50, dec50(v, in.b50), check50(in.b50, r));
    cand(R60, dec60(v, in.b60), check60(in.b60, r));
    cand(R44, dec44(v, in.b44), check44(in.b44, r));
    if (n == 0 && dec17(v, in.b17)) { in.reg = R17; return in; }
    if (n == 1) { in.reg = pick; in.confirmed = conf >= 2; return in; }
    if (n > 1) in.ambiguous = true;
    return in;
}

/** Two inferences of the same register "agree" (the second-reply confirmation): values close in time. */
inline bool agrees(const Inference& a, const Inference& b) {
    if (a.reg != b.reg) return false;
    switch (a.reg) {
        case R40: return a.b40.hasMcp == b.b40.hasMcp && std::abs(a.b40.mcpFt - b.b40.mcpFt) <= 100
                      && (!a.b40.hasBaro || !b.b40.hasBaro || std::fabs(a.b40.baroHpa - b.b40.baroHpa) < 1);
        case R50: return (!a.b50.hasGs || !b.b50.hasGs || std::abs(a.b50.gs - b.b50.gs) <= 20)
                      && (!a.b50.hasTrack || !b.b50.hasTrack || angDiff(a.b50.track, b.b50.track) <= 20)
                      && (!a.b50.hasTas || !b.b50.hasTas || std::abs(a.b50.tas - b.b50.tas) <= 20);
        case R60: return (!a.b60.hasHdg || !b.b60.hasHdg || angDiff(a.b60.heading, b.b60.heading) <= 20)
                      && (!a.b60.hasIas || !b.b60.hasIas || std::abs(a.b60.ias - b.b60.ias) <= 20)
                      && (!a.b60.hasMach || !b.b60.hasMach || std::fabs(a.b60.mach - b.b60.mach) <= 0.04);
        case R44: return std::abs(a.b44.windKt - b.b44.windKt) <= 20 && angDiff(a.b44.windDir, b.b44.windDir) <= 30
                      && std::fabs(a.b44.tempC - b.b44.tempC) <= 5;
        default: return true;
    }
}

/** Per aircraft: the latest CONFIRMED value of each register, and the last unconfirmed one waiting for a second. */
struct State {
    double at[kRegs] = {};            ///< when each register was last confirmed (0 = never)
    Inference last[kRegs];            ///< its confirmed values
    Inference pend[kRegs];            ///< an unconfirmed inference awaiting a second reply
    double pendAt[kRegs] = {};
    bool anyCaps = false;             ///< a 1,7 report was seen (its flags in last[R17])
};

/** Apply one inference to an aircraft's state at `now`. Returns true if a register became/stayed confirmed. */
inline bool apply(State& s, const Inference& in, double now) {
    if (in.reg < 0) return false;
    // ★ an aircraft's own capability report rules a register out: if 1,7 says it has no 6,0, a "6,0" is something else
    if (s.anyCaps && (in.reg == R40 || in.reg == R44 || in.reg == R50 || in.reg == R60) && !in.confirmed
        && !caps17(s.last[R17].b17, regCode(in.reg))) return false;
    bool ok = in.confirmed || in.reg == R20 || in.reg == R30 || in.reg == R10 || in.reg == R17;
    if (!ok && s.pendAt[in.reg] > 0 && now - s.pendAt[in.reg] <= 30 && agrees(s.pend[in.reg], in)) ok = true;
    if (!ok) { s.pend[in.reg] = in; s.pendAt[in.reg] = now; return false; }
    s.last[in.reg] = in; s.at[in.reg] = now;
    if (in.reg == R17) s.anyCaps = true;
    return true;
}

/** ,"commb":{…} for the aircraft JSON — only confirmed registers heard in the last `maxAge` s (ACAS: 60 s).
 *  Field names follow the ones aircraft.json made familiar where one exists (nav_altitude_mcp, nav_qnh, roll, ias,
 *  mach, wd/ws, oat …), inside their own object so a client can say where they came from. */
inline std::string json(const State& s, double now, double maxAge = 30) {
    std::string o;
    char b[160];
    auto fresh = [&](int r, double age) { return s.at[r] > 0 && now - s.at[r] <= age; };
    auto add = [&](const char* fmt, auto... a) { std::snprintf(b, sizeof b, fmt, a...); o += b; };
    if (fresh(R40, maxAge)) {
        const Bds40& x = s.last[R40].b40;
        if (x.hasMcp) add(",\"nav_altitude_mcp\":%d", x.mcpFt);
        if (x.hasFms) add(",\"nav_altitude_fms\":%d", x.fmsFt);
        if (x.hasBaro) add(",\"nav_qnh\":%.1f", x.baroHpa);
        if (x.hasModes) {
            std::string m;
            if (x.vnav) m += "\"vnav\",";
            if (x.altHold) m += "\"althold\",";
            if (x.approach) m += "\"approach\",";
            if (!m.empty()) m.pop_back();
            o += ",\"nav_modes\":[" + m + "]";
        }
    }
    if (fresh(R50, maxAge)) {
        const Bds50& x = s.last[R50].b50;
        if (x.hasRoll) add(",\"roll\":%.1f", x.roll);
        if (x.hasTrack) add(",\"true_track\":%.1f", x.track);
        if (x.hasGs) add(",\"gs\":%d", x.gs);
        if (x.hasRate) add(",\"track_rate\":%.2f", x.rate);
        if (x.hasTas) add(",\"tas\":%d", x.tas);
    }
    if (fresh(R60, maxAge)) {
        const Bds60& x = s.last[R60].b60;
        if (x.hasHdg) add(",\"mag_heading\":%.1f", x.heading);
        if (x.hasIas) add(",\"ias\":%d", x.ias);
        if (x.hasMach) add(",\"mach\":%.3f", x.mach);
        if (x.hasVrBaro) add(",\"baro_rate\":%d", x.vrBaro);
        if (x.hasVrIns) add(",\"geom_rate\":%d", x.vrIns);
    }
    if (fresh(R44, 120)) {           // ★ weather changes slowly; 4,4 is asked for rarely
        const Bds44& x = s.last[R44].b44;
        add(",\"ws\":%d,\"wd\":%.0f,\"oat\":%.2f", x.windKt, x.windDir, x.tempC);
        if (x.hasPress) add(",\"static_hpa\":%d", x.pressHpa);
        if (x.hasHum) add(",\"humidity\":%.0f", x.humidity);
    }
    if (fresh(R20, 300)) {
        o += ",\"ident\":\"";
        for (char c : s.last[R20].b20) if (c >= ' ' && c <= 'Z') o += c;
        o += '"';
    }
    if (fresh(R30, 60)) {
        const Bds30& x = s.last[R30].b30;
        add(",\"acas_ra\":{\"age\":%.0f,\"active\":%s,\"terminated\":%s", now - s.at[R30],
            x.active || x.multiple ? "true" : "false", x.terminated ? "true" : "false");
        if (x.active) {
            // the advisory in plain words (Annex 10 Vol IV ARA bits 41–47)
            const char* t = x.corrective ? (x.down ? (x.positive ? "Descend" : "Reduce climb")
                                                   : (x.positive ? "Climb" : "Reduce descent"))
                                         : (x.down ? "Do not climb" : "Do not descend");
            add(",\"advisory\":\"%s%s%s\"", t, x.crossing ? " (crossing)" : "", x.reversal ? " (reversed)" : "");
        } else if (x.multiple) o += ",\"advisory\":\"Multiple threats\"";
        if (x.threat) add(",\"threat\":\"%06x\"", x.threat);
        o += '}';
    }
    if (fresh(R17, 600)) {
        const Bds17& x = s.last[R17].b17;
        o += ",\"bds\":[";
        bool first = true;
        for (uint8_t c : {uint8_t(0x40), uint8_t(0x44), uint8_t(0x50), uint8_t(0x60)})
            if (caps17(x, c)) { add("%s\"%d,%d\"", first ? "" : ",", c >> 4, c & 15); first = false; }
        o += ']';
    }
    if (o.empty()) return "";
    o[0] = '{';
    return ",\"commb\":" + o + "}";
}

/** Counters for the report and the tests: how many MB fields were read, and what became of them. */
struct Stats { uint64_t fields = 0, identified[kRegs] = {}, confirmed = 0, ambiguous = 0, unknown = 0, dataParity = 0; };

} // namespace commb
} // namespace vibeadsb
