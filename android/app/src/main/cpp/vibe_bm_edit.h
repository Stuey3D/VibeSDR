// ★★ EDITING A SERVER BOOKMARK (V12, GitHub discussion #59, Gesiima) — the rules a save must pass.
//
// The owner could add and delete a receiver's bookmarks but not CHANGE one: a corrected frequency or a narrower passband
// meant stopping the service and editing bookmarks-<serial>.json by hand. The edit is POST /bookmarks with `was=<old Hz>`
// (and `was_sid=` for a DAB service), checked here BEFORE anything is touched, so a broken row can never be written.
//
//  ★ Not "can this radio tune it": a bookmark may be for another radio or another aerial on the same machine. Only sane.
//  ★ Pure and header-only so vibeserver/test-bm-edit.cpp checks it with no radio and no network.
#pragma once

#include <cmath>
#include <cstdlib>
#include <string>

namespace vibe {
namespace bmedit {

/** Highest frequency a bookmark may carry — 100 GHz, far past anything a supported radio reaches, short of nonsense. */
constexpr double kMaxHz = 100e9;
/** A passband edge is an offset from the carrier: ±500 kHz is wider than any demodulator's filter (WFM is ±100 kHz). */
constexpr int kMaxEdgeHz = 500000;

struct Fields {
    double      hz = 0;
    bool        hasBw = false;
    int         lo = 0, hi = 0;
};

/** Parse an integer query value strictly ("-3000" yes, "3k" / "" / "1e9" no). */
inline bool parseEdge(const std::string& s, int& out) {
    if (s.empty() || s.size() > 12) return false;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    if (v < -kMaxEdgeHz || v > kMaxEdgeHz) return false;
    out = int(v);
    return true;
}

/** Check an edit. `freq` / `lo` / `hi` are the raw query values; `nameClean` the name AFTER bmClean (so an all-control-
 *  character name counts as empty); `modeKnown` whether the mode is one of the server's. Returns "" and fills `f` when
 *  the edit is good, or the refusal in plain words — what the owner sees under the form. */
inline std::string check(const std::string& freq, const std::string& nameClean, const std::string& mode, bool modeKnown,
                         const std::string& lo, const std::string& hi, Fields& f) {
    char* end = nullptr;
    const double hz = freq.empty() ? 0.0 : std::strtod(freq.c_str(), &end);
    if (freq.empty() || !end || *end != '\0' || !std::isfinite(hz) || !(hz >= 1.0))
        return "The frequency must be a number of hertz above zero.";
    if (hz > kMaxHz) return "That frequency is past 100 GHz — check the units (the server takes hertz).";
    if (nameClean.empty()) return "The bookmark needs a name.";
    if (mode.empty()) return "Choose a mode for the bookmark.";
    if (!modeKnown) return "\"" + mode + "\" is not a mode this server knows.";
    f = Fields{};
    f.hz = hz;
    if (lo.empty() && hi.empty()) return "";                 // ★ the mode's default passband
    if (lo.empty() || hi.empty()) return "Give both passband edges, or neither to use the mode's default.";
    if (!parseEdge(lo, f.lo) || !parseEdge(hi, f.hi))
        return "Passband edges are whole hertz from the carrier, between -500000 and 500000.";
    if (f.hi <= f.lo) return "The passband's low edge must be below its high edge.";
    f.hasBw = true;
    return "";
}

}  // namespace bmedit
}  // namespace vibe
