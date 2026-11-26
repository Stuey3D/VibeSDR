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
enum Kind { None = 0, Adsb = 1, Avdata = 2, Ais = 3, Vormap = 4, Ism = 5 };

/** The landing map's name in the config — b96-onemap's two maps — "" = no map landing.
 *  ★★ AMATEUR (Stuart 11-26): ISM on its own band plan (868 / 433 …, LoRa — MeshCore, Meshtastic, LoRaWAN — as ISM's own
 *  "every channel" setting has it), so a radio can sit on 868 MHz for MeshCore repeaters all day and feed the community
 *  map's MeshCore layer. ★ No IQ capture or download of ISM bursts, ever — the mode is entered exactly as its key does. */
constexpr const char* kMapAvMar = "avmar";
constexpr const char* kMapAmateur = "amateur";
inline bool mapKnown(const std::string& m) { return m == kMapAvMar || m == kMapAmateur; }
inline const char* mapTitle(const std::string& m) { return m == kMapAvMar ? "Aviation & Maritime" : m == kMapAmateur ? "Amateur" : ""; }

struct Mode {
    const char* id;      ///< the config's word: "adsb" | "vdl2" | "acars" | "vor" | "ais"
    const char* name;    ///< what the page says
    const char* group;   ///< "aviation" | "maritime" — the heading it sits under
    Kind kind;
    const char* band;    ///< the datalink band (Avdata only)
    const char* bench;   ///< the benchmark row that prices it (prefix for ADS-B's per-rate rows)
    const char* map = kMapAvMar;   ///< the landing map it belongs to
    const char* block = nullptr;   ///< the modes-card switch it stands on (nullptr = its id)
};
inline const std::vector<Mode>& modes() {
    static const std::vector<Mode> v = {
        { "adsb",  "ADS-B",          "aviation", Adsb,   "",      "adsb_" },
        { "vdl2",  "VDL2",           "aviation", Avdata, "vdl2",  "dec_vdl2", kMapAvMar, "avdata" },
        { "acars", "ACARS",          "aviation", Avdata, "acars", "dec_acars", kMapAvMar, "avdata" },
        { "vor",   "VOR / ILS map",  "aviation", Vormap, "",      "dec_vormap", kMapAvMar, "vormap" },
        { "ais",   "AIS",            "maritime", Ais,    "",      "dec_ais" },
        // ★★ the band is the owner's pick from ISM's own plan, carried in the token: "ism:868" (no frequency typed)
        { "ism",   "ISM",            "ism",      Ism,    "",      "dec_ism", kMapAmateur },
    };
    return v;
}
/** "ism:868" → "ism"; "vdl2" → "vdl2". */
inline std::string baseOf(const std::string& tok) { const size_t c = tok.find(':'); return c == std::string::npos ? tok : tok.substr(0, c); }
/** "ism:868" → "868"; "" for a mode with no band of its own. */
inline std::string bandOfTok(const std::string& tok) { const size_t c = tok.find(':'); return c == std::string::npos ? std::string() : tok.substr(c + 1); }
inline const Mode* modeById(const std::string& id) {
    const std::string b = baseOf(id);
    for (const auto& m : modes()) if (b == m.id) return &m;
    return nullptr;
}
/** A token the table accepts: a known mode; ISM only with a band id (1–8 letters / digits — vibeism::bands() decides it is real). */
inline bool tokenOk(const std::string& t) {
    const Mode* m = modeById(t);
    if (!m) return false;
    const std::string band = bandOfTok(t);
    if (m->kind != Ism) return band.empty() && t == m->id;
    if (band.empty() || band.size() > 8) return false;
    for (char c : band) if (!std::isalnum((unsigned char)c)) return false;
    return true;
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
        else if (tokenOk(t)) got.push_back(t);
        tok.clear();
    };
    for (char c : s) { if (c == ',' || c == ';' || c == '\t' || c == '\n') flush(); else tok += c; }
    flush();
    std::vector<std::string> out;
    for (const auto& m : modes())
        for (const auto& g : got) if (baseOf(g) == m.id) { out.push_back(g); break; }   // ★ one band per mode: the first
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
    std::string band;    ///< the datalink band ("vdl2" | "acars" | "acars+vdl2") or the ISM band ("868")
    std::string msg;     ///< {"type":"avdata","on":1,"band":"vdl2"} — through handleControl, the key's own door
    std::string name;    ///< "VDL2"
    std::string bench;   ///< the benchmark row
    std::string kindWord() const { return kind == Adsb ? "adsb" : kind == Avdata ? "avdata" : kind == Ais ? "ais" : kind == Vormap ? "vormap" : kind == Ism ? "ism" : ""; }
    std::string map = kMapAvMar;
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
    e.kind = m->kind; e.band = m->band; e.name = m->name; e.bench = m->bench; e.map = m->map;
    if (m->kind == Ism) {
        e.band = bandOfTok(ids[0]);
        e.name = "ISM " + e.band + " MHz";
        e.msg = "{\"type\":\"ism\",\"on\":1,\"band\":\"" + e.band + "\"}";
        return true;
    }
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
    std::string landingMap;     ///< "" | "avmar" | "amateur"
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
    if (!mapKnown(s.landingMap)) s.landingMap.clear();
    std::vector<std::string> ids;
    for (const auto& t : parseCsv(s.landingModes)) if (const Mode* m = modeById(t); m && s.landingMap == m->map) ids.push_back(t);   // ★ the map's own modes only
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

// ══ ★★★ beta96 THE SINGLE-MODE RADIO (Stuart 11-26) ═══════════════════════════════════════════════════════════════════════
/* "RADIO USE: General | Single mode: [ADS-B ▾] — lock the radio to a single mode such as ADS-B / AIS / VDL2. Only the selected
 *  decoder is available." Today's "only mode" is inferred from the blocked list, which broke when new modes arrived; this is
 *  an EXPLICIT field (RadioConfig::singleMode) the server enforces directly: every other listening demodulator, whole-radio
 *  mode and SATELLITE is blocked on top of the owner's list (which is kept untouched underneath, so General gives the old
 *  ticks back), the mode is the landing, kept running where keep-running applies, and the only-mode exit lock holds.
 *  Tokens: the keep tokens (adsb, vdl2, acars, vor, ais, ism:<band>) + dab, ndb, sondes + the plain demodulators below. */
inline const std::vector<const char*>& singleDemods() {
    static const std::vector<const char*> v = { "wfm", "nfm", "am", "usb", "lsb", "drm" };   // ★ listening "where sensible"
    return v;
}
inline const std::vector<const char*>& singleWholeOnly() {
    static const std::vector<const char*> v = { "dab", "ndb", "sondes" };   // ★ whole-radio modes with no keep-running
    return v;
}
/** A token the single-mode field accepts ("" = General). */
inline bool singleOk(const std::string& t) {
    if (t.empty() || t == "sat" || t == "general") return true;   // ★ "general" = the owner chose General (no derivation from ticks)   // ★★ "sat" = SATELLITES: the radio follows the passes, listeners watch
    if (tokenOk(t)) return true;
    for (const char* m : singleWholeOnly()) if (t == m) return true;
    for (const char* m : singleDemods()) if (t == m) return true;
    return false;
}
/** The modes-card switch a single token stands on: VDL2 / ACARS → "avdata", the VOR map → "vormap", ISM 868 → "ism". */
inline std::string singleBlockOf(const std::string& t) {
    if (t.empty()) return {};
    if (const Mode* m = modeById(t); m && tokenOk(t)) return m->block ? m->block : m->id;
    return t;
}
/** Is the single mode one that keeps running (and so lands kept, through the landing map's machinery)? */
inline bool singleKeepable(const std::string& t) { return !t.empty() && t != "general" && tokenOk(t); }
/** The landing map a keepable single token belongs to. */
inline std::string singleMap(const std::string& t) { const Mode* m = modeById(t); return m ? m->map : ""; }
/** ★★ THE ENFORCEMENT RULE (vsModeBlocked): with `single` set, is mode `id` switched off REGARDLESS of the owner's list? Every
 *  listening demodulator, whole-radio mode and satellite switch but the single one. Options ("+…", "commb") are untouched. */
template <class Lists> inline bool singleBlocks(const std::string& single, const std::string& id, const Lists& demods, const Lists& whole) {
    if (single.empty() || single == "general") return false;
    const std::string keep = singleBlockOf(single);
    if (id == keep) return false;
    if (single == "sat") {   // ★ the passes need the demodulators (the pass's own tune); listeners are held by singleSatRefuses
        if (id == "sat_vfob") return false;
        for (const char* m : demods) if (id == m) return false;
    }
    if (id == "sat" || id == "sat_vfob") return true;
    for (const char* m : demods) if (id == m) return true;
    for (const char* m : whole) if (id == m) return true;
    return false;
}

}  // namespace vibekeep
