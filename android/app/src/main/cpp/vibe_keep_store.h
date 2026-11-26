// vibe_keep_store.h — the LANDING MAP's stored words (V12 beta96): the mode table, the CSV, the entry each choice opens
// through, and the MIGRATION from beta95's landingAdsb / adsbKeepRunning. No DSP: the config reader includes it
// (vibeserver_config.cpp), the shim and vibe_keep_run.h build on it. The why of it all: vibe_keep_run.h, docs/v12/KEEP-RUNNING.md.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace vibekeep {

/** The whole-radio mode a choice runs as (the shim's g_*Mode). */
enum Kind { None = 0, Adsb = 1, Avdata = 2, Ais = 3, Vormap = 4 };

/** The landing map's name in the config. One map today (b96-onemap merges aviation + maritime); "" = no map landing. */
constexpr const char* kMapAvMar = "avmar";
inline const char* mapTitle(const std::string& m) { return m == kMapAvMar ? "Aviation & Maritime" : ""; }

struct Mode {
    const char* id;      ///< the config's word: "adsb" | "vdl2" | "acars" | "vor" | "ais"
    const char* name;    ///< what the page says
    const char* group;   ///< "aviation" | "maritime" — the heading it sits under
    Kind kind;
    const char* band;    ///< the datalink band (Avdata only)
    const char* bench;   ///< the benchmark row that prices it (prefix for ADS-B's per-rate rows)
};
inline const std::vector<Mode>& modes() {
    static const std::vector<Mode> v = {
        { "adsb",  "ADS-B",          "aviation", Adsb,   "",      "adsb_" },
        { "vdl2",  "VDL2",           "aviation", Avdata, "vdl2",  "dec_vdl2" },
        { "acars", "ACARS",          "aviation", Avdata, "acars", "dec_acars" },
        { "vor",   "VOR / ILS map",  "aviation", Vormap, "",      "dec_vormap" },
        { "ais",   "AIS",            "maritime", Ais,    "",      "dec_ais" },
    };
    return v;
}
inline const Mode* modeById(const std::string& id) {
    for (const auto& m : modes()) if (id == m.id) return &m;
    return nullptr;
}
/** ★ The one combination a single whole-radio mode decodes: ACARS + VDL2 (the datalink's wide band). */
struct Combo { const char* a; const char* b; const char* band; const char* name; const char* bench; };
inline const Combo& acarsVdl2() { static const Combo c{ "acars", "vdl2", "acars+vdl2", "ACARS + VDL2", "dec_avboth" }; return c; }

/** "VDL2,acars ,  bogus" → {"vdl2","acars"} in TABLE order, known ids only, no repeats. */
inline std::vector<std::string> parseCsv(const std::string& s) {
    std::vector<std::string> got;
    std::string tok;
    auto flush = [&] {
        while (!tok.empty() && tok.back() == ' ') tok.pop_back();
        size_t i = 0; while (i < tok.size() && tok[i] == ' ') ++i;
        std::string t = tok.substr(i);
        for (auto& c : t) c = char(std::tolower((unsigned char)c));
        if (t == "acars+vdl2") { got.push_back("acars"); got.push_back("vdl2"); }
        else if (modeById(t)) got.push_back(t);
        tok.clear();
    };
    for (char c : s) { if (c == ',' || c == ';' || c == '\t' || c == '\n') flush(); else tok += c; }
    flush();
    std::vector<std::string> out;
    for (const auto& m : modes()) if (std::find(got.begin(), got.end(), m.id) != got.end()) out.push_back(m.id);
    return out;
}
inline std::string toCsv(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) { if (!s.empty()) s += ","; s += x; }
    return s;
}
inline bool isCombo(const std::vector<std::string>& ids) {
    if (ids.size() != 2) return false;   // ★ either order (the table's is vdl2, acars)
    const std::string a = acarsVdl2().a, b = acarsVdl2().b;
    return (ids[0] == a && ids[1] == b) || (ids[0] == b && ids[1] == a);
}

/** What the shim enters for a choice: the mode, its band, the control frame its own key sends, the words. */
struct Entry {
    Kind kind = None;
    std::string band;    ///< the datalink band ("vdl2" | "acars" | "acars+vdl2")
    std::string msg;     ///< {"type":"avdata","on":1,"band":"vdl2"} — through handleControl, the key's own door
    std::string name;    ///< "VDL2"
    std::string bench;   ///< the benchmark row
    std::string kindWord() const { return kind == Adsb ? "adsb" : kind == Avdata ? "avdata" : kind == Ais ? "ais" : kind == Vormap ? "vormap" : ""; }
};
/** One mode, or the one combination. false = nothing (empty) or a set no single mode decodes. */
inline bool entryFor(const std::vector<std::string>& ids, Entry& e) {
    e = Entry{};
    if (ids.empty()) return false;
    if (ids.size() == 2) {
        if (!isCombo(ids)) return false;
        const Combo& c = acarsVdl2();
        e.kind = Avdata; e.band = c.band; e.name = c.name; e.bench = c.bench;
        e.msg = std::string("{\"type\":\"avdata\",\"on\":1,\"band\":\"") + c.band + "\"}";
        return true;
    }
    if (ids.size() != 1) return false;
    const Mode* m = modeById(ids[0]);
    if (!m) return false;
    e.kind = m->kind; e.band = m->band; e.name = m->name; e.bench = m->bench;
    if (m->kind == Adsb) e.msg = "{\"type\":\"adsb\",\"on\":1}";
    else if (m->kind == Avdata) e.msg = std::string("{\"type\":\"avdata\",\"on\":1,\"band\":\"") + m->band + "\"}";
    else if (m->kind == Ais) e.msg = "{\"type\":\"ais\",\"on\":1}";
    else if (m->kind == Vormap) e.msg = "{\"type\":\"vormap\",\"on\":1}";
    return true;
}

// ══ THE STORED SETTINGS AND THEIR MIGRATION ═════════════════════════════════════════════════════════════════════════════
struct Stored {
    // beta95's two (still written, mirroring the ADS-B case — an older build reads the same landing)
    bool landingAdsb = false, adsbKeepRunning = false;
    // beta96
    std::string landingMap;     ///< "" | "avmar"
    std::string landingModes;   ///< "vdl2" | "acars,vdl2" | "adsb" …
    bool mapKeep = false;       ///< keep the mode running when nobody is connected
};
/** ★★ Read side: a config with no landingMap but beta95's ADS-B landing becomes the same thing in the new words; then the
 *  set is normalised (known ids, one mode or the one combination — a bad CSV keeps its FIRST mode), and the old two are
 *  mirrored from it. Idempotent. */
inline void migrate(Stored& s) {
    if (s.landingMap.empty() && s.landingAdsb) {
        s.landingMap = kMapAvMar; s.landingModes = "adsb"; s.mapKeep = s.adsbKeepRunning;
    }
    if (s.landingMap != kMapAvMar) s.landingMap.clear();
    std::vector<std::string> ids = parseCsv(s.landingModes);
    if (ids.size() > 1 && !isCombo(ids)) ids.resize(1);
    if (ids.empty()) s.landingMap.clear();          // ★ a map with no mode is no landing (the plain one applies)
    if (s.landingMap.empty()) { ids.clear(); s.mapKeep = false; }
    s.landingModes = toCsv(ids);
    s.landingAdsb = !s.landingMap.empty() && s.landingModes == "adsb";
    s.adsbKeepRunning = s.landingAdsb && s.mapKeep;
}

/** ★★ A READ that may come from an OLDER WRITER (beta95's page or app, which know only landingAdsb / adsbKeepRunning):
 *  `sawOld` / `sawNew` = which of the two sets of keys the JSON being read carried. Old words alone speak only for the
 *  ADS-B case — ADS-B landed (and kept or not) as they say; "ADS-B off" from them clears an ADS-B landing but never a VDL2
 *  or AIS one they cannot know about. Then migrate(). A partial live update ({"adsbKeepRunning":false}) works the same. */
inline void applyRead(Stored& s, bool sawOld, bool sawNew) {
    if (sawOld && !sawNew) {
        if (s.landingAdsb) { s.landingMap = kMapAvMar; s.landingModes = "adsb"; s.mapKeep = s.adsbKeepRunning; }
        else if (s.landingModes == "adsb") { s.landingMap.clear(); s.landingModes.clear(); s.mapKeep = false; }
    }
    migrate(s);
}

}  // namespace vibekeep
