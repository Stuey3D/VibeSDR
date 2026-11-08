// map_modes.h — THE MAP DECODERS on the R36S: ADS-B, AIS, NDB and ISM as the web client has them (whole-radio modes in
// the mode picker, like DAB — web/client/src/main.ts adsbSetMode / aisSetMode / ndbSetMode / ismSetMode and the
// spectrum.ts shim's messages), and the APRS / DSC maps of the decoder box. Each draws its list and its markers on the
// native map (map_page.h).
//
// ★★★ THE SAME MESSAGES AS THE WEB, ON THE SPECTRUM SOCKET: {"type":"adsb"|"ais"|"ndb"|"ism","on":1|0} (ISM: + "band");
//     the server answers with `<mode>` reports (once a second), `<mode>_off` when it leaves, `<mode>_error` {why} when it
//     refuses. A report's ARRIVAL means the receiver is in that mode, WHOEVER put it there (the shared-dial contract:
//     the client has no vote — somebody else's ADS-B opens the map here too). engine.h keeps the last message of each
//     type whole and counts arrivals (RadioState::seq), so nothing here needs its own socket.
// ★★ THE GHOST GUARD (spectrum.ts adsbQuietUntil): a report already on the wire when our "off" lands is ignored for 5 s.
// ★★ OFFERED ONLY WHERE THE SERVER SAYS (vibeserver.json): `<mode>` true = offered, false = shown and refused in the
//    server's words (`<mode>Why`), ABSENT = no entry at all — an HF+ reaches no 1090 MHz, and a key that can never work
//    is a dead control (AGENTS.md). ISM also needs its `ismBands`.
// ★★ ENTERING ONE takes the whole radio as the web does: DAB's UI is unwound (the server leaves DAB itself — sending
//    dab:0 too would arm DAB's restore and race the new tune), a running decoder is stopped (it would decode noise).
// ★ THE DIAL: held in ADS-B and AIS (L2/R2 say so), BAND steps in ISM (DAB's settle-then-send stepper, 450 ms), the
//   listener's own in NDB (the beacon band is the capture; tuning stays inside it).
// ★ BOUNDED: lists 150 rows, markers 600, the aircraft-info store 3000 entries, APRS 300 stations, DSC 200 calls.
// ★ Tests: VIBE_HHUI_MAP_CANNED=adsb|ais|ndb|ism|aprs|dsc feeds canned reports through the same readers (no radio
//   traffic, nothing sent) — the fake RTL has clips for some of these, not for all, and the screenshots need all.
#pragma once
#include "map_page.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace hh {

struct MapMode {
    std::string id, label, title, profile;
    int z = 8;
    std::vector<double> rings;
    bool on = false;
    double quietUntil = 0, lastAt = 0, enteredAt = 0;
    uint64_t seenRep = 0, seenOff = 0, seenErr = 0;
    int cap = -1;                     // vibeserver.json: -1 absent · 0 false · 1 true
    std::string why;
    Json canned;                      // tests: the report the canned feed made
};

struct MapAdsbInfo { std::string reg, type, desc, op, route; };
struct MapAprsSt { std::string key, name, kind, line, sym; double lat = NAN, lon = NAN, course = NAN, speed = NAN, seen = 0, at = 0; std::vector<std::pair<double, double>> trail; };
struct MapDscPin { std::string key, line; double lat = 0, lon = 0, at = 0; int level = 0; };

struct MapModes {
    std::vector<MapMode> m;
    bool init = false, seeded = false;
    std::string lastSession;
    // vibeserver.json (worker)
    std::mutex capMtx;
    bool capFetching = false; double capAt = -1e9, capDoneAt = -1e9;
    std::string enterAfterCaps; double enterAskedAt = 0;   // ★ a refused mode, entered again once fresh caps say yes
    std::vector<std::pair<std::string, std::string>> ismBands;   // id, name
    std::string ismLanding;
    // ISM band stepper (main.ts ismStepperFor: the band the person STOPPED on, once, 450 ms after the last press)
    int ismHop = -1; double ismSendAt = 0; double ismAskUntil = 0; int ismAsk = -1;
    // ADS-B aircraft data (adsb_info), per hex
    std::map<std::string, MapAdsbInfo> info;
    uint64_t seenInfo = 0;
    // the decoder box's maps
    std::map<std::string, MapAprsSt> aprs;
    std::deque<MapDscPin> dsc;
    // tests
    std::string canned; double cannedAt = 0; int cannedN = 0;
};
inline MapModes& mapModes() {
    static MapModes x;
    if (!x.init) {
        x.init = true;
        auto mk = [](const char* id, const char* label, const char* title, const char* prof, int z, std::vector<double> rings) {
            MapMode m; m.id = id; m.label = label; m.title = title; m.profile = prof; m.z = z; m.rings = std::move(rings); return m;
        };
        x.m = {mk("adsb", "ADS-B", "ADS-B · 1090 MHz", "aero", 8, {50, 100, 200}),
               mk("ais", "AIS", "AIS · 161.975 + 162.025", "marine", 9, {10, 25, 50}),
               mk("ism", "ISM", "ISM", "spots", 9, {}),
               mk("ndb", "NDB", "NDB · LF BEACONS", "aero", 6, {100, 250, 500})};
        const char* c = std::getenv("VIBE_HHUI_MAP_CANNED");
        x.canned = c ? c : "";
    }
    return x;
}
inline MapMode* mapMode(const std::string& id) { for (auto& m : mapModes().m) if (m.id == id) return &m; return nullptr; }
inline MapMode* mapModeOn() { for (auto& m : mapModes().m) if (m.on) return &m; return nullptr; }
inline const Json& mapReport(const RadioState& s, const MapMode& m) { return mapModes().canned == m.id ? m.canned : s.msg(m.id); }
inline uint64_t mapArrived(const RadioState& s, const std::string& t) { auto it = s.seq.find(t); return it == s.seq.end() ? 0 : it->second; }

/** The server's offer (GET /vibeserver.json, a worker): adsb / ais / ndb / ism, their reasons, the ISM bands. */
inline void mapFetchCaps(int port) {
    MapModes& x = mapModes();
    { std::lock_guard<std::mutex> lk(x.capMtx); if (x.capFetching) return; x.capFetching = true; x.capAt = nowSec(); }
    hwAsync([port] {
        MapModes& x = mapModes();
        const HttpResult r = httpRequest(port, "GET", "/vibeserver.json", "", 4);
        std::lock_guard<std::mutex> lk(x.capMtx);
        x.capFetching = false; x.capDoneAt = nowSec();
        if (r.status != 200) return;
        const Json j = Json::parse(r.body);
        bool inModes = false;
        for (auto& v : j["modes"].a) if (v.str() == "adsb") inModes = true;
        for (auto& m : x.m) {
            const Json& v = j[m.id];
            m.cap = v.kind == Json::Bool ? (v.b ? 1 : 0) : -1;
            if (m.id == "adsb" && inModes) m.cap = 1;
            m.why = j[m.id + "Why"].str().substr(0, 200);
        }
        x.ismBands.clear();
        for (auto& b : j["ismBands"].a) if (b["id"].kind == Json::Str && b["name"].kind == Json::Str && b["hz"].kind == Json::Num && x.ismBands.size() < 8) x.ismBands.push_back({b["id"].s, b["name"].s});
        x.ismLanding = j["ismLanding"].str();
        for (auto& m : x.m) if (m.id == "ism" && x.ismBands.empty()) m.cap = -1;   // ★ no bands = no ISM (main.ts ismReadCap)
    });
}

// ── in and out ───────────────────────────────────────────────────────────────────────────────────────────────────────

inline void mapModeUiOn(App& a, const RadioState& s, MapMode& m) {
    for (auto& o : mapModes().m) if (&o != &m && o.on) { o.on = false; }   // ★ one whole-radio mode at a time (the server's rule)
    m.on = true; m.enteredAt = nowSec();
    // ★ DAB's UI unwound without a message: the server leaves DAB itself (main.ts adsbSetMode)
    if (dabActive()) {
        DabUi& d = dabUi();
        d.on = false; d.haveReport = false; d.restoreArmed = false; d.stepPending = -1; d.askBlock = -1; d.meter.reset();
        d.scopesOn = false; d.slideFull = false;
        dabReleaseBox(a);
    }
    // ★ a running decoder would only decode the noise here (main.ts adsbUiOn: decoders.detach())
    DecUi& du = decUi();
    if (!du.active.empty()) decStop(a, du.active);
    if (m.id == "ism") { mapModes().ismHop = -1; mapModes().ismAsk = -1; }
    if (m.id == "adsb") mapModes().info.clear();
    mapOpenPage(a, "mode:" + m.id);
    std::fprintf(stderr, "vibe-hhui: map mode %s on\n", m.id.c_str());
    (void)s;
}
inline void mapModeUiOff(App& a, MapMode& m) {
    if (!m.on) return;
    m.on = false;
    if (mapUi().page == "mode:" + m.id) mapClosePage(a);
    if (m.id == "adsb") mapModes().info.clear();
    std::fprintf(stderr, "vibe-hhui: map mode %s off\n", m.id.c_str());
}
inline void mapModeEnter(App& a, const RadioState& s, MapMode& m) {
    int cap; std::string why;
    { std::lock_guard<std::mutex> lk(mapModes().capMtx); cap = m.cap; why = m.why; }
    /* ★★ A "NO" IS CHECKED AGAIN BEFORE IT IS SAID (Stuart, 2026-11-08: turned the boosted sample rate on, and ADS-B
     *  still said to enable it — the second try worked). The offer is fetched once a minute, so a setting just changed
     *  was not in it yet. Refused here, it asks the server afresh and enters the moment the fresh answer says yes. */
    if (cap == 0) {
        MapModes& x = mapModes();
        if (x.enterAfterCaps != m.id) {
            x.enterAfterCaps = m.id; x.enterAskedAt = nowSec();
            mapFetchCaps(a.eng_->port());
            a.toast("Checking " + m.label + "…");
            return;
        }
        x.enterAfterCaps.clear();
        a.toastLong(why.empty() ? m.label + " is not available on this receiver" : why);
        return;
    }
    mapModes().enterAfterCaps.clear();
    if (s.session != "running" && mapModes().canned != m.id) { a.toast("No radio — plug one in first"); return; }
    m.quietUntil = 0;
    if (mapModes().canned != m.id) a.eng_->send("{\"type\":\"" + m.id + "\",\"on\":1}");   // ISM: no band = where it last was / the landing band
    mapModeUiOn(a, s, m);
}
inline void mapModeLeave(App& a, MapMode& m) {
    if (mapModes().canned != m.id) a.eng_->send("{\"type\":\"" + m.id + "\",\"on\":0}");
    m.quietUntil = nowSec() + 5;   // ★ the ghost guard
    mapModeUiOff(a, m);
}

// ── the picker's entries ─────────────────────────────────────────────────────────────────────────────────────────────

inline void mapModeEntries(const RadioState& s, std::vector<ModeDef>& ms) {
    MapModes& x = mapModes();
    std::lock_guard<std::mutex> lk(x.capMtx);
    for (auto& m : x.m) {
        if (m.cap < 0 && x.canned != m.id) continue;               // ★ absent = no key (AGENTS.md)
        if (std::find(s.blockedModes.begin(), s.blockedModes.end(), m.id) != s.blockedModes.end()) continue;
        ms.push_back({"map:" + m.id, m.label});
    }
}
inline bool mapModeLit(const std::string& id) { if (id.compare(0, 4, "map:") != 0) return false; const MapMode* m = mapMode(id.substr(4)); return m && m->on; }
inline bool mapModePick(App& a, const RadioState& s, const std::string& id) {
    if (id.compare(0, 4, "map:") != 0) return false;
    MapMode* m = mapMode(id.substr(4));
    if (!m) return true;
    if (m->on) mapOpenPage(a, "mode:" + m->id);   // ★ back to its map (EXIT on the map leaves the mode)
    else mapModeEnter(a, s, *m);
    return true;
}

/** tuneSteps' hook: ADS-B / AIS hold the dial; ISM steps bands; NDB tunes as usual (inside the capture). */
/* ★★ ADS-B GAIN ON THE HANDHELD (Stuart, 2026-11-08: "need the gain control/indication as I dont know what the gain was
 *  doing"). The engine already reports it with every aircraft report — gain (dB), gainAuto, gainWhy (why the automatic
 *  rule chose it), feedWarn (the feed looks dead) — and takes a listener's gain like any mode's: a value pauses the rule
 *  ("set by a listener"), -1 hands it back ("ramping from the middle").
 * ★★ A POP-UP, NOT THE SHOULDERS (Stuart, same day: "remove the gain control off the shoulder buttons as its easy to
 *  accidentally mess with the gain, leave it in a pop up menu on the button"). The header's GAIN key opens a short,
 *  scrolling list — AUTO, then the radio's own gain steps — the current one lit; ENTER sets, BACK leaves it alone. */
inline bool mapAdsbGainPop(App& a, Ctx& c, const RadioState& s) {
    MapUi& u = mapUi();
    const Json& r = mapReport(s, *mapMode("adsb"));
    const bool au = r["gainAuto"].num(0) != 0;
    const double cur = r["gain"].num(s.gainNow / 10.0);
    // rows: AUTO, then the steps high → low (the useful end first)
    std::vector<int> rows{-1};
    for (size_t k = s.gains.size(); k-- > 0;) rows.push_back(s.gains[k]);
    int lit = 0;
    if (!au) { double best = 1e9; for (size_t i = 1; i < rows.size(); i++) if (std::fabs(rows[i] / 10.0 - cur) < best) { best = std::fabs(rows[i] / 10.0 - cur); lit = (int)i; } }
    auto rid = [&](size_t i) { return "map.g." + std::to_string(rows[i]); };
    constexpr size_t kShow = 7;
    const float rh = 36, w = 230, headH = 30;
    const size_t shown = std::min(rows.size(), kShow);
    const Rect p{640 - w - 6, mapl::headH + 4, w, headH + shown * rh + 12};
    if (a.nav_.focus("map").compare(0, 6, "map.g.") != 0) {   // ★ first frame: on the current setting, in view
        a.nav_.setFocus("map", rid((size_t)lit));
        u.popScroll = Scroller{};
        u.popScroll.y = u.popScroll.target = std::max(0.f, std::min(lit * rh - (shown - 1) * rh / 2.f, (rows.size() - shown) * rh));
    }
    c.g.rect(0, 0, 640, 480, rgb(0, 0.35f));
    c.g.rrect(p.x, p.y, p.w, p.h, 8, rgb(0x0d0d0d, 0.98f));
    c.g.rframe(p.x, p.y, p.w, p.h, 8, 1, th::amber);
    c.tc.draw(c.g, c.f.bodyB, "ADS-B GAIN", p.x + 12, p.y + 6, th::btnText);
    const Rect view{p.x + 6, p.y + headH + 4, p.w - 12, shown * rh};
    u.popScroll.contentH = rows.size() * rh;
    int set = -2;
    c.g.clip(view.x - 4, view.y - 4, view.w + 8, view.h + 8);   // (room for the highlight ring)
    for (size_t i = 0; i < rows.size(); i++) {
        const Rect b{view.x, view.y + i * rh - u.popScroll.y, view.w - (rows.size() > shown ? 8.f : 0.f), rh - 2};
        const Nav::Hit h = c.nav.item(rid(i), b);
        if (h.pressed) set = rows[i];
        if (b.b() < view.y - rh || b.y > view.b() + rh) continue;
        char t[48];
        if (rows[i] >= 0) std::snprintf(t, sizeof t, "%.1f dB", rows[i] / 10.0);
        else if (au) std::snprintf(t, sizeof t, "AUTO  (%.1f)", cur);
        else std::snprintf(t, sizeof t, "AUTO");
        c.tc.draw(c.g, c.f.btn, t, b.x + 12, b.cy() - c.f.btn.lineHeight() / 2.f, (int)i == lit ? th::activeTx : rgb(0xdddddd));
        if (h.focused) focusRing(c, b, 5);
    }
    c.g.unclip();
    u.popScroll.follow(c.nav.focusedRect(), view, 4);
    if (rows.size() > shown) {
        const float frac = view.h / u.popScroll.contentH, top = u.popScroll.y / u.popScroll.contentH;
        c.g.rrect(view.r() - 4, view.y + top * view.h, 3, std::max(10.f, frac * view.h), 1.5f, th::dim);
    }
    if (set == -2) return true;
    if (a.eng_) a.eng_->gain(set);
    if (set < 0) a.toast("ADS-B gain automatic — it ramps from the middle");
    else { char t[64]; std::snprintf(t, sizeof t, "ADS-B gain %.1f dB — set by you (AUTO to hand back)", set / 10.0); a.toast(t); }
    return false;
}
inline bool mapTune(App& a, int n) {
    MapMode* m = mapModeOn();
    if (!m || n == 0) return false;
    if (m->id == "ndb") return false;
    if (m->id == "ism") {
        MapModes& x = mapModes();
        std::string band = mapReport(a.eng_->state(), *m)["band"].str();
        int cur = x.ismHop;
        if (cur < 0) for (size_t i = 0; i < x.ismBands.size(); i++) if (x.ismBands[i].first == band) cur = (int)i;
        if (cur < 0 || x.ismBands.size() < 2) return true;
        const int nx = std::max(0, std::min((int)x.ismBands.size() - 1, cur + (n < 0 ? -1 : 1)));   // ★ clamped, never wrapped
        if (nx == cur) return true;
        x.ismHop = nx; x.ismSendAt = nowSec() + 0.45;
        return true;
    }
    if (m->id == "adsb") {   // ★ the dial is held at 1090, and the gain is the GAIN key's pop-up — never the shoulders
        static double saidAt = 0;
        if (nowSec() - saidAt > 3) { saidAt = nowSec(); a.toast("The dial is held at 1090 MHz — the GAIN key sets the gain"); }
        return true;
    }
    static double toldAt = 0;
    if (nowSec() - toldAt > 3) { toldAt = nowSec(); a.toast("The dial is held in " + m->label + " — EXIT " + m->label + " to tune"); }
    return true;
}
inline bool mapZoom(App&, int) { return mapModeOn() != nullptr; }

// ── the readers ──────────────────────────────────────────────────────────────────────────────────────────────────────

inline std::string mapTrim(std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); while (!s.empty() && s[0] == ' ') s.erase(0, 1); return s; }
inline std::string mapFmt(const char* f, double v) { char b[48]; std::snprintf(b, sizeof b, f, v); return b; }
inline std::string mapDist(double lat, double lon) {
    double ml, mo, km, brg;
    if (!mapMe(ml, mo)) return "";
    rangeBearing(ml, mo, lat, lon, km, brg);
    return nmText(km, brg);
}
inline double mapKm(double lat, double lon) { double ml, mo, km, brg; if (!mapMe(ml, mo)) return -1; rangeBearing(ml, mo, lat, lon, km, brg); return km; }
inline bool mapPos(const Json& o, double& lat, double& lon) {
    if (o["lat"].kind != Json::Num || o["lon"].kind != Json::Num) return false;
    lat = o["lat"].n; lon = o["lon"].n;
    return std::fabs(lat) <= 90 && std::fabs(lon) <= 180 && !(lat == 0 && lon == 0);
}
template <class Key> inline void mapSortRows(std::vector<MapListRow>& rows, Key key) {
    std::stable_sort(rows.begin(), rows.end(), [&](const MapListRow& a, const MapListRow& b) { return key(a) < key(b); });
    if (rows.size() > 150) rows.resize(150);
}

// ADS-B ────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** adsblayer.js altHue: red on the ground → yellow 4 000 ft → green 10 000 → cyan 20 000 → blue 30 000 → violet 40 000. */
inline Rgba mapAltColour(double ft) {
    static const double H[][2] = {{0, 15}, {4000, 55}, {10000, 120}, {20000, 180}, {30000, 235}, {40000, 285}};
    if (!(ft >= 0)) ft = 0;
    double h = 285;
    for (int i = 1; i < 6; i++) if (ft <= H[i][0]) { h = H[i - 1][1] + (H[i][1] - H[i - 1][1]) * (ft - H[i - 1][0]) / (H[i][0] - H[i - 1][0]); break; }
    return mapHsl((float)h, 0.9f, 0.62f);
}
inline void mapAdsbInfoTake(const Json& j) {
    MapModes& x = mapModes();
    if (j["full"].boolean()) x.info.clear();
    for (auto& kv : j["aircraft"].o) {
        if (x.info.size() >= 3000) break;
        const Json& v = kv.second;
        MapAdsbInfo i;
        i.reg = v["r"].str().substr(0, 12); i.type = v["t"].str().substr(0, 4); i.desc = v["desc"].str().substr(0, 60);
        i.op = v["opName"].str().substr(0, 60);
        if (v["route"]["from"].kind == Json::Str) i.route = v["route"]["from"].s.substr(0, 4) + " to " + v["route"]["to"].str().substr(0, 4);
        x.info[kv.first.substr(0, 8)] = i;
    }
}
inline void mapAdsbBuild(App& a, const RadioState& s, std::vector<MapListRow>& rows, std::vector<MapMarker>& mk, std::string& status) {
    const Json& r = mapReport(s, *mapMode("adsb"));
    const auto& ac = r["aircraft"].a;
    int withPos = 0;
    for (size_t i = 0; i < ac.size() && i < 2000; i++) {
        const Json& p = ac[i];
        const std::string hex = p["hex"].str().substr(0, 8);
        if (hex.empty()) continue;
        std::string call = mapTrim(p["flight"].str().substr(0, 10));
        const bool ground = p["alt_baro"].str() == "ground";
        const double alt = p["alt_baro"].kind == Json::Num ? p["alt_baro"].n : p["alt_geom"].num(NAN);
        MapListRow row;
        row.key = hex; row.title = call.empty() ? upper(hex) : call;
        row.right = ground ? "GND" : std::isfinite(alt) ? mapFmt("%.0f ft", alt) : "—";
        row.col = ground ? rgb(0xc0c0c0) : mapAltColour(alt);
        std::string sub;
        if (p["gs"].kind == Json::Num) sub = mapFmt("%.0f kt", p["gs"].n);
        double lat, lon;
        if (mapPos(p, lat, lon)) {
            row.pos = true; row.lat = lat; row.lon = lon; withPos++;
            const std::string d = mapDist(lat, lon);
            if (!d.empty()) sub += (sub.empty() ? "" : " · ") + d;
            if (mk.size() < 600) mk.push_back({hex, lat, lon, MG_PLANE, (float)p["track"].num(NAN), row.col, row.title});
        } else sub += (sub.empty() ? "" : " · ") + std::string("no position");
        auto in = mapModes().info.find(hex);
        if (in != mapModes().info.end() && !in->second.type.empty()) sub += " · " + in->second.type;
        if (p["squawk"].kind == Json::Str) { const std::string sq = p["squawk"].s; if (sq == "7700" || sq == "7600" || sq == "7500") { row.col = th::red; sub = "SQUAWK " + sq + " · " + sub; } }
        row.sub = sub;
        rows.push_back(row);
    }
    // ★ nearest first when the receiver has a location, else the most recently heard
    mapSortRows(rows, [](const MapListRow& r) { const double k = r.pos ? mapKm(r.lat, r.lon) : -1; return r.pos ? (k >= 0 ? k : 0) : 1e9; });
    char b[160];
    std::snprintf(b, sizeof b, "%zu aircraft · %d on the map%s", ac.size(), withPos, r["msgRate"].kind == Json::Num ? mapFmt(" · %.0f msg/s", r["msgRate"].n).c_str() : "");
    status = b;
    // ★ the feed looks dead (the engine's warning): said once each time it appears
    { static std::string told; const std::string w = r["feedWarn"].str(); if (!w.empty() && w != told) a.toast(w); told = w; }
    if (r.isNull()) status = "starting — tuning to 1090 MHz…";
}
inline std::vector<std::string> mapAdsbCard(App&, const RadioState& s, const std::string& key) {
    const Json& r = mapReport(s, *mapMode("adsb"));
    for (auto& p : r["aircraft"].a) {
        if (p["hex"].str().substr(0, 8) != key) continue;
        std::vector<std::string> l;
        const std::string call = mapTrim(p["flight"].str());
        auto in = mapModes().info.find(key);
        l.push_back((call.empty() ? upper(key) : call) + (in != mapModes().info.end() && !in->second.reg.empty() ? "  ·  " + in->second.reg : ""));
        if (in != mapModes().info.end()) {
            const MapAdsbInfo& i = in->second;
            std::string t = i.desc.empty() ? i.type : i.desc;
            if (!i.op.empty()) t += (t.empty() ? "" : " · ") + i.op;
            if (!t.empty()) l.push_back(t);
            if (!i.route.empty()) l.push_back("Route " + i.route);
        }
        std::string a1 = p["alt_baro"].str() == "ground" ? "On the ground" : p["alt_baro"].kind == Json::Num ? mapFmt("%.0f ft", p["alt_baro"].n) : "Altitude unknown";
        if (p["baro_rate"].kind == Json::Num && std::fabs(p["baro_rate"].n) > 200) a1 += p["baro_rate"].n > 0 ? mapFmt(" · climbing %.0f ft/min", p["baro_rate"].n) : mapFmt(" · descending %.0f ft/min", -p["baro_rate"].n);
        l.push_back(a1);
        std::string a2;
        if (p["gs"].kind == Json::Num) a2 = mapFmt("%.0f kt", p["gs"].n);
        if (p["track"].kind == Json::Num) a2 += mapFmt(" · track %03.0f°", p["track"].n);
        if (p["squawk"].kind == Json::Str) a2 += " · squawk " + p["squawk"].s;
        if (!a2.empty()) l.push_back(a2);
        double lat, lon;
        std::string a3 = "ICAO " + upper(key);
        if (mapPos(p, lat, lon)) { const std::string d = mapDist(lat, lon); if (!d.empty()) a3 += " · " + d; }
        if (p["seen"].kind == Json::Num) a3 += " · heard " + agoText(p["seen"].n) + (p["seen"].n < 5 ? "" : " ago");
        l.push_back(a3);
        return l;
    }
    return {};
}

// AIS ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline Rgba mapShipColour(const std::string& fam) {
    if (fam == "cargo") return rgb(0x6fdc6f);
    if (fam == "tanker") return rgb(0xff6b5b);
    if (fam == "passenger") return rgb(0x5bb6ff);
    if (fam == "fishing") return rgb(0xffa64d);
    if (fam == "pleasure" || fam == "sailing") return rgb(0xd88cff);
    if (fam == "tug" || fam == "special") return rgb(0x4de0d0);
    if (fam == "hsc") return rgb(0xffe14d);
    return rgb(0xc8c8c8);
}
inline void mapAisBuild(App&, const RadioState& s, std::vector<MapListRow>& rows, std::vector<MapMarker>& mk, std::string& status) {
    const Json& r = mapReport(s, *mapMode("ais"));
    const auto& vs = r["vessels"].a;
    int withPos = 0;
    for (size_t i = 0; i < vs.size() && i < 1500; i++) {
        const Json& v = vs[i];
        const double mmsi = v["mmsi"].num(0);
        if (mmsi <= 0) continue;
        MapListRow row;
        row.key = mapFmt("%.0f", mmsi);
        const std::string name = mapTrim(v["name"].str().substr(0, 40));
        const bool aton = v["aton"].kind == Json::Num || v["kind"].str() == "aton";
        row.title = name.empty() ? "MMSI " + row.key : name;
        row.col = aton ? rgb(0xff66cc) : mapShipColour(v["family"].str());
        const double sog = v["sog"].num(NAN);
        row.right = aton ? "AtoN" : std::isfinite(sog) ? mapFmt("%.1f kn", sog) : "";
        std::string sub = v["stypeName"].str().substr(0, 30);
        if (sub.empty()) sub = v["navName"].str().substr(0, 30);
        double lat, lon;
        if (mapPos(v, lat, lon)) {
            row.pos = true; row.lat = lat; row.lon = lon; withPos++;
            const std::string d = mapDist(lat, lon);
            if (!d.empty()) sub += (sub.empty() ? "" : " · ") + d;
            const double hd = v["hdg"].kind == Json::Num && v["hdg"].n < 360 ? v["hdg"].n : v["cog"].num(NAN);
            const int g = aton ? MG_ATON : (std::isfinite(sog) && sog >= 0.5 && std::isfinite(hd)) ? MG_SHIP : MG_MOORED;
            if (mk.size() < 600) mk.push_back({row.key, lat, lon, g, (float)hd, row.col, row.title});
        } else sub += (sub.empty() ? "" : " · ") + std::string("no position yet");
        row.sub = sub;
        rows.push_back(row);
    }
    mapSortRows(rows, [](const MapListRow& r) { const double k = r.pos ? mapKm(r.lat, r.lon) : -1; return r.pos ? (k >= 0 ? k : 0) : 1e9; });
    char b[160];
    std::snprintf(b, sizeof b, "%zu ships · %d on the map · A %.0f / B %.0f msgs", vs.size(), withPos, r["chA"].num(0), r["chB"].num(0));
    status = r.isNull() ? "starting — tuning to the AIS channels…" : b;
}
inline std::vector<std::string> mapAisCard(App&, const RadioState& s, const std::string& key) {
    const Json& r = mapReport(s, *mapMode("ais"));
    for (auto& v : r["vessels"].a) {
        if (mapFmt("%.0f", v["mmsi"].num(0)) != key) continue;
        std::vector<std::string> l;
        const std::string name = mapTrim(v["name"].str());
        l.push_back((name.empty() ? "MMSI " + key : name) + (v["call"].str().empty() ? "" : "  ·  " + v["call"].str()));
        std::string t = v["stypeName"].str();
        if (!v["country"].str().empty()) t += (t.empty() ? "" : " · ") + v["country"].str();
        if (!t.empty()) l.push_back(t);
        std::string n = v["navName"].str();
        if (v["sog"].kind == Json::Num) n += (n.empty() ? "" : " · ") + mapFmt("%.1f kn", v["sog"].n);
        if (v["cog"].kind == Json::Num) n += mapFmt(" · course %03.0f°", v["cog"].n);
        if (!n.empty()) l.push_back(n);
        if (!v["dest"].str().empty()) l.push_back("To " + v["dest"].str() + (v["eta"].str().empty() ? "" : " · ETA " + v["eta"].str()));
        double lat, lon;
        std::string d = "MMSI " + key;
        if (mapPos(v, lat, lon)) { const std::string dd = mapDist(lat, lon); if (!dd.empty()) d += " · " + dd; }
        if (v["seen"].kind == Json::Num) d += " · heard " + agoText(v["seen"].n) + (v["seen"].n < 5 ? "" : " ago");
        l.push_back(d);
        return l;
    }
    return {};
}

// NDB ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────
/** ndb.ts ndbBars: < 10 dB one, < 20 two, else three; 0 = not heard in the last minute. */
inline int mapNdbBars(double snr, bool live) { if (!live) return 0; return snr < 10 ? 1 : snr < 20 ? 2 : 3; }
inline void mapNdbBuild(App&, const RadioState& s, std::vector<MapListRow>& rows, std::vector<MapMarker>& mk, std::string& status) {
    const Json& r = mapReport(s, *mapMode("ndb"));
    std::map<std::string, bool> drawn;
    for (auto& b : r["beacons"].a) {
        if (rows.size() >= 200) break;
        MapListRow row;
        const std::string id = b["ident"].str().substr(0, 6);
        row.key = "b" + mapFmt("%.0f", b["id"].num(0));
        row.title = (id.empty() ? std::string("?") : id) + "  " + mapFmt("%.1f kHz", b["hz"].num(0) / 1e3);
        const int bars = mapNdbBars(b["snr"].num(0), b["active"].boolean());
        row.right = bars == 0 ? "—" : std::string((size_t)bars, '|') + mapFmt(" %.0f dB", b["snr"].num(0));
        const Json& db = b["db"];
        const bool match = db["k"].str() == "match";
        row.col = match ? rgb(0x7cff9b) : !id.empty() ? th::amber : rgb(0xbbbbbb);
        std::string sub = match ? db["name"].str().substr(0, 40) : !id.empty() ? std::string("not in the published list") : "reading the ident…";
        double lat, lon;
        if (mapPos(db, lat, lon)) {
            row.pos = true; row.lat = lat; row.lon = lon;
            const std::string d = db["km"].num(-1) >= 0 ? nmText(db["km"].n, db["brg"].num(0)) : mapDist(lat, lon);
            if (!d.empty()) sub += " · " + d;
            if (mk.size() < 600) mk.push_back({row.key, lat, lon, MG_NDB, NAN, bars ? rgb(0xd68cff) : rgb(0x9a7aa8), id});
            drawn[id + mapFmt("%.0f", b["hz"].num(0))] = true;
        }
        row.sub = sub;
        rows.push_back(row);
    }
    // ★ the log: beacons heard earlier this session (dimmer), on the map too
    for (auto& e : r["log"].a) {
        double lat, lon;
        const std::string id = e["ident"].str().substr(0, 6);
        if (!mapPos(e, lat, lon) || drawn.count(id + mapFmt("%.0f", e["hz"].num(0))) || mk.size() >= 600) continue;
        mk.push_back({"l" + id + mapFmt("%.0f", e["hz"].num(0)), lat, lon, MG_NDB, NAN, rgb(0x8a6a98), id});
    }
    char b[160];
    std::snprintf(b, sizeof b, "%.0f carriers · %.0f identified · %.0f–%.0f kHz", r["carriers"].num(0), r["identified"].num(0), r["lo"].num(0) / 1e3, r["hi"].num(0) / 1e3);
    status = r.isNull() ? "starting — the beacon band…" : r["starting"].boolean() ? "starting the skimmer…" : b;
}
inline std::vector<std::string> mapNdbCard(App&, const RadioState& s, const std::string& key) {
    const Json& r = mapReport(s, *mapMode("ndb"));
    for (auto& b : r["beacons"].a) {
        if ("b" + mapFmt("%.0f", b["id"].num(0)) != key) continue;
        std::vector<std::string> l;
        const Json& db = b["db"];
        l.push_back((b["ident"].str().empty() ? std::string("Unidentified") : b["ident"].str()) + "  ·  " + mapFmt("%.1f kHz", b["hz"].num(0) / 1e3));
        if (db["k"].str() == "match") l.push_back(db["name"].str() + (db["cc"].str().empty() ? "" : " (" + db["cc"].str() + ")"));
        else if (!b["ident"].str().empty()) l.push_back("Heard, but not in the published list");
        if (!b["copy"].str().empty()) l.push_back("Copy: " + b["copy"].str());
        std::string d = mapFmt("SNR %.0f dB", b["snr"].num(0));
        if (b["tone"].kind == Json::Num && b["tone"].n > 0) d += mapFmt(" · tone %.0f Hz", b["tone"].n);
        if (db["km"].num(-1) >= 0) d += " · " + nmText(db["km"].n, db["brg"].num(0));
        l.push_back(d);
        if (!b["unlikely"].str().empty()) l.push_back("Unlikely: " + b["unlikely"].str());
        return l;
    }
    return {};
}

// ISM ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────
inline void mapIsmBuild(App&, const RadioState& s, std::vector<MapListRow>& rows, std::vector<MapMarker>& mk, std::string& status) {
    const Json& r = mapReport(s, *mapMode("ism"));
    int withPos = 0;
    for (auto& d : r["devices"].a) {
        if (rows.size() >= 300) break;
        if (d["line"].kind != Json::Str) continue;
        MapListRow row;
        row.key = d["key"].str().substr(0, 24);
        std::string name = d["name"].str().substr(0, 60);
        if (name.empty()) name = mapTrim(d["make"].str().substr(0, 30) + " " + d["model"].str().substr(0, 40));
        row.title = name.empty() ? d["kind"].str() : name;
        row.right = agoText(d["age"].num(0));
        row.sub = d["line"].str().substr(0, 120);
        const std::string kind = d["kind"].str();
        row.col = kind == "meshtastic" || kind == "meshcore" || kind == "lorawan" ? rgb(0x7cff9b) : kind == "meter" ? rgb(0x9fb3c8) : th::amber;
        double lat, lon;
        if (mapPos(d, lat, lon)) {
            row.pos = true; row.lat = lat; row.lon = lon; withPos++;
            const std::string short_ = d["short"].str().substr(0, 8);
            if (mk.size() < 600) mk.push_back({row.key, lat, lon, MG_NODE, NAN, row.col, short_.empty() ? row.title.substr(0, 16) : short_});
        }
        rows.push_back(row);
    }
    MapModes& x = mapModes();
    std::string band = r["bandName"].str();
    if (x.ismHop >= 0 && x.ismHop < (int)x.ismBands.size()) band = x.ismBands[(size_t)x.ismHop].second + " …";
    char b[200];
    std::snprintf(b, sizeof b, "%s · %zu devices · %d on the map · L2/R2 band", band.c_str(), r["devices"].size(), withPos);
    status = r.isNull() ? "starting the band…" : b;
}
inline std::vector<std::string> mapIsmCard(App&, const RadioState& s, const std::string& key) {
    const Json& r = mapReport(s, *mapMode("ism"));
    for (auto& d : r["devices"].a) {
        if (d["key"].str().substr(0, 24) != key) continue;
        std::vector<std::string> l;
        std::string name = d["name"].str();
        if (name.empty()) name = mapTrim(d["make"].str() + " " + d["model"].str());
        l.push_back(name.empty() ? d["kind"].str() : name);
        l.push_back(d["line"].str().substr(0, 200));
        l.push_back(mapFmt("%.3f MHz", d["freq"].num(0) / 1e6) + mapFmt(" · %.0f dB SNR", d["snr"].num(0)) + " · heard " + agoText(d["age"].num(0)) + " ago");
        double lat, lon;
        if (mapPos(d, lat, lon)) { const std::string dd = mapDist(lat, lon); l.push_back(mapFmt("%.4f", lat) + ", " + mapFmt("%.4f", lon) + (dd.empty() ? "" : " · " + dd)); }
        return l;
    }
    return {};
}

// APRS / DSC (the decoder box's maps) ──────────────────────────────────────────────────────────────────────────────────
/** The decoder socket's aprs_list / dsc_call (dec_state.h decOnJson calls this): the positions, bounded. */
inline void mapDecFeed(App&, const std::string& type, const Json& d) {
    MapModes& x = mapModes();
    const double now = nowSec();
    if (type == "aprs_list") {
        std::map<std::string, MapAprsSt> next;
        for (auto& s : d["stations"].a) {
            if (next.size() >= 300) break;
            MapAprsSt st;
            st.key = s["key"].str().substr(0, 40);
            if (st.key.empty()) continue;
            st.name = (s["name"].str().empty() ? s["from"].str() : s["name"].str()).substr(0, 24);
            st.kind = s["kind"].str().substr(0, 14); st.line = decAscii(s["line"].str().substr(0, 200)); st.sym = s["symName"].str().substr(0, 40);
            mapPos(s, st.lat, st.lon) || (st.lat = NAN, true);
            st.course = s["course"].num(NAN); st.speed = s["speed"].num(NAN); st.seen = s["seen"].num(0); st.at = now;
            for (auto& p : s["trail"].a) if (p.size() >= 2 && st.trail.size() < 200) st.trail.push_back({p[0].num(), p[1].num()});
            next[st.key] = std::move(st);
        }
        x.aprs.swap(next);
    } else if (type == "dsc_call") {
        double lat, lon;
        if (!mapPos(d, lat, lon)) return;
        MapDscPin p;
        p.key = mapFmt("%.0f", d["id"].num(0)) + "." + mapFmt("%.0f", d["rf"].num(0));
        p.line = decAscii(d["line"].str().substr(0, 300)); p.lat = lat; p.lon = lon; p.at = now;   // ★ "A → B" in ASCII (Atkinson has no arrow)
        p.level = std::max(0, std::min(3, (int)d["level"].num(0)));
        for (auto it = x.dsc.begin(); it != x.dsc.end(); ++it) if (it->key == p.key) { x.dsc.erase(it); break; }
        x.dsc.push_front(p);
        while (x.dsc.size() > 200) x.dsc.pop_back();
    }
}
inline void mapAprsBuild(App&, const RadioState&, std::vector<MapListRow>& rows, std::vector<MapMarker>& mk, std::string& status) {
    MapModes& x = mapModes();
    int withPos = 0;
    for (auto& kv : x.aprs) {
        const MapAprsSt& st = kv.second;
        MapListRow row;
        row.key = st.key; row.title = st.name.empty() ? st.key : st.name;
        row.right = agoText(st.seen + (nowSec() - st.at));
        row.sub = st.line;
        row.col = st.kind == "weather" ? rgb(0x5bb6ff) : st.kind == "object" || st.kind == "item" ? rgb(0xd88cff) : th::amber;
        if (std::isfinite(st.lat)) {
            row.pos = true; row.lat = st.lat; row.lon = st.lon; withPos++;
            mk.push_back({st.key, st.lat, st.lon, MG_STATION, NAN, row.col, row.title});
        }
        rows.push_back(row);
    }
    mapSortRows(rows, [](const MapListRow& r) { return r.right == "now" ? 0.0 : (double)r.right.size(); });
    char b[120]; std::snprintf(b, sizeof b, "%zu stations · %d on the map", x.aprs.size(), withPos);
    status = b;
}
inline std::vector<std::string> mapAprsCard(App&, const RadioState&, const std::string& key) {
    auto it = mapModes().aprs.find(key);
    if (it == mapModes().aprs.end()) return {};
    const MapAprsSt& st = it->second;
    std::vector<std::string> l{st.name.empty() ? st.key : st.name};
    if (!st.sym.empty()) l.push_back(st.sym + (st.kind.empty() ? "" : " · " + st.kind));
    if (!st.line.empty()) l.push_back(st.line);
    std::string m;
    if (std::isfinite(st.speed)) m = mapFmt("%.0f km/h", st.speed);
    if (std::isfinite(st.course)) m += mapFmt(" · course %03.0f°", st.course);
    if (std::isfinite(st.lat)) { const std::string d = mapDist(st.lat, st.lon); if (!d.empty()) m += (m.empty() ? "" : " · ") + d; }
    if (!m.empty()) l.push_back(m);
    return l;
}
inline void mapDscBuild(App&, const RadioState&, std::vector<MapListRow>& rows, std::vector<MapMarker>& mk, std::string& status) {
    static const Rgba lv[4] = {rgb(0x7cff9b), rgb(0xffd24d), rgb(0xff9a3c), rgb(0xff4d4d)};   // routine · safety · urgency · distress
    for (auto& p : mapModes().dsc) {
        MapListRow row;
        // ★ "DISTRESS · MMSI … · nature · position": the first two parts the title, the rest beneath
        size_t cut = p.line.find(" · ");
        if (cut != std::string::npos) cut = p.line.find(" · ", cut + 3);
        row.key = p.key; row.title = cut == std::string::npos ? p.line : p.line.substr(0, cut);
        row.sub = cut == std::string::npos ? "" : p.line.substr(cut + 4 > p.line.size() ? p.line.size() : cut + 4);
        row.right = agoText(nowSec() - p.at); row.col = lv[p.level]; row.pos = true; row.lat = p.lat; row.lon = p.lon;
        rows.push_back(row);
        mk.push_back({p.key, p.lat, p.lon, MG_PIN, NAN, lv[p.level], ""});
    }
    // ★ distress first, then urgency, safety, routine — newest first within each
    std::stable_sort(rows.begin(), rows.end(), [](const MapListRow& a, const MapListRow& b) { return a.title.compare(0, 8, "DISTRESS") == 0 && b.title.compare(0, 8, "DISTRESS") != 0; });
    status = mapFmt("%.0f calls with a position", (double)mapModes().dsc.size());
}
inline std::vector<std::string> mapDscCard(App&, const RadioState&, const std::string& key) {
    for (auto& p : mapModes().dsc) if (p.key == key) return {p.level == 3 ? "DISTRESS" : p.level == 2 ? "URGENCY" : p.level == 1 ? "SAFETY" : "ROUTINE", p.line,
                                                         mapFmt("%.4f", p.lat) + ", " + mapFmt("%.4f", p.lon) + " · " + mapDist(p.lat, p.lon)};
    return {};
}
/** The decoder box's MAP key (APRS, DSC): the map of what the decoder has heard; the decoder runs on under it. */
// ── DIGITAL SPOTS ───────────────────────────────────────────────────────────────────────────────────────────────────
// ★★ THE SPOTS MAP (Stuart, 2026-11-07: "is there a map view for FT8 etc?" — the app's and the web's DIGITAL SPOTS MAP):
//    every station the digital spots heard, at the centre of its grid square, coloured by mode; the list beside it newest
//    first (call · mode · band · SNR · distance), the selected one's card. From decUi().spots — what the box already holds.
inline Rgba mapSpotColour(const std::string& m0) {
    std::string m; for (char ch : m0) m += (char)std::toupper((unsigned char)ch);
    if (m == "FT8") return rgb(0x5be36b);
    if (m == "FT4") return rgb(0x46e0ff);
    if (m == "WSPR") return rgb(0xff7ad9);
    if (m == "JS8") return rgb(0xffb833);
    return th::amber;
}
inline std::string mapSpotKey(const DecSpot& sp) { return sp.call + "|" + sp.band + "|" + sp.mode; }
inline void mapSpotsBuild(App&, const RadioState&, std::vector<MapListRow>& rows, std::vector<MapMarker>& mk, std::string& status) {
    const DecUi& d = decUi();
    const double now = (double)std::time(nullptr);
    int withPos = 0;
    for (const DecSpot& sp : d.spots) {
        if (rows.size() >= 300) break;   // ★ bounded — the newest 300 (the box keeps one per call · band · mode)
        MapListRow row;
        row.key = mapSpotKey(sp); row.title = sp.call.empty() ? "?" : sp.call;
        row.right = agoText(std::max(0.0, now - sp.at));
        row.col = mapSpotColour(sp.mode);
        std::string sub = sp.mode + (sp.band.empty() ? "" : " · " + sp.band) + mapFmt(" · %+.0f dB", sp.snr);
        double lat, lon;
        if (!sp.grid.empty() && mapGridCentre(sp.grid, lat, lon)) {
            row.pos = true; row.lat = lat; row.lon = lon; withPos++;
            const std::string dist = mapDist(lat, lon);
            sub += " · " + sp.grid + (dist.empty() ? "" : " · " + dist);
            mk.push_back({row.key, lat, lon, MG_DOT, NAN, row.col, row.title});
        } else if (!sp.country.empty()) sub += " · " + sp.country;
        row.sub = sub;
        rows.push_back(row);
    }
    char b[120]; std::snprintf(b, sizeof b, "%zu spots · %d on the map", d.spots.size(), withPos);
    status = b;
}
inline std::vector<std::string> mapSpotsCard(App&, const RadioState&, const std::string& key) {
    for (const DecSpot& sp : decUi().spots) {
        if (mapSpotKey(sp) != key) continue;
        std::vector<std::string> l{sp.call};
        l.push_back(sp.mode + (sp.band.empty() ? "" : " · " + sp.band) + mapFmt(" · %+.0f dB", sp.snr) + mapFmt(" · %.3f MHz", sp.hz / 1e6));
        double lat, lon;
        std::string where = sp.country;
        if (!sp.grid.empty() && mapGridCentre(sp.grid, lat, lon)) {
            const std::string dist = mapDist(lat, lon);
            where = sp.grid + (sp.country.empty() ? "" : " · " + sp.country) + (dist.empty() ? "" : " · " + dist);
        }
        if (!where.empty()) l.push_back(where);
        const std::string ago = agoText(std::max(0.0, (double)std::time(nullptr) - sp.at));
        l.push_back(ago == "now" ? "heard just now" : "heard " + ago + " ago");
        return l;
    }
    return {};
}

inline void mapOpenDecoder(App& a, const std::string& view) {
    if (view == "aprs") mapOpenPage(a, "dec:aprs");
    else if (view == "dsc") mapOpenPage(a, "dec:dsc");
    else if (view == "spots") mapOpenPage(a, "dec:spots");
}

// ── registration, the tick ───────────────────────────────────────────────────────────────────────────────────────────

inline void mapRegisterModes() {
    static bool done = false;
    if (done) return;
    done = true;
    for (auto& m : mapModes().m) {
        MapPageDef d;
        d.id = "mode:" + m.id; d.title = m.title; d.profile = m.profile; d.startZ = m.z; d.ringsNm = m.rings; d.exitLabel = "EXIT " + m.label;
        const std::string id = m.id;
        d.exit = [id](App& a, const RadioState&) { if (MapMode* x = mapMode(id)) mapModeLeave(a, *x); };
        if (id == "adsb") {
            d.build = mapAdsbBuild; d.card = mapAdsbCard; d.empty = "listening on 1090 MHz — aircraft appear as they are heard";
            // ★ GAIN 37.2 AUTO / GAIN 40.2 — the key hands the gain back to the automatic rule
            d.keys = [](App& a, const RadioState& s, std::vector<std::pair<std::string, std::function<void()>>>& k) {
                const Json& r = mapReport(s, *mapMode("adsb"));
                if (r["gain"].kind != Json::Num) return;
                const bool au = r["gainAuto"].num(0) != 0;
                k.push_back({mapFmt("GAIN %.1f", r["gain"].n) + (au ? " AUTO" : ""), [&a, &s] {
                    if (s.gains.empty()) { a.toast("This radio has no gain steps to set"); return; }
                    mapUi().pop = mapAdsbGainPop;
                }});
            };
        }
        if (id == "ais") { d.build = mapAisBuild; d.card = mapAisCard; d.empty = "listening on 161.975 and 162.025 MHz — ships appear as they report"; }
        if (id == "ndb") { d.build = mapNdbBuild; d.card = mapNdbCard; d.empty = "skimming the beacon band — each carrier is listed as it is found, its ident as it is read"; }
        if (id == "ism") { d.build = mapIsmBuild; d.card = mapIsmCard; d.empty = "listening — sensors, meters and mesh nodes appear as they are decoded"; d.listFirst = true; }
        mapRegisterPage(d);
    }
    MapPageDef ap; ap.id = "dec:aprs"; ap.title = "APRS MAP"; ap.profile = "spots"; ap.startZ = 8; ap.build = mapAprsBuild; ap.card = mapAprsCard;
    ap.empty = "no stations with a position yet — the APRS decoder runs on under the map"; ap.exitLabel = "CLOSE";
    mapRegisterPage(ap);
    MapPageDef dp; dp.id = "dec:dsc"; dp.title = "DSC MAP"; dp.profile = "marine"; dp.startZ = 6; dp.build = mapDscBuild; dp.card = mapDscCard;
    dp.empty = "no DSC call with a position yet — distress calls carry one"; dp.exitLabel = "CLOSE";
    mapRegisterPage(dp);
    MapPageDef sp; sp.id = "dec:spots"; sp.title = "DIGITAL SPOTS MAP"; sp.profile = "spots"; sp.startZ = 3; sp.build = mapSpotsBuild;
    sp.card = mapSpotsCard; sp.empty = "no spots yet — the digital spots run on under the map; each station is placed on its grid square";
    sp.exitLabel = "CLOSE";
    mapRegisterPage(sp);
}

inline void mapCannedTick(App& a, const RadioState& s);

/** Once a loop, after eng.pump (dabTick's neighbour): arrivals, the ISM stepper, the offer, the canned feed. */
inline void mapTick(App& a, const RadioState& s) {
    MapModes& x = mapModes();
    mapRegisterModes();
    mapIdle(a);
    const double now = nowSec();
    // the offer: at the session's start, then once a minute
    if (s.session != x.lastSession) { x.lastSession = s.session; if (s.session == "running") x.capAt = -1e9; else for (auto& m : x.m) if (m.on && x.canned != m.id) mapModeUiOff(a, m); }
    if (s.session == "running" && now - x.capAt > 60) mapFetchCaps(a.eng_->port());
    // ★ the refused mode's second look (mapModeEnter): once a fetch made after the ask has answered
    if (!x.enterAfterCaps.empty()) {
        bool answered, stale;
        { std::lock_guard<std::mutex> lk(x.capMtx); answered = !x.capFetching && x.capAt >= x.enterAskedAt && x.capDoneAt > x.enterAskedAt;
          stale = !x.capFetching && x.capAt < x.enterAskedAt; }   // ★ a fetch begun before the ask does not count
        if (stale) mapFetchCaps(a.eng_->port());
        else if (answered) { if (MapMode* m = mapMode(x.enterAfterCaps)) mapModeEnter(a, s, *m); else x.enterAfterCaps.clear(); }
        else if (now - x.enterAskedAt > 6) { x.enterAfterCaps.clear(); a.toast("The receiver did not answer — try again"); }
    }
    // arrivals (seeded once, so a report from before this UI started does not count as new)
    for (auto& m : x.m) {
        const uint64_t rep = mapArrived(s, m.id), off = mapArrived(s, m.id + "_off"), err = mapArrived(s, m.id + "_error");
        if (!x.seeded) { m.seenRep = rep; m.seenOff = off; m.seenErr = err; continue; }
        if (err != m.seenErr) { m.seenErr = err; const std::string why = s.msg(m.id + "_error")["why"].str(); a.toastLong(why.empty() ? m.label + " is not available on this receiver" : why.substr(0, 200)); mapModeUiOff(a, m); }
        if (off != m.seenOff) { m.seenOff = off; if (now >= m.quietUntil || m.on) mapModeUiOff(a, m); }
        if (rep != m.seenRep) {
            m.seenRep = rep;
            if (now < m.quietUntil) continue;          // ★ a ghost of the mode we left
            m.lastAt = now;
            if (!m.on) mapModeUiOn(a, s, m);           // ★ somebody else put the receiver in this mode (or we joined it)
        }
    }
    x.seeded = true;
    // aircraft data (arrives just before the report it belongs to)
    { const uint64_t n = mapArrived(s, "adsb_info"); if (n != x.seenInfo) { x.seenInfo = n; if (MapMode* m = mapMode("adsb"); m && m->on) mapAdsbInfoTake(s.msg("adsb_info")); } }
    // ISM: the band the person stopped on, once
    if (x.ismHop >= 0 && x.ismSendAt > 0 && now >= x.ismSendAt) {
        x.ismSendAt = 0;
        if (MapMode* m = mapMode("ism"); m && m->on && x.ismHop < (int)x.ismBands.size()) {
            if (x.canned != "ism") a.eng_->send("{\"type\":\"ism\",\"on\":1,\"band\":" + jsonQuote(x.ismBands[(size_t)x.ismHop].first) + "}");
            x.ismAsk = x.ismHop; x.ismAskUntil = now + 4;
        }
    }
    if (x.ismAsk >= 0) {
        // ★ the label lets go to the server's once it names ours, or the hold runs out (DabAskGuard)
        MapMode* m = mapMode("ism");
        const std::string band = m ? mapReport(s, *m)["band"].str() : "";
        if ((x.ismAsk < (int)x.ismBands.size() && band == x.ismBands[(size_t)x.ismAsk].first) || now > x.ismAskUntil) { x.ismAsk = -1; x.ismHop = -1; }
    }
    // a whole-radio mode keeps its map up: a sheet that went (the menu took the screen) comes back when it closes
    if (MapMode* m = mapModeOn(); m && a.sheets_.empty()) mapOpenPage(a, "mode:" + m->id);
    if (!x.canned.empty()) mapCannedTick(a, s);
    // ★ tests: VIBE_HHUI_MAP_OPEN=pick | mode:<id> | dec:aprs | dec:dsc opens that page once, a second in
    static const char* open = std::getenv("VIBE_HHUI_MAP_OPEN");
    static double openAt = now + 1.0;
    if (open && *open && now >= openAt) {
        const std::string o = open; open = nullptr;
        if (o == "pick") mapOpenPicker(a);
        else if (o.compare(0, 5, "mode:") == 0) { if (MapMode* m = mapMode(o.substr(5))) mapModeEnter(a, s, *m); }
        else mapOpenPage(a, o);
        // ★ …and VIBE_HHUI_MAP_AT="lat,lon,zoom" puts the view there
        if (const char* at = std::getenv("VIBE_HHUI_MAP_AT")) { double la = 0, lo = 0; int z = 8; if (std::sscanf(at, "%lf,%lf,%d", &la, &lo, &z) >= 2) mapUi().view.centreOn(la, lo, z); mapUi().centredOnce = true; }
    }
}

// ── tests: the canned feed ───────────────────────────────────────────────────────────────────────────────────────────

inline void mapCannedTick(App& a, const RadioState& s) {
    MapModes& x = mapModes();
    const double now = nowSec();
    if (now - x.cannedAt < 1) return;
    x.cannedAt = now;
    const int k = x.cannedN++;
    double lat0 = 52.24, lon0 = -0.90;           // Northampton (the receiver, if none is set)
    mapMe(lat0, lon0);
    std::string j;
    char b[512];
    if (x.canned == "adsb") {
        j = "{\"type\":\"adsb\",\"msgRate\":143,\"gain\":40.2,\"gainAuto\":1,\"aircraft\":[";
        const char* calls[] = {"BAW123", "EZY45KP", "RYR8KJ", "KLM1007", "DLH4TC", "VIR25", "SHT7", "TOM62", "", "EXS33"};
        for (int i = 0; i < 10; i++) {
            const double ang = i * 0.63 + k * 0.01, rkm = 15 + i * 18;
            const double la = lat0 + rkm / 111.32 * std::cos(ang), lo = lon0 + rkm / (111.32 * std::cos(lat0 * M_PI / 180)) * std::sin(ang);
            std::snprintf(b, sizeof b, "%s{\"hex\":\"40%04x\",\"flight\":\"%-8s\",\"lat\":%.5f,\"lon\":%.5f,\"alt_baro\":%d,\"gs\":%d,\"track\":%.0f,\"seen\":%d,\"squawk\":\"%s\"}",
                          i ? "," : "", 0x6a00 + i * 37, calls[i], la, lo, i == 6 ? 1200 : 3000 + i * 4000, 180 + i * 30, std::fmod(ang * 57.3 + 90, 360), i % 3, i == 4 ? "7700" : "2000");
            j += b;
        }
        j += ",{\"hex\":\"4ca9f1\",\"alt_baro\":36000,\"gs\":450,\"seen\":2}]}";
        if (k == 0) mapAdsbInfoTake(Json::parse("{\"type\":\"adsb_info\",\"full\":1,\"aircraft\":{\"406a00\":{\"r\":\"G-EUPT\",\"t\":\"A319\",\"desc\":\"AIRBUS A-319\",\"opName\":\"British Airways\",\"route\":{\"from\":\"EGLL\",\"to\":\"LEMD\"}}}}"));
    } else if (x.canned == "ais") {
        j = "{\"type\":\"ais\",\"chA\":512,\"chB\":498,\"vessels\":[";
        const char* names[] = {"EVER GIVEN", "STENA BRITANNICA", "PRIDE OF KENT", "SVITZER MARS", "MAERSK ESSEN", "", "NORTH FORELAND LT", "JENNY"};
        const char* fam[] = {"cargo", "passenger", "passenger", "tug", "cargo", "fishing", "", "pleasure"};
        for (int i = 0; i < 8; i++) {
            const double la = 51.2 + i * 0.05 + k * 0.0004, lo = 1.3 + i * 0.07;
            std::snprintf(b, sizeof b, "%s{\"mmsi\":%d,\"name\":\"%s\",\"family\":\"%s\",\"stypeName\":\"%s\",\"lat\":%.5f,\"lon\":%.5f,\"sog\":%.1f,\"cog\":%d,\"hdg\":%d,\"seen\":%d%s}",
                          i ? "," : "", 235000100 + i * 11, names[i], fam[i], fam[i], la, lo, i == 3 ? 0.1 : 4.0 + i * 2, 40 + i * 35, 40 + i * 35, i, i == 6 ? ",\"aton\":1" : "");
            j += b;
        }
        j += "]}";
    } else if (x.canned == "ndb") {
        j = "{\"type\":\"ndb\",\"lo\":255000,\"hi\":526500,\"carriers\":14,\"identified\":5,\"beacons\":["
            "{\"id\":1,\"hz\":338000,\"snr\":24,\"active\":true,\"ident\":\"CT\",\"db\":{\"k\":\"match\",\"name\":\"Coventry\",\"cc\":\"GB\",\"lat\":52.41,\"lon\":-1.4,\"km\":40,\"brg\":300}},"
            "{\"id\":2,\"hz\":385000,\"snr\":14,\"active\":true,\"ident\":\"WCO\",\"db\":{\"k\":\"match\",\"name\":\"Westcott\",\"cc\":\"GB\",\"lat\":51.85,\"lon\":-0.96,\"km\":45,\"brg\":190}},"
            "{\"id\":3,\"hz\":400000,\"snr\":8,\"active\":true,\"ident\":\"OX\",\"db\":{\"k\":\"match\",\"name\":\"Oxford\",\"cc\":\"GB\",\"lat\":51.84,\"lon\":-1.32,\"km\":52,\"brg\":215}},"
            "{\"id\":4,\"hz\":316000,\"snr\":6,\"active\":false,\"ident\":\"\"},"
            "{\"id\":5,\"hz\":423500,\"snr\":19,\"active\":true,\"ident\":\"LA\",\"copy\":\"LA LA\"}],"
            "\"log\":[{\"ident\":\"BIA\",\"hz\":391500,\"lat\":50.78,\"lon\":-1.84},{\"ident\":\"EAS\",\"hz\":391500,\"lat\":50.95,\"lon\":-1.36}]}";
    } else if (x.canned == "ism") {
        std::snprintf(b, sizeof b, "{\"type\":\"ism\",\"band\":\"%s\",\"bandName\":\"%s\",\"devices\":[", x.ismAsk >= 0 && x.ismAsk < (int)x.ismBands.size() ? x.ismBands[(size_t)x.ismAsk].first.c_str() : "868",
                      x.ismAsk >= 0 && x.ismAsk < (int)x.ismBands.size() ? x.ismBands[(size_t)x.ismAsk].second.c_str() : "868 MHz (EU)");
        j = b;
        j += "{\"key\":\"t1\",\"kind\":\"sensor\",\"make\":\"Oregon\",\"model\":\"THGR810\",\"line\":\"Temperature 11.4 °C · humidity 78 %\",\"freq\":433920000,\"snr\":18,\"age\":12},"
             "{\"key\":\"m1\",\"kind\":\"meshtastic\",\"name\":\"Bob's base\",\"short\":\"BOB\",\"line\":\"Meshtastic · ROUTER · 3 hops\",\"lat\":52.30,\"lon\":-0.80,\"freq\":869525000,\"snr\":7,\"age\":40},"
             "{\"key\":\"m2\",\"kind\":\"meshcore\",\"name\":\"NN-Repeater\",\"short\":\"NNR\",\"line\":\"MeshCore repeater\",\"lat\":52.21,\"lon\":-0.95,\"freq\":869618000,\"snr\":3,\"age\":5},"
             "{\"key\":\"w1\",\"kind\":\"meter\",\"make\":\"Kamstrup\",\"model\":\"Multical 21\",\"line\":\"Water 123.456 m³\",\"freq\":868950000,\"snr\":11,\"age\":2}]}";
        if (x.ismBands.empty()) { std::lock_guard<std::mutex> lk(x.capMtx); x.ismBands = {{"433", "433 MHz"}, {"868", "868 MHz (EU)"}, {"915", "915 MHz (US)"}}; }
    } else if (x.canned == "aprs") {
        if (k % 3 == 0) mapDecFeed(a, "aprs_list", Json::parse("{\"stations\":[{\"key\":\"M0ABC-9\",\"from\":\"M0ABC-9\",\"kind\":\"position\",\"symName\":\"Car\",\"line\":\"car, 48 km/h · via MB7UH\",\"lat\":52.25,\"lon\":-0.88,\"speed\":48,\"course\":120,\"seen\":3},"
                                                            "{\"key\":\"MB7UH\",\"from\":\"MB7UH\",\"kind\":\"position\",\"symName\":\"Digipeater\",\"line\":\"digipeater\",\"lat\":52.33,\"lon\":-0.71,\"seen\":40},"
                                                            "{\"key\":\"G4XYZ-13\",\"from\":\"G4XYZ-13\",\"kind\":\"weather\",\"symName\":\"Weather station\",\"line\":\"9.8 °C · 1012 hPa\",\"lat\":52.16,\"lon\":-1.02,\"seen\":120}]}"));
        return;
    } else if (x.canned == "dsc") {
        if (k == 1) mapDecFeed(a, "dsc_call", Json::parse("{\"id\":1,\"rf\":2187500,\"level\":3,\"line\":\"DISTRESS · MMSI 235012345 · fire, explosion · 50°12'N 001°18'W\",\"lat\":50.2,\"lon\":-1.3}"));
        if (k == 2) mapDecFeed(a, "dsc_call", Json::parse("{\"id\":2,\"rf\":2187500,\"level\":1,\"line\":\"SAFETY · Solent Coastguard → all ships\",\"lat\":50.8,\"lon\":-1.1}"));
        return;
    }
    MapMode* m = mapMode(x.canned);
    if (!m) return;
    m->canned = Json::parse(j);
    if (k == 0) { std::lock_guard<std::mutex> lk(x.capMtx); m->cap = 1; }
    (void)s;
}

}  // namespace hh
