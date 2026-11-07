// adv_rds_core.h — ADVANCED RDS's readings, the app's AdvRdsPanel.tsx ported (no GL, no SDL: test/adv_rds_test.cpp
// runs it on a desktop). One `rdsx` frame (≈ 6 a second while somebody has the panel open) plus the basic `rds`
// message in, the panel's rows, verdicts and plot data out.
//
// ★★★ NO DSP HERE, AS IN THE APP. Every number, the constellation, the MPX curve and the eye grids are computed by the
//     engine beside the decoder (local_sdr_shim.cpp, the rdsx writer) and only INTERPRETED here. A value derived here
//     would belong in rds.cpp instead.
// ★★ THE THRESHOLDS AND WORDINGS ARE THE APP'S (src/components/AdvRdsPanel.tsx), which mirrors web/client/src/main.ts
//    renderRds() — set against a Pira analyser on real stations and deliberately WIDE in places so a steady
//    measurement does not flap. Change one reader and you change three: keep them in step.
// ★ The app's parse defaults (src/services/VibeServerWsClient.ts, the rdsx case) are reproduced in `num()` calls below,
//   so a field an older engine leaves out reads as "nothing to report", never as alarming news.
// ★ Substitutions for glyphs Atkinson Hyperlegible does not have (checked against its cmap): U+21D4 ⇔ → "/",
//   U+2713 ✓ → "*" (with the key said in the row), U+2192 → → "before … · after …". U+2212 − and U+00B0 ° it has.
#pragma once
#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hh {
namespace ardr {

/** A reading's MEANING colour (decoderTokens.ts DECODER_MEANING) — never a decoration. */
enum Tone { T_VALUE, T_GOOD, T_WARN, T_BAD, T_MUTED };

inline const char* const kDash = "\xE2\x80\x94";   // —
inline const char* const kMinus = "\xE2\x88\x92";  // −
inline const char* const kDeg = "\xC2\xB0";        // °

inline const char* ptyName(int v) {
    static const char* t[] = {"None", "News", "Current Affairs", "Information", "Sport", "Education", "Drama", "Culture",
                              "Science", "Varied", "Pop Music", "Rock Music", "Easy Listening", "Light Classical",
                              "Serious Classical", "Other Music", "Weather", "Finance", "Children's", "Social Affairs",
                              "Religion", "Phone In", "Travel", "Leisure", "Jazz Music", "Country Music", "National Music",
                              "Oldies Music", "Folk Music", "Documentary", "Alarm Test", "Alarm"};
    return v >= 0 && v < 32 ? t[v] : "?";
}
inline const char* langName(int v) {
    static const char* t[] = {nullptr, "Albanian", "Breton", "Catalan", "Croatian", "Welsh", "Czech", "Danish", "German",
                              "English", "Spanish", "Esperanto", "Estonian", "Basque", "Faroese", "French", "Frisian",
                              "Irish", "Gaelic", "Galician", "Icelandic", "Italian", "Lappish", "Latin", "Latvian",
                              "Luxembourgish", "Lithuanian", "Hungarian", "Maltese", "Dutch", "Norwegian", "Occitan",
                              "Polish", "Portuguese", "Romanian", "Romansh", "Serbian", "Slovak", "Slovene", "Finnish",
                              "Swedish", "Turkish", "Flemish", "Walloon"};
    return v >= 1 && v <= 43 ? t[v] : nullptr;
}
/** ★ "RT+ in 12A" proves only that the ODA ANNOUNCEMENT decoded — the Now playing row is the evidence for the rest. */
inline std::string odaName(const std::string& aid) {
    static const std::pair<const char*, const char*> t[] = {{"4BD7", "RT+"}, {"6552", "eRT"}, {"CD46", "TMC"}, {"CD47", "TMC"},
        {"0093", "DAB x-ref"}, {"4BD8", "RT+ (group B)"}, {"C563", "ID Logic"}, {"6365", "RDS2 station logo"}};
    for (auto& p : t) if (aid == p.first) return p.second;
    return aid;
}
inline const char* coverage(int v) {
    static const char* t[] = {"Local", "International", "National", "Supra-regional", "Regional 1", "Regional 2", "Regional 3",
                              "Regional 4", "Regional 5", "Regional 6", "Regional 7", "Regional 8", "Regional 9",
                              "Regional 10", "Regional 11", "Regional 12"};
    return t[v & 15];
}

// ── formatting the way JS does ───────────────────────────────────────────────────────────────────────────────────────
/** Number.prototype.toFixed: the exact decimal of the double, a TIE going AWAY from zero (printf's goes to even —
 *  (2.5).toFixed(0) is "3" in JS and "2" from %.0f). A negative that rounds to zero keeps JS's "-0". */
inline std::string fx(double v, int d) {
    if (!std::isfinite(v)) return std::isnan(v) ? "NaN" : v > 0 ? "Infinity" : "-Infinity";
    const double a = std::fabs(v);
    char b[1200];   // (a double prints to ≤ 309 integer digits)
    // ★ an exact tie only: the double's own decimal expansion is …5000… past the last kept digit (6.85 is 6.8499…)
    std::snprintf(b, sizeof b, "%.*f", d + 40, a);
    const char* dot = std::strchr(b, '.');
    bool tie = dot && dot[d + 1] == '5';
    for (int i = d + 2; tie && dot[i]; i++) if (dot[i] != '0') tie = false;
    const double use = tie ? a + 0.5 * std::pow(10.0, -d) : a;   // (an exact tie lifted onto the next step: rounds up)
    std::snprintf(b, sizeof b, "%.*f", d, use);
    return (v < 0 ? "-" : "") + std::string(b);
}
inline long jsRound(double v) { return (long)std::floor(v + 0.5); }   // Math.round (−2.5 → −2, unlike lround)
inline std::string two(int v) { char b[16]; std::snprintf(b, sizeof b, "%02d", v); return b; }
inline std::string hex(int v, int w) { char b[16]; std::snprintf(b, sizeof b, "%0*X", w, v); return b; }
/** String(n) for the half-hour offset: 1, 5.5 … */
inline std::string jsNum(double v) { if (v == std::floor(v)) return std::to_string((long)v); char b[32]; std::snprintf(b, sizeof b, "%g", v); return b; }

/** typeof x === 'number' && isFinite(x) ? x : d — the app's parse (VibeServerWsClient.ts). */
inline double num(const Json& j, const char* k, double d = -1) {
    const Json& v = j[k];
    return v.kind == Json::Num && std::isfinite(v.n) ? v.n : d;
}
/** Number(msg.k ?? def) === 1 */
inline bool flag(const Json& j, const char* k, bool def) {
    const Json& v = j[k];
    if (v.kind == Json::Null) return def;
    if (v.kind == Json::Bool) return v.b;
    if (v.kind == Json::Num) return v.n == 1;
    return false;
}

// ── src/services/lrMeter.ts ──────────────────────────────────────────────────────────────────────────────────────────
constexpr double LR_LIMIT_KHZ = 75;
struct LrSide { double pct = 0; std::string text = kDash; Tone tone = T_MUTED; };
struct Lr { bool show = false; LrSide l, r; };
inline bool lrParts(bool have, double lDev, double rDev, Lr& out) {
    if (!have || !(lDev >= 0) || !(rDev >= 0)) return false;
    auto side = [](double k) { LrSide s; s.pct = std::max(0.0, std::min(100.0, k)); s.text = fx(k, 0) + " kHz";
                               s.tone = k > 82 ? T_BAD : k > LR_LIMIT_KHZ ? T_WARN : T_GOOD; return s; };
    out.show = true; out.l = side(lDev); out.r = side(rDev);
    return true;
}
/** lrDisplay: the parts, else (seen once) the empty meters, else nothing drawn at all — never a false zero. */
inline Lr lrDisplay(bool have, double lDev, double rDev, bool seen) {
    Lr o;
    if (lrParts(have, lDev, rDev, o)) return o;
    if (seen) { o.show = true; }   // LR_EMPTY: an empty track and a dash, tone 'none'
    return o;
}

// ── src/services/mpxPower.ts ─────────────────────────────────────────────────────────────────────────────────────────
constexpr double MPX_POWER_WINDOW_S = 60;
inline std::pair<std::string, std::string> mpxPowerParts(double db, double secs) {
    if (!(secs > 0) || !std::isfinite(db)) return {kDash, ""};
    const std::string value = std::string(db >= 0 ? "+" : kMinus) + fx(std::fabs(db), 1) + " dBr";
    const double left = std::ceil(MPX_POWER_WINDOW_S - secs - 0.5);
    return {value, left > 0 ? "settling " + jsNum(left) + " s" : ""};
}

// ── the constellation (AdvRdsPanel constellationAngle / Scale / Verdict) ─────────────────────────────────────────────
/** ★★ The rotation that lays the two BPSK lobes on the horizontal (angle doubled to fold the lobes, magnitude-weighted,
 *  halved). Our detector is differential, so the plot arrives tilted by the reference's offset. */
inline double constellationAngle(const std::vector<float>& xy) {
    double sx = 0, sy = 0;
    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        const double x = xy[i], y = xy[i + 1], r2 = x * x + y * y;
        if (r2 < 1) continue;
        const double a2 = 2 * std::atan2(y, x);
        sx += r2 * std::cos(a2); sy += r2 * std::sin(a2);
    }
    return (sx != 0 || sy != 0) ? -0.5 * std::atan2(sy, sx) : 0;
}
/** ★ Scale to the MEAN LOBE DISTANCE (never a constant — the plot's meaning is its shape). */
inline double constellationScale(const std::vector<float>& xy, double box) {
    int n = 0; double sum = 0;
    for (size_t i = 0; i + 1 < xy.size(); i += 2) { const double r = std::hypot((double)xy[i], (double)xy[i + 1]); if (r < 1) continue; n++; sum += r; }
    if (!n) return (box / 2) / 110;
    return (box * 0.30) / std::max(1.0, sum / n);
}
inline const char* const kConstRotating = "rotating \xE2\x80\x94 unlocked encoder";
inline std::pair<std::string, Tone> constellationVerdict(const std::vector<float>& xy, double phaseCoh, double ber) {
    const double rot = constellationAngle(xy), cr = std::cos(rot), sr = std::sin(rot);
    std::vector<double> rx;
    int n = 0; double sumAbsX = 0, sumY2 = 0, sumXErr2 = 0;
    for (size_t i = 0; i + 1 < xy.size(); i += 2) {
        const double r2 = (double)xy[i] * xy[i] + (double)xy[i + 1] * xy[i + 1];
        if (r2 < 1) continue;
        const double x = xy[i] * cr - xy[i + 1] * sr, y = xy[i] * sr + xy[i + 1] * cr;
        rx.push_back(x); n++; sumAbsX += std::fabs(x); sumY2 += y * y;
    }
    if (n < 8) return {"RDS no lock", T_BAD};
    const double meanAbsX = sumAbsX / n;
    if (meanAbsX < 1) return {"RDS no lock", T_BAD};
    for (double x : rx) { const double dx = std::fabs(x) - meanAbsX; sumXErr2 += dx * dx; }
    const double evm = (std::sqrt((sumY2 + sumXErr2) / n) / meanAbsX) * 100;
    if (phaseCoh < 0.35 && ber >= 0 && ber < 20) return {kConstRotating, T_WARN};
    if (evm < 45) return {"clean \xC2\xB7 " + fx(evm, 0) + "% scatter", T_GOOD};
    if (evm < 80) return {"usable \xC2\xB7 " + fx(evm, 0) + "% scatter", T_WARN};
    return {"noisy \xC2\xB7 " + fx(evm, 0) + "% scatter", T_BAD};
}

// ── the eye grids (decodeEye — MUST match kEyeAlphabet in local_sdr_shim.cpp) ───────────────────────────────────────
inline int eyeIndex(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + c - 'a';
    if (c >= '0' && c <= '9') return 52 + c - '0';
    if (c == '+') return 62;
    if (c == '-') return 63;
    return -1;
}
/** '.' then one character = 1..64 zeros; anything else a literal cell 0..63. False on a malformed or short grid. */
inline bool decodeEye(const std::string& src, int want, std::vector<uint8_t>& out) {
    if (src.empty() || want <= 0) return false;
    out.assign((size_t)want, 0);
    int o = 0;
    for (size_t i = 0; i < src.size() && o < want; i++) {
        const char c = src[i];
        if (c == '.') {
            if (++i >= src.size()) return false;
            const int n = eyeIndex(src[i]) + 1;
            if (n <= 0) return false;
            o += n;
        } else {
            const int v = eyeIndex(c);
            if (v < 0) return false;
            out[(size_t)o++] = (uint8_t)v;
        }
    }
    return o >= want;
}

/** One row of the panel. `summary` = shown in SMALL (the readings you watch while tuning). */
struct Row { std::string label, text; Tone tone = T_VALUE; bool summary = false; };

/** The MPX deviation readout (mpxDevInfo). */
struct Dev { std::string avg = kDash, peak = kDash, verdict = "no signal"; Tone tone = T_MUTED; double pct = 0, avgPct = 0, hold = 0; };
/** One SCOPE VIEW band. */
struct Band { const char* name; Tone tone; double khz; };

/** The panel's state between frames (the app's refs) and its latest reading. */
class Core {
public:
    // ── what the UI draws ──
    std::vector<Row> rows;
    bool have = false;                          // an rdsx has arrived since the panel opened
    std::pair<std::string, Tone> constV{"RDS no lock", T_BAD};
    std::pair<std::string, Tone> eyeV{kDash, T_MUTED};
    Dev dev;
    Lr lr;
    std::string powValue = kDash, powSettling;
    Band bands[3] = {{"PILOT", T_WARN, 0}, {"STEREO", T_BAD, 0}, {"RDS", T_BAD, 0}};
    std::vector<float> xy, mpx;                 // the latest constellation (x100, ±127) and MPX spectrum (dB)
    std::string eye[3];                         // ★ the LAST grids seen: with eyeEvery the engine sends them every Nth frame
    int eyeW = 0, eyeH = 0;
    uint64_t eyeGen = 0;                        // bumps when a new grid arrives (the UI re-uploads its textures)
    double rate = 0;                            // groups a second (successive deltas)

    /** The panel opened: start afresh (the app's refs live as long as the component). */
    void reset() { *this = Core(); }

    /** `x` = the rdsx frame; `rds` = the latest basic RDS message (pi, ecc, ber, call); `wsp` = weak-signal processing
     *  (-1 not reported, 0 off, 1 on); `iso` = the transmitter's country from ECC + PI ("" = unresolved). */
    void update(const Json& x, const Json& rds, int wsp, const std::string& iso, double nowSec) {
        have = true;
        rows.clear();
        // ── the arrays ──
        xy.clear(); mpx.clear();
        for (auto& v : x["xy"].a) if (v.kind == Json::Num) xy.push_back((float)v.n);
        for (auto& v : x["mpx"].a) if (v.kind == Json::Num) mpx.push_back((float)v.n);
        static const char* ek[3] = {"eyeP", "eyeS", "eyeR"};
        bool newEye = false;
        for (int b = 0; b < 3; b++) if (x[ek[b]].kind == Json::Str) { eye[b] = x[ek[b]].s; newEye = true; }   // ★ ABSENT = UNCHANGED
        eyeW = (int)num(x, "eyeW", 0); eyeH = (int)num(x, "eyeH", 0);
        if (newEye) eyeGen++;

        const double pdev = num(x, "pilotDev"), rdev = num(x, "rdsDev"), mpxSnr = num(x, "mpxSnr", 0);
        const bool pilotLock = flag(x, "pilotLock", false);
        const bool snrOk = flag(x, "snrOk", true);
        const double berX = num(x, "ber"), berRds = num(rds, "ber");
        const double ber = berX >= 0 ? berX : berRds;   // ★ the frame's own BER first (measured with the phase)

        // ── the MPX deviation meter (mpxDevInfo) — ★ it also latches devGate, which the RDS dev row reads ──
        {
            const double md = num(x, "mpxDev", 0), hd = num(x, "mpxHold", 0), av = num(x, "mpxAvg", 0);
            // ★★★ HYSTERESIS: 10 dB in, 8 dB out — a station on the line must not blink between trusted and not
            if (mpxSnr >= 10) devGate_ = true;
            else if (mpxSnr > 0 && mpxSnr < 8) devGate_ = false;
            const bool live = md > 0.1 || hd > 0.1;
            const bool ok = live && devGate_;
            Dev d;
            if (live) {
                const double pk = hd;   // ★★★ the digits and the verdict read the 6 s HOLD; the bar reads the fast peak
                const double nz = num(x, "mpxNoise", 0);
                const std::string nzTxt = nz >= 1 ? " \xC2\xB7 " + fx(nz, 0) + " kHz noise removed"
                                        : nz <= -1 ? " \xC2\xB7 neighbour in the guard band, noise not removed" : "";
                d.avg = av >= 0.5 ? fx(av, 0) + " kHz" : kDash;
                // ★★★ 75–82 HEDGES (two radios agree to ~7 %); OVERMODULATED only above 82
                const std::string v = ok ? (pk > 82 ? "OVERMODULATED" : pk > 75 ? "close to the limit" : "nominal") : "low S/N, unreliable";
                d.tone = !ok ? T_MUTED : pk > 82 ? T_BAD : pk > 75 ? T_WARN : T_GOOD;
                d.peak = fx(pk, 0) + " kHz";
                d.verdict = v + nzTxt;
                d.pct = std::max(0.0, std::min(100.0, md));
                d.avgPct = std::max(0.0, std::min(100.0, av));
                d.hold = std::max(0.0, std::min(100.0, hd));
            }
            dev = d;
        }
        // ── the composite eye's reading (eyeVerdict) ──
        {
            const double d = num(x, "eyeDev", 0);
            const bool pilotSeen = pdev > 0.5;
            if (!pilotLock && pilotSeen) eyeV = {"pilot present but not locking \xE2\x80\x94 forms and collapses", T_WARN};
            else if (!pilotLock) eyeV = {"no pilot \xE2\x80\x94 untriggered, so this is noise not a trace", T_BAD};
            else if (mpxSnr > 0 && !devGate_) eyeV = {"buried in noise \xE2\x80\x94 level not measurable", T_BAD};
            else {
                const char* what = d < 3 ? "little above the audio \xE2\x80\x94 mono or blended" : d < 10 ? "pilot dominates \xC2\xB7 little stereo"
                                 : d < 25 ? "pilot, stereo and RDS" : d < 50 ? "strong stereo" : "very strong \xC2\xB7 near full deviation";
                const bool noisy = mpxSnr > 0 && mpxSnr < 28;
                eyeV = {d > 0.1 ? std::string(what) + (noisy ? " \xC2\xB7 noisy" : "") : std::string(kDash), noisy ? T_WARN : T_MUTED};
            }
        }
        // ── SCOPE VIEW colours: the VERDICT, not the identity (main.ts drawMpxEye) ──
        {
            double amp[3] = {0, 0, 0};
            for (size_t i = 0; i < 3 && i < x["eyeAmp"].a.size(); i++) amp[i] = x["eyeAmp"].a[i].kind == Json::Num ? x["eyeAmp"].a[i].n : 0;
            const double pd = std::max(0.0, pdev), rd = std::max(0.0, rdev);   // (x?.pilotDev ?? 0: −1 from the parse reads as "none")
            bands[0] = {"PILOT", !pilotLock ? T_WARN : (pd >= 6.0 && pd <= 7.5) ? T_GOOD : T_WARN, pdev};
            bands[1] = {"STEREO", mpxSnr >= 28 ? T_GOOD : mpxSnr >= 10 ? T_WARN : T_BAD, amp[1]};
            bands[2] = {"RDS", (rd <= 0.2 || rd > 5.8) ? T_BAD : rd < 1.5 ? T_WARN : T_GOOD, rdev};
        }
        // ── L / R, sticky once seen (lrDisplay) ──
        {
            const bool hasLr = x["lDev"].kind == Json::Num && x["rDev"].kind == Json::Num;
            const double l = num(x, "lDev", -1), r = num(x, "rDev", -1);
            if (hasLr && l >= 0) lrSeen_ = true;
            lr = lrDisplay(hasLr, l, r, lrSeen_);
        }
        { auto p = mpxPowerParts(num(x, "mpxPow", 0), num(x, "mpxPowS", 0)); powValue = p.first; powSettling = p.second; }
        constV = constellationVerdict(xy, num(x, "phaseCoh", 0), ber);

        // ════ the rows, in the app's order (PI / Station / RadioText are the VTS bar's — vts.h shows them already) ════
        // ── PTY ──
        const int ptyV = (int)num(x, "pty");
        add("Type", ptyV >= 0 ? std::string(ptyName(ptyV)) + " (" + std::to_string(ptyV) + ")" : kDash, T_VALUE, true);
        // ── TP / TA / MS (block B: they confirm together) ──
        {
            std::string f;
            auto push = [&](const char* s) { if (!f.empty()) f += " \xC2\xB7 "; f += s; };
            if ((int)num(x, "tp") == 1) push("TP");
            if ((int)num(x, "ta") == 1) push("TA");
            const int ms = (int)num(x, "ms");
            if (ms == 1) push("Music"); else if (ms == 0) push("Speech");
            add("Flags", f.empty() ? kDash : f, T_VALUE, true);
        }
        add("Errors", ber >= 0 ? jsNum(ber) + "%" : kDash, T_VALUE, true);
        // ── pilot deviation, against its own 6.0–7.5 kHz band, and whether the PLL is LOCKED ──
        {
            std::string t = kDash; Tone c = T_VALUE;
            if (pdev > 0.2) {
                const bool ok = pdev >= 6.0 && pdev <= 7.5;
                const bool hasLock = x["pilotLock"].kind != Json::Null;
                const std::string lockTxt = !hasLock ? "" : pilotLock ? " \xC2\xB7 locked" : " \xC2\xB7 NOT locked";
                t = fx(pdev, 1) + " kHz \xC2\xB7 " + (ok ? "nominal" : pdev < 6 ? "low" : "high") + lockTxt;
                c = (hasLock && !pilotLock) ? T_WARN : ok ? T_GOOD : T_WARN;
            }
            add("Pilot dev", t, c, true);
        }
        // ── RDS deviation: refused below the gate, held 4 s through a dropout, never a strobing dash ──
        {
            std::string t = "0.0 kHz \xC2\xB7 none"; Tone c = T_MUTED;
            const bool rdsBerBad = ber >= 50;   // ✗✗✗ block sync is not usable blocks
            const bool gateOpen = (devGate_ || mpxSnr <= 0) && !rdsBerBad;
            if (rdev > 0.2 && !gateOpen) {
                t = rdsBerBad ? "not measurable at this error rate" : "not measurable at this S/N";
                c = T_WARN;                       // ✗ never written to the hold
            } else if (rdev > 0.2) {
                const double rpk = num(x, "rdsDevPeak", 0), rraw = num(x, "rdsDevRaw", 0);
                const std::string pkTxt = rpk > 0.2 ? " \xC2\xB7 peak " + fx(rpk, 1) : "";
                const std::string rawTxt = rraw > 0.2 && std::fabs(rraw - rdev) > rdev * 0.02 ? " \xC2\xB7 raw " + fx(rraw, 1) : "";
                const bool impossible = rdev > 5.8, strong = rdev >= 4.0, low = rdev < 1.5;
                t = "avg " + fx(rdev, 1) + pkTxt + rawTxt + " kHz \xC2\xB7 " +
                    (impossible ? "over spec \xE2\x80\x94 suspect" : low ? "weak" : strong ? "generous" : "nominal");
                c = impossible ? T_BAD : low ? T_WARN : T_GOOD;
                rdsHoldTxt_ = t; rdsHoldTone_ = c; rdsHoldAt_ = nowSec;
            } else if (rdsHoldAt_ >= 0 && nowSec - rdsHoldAt_ < 4.0) {
                t = rdsHoldTxt_; c = rdsHoldTone_;
            }
            add("RDS dev", t, c, true);
        }
        // ── MPX S/N, and what the receiver DID about it ──
        {
            std::string t = kDash; Tone c = T_VALUE;
            if (!snrOk) t = "no pilot to measure";   // ★★★ no pilot, no figure
            else if (mpxSnr > 0.5 && wsp == 0) t = fx(mpxSnr, 0) + " dB \xC2\xB7 bypassed";   // ★★★ switched off is not clean
            else if (mpxSnr > 0.5) {
                const double lmr = num(x, "hiCutLmr", 15000), aud = num(x, "hiCutAud", 15000);
                const bool acting = lmr < 14000 || aud < 14000;
                t = fx(mpxSnr, 0) + " dB \xC2\xB7 " + (!acting ? std::string("clean \xC2\xB7 no treatment")
                    : aud < 14000 ? "blend " + fx(lmr / 1000, 1) + "k \xC2\xB7 cut " + fx(aud / 1000, 1) + "k"
                                  : "blend " + fx(lmr / 1000, 1) + "k");
                c = acting ? T_WARN : T_GOOD;
            }
            add("MPX S/N", t, c, true);
        }
        // ── Rate: from SUCCESSIVE deltas over ≥ 1 s, never total-over-elapsed ──
        const double tot = num(x, "gtot", 0);
        {
            if (tot > rTot_ && rAt_ > 0) {
                const double dt = nowSec - rAt_;
                if (dt >= 1.0) { const double inst = (tot - rTot_) / dt; rRate_ = rRate_ > 0 ? rRate_ * 0.7 + inst * 0.3 : inst; rTot_ = tot; rAt_ = nowSec; }
            } else if (tot != rTot_) { rTot_ = tot; rAt_ = nowSec; }
            rate = rRate_;
            add("Rate", tot > 0 ? (rRate_ > 0 ? fx(rRate_, 1) + "/s of 11.4 \xC2\xB7 " + jsNum(tot) + " total" : jsNum(tot) + " total") : kDash, T_VALUE, true);
        }
        // ════ BIG only from here ════
        // ── RDS-to-pilot phase — the field this panel exists for ──
        {
            const double ph = num(x, "phase"), coh = num(x, "phaseCoh", 0);
            std::string t = kDash; Tone c = T_VALUE;
            if (ph < 0 || coh < 0.35) {
                const bool rotating = ph >= 0 && coh < 0.35 && ber >= 0 && ber < 20;   // ★★ a ring is a diagnosis
                t = ph < 0 ? std::string(kDash) : rotating ? "rotating \xE2\x80\x94 encoder not locked to pilot" : "unstable \xE2\x80\x94 not measurable";
                c = rotating ? T_WARN : T_MUTED;
            } else {
                const double d0 = std::min(ph, 180 - ph), d90 = std::fabs(ph - 90), nearv = std::min(d0, d90);
                const double drift = num(x, "phaseDrift", 0);
                if (drift >= 2) { t = "rotating " + fx(drift, 0) + kDeg + "/s \xE2\x80\x94 encoder not locked to pilot"; c = T_WARN; }
                else {
                    const char* v = nearv <= 12 ? (d0 <= d90 ? "in phase" : "quadrature") : nearv <= 40 ? "off nominal" : coh > 0.7 ? "FAULT" : "off nominal";
                    std::string shown;
                    if (x["phaseSigned"].kind == Json::Num) {
                        const long r = jsRound(x["phaseSigned"].n);
                        shown = std::string(r > 0 ? "+" : r < 0 ? kMinus : "") + std::to_string(std::labs(r)) + kDeg;
                    } else shown = fx(ph, 0) + kDeg;
                    t = shown + " \xC2\xB7 " + v + " \xC2\xB7 " + fx(coh * 100, 0) + "% steady";
                    c = nearv <= 12 ? T_GOOD : nearv <= 40 ? T_WARN : T_BAD;
                }
            }
            add("RDS/pilot", t, c);
        }
        // ── Multipath (a REFLECTION, not weakness), held through a dropout, then what IMS does about it ──
        const double mp = num(x, "multipath", 0);
        {
            std::string t = kDash; Tone c = T_VALUE;
            if (!snrOk) t = "no pilot to measure";
            else if (flag(x, "multipathOk", false)) {
                const double pct = mp * 100;
                const char* label = pct < 0.5 ? "none detected" : mp < 0.03 ? "clean" : mp < 0.10 ? "slight" : mp < 0.20 ? "moderate" : "severe";
                t = pct < 0.5 ? std::string(label) : fx(pct, 1) + "% \xC2\xB7 " + label;
                c = mp < 0.03 ? T_GOOD : mp < 0.10 ? T_VALUE : mp < 0.20 ? T_WARN : T_BAD;
                mpHeld_ = t;
            } else if (!mpHeld_.empty()) { t = mpHeld_ + " \xC2\xB7 held"; c = T_MUTED; }
            else t = "too noisy to judge";
            if (snrOk) {
                const double ib = num(x, "imsBlend", 0), iw = num(x, "imsWhy", 1);
                t += ib > 0 ? " \xC2\xB7 IMS blending L\xE2\x88\x92R to " + fx(ib / 1000, 1) + "k"
                   : iw == 2 ? " \xC2\xB7 IMS standing by \xC2\xB7 CEQ has it"
                   : iw == 3 ? " \xC2\xB7 IMS standing by \xC2\xB7 nothing to suppress"
                   : iw == 4 ? " \xC2\xB7 IMS standing by \xC2\xB7 NR already blending further"
                   : iw == 5 ? " \xC2\xB7 multipath not measurable at this S/N" : "";
            }
            add("Multipath", t, c);
        }
        // ── CEQ, BEFORE and AFTER (green = it helped) ──
        {
            std::string t; Tone c = T_VALUE;
            if (flag(x, "ceqOn", false)) {
                const double before = mp * 100, after = num(x, "ceqAfter", 0) * 100;
                const bool helped = before > 0.5 && after < before * 0.8;
                c = helped ? T_GOOD : T_VALUE;
                t = "before " + fx(before, 1) + "% \xC2\xB7 after " + fx(after, 1) + "%" + (helped ? "" : " \xC2\xB7 little change");
            } else {
                const double why = num(x, "ceqWhy", 3);
                t = why == 1 ? "off" : why == 2 ? "signal too weak to equalise" : "standing by \xC2\xB7 nothing to correct";
                c = (why == 1 || why == 2) ? T_VALUE : T_GOOD;
            }
            add("CEQ", t, c);
        }
        // ── Blanker ──
        {
            const double nb = num(x, "nbRate", 0) * 100;
            add("Blanker", nb < 0.005 ? std::string("nothing to blank") : fx(nb, nb < 1 ? 2 : 1) + "% blanked",
                nb < 0.005 ? T_GOOD : nb > 0.5 ? T_WARN : T_VALUE);
        }
        // ── IF narrow: the filter ACTUALLY applied, its adjective from its width, the shadow's comparison when there is one ──
        {
            const double g = num(x, "ifGain", 0), cand = num(x, "ifCand", 0), bw = num(x, "ifBw", 0);
            std::string t = kDash; Tone c = T_VALUE;
            if (bw > 0) {
                c = T_GOOD;
                const char* adj = bw <= 160000 ? " narrow" : bw >= 220000 ? " wide" : "";
                t = std::to_string(jsRound(bw / 1000)) + "k" + adj;
                if (cand > 0 && mpxSnr > 0.5) t += " \xC2\xB7 wide would " + (g > 1.5 ? "gain " + fx(g, 1) + " dB" : "cost " + fx(std::fabs(g), 1) + " dB");
            } else if (cand > 0 && mpxSnr > 0.5) {
                c = T_GOOD;
                t = "wide \xC2\xB7 " + std::to_string(jsRound(cand / 1000)) + "k " + (g > 1.5 ? "would help" : g < -1.5 ? "would cost" : "no real gain");
            }
            add("IF narrow", t, c);
        }
        {
            const std::string a = x["rtpArtist"].str(), ti = x["rtpTitle"].str();
            add("Now playing", !a.empty() && !ti.empty() ? a + " \xE2\x80\x94 " + ti : !a.empty() ? a : !ti.empty() ? ti : kDash);
        }
        add("Long PS", x["longPs"].str().empty() ? kDash : x["longPs"].str());
        add("PTYN", x["ptyn"].str().empty() ? kDash : x["ptyn"].str());
        {
            const int l = (int)num(x, "lang");
            add("Language", l > 0 ? (langName(l) ? langName(l) : "code " + std::to_string(l)) : kDash);
        }
        {
            const int d = (int)num(x, "pinDay");
            add("PIN", d > 0 ? "day " + std::to_string(d) + " " + two((int)num(x, "pinHour")) + ":" + two((int)num(x, "pinMin")) : kDash);
        }
        {
            std::string t;
            for (auto& o : x["oda"].a) {
                if (!t.empty()) t += ", ";
                const int g = (int)num(o, "grp", 0);
                t += odaName(o["aid"].str()) + " in " + std::to_string(g >> 1) + ((g & 1) ? "B" : "A");
            }
            add("ODA", t.empty() ? kDash : t);
        }
        {
            std::string t;
            for (auto& e : x["eon"].a) {
                if (!t.empty()) t += "  ";
                std::string ps = e["ps"].str();
                while (!ps.empty() && ps.back() == ' ') ps.pop_back();
                while (!ps.empty() && ps.front() == ' ') ps.erase(0, 1);
                const double af = num(e, "af", 0);
                t += (ps.empty() ? e["pi"].str() : ps) + (af ? " " + fx(af / 1000, 1) : "") + ((int)num(e, "ta", 0) == 1 ? " [TA]" : "");
            }
            add("Other networks", t.empty() ? kDash : t);
        }
        {
            const int di = (int)num(x, "di");
            std::string t = kDash;
            if (di >= 0) {
                t = di & 1 ? "Stereo" : "Mono";
                if (di & 2) t += " \xC2\xB7 Artificial head";
                if (di & 4) t += " \xC2\xB7 Compressed";
                if (di & 8) t += " \xC2\xB7 Dynamic PTY";
            }
            add("DI", t);
        }
        // ── Clock, AS TRANSMITTED with its offset; "waiting" — CT is one group in ~660 ──
        {
            const int ct = (int)num(x, "ct");
            const double g4a = x["grp"].a.size() > 8 ? x["grp"].a[8].num(0) : 0;
            std::string t = kDash;
            if (ct < 0) { if (tot > 0) t = g4a > 0 ? "seen, damaged" : "waiting\xE2\x80\xA6 (1/min)"; }
            else {
                const double off = num(x, "ctoff", 0);
                t = two(ct / 60) + ":" + two(ct % 60) + " " + (off == 0 ? std::string("UTC") : "UTC" + std::string(off > 0 ? "+" : kMinus) + jsNum(std::fabs(off) / 2));
            }
            add("Clock", t);
        }
        // ── Country: SAY HOW WE KNOW (ECC, or nothing yet) ──
        const int pi = (int)num(rds, "pi", 0), ecc = (int)num(rds, "ecc", 0);
        {
            std::string eccTxt = ecc > 0 ? "ECC " + hex(ecc, 1) : "";
            std::string t = !iso.empty() ? iso + " \xC2\xB7 " + (ecc > 0 ? eccTxt : std::string("from PI"))
                          : ecc > 0 ? eccTxt + " \xC2\xB7 unmatched" : tot > 0 ? "waiting for ECC (1A)" : kDash;
            add("Country", t);
        }
        if (!rds["call"].str().empty()) add("Call", rds["call"].str() + " \xC2\xB7 from the PI (US)");
        // ★ PI detail carries the DECIMAL too (the VTS bar has the hex; the logs DXers use are split between the two)
        add("PI detail", pi > 0 ? std::to_string(pi) + " \xC2\xB7 " + coverage((pi >> 8) & 0xF) + " \xC2\xB7 ref " + std::to_string(pi & 0xFF) +
                                  " \xC2\xB7 cc " + std::to_string((pi >> 12) & 0xF) : kDash);
        const size_t nAf = x["af"].a.size();
        {
            const double seen = num(x, "afseen", 0);
            add("AF score", seen > 0 ? std::to_string(nAf) + "/" + jsNum(seen) + " \xC2\xB7 " + std::to_string(jsRound(nAf / seen * 100)) + "%" : kDash);
        }
        // ── AF: every entry, the CONFIRMED marked (sticky — a later damaged copy never un-confirms) ──
        {
            std::map<long, bool> seen;
            for (auto& e : x["afAll"].a) {
                if (e.kind != Json::Arr || e.a.size() < 2) continue;
                const long khz = (long)e.a[0].num(0);
                const bool ok = e.a[1].num(0) != 0;
                seen[khz] = seen[khz] || ok;
            }
            std::string t;
            bool anyOk = false;
            if (!seen.empty()) {
                for (auto& kv : seen) { if (!t.empty()) t += "  "; t += fx(kv.first / 1000.0, 1) + (kv.second ? "*" : ""); anyOk |= kv.second; }
                if (anyOk) t += "   (* confirmed)";
            } else {
                std::vector<long> v;
                for (auto& a : x["af"].a) v.push_back((long)a.num(0));
                std::sort(v.begin(), v.end()); v.erase(std::unique(v.begin(), v.end()), v.end());
                for (long k : v) { if (!t.empty()) t += "  "; t += fx(k / 1000.0, 1); }
            }
            add("AF MHz", t.empty() ? kDash : t);
        }
        // ── Group share: SAY WHAT THE PERCENTAGES ARE OF ──
        {
            std::string t = kDash;
            if (tot > 0) {
                std::vector<std::pair<std::string, long>> parts;
                const auto& g = x["grp"].a;
                for (size_t i = 0; i < g.size(); i++) {
                    const double n = g[i].num(0);
                    if (!n) continue;
                    parts.push_back({std::to_string(i >> 1) + ((i & 1) ? "B" : "A"), jsRound(n / tot * 100)});
                }
                std::stable_sort(parts.begin(), parts.end(), [](auto& a, auto& b) { return a.second > b.second; });
                if (!parts.empty()) {
                    t = "of " + jsNum(tot) + " groups:";
                    for (size_t i = 0; i < parts.size(); i++) t += (i ? "  " : " ") + parts[i].first + " " + std::to_string(parts[i].second) + "%";
                }
            }
            add("Group share", t);
        }
    }

    const Row* row(const std::string& label) const { for (auto& r : rows) if (r.label == label) return &r; return nullptr; }

private:
    bool devGate_ = false, lrSeen_ = false;
    std::string mpHeld_;
    std::string rdsHoldTxt_; Tone rdsHoldTone_ = T_MUTED; double rdsHoldAt_ = -1;
    double rTot_ = 0, rAt_ = 0, rRate_ = 0;
    void add(const char* l, const std::string& t, Tone c = T_VALUE, bool summary = false) { rows.push_back({l, t, c, summary}); }
};

}  // namespace ardr
}  // namespace hh
