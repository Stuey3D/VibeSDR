// known_labels.h — the SIGNAL LABELS on the spectrum: which decoder a well-known signal in view wants, and the names of
// the receiver's saved and learnt stations. A port of src/utils/knownSignals.ts (the ONE module the app's
// WaterfallView.tsx and the web client's waterfall.ts share) and of the web's drawing (waterfall.ts _drawKnown), so the
// R36S shows what the app shows. Golden-tested against the TS: test/known_labels_test.cpp + gen_known_labels_golden.ts.
//
// ★★★ WHAT FEEDS THEM (as the app — Stuart, 2026-10-30: "this is more for a user to know what decoder to use, the VTS
//     shows the bookmarks"). A STATIC list, never a guess from the signal:
//      • the CURATED TABLE (known_signals_gen.h, generated from KNOWN_SIGNALS): DWD RTTY, DWD + Northwood WEFAX, NAVTEX,
//        DSC, the FT8/FT4/WSPR/JS8/Q65 dials, Shannon VOLMET, the time stations. Compiled in — works with no internet,
//        no EiBi, nothing on the card.
//      • EiBi's UTILITY rows on air now (eibiKnownSignals) from the engine's GET /stations — on the R36S that is the
//        schedule the owner put on the card (EiBi folder → IMPORT EIBI); without one there are simply none.
//      • the receiver's own list (GET /bookmarks): the owner's saved rows ("manual": true) and FM names this receiver
//        LEARNT from RDS ("manual": false, settled nameSrc ≥ 2). EiBi's broadcast rows are never labels.
//
// ★★ THE R36S's DIFFERENCES (Stuart, 2026-11-12: "the text doesnt have to be massive, and we can limit the amount of
//    markers … due to the screen size"): at most HH_LABEL_MAX (8, the app's 12) on the 640-px spectrum, in the UI's
//    smallest face (Fonts::tiny, already in the glyph atlas); the dial's NEEDLE and the dB axis are blocked boxes, so a
//    label is never drawn over where you are tuned (the web blocks its LISTEN readout the same way). No tap — the R36S
//    has no touch screen.
// ★ COST: labelLayout runs only when the view, the dial, the spectrum rect or the list changes (KnownLabels::layout's
//   key); a frame that changes none of them draws ≤ 8 × (tick + box + its glyphs) into the one batch. The list itself is
//   rebuilt only when the bookmarks, the schedule or the ten-minute EiBi clock change (known_labels_app.h).
#pragma once
#include "gfx.h"
#include "json.h"
#include "known_signals_gen.h"
#include "text.h"
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hh {

// ★ THE LABELS' COLOURS — knownSignals.ts KNOWN_LABEL_COLOUR / KNOWN_LABEL_BOX: a fixed pale lilac on RF CENTRE's dark
//   box. Not the VFO's (a second needle), the scale's amber, the band plan's or the meter's green — each means something.
namespace th {
inline const Rgba knownLabel = rgb(0xd9b8ff);
inline const Rgba knownBox   = rgb(0x000000, 0.62f);
// ★★ V12 knownSignals.ts DX_LABEL_COLOUR — a DX catch's label and line (lemon: not the lilac, the VFO's, DAB's)
inline const Rgba dxLabel    = rgb(0xffe45c);
}  // namespace th

// ── the list ─────────────────────────────────────────────────────────────────────────────────────────────────────────

struct KnownSig {
    double hz = 0, lo = -1, hi = -1;   // marker; the occupied span (-1 = none)
    std::string label, dec, mode;
    double dialHz = 0;
    int prio = 0;
    std::string src;                   // table · eibi · server · learnt · dx (personal: the handheld keeps no own list)
    // ★★ V12 a DX catch (dxDialSignals): when it was last heard (unix s) — drawn in th::dxLabel
    bool dx = false;
    double heard = 0;
};

constexpr int PRIO_SAFETY = 7, PRIO_TABLE = 6, PRIO_ROUTINE = 5, PRIO_PERSONAL = 4, PRIO_SERVER = 3, PRIO_LEARNT = 2,
              PRIO_EIBI = 1;
/** The app's cap (knownSignals.ts LABEL_MAX) and the R36S's — 640 px, and a Mali-G31. */
constexpr int LABEL_MAX = 12, HH_LABEL_MAX = 8;
constexpr int LABEL_ROWS = 2;
/** knownSignals.ts LABEL_ROWS_MAX / LABEL_MIN_CHARS: the most rows labels stack on; the fewest characters a shortened one keeps. */
constexpr int LABEL_ROWS_MAX = 4, LABEL_MIN_CHARS = 3;
constexpr double LABEL_GAP = 6;
constexpr int BOOKMARK_LABEL_MAX = 16;

namespace kl {
inline bool isW(unsigned char c) { return std::isalnum(c) || c == '_'; }   // JS \w (ASCII only, as JS without /u)
inline char lc(char c) { return (char)std::tolower((unsigned char)c); }
/** Case-blind `\bword\b` (word in lower case). */
inline bool hasWord(const std::string& s, const char* w) {
    const size_t n = std::strlen(w);
    for (size_t p = 0; p + n <= s.size(); p++) {
        size_t k = 0;
        while (k < n && lc(s[p + k]) == w[k]) k++;
        if (k < n) continue;
        if ((p == 0 || !isW(s[p - 1])) && (p + n == s.size() || !isW(s[p + n]))) return true;
    }
    return false;
}
/** /\b(meteo|met|wx)?\s*fax\b/i — "fax" ending a word, either starting one or straight after meteo/met/wx that does
 *  (a gap of spaces before "fax" makes "fax" start a word anyway). */
inline bool hasFax(const std::string& s) {
    for (size_t p = 0; p + 3 <= s.size(); p++) {
        if (!(lc(s[p]) == 'f' && lc(s[p + 1]) == 'a' && lc(s[p + 2]) == 'x')) continue;
        if (p + 3 < s.size() && isW(s[p + 3])) continue;
        if (p == 0 || !isW(s[p - 1])) return true;
        for (const char* pre : {"meteo", "met", "wx"}) {
            const size_t n = std::strlen(pre);
            if (p < n) continue;
            size_t a = p - n, k = 0;
            while (k < n && lc(s[a + k]) == pre[k]) k++;
            if (k == n && (a == 0 || !isW(s[a - 1]))) return true;
        }
    }
    return false;
}
/** UTF-8 code points (a JS string's length for the BMP). */
inline size_t cpLen(const std::string& s) { size_t n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++; return n; }
/** The first `n` code points. */
inline std::string cpHead(const std::string& s, size_t n) {
    size_t i = 0, k = 0;
    while (i < s.size()) { if (((unsigned char)s[i] & 0xC0) != 0x80) { if (k == n) break; k++; } i++; }
    return s.substr(0, i);
}
inline std::string trimEnd(std::string s) { while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back(); return s; }
inline std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> o; std::string w;
    for (char c : s) { if (std::isspace((unsigned char)c)) { if (!w.empty()) o.push_back(w), w.clear(); } else w += c; }
    if (!w.empty()) o.push_back(w);
    return o;
}
}  // namespace kl

/** knownSignals.ts eibiOnAir: "HHMM-HHMM" covers `nowMin` (minutes past 00:00 UTC); no time = on air. */
inline bool eibiOnAir(const std::string& c, int nowMin) {
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

/** knownSignals.ts eibiPlace: a short place from an EiBi name ("GYA Northwood Meteo Fax" → "Northwood"), ≤ 14. Runs
 *  only on the utility rows on air (a few dozen), so std::regex is fine here — never on the whole schedule. */
inline std::string eibiPlace(const std::string& name) {
    using std::regex;
    static const regex paren(R"(\([^)]*\))");
    static const regex words(R"(\b(meteo|met|wx)?\s*fax\b|\bhfdl\b|\bvolmet\b|\brtty\b|\bnavtex\b|\bradio\b|\btime signal\b)",
                             regex::ECMAScript | regex::icase);
    static const regex xcodes(R"(\bx\d{2}\b,?\s*(x?\d{2}\b)?)", regex::ECMAScript | regex::icase);
    static const regex punct(R"([,;])");
    std::string s = std::regex_replace(name, paren, " ");
    s = std::regex_replace(s, words, " ");
    s = std::regex_replace(s, xcodes, " ");
    s = std::regex_replace(s, punct, " ");
    std::vector<std::string> w = kl::splitWs(s);
    auto callSign = [](const std::string& x) {
        if (x.size() < 3 || x.size() > 6) return false;
        bool letter = false;
        for (char ch : x) { if (!(std::isdigit((unsigned char)ch) || (ch >= 'A' && ch <= 'Z'))) return false; if (ch >= 'A' && ch <= 'Z') letter = true; }
        return letter;
    };
    if (w.size() > 1 && callSign(w[0])) w.erase(w.begin());
    std::string o;
    for (auto& x : w) {
        if (std::all_of(x.begin(), x.end(), [](char ch) { return std::isdigit((unsigned char)ch); })) continue;
        if (!o.empty()) o += ' ';
        o += x;
    }
    return kl::cpLen(o) > 14 ? kl::trimEnd(kl::cpHead(o, 13)) + "…" : o;
}

struct EibiClass { const char* dec = nullptr; const char* kind = ""; double af = 0; bool voice = false; };
/** knownSignals.ts eibiClass — dec == nullptr: a broadcast (no label). EiBi's language-column codes, then the name. */
inline EibiClass eibiClass(const std::string& name, const std::string& comment) {
    bool hf = false, ty = false, cw = false;
    {
        std::string t;
        auto flush = [&] { if (t == "-HF") hf = true; else if (t == "-TY") ty = true; else if (t == "-CW") cw = true; t.clear(); };
        for (size_t i = 0; i < comment.size(); i++) {
            const unsigned char ch = (unsigned char)comment[i];
            if (ch == 0xC2 && i + 1 < comment.size() && (unsigned char)comment[i + 1] == 0xB7) { flush(); i++; continue; }   // ·
            if (std::isspace(ch) || ch == ',' || ch == '/' || ch == ';') { flush(); continue; }
            t += (char)std::toupper(ch);
        }
        flush();
    }
    if (hf || kl::hasWord(name, "hfdl")) return {"hfdl", "HFDL", 1440};
    if (kl::hasFax(name)) return {"wefax", "WEFAX", 1900};
    if (kl::hasWord(name, "navtex")) return {"navtex", "NAVTEX", 500};
    if (ty || kl::hasWord(name, "rtty")) return {"rtty", "RTTY", 1000};
    if (cw) return {"cw", "CW", 0};
    if (kl::hasWord(name, "volmet")) return {"", "VOLMET", 1500, true};
    return {};
}

/** The curated table as KnownSigs (made once). */
inline const std::vector<KnownSig>& knownTable() {
    static const std::vector<KnownSig> t = [] {
        std::vector<KnownSig> v;
        v.reserve(kKnownSignalsN);
        for (int i = 0; i < kKnownSignalsN; i++) {
            const KnownSigDef& d = kKnownSignals[i];
            KnownSig s;
            s.hz = d.hz; s.lo = d.lo; s.hi = d.hi; s.label = d.label; s.dec = d.dec; s.dialHz = d.dialHz; s.mode = d.mode;
            s.prio = d.prio; s.src = "table";
            v.push_back(std::move(s));
        }
        return v;
    }();
    return t;
}

struct EibiIn { std::string name, comment; double frequency = 0; };
/** knownSignals.ts eibiKnownSignals: EiBi's utility entries on air at `nowMin`. One label a signal: within 1 kHz of a
 *  curated one, or of an earlier EiBi one of the same kind, is dropped. (Every /stations row from a VibeServer is the
 *  schedule — vibeserver/eibi.cpp tags each "source":"eibi" — so the TS's source check has nothing to drop here.) */
inline std::vector<KnownSig> eibiKnownSignals(const std::vector<EibiIn>& rows, int nowMin,
                                              const std::vector<KnownSig>& curated = knownTable()) {
    std::vector<KnownSig> out;
    for (const EibiIn& r : rows) {
        if (!(r.frequency > 0)) continue;
        const EibiClass c = eibiClass(r.name, r.comment);
        if (!c.dec || !eibiOnAir(r.comment, nowMin)) continue;
        const double f = r.frequency;
        const bool dialIsListed = std::strcmp(c.dec, "hfdl") == 0 || c.voice;
        const double hz = dialIsListed ? f + c.af : f, dialHz = dialIsListed ? f : f - c.af;
        bool dup = false;
        for (const KnownSig& k : curated) if (std::fabs(k.hz - hz) <= 1000) { dup = true; break; }
        const size_t kn = std::strlen(c.kind);
        for (size_t i = 0; !dup && i < out.size(); i++)
            if (out[i].label.compare(0, kn, c.kind) == 0 && std::fabs(out[i].hz - hz) <= 1000) dup = true;
        if (dup) continue;
        const std::string place = eibiPlace(r.name);
        KnownSig s;
        s.hz = hz; s.label = place.empty() ? c.kind : std::string(c.kind) + " · " + place; s.dec = c.dec; s.dialHz = dialHz;
        s.mode = std::strcmp(c.dec, "cw") == 0 ? "cwu" : "usb"; s.prio = PRIO_EIBI; s.src = "eibi";
        if (c.voice) { s.lo = f + 300; s.hi = f + 2700; }
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const KnownSig& a, const KnownSig& b) { return a.hz < b.hz; });
    return out;
}

/** knownSignals.ts bookmarkLabel: whitespace collapsed, ≤ BOOKMARK_LABEL_MAX (an ellipsis included). */
inline std::string bookmarkLabel(const std::string& name) {
    std::string s;
    for (const std::string& w : kl::splitWs(name)) { if (!s.empty()) s += ' '; s += w; }
    return (int)kl::cpLen(s) > BOOKMARK_LABEL_MAX ? kl::trimEnd(kl::cpHead(s, BOOKMARK_LABEL_MAX - 1)) + "…" : s;
}

struct BookmarkIn {
    std::string name, mode;
    double frequency = 0;
    int manual = -1;      // 1 true · 0 false · -1 not sent
    int nameSrc = -1;     // -1 = not sent (the TS reads that as 2)
    int sid = -1;         // ≥ 0 = a DAB service
    bool repeater = false;
};
/** knownSignals.ts isLearntFm: a settled name this receiver learnt for an FM broadcast channel. */
inline bool isLearntFm(const BookmarkIn& b) {
    if (b.manual != 0) return false;
    if (!(b.frequency >= 76e6 && b.frequency <= 108e6)) return false;
    if (!b.mode.empty()) { std::string m = b.mode; for (auto& ch : m) ch = kl::lc(ch); if (m != "wfm") return false; }
    if (b.sid >= 0) return false;
    const int src = b.nameSrc >= 0 ? b.nameSrc : 2;
    if (src < 2) return false;
    // /^PI[0-9A-F]{4}\b/i — a provisional "PI4322 93.7MHz"
    const std::string& n = b.name;
    if (n.size() >= 6 && kl::lc(n[0]) == 'p' && kl::lc(n[1]) == 'i' && std::all_of(n.begin() + 2, n.begin() + 6, [](char ch) { return std::isxdigit((unsigned char)ch); })
        && (n.size() == 6 || !kl::isW(n[6]))) return false;
    return true;
}
/** knownSignals.ts bookmarkSignals, for the SERVER's list (the handheld has no list of its own — `personal` is empty;
 *  unmarkedIsSaved false, as the web: a VibeServer always says "manual"). One label a frequency (±1 kHz), the higher
 *  priority kept. */
inline std::vector<KnownSig> bookmarkSignals(const std::vector<BookmarkIn>& server, bool unmarkedIsSaved = false) {
    std::vector<KnownSig> cand;
    auto add = [&](const BookmarkIn& b, int prio, const char* src) {
        if (!(b.frequency > 0) || !std::isfinite(b.frequency)) return;
        std::string label = bookmarkLabel(b.name);
        if (label.empty()) return;
        std::string m = b.mode.empty() ? "am" : b.mode;
        for (auto& ch : m) ch = kl::lc(ch);
        KnownSig s;
        s.hz = std::round(b.frequency); s.label = std::move(label); s.dec = ""; s.dialHz = s.hz; s.mode = m; s.prio = prio; s.src = src;
        cand.push_back(std::move(s));
    };
    for (const BookmarkIn& b : server) {
        if (b.repeater) continue;
        if (b.manual == 1 || (b.manual == -1 && unmarkedIsSaved)) add(b, PRIO_SERVER, "server");
        else if (isLearntFm(b)) add(b, PRIO_LEARNT, "learnt");
    }
    std::stable_sort(cand.begin(), cand.end(), [](const KnownSig& a, const KnownSig& b) { return a.prio != b.prio ? a.prio > b.prio : a.hz < b.hz; });
    std::vector<KnownSig> out;
    for (KnownSig& c : cand) {
        bool near = false;
        for (const KnownSig& o : out) if (std::fabs(o.hz - c.hz) <= 1000) { near = true; break; }
        if (!near) out.push_back(std::move(c));
    }
    std::stable_sort(out.begin(), out.end(), [](const KnownSig& a, const KnownSig& b) { return a.hz < b.hz; });
    return out;
}

/** knownSignals.ts knownSignalList: the table + EiBi's + the bookmarks + the DX catches, one list sorted by hz (stable). */
inline std::vector<KnownSig> knownSignalList(const std::vector<KnownSig>& eibi, const std::vector<KnownSig>& bms,
                                             const std::vector<KnownSig>& dx = {}) {
    std::vector<KnownSig> v = knownTable();
    v.insert(v.end(), eibi.begin(), eibi.end());
    v.insert(v.end(), bms.begin(), bms.end());
    v.insert(v.end(), dx.begin(), dx.end());
    std::stable_sort(v.begin(), v.end(), [](const KnownSig& a, const KnownSig& b) { return a.hz < b.hz; });
    return v;
}

// ── ★★ V12 DX catches on the dial (knownSignals.ts dxDialSignals — Stuart, 2026-11-16) ─────────────────────────────────
// The engine's DX Log (GET /vibeserver/dxlog.json?dial=1): class "dx" only, a WEEK from last heard, a bookmark or a
// known-signal label on the same channel wins, one a channel (the most recently heard), PRIO_DX below every bookmark,
// "RNE 1 · DX · 2 h ago", OIRT to two decimals. ★ The R36S has no italic face in its atlas (Atkinson Hyperlegible
// Regular/Bold only), so a DX label is marked by its colour (th::dxLabel) and the "DX" tag — not slanted.
// ★ A DAB catch needs the server's freqKhz (it always sends the block's centre); the TS's Band III fallback is not here.
constexpr int PRIO_DX = 0;
constexpr double DX_DIAL_MAX_AGE_S = 7 * 86400;
constexpr int DX_DIAL_MAX = 100;

struct DxDialIn {
    std::string band, cls, range, pi, ps, block, eid, ensemble;
    double freqKhz = 0, lastHeard = 0;
    bool rds = true;
};
/** dxDialAgo: "just now", "12 min ago", "2 h ago", "1 day ago", "3 days ago". */
inline std::string dxDialAgo(double heard, double nowS) {
    const double s = std::max(0.0, nowS - heard);
    if (s < 120) return "just now";
    if (s < 3600) return std::to_string((long long)std::floor(s / 60)) + " min ago";
    if (s < 86400) return std::to_string((long long)std::floor(s / 3600)) + " h ago";
    const long long d = (long long)std::floor(s / 86400);
    return std::to_string(d) + (d == 1 ? " day ago" : " days ago");
}
/** dxFmChannel: "98.8"; OIRT and any channel off the 100 kHz raster to two decimals ("69.80", "86.65"). */
inline std::string dxFmChannel(double khz, const std::string& range) {
    const long long k = (long long)std::llround(khz);
    const bool oirt = range == "oirt" || (range.empty() && k > 0 && k < 75000);
    char b[32];
    std::snprintf(b, sizeof b, oirt || k % 100 ? "%.2f" : "%.1f", khz / 1000.0);
    return b;
}
inline std::string dxTidy(const std::string& s) {
    std::string o;
    for (const std::string& w : kl::splitWs(s)) { if (!o.empty()) o += ' '; o += w; }
    return o;
}
/** dxDialName: FM — the PS, else the PI, else (no RDS) the channel; DAB — the block and the ensemble. */
inline std::string dxDialName(const DxDialIn& e) {
    if (e.band == "DAB") {
        const std::string ens = dxTidy(e.ensemble);
        const std::string n = !ens.empty() ? ens : (!e.eid.empty() ? "EId " + e.eid : "");
        return e.block.empty() ? n : n.empty() ? e.block : e.block + " " + n;
    }
    const std::string ps = dxTidy(e.ps);
    if (!ps.empty()) return ps;
    if (e.rds && !e.pi.empty() && e.pi != "0000") return "PI " + e.pi;
    return dxFmChannel(e.freqKhz, e.range);
}
inline std::string dxDialLabel(const DxDialIn& e, double nowS) {
    return bookmarkLabel(dxDialName(e)) + " \xc2\xb7 DX \xc2\xb7 " + dxDialAgo(e.lastHeard, nowS);
}
/** knownSignals.ts dxDialSignals. `others` = the labels already in force, `bmHz` = every bookmark's frequency. */
inline std::vector<KnownSig> dxDialSignals(std::vector<DxDialIn> entries, double nowS, const std::vector<KnownSig>& others,
                                           const std::vector<double>& bmHz) {
    std::vector<KnownSig> out;
    std::vector<DxDialIn> cand;
    for (DxDialIn& e : entries)
        if (e.cls == "dx" && e.lastHeard > 0 && nowS - e.lastHeard <= DX_DIAL_MAX_AGE_S && e.lastHeard - nowS <= 3600) cand.push_back(std::move(e));
    std::stable_sort(cand.begin(), cand.end(), [](const DxDialIn& a, const DxDialIn& b) { return a.lastHeard > b.lastHeard; });
    std::vector<double> taken;
    for (const KnownSig& o : others) if (o.src != "dx") taken.push_back(o.hz);
    for (double h : bmHz) if (h > 0) taken.push_back(h);
    for (const DxDialIn& e : cand) {
        if ((int)out.size() >= DX_DIAL_MAX) break;
        const bool dab = e.band == "DAB";
        if (!dab && e.band != "FM") continue;
        if (!dab && !(e.freqKhz >= 65800 && e.freqKhz <= 108000)) continue;
        const double hz = e.freqKhz * 1000;
        if (!(hz > 0)) continue;
        const double tol = dab ? 50000 : 12000;
        bool near = false;
        for (double h : taken) if (std::fabs(h - hz) <= tol) { near = true; break; }
        for (const KnownSig& o : out) if (!near && std::fabs(o.hz - hz) <= tol) near = true;
        if (near) continue;
        KnownSig s;
        s.hz = hz; s.label = dxDialLabel(e, nowS); s.dec = ""; s.dialHz = hz; s.mode = dab ? "dab" : "wfm"; s.prio = PRIO_DX;
        s.src = "dx"; s.dx = true; s.heard = e.lastHeard;
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const KnownSig& a, const KnownSig& b) { return a.hz < b.hz; });
    return out;
}
/** The engine's dxlog.json (?dial=1, or an older engine's page) → the entries. Bounded: at most DX_DIAL_MAX read. */
inline std::vector<DxDialIn> dxDialParse(const std::string& body, double* pageNow = nullptr) {
    std::vector<DxDialIn> v;
    const Json j = Json::parse(body);
    if (pageNow) *pageNow = j["now"].num(0);
    const Json& es = j["entries"];
    for (size_t i = 0; i < es.size() && (int)v.size() < DX_DIAL_MAX; i++) {
        const Json& o = es[i];
        DxDialIn e;
        e.band = o["band"].str(); e.cls = o["class"].str(); e.range = o["range"].str(); e.pi = o["pi"].str(); e.ps = o["ps"].str();
        e.block = o["block"].str(); e.eid = o["eid"].str(); e.ensemble = o["ensemble"].str();
        e.freqKhz = o["freqKhz"].num(0); e.lastHeard = o["lastHeard"].num(0); e.rds = o["rds"].boolean(true);
        v.push_back(std::move(e));
    }
    return v;
}

// ── in view, and laid out ──────────────────────────────────────────────────────────────────────────────────────────

struct PlacedLabel {
    const KnownSig* sig = nullptr; double x = 0, lx = 0, w = 0; int row = 0;
    std::string text;   // ★ the shortened name ("…") when every row was used up; empty = sig->label (the TS's `lines`)
    const std::string& shown() const { return text.empty() ? sig->label : text; }
};
struct BlockedBox { int row; double x0, x1; };

/** knownSignals.ts labelBoxX: a box CENTRED on its marker, moved inward only as far as it must to stay on screen. */
inline double labelBoxX(double x, double w, double width) { return std::max(0.0, std::min(std::max(0.0, width - w), x - w / 2)); }

/**
 * knownSignals.ts labelLayout, line for line (Stuart, 2026-11-16: "centre all signal labels on their carrier frequency",
 * close signals "must still stack, never overlap or drop a label"): the labels inside [lo, lo + span] across `width` px;
 * higher PRIORITY first, then the one nearer the dial. Each box CENTRED on its marker (shifted inward at an edge), on the
 * lowest row where it overlaps no box and keeps every marker line clear (no box over a lower label's line, no line under a
 * higher label's box), sliding along the row (nearest to centred, still over its own carrier) where centred does not fit;
 * only with every row used is the name shortened with "…" (to LABEL_MIN_CHARS). At most `maxN`. Two R36S additions, both
 * defaulting to the TS's behaviour: `maxN` (LABEL_MAX there) and `blockGap`, the clear space kept from a BLOCKED box (the
 * TS keeps `gap` from both). `pad` is no longer used.
 */
inline std::vector<PlacedLabel> labelLayout(const std::vector<KnownSig>& list, double lo, double span, double width,
                                            const std::function<double(const std::string&)>& measure, double dialHz,
                                            const std::vector<BlockedBox>& blocked = {}, double pad = 3, int rows = LABEL_ROWS_MAX,
                                            double gap = LABEL_GAP, int maxN = LABEL_MAX, double blockGap = -1) {
    (void)pad;
    std::vector<PlacedLabel> out;
    if (!(span > 0 && width > 0)) return out;
    const double hi = lo + span;
    auto it = std::lower_bound(list.begin(), list.end(), lo, [](const KnownSig& s, double v) { return s.hz < v; });
    std::vector<const KnownSig*> vis;
    for (; it != list.end() && it->hz <= hi; ++it) vis.push_back(&*it);
    if (vis.empty()) return out;
    const double d = std::isfinite(dialHz) ? dialHz : lo + span / 2;
    std::stable_sort(vis.begin(), vis.end(), [d](const KnownSig* a, const KnownSig* b) {
        if (a->prio != b->prio) return a->prio > b->prio;
        const double da = std::fabs(a->hz - d), db = std::fabs(b->hz - d);
        if (da != db) return da < db;
        return a->hz < b->hz;
    });
    if (blockGap < 0) blockGap = gap;
    struct Box { double a, b, g; };
    std::vector<std::vector<Box>> taken((size_t)rows);
    for (const BlockedBox& b : blocked) if (b.row >= 0 && b.row < rows) taken[(size_t)b.row].push_back({b.x0, b.x1, blockGap});
    const double LINE = 1;   // a marker line's half-clearance (px)
    // where a w-wide box for a marker at x can go on row r (its left edge), nearest to centred; NaN = nowhere
    auto slot = [&](int r, double x, double w) -> double {
        for (const PlacedLabel& p : out) if (p.row > r && x >= p.lx - LINE && x <= p.lx + p.w + LINE) return NAN;
        const double c = labelBoxX(x, w, width);
        auto ok = [&](double lx) {
            if (!(lx >= 0 && lx + w <= width && lx - LINE <= x && x <= lx + w + LINE)) return false;
            for (const Box& t : taken[(size_t)r]) if (!(lx + w + t.g <= t.a || lx >= t.b + t.g)) return false;
            for (const PlacedLabel& p : out) if (p.row < r && !(p.x < lx - LINE || p.x > lx + w + LINE)) return false;
            return true;
        };
        if (ok(c)) return c;
        std::vector<double> cands;
        for (const Box& t : taken[(size_t)r]) { cands.push_back(t.a - t.g - w); cands.push_back(t.b + t.g); }
        for (const PlacedLabel& p : out) if (p.row < r) { cands.push_back(p.x - LINE - 0.5 - w); cands.push_back(p.x + LINE + 0.5); }
        std::stable_sort(cands.begin(), cands.end(), [c](double u, double v) { return std::fabs(u - c) < std::fabs(v - c); });
        for (double lx : cands) if (ok(lx)) return lx;
        return NAN;
    };
    auto tryRows = [&](const KnownSig* sig, double x, const std::string& text, double w) {
        for (int r = 0; r < rows; r++) {
            const double lx = slot(r, x, w);
            if (std::isnan(lx)) continue;
            taken[(size_t)r].push_back({lx, lx + w, gap});
            PlacedLabel p{sig, x, lx, w, r, {}};
            if (text != sig->label) p.text = text;
            out.push_back(std::move(p));
            return true;
        }
        return false;
    };
    for (const KnownSig* sig : vis) {
        if ((int)out.size() >= maxN) break;
        const double x = (sig->hz - lo) / span * width;
        if (tryRows(sig, x, sig->label, measure(sig->label))) continue;
        // every row used up: shorten with "…", longest first
        std::string base = sig->label;
        static const std::string ell = "\xE2\x80\xA6";
        if (base.size() >= 3 && base.compare(base.size() - 3, 3, ell) == 0) base.resize(base.size() - 3);
        std::vector<size_t> cut;   // byte offset after each code point
        for (size_t i = 0; i < base.size(); i++) if (i + 1 == base.size() || (((unsigned char)base[i + 1]) & 0xC0) != 0x80) cut.push_back(i + 1);
        for (int n = (int)cut.size() - 1; n >= LABEL_MIN_CHARS; n--) {
            std::string t = base.substr(0, cut[(size_t)n - 1]);
            while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
            t += ell;
            if (tryRows(sig, x, t, measure(t))) break;
        }
    }
    return out;
}

/** knownSignals.ts labelStems, line for line: each placed label's marker line, at its TRUE carrier x, from its box's top
 *  (y1) up to `rise` above the highest box (y0), stopping `gap` short of any box in the way; at least `minLen` long. */
struct LabelStem { double x, y0, y1; };
inline std::vector<LabelStem> labelStems(const std::vector<PlacedLabel>& placed, const std::function<double(int)>& rowTop,
                                         double boxH, double rise, double gap = 1, double minLen = 2) {
    std::vector<LabelStem> out;
    if (placed.empty()) return out;
    double stackTop = INFINITY;
    for (const PlacedLabel& p : placed) stackTop = std::min(stackTop, rowTop(p.row));
    for (const PlacedLabel& p : placed) {
        const double x = std::round(p.x), y1 = rowTop(p.row);
        double y0 = stackTop - rise;
        for (const PlacedLabel& q : placed) {
            if (&q == &p || x < q.lx || x > q.lx + q.w) continue;
            const double qb = rowTop(q.row) + boxH;
            if (qb <= y1 && qb + gap > y0) y0 = qb + gap;
        }
        if (y1 - y0 < minLen) y0 = y1 - minLen;
        out.push_back({x, y0, y1});
    }
    return out;
}

// ── the R36S's drawing ─────────────────────────────────────────────────────────────────────────────────────────────

/**
 * The laid-out labels for one spectrum, cached. layout() recomputes only when its key changes; draw() is the per-frame
 * part: for each placed label a tick from its box down to the spectrum's floor, a 2-px bar along a digital mode's span,
 * the dark box and the lilac name — waterfall.ts _drawKnown's look, at the R36S's size.
 */
class KnownLabels {
public:
    static constexpr float kPadX = 3;        // text inset in its box (the web's 3 px)
    static constexpr float kMarkPad = 3;     // box beside its marker
    static constexpr float kGap = 4;         // clear space between two labels on a row
    static constexpr float kAxisW = 36;      // the dB axis's labels at the left (drawOverlays)
    /** The list changed (bump it with every new list). */
    void setList(std::vector<KnownSig>&& l) { placed_.clear(); list_ = std::move(l); gen_++; }   // (placed_ points into list_)
    void clear() { if (!list_.empty()) { placed_.clear(); list_.clear(); gen_++; } }
    const std::vector<KnownSig>& list() const { return list_; }
    const std::vector<PlacedLabel>& placed() const { return placed_; }
    int layouts() const { return layouts_; }
    float boxH(const Font& f) const { return (float)std::max(12, f.lineHeight() - 3); }
    /** The top of a row's box: row 0 by the spectrum's floor, rows 1–3 stacked above it. */
    float rowTop(Rect spec, const Font& f, int row) const { const float h = boxH(f); return spec.b() - 2 - h - row * (h + 1); }

    /** Lay out for this view. `needleX` < 0: no dial on screen. */
    void layout(TextCache& tc, const Font& f, Rect spec, double lo, double span, double dialHz, float needleX) {
        const Key k{lo, span, dialHz, needleX, spec.x, spec.w, spec.b(), gen_, f.get()};
        if (k == key_) return;
        key_ = k;
        layouts_++;
        placed_.clear();
        if (list_.empty() || !(span > 0)) return;
        if (widths_.size() > 512 || f.get() != widthsFont_) { widths_.clear(); widthsFont_ = f.get(); }
        auto measure = [&](const std::string& t) {
            auto it = widths_.find(t);
            if (it != widths_.end()) return it->second;
            const double w = tc.width(f, t) + 2 * kPadX;
            widths_[t] = w;
            return w;
        };
        // ★ never over the dB axis. ★★ (2026-11-16) No longer kept off the needle: as on the app and the web, the labels are
        //   centred on their carriers and stack; the one naming the station you are on is drawn as the tuned label, on
        //   the needle at the waterfall's top (tuned_label_app.h), and not here
        std::vector<BlockedBox> blocked = {{0, 0, kAxisW}, {1, 0, kAxisW}};
        (void)needleX;
        placed_ = labelLayout(list_, lo, span, spec.w, measure, dialHz, blocked, kMarkPad, LABEL_ROWS_MAX, kGap, HH_LABEL_MAX, 1);
    }

    /** Draw the last layout. Every quad goes into the frame's one batch; glyphs come from the atlas. ★★ V12 `hi` (≥ 0): the
     *  placed label naming the station you are tuned to — NOT drawn here: its name is the tuned label's, centred on the
     *  needle (tuned_label_app.h — Stuart, 2026-11-16: "centre the frequency and bookmark reading on the VFO"), so it is
     *  never drawn twice. `hiCol` is no longer used (kept so callers need not change). */
    void draw(Gfx& g, TextCache& tc, const Font& f, Rect spec, double lo, double span, int hi = -1, Rgba hiCol = Rgba{}) const {
        if (placed_.empty()) return;
        const float h = boxH(f), ty = (h - f.lineHeight()) / 2.f + 0.5f;
        const Rgba tick = rgb(0xd9b8ff, 0.8f), bar = rgb(0xd9b8ff, 0.55f);
        // ★★★ each label's MARKER LINE ABOVE it (Stuart, 2026-11-16): from its top edge at the TRUE carrier, up past the
        //     stack, never through another label (labelStems — the label being drawn as the tuned one left out)
        std::vector<PlacedLabel> ps;
        for (size_t pi = 0; pi < placed_.size(); pi++) if ((int)pi != hi) ps.push_back(placed_[pi]);
        const auto stems = labelStems(ps, [&](int r) { return (double)rowTop(spec, f, r); }, h, 6);
        const Rgba dxTick = rgb(0xffe45c, 0.8f);   // ★★ V12 a DX catch's line in its own colour
        for (size_t i = 0; i < ps.size(); i++)
            g.rect(spec.x + (float)stems[i].x, (float)stems[i].y0, 1, (float)(stems[i].y1 - stems[i].y0), ps[i].sig->dx ? dxTick : tick);
        for (size_t pi = 0; pi < placed_.size(); pi++) {
            const PlacedLabel& p = placed_[pi];
            if ((int)pi == hi) continue;   // ★★ drawn as the tuned label, on the needle
            const float top = rowTop(spec, f, p.row);
            const KnownSig& s = *p.sig;
            if (s.lo >= 0 && s.hi >= 0) {
                const float x0 = std::max(spec.x, spec.x + (float)((s.lo - lo) / span * spec.w));
                const float x1 = std::min(spec.r(), spec.x + (float)((s.hi - lo) / span * spec.w));
                if (x1 - x0 >= 3) g.rect(x0, spec.b() - 2, x1 - x0, 2, bar);
            }
            g.rect(spec.x + (float)p.lx, top, (float)p.w, h, th::knownBox);
            // centred in its box (it is laid out centred on its carrier; a shortened name carries its own text)
            const float tw = (float)tc.width(f, p.shown());
            tc.draw(g, f, p.shown(), spec.x + (float)(p.lx + (p.w - tw) / 2), top + ty, s.dx ? th::dxLabel : th::knownLabel);
        }
    }

private:
    struct Key {
        double lo = -1, span = -1, dial = -1; float needle = -2, sx = 0, sw = 0, sb = 0; uint64_t gen = ~0ull; const void* font = nullptr;
        bool operator==(const Key& o) const {
            return lo == o.lo && span == o.span && dial == o.dial && needle == o.needle && sx == o.sx && sw == o.sw && sb == o.sb && gen == o.gen && font == o.font;
        }
    };
    std::vector<KnownSig> list_;
    std::vector<PlacedLabel> placed_;
    std::unordered_map<std::string, double> widths_;
    const void* widthsFont_ = nullptr;
    Key key_;
    uint64_t gen_ = 0;
    int layouts_ = 0;
};

}  // namespace hh
