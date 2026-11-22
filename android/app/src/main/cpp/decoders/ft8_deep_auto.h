// decoders/ft8_deep_auto.h — WHETHER FT8 / FT4 "DIG DEEPER" IS ON (V12 beta91, docs/v12/FTX-SENSITIVITY.md § Deep tier).
//
// ★★★ Stuart, 2026-11-22: "should be automatic and simply enable when the cpu has headroom with a benchmark driven toggle
//     in the settings with the rest of the digital modes, no user facing toggles". So:
//   1. The OWNER's word wins, once given: "ft8+deep" (on) or "ft8-deep" (off) in the radio's mode list (Setup → Modes and
//      decoders → Digital spots, and the Android server screen). Listeners never see or change it — they get the decodes.
//   2. Otherwise it is AUTOMATIC, from what this box measured:
//      · the benchmark's own dig-deeper row (dec_ft8deep): on below kGreenPct of a core;
//      · a benchmark from before that row (every server upgrading from beta90 or earlier): its Band row (dec_ft8band, one
//        pass) × kPerBand — the dig-deeper row measured 1.25 × the Band row on the same band — below kGreenPct; a Band row
//        that was not run (no radio here can make spots) is off;
//      · no benchmark at all: the server measures the dig-deeper row once by itself, off-thread at idle (the shim's
//        probe), keeps the figure beside the radio's bookmarks, and that figure decides.
//   3. On or off, the tier still only runs on slots with time to spare (Ft8Decoder::runDecode).
// Pure: the shim, the setup page's reading of it and test-ft8-deep-auto use the same rule.
#pragma once
#include <cstdlib>
#include <string>

namespace vibe {
namespace ft8deep {

constexpr double kGreenPct = 25.0;   ///< == benchdec::kDeepGreenPct (checked by test-ft8-deep-auto)
constexpr double kPerBand = 1.25;    ///< the dig-deeper row ÷ the Band row, same band (M5: 0.97 % ÷ 0.78 %)

enum class Choice : int { Off = -1, Auto = 0, On = 1 };
enum class Verdict : int { Off = 0, On = 1, Probe = 2 };

/** A benchmark row's "pct" from the stored JSON: 1 = found with a pct (in *pct, may be −1 = not measured),
 *  0 = listed without one (the "not run" list), −1 = absent. Only the row's own object is read. */
inline int rowPct(const std::string& j, const char* id, double* pct) {
    const std::string key = std::string("\"id\":\"") + id + "\"";
    const size_t at = j.find(key);
    if (at == std::string::npos) return -1;
    const size_t end = j.find('}', at);
    const std::string row = j.substr(at, end == std::string::npos ? std::string::npos : end - at);
    const size_t pp = row.find("\"pct\":");
    if (pp == std::string::npos) return 0;
    *pct = std::atof(row.c_str() + pp + 6);
    return 1;
}

/** What a stored benchmark says (Probe = it says nothing about FT8: measure). */
inline Verdict fromBench(const std::string& j) {
    if (j.empty()) return Verdict::Probe;
    double d = -1, b = -1;
    const int fd = rowPct(j, "dec_ft8deep", &d), fb = rowPct(j, "dec_ft8band", &b);
    if (fd == 1 && d >= 0) return d < kGreenPct ? Verdict::On : Verdict::Off;
    if (fb == 1) return (b >= 0 && b * kPerBand < kGreenPct) ? Verdict::On : Verdict::Off;
    if (fb == 0 || fd >= 0) return Verdict::Off;    // ★ listed but not run: no radio here makes spots
    return Verdict::Probe;
}

/** The owner's word, from the radio's mode list (the "+"/"−" tokens; the last one written wins, as the page writes one). */
inline Choice ownerChoice(bool hasOn, bool hasOff) {
    if (hasOn == hasOff) return Choice::Auto;
    return hasOn ? Choice::On : Choice::Off;
}

/** ★★ THE DECISION. `probePct` = the shim's own measurement of the dig-deeper row (−1 = none yet). `needProbe` is set
 *  when nothing decides yet — the shim then measures once at idle (off until then). */
inline bool decide(Choice owner, const std::string& benchJson, double probePct, bool& needProbe) {
    needProbe = false;
    if (owner == Choice::On) return true;
    if (owner == Choice::Off) return false;
    const Verdict v = fromBench(benchJson);
    if (v != Verdict::Probe) return v == Verdict::On;
    if (probePct >= 0) return probePct < kGreenPct;
    needProbe = true;
    return false;
}

} // namespace ft8deep
} // namespace vibe
