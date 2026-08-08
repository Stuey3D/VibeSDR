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

/** ★ Named bands, so an owner can say "VHF airband" instead of looking up 108–137 MHz and getting
 *  it slightly wrong. Region 1 (UK/Europe) where the allocations differ — this server is written
 *  and operated there, and a band edge that is right for the wrong continent is worse than no
 *  preset at all. A typed range always overrides a name. */
struct NamedBand { const char* id; const char* label; double lo, hi; };

inline const std::vector<NamedBand>& namedBands() {
    static const std::vector<NamedBand> kBands = {
        { "lw",       "Long wave broadcast",   148500.0,     283500.0 },
        { "mw",       "AM (medium wave) broadcast", 526500.0, 1606500.0 },
        { "160m",     "160 m amateur",        1810000.0,    2000000.0 },
        { "120m",     "120 m broadcast",      2300000.0,    2495000.0 },
        { "90m",      "90 m broadcast",       3200000.0,    3400000.0 },
        { "80m",      "80 m amateur",         3500000.0,    3800000.0 },
        { "75m",      "75 m broadcast",       3900000.0,    4000000.0 },
        { "60mb",     "60 m broadcast",       4750000.0,    5060000.0 },
        { "60m",      "60 m amateur",         5258500.0,    5406500.0 },
        { "49m",      "49 m broadcast",       5900000.0,    6200000.0 },
        { "40m",      "40 m amateur",         7000000.0,    7200000.0 },
        { "41m",      "41 m broadcast",       7200000.0,    7450000.0 },
        { "31m",      "31 m broadcast",       9400000.0,    9900000.0 },
        { "30m",      "30 m amateur",        10100000.0,   10150000.0 },
        { "25m",      "25 m broadcast",      11600000.0,   12100000.0 },
        { "22m",      "22 m broadcast",      13570000.0,   13870000.0 },
        { "20m",      "20 m amateur",        14000000.0,   14350000.0 },
        { "19m",      "19 m broadcast",      15100000.0,   15800000.0 },
        { "17m",      "17 m amateur",        18068000.0,   18168000.0 },
        { "16m",      "16 m broadcast",      17480000.0,   17900000.0 },
        { "15m",      "15 m amateur",        21000000.0,   21450000.0 },
        { "13m",      "13 m broadcast",      21450000.0,   21850000.0 },
        { "12m",      "12 m amateur",        24890000.0,   24990000.0 },
        { "11m",      "11 m broadcast",      25670000.0,   26100000.0 },
        { "cb",       "CB (27 MHz)",         26965000.0,   27405000.0 },
        { "10m",      "10 m amateur",        28000000.0,   29700000.0 },
        { "6m",       "6 m amateur",         50000000.0,   52000000.0 },
        { "fm",       "FM broadcast",        87500000.0,  108000000.0 },
        { "air",      "VHF airband",        108000000.0,  137000000.0 },
        { "2m",       "2 m amateur",        144000000.0,  146000000.0 },
        { "marine",   "Marine VHF",         156000000.0,  162050000.0 },
        { "dab",      "DAB (Band III)",     174000000.0,  240000000.0 },
        { "70cm",     "70 cm amateur",      430000000.0,  440000000.0 },
        { "pmr446",   "PMR446",             446000000.0,  446200000.0 },
    };
    return kBands;
}

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
    for (const auto& b : namedBands())
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
