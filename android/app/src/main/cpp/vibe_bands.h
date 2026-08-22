// vibe_bands.h — what an owner will let listeners tune to, as a set of ranges.
//
// ★★★ ONE EFFECTIVE SET, COMPUTED ONCE. The owner writes two lists — allow and block — and the
//     question every other part of the server asks is the same one: "may this frequency be tuned?"
//     Answering it from two lists at each call site is how the client and the server end up
//     disagreeing about where the radio may go, which is the worst outcome: the readout says one
//     thing and the hardware does another. So both lists collapse HERE into a single ordered,
//     non-overlapping set of permitted ranges, and everything downstream reads only that.
//
// ★★ THE HARDWARE ALWAYS WINS. Coverage is intersected first: an owner may allow 0–2 GHz, but an
//    Airspy HF+ still cannot hear 40 MHz, and offering it is how a listener ends up parked on dead
//    air blaming the receiver. Same reasoning as the published 31–60 MHz hole.
//
// ★ Empty allow list = "everything the hardware can reach". It must not mean "nothing", or an
//   owner who only wanted to block the airband would take their receiver off the air.
#pragma once

#include <algorithm>
#include <cctype>
// ★ <cmath> for std::fabs in bandLabel(). It arrives transitively on clang/macOS and NOT on
//   Debian's gcc, so leaving it out builds clean here and fails on the machine that ships —
//   see mac_only_compile_is_not_a_build. Named includes only.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace vibebands {

struct Range {
    double lo = 0, hi = 0;
    bool valid() const { return hi > lo && lo >= 0; }
};
using Ranges = std::vector<Range>;

/** ★★★ THE BAND PLAN DEPENDS ON WHERE THE RECEIVER IS. A server in the US is in ITU Region 2 and
 *  its allocations genuinely differ — 40 m runs to 7.300 not 7.200, 2 m to 148 not 146, medium
 *  wave to 1705 not 1606.5 — so shipping one continent's edges everywhere would block slices an
 *  American owner is entitled to and offer slices a European one is not (Stuart, 2026-08-08: "the
 *  bandplan needs to be ITU related so a server setup in the US gets its correct bandplan rather
 *  than our European one").
 *  ★ Only the differences are encoded. Where all three regions agree — the shortwave broadcast
 *    bands, the airband, DAB — there is one entry and no room for a transcription error. */
struct NamedBand { const char* id; const char* label; double lo, hi; };

/**
 * ITU region from a position. 1 = Europe/Africa/Middle East, 2 = the Americas, 3 = Asia-Pacific.
 *
 * ★★ SIMPLIFIED ON PURPOSE, and here is the seam: the real boundary runs along political borders,
 *    so longitude alone puts SIBERIA in Region 3 when Russia is Region 1 throughout. The carve-out
 *    below handles that case because it is the large one; the remaining error is at borders where
 *    the amateur allocations are the same anyway. A receiver's own operator can always type an
 *    explicit range, which is the escape hatch that makes an approximation acceptable here.
 * ★ Unknown position ⇒ Region 1: this server is written and mostly run in Europe, and a default
 *   that is right most of the time beats refusing to offer any presets at all.
 */
inline int ituRegion(double lat, double lon, bool havePos) {
    if (!havePos) return 1;
    while (lon >  180.0) lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    if (lon >= -170.0 && lon < -20.0) return 2;             // the Americas
    if (lon >= -20.0  && lon <  40.0) return 1;             // Europe, Africa, the Middle East
    if (lon >= 40.0 && lat >= 41.0)   return 1;             // ★ Russia is Region 1 to the Pacific
    return 3;                                                // Asia-Pacific
}

inline const std::vector<NamedBand>& namedBands(int region = 1) {
    // ★ Built once per region and handed out by reference — the caller iterates it on every
    //   hardware request and every parse.
    static const std::vector<NamedBand> kCommon = {
        { "lw",       "Long wave broadcast",   148500.0,     283500.0 },
        { "120m",     "120 m broadcast",      2300000.0,    2495000.0 },
        { "90m",      "90 m broadcast",       3200000.0,    3400000.0 },
        { "75m",      "75 m broadcast",       3900000.0,    4000000.0 },
        { "60mb",     "60 m broadcast",       4750000.0,    5060000.0 },
        { "60m",      "60 m amateur",         5258500.0,    5406500.0 },
        { "49m",      "49 m broadcast",       5900000.0,    6200000.0 },
        { "41m",      "41 m broadcast",       7200000.0,    7450000.0 },
        { "31m",      "31 m broadcast",       9400000.0,    9900000.0 },
        { "30m",      "30 m amateur",        10100000.0,   10150000.0 },
        { "25m",      "25 m broadcast",      11600000.0,   12100000.0 },
        { "22m",      "22 m broadcast",      13570000.0,   13870000.0 },
        { "20m",      "20 m amateur",        14000000.0,   14350000.0 },
        { "17m",      "17 m amateur",        18068000.0,   18168000.0 },
        { "16m",      "16 m broadcast",      17480000.0,   17900000.0 },
        { "15m",      "15 m amateur",        21000000.0,   21450000.0 },
        { "13m",      "13 m broadcast",      21450000.0,   21850000.0 },
        { "12m",      "12 m amateur",        24890000.0,   24990000.0 },
        { "11m",      "11 m broadcast",      25670000.0,   26100000.0 },
        { "cb",       "CB (27 MHz)",         26965000.0,   27405000.0 },
        { "10m",      "10 m amateur",        28000000.0,   29700000.0 },
        { "air",      "VHF airband",        108000000.0,  137000000.0 },
        { "marine",   "Marine VHF",         156000000.0,  162050000.0 },
        { "dab",      "DAB (Band III)",     174000000.0,  240000000.0 },
    };
    // Where the regions differ. Sources: ITU Radio Regulations Article 5 allocations.
    // ★★★ "FM Broadcast Band", NOT "FM broadcast". A restriction shown as "FM" is ambiguous in a
    //     way that matters here: it reads as easily as "this receiver is locked to the FM
    //     DEMODULATOR" as it does "locked to the FM broadcast band", and those are completely
    //     different limitations (Stuart, 2026-08-22). Naming the BAND removes the reading that is
    //     not true.
    //  ★ Changed in the table rather than at either call site, because this is the one vocabulary
    //    the receiver's landing page and the public directory both draw on — a second copy is how
    //    they end up disagreeing about what a server offers.
    static const std::vector<NamedBand> kR1 = {
        { "mw",   "AM (medium wave) broadcast", 526500.0, 1606500.0 },
        { "160m", "160 m amateur",   1810000.0,    2000000.0 },
        { "80m",  "80 m amateur",    3500000.0,    3800000.0 },
        { "40m",  "40 m amateur",    7000000.0,    7200000.0 },
        { "6m",   "6 m amateur",    50000000.0,   52000000.0 },
        { "fm",   "FM Broadcast Band",   87500000.0,  108000000.0 },
        { "2m",   "2 m amateur",   144000000.0,  146000000.0 },
        { "70cm", "70 cm amateur", 430000000.0,  440000000.0 },
        { "pmr",  "PMR446",        446000000.0,  446200000.0 },
    };
    static const std::vector<NamedBand> kR2 = {
        { "mw",   "AM (medium wave) broadcast", 525000.0, 1705000.0 },
        { "160m", "160 m amateur",   1800000.0,    2000000.0 },
        { "80m",  "80 m amateur",    3500000.0,    4000000.0 },
        { "40m",  "40 m amateur",    7000000.0,    7300000.0 },
        { "6m",   "6 m amateur",    50000000.0,   54000000.0 },
        { "fm",   "FM Broadcast Band",   88000000.0,  108000000.0 },
        { "2m",   "2 m amateur",   144000000.0,  148000000.0 },
        { "70cm", "70 cm amateur", 420000000.0,  450000000.0 },
        { "frs",  "FRS/GMRS",      462550000.0,  467725000.0 },
    };
    static const std::vector<NamedBand> kR3 = {
        { "mw",   "AM (medium wave) broadcast", 526500.0, 1606500.0 },
        { "160m", "160 m amateur",   1800000.0,    2000000.0 },
        { "80m",  "80 m amateur",    3500000.0,    3900000.0 },
        { "40m",  "40 m amateur",    7000000.0,    7200000.0 },
        { "6m",   "6 m amateur",    50000000.0,   54000000.0 },
        { "fm",   "FM Broadcast Band",   87500000.0,  108000000.0 },
        { "2m",   "2 m amateur",   144000000.0,  148000000.0 },
        { "70cm", "70 cm amateur", 430000000.0,  450000000.0 },
    };
    static std::vector<NamedBand> built[4];
    static bool done[4] = { false, false, false, false };
    const int r = (region >= 1 && region <= 3) ? region : 1;
    if (!done[r]) {
        const auto& extra = r == 2 ? kR2 : r == 3 ? kR3 : kR1;
        built[r] = kCommon;
        built[r].insert(built[r].end(), extra.begin(), extra.end());
        done[r] = true;
    }
    return built[r];
}

/** The region every lookup uses when none is given. Set once from the owner's own position. */
inline int& defaultRegion() { static int r = 1; return r; }

namespace detail {

inline std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/** "87.5MHz", "7074k", "530000" → Hz. Returns <0 when it is not a number at all. */
inline double parseHz(const std::string& raw) {
    const std::string t = trim(raw);
    if (t.empty()) return -1;
    char* end = nullptr;
    const double v = strtod(t.c_str(), &end);
    if (end == t.c_str() || v < 0) return -1;
    std::string suffix;
    for (const char* q = end; *q; ++q) if (!isspace((unsigned char)*q)) suffix += (char)tolower(*q);
    // ★ A BARE NUMBER IS HERTZ. Guessing "probably kHz because it is small" would make 7074 mean
    //   one thing in one box and another elsewhere; the UI writes units, so ambiguity here is a
    //   choice we do not have to make.
    if (suffix.empty()) return v;
    if (suffix == "hz")  return v;
    if (suffix == "k" || suffix == "khz") return v * 1e3;
    if (suffix == "m" || suffix == "mhz") return v * 1e6;
    if (suffix == "g" || suffix == "ghz") return v * 1e9;
    return -1;
}

}  // namespace detail

/** Parse one entry: a named band id, or "lo-hi" in any unit. Invalid → !valid(). */
inline Range parseEntry(const std::string& raw) {
    const std::string t = detail::trim(raw);
    Range r;
    if (t.empty()) return r;
    // A name first — names cannot contain '-', so there is no ambiguity to resolve.
    std::string lower;
    for (char c : t) lower += (char)tolower((unsigned char)c);
    // ★★ "all" — EVERYWHERE THIS RADIO CAN HEAR. A per-band list with no way to say "everywhere"
    //    forces an owner who wants one overall ceiling to add a rule for every band and to come
    //    back whenever a band is added (Stuart, 2026-08-12). A name rather than "0-4000M" so the
    //    saved config still reads as what was meant, and so no radio can ever out-range it.
    //    ★ It is an ordinary rule, so the LOWEST-WINS merge already does the right thing: an
    //      overall ceiling with a tighter FM rule leaves FM tighter, which is what both mean.
    if (lower == "all") { r.lo = 0.0; r.hi = 1e12; return r; }
    for (const auto& b : namedBands(defaultRegion()))
        if (lower == b.id) { r.lo = b.lo; r.hi = b.hi; return r; }

    const size_t dash = t.find('-');
    if (dash == std::string::npos || dash == 0) return r;
    const double lo = detail::parseHz(t.substr(0, dash));
    const double hi = detail::parseHz(t.substr(dash + 1));
    if (lo < 0 || hi < 0) return r;
    r.lo = std::min(lo, hi);        // ★ tolerate a reversed pair rather than discarding it
    r.hi = std::max(lo, hi);
    return r;
}

/** Parse a comma/newline separated list. Bad entries are skipped, not fatal. */
inline Ranges parseList(const std::string& csv) {
    Ranges out;
    std::string cur;
    for (size_t i = 0; i <= csv.size(); ++i) {
        const char c = i < csv.size() ? csv[i] : ',';
        if (c == ',' || c == '\n' || c == ';') {
            const Range r = parseEntry(cur);
            if (r.valid()) out.push_back(r);
            cur.clear();
        } else cur += c;
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────
//  ★★★ PER-BAND GAIN CEILINGS
// ─────────────────────────────────────────────────────────────────────────────────────────────
//
// ★★★ THE VALUE IS IN THE RADIO'S OWN CONTROL UNITS and this parser does not care what they are.
//     The three radios do not share a gain model — an RTL tuner gain is tenths of a dB from a
//     discrete list, an RSP's is an RF slider POSITION, an Airspy HF+ has no variable gain at all
//     — so nothing here converts or validates the number beyond "it is a number". The config is
//     per radio, a radio has one driver, and the admin sets it while looking at that radio's own
//     control. See BRIEF-admin-gain-limits.md.
//
// ★★ The BAND half reuses parseEntry, so an owner may write a named band — "fm:250" — as well as
//    "88-108:250". Named bands are already ITU-region aware, which a hand-typed pair is not.

struct GainRule {
    Range band;
    int   max = -1;     ///< ceiling in the radio's units; -1 = parsed nothing useful
    bool valid() const { return band.valid() && max >= 0; }
};
using GainRules = std::vector<GainRule>;

/** Parse "fm:250, 0-30M:400" — comma/newline separated, bad entries skipped rather than fatal. */
inline GainRules parseGainList(const std::string& csv) {
    GainRules out;
    std::string cur;
    for (size_t i = 0; i <= csv.size(); ++i) {
        const char c = i < csv.size() ? csv[i] : ',';
        if (c == ',' || c == '\n' || c == ';') {
            // ★ Split on the LAST colon: a band name has none, but "88-108" written as a frequency
            //   pair never contains one either, so the last is unambiguous and tolerates spaces.
            const size_t colon = cur.rfind(':');
            if (colon != std::string::npos) {
                GainRule g;
                g.band = parseEntry(cur.substr(0, colon));
                const std::string v = detail::trim(cur.substr(colon + 1));
                if (!v.empty()) g.max = atoi(v.c_str());
                if (g.valid()) out.push_back(g);
            }
            cur.clear();
        } else cur += c;
    }
    return out;
}

/**
 * The ceiling that applies at a frequency, or -1 for none.
 *
 * ★★ THE LOWEST WINS WHERE RULES OVERLAP. An owner who writes both "fm:250" and a wider
 *    "0-2000M:400" means the tighter one on FM — reading it the other way would let a broad
 *    catch-all silently undo the specific limit they wrote for the band that was overloading.
 */
inline int gainCapAt(const GainRules& rules, double hz) {
    int cap = -1;
    for (const auto& g : rules)
        if (hz >= g.band.lo && hz <= g.band.hi && (cap < 0 || g.max < cap)) cap = g.max;
    return cap;
}

/** Sort and merge touching/overlapping ranges into a canonical set. */
inline Ranges normalise(Ranges in) {
    if (in.empty()) return in;
    std::sort(in.begin(), in.end(), [](const Range& a, const Range& b) { return a.lo < b.lo; });
    Ranges out;
    for (const auto& r : in) {
        if (!r.valid()) continue;
        if (!out.empty() && r.lo <= out.back().hi) out.back().hi = std::max(out.back().hi, r.hi);
        else out.push_back(r);
    }
    return out;
}

inline Ranges intersect(const Ranges& a, const Ranges& b) {
    Ranges out;
    for (const auto& x : a)
        for (const auto& y : b) {
            const double lo = std::max(x.lo, y.lo), hi = std::min(x.hi, y.hi);
            if (hi > lo) out.push_back({ lo, hi });
        }
    return normalise(out);
}

inline Ranges subtract(const Ranges& from, const Ranges& cut) {
    Ranges out = normalise(from);
    for (const auto& c : normalise(cut)) {
        Ranges next;
        for (const auto& r : out) {
            if (c.hi <= r.lo || c.lo >= r.hi) { next.push_back(r); continue; }  // no overlap
            if (c.lo > r.lo) next.push_back({ r.lo, c.lo });                    // left remainder
            if (c.hi < r.hi) next.push_back({ c.hi, r.hi });                    // right remainder
        }
        out = next;
    }
    return normalise(out);
}

/**
 * What listeners may tune, given the hardware's coverage and the owner's two lists.
 *
 * ★ An EMPTY allow list means "everything the hardware can reach" — see the header note. An allow
 *   list that ends up empty after intersection is a mistake the owner can see (the receiver has no
 *   tunable range at all), and is returned as such rather than silently ignored: quietly serving
 *   the whole band because their entry was wrong is the opposite of what they asked for.
 */
/** ★★★ WHAT A RADIO CAN HEAR AT ALL, by driver. A listener deciding whether a receiver is worth
 *      opening wants its COVERAGE — "500 kHz to 1.7 GHz" — not the one frequency it happens to be
 *      parked on. The directory only ever published the latter, so three very different radios
 *      looked interchangeable (Stuart, 2026-08-09).
 *  ★★ These are the same figures the sources enforce, kept here so the FRONT DOOR can answer for a
 *     radio owned by another process — it has the config but never the device.
 *  ★ The HF+'s gap is real hardware, not a limitation we invented: 31-60 MHz does not exist on it,
 *    and a listener who is not told will park there and conclude the receiver is broken. */
inline Ranges driverCoverage(const std::string& driver) {
    if (driver == "airspyhf") return { {500.0, 31.0e6}, {60.0e6, 260.0e6} };
    if (driver == "sdrplay")  return { {1000.0, 2.0e9} };
    if (driver == "rtl" || driver == "rtlsdr") return { {500.0e3, 1.766e9} };
    return {};                                   // unknown: say nothing rather than guess
}

inline Ranges permitted(const Ranges& hardware, const std::string& allowCsv,
                        const std::string& blockCsv) {
    const Ranges hw = normalise(hardware);
    const Ranges allow = parseList(allowCsv);
    const Ranges block = parseList(blockCsv);
    Ranges base = allow.empty() ? hw : intersect(hw, allow);
    return subtract(base, block);
}

/** Serialise as the client's `ranges` wire format: [[lo,hi],[lo,hi]] in Hz. */
inline std::string toJson(const Ranges& r) {
    std::string j = "[";
    for (size_t i = 0; i < r.size(); ++i) {
        char b[96];
        snprintf(b, sizeof b, "%s[%lld,%lld]", i ? "," : "",
                 (long long)(r[i].lo + 0.5), (long long)(r[i].hi + 0.5));
        j += b;
    }
    return j + "]";
}

/** ★★★ NAME THE BANDS A LISTENER IS ACTUALLY ALLOWED INTO, rather than reciting the hardware's
 *  reach. A card reading "1 kHz – 31 MHz, 60 MHz – 260 MHz · RESTRICTIONS IN PLACE" describes a
 *  receiver nobody can use as advertised; "AM (medium wave) broadcast, FM broadcast" describes the
 *  one that is actually there (Stuart, 2026-08-20).
 *
 *  ★★ A range is named when it MATCHES a band in the plan closely enough to be that band — within
 *     `tol` at each edge, so an owner who typed 87.5–108.0 and one who typed the band id `fm` get
 *     the same words. An owner's arbitrary slice has no name and keeps its numbers, which is the
 *     honest answer: inventing "part of FM broadcast" would be worse than the figures.
 *  ★ Region-aware, because the plan is: medium wave stops at 1606.5 kHz in Region 1 and 1705 in
 *    Region 2, and the label follows whichever this server is in. */
inline std::string bandLabel(const Range& r, int region, double tol = 50000.0) {
    for (const auto& b : namedBands(region))
        if (std::fabs(r.lo - b.lo) <= tol && std::fabs(r.hi - b.hi) <= tol) return b.label;
    return std::string();
}

/** Labels for a permitted set, as a JSON array. Unnamed ranges are omitted — the caller still has
 *  the numbers, and a half-named list ("FM broadcast, 4.1–4.9 MHz") is for the caller to compose. */
inline std::string labelsJson(const Ranges& rs, int region) {
    std::string j = "[";
    bool first = true;
    for (const auto& r : rs) {
        const std::string l = bandLabel(r, region);
        if (l.empty()) continue;
        if (!first) j += ',';
        first = false;
        j += '"';
        for (char c : l) { if (c == '"' || c == '\\') j += '\\'; j += c; }
        j += '"';
    }
    return j + "]";
}

/** True when `hz` falls inside the permitted set (empty set = nothing is permitted). */
inline bool allows(const Ranges& r, double hz) {
    for (const auto& x : r) if (hz >= x.lo && hz <= x.hi) return true;
    return false;
}

/**
 * Nearest permitted frequency to `hz`, preferring the direction of travel.
 * ★ The SERVER's job is to land somewhere legal, not to be clever: the CLIENT does the
 *   bounce-then-jump that makes tuning feel right (clampTune). This exists so a hand-rolled client
 *   cannot simply ask for a blocked frequency and be given it.
 */
inline double clamp(const Ranges& r, double hz) {
    if (r.empty()) return hz;
    if (allows(r, hz)) return hz;
    double best = r[0].lo, bestD = 1e18;
    for (const auto& x : r) {
        for (const double edge : { x.lo, x.hi }) {
            const double d = std::abs(edge - hz);
            if (d < bestD) { bestD = d; best = edge; }
        }
    }
    return best;
}

}  // namespace vibebands
