// vts_core.h — the VTS bar's LOGIC, ported from the app: what the bar says, in which order, when it comes and goes,
// and how its line moves. No GL, no SDL, no App — plain C++17, so test/vts_golden_test.cpp can replay the app's own TS
// against it (test/gen_vts_golden.ts). The drawing is vts.h.
//
// Sources, ported line for line (keep in step — the app is the reference):
//   src/services/vtsLine.ts        the line: "PI: C363 / Name: RadioText" (vtsIdText / vtsStationText / vtsJoin /
//                                  vtsLineSegments / vtsHex)
//   src/services/psStabiliser.ts   the RDS PS stabiliser (a marquee PS shown as ONE stable name, minimum dwell)
//   src/utils/cleanLines.ts        cleanKeepSpacing (off-air RDS text: controls → spaces, capped)
//   src/services/dabTypes.ts       dabSafeText (off-air DAB text)
//   src/services/rdsCountry.ts     eccPiToIso / validIso (the transmitter's country — ECC + PI only, see VtsScreen)
//   src/constants/bandPlan.ts      getBandsAtRegion · src/services/stations.ts fmtBandFreq (the band announcement)
//   src/screens/SDRScreen.tsx      the bar's ONE owner: the notice queue (vtsNoticeOps), showBandNotif, vtsCheck, the
//                                  live-station effect, the tone effect, the gain-at-minimum / SDRplay AGC / RTL
//                                  automation notices, withCall (US call letters)
//   src/services/VibeServerWsClient.ts  how the `rds` / `notice` / `tone` messages become those inputs
//   src/components/VTSBar.tsx      the bar itself: entrance / in-place update / timed exit, the pixel slide (the
//                                  default HYPER style — Atkinson; the R36S never draws the VFD strip)
//
// ★ Not ported, because the handheld has no source for them: bookmarks (no bookmark list on the device), the EiBi
//   schedule, station logos (no internet lookup), band CONDITIONS (UberSDR-only), the VFD (dot / seg) strip, the DX-log
//   alerts (needs a server DX log poll), OWRX log messages. Each is a source that never fires here, never a dead control.
#pragma once
#include "bandplan_gen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hh {
namespace vts {

// ── UTF-8 / JS string semantics ─────────────────────────────────────────────────────────────────────────────────────
// ★ The TS works on UTF-16 strings; everything a PS / RadioText / DAB label carries is in the BMP, where a code point is
//   one UTF-16 unit, so the port works on code points (std::u32string) and lengths agree.

inline std::u32string u32(const std::string& s) {
    std::u32string out;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        char32_t cp; int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { out.push_back(0xFFFD); i++; continue; }
        if (i + n > s.size()) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k < n; k++) {
            const unsigned char d = (unsigned char)s[i + k];
            if ((d >> 6) != 2) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); i++; continue; }
        out.push_back(cp);
        i += n;
    }
    return out;
}
inline std::string u8(const std::u32string& s) {
    std::string out;
    for (char32_t cp : s) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
        else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 63)); out += (char)(0x80 | (cp >> 6 & 63)); out += (char)(0x80 | (cp & 63)); }
    }
    return out;
}
/** JS `\s` / String.prototype.trim's white space (WhiteSpace + LineTerminator). */
inline bool jsSpace(char32_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}
inline std::u32string trim32(const std::u32string& s) {
    size_t a = 0, b = s.size();
    while (a < b && jsSpace(s[a])) a++;
    while (b > a && jsSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}
inline std::string trim(const std::string& s) { return u8(trim32(u32(s))); }
inline bool endsWith(const std::u32string& s, const std::u32string& t) { return t.size() <= s.size() && s.compare(s.size() - t.size(), t.size(), t) == 0; }
inline bool contains(const std::u32string& s, const std::u32string& t) { return s.find(t) != std::u32string::npos; }
/** `[\p{L}\p{N}]` for the scripts a PS carries: ASCII letters and digits, Latin-1 / Latin Extended letters, Greek,
 *  Cyrillic. (A non-letter symbol above U+00BF is not in a PS's character set.) */
inline bool letterOrDigit(char32_t c) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) return true;
    if (c == 0xAA || c == 0xB5 || c == 0xBA || c == 0xB2 || c == 0xB3 || c == 0xB9 || (c >= 0xBC && c <= 0xBE)) return true;
    if (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) return true;
    if (c >= 0x370 && c <= 0x52F && c != 0x37E && c != 0x387) return true;
    return false;
}

/** src/utils/cleanLines.ts cleanKeepSpacing: controls, bidi overrides and zero-width characters → a space, one for one
 *  (no collapsing, no trimming), capped at `max` units. */
inline std::string cleanKeepSpacing(const std::string& v, size_t max = 64) {
    std::u32string s = u32(v);
    for (auto& c : s)
        if (c <= 0x1F || (c >= 0x7F && c <= 0x9F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)
            || c == 0x200B || c == 0x2060 || c == 0xFEFF) c = ' ';
    if (s.size() > max) s.resize(max);
    return u8(s);
}
/** src/services/dabTypes.ts dabSafeText. */
inline std::string dabSafeText(const std::string& v, size_t max = 128) {
    std::u32string out;
    for (char32_t c : u32(v)) {
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) { out.push_back(' '); continue; }
        if ((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) || c == 0x200B || c == 0x2060 || c == 0xFEFF) continue;
        out.push_back(c);
        if (out.size() >= max) break;
    }
    return u8(trim32(out));
}

// ── the line (src/services/vtsLine.ts) ──────────────────────────────────────────────────────────────────────────────

inline const char* const kIdSep = " / ";
inline const char* const kTextSep = ": ";

/** vtsHex: four upper-case hex digits, '' for none (≤ 0, NaN, > 0xFFFFFFFF). */
inline std::string vtsHex(double code) {
    if (!std::isfinite(code) || code <= 0 || code > 4294967295.0) return "";
    char b[16];
    std::snprintf(b, sizeof b, "%04llX", (unsigned long long)std::llround(code));
    return b;
}
inline std::string vtsIdText(const std::string& id, const std::string& label = "PI") {
    const std::string code = trim(id);
    return code.empty() ? "" : label + ": " + code;
}
/** "Name: RadioText"; either half alone; the message is dropped when it merely repeats the name. */
inline std::string vtsStationText(const std::string& name, const std::string& text) {
    const std::string n = trim(name), t = trim(text);
    if (t.empty() || t == n) return n;
    if (n.empty()) return t;
    return n + kTextSep + t;
}
inline std::string vtsJoin(const std::string& idText, const std::string& station) {
    const std::string a = trim(idText), b = trim(station);
    return !a.empty() && !b.empty() ? a + kIdSep + b : !a.empty() ? a : b;
}
struct Seg { enum Kind { Id, Sep, Name, Text } kind; std::string s; };
inline std::vector<Seg> vtsLineSegments(const std::string& id, const std::string& idLabel, const std::string& name, const std::string& text) {
    std::vector<Seg> out;
    const std::string idText = vtsIdText(id, idLabel.empty() ? "PI" : idLabel);
    const std::string station = vtsStationText(name, text);
    if (!idText.empty()) out.push_back({Seg::Id, idText});
    if (!idText.empty() && !station.empty()) out.push_back({Seg::Sep, kIdSep});
    if (!station.empty()) {
        const std::string n = trim(name), t = trim(text);
        if (station == n) out.push_back({Seg::Name, n});
        else if (n.empty()) out.push_back({Seg::Text, t});
        else { out.push_back({Seg::Name, n}); out.push_back({Seg::Sep, kTextSep}); out.push_back({Seg::Text, t}); }
    }
    return out;
}
inline std::string vtsLine(const std::string& id, const std::string& idLabel, const std::string& name, const std::string& text) {
    return vtsJoin(vtsIdText(id, idLabel.empty() ? "PI" : idLabel), vtsStationText(name, text));
}

// ── the PS stabiliser (src/services/psStabiliser.ts) ────────────────────────────────────────────────────────────────

constexpr double kMinDwellMs = 3000, kRotatingDwellMs = 10000;
constexpr int kRotMinChanges = 3;
constexpr double kRotWindowMs = 30000, kHistoryMs = 120000;
constexpr size_t kHistoryMax = 200;
constexpr int kMaxPeriod = 64;

inline std::u32string startAtWord(const std::u32string& ring) {
    if (ring.empty()) return ring;
    if (jsSpace(ring.front()) || jsSpace(ring.back())) return trim32(ring);
    const size_t k = ring.rfind(U' ');
    if (k == std::u32string::npos) return trim32(ring);
    return trim32(ring.substr(k + 1) + ring.substr(0, k));
}
inline std::u32string trimWrap(const std::u32string& s) {
    for (long o = (long)(s.size() / 2); o >= 3; o--)
        if (endsWith(s, s.substr(0, (size_t)o))) return startAtWord(s.substr(0, s.size() - (size_t)o));
    return s;
}
/** mergeSegments: a character scroller's overlapping pieces are fused, a contained one skipped, the rest space-joined. */
inline std::u32string mergeSegments(const std::vector<std::u32string>& segs) {
    std::u32string acc;
    for (const auto& s : segs) {
        if (s.empty()) continue;
        if (acc.empty()) { acc = s; continue; }
        if (contains(acc, s) && s.size() >= 3) continue;
        long o = (long)std::min(acc.size(), s.size());
        const long need = std::max(3L, (long)std::ceil(std::min(acc.size(), s.size()) / 2.0));
        for (; o >= need; o--) if (endsWith(acc, s.substr(0, (size_t)o))) break;
        if (o < need) {
            for (o = std::min({(long)acc.size() - 1, (long)s.size() - 1, need - 1}); o >= 3; o--)
                if (endsWith(acc, s.substr(0, (size_t)o)) && letterOrDigit(acc[acc.size() - (size_t)o - 1])) break;
            if (o < 3) o = 0;
        }
        acc = o > 0 ? acc + s.substr((size_t)o) : acc + U" " + s;
    }
    return acc;
}

class PsStabiliser {
public:
    void reset() {
        hasKey_ = false; key_.clear(); hist_.clear(); firstSeen_.clear();
        shown_.clear(); shownAt_ = 0; shownRotating_ = false; shownSettled_ = true; lastRaw_.clear();
    }
    bool rotating() const { return shownRotating_; }
    bool settled() const { return !shownRotating_ || shownSettled_; }

    std::string feed(const std::string& key, const std::string& psRaw, double now) {
        if (!hasKey_ || key != key_) { reset(); hasKey_ = true; key_ = key; }
        // ps = raw.replace(/\s+/g, ' ').trim()
        std::u32string ps;
        bool inSp = false;
        for (char32_t c : u32(psRaw)) {
            if (jsSpace(c)) { if (!inSp) ps.push_back(U' '); inSp = true; }
            else { ps.push_back(c); inSp = false; }
        }
        ps = trim32(ps);
        if (!ps.empty() && ps != lastRaw_) {
            lastRaw_ = ps;
            hist_.push_back({ps, now});
            if (!firstSeen_.count(ps)) firstSeen_[ps] = now;
        }
        prune(now);
        return u8(evaluate(now));
    }
    std::string tick(double now) { prune(now); return u8(evaluate(now)); }
    /** ms until the display may change without a new observation; < 0 = nothing waiting. */
    double nextDueIn(double now) {
        Want w;
        if (!target(now, w) || shown_.empty() || w.name == shown_) return -1;
        return std::max(0.0, dueAt(w) - now);
    }

private:
    struct H { std::u32string ps; double t; };
    struct Want { std::u32string name; bool rotating = false; int settled = -1; /* -1 undefined, 0 false, 1 true */ };
    bool hasKey_ = false;
    std::string key_;
    std::deque<H> hist_;
    std::map<std::u32string, double> firstSeen_;
    std::u32string shown_, lastRaw_;
    double shownAt_ = 0;
    bool shownRotating_ = false, shownSettled_ = true;

    void prune(double now) {
        while (hist_.size() > kHistoryMax || (hist_.size() > 1 && now - hist_.front().t > kHistoryMs)) hist_.pop_front();
    }
    bool target(double now, Want& w) {
        if (lastRaw_.empty()) return false;
        std::vector<const H*> recent;
        for (auto& s : hist_) if (now - s.t <= kRotWindowMs) recent.push_back(&s);
        const long changes = (long)recent.size() - 1;
        std::vector<std::u32string> seen;
        bool recurs = false;
        for (size_t i = 0; i < recent.size(); i++) {
            const bool has = std::find(seen.begin(), seen.end(), recent[i]->ps) != seen.end();
            if (has && i > 0 && recent[i - 1]->ps != recent[i]->ps) { recurs = true; break; }
            seen.push_back(recent[i]->ps);
        }
        const bool rot = changes >= kRotMinChanges || recurs;
        if (!rot) { w = {lastRaw_, false, -1}; return true; }
        cycleName(w);
        w.rotating = true;
        return true;
    }
    std::u32string evaluate(double now) {
        Want w;
        if (!target(now, w)) return shown_;
        if (shown_.empty()) { adopt(w, now); return shown_; }
        if (w.name == shown_) { shownRotating_ = w.rotating; shownSettled_ = w.settled != 0; return shown_; }
        if (now >= dueAt(w)) adopt(w, now);
        return shown_;
    }
    double dueAt(const Want& w) const {
        if (w.rotating && w.settled != 0 && shownRotating_ && !shownSettled_) return shownAt_;
        const double base = shownAt_ + (shownRotating_ ? kRotatingDwellMs : kMinDwellMs);
        if (w.rotating) return base;
        const double since = hist_.empty() ? shownAt_ : hist_.back().t;
        return since - shownAt_ >= kMinDwellMs ? base : std::max(base, since + kMinDwellMs);
    }
    void adopt(const Want& w, double now) { shown_ = w.name; shownAt_ = now; shownRotating_ = w.rotating; shownSettled_ = w.settled != 0; }
    void cycleName(Want& w) const {
        std::vector<std::u32string> seq;
        for (auto& h : hist_) seq.push_back(h.ps);
        std::vector<std::u32string> cycle;
        for (int p = 2; p <= kMaxPeriod && (size_t)(2 * p) <= seq.size(); p++) {
            bool ok = true;
            for (size_t i = seq.size() - p; i < seq.size(); i++) if (seq[i] != seq[i - p]) { ok = false; break; }
            if (ok) { cycle.assign(seq.end() - p, seq.end()); break; }
        }
        if (cycle.empty()) { w.name = !shown_.empty() ? shown_ : (seq.empty() ? U"" : seq.back()); w.settled = 0; return; }
        w.name = trimWrap(mergeSegments(canonicalRotation(cycle)));
        w.settled = 1;
    }
    std::vector<std::u32string> canonicalRotation(const std::vector<std::u32string>& cycle) const {
        auto fs = [&](const std::u32string& s) { auto it = firstSeen_.find(s); return it == firstSeen_.end() ? 9007199254740991.0 : it->second; };
        const size_t n = cycle.size();
        size_t best = 0;
        for (size_t r = 1; r < n; r++)
            for (size_t i = 0; i < n; i++) {
                const double a = fs(cycle[(r + i) % n]), b = fs(cycle[(best + i) % n]);
                if (a != b) { if (a < b) best = r; break; }
            }
        std::vector<std::u32string> out(cycle.begin() + best, cycle.end());
        out.insert(out.end(), cycle.begin(), cycle.begin() + best);
        return out;
    }
};

// ── the station's country (src/services/rdsCountry.ts) ──────────────────────────────────────────────────────────────

/** eccPiToIso: ECC + PI → ISO country, '' when unresolved (IEC 62106-4:2018 via librdsparser). */
inline std::string eccPiToIso(int ecc, const std::string& pi) {
    if (ecc <= 0 || pi.empty()) return "";
    struct Row { int ecc; const char* iso[15]; };
    static const Row rows[] = {
        {0xA1, {"", "", "", "", "", "", "", "", "", "", "CA", "CA", "CA", "CA", "GL"}},
        {0xA2, {"AI", "AG", "", "FK", "BB", "BZ", "KY", "CR", "CU", "AR", "BR", "", "", "GP", "BS"}},
        {0xA3, {"BO", "CO", "JM", "MQ", "", "PY", "NI", "", "PA", "DM", "DO", "CL", "GD", "TB", "GY"}},
        {0xA4, {"GT", "HN", "AW", "", "MS", "TT", "PE", "SR", "UY", "KN", "LC", "SN", "HT", "VE", "VG"}},
        {0xA5, {"", "", "", "", "", "", "", "", "", "", "MX", "VC", "MX", "MX", "MX"}},
        {0xA6, {"", "", "", "", "", "", "", "", "", "", "", "", "", "", "PM"}},
        {0xD0, {"CM", "CF", "DJ", "MG", "ML", "AO", "GQ", "GA", "GN", "ZA", "BF", "CG", "TG", "BJ", "MW"}},
        {0xD1, {"NA", "LR", "GH", "MR", "ST", "CV", "SN", "GM", "BI", "SH", "BW", "KM", "TZ", "ET", "NG"}},
        {0xD2, {"SL", "ZW", "MZ", "UG", "SZ", "KE", "SO", "NE", "TD", "GW", "CD", "CI", "", "ZM", "ER"}},
        {0xD3, {"", "", "EH", "", "RW", "LS", "", "SC", "", "MU", "", "SD", "", "", ""}},
        {0xD4, {"", "", "", "", "", "", "", "", "", "SS", "", "", "", "", ""}},
        {0xE0, {"DE", "DZ", "AD", "IL", "IT", "BE", "RU", "PS", "AL", "AT", "HU", "MT", "DE", "", "EG"}},
        {0xE1, {"GR", "CY", "SM", "CH", "JO", "FI", "LU", "BG", "DK", "GI", "IQ", "GB", "LY", "RO", "FR"}},
        {0xE2, {"MA", "CZ", "PL", "VA", "SK", "SY", "TN", "", "LI", "IS", "MC", "LT", "RS", "ES", "NO"}},
        {0xE3, {"ME", "IE", "TR", "", "TJ", "", "", "NL", "LV", "LB", "AZ", "HR", "KZ", "SE", "BY"}},
        {0xE4, {"MD", "EE", "MK", "", "", "UA", "XK", "PT", "SI", "AM", "UZ", "GE", "", "TM", "BA"}},
        {0xE5, {"", "", "KG", "", "", "", "", "", "", "", "", "", "", "", ""}},
        {0xF0, {"AU", "AU", "AU", "AU", "AU", "AU", "AU", "AU", "SA", "AF", "MM", "CN", "KP", "BH", "MY"}},
        {0xF1, {"KI", "BT", "BD", "PK", "FJ", "OM", "NR", "IR", "NZ", "SB", "BN", "LK", "TW", "KR", "HK"}},
        {0xF2, {"KW", "QA", "KH", "WS", "IN", "MO", "VN", "PH", "JP", "SG", "MV", "ID", "AE", "NP", "VU"}},
        {0xF3, {"LA", "TH", "TO", "", "", "", "", "CN", "PG", "", "YE", "", "", "FM", "MN"}},
        {0xF4, {"", "", "", "", "", "", "", "", "CN", "", "MH", "", "", "", ""}},
    };
    const char h = pi[0];
    const int nib = h >= '0' && h <= '9' ? h - '0' : h >= 'A' && h <= 'F' ? h - 'A' + 10 : h >= 'a' && h <= 'f' ? h - 'a' + 10 : -1;
    if (nib < 1 || nib > 15) return "";
    for (auto& r : rows) if (r.ecc == ecc) return r.iso[nib - 1];
    return "";
}
inline bool validIso(const std::string& iso) {
    const std::string c = trim(iso);
    if (c.size() != 2) return false;
    for (char ch : c) if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))) return false;
    std::string u = c; for (auto& ch : u) ch = (char)std::toupper((unsigned char)ch);
    return u != "UN" && u != "XX";
}

// ── the band announcement (bandPlan.ts getBandsAtRegion · stations.ts fmtBandFreq · SDRScreen showBandNotif) ────────

inline std::vector<const BandDef*> bandsAtRegion(double hz, int region) {
    std::vector<const BandDef*> out;
    std::vector<std::string> seen;
    for (int i = 0; i < kBandPlanN; i++) {
        const BandDef& b = kBandPlan[i];
        if (hz < b.lo || hz > b.hi) continue;
        if (b.regions && region && !(b.regions & (1 << region))) continue;
        const std::string key = std::string(b.name) + "|" + std::to_string(b.type);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        out.push_back(&b);
    }
    // vtsCheck's order: ham, broadcast, utility (a stable sort, as JS's)
    std::stable_sort(out.begin(), out.end(), [](const BandDef* a, const BandDef* b) { return a->type < b->type; });
    return out;
}
/** JS Number.prototype.toFixed for the band edges (exact for the plan's values). */
inline std::string toFixed(double v, int d) { char b[48]; std::snprintf(b, sizeof b, "%.*f", d, v); return b; }
inline std::string fmtBandFreq(double hz) {
    if (hz >= 1000000) {
        const double mhz = hz / 1000000;
        return (mhz == std::floor(mhz) ? toFixed(mhz, 0) : toFixed(mhz, mhz < 10 ? 3 : 1)) + " MHz";
    }
    return std::to_string((long long)std::floor(hz / 1000 + 0.5)) + " kHz";
}
/** The band line and its sub-line. ★ No "Conditions": band conditions are UberSDR's noise-floor API only. */
inline void bandNotifText(const std::vector<const BandDef*>& bands, int region, std::string& primary, std::string& secondary) {
    const BandDef& p = *bands[0];
    primary = "BAND: " + fmtBandFreq(p.lo) + "\xE2\x80\x93" + fmtBandFreq(p.hi) + " \xC2\xB7 " + p.name;
    if (bands.size() > 1 && region) primary += " (ITU R" + std::to_string(region) + ")";
    secondary.clear();
    for (size_t i = 1; i < bands.size(); i++) { if (i > 1) secondary += "  \xE2\x94\x82  "; secondary += bands[i]->name; }
}
inline std::string bandKey(const std::vector<const BandDef*>& bands) {
    std::string k;
    for (size_t i = 0; i < bands.size(); i++) { if (i) k += "|"; k += bands[i]->name; }
    return k;
}

// ── what the bar shows (VTSBar.tsx VtsNotifData) ────────────────────────────────────────────────────────────────────

struct Notif {
    uint64_t key = 0;
    enum Kind { StationOn, Band, Notice } kind = StationOn;
    std::string name, rt, id, idLabel = "PI", secondary;
    double ms = 0;                 // 0 = NOTIF_MS
    bool hold = false;
    std::string badge;             // "RDS" / "DAB" / ""
    std::string iso;               // the transmitter's country (the app's flag), "" = none
    bool annunciators = false, tp = false, ta = false, af = false;
    bool dab = false, dabPlus = false;
};
constexpr double kNotifMs = 8000;
/** What a held bar's scroll restarts on: the WORDS, never a mark landing under the same words. */
inline std::string lineKey(const Notif& n) { return n.id + "|" + n.name + "|" + n.rt + "|" + n.secondary; }

// ── the screen: the bar's one owner (SDRScreen.tsx) ─────────────────────────────────────────────────────────────────

/** One frame's inputs, already read off the engine's messages (vts.h does that from RadioState). Every `*New` flag is
 *  "this message ARRIVED since the last frame" — the app reacts to arrivals, not to state. */
struct Inputs {
    double now = 0;                       // ms, monotonic
    std::string mode;                     // config.mode
    double vfoHz = 0;
    int region = 0;                       // ITU region (App::region())
    // rds (VibeServerWsClient 'rds')
    bool rdsNew = false;
    std::string rdsPs, rdsRt, rdsCall;
    int rdsPi = -1, rdsEcc = 0;
    bool rdsHasFlags = false; int rdsTp = -1, rdsTa = -1, rdsAf = -1;
    // dab (a `dab` report — the playing service) / dab_off
    bool dabNew = false, dabOffNew = false;
    std::string dabSvcLabel, dabSvcDls, dabDls, dabSvcCodec;
    double dabSvcSid = 0;
    // drm (a `drm` report while in DRM): the station names itself in its SDC (label) and sends a text message — the bar
    // shows them like a DAB service's label and DLS (the web page's updateVts), plus the audio it is sending
    bool drmNew = false, drmLive = false;
    std::string drmLabel, drmText, drmCountry, drmAudio;
    double drmSid = 0;
    // tone ({"type":"tone","label":…})
    bool toneNew = false; std::string toneLabel;
    // notice ({"type":"notice"} — `why` (a refusal) or `vts` (a server line for the bar); `ask` refusals are dropped)
    bool noticeNew = false; std::string noticeText;
    // the radio, for the app's own notices
    std::string driver;                   // hwinfo radio.driver
    bool connectedEdge = false;           // a (re)connection: a new radio's questions re-arm
    bool vibeAgc = false;                 // hwinfo agc (VibeAGC)
    bool hasTunerBw = false, tunerBwAuto = false;
    std::vector<int> gains; int gainNow = 0;
    bool hackrf = false; int hackLna = 0, hackVga = 0;
    int dsLive = 0;                       // direct sampling live (0 = off)
    bool rspAgcInit = false;              // rspstat agcInit
};

class Screen {
public:
    /** The notif the bar is handed (VTSBar `notif`), or none. */
    const std::optional<Notif>& notif() const { return notif_; }
    /** VTSBar onTimedEnd: a timed notif has had its time — forget it if it is still ours. */
    void timedEnd(uint64_t key) { if (notif_ && notif_->key == key) notif_.reset(); }

    void update(const Inputs& in) {
        now_ = in.now;
        if (in.connectedEdge) { gainMinShown_ = false; rtlExplained_ = false; }
        // the timers the app arms (setTimeout) land first
        if (noticeEndAt_ > 0 && now_ >= noticeEndAt_) endNotice();
        if (psTickAt_ > 0 && now_ >= psTickAt_) psTick();
        // ★ the mode first: the tone belongs to NFM only (SDRScreen: any other mode drops it at once)
        if (in.mode != "nfm" && in.mode != "fm" && !toneLabel_.empty()) setTone("");
        mode_ = in.mode;
        // arrivals
        if (in.rdsNew) onRds(in);
        if (in.dabOffNew) setDabOn(false);
        else if (in.dabNew) onDab(in);
        if (in.drmNew) onDrm(in);
        else if (in.mode != "drm" && live_.badge == "DRM") dropDrm();   // ★ leaving DRM takes its station with it
        if (in.toneNew) setTone(in.mode == "nfm" || in.mode == "fm" ? in.toneLabel : "");
        if (in.noticeNew && !in.noticeText.empty()) show(in.noticeText, 7000);
        ownNotices(in);
        // the tuned frequency, debounced 250 ms (the drum emits many per second)
        if (in.vfoHz > 0 && in.vfoHz != lastHz_) { lastHz_ = in.vfoHz; checkAt_ = now_ + 250; }
        if (checkAt_ > 0 && now_ >= checkAt_) { checkAt_ = 0; vtsCheck(in.vfoHz, in.region); }
        if (noticeEnded_) { noticeEnded_ = false; if (in.vfoHz > 0) vtsCheck(in.vfoHz, in.region); }
        liveStationEffect(in);
    }

private:
    double now_ = 0;
    uint64_t key_ = 0;
    std::optional<Notif> notif_;
    std::string mode_;
    // the notice queue (vtsNoticeOps)
    struct Q { uint64_t key; std::string msg; double ms; };
    std::optional<Q> showing_;
    std::vector<Q> queue_;
    double noticeUntil_ = 0, noticeEndAt_ = 0, startedAt_ = 0;
    bool noticeEnded_ = false;
    std::string lastStation_;              // vtsLastStation
    // the band crossing (vtsCheck)
    bool bandInit_ = false; std::string bandKey_; bool bandKeyNull_ = true;
    double lastHz_ = 0, checkAt_ = 0;
    // the live station (liveStation + its refs)
    struct Live { std::string name, text, badge, iso, pi, sid; int ecc = 0; bool hasFlags = false, tp = false, ta = false, af = false; int dabPlus = -1; };
    Live live_;
    std::string liveRef_, liveBadge_, liveCall_;
    bool hasLiveBadge_ = false;
    PsStabiliser ps_;
    double psTickAt_ = 0;
    bool dabOn_ = false;
    // the tone
    std::string toneLabel_;
    uint64_t toneKey_ = 0;
    // the app's own notices
    bool gainMinShown_ = false, gainMinOn_ = false; uint64_t gainMinKey_ = 0;
    bool agcInitOn_ = false; uint64_t agcInitKey_ = 0;
    bool rtlExplained_ = false;

    // ── the notice queue ──
    void armEnd(double ms) { noticeEndAt_ = now_ + std::max(0.0, ms) + 50; }
    void present(const Q& n) {
        showing_ = n;
        startedAt_ = now_;
        noticeUntil_ = startedAt_ + n.ms;
        Notif x; x.key = n.key; x.name = n.msg; x.kind = Notif::Notice; x.ms = n.ms;
        notif_ = x;
        armEnd(n.ms);
    }
    void endNotice() {
        noticeEndAt_ = 0;
        showing_.reset();
        noticeUntil_ = 0;
        if (!queue_.empty()) { const Q next = queue_.front(); queue_.erase(queue_.begin()); present(next); return; }
        lastStation_.clear();
        noticeEnded_ = true;
    }
    bool noticeUp() const { return now_ < noticeUntil_; }
public:
    /** showVtsNotice(msg, ms): one showing, the rest queue (≤ 3, the oldest dropped); the same words already showing or
     *  waiting are not said twice. Returns the notice's key. */
    uint64_t show(const std::string& msg, double ms) {
        if (showing_ && showing_->msg == msg) return showing_->key;
        for (auto& q : queue_) if (q.msg == msg) return q.key;
        const Q n{++key_, msg, ms};
        if (showing_) { queue_.push_back(n); if (queue_.size() > 3) queue_.erase(queue_.begin()); }
        else present(n);
        return n.key;
    }
    /** withdrawVtsNotice(key): exactly this notice, showing or waiting, and nothing that replaced it. */
    void withdraw(uint64_t key) {
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const Q& q) { return q.key == key; }), queue_.end());
        if (!showing_ || showing_->key != key) return;
        if (notif_ && notif_->key == key) notif_.reset();
        endNotice();
    }
private:
    // ── the app's own notices (SDRScreen: gain at minimum · SDRplay AGC initialising · RTL automation) ──
    void ownNotices(const Inputs& in) {
        // gain at minimum: once per connection, withdrawn the moment the gain moves
        if (in.dsLive > 0) gainMinShown_ = true;
        bool atMin = false;
        if (in.dsLive > 0) atMin = false;
        else if (in.hackrf) atMin = in.hackLna == 0 && in.hackVga == 0;
        else if (!in.vibeAgc && !in.gains.empty()) atMin = in.gainNow <= in.gains[0];
        bool want = gainMinOn_;
        if (!atMin) want = false;
        else if (!gainMinShown_) { gainMinShown_ = true; want = true; }
        if (want != gainMinOn_) {
            gainMinOn_ = want;
            if (want) gainMinKey_ = show("This receiver's gain is at minimum \xE2\x80\x94 on a strong local signal that may be deliberate, "
                                         "but if the spectrum looks empty, raise the gain before deciding the receiver is dead.", 30000);
            else withdraw(gainMinKey_);
        }
        // the SDRplay AGC is still setting itself up — said until it is done (75 s bound)
        if (in.rspAgcInit != agcInitOn_) {
            agcInitOn_ = in.rspAgcInit;
            if (agcInitOn_) agcInitKey_ = show("SDRplay AGC initialising: noise floor and signals will bounce until it settles (approx. 30 seconds)", 75000);
            else withdraw(agcInitKey_);
        }
        // the RTL's automation, once per connection, RTL only and only when it is really on
        if (!rtlExplained_ && in.driver == "rtl" && ((in.hasTunerBw && in.tunerBwAuto) || in.vibeAgc)) {
            rtlExplained_ = true;
            const bool bw = in.hasTunerBw && in.tunerBwAuto;
            show(bw && in.vibeAgc ? "AGC: On  |  IF Filter: Auto" : bw ? "IF Filter: Auto" : "AGC: On", 8000);
        }
    }

    // ── the band crossing (vtsCheck) ──
    void vtsCheck(double hz, int region) {
        const auto bands = bandsAtRegion(hz, region);
        const bool keyNull = bands.empty();
        const std::string key = bandKey(bands);
        if (!bandInit_) { bandInit_ = true; bandKey_ = key; bandKeyNull_ = keyNull; }
        else if (keyNull != bandKeyNull_ || key != bandKey_) {
            bandKey_ = key; bandKeyNull_ = keyNull;
            if (!bands.empty() && !noticeUp()) {
                Notif x; x.key = ++key_; x.kind = Notif::Band;
                bandNotifText(bands, region, x.name, x.secondary);
                notif_ = x;
            }
        }
        // a live RDS / DAB station owns the station display; with no bookmarks on the device the rest is the latch
        if (!liveRef_.empty()) return;
        lastStation_.clear();
    }

    // ── the live station ──
    std::string psKey(const std::string& pi) const {
        if (!pi.empty()) return "pi:" + pi;
        return "f:" + std::to_string((long long)std::llround(lastHz_ / 100000));
    }
    /** withCall: US call letters stand in for an absent or unsettled name, and lead a rotating one. */
    std::string withCall(const std::string& name) const {
        const std::string& call = liveCall_;
        if (call.empty() || (!name.empty() && !ps_.rotating())) return name;
        if (!ps_.settled()) return call;
        if (name.empty()) return call;
        return name.find(call) != std::string::npos ? name : call + " " + name;
    }
    void armPsTick() {
        const double due = ps_.nextDueIn(now_);
        psTickAt_ = due < 0 ? 0 : now_ + due + 20;
    }
    void psTick() {
        psTickAt_ = 0;
        const std::string name = withCall(ps_.tick(now_));
        if (!name.empty() && name != liveRef_) {
            liveRef_ = name;
            if (live_.badge == "RDS" && !live_.name.empty()) live_.name = name;
        }
        armPsTick();
    }
    void onRds(const Inputs& in) {
        const std::string ps = trim(cleanKeepSpacing(in.rdsPs, 16)), rt = trim(cleanKeepSpacing(in.rdsRt, 64));
        // ★ the client's own hex (msg.pi.toString(16).toUpperCase().padStart(4, '0')), any pi ≥ 0 — not vtsHex
        char pb[16] = {0};
        if (in.rdsPi >= 0) std::snprintf(pb, sizeof pb, "%04X", (unsigned)in.rdsPi);
        const std::string piHex = pb;
        const int ecc = in.rdsEcc > 0 ? in.rdsEcc : 0;
        std::string call;
        if (in.rdsCall.size() == 4 && (in.rdsCall[0] == 'K' || in.rdsCall[0] == 'W')
            && std::all_of(in.rdsCall.begin() + 1, in.rdsCall.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) call = in.rdsCall;
        const std::string badge = (!ps.empty() || !rt.empty() || !piHex.empty()) ? "RDS" : "";
        std::string stationName;
        if (badge == "RDS") {
            liveCall_ = call;
            stationName = withCall(ps_.feed(psKey(piHex), ps, now_));
            armPsTick();
        } else {
            ps_.reset();
            liveCall_.clear();
            psTickAt_ = 0;
            stationName.clear();
        }
        liveRef_ = stationName;
        liveBadge_ = badge; hasLiveBadge_ = !badge.empty();
        Live l;
        l.name = stationName; l.text = rt; l.badge = badge; l.pi = piHex; l.ecc = ecc;
        l.iso = eccPiToIso(ecc, piHex);   // ★ resolveStationIso with no receiver country = the ECC + PI answer alone
        l.hasFlags = in.rdsHasFlags;
        if (l.hasFlags) { l.tp = in.rdsTp == 1; l.ta = in.rdsTa == 1; l.af = in.rdsAf > 0; }
        live_ = l;
    }
    void onDab(const Inputs& in) {
        setDabOn(true);
        const std::string svc = dabSafeText(in.dabSvcLabel, 32);
        if (svc.empty()) return;
        liveRef_ = svc;
        liveBadge_ = "DAB"; hasLiveBadge_ = true;
        ps_.reset(); liveCall_.clear(); psTickAt_ = 0;
        Live l;
        l.name = svc;
        const std::string sdls = dabSafeText(in.dabSvcDls), dls = dabSafeText(in.dabDls);
        l.text = !sdls.empty() ? sdls : dls;
        l.badge = "DAB";
        l.sid = vtsHex(in.dabSvcSid);
        l.dabPlus = in.dabSvcCodec == "DAB+" ? 1 : in.dabSvcCodec == "MP2" ? 0 : -1;
        live_ = l;
    }
    void onDrm(const Inputs& in) {
        const std::string svc = in.drmLive ? dabSafeText(in.drmLabel, 32) : "";
        if (svc.empty()) { dropDrm(); return; }
        liveRef_ = svc;
        liveBadge_ = "DRM"; hasLiveBadge_ = true;
        ps_.reset(); liveCall_.clear(); psTickAt_ = 0;
        Live l;
        l.name = svc;
        const std::string txt = dabSafeText(in.drmText);
        l.text = txt.empty() ? in.drmAudio : in.drmAudio.empty() ? txt : txt + "  \xc2\xb7  " + in.drmAudio;
        l.badge = "DRM";
        l.sid = vtsHex(in.drmSid);
        l.iso = in.drmCountry;
        live_ = l;
    }
    void dropDrm() {
        if (liveBadge_ == "DRM") { liveBadge_.clear(); hasLiveBadge_ = false; liveRef_.clear(); }
        if (live_.badge == "DRM") live_ = Live{};
    }
    void setDabOn(bool on) {
        const bool was = dabOn_;
        dabOn_ = on;
        if (!was || on) return;
        // leaving DAB takes the DAB station with it (liveStationAfterDab)
        if (liveBadge_ == "DAB") { liveBadge_.clear(); hasLiveBadge_ = false; liveRef_.clear(); }
        if (live_.badge == "DAB") live_ = Live{};
    }
    void liveStationEffect(const Inputs& in) {
        const std::string& name = live_.name;
        const bool textOnly = name.empty() && !live_.text.empty();
        const bool isDrm = liveBadge_ == "DRM";
        const bool isDab = liveBadge_ == "DAB" || dabOn_ || isDrm;   // ★ DRM's service id reads like DAB's: "SId: 1000"
        const std::string vtsId = isDab ? live_.sid : liveBadge_ == "RDS" ? live_.pi : "";
        if (name.empty() && !textOnly && vtsId.empty()) {
            if (!lastStation_.empty()) {
                lastStation_.clear();
                if (!(notif_ && notif_->kind == Notif::Notice && !notif_->hold)) notif_.reset();
                if (in.vfoHz > 0) vtsCheck(in.vfoHz, in.region);
            }
            return;
        }
        const bool wfm = in.mode == "wfm" && !dabOn_;
        const std::string iso = (wfm || isDrm) && validIso(live_.iso) ? live_.iso : "";
        const bool ann = wfm && live_.hasFlags;
        const bool dab = liveBadge_ == "DAB";
        const bool plus = live_.dabPlus == 1;
        const std::string composite = vtsId + "|" + name + "|" + live_.text + "|" + iso + "||" + (dab ? (plus ? "D+" : "D") : "")
            + "|" + (ann ? std::string(1, '0' + live_.tp) + (char)('0' + live_.ta) + (char)('0' + live_.af) : "");
        if (noticeUp()) return;
        if (composite != lastStation_) {
            lastStation_ = composite;
            Notif x;
            x.key = ++key_; x.name = name; x.rt = live_.text; x.id = vtsId; x.idLabel = isDab ? "SId" : "PI";
            x.kind = Notif::StationOn; x.hold = true;
            x.badge = isDrm ? "" : hasLiveBadge_ ? liveBadge_ : (!vtsId.empty() ? "RDS" : "");   // ★ no DRM mark (the web has none)
            x.iso = iso;
            x.annunciators = ann; x.tp = live_.tp; x.ta = live_.ta; x.af = live_.af;
            x.dab = dab; x.dabPlus = plus;
            notif_ = x;
        }
    }

    // ── the tone (SDRScreen's toneLabel effect; no bookmark to sit after, so it is the line on its own) ──
    void setTone(const std::string& label) {
        if (label == toneLabel_) return;
        toneLabel_ = label;
        if (!liveRef_.empty() || noticeUp()) return;
        if (!label.empty()) {
            Notif x; x.key = ++key_; toneKey_ = x.key; x.rt = label; x.kind = Notif::StationOn; x.hold = true;
            notif_ = x;
        } else if (toneKey_) {
            const uint64_t k = toneKey_;
            toneKey_ = 0;
            if (notif_ && notif_->key == k) notif_.reset();
        }
    }
};

// ── the bar (VTSBar.tsx): entrance, in-place update, timed exit, the slide ──────────────────────────────────────────

/** Animated.timing's default easing, Easing.inOut(Easing.ease) — ease = cubic-bezier(0.42, 0, 1, 1). */
inline double easeInOut(double t) {
    auto bez = [](double x) {
        // solve x(u) = x for the bezier (0,0) (0.42,0) (1,1) (1,1), then y(u)
        const double x1 = 0.42, x2 = 1.0, y1 = 0.0, y2 = 1.0;
        auto bx = [&](double u) { return 3 * x1 * u * (1 - u) * (1 - u) + 3 * x2 * u * u * (1 - u) + u * u * u; };
        auto by = [&](double u) { return 3 * y1 * u * (1 - u) * (1 - u) + 3 * y2 * u * u * (1 - u) + u * u * u; };
        double lo = 0, hi = 1, u = x;
        for (int i = 0; i < 40; i++) { u = (lo + hi) / 2; if (bx(u) < x) lo = u; else hi = u; }
        return by(u);
    };
    t = std::max(0.0, std::min(1.0, t));
    return t < 0.5 ? bez(t * 2) / 2 : 1 - bez((1 - t) * 2) / 2;
}

class Bar {
public:
    /** The bar's showing notif (null = drawn not at all). */
    const std::optional<Notif>& shown() const { return shown_; }

    /** Once a frame, with the screen's notif. Returns the key of a timed notif that has just had its time (0 = none):
     *  pass it to Screen::timedEnd. */
    uint64_t update(const std::optional<Notif>& notif, double now) {
        now_ = now;
        uint64_t ended = 0;
        // the bar's own hide timer
        if (hideAt_ > 0 && now >= hideAt_) { hideAt_ = 0; fadeOut(); ended = shownKey(); }
        // the effect keyed on [notif?.key, notif === null]
        const bool isNull = !notif.has_value();
        const uint64_t k = notif ? notif->key : 0;
        if (first_ || isNull != lastNull_ || k != lastKey_) {
            first_ = false; lastNull_ = isNull; lastKey_ = k;
            onNotif(notif);
        } else if (notif && shown_ && shown_->key == notif->key) {
            shown_ = notif;          // same key: the same object (the screen never mutates a notif in place)
        }
        // the fade-out's completion
        if (!visible_ && shown_ && opacity() <= 0 && now >= fadeAt_ + fadeMs_) shown_.reset();
        return ended;
    }
    double opacity() const {
        if (fadeMs_ <= 0) return fadeTo_;
        const double t = (now_ - fadeAt_) / fadeMs_;
        if (t >= 1) return fadeTo_;
        return fadeFrom_ + (fadeTo_ - fadeFrom_) * easeInOut(t);
    }
    /** The line's translateX (≤ 0) for a text `textW` wide in a window `areaW` wide. */
    double slideX(double textW, double areaW) {
        if (!shown_ || areaW <= 0 || textW <= areaW) return 0;
        const double dist = textW - areaW;
        // [shown.hold ? lineKey : key, areaW, textW] restart the slide
        const std::string sk = (shown_->hold ? "h" + lineKey(*shown_) : "k" + std::to_string(shown_->key));
        // ★ the first measurement after an entrance / a change of words starts nothing new: the slide began THEN
        //   (the app measures the line in the same layout pass that shows it)
        if (sk != slideKey_ || (slideTextW_ >= 0 && (textW != slideTextW_ || areaW != slideAreaW_))) slideAt_ = now_;
        slideKey_ = sk; slideTextW_ = textW; slideAreaW_ = areaW;
        const double t = now_ - slideAt_;
        if (shown_->hold) {
            const double dur = std::max(2200.0, dist * 16);
            const double period = 1200 + dur + 1500;
            const double ph = std::fmod(t, period);
            if (ph < 1200) return 0;
            if (ph < 1200 + dur) return -dist * (ph - 1200) / dur;
            return -dist;
        }
        const double ms = shown_->ms > 0 ? shown_->ms : kNotifMs;
        const double dur = std::max(1500.0, std::min(dist * 16, ms - 2500));
        if (t < 900) return 0;
        if (t < 900 + dur) return -dist * (t - 900) / dur;
        return -dist;
    }

private:
    double now_ = 0;
    std::optional<Notif> shown_;
    bool visible_ = false;
    double hideAt_ = 0;
    double fadeFrom_ = 0, fadeTo_ = 0, fadeAt_ = 0, fadeMs_ = 0;
    bool first_ = true, lastNull_ = true;
    uint64_t lastKey_ = 0;
    std::string slideKey_;
    double slideTextW_ = -1, slideAreaW_ = -1, slideAt_ = 0;

    uint64_t shownKey() const { return shown_ ? shown_->key : 0; }
    void fadeTo(double to, double ms) { fadeFrom_ = opacity(); fadeTo_ = to; fadeAt_ = now_; fadeMs_ = ms; }
    void fadeOut() { visible_ = false; fadeTo(0, 300); }
    void onNotif(const std::optional<Notif>& n) {
        if (!n) {
            // ★★★ an explicit clear takes down whatever is showing — timed or held
            if (shown_ && visible_) { hideAt_ = 0; fadeOut(); }
            return;
        }
        // ★★★ live data updates IN PLACE: a held notif replacing a held notif on screen keeps the opacity, and the
        //     scroll restarts only if the words changed
        const bool inPlace = n->hold && shown_ && shown_->hold && visible_;
        const bool textChanged = !shown_ || lineKey(*shown_) != lineKey(*n);
        shown_ = n;
        auto restartSlide = [&] {
            slideAt_ = now_;
            slideKey_ = n->hold ? "h" + lineKey(*n) : "k" + std::to_string(n->key);
            slideTextW_ = slideAreaW_ = -1;
        };
        if (!inPlace) {
            restartSlide();                          // textW reset → the slide restarts from its start
            fadeFrom_ = 0; fadeTo_ = 1; fadeAt_ = now_; fadeMs_ = 180;
        } else if (textChanged) {
            restartSlide();
        }
        visible_ = true;
        hideAt_ = 0;
        if (!n->hold) hideAt_ = now_ + (n->ms > 0 ? n->ms : kNotifMs);
    }
};

}  // namespace vts
}  // namespace hh
