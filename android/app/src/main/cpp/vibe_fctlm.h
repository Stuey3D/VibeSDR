// vibe_fctlm.h — V12 beta90: FUNcube telemetry (AO-73 / FUNcube-1; beta94 JY1Sat / JO-97, units), docs/v12/SATELLITE-BEST.md 1.2 + 1.3. Our own code.
//
// ★★★ SOURCES (published, read for facts only):
//   · AMSAT-UK, "FUNcube-1 Downlink Data", issue 1.0 final as flown (2015-03-05), "unlimited public release":
//     1200 bps BPSK with the AO-40 FEC; a 256-byte frame = 2-bit satellite id + 6-bit frame type (MSB first) + 440 bits
//     of real-time telemetry + 1600 bits (200 bytes) of payload; one frame every 5 s; the 24-frame schedule (WO1–12,
//     HR1–3, FM1–9); the real-time field list and widths (EPS 192, BOB 100, RF 48, PA 32, ANTS 20, SW 48). It gives
//     no calibration itself. funcube.org.uk/wp-content/uploads/2010/11/funcube-1-downlink-data-spec_ver1-as_flown.pdf
//   · ★★ beta94 AMSAT-UK, "FUNcube-1 telemetry_equations_b1" (G. Shirville, Sept 2013 — the handbook: "All telemetry
//     calibration equations are on the FUNcube website"), funcube.org.uk/wp-content/uploads/2010/11/funcube-1_-telemetry_
//     equations_b1.xls: per real-time channel the equation or "real values are transmitted" (the EPS: mV, mA, °C), the
//     PA temperature table, the AntS sensor curve, the sun sensors' log(lux) table, reset-cause and PPT words.
//   · ★★ beta94 AMSAT-UK, "JY1SAT data budget with tlm equations 2F" (Nov 2018), funcube.org.uk/wp-content/uploads/2018/
//     11/jy1sat_data_budget-with-tlm-equations-2f.xls: JY1Sat's OWN 432-bit real-time layout, field by field, with its
//     conversions ("Treat as FC1" = FUNcube-1's) — linked from funcube.org.uk/2018/11/15/jy1sat/.
//   · ★★ beta94 AMSAT-UK, "ESEO Downlink Data" issue 1.2 §3 "Extended Sat Id/Frame Types", funcube.org.uk/wp-content/
//     uploads/2018/11/eseo_downlink_data_1_21a.pdf: satellite id 3 = an extended 16-bit header (S6 S7 F2–F7 | S0–S5 F0
//     F1), then 432 bits of real-time data — the scheme JY1Sat uses.
//   · AMSAT-UK, "The FUNcube Handbook" v1.3 (the PA: forward / reverse power mW, current mA, temperature °C; 300 mW sunlit).
//   All four are AMSAT-UK's public documents, read for facts; no code of anyone else's was read or copied.
//   · P. Karn KA9Q, "AO-40 FEC telemetry" (ka9q.net/papers/ao40tlm.html): two RS(160,128) words (even data bytes → word
//     0, odd → word 1), shortened from CCSDS (255,223) by 95 leading zeros, field x⁸+x⁷+x²+x+1, roots α^(11j)
//     j = 112…143, CONVENTIONAL basis; the PN h(x)=x⁸+x⁷+x⁵+x³+1 from all ones over the 320 RS bytes, its first bit on the
//     first byte's MSB; K=7 r½ 171/133 (register fed from the high end), G2 INVERTED, starting all zeros, six zero tail
//     bits unscrambled; written by rows into an 80 × 65 interleaver from row 2, row 1 = the 65-bit sync vector
//     (x⁷+x³+1 from all ones), read out by columns into the differential encoder; 5132 coded + 65 sync = 5197 of 5200
//     (the last three unused). Bytes MSB first.
// ★★ WHAT THE SOURCES DO NOT SAY, and how we do not depend on it: which of the two coder outputs goes first, the
//    differential code's polarity (and whether FUNcube's 1200 Bd keeps AO-40's differential code at all), the carrier's
//    phase. The framer tries the sync vector on the coherent stream and on its differential decode, both signs (four
//    cases, cheap), and the Viterbi on both output orders; RS decides. A frame is trusted only when BOTH RS words
//    decode AND the 95 padding bytes are still zero after correction — 0 false frames on noise (test-fctlm §3).
// ★★★ WHAT THE AIR SAYS (AO-73, 2026-11-24 15:27–15:38 UTC, three recordings, ~64 frames decoded — test-fctlm §10):
//    · DIFFERENTIAL, and a 1 is NO phase change (the framer's form 3: the differential decode, inverted). Our test
//      transmitter now sends exactly that by default (vibe_fctlm_tx.h).
//    · the symbol clock is ~1201.6 Bd — 1400 ppm FAST — on both the Airspy (XCover) and the V4L (Pi 500): the
//      satellite's own clock. The receiver's timing loop was proportional-only and held ±800 ppm: it slipped symbols
//      all pass and the sync never matched (0 syncs, 0 frames, with a 12 dB signal). vibe_bpsk_rx.h now integrates.
//    · the frame TYPE counts 0–23, not 1–24: the sequence number steps on type 0, which follows type 23 (WO1 = 0).
//    · the carrier sat ~3.2 kHz into the audio (centre 3800, AFC ≈ −600 Hz); the passband (0–7.4 kHz) cut nothing.
//      ★★ beta93 the pick puts it at ~1.5 kHz instead (vibe_sat_engine.h kTlmCentreHz) — the same frames decode.
// ★ The Viterbi, the Reed–Solomon (255,223) and the PN are vibe_lrpt_fec.h's — the same code, field, roots and
//   scrambler (LRPT is CCSDS too); only the shortening, the de-interleaver and the sync are new here.
// ★ Content: FUNcube is an AMATEUR satellite; its telemetry and fitter messages are amateur transmissions meant to be
//   received by anyone (they are the point of the mission), so they are shown — text whitelisted to printable ASCII.
#pragma once

#include "vibe_bpsk_rx.h"
#include "vibe_lrpt_fec.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace vibefc {

inline constexpr int kRows = 80, kCols = 65, kSyms = kRows * kCols;   // 5200 a frame
inline constexpr int kDataBytes = 256, kRsBytes = 320, kCoded = (kRsBytes * 8 + 6) * 2;   // 5132
inline constexpr int kPad = 95;                                          // 255 − 160

/** The 65-bit sync vector (x⁷+x³+1 from all ones) — generated, and checked against Karn's printed sequence in the test. */
inline const std::array<uint8_t, 65>& syncVector() {
    static const std::array<uint8_t, 65> v = [] {
        std::array<uint8_t, 65> s{};
        unsigned sr = 0x7F;
        for (int i = 0; i < 65; ++i) {
            s[(size_t)i] = uint8_t((sr >> 6) & 1);
            const unsigned fb = ((sr >> 6) ^ (sr >> 3)) & 1;   // x⁷ + x³ + 1 (Fibonacci, the output from the top)
            sr = ((sr << 1) | fb) & 0x7F;
        }
        return s;
    }();
    return v;
}

// ── The encoder (OUR TRANSMITTER for the tests, written from the same two documents) ─────────────────────────────────
/** 256 data bytes → 5200 channel bits (0/1) in transmission order, BEFORE the differential encoder. */
inline void encodeFrame(const uint8_t* data, uint8_t* out5200) {
    const auto& rs = vibelrpt::Rs223::get();
    uint8_t words[2][160];
    for (int w = 0; w < 2; ++w) {
        uint8_t d[223] = {}, p[32];
        for (int i = 0; i < 128; ++i) d[kPad + i] = data[2 * i + w];
        rs.encode(d, p);
        for (int i = 0; i < 128; ++i) words[w][i] = d[kPad + i];
        for (int i = 0; i < 32; ++i) words[w][128 + i] = p[i];
    }
    const uint8_t* pn = vibelrpt::pnSequence();
    std::vector<uint8_t> bits; bits.reserve(kRsBytes * 8 + 6);
    for (int i = 0; i < kRsBytes; ++i) {
        const uint8_t b = uint8_t(words[i & 1][i >> 1] ^ pn[i % 255]);
        for (int k = 7; k >= 0; --k) bits.push_back(uint8_t((b >> k) & 1));
    }
    for (int k = 0; k < 6; ++k) bits.push_back(0);   // the tail, unscrambled
    std::vector<uint8_t> coded; coded.reserve(kCoded);
    vibelrpt::convEncode(bits.data(), bits.size(), 0, coded);
    for (size_t i = 1; i < coded.size(); i += 2) coded[i] ^= 1;   // G2 inverted
    uint8_t A[kRows][kCols] = {};
    const auto& sv = syncVector();
    for (int c = 0; c < kCols; ++c) A[0][c] = sv[(size_t)c];
    for (int k = 0; k < kCoded; ++k) A[1 + k / kCols][k % kCols] = coded[(size_t)k];
    for (int c = 0; c < kCols; ++c) for (int r = 0; r < kRows; ++r) out5200[c * kRows + r] = A[r][c];
}

// ── The decoder ──────────────────────────────────────────────────────────────────────────────────────────────────
/** 5200 soft channel values in transmission order (POSITIVE = bit 1) → 256 data bytes. Returns RS bytes corrected
 *  (≥ 0), or −1. Tries both coder output orders. */
inline int decodeFrame(const int8_t* soft, uint8_t* data256, int* orderOut = nullptr) {
    std::vector<int8_t> c((size_t)kCoded);
    for (int k = 0; k < kCoded; ++k) { const int r = 1 + k / kCols, col = k % kCols; c[(size_t)k] = soft[col * kRows + r]; }
    static thread_local vibelrpt::Viterbi vit;
    const int steps = kCoded / 2;   // 2566
    std::vector<int8_t> pair((size_t)kCoded);
    std::vector<uint8_t> bits((size_t)steps);
    const auto& rs = vibelrpt::Rs223::get();
    const uint8_t* pn = vibelrpt::pnSequence();
    for (int order = 0; order < 2; ++order) {
        for (int t = 0; t < steps; ++t) {
            int8_t a = c[(size_t)(2 * t)], b = c[(size_t)(2 * t + 1)];
            if (order) std::swap(a, b);
            pair[(size_t)(2 * t)] = a;
            pair[(size_t)(2 * t + 1)] = int8_t(b == -128 ? 127 : -b);   // G2 inverted on the air
        }
        vit.decode(pair.data(), steps, 0, steps, bits.data());
        uint8_t words[2][255];
        for (int w = 0; w < 2; ++w) std::memset(words[w], 0, kPad);
        for (int i = 0; i < kRsBytes; ++i) {
            uint8_t b = 0;
            for (int k = 0; k < 8; ++k) b = uint8_t((b << 1) | (bits[(size_t)(i * 8 + k)] & 1));
            words[i & 1][kPad + (i >> 1)] = uint8_t(b ^ pn[i % 255]);
        }
        int fixed = 0; bool ok = true;
        for (int w = 0; w < 2 && ok; ++w) {
            const int e = rs.decode(words[w]);
            if (e < 0) { ok = false; break; }
            for (int i = 0; i < kPad; ++i) if (words[w][i]) { ok = false; break; }   // ★ a "correction" into the padding
            fixed += e;
        }
        if (!ok) continue;
        for (int i = 0; i < 128; ++i) { data256[2 * i] = words[0][kPad + i]; data256[2 * i + 1] = words[1][kPad + i]; }
        if (orderOut) *orderOut = order;
        return fixed;
    }
    return -1;
}

// ── The frame's meaning ─────────────────────────────────────────────────────────────────────────────────────────
// ★★★ beta94 UNITS AND JY1SAT (Stuart 11-25 on a JO-97 pass: "charts are useless what is the voltage not raw?").
//   Every channel now carries its engineering unit from AMSAT-UK's OWN published equations (the sources in the file
//   header): FUNcube-1's "telemetry_equations_b1" sheet and JY1Sat's "data budget with tlm equations 2F" sheet. A channel
//   is labelled raw only where those sheets say "use raw values" / "no conversion required" / "not translated".
//   ★ Checked on AO-73's own frames (test-fctlm §10, the 11-24 recording): battery 7.876 V, panels 14–23 °C, the
//     internal temperatures 22–37 °C, PA forward power 303 mW (the handbook's 300 mW sunlit beacon), buses 3.28 V /
//     4.96 V. ★ The sheets' "3v3 voltage = ADC count divided by 4" / "5v0 … divided by 6" read as the COUNT being the
//     millivolts ÷ 4 (÷ 6): 819 counts = 3.276 V, 826 = 4.956 V on air; the other reading (204.75, 137.7) is no voltage.
//   ★★ JY1SAT IS NOT FUNcube-1's LAYOUT. It uses the extended satellite id (ESEO Downlink Data §3): 16 header bits, then
//     JY1Sat's own 432-bit real-time block (its sheet, field by field — 14-bit voltages, 10-bit currents, three photo
//     currents, an iMTQ magnetorquer block, six sun sensors). Read as FUNcube-1 it was one byte out from the start:
//     "battery 0", "panel temps" that swung 520 counts in two minutes (they were SUN SENSORS on a spinning bird), the
//     second header byte (0x10) as a photo voltage of 4096. Its data-valid bits sit where FUNcube-1's do; the 11-25 pass
//     said EPS 0 — so no battery was ever sent, and none is shown.
//   ★★ DATA-VALID FLAGS ARE OBEYED: a board whose flag is off in a frame has its values marked !valid — left out of the
//     JSON (with "absent":["EPS"] so the box says "no EPS data in this frame"), blank in the CSV, never charted as 0.
struct Value {
    std::string key, name, unit;
    double v = 0;
    bool raw = false;       ///< no published conversion: the count as sent, labelled raw
    bool valid = true;      ///< false = its board's data-valid flag is off in this frame (not shown, not charted, blank in CSV)
    int dp = 0;             ///< the decimals a reading needs (V 3, °C 1, mA 1 …)
    std::string text;       ///< an enumerated value in words ("Watchdog reset"), else ""
    std::string note;       ///< per channel: why it is raw, or how the conversion was made
    std::string section;    ///< the board whose data-valid flag governs it ("EPS", "ASIB", "RF", "PA", "ANTS", "iMTQ"), "" = always
};
struct Decoded {
    int satId = 0, frameType = 0;
    int extId = -1;         ///< the extended satellite id (satId 3), else −1
    int rsFixed = 0;        ///< RS bytes corrected in this frame (the run fills it in)
    std::string frameName, mode;
    std::string layout;     ///< "FUNcube-1" / "JY1Sat" / "" (not known: no values)
    long long seq = -1;
    std::vector<Value> values;
    std::vector<std::string> absent;   ///< boards whose data-valid flag is off in this frame
    std::string fitter;     ///< a fitter message frame's text (printable ASCII only), else ""
    const Value* find(const char* key) const { for (const auto& v : values) if (v.key == key) return &v; return nullptr; }
};
/** MSB-first bit reader. */
struct Bits {
    const uint8_t* p; size_t pos = 0;
    explicit Bits(const uint8_t* d, size_t start = 0) : p(d), pos(start) {}
    uint32_t get(int n) { uint32_t v = 0; for (int i = 0; i < n; ++i, ++pos) v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1u); return v; }
};
/** ★★★ 11-24 ON AIR: the 6-bit frame type counts 0–23 (WO1 = 0) — AO-73 steps its sequence number on type 0, which
 *  follows type 23; the 1–24 reading named every frame one off (FM1's fitter text arrived as "FM2"). */
inline std::string frameName(int t) {
    if (t >= 0 && t <= 11) return "WO" + std::to_string(t + 1);
    static const char* rest[] = {"HR1", "FM1", "FM2", "FM3", "HR2", "FM4", "FM5", "FM6", "HR3", "FM7", "FM8", "FM9"};
    if (t >= 12 && t <= 23) return rest[t - 12];
    return "type " + std::to_string(t);
}
inline bool isFitterType(int t) { return t >= 13 && t <= 23 && t != 16 && t != 20; }
/** ★★ The extended satellite id JY1Sat sends: its second header byte 0x10 (the 11-25 JO-97 pass) = S0–S5 000100, with
 *  byte 0's S6 S7 = 11 → 0b00010011. ESEO Downlink Data §3 gives the scheme; no list of ids is published. */
inline constexpr int kExtIdJy1Sat = 0x13;
inline const char* satIdName(int id) {
    switch (id) { case 0: return "FUNcube-1 engineering model"; case 1: return "FUNcube-2 (UKube-1)"; case 2: return "FUNcube-1"; default: return "extended"; }
}
inline std::string satIdText(const struct Decoded& d) {
    if (d.satId != 3) return satIdName(d.satId);
    if (d.extId == kExtIdJy1Sat) return "JY1Sat (extended id " + std::to_string(d.extId) + ")";
    return "extended id " + std::to_string(d.extId);
}

// ── The published conversions ───────────────────────────────────────────────────────────────────────────────────
namespace cal {
/** Piecewise-linear through (x, y) points, x ascending; held flat past either end. */
inline double interp(const double (*t)[2], int n, double x) {
    if (x <= t[0][0]) return t[0][1];
    for (int i = 1; i < n; ++i)
        if (x <= t[i][0]) { const double f = (x - t[i - 1][0]) / (t[i][0] - t[i - 1][0]); return t[i - 1][1] + f * (t[i][1] - t[i - 1][1]); }
    return t[n - 1][1];
}
/** PA board temperature, °C, from its 8-bit count — the FC1 sheet's "PA TEMP lookup table" (measured 79–252, its ends
 *  continued as the sheet gives them; JY1Sat's sheet: "See PA TEMP lookup table from FC1"). */
inline double paTempC(double adc) {
    static const double t[][2] = {{0, 87.983}, {79, 55.3}, {91, 49.6}, {103, 45.3}, {115, 41.1}, {125, 37.6}, {129, 35.7},
        {137, 33.6}, {145, 30.6}, {154, 27.6}, {161, 25.1}, {169, 22.6}, {176, 20}, {183, 17.6}, {189, 15.1}, {196, 12.6},
        {203, 10}, {208, 7.5}, {214, 5}, {220, 2.4}, {224, 0}, {230, -2.9}, {233, -5}, {237, -7.5}, {241, -10},
        {244, -12.3}, {247, -15}, {252, -20}, {255, -22.846}};
    return interp(t, (int)(sizeof t / sizeof t[0]), adc);
}
/** Antenna temperature, °C — "see ISIS AntS User Manual Appendix C": the FC1 sheet's copy of that curve (sensor mV
 *  against °C, −50…150; every 5 °C here). ★ The sheets do not say how the 8-bit count maps to mV: taken as the AntS
 *  ADC's 0–3.3 V full scale (count × 3300 / 255). On AO-73 it reads 24–25 °C beside the EPS's 22 °C. */
inline double antTempC(double adc) {
    static const double t[][2] = {{420, 150}, {479, 145}, {537, 140}, {595, 135}, {653, 130}, {711, 125}, {769, 120},
        {826, 115}, {883, 110}, {940, 105}, {997, 100}, {1054, 95}, {1110, 90}, {1167, 85}, {1223, 80}, {1279, 75},
        {1335, 70}, {1391, 65}, {1448, 60}, {1503, 55}, {1558, 50}, {1613, 45}, {1668, 40}, {1723, 35}, {1777, 30},
        {1831, 25}, {1885, 20}, {1939, 15}, {1993, 10}, {2047, 5}, {2100, 0}, {2154, -5}, {2207, -10}, {2260, -15},
        {2313, -20}, {2366, -25}, {2418, -30}, {2470, -35}, {2522, -40}, {2571, -45}, {2616, -50}};
    return interp(t, (int)(sizeof t / sizeof t[0]), adc * 3300.0 / 255.0);
}
inline constexpr double kSunMaxAdc = 984;   ///< the sheets' table ends in four rows at 984: the sensor's limit
/** Sun sensor, lux — the sheets' "Raw ADC reading to log (Lux)" table (JY1Sat's copy: FC1's has 3.980 at 400, out of
 *  order between 3.475 and 3.690; JY1Sat's corrects it to 3.58). 5 counts and below = noise, "0". */
inline double sunLux(double adc) {
    static const double t[][2] = {{9, 2.158}, {19, 2.477}, {23, 2.640}, {33, 2.80}, {50, 2.881}, {56, 2.889}, {100, 3.04},
        {133, 3.14}, {200, 3.25}, {265, 3.346}, {333, 3.475}, {400, 3.58}, {467, 3.69}, {533, 3.81}, {600, 3.92},
        {666, 4.03}, {700, 4.079}, {732, 4.13}, {800, 4.245}, {866, 4.325}, {933, 4.38}, {984, 4.42}};
    if (adc <= 5) return 0;
    if (adc < 9) return std::pow(10.0, 2.158) * (adc - 5) / 4.0;   // between the noise floor and the table's first row
    return std::pow(10.0, interp(t, (int)(sizeof t / sizeof t[0]), adc));
}
/** PA forward / reverse power, mW: 0.005 × count^2.0629 (both sheets). */
inline double paPowerMw(double adc) { return 0.005 * std::pow(adc, 2.0629); }
/** Two's complement 8-bit (the EPS's temperatures; JY1Sat's sheet: "int16 to int8 conversion (as batt temp)"). */
inline double s8(uint32_t v) { return (double)(int8_t)(uint8_t)v; }
inline const char* resetCause(int v) {
    static const char* n[] = {"Power-on reset", "External reset", "Brown-out reset", "Watchdog reset", "JTAG reset", "Other"};
    return v >= 0 && v <= 5 ? n[v] : "";
}
inline const char* pptMode(int v) {
    static const char* n[] = {"Hardware", "MPPT", "Fixed software PPT"};
    return v >= 0 && v <= 2 ? n[v] : "";
}
inline const char* imtqMode(int v) {
    static const char* n[] = {"Idle", "Self-test", "Detumbling"};
    return v >= 0 && v <= 2 ? n[v] : "Invalid";
}
} // namespace cal

/** Fills Value rows from a bit reader: one helper per kind of channel, so each layout below reads like its sheet. */
struct Reader {
    Bits b; Decoded& o; std::string sec;
    Reader(const uint8_t* d, size_t start, Decoded& out) : b(d, start), o(out) {}
    Value& add(const char* k, const char* n, double v, const char* unit, int dp) {
        Value x; x.key = k; x.name = n; x.v = v; x.unit = unit; x.dp = dp; x.section = sec; o.values.push_back(x); return o.values.back();
    }
    // "real values are transmitted": millivolts → V, milliamps, °C as signed bytes
    void mV(const char* k, const char* n, int w) { add(k, n, b.get(w) / 1000.0, "V", 3); }
    void mA(const char* k, const char* n, int w) { add(k, n, b.get(w), "mA", 0); }
    void degS8(const char* k, const char* n) { add(k, n, cal::s8(b.get(8)), "\xc2\xb0" "C", 0); }
    void count(const char* k, const char* n, int w) { add(k, n, b.get(w), "", 0); }
    void scaled(const char* k, const char* n, int w, double m, double c, const char* unit, int dp) { add(k, n, m * b.get(w) + c, unit, dp); }
    void rawCount(const char* k, const char* n, int w, const char* why) { Value& v = add(k, n, b.get(w), "", 0); v.raw = true; v.note = why; }
    void sun(const char* k, const char* n) {
        const double a = b.get(10);
        Value& v = add(k, n, std::round(cal::sunLux(a)), "lux", 0);
        if (a >= cal::kSunMaxAdc) v.note = "at the sensor's limit (984 counts = 26 klux or more)";
    }
    void en(const char* k, const char* n, int w, const char* (*words)(int)) { const uint32_t x = b.get(w); Value& v = add(k, n, x, "", 0); v.text = words((int)x); }
};

/** ★★ FUNcube-1 (AO-73): the Downlink Data document's real-time block, 440 bits from bit 8, with the FC1 sheet's units. */
inline void parseFc1(const uint8_t* d, Decoded& o) {
    Reader r(d, 8, o);
    r.sec = "EPS";   // 192 — "real values are transmitted": mV, mA, °C
    r.mV("eps.pv1", "Photo voltage 1", 16); r.mV("eps.pv2", "Photo voltage 2", 16); r.mV("eps.pv3", "Photo voltage 3", 16);
    r.mA("eps.ipv", "Total photo current", 16); r.mV("eps.vbat", "Battery voltage", 16); r.mA("eps.isys", "Total system current", 16);
    r.count("eps.reboots", "Reboot count", 16); r.count("eps.swerr", "EPS software errors", 16);
    r.degS8("eps.tboost1", "Boost converter temp 1"); r.degS8("eps.tboost2", "Boost converter temp 2"); r.degS8("eps.tboost3", "Boost converter temp 3");
    r.degS8("eps.tbat", "Battery temp"); r.count("eps.lu5", "Latch-up count 5 V", 8); r.count("eps.lu33", "Latch-up count 3.3 V", 8);
    r.en("eps.reset", "Reset cause", 8, cal::resetCause); r.en("eps.ppt", "Power point tracking mode", 8, cal::pptMode);
    r.sec = "ASIB";  // 100 (the document's "BOB")
    r.sun("bob.sunx", "Sun sensor X+"); r.sun("bob.suny", "Sun sensor Y+"); r.sun("bob.sunz", "Sun sensor Z+");
    r.scaled("bob.tpxp", "Solar panel temp X+", 10, -0.2073, 158.239, "\xc2\xb0" "C", 1);
    r.scaled("bob.tpxm", "Solar panel temp X-", 10, -0.2083, 159.227, "\xc2\xb0" "C", 1);
    r.scaled("bob.tpyp", "Solar panel temp Y+", 10, -0.2076, 158.656, "\xc2\xb0" "C", 1);
    r.scaled("bob.tpym", "Solar panel temp Y-", 10, -0.2087, 159.045, "\xc2\xb0" "C", 1);
    r.scaled("bob.v33", "3.3 V bus voltage", 10, 0.004, 0, "V", 3); r.mA("bob.i33", "3.3 V bus current", 10);
    r.scaled("bob.v50", "5.0 V bus voltage", 10, 0.006, 0, "V", 3);
    r.sec = "RF";    // 48
    r.rawCount("rf.doppler", "Receiver Doppler", 8, "uncalibrated in the published sheet (about 163 = no signal)");
    r.rawCount("rf.rssi", "Receiver RSSI", 8, "uncalibrated in the published sheet (\"use raw values\")");
    r.scaled("rf.temp", "RF temperature", 8, -0.857, 193.672, "\xc2\xb0" "C", 1);
    r.scaled("rf.irx", "Receive current", 8, 0.636, 0, "mA", 1); r.scaled("rf.itx33", "Transmit current 3.3 V bus", 8, 0.636, 0, "mA", 1);
    r.scaled("rf.itx50", "Transmit current 5.0 V bus", 8, 1.272, 0, "mA", 1);
    r.sec = "PA";    // 32 (as flown: reverse then forward)
    r.add("pa.rev", "PA reverse power", cal::paPowerMw(r.b.get(8)), "mW", 1); r.add("pa.fwd", "PA forward power", cal::paPowerMw(r.b.get(8)), "mW", 1);
    r.add("pa.temp", "PA board temperature", cal::paTempC(r.b.get(8)), "\xc2\xb0" "C", 1);
    r.scaled("pa.i", "PA board current", 8, 0.5496, 2.5435, "mA", 1);
    r.sec = "ANTS";  // 20
    r.add("ant.t0", "Antenna temp 0", cal::antTempC(r.b.get(8)), "\xc2\xb0" "C", 1).note = "the count taken as 0-3.3 V (not stated)";
    r.add("ant.t1", "Antenna temp 1", cal::antTempC(r.b.get(8)), "\xc2\xb0" "C", 1).note = "the count taken as 0-3.3 V (not stated)";
    r.count("ant.dep0", "Antenna 0 deployed", 1); r.count("ant.dep1", "Antenna 1 deployed", 1);
    r.count("ant.dep2", "Antenna 2 deployed", 1); r.count("ant.dep3", "Antenna 3 deployed", 1);
}
/** ★★ JY1Sat (JO-97): its own sheet's real-time block, 432 bits from bit 16 (the extended header). Widths, order and
 *  conversions as the sheet gives them; "Treat as FC1" = FUNcube-1's conversion. */
inline void parseJy1(const uint8_t* d, Decoded& o) {
    Reader r(d, 16, o);
    r.sec = "EPS";   // 170 — 14-bit mV ("Delete 2 MSB"), 10-bit mA ("Delete 4 MSB")
    r.mV("eps.pv1", "Photo voltage 1", 14); r.mV("eps.pv2", "Photo voltage 2", 14); r.mV("eps.pv3", "Photo voltage 3", 14);
    r.mV("eps.vbat", "Battery voltage", 14);
    r.mA("eps.ipv1", "Photo current 1", 10); r.mA("eps.ipv2", "Photo current 2", 10); r.mA("eps.ipv3", "Photo current 3", 10);
    r.mA("eps.ipv", "Total photo current", 10); r.mA("eps.isys", "Total system current", 10);
    r.count("eps.reboots", "Reboot count", 8);
    r.degS8("eps.tboost1", "Boost converter temp 1"); r.degS8("eps.tboost2", "Boost converter temp 2"); r.degS8("eps.tboost3", "Boost converter temp 3");
    r.degS8("eps.tbat", "Battery temp"); r.count("eps.lu5", "Latch-up count 5 V", 8);
    r.mA("eps.i5sw1", "5 V switch 1 current", 8);
    r.en("eps.reset", "Reset cause", 4, cal::resetCause); r.en("eps.ppt", "Power point tracking mode", 4, cal::pptMode);
    r.sec = "iMTQ";  // 14 — the magnetorquer
    r.en("imtq.mode", "iMTQ mode", 2, cal::imtqMode);
    r.rawCount("imtq.err", "iMTQ error code", 3, "\"not translated\" in the published sheet");
    r.count("imtq.cfg", "iMTQ configuration set", 1);
    {   const uint32_t t = r.b.get(8);
        Value& v = r.add("imtq.temp", "iMTQ MCU temperature", cal::s8(t), "\xc2\xb0" "C", 0);
        if (t == 85) { v.valid = false; v.note = "85 = invalid (the sheet)"; } }
    r.sec = "ASIB";  // 100 — six sun sensors, the 3.3 V bus, the iMTQ's uptime, the 5 V bus
    r.sun("asib.sunxp", "Sun sensor X+"); r.sun("asib.sunxm", "Sun sensor X-"); r.sun("asib.sunyp", "Sun sensor Y+");
    r.sun("asib.sunym", "Sun sensor Y-"); r.sun("asib.sunzp", "Sun sensor Z+"); r.sun("asib.sunzm", "Sun sensor Z-");
    r.scaled("bob.v33", "3.3 V bus voltage", 10, 0.004, 0, "V", 3);
    r.sec = "iMTQ";
    r.scaled("imtq.up", "iMTQ uptime", 20, 5.0 / 3600.0, 0, "h", 1);   // "20 bit count of 5 sec period"
    r.sec = "ASIB";
    r.scaled("bob.v50", "5.0 V bus voltage", 10, 0.006, 0, "V", 3);
    r.sec = "RF";    // 48
    r.rawCount("rf.doppler", "Receiver Doppler", 8, "\"no conversion required\" in the published sheet");
    r.rawCount("rf.rssi", "Receiver RSSI", 8, "\"no conversion required\" in the published sheet");
    r.scaled("rf.temp", "RF temperature", 8, -0.98, 234.58, "\xc2\xb0" "C", 1);
    r.scaled("rf.irx", "Receive current", 8, 0.636, 0, "mA", 1); r.scaled("rf.itx33", "Transmit current 3.3 V bus", 8, 0.636, 0, "mA", 1);
    r.scaled("rf.itx50", "Transmit current 5.0 V bus", 8, 0.636, 0, "mA", 1);
    r.sec = "PA";    // 32
    r.add("pa.rev", "PA reverse power", cal::paPowerMw(r.b.get(8)), "mW", 1); r.add("pa.fwd", "PA forward power", cal::paPowerMw(r.b.get(8)), "mW", 1);
    r.add("pa.temp", "PA board temperature", cal::paTempC(r.b.get(8)), "\xc2\xb0" "C", 1);
    r.scaled("pa.i", "PA board current", 8, 0.5496, 2.5435, "mA", 1);
    r.sec = "ANTS";  // 20
    r.add("ant.t0", "Antenna temp 0", cal::antTempC(r.b.get(8)), "\xc2\xb0" "C", 1).note = "the count taken as 0-3.3 V (not stated)";
    r.add("ant.t1", "Antenna temp 1", cal::antTempC(r.b.get(8)), "\xc2\xb0" "C", 1).note = "the count taken as 0-3.3 V (not stated)";
    r.count("ant.dep0", "Antenna 0 deployed", 1); r.count("ant.dep1", "Antenna 1 deployed", 1);
    r.count("ant.dep2", "Antenna 2 deployed", 1); r.count("ant.dep3", "Antenna 3 deployed", 1);
}

/** ★★ The frame: its header (plain or extended id), the satellite's layout, the software block (the same 48 bits at the
 *  same place — bit 400 — in both layouts) and the data-valid flags applied. `satHint` = the pick's catalogue name: an
 *  extended id we do not know yet, heard on the JO-97 pick, is read as JY1Sat. */
inline Decoded parse(const uint8_t* d, const std::string& satHint = std::string()) {
    Decoded o;
    o.satId = d[0] >> 6; o.frameType = d[0] & 0x3F;
    if (o.satId == 3) {   // ESEO Downlink Data §3: byte 1 = S0–S5 F0 F1; the id's low bits are byte 0's two
        o.extId = ((d[1] >> 2) << 2) | 3;
        o.frameType |= (d[1] & 3) << 6;
    }
    o.frameName = frameName(o.frameType);
    const bool jyHint = satHint.find("JO-97") != std::string::npos || satHint.find("JY1") != std::string::npos;
    if (o.satId == 3 && (o.extId == kExtIdJy1Sat || jyHint)) { o.layout = "JY1Sat"; parseJy1(d, o); }
    else if (o.satId != 3) { o.layout = "FUNcube-1"; parseFc1(d, o); }
    // the software block (48 bits) ends both real-time blocks: FUNcube-1 8 + 392, JY1Sat 16 + 384
    Reader r(d, 400, o);
    const uint32_t seq = r.b.get(24);
    if (!o.layout.empty()) {
        r.count("sw.seq", "Sequence number", 0); o.values.back().v = seq;
        o.seq = seq;
        r.count("sw.dtmfn", "DTMF command count", 6); r.count("sw.dtmflast", "DTMF last command", 5); r.count("sw.dtmfok", "DTMF command success", 1);
        const bool jy = o.layout == "JY1Sat";
        r.count("sw.vasib", "Data valid ASIB", 1); r.count("sw.veps", "Data valid EPS", 1); r.count("sw.vpa", "Data valid PA", 1);
        r.count("sw.vrf", "Data valid RF", 1); r.count(jy ? "sw.vimtq" : "sw.vmse", jy ? "Data valid iMTQ" : "Data valid MSE", 1);
        r.count("sw.vantb", "Data valid ANTS bus B", 1); r.count("sw.vanta", "Data valid ANTS bus A", 1);
        const uint32_t eclipse = r.b.get(1), safe = r.b.get(1);
        r.add("sw.eclipse", "In eclipse mode", eclipse, "", 0); r.add("sw.safe", "In safe mode", safe, "", 0);
        r.count("sw.habf", "Hardware ABF on", 1); r.count("sw.sabf", "Software ABF on", 1); r.count("sw.depwait", "Deployment wait at next boot", 1);
        o.mode = safe ? "Safe" : eclipse ? "Eclipse" : "Sunlight";
        // ★★ the flags, applied: a board that says its data is not valid has none shown
        auto flag = [&](const char* k) { const Value* v = o.find(k); return v && v->v != 0; };
        struct F { const char* sec; bool ok; };
        const F fl[] = {{"ASIB", flag("sw.vasib")}, {"EPS", flag("sw.veps")}, {"PA", flag("sw.vpa")}, {"RF", flag("sw.vrf")},
                        {"iMTQ", jy && flag("sw.vimtq")}, {"ANTS", flag("sw.vanta") || flag("sw.vantb")}};
        for (const F& f : fl) {
            if (f.ok || (!jy && std::string(f.sec) == "iMTQ")) continue;
            o.absent.push_back(f.sec);
            for (auto& v : o.values) if (v.section == f.sec) v.valid = false;
        }
    }
    // ★ a fitter message frame: its 200 payload bytes are the message, copied verbatim on board, NUL-terminated.
    //   ★★ 11-24 ON AIR: an empty slot is sent as ".." and FM9 (type 23) carries binary — so a message is shown only
    //   when it is text (≥ 90 % printable) and says something (not only dots and spaces).
    if (isFitterType(o.frameType)) {
        std::string t; int bad = 0;
        for (int i = 56; i < 256; ++i) {
            const unsigned char ch = d[i];
            if (ch == 0) break;
            if (ch >= 0x20 && ch < 0x7F) t += char(ch);
            else if (ch == '\n' || ch == '\r') t += ' ';
            else { t += '.'; ++bad; }
        }
        while (!t.empty() && t.back() == ' ') t.pop_back();
        const bool text = !t.empty() && bad * 10 <= (int)t.size();
        const bool says = t.find_first_not_of(". ") != std::string::npos;
        if (text && says) o.fitter = t;
    }
    return o;
}

inline std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c >= 0x20 && c < 0x7F) o += char(c);
    }
    return o;
}
/** UTF-8 kept (the units' "°"), quotes / backslashes escaped, controls dropped. */
inline std::string escU(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
        else if (c >= 0x20 && c != 0x7F) o += char(c);
    }
    return o;
}
inline std::string num(double v) {
    char b[32];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof b, "%.0f", v); else std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
/** A reading to `dp` decimals (−0 printed as 0). */
inline std::string num(double v, int dp) {
    if (dp <= 0) return num(std::round(v) == 0 ? 0.0 : std::round(v));
    char b[32]; std::snprintf(b, sizeof b, "%.*f", dp > 4 ? 4 : dp, v);
    std::string s = b;
    if (s[0] == '-' && s.find_first_not_of("-0.") == std::string::npos) s.erase(0, 1);
    return s;
}
/** The day log's / the gallery line's battery: "battery 7.876 V", or "no EPS data". */
inline std::string batteryText(const Decoded& d) {
    const Value* b = d.find("eps.vbat");
    if (!b) return "";
    if (!b->valid) return "no EPS data";
    return "battery " + num(b->v, b->dp) + " V";
}
/** `{"type":"sat_tlm","data":{…}}` — the contract shape (docs/v12/SATELLITE-BEST.md "Contract additions (beta90)").
 *  ★ beta94: values in their units ("unit"), "raw":true only with no published conversion (+ "note" saying so), an
 *  enumerated value's words in "text"; a board whose data-valid flag is off is left out and named in "absent". */
inline std::string tlmJson(const Decoded& d, const uint8_t* bytes, size_t n, const std::string& sat, double unixNow) {
    std::string j = "{\"type\":\"sat_tlm\",\"data\":{\"sat\":\"" + esc(sat) + "\",\"format\":\"funcube-rt\",\"t\":" + num(std::floor(unixNow))
                  + ",\"frame\":\"" + esc(d.frameName) + "\",\"satId\":\"" + esc(satIdText(d)) + "\"";
    if (!d.layout.empty()) j += ",\"layout\":\"" + esc(d.layout) + "\"";
    if (d.seq >= 0) j += ",\"seq\":" + std::to_string(d.seq);
    if (!d.mode.empty()) j += ",\"mode\":\"" + d.mode + "\"";
    if (!d.fitter.empty()) j += ",\"fitter\":\"" + esc(d.fitter) + "\"";
    j += ",\"absent\":[";
    for (size_t i = 0; i < d.absent.size(); ++i) j += (i ? ",\"" : "\"") + esc(d.absent[i]) + "\"";
    j += "],\"values\":[";
    int k = 0;
    for (size_t i = 0; i < d.values.size() && k < 96; ++i) {   // ★ beta94 JY1Sat sends 63, FUNcube-1 58 — room above both
        const Value& v = d.values[i];
        if (!v.valid) continue;
        if (k++) j += ",";
        j += "{\"k\":\"" + v.key + "\",\"name\":\"" + esc(v.name) + "\",\"v\":" + num(v.v, v.dp);
        if (!v.unit.empty()) j += ",\"unit\":\"" + escU(v.unit) + "\"";
        if (v.raw) j += ",\"raw\":true";
        if (!v.text.empty()) j += ",\"text\":\"" + esc(v.text) + "\"";
        if (!v.note.empty()) j += ",\"note\":\"" + esc(v.note) + "\"";
        j += "}";
    }
    j += "],\"hex\":\"";
    static const char* hx = "0123456789abcdef";
    for (size_t i = 0; i < n && i < 256; ++i) { j += hx[bytes[i] >> 4]; j += hx[bytes[i] & 15]; }
    return j + "\"}}";
}

// ── The framer: soft symbols in, frames out ─────────────────────────────────────────────────────────────────────
/** ★★ Watches the symbol stream for the sync vector (one bit every 80 symbols) in four forms — coherent / differential
 *  × either sign — and decodes the 5200 symbols behind a match. A match needs ≥ kSyncMin of 65 bits; on noise a chance
 *  match is ~1e-9 a symbol, and RS + the zero padding then refuse it. */
class Framer {
public:
    static constexpr int kSyncMin = 54;
    std::function<void(const uint8_t* data256, int rsFixed, int form)> onFrame;
    std::function<void()> onFail;   ///< a sync matched, the frame did not decode

    void push(float s) {
        const float prev = last_;
        last_ = s;
        put(coh_, s);
        put(dif_, -(s * prev));   // ★ a phase CHANGE = 1 (the other convention is the inverted form, tried too)
        ++n_;
        if (n_ < (uint64_t)kSyms || hold_ > 0) { if (hold_ > 0) --hold_; return; }
        tryAt(coh_, 0);
        if (hold_ == 0) tryAt(dif_, 2);
    }
    uint64_t syncs() const { return syncs_; }
    uint64_t good() const { return good_; }
    uint64_t failed() const { return failed_; }
    /** Symbols since the last good frame (a lock indicator for the demodulator's hunt). */
    uint64_t sinceGood() const { return n_ - lastGood_; }

private:
    static constexpr size_t kRing = 8192;   // > 5200, a power of two
    struct R { std::vector<float> v = std::vector<float>(kRing, 0.f); };
    void put(R& r, float s) { r.v[(size_t)(n_ & (kRing - 1))] = s; }
    float at(const R& r, uint64_t i) const { return r.v[(size_t)(i & (kRing - 1))]; }
    void tryAt(const R& r, int form) {
        const uint64_t start = n_ - (uint64_t)kSyms;   // the newest symbol is n_ − 1: a whole frame ends there
        const auto& sv = syncVector();
        int match = 0;
        for (int j = 0; j < 65; ++j) match += ((at(r, start + (uint64_t)(80 * j)) > 0) == (sv[(size_t)j] != 0));
        int sign = 0;
        if (match >= kSyncMin) sign = 1; else if (65 - match >= kSyncMin) sign = -1;
        if (!sign) return;
        ++syncs_;
        // soft int8: scaled by the frame's own mean magnitude (≈ 40 per unit), sign-corrected
        double m = 0;
        for (int i = 0; i < kSyms; ++i) m += std::fabs(at(r, start + (uint64_t)i));
        m = m / kSyms;
        const double g = m > 1e-9 ? 40.0 / m : 0.0;
        int8_t soft[kSyms];
        for (int i = 0; i < kSyms; ++i) {
            const double v = sign * at(r, start + (uint64_t)i) * g;
            soft[i] = int8_t(std::max(-127.0, std::min(127.0, std::round(v))));
        }
        uint8_t data[kDataBytes];
        const int e = decodeFrame(soft, data);
        if (e < 0) { ++failed_; hold_ = 0; if (onFail) onFail(); return; }
        ++good_; lastGood_ = n_;
        hold_ = kSyms - 200;   // ★ the next frame starts ≥ 5200 symbols on (frames are 5 s apart)
        if (onFrame) onFrame(data, e, form + (sign < 0 ? 1 : 0));
    }
    R coh_, dif_;
    float last_ = 0;
    uint64_t n_ = 0, syncs_ = 0, good_ = 0, failed_ = 0, lastGood_ = 0;
    int hold_ = 0;
};

// ── ★★ V12 beta91: where and when a frame was heard (the gallery's per-pass log) ────────────────────────────────
struct TlmCtx {
    std::string sat;          ///< "AO-73"
    double unix = 0;          ///< the frame's time (the server's corrected clock), seconds
    double aos = 0, los = 0;  ///< the pass (the satellite engine's), 0 = not known (a hand-tuned listener)
    double elDeg = NAN;       ///< the bird's elevation now, NaN = not known
};

// ── ★★★ beta92 DRIFT FOLLOW: the locked decoder steers the satellite lock's trim ─────────────────────────────────────
/** Stuart 11-23: "a wider usb passband and audio correction too that way it compensates for transmitter drift too".
 *  Once frames decode, the decoder knows where the signal really is (its AFC: afcHz() from the centre); the lock's trim
 *  (vibe::sat::Engine::setDecoderTrim) moves the CHANNEL by that much, so the signal goes back to the centre and the
 *  search keeps its whole ±range for whatever comes next (drift, an old element set, a pass's far end).
 *  ★★ THE HAND-OFF — when the trim moves the channel by X, the decoder sees its signal move by −X. They must never
 *     chase each other, so this asks in DISCRETE MOVES, never continuously:
 *      · only while locked (good frames in the last 10 s);
 *      · never while a move is under way (the trim not yet at its target) — the engine slews it at 50 Hz/s and the
 *        Costas loop follows that ramp (the first NCO takes each 20 Hz from it, phase-continuous);
 *      · after a move has arrived, kSettleSec, then a fresh window of kWindowSec of estimates that agree within
 *        kSteadyHz — an estimate taken while the channel moved (or before the audio carrying the move reached the
 *        decoder: the pipeline's latency is unknown) is never used;
 *      · only when the steady offset is over kStartHz (the hysteresis — inside it nothing moves at all);
 *      · the move is the whole measured offset (gain 1): the next one starts again from a measurement, so there is no
 *        integrator to wind up and nothing to ring.
 *  A slow drift (300 Hz a minute) is thus followed in steps of ~kStartHz; a fixed error (1.3 kHz) in one move. */
struct TrimFollow {
    static constexpr double kStartHz = 120.0;   ///< the hysteresis: closer to the centre than this, no move
    static constexpr double kSteadyHz = 40.0;   ///< the window's estimates agree within this (max − min)
    static constexpr double kWindowSec = 3.0;   ///< steady for this long (≥ 3 estimates, one a second)
    static constexpr double kSettleSec = 2.0;   ///< after a move arrives, before estimates count again
    /** One estimate (once a second): `t` the audio clock (s), `locked` frames decoding, `offHz` the decoder's offset
     *  from its centre, `applied` / `target` the lock's trim (`ok` false = no lock that takes one). Returns the new
     *  trim target to ask for, or NaN = nothing to do. */
    double step(double t, bool locked, double offHz, bool ok, double applied, double target) {
        if (!ok || !locked || !std::isfinite(offHz)) { win_.clear(); return NAN; }
        if (std::fabs(applied - target) > 1.0) { win_.clear(); arrived_ = -1; return NAN; }   // a move under way
        if (arrived_ < 0) arrived_ = t;
        if (t - arrived_ < kSettleSec) { win_.clear(); return NAN; }
        win_.push_back({t, offHz});
        while (!win_.empty() && t - win_.front().first > kWindowSec + 0.01) win_.pop_front();
        if (win_.size() < 3 || t - win_.front().first < kWindowSec - 1.01) return NAN;
        double lo = 1e9, hi = -1e9, sum = 0;
        for (auto& e : win_) { lo = std::min(lo, e.second); hi = std::max(hi, e.second); sum += e.second; }
        if (hi - lo > kSteadyHz) return NAN;
        const double mean = sum / (double)win_.size();
        if (std::fabs(mean) < kStartHz) return NAN;
        win_.clear(); arrived_ = -1;
        ++moves_;
        return applied + mean;
    }
    int moves() const { return moves_; }
private:
    std::deque<std::pair<double, double>> win_;
    double arrived_ = -1;
    int moves_ = 0;
};

// ── The run inside a decoder host ───────────────────────────────────────────────────────────────────────────────
/** ★ 48 kHz listener audio in (USB, the signal near `centre` Hz); `sat_tlm` per good frame, `fctlm_status` once a
 *  second, a readable gallery line per frame. All on the host's decode thread (never the audio thread). */
class Run {
public:
    struct Out {
        std::function<void(const std::string&)> json;   ///< a JSON text frame to the decoder socket's peers
        std::function<void(const std::string&)> line;   ///< a readable line (the gallery's day log)
        std::function<std::string()> satName;           ///< the pick's satellite ("" = AO-73)
        std::function<double()> unixNow;                ///< the server's corrected clock
        /** ★★ V12 beta91 TELEMETRY CHARTS: every good frame, parsed, for the gallery's per-pass CSV and day log
         *  (vibe_fctlm_gallery.h). Optional. */
        std::function<void(const Decoded&, const TlmCtx&)> record;
        /** ★ beta91: the pass the frame belongs to (the satellite engine's AOS / LOS and elevation now) — 0 = not known. */
        std::function<void(TlmCtx&)> pass;
        /** ★★★ beta92 DRIFT FOLLOW (TrimFollow): the satellite lock's trim now (false = no lock that takes a decoder's
         *  trim — a hand-started decoder, a voice downlink), and the move to ask for. Both optional: without them the
         *  decoder only listens. */
        std::function<bool(double& applied, double& target)> trimNow;
        std::function<void(double target)> trimTo;
        /** A line for the server's log (the follow's moves). Optional. */
        std::function<void(const std::string&)> log;
    };
    Run(const vibebpsk::Rx::Cfg& cfg, bool invert, Out o) : rx_(cfg), inv_(invert), o_(std::move(o)) {
        rx_.onSymbol = [this](float s) { fr_.push(inv_ ? -s : s); };
        fr_.onFrame = [this](const uint8_t* d, int fixed, int form) { frame(d, fixed, form); };
    }
    void feedAudio(const float* x, int n) {
        rx_.feed(x, n);
        // ★ ten seconds without a frame: hunt again. ★★ AND NO FRAME YET = hunting: sinceGood() counts from 0 before the
        //   first frame, so without good() > 0 a fresh run thought itself locked for its first 10 s and never found a
        //   signal more than the Costas loop's ~60 Hz off (the benchmark row decoded 0; 20 ms blocks lost frames).
        rx_.setLocked(fr_.good() > 0 && fr_.sinceGood() < 12000);
        if (rx_.samples() - lastStatus_ >= 48000) { lastStatus_ = rx_.samples(); follow(); status(); }
    }
    const TrimFollow& follower() const { return follow_; }
    std::string statusJson() const {
        char b[320];
        std::snprintf(b, sizeof b, "{\"type\":\"fctlm_status\",\"data\":{\"sync\":%s,\"frames\":%llu,\"good\":%llu,\"failed\":%llu,"
                      "\"afcHz\":%.1f,\"snrDb\":%.1f,\"centreHz\":%.0f,\"auto\":%s}}",
                      fr_.sinceGood() < 12000 && fr_.good() > 0 ? "true" : "false", (unsigned long long)fr_.syncs(),
                      (unsigned long long)fr_.good(), (unsigned long long)fr_.failed(), rx_.afcHz(), rx_.snrDb(), rx_.centreHz(),
                      rx_.afcOn() ? "true" : "false");
        return b;
    }
    const Framer& framer() const { return fr_; }
    const vibebpsk::Rx& rx() const { return rx_; }
    vibebpsk::Rx& rxMut() { return rx_; }
private:
    void status() { if (o_.json) o_.json(statusJson()); }
    /** ★★★ beta92 once a second: the locked decoder's offset → the lock's trim (TrimFollow says when and how far). */
    void follow() {
        if (!o_.trimNow || !o_.trimTo) return;
        double applied = 0, target = 0;
        const bool ok = o_.trimNow(applied, target);
        const bool locked = fr_.good() > 0 && fr_.sinceGood() < 12000;
        const double t = (double)rx_.samples() / 48000.0;
        const double want = follow_.step(t, locked, rx_.afcHz(), ok, applied, target);
        if (!std::isfinite(want)) return;
        o_.trimTo(want);
        // ★ the log says where the satellite is against the prediction — once, then when that moves by 250 Hz or more
        if (o_.log && (!std::isfinite(logged_) || std::fabs(want - logged_) >= 250.0)) {
            logged_ = want;
            std::string sat = o_.satName ? o_.satName() : std::string();
            if (sat.empty()) sat = "AO-73";
            char b[200];
            std::snprintf(b, sizeof b, "satellite pass: %s telemetry is %.1f kHz %s the prediction \xe2\x80\x94 following it (trim %+.0f Hz)",
                          sat.c_str(), std::fabs(want) / 1000.0, want >= 0 ? "above" : "below", want);
            o_.log(b);
        }
    }
    void frame(const uint8_t* d, int fixed, int form) {
        std::string sat = o_.satName ? o_.satName() : std::string();
        if (sat.empty()) sat = "AO-73";
        const Decoded p = parse(d, sat);   // ★ beta94 the pick's name picks the layout of an extended id not yet known
        const double now = o_.unixNow ? o_.unixNow() : 0.0;
        if (o_.json) o_.json(tlmJson(p, d, kDataBytes, sat, now));
        if (o_.record) {
            TlmCtx c; c.sat = sat; c.unix = now;
            if (o_.pass) o_.pass(c);
            Decoded q = p; q.rsFixed = fixed;
            o_.record(q, c);
        }
        if (o_.line) {
            std::string l = sat + " telemetry " + p.frameName + " \xc2\xb7 seq " + std::to_string(p.seq) + " \xc2\xb7 " + p.mode
                          + (batteryText(p).empty() ? std::string() : " \xc2\xb7 " + batteryText(p)) + " \xc2\xb7 RS fixed " + std::to_string(fixed);
            if (!p.fitter.empty()) l += " \xc2\xb7 \"" + p.fitter.substr(0, 200) + "\"";
            o_.line(l);
        }
        (void)form;
        status();
    }
    vibebpsk::Rx rx_;
    Framer fr_;
    bool inv_;
    Out o_;
    uint64_t lastStatus_ = 0;
    TrimFollow follow_;
    double logged_ = NAN;
};

} // namespace vibefc
