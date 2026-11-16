// tuned_label.h — THE TUNED STATION'S LABEL AT THE VFO: a line-for-line port of src/utils/tunedStation.ts (the ONE module
// the app's WaterfallView and the web client's waterfall share), plus wfStamps.ts stampColour (the colour it is drawn in).
// Pure — no SDL, no GL — so test/tuned_label_test.cpp replays gen_tuned_label_golden.ts's lines (the TS itself) exactly.
// The R36S's plumbing and drawing are tuned_label_app.h.
//
// ★★★ STUART (2026-11-15): "Right now the VTS shows the station name then disappears unless it is DAB/DRM/RDS. When the
//     VTS disappears can we have a label on the VFO with the current station name. We don't show EiBi bookmarks on the
//     spectrum as they would swamp it, but we could have the current tuned one shown."
//         [EiBi] China Radio Int     [Server] Radio Caroline     [User] Flex FM
//  ★ WHICH: the listener's own (the handheld: the rows its owner SAVED on it) → the server's (the stations this receiver
//    HEARD) → EiBi on air now; the nearest to the dial within a source; RECEIVED: inside the receive passband (the
//    demodulator's filter, else a mode default, never past ±100 kHz) or on the dial (±99 Hz, the VTS's own). A DAB service
//    never (its frequency is its block's). WHEN: the dial has rested, the VTS has let the name go, no live RDS/DAB/DRM name.
//  ★★ A SUPPLEMENT, NEVER A REPEAT: when the spectrum's own SIGNAL LABELS already name the station, no tuned label (and no
//     other name in its place) — that label is HIGHLIGHTED instead, drawn in the tuned colour (tunedPick).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hh {
namespace tl {

/** One candidate (TunedRow). `source`: "user" · "server" · "eibi" — anything else is skipped. */
struct Row { double frequency = 0; std::string name, source, comment, mode; };
struct Station { std::string source, name; double hz = 0; bool ok = false; };
/** A spectrum label drawn now (known_labels.h PlacedLabel's sig): marker, dial, source. */
struct Shown { double hz = 0, dialHz = 0; std::string src; };
/** The receive passband as offsets from the dial (Hz). */
struct Passband { double lo = -99, hi = 99; };

constexpr double kTolHz = 99;        // TUNED_TOL_HZ = stations.ts VTS_ON_HZ (vts_core.h kVtsOnHz)
constexpr double kSettleMs = 600;    // TUNED_SETTLE_MS
constexpr size_t kMaxChars = 28;     // TUNED_MAX_CHARS
constexpr int kHueClash = 35;        // wfStamps.ts STAMP_HUE_CLASH
constexpr double kMaxHalfHz = 100000; // TUNED_MAX_HALF_HZ

inline size_t cpLen(const std::string& s) { size_t n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++; return n; }
inline std::string cpHead(const std::string& s, size_t n) {
    size_t i = 0, k = 0;
    while (i < s.size()) { if (((unsigned char)s[i] & 0xC0) != 0x80) { if (k == n) break; k++; } i++; }
    return s.substr(0, i);
}
/** JS String.trim for the names here: ASCII white space (the clients' names are cleaned on the way in). */
inline std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

/** knownSignals.ts eibiOnAir (as known_labels.h): the first "HHMM-HHMM" covers `nowMin`; no time = on air. */
inline bool onAir(const std::string& c, int nowMin) {
    for (size_t p = 0; p + 9 <= c.size(); p++) {
        bool ok = c[p + 4] == '-';
        for (int k : {0, 1, 2, 3, 5, 6, 7, 8}) ok = ok && std::isdigit((unsigned char)c[p + k]);
        if (!ok) continue;
        auto d = [&](size_t i) { return (c[i] - '0') * 10 + (c[i + 1] - '0'); };
        const int s = d(p) * 60 + d(p + 2), e = d(p + 5) * 60 + d(p + 7);
        if (s == e) return true;
        return s < e ? nowMin >= s && nowMin < e : nowMin >= s || nowMin < e;
    }
    return true;
}

/** tunedPassband: the demodulator's own when sane (low < high), else the mode's default; clamped to ±kMaxHalfHz. */
inline Passband tunedPassband(const std::string& modeIn, double bwLow, double bwHigh, bool known = true) {
    auto clamp = [](double v) { return std::max(-kMaxHalfHz, std::min(kMaxHalfHz, v)); };
    if (known && std::isfinite(bwLow) && std::isfinite(bwHigh) && bwHigh > bwLow) return {clamp(bwLow), clamp(bwHigh)};
    std::string m = modeIn;
    for (auto& ch : m) ch = (char)std::tolower((unsigned char)ch);
    Passband p;
    if (m == "wfm") p = {-75000, 75000};
    else if (m == "nfm" || m == "fm") p = {-6000, 6000};
    else if (m == "usb" || m == "ecssu") p = {0, 3000};
    else if (m == "lsb" || m == "ecssl") p = {-3000, 0};
    else if (m == "cw" || m == "cwu" || m == "cwl") p = {-300, 300};
    else if (m == "am" || m == "sam" || m == "sau" || m == "sal" || m == "amst" || m == "drm") p = {-5000, 5000};
    else p = {-1500, 1500};
    return {clamp(p.lo), clamp(p.hi)};
}
/** inPassband: inside it, or on the dial (±kTolHz). */
inline bool inPassband(double hz, double dialHz, const Passband& pb) {
    const double off = hz - dialHz;
    return std::fabs(off) <= kTolHz || (off >= pb.lo && off <= pb.hi);
}
/** markerIndex: the drawn label naming this station (a bookmark by a personal / server / learnt label, an EiBi row by an
 *  EiBi label, by marker or dial within kTolHz) — the nearest, the first on a tie; −1 = none. */
inline int markerIndex(double frequency, const std::string& source, const std::vector<Shown>* shown) {
    if (!shown) return -1;
    const bool eibi = source == "eibi";
    int best = -1; double bestD = INFINITY;
    for (size_t i = 0; i < shown->size(); i++) {
        const Shown& m = (*shown)[i];
        const bool mine = eibi ? m.src == "eibi" : m.src == "personal" || m.src == "server" || m.src == "learnt";
        if (!mine) continue;
        const double d = std::min(std::fabs(m.hz - frequency), std::fabs(m.dialHz - frequency));
        if (d <= kTolHz && d < bestD) { best = (int)i; bestD = d; }
    }
    return best;
}

inline int rankOf(const std::string& s) { return s == "user" ? 0 : s == "server" ? 1 : s == "eibi" ? 2 : -1; }

/** tunedStation: the best-ranked source within `tol` (user → server → EiBi), the nearest within it, the first on a tie. */
inline Station tunedStation(const std::vector<Row>& rows, double dialHz, int nowMin, const Passband& pb = Passband{}) {
    Station best;
    if (!(dialHz > 0)) return best;
    int bestRank = 9; double bestOff = INFINITY;
    for (const Row& r : rows) {
        const int rank = rankOf(r.source);
        if (rank < 0 || !(r.frequency > 0) || r.mode == "dab") continue;
        const double off = std::fabs(r.frequency - dialHz);
        if (!inPassband(r.frequency, dialHz, pb)) continue;
        const std::string name = trim(r.name);
        if (name.empty()) continue;
        if (rank > bestRank || (rank == bestRank && off >= bestOff)) continue;
        if (rank == 2 && !onAir(r.comment, nowMin)) continue;
        best.source = r.source; best.name = name; best.hz = r.frequency; best.ok = true;
        bestRank = rank; bestOff = off;
    }
    return best;
}

/** tunedLabelText: "[EiBi] China Radio Int", the name cut to kMaxChars code points with "…". */
inline std::string labelText(const Station& st) {
    const std::string tag = st.source == "user" ? "[User]" : st.source == "server" ? "[Server]" : "[EiBi]";
    std::string name = st.name;
    if (cpLen(name) > kMaxChars) {
        name = cpHead(name, kMaxChars - 1);
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
        name += "\xE2\x80\xA6";
    }
    return tag + " " + name;
}

/** tunedPick: the tuned label's text, OR the index into `shown` of the drawn label that names the station — never both. */
struct Pick { std::string label; int highlight = -1; };
inline Pick tunedPick(const std::vector<Row>& rows, double dialHz, int nowMin, const std::string& liveName,
                      bool vtsShowing, bool settled, const Passband& pb = Passband{}, const std::vector<Shown>* shown = nullptr) {
    Pick p;
    if (!settled || vtsShowing || !trim(liveName).empty()) return p;
    const Station t = tunedStation(rows, dialHz, nowMin, pb);
    if (!t.ok) return p;
    p.highlight = markerIndex(t.hz, t.source, shown);
    if (p.highlight < 0) p.label = labelText(t);
    return p;
}
/** tunedLabel: "" = no label. */
inline std::string tunedLabel(const std::vector<Row>& rows, double dialHz, int nowMin, const std::string& liveName,
                              bool vtsShowing, bool settled, const Passband& pb = Passband{},
                              const std::vector<Shown>* shown = nullptr) {
    return tunedPick(rows, dialHz, nowMin, liveName, vtsShowing, settled, pb, shown).label;
}

/** tunedLabelX: the box's left edge — CENTRED on the needle (Stuart, 2026-11-16: "centre the frequency and bookmark
 *  reading on the VFO"), shifted inward near an edge; kept on screen. */
inline double labelX(double needleX, double boxW, double width, double margin = 2) {
    return std::max(margin, std::min(width - margin - boxW, needleX - boxW / 2));
}

/** wfStamps.ts stampColour: the needle colour (0xRRGGBB), or white when its hue is within kHueClash° of the palette's at
 *  LUT 96 / 160 / 224 and both are saturated. `lut` = 256 × RGBA, or null. */
inline unsigned stampColour(unsigned needle, const unsigned char* lut) {
    auto hs = [](double r, double g, double b, double& hue, double& sat) {
        const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
        if (mx <= 0 || d == 0) { hue = 0; sat = 0; return; }
        const double h = mx == r ? std::fmod((g - b) / d + 6, 6.0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        hue = h * 60; sat = d / mx;
    };
    double nh, ns;
    hs((needle >> 16) & 255, (needle >> 8) & 255, needle & 255, nh, ns);
    if (lut && ns > 0.3) {
        for (int i : {96, 160, 224}) {
            double ph, ps;
            hs(lut[i * 4], lut[i * 4 + 1], lut[i * 4 + 2], ph, ps);
            const double dh = std::min(std::fabs(ph - nh), 360 - std::fabs(ph - nh));
            if (ps > 0.3 && dh < kHueClash) return 0xffffff;
        }
    }
    return needle & 0xffffff;
}

}  // namespace tl
}  // namespace hh
