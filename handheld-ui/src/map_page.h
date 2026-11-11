// map_page.h — THE MAP SHEET (App::S_MAP): one full-screen page over everything (the receiver, or "Please connect an
// SDR" — the map needs no radio), drawn natively from the map files on the card. No webview, no tile server, no
// internet: there is none on the handheld.
//
// Pages: SET LOCATION FROM MAP (R36S HARDWARE → LOCATION — web/client/src/handheldMap.ts openMapPick), and the map
// decoders (map_modes.h registers ADS-B, AIS, NDB, ISM, and the APRS / DSC maps of the decoder box). A page gives the
// sheet its list rows, its markers and its card; this file draws them, moves the map and routes the pad.
// ★ The pad on the map: map_view.h's header (mapControl — one table).
// ★ Include AFTER dec_box.h and dab_box.h (a whole-radio mode leaves DAB and stops a decoder, as the web does).
#pragma once
#include "aprs_marks.h"
#include "hw_state.h"
#include "map_view.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hh {

/** A thing on the map (an aircraft, a ship, a beacon, a station, a pin). glyph: see mapGlyph. angle: degrees, NaN = none. */
struct MapMarker { std::string key; double lat = 0, lon = 0; int glyph = 0; float angle = NAN; Rgba col{1, 1, 1, 1}; std::string label;
                   std::string sym;   // MG_APRS: the station's APRS symbol, table + code ("/>" a car)
};
enum { MG_DOT, MG_PLANE, MG_SHIP, MG_MOORED, MG_ATON, MG_NDB, MG_PIN, MG_STATION, MG_NODE, MG_APRS };
/** One row of the page's list. */
struct MapListRow { std::string key, title, right, sub; Rgba col = th::amber; bool pos = false; double lat = 0, lon = 0; };

struct MapPageDef {
    std::string id, title, profile = "directory", exitLabel = "CLOSE";
    /** The page's own data credit under the map (ADS-B: the aircraft database's ODC-By notice). */
    std::string credit;
    int startZ = 7, maxZ = 12;
    std::vector<double> ringsNm;                       // range rings round the receiver
    std::string empty = "nothing yet";
    /** ★★ THE LIST FIRST (Stuart, 2026-11-07: "ISM the map is an optional view for the Meshcore repeaters so ISM needs to
     *  be the list view primarily"): the page opens as its list across the whole screen, no map drawn behind it; MAP
     *  brings the map (and the list beside it), LIST goes back. */
    bool listFirst = false;
    /** Fill the list, the markers and the header's status line (once a frame — keep it bounded). */
    std::function<void(App&, const RadioState&, std::vector<MapListRow>&, std::vector<MapMarker>&, std::string&)> build;
    /** The selected row's card: a few lines. */
    std::function<std::vector<std::string>(App&, const RadioState&, const std::string&)> card;
    /** BACK / EXIT: leave the page (a whole-radio mode leaves the mode). */
    std::function<void(App&, const RadioState&)> exit;
    /** Extra header keys (label, press). */
    std::function<void(App&, const RadioState&, std::vector<std::pair<std::string, std::function<void()>>>&)> keys;
};
inline std::map<std::string, MapPageDef>& mapPages() { static std::map<std::string, MapPageDef> m; return m; }
inline void mapRegisterPage(MapPageDef d) { mapPages()[d.id] = std::move(d); }

struct MapUi {
    std::string page;                 // "" = no map sheet
    MapView view;
    bool listOpen = true;
    bool listFull = false;            // ★ a listFirst page showing only its list (no map drawn — mapShowsMap)
    std::string sel;                  // the selected row / marker
    std::string dismissed;            // ★ the row whose card BACK closed — the D-pad resting on it does not reopen it
    Scroller listScroll;
    /* ★ A POP-UP OVER THE PAGE (the ADS-B gain list): while one is up it alone takes the pad — the page below draws on a
     *  layer that never takes the highlight — and BACK closes it. It returns false to close itself. */
    std::function<bool(App&, Ctx&, const RadioState&)> pop;
    Scroller popScroll;
    bool popRefocus = false;          // ★ the frame after a pop-up closes: the highlight back on GAIN (not EXIT beside it)
    uint64_t frame = 0;
    bool started = false;
    double perfAt = 0;
    long perfFrames = 0; double perfDraw = 0;
    // the receiver's location (GET /handheld/location → hwShared)
    double meAskedAt = -1e9;
    bool centredOnce = false;
    std::string note;                 // a message over the map (loading / no data)
    std::vector<MapMarker> markers;   // this frame's (the pointer's hit test)
    std::vector<MapListRow> rows;
    double closedAt = 0;              // when the last page closed (the buffers are let go 90 s later)
};
inline MapUi& mapUi() { static MapUi u; return u; }

/** The receiver's position, if set (R36S HARDWARE → LOCATION). */
inline bool mapMe(double& lat, double& lon) {
    HwShared& h = hwShared();
    std::lock_guard<std::mutex> lk(h.m);
    if (!h.locKnown || !h.locSet) return false;
    /* ★★ A LOCATOR IS A LOCATION (Stuart, 2026-11-08: set IO92nh, and no rings). The engine stores what was typed —
     *  a locator alone, lat / lon empty — so the map read "no location". A locator is placed at its square's centre
     *  (IO92nh: about 2.5 × 4.6 km — well inside the smallest ring). */
    if (h.lat.empty() || h.lon.empty()) return !h.locator.empty() && mapGridCentre(h.locator, lat, lon);
    lat = std::atof(h.lat.c_str()); lon = std::atof(h.lon.c_str());
    return std::isfinite(lat) && std::isfinite(lon) && !(lat == 0 && lon == 0);
}
inline void mapAskLocation(int port) {
    MapUi& u = mapUi();
    if (nowSec() - u.meAskedAt < 20) return;
    u.meAskedAt = nowSec();
    hwAsync([port] { const HttpResult l = httpRequest(port, "GET", "/handheld/location", "", 4); if (l.status == 200) hwParseLocation(Json::parse(l.body)); });
}
inline bool mapTop(const App& a) { return !a.sheets_.empty() && a.sheets_.back() == App::S_MAP; }
inline bool mapCoversScreen(const App& a) { for (auto s : a.sheets_) if (s == App::S_MAP) return true; return false; }
/** The map itself is being drawn (not a listFirst page's list) — what the performance boost is for (app_impl.h). */
inline bool mapShowsMap(const App& a) { return mapCoversScreen(a) && !mapUi().listFull; }

/** Start the engine's half (the worker, the GL buffer) the first time a map opens. UI thread. */
inline void mapEnsureStarted(App& a) {
    MapUi& u = mapUi();
    if (u.started) return;
    u.started = true;
    mapStore().start(a.eng_->port());
    mapFonts().load(fontPath("AtkinsonHyperlegible-Regular.ttf"), fontPath("AtkinsonHyperlegible-Bold.ttf"));
    mapRender().init(a.W, a.H, 128);
}
inline void mapEnsureGpu(App& a) { if (!mapRender().ready()) mapRender().init(a.W, a.H, 128); }

/** Open a page (centred on the receiver if it has a location, else where `lat`, `lon` say). */
inline void mapOpenPage(App& a, const std::string& id, double lat = NAN, double lon = NAN, int z = -1) {
    auto it = mapPages().find(id);
    if (it == mapPages().end()) return;
    mapEnsureStarted(a);
    mapEnsureGpu(a);
    MapUi& u = mapUi();
    const bool same = u.page == id;
    u.page = id;
    u.view.profile = it->second.profile; u.view.maxZ = it->second.maxZ; u.view.dirty = true;
    if (!same) {
        double mlat, mlon;
        const bool me = mapMe(mlat, mlon);
        if (std::isfinite(lat)) u.view.centreOn(lat, lon, z > 0 ? z : it->second.startZ);
        else if (me) u.view.centreOn(mlat, mlon, z > 0 ? z : it->second.startZ);
        else u.view.centreOn(30, 0, 2);
        u.sel.clear(); u.listScroll = {}; u.centredOnce = me || std::isfinite(lat);
        u.listFull = it->second.listFirst;
        a.nav_.forget("map");
    }
    mapAskLocation(a.eng_->port());
    u.view.ptrAt = nowSec(); u.view.dpadAt = 0;   // ★ the bottom face starts as "select under the pointer", never a key
    u.closedAt = 0;
    if (!mapCoversScreen(a)) a.open(App::S_MAP);
}
/** Close the sheet (the page's own exit decides what else happens). */
inline void mapClosePage(App& a) {
    MapUi& u = mapUi();
    u.page.clear(); u.sel.clear(); u.pop = nullptr; u.closedAt = nowSec();
    /* ★★★ CLOSED = FLUSHED (Stuart, 2026-11-08: "When exiting a map we need to flush it completely" — reopening within
     *  the old 90 s grace drew the tiles all wrong and ran slowly). Every file off the GPU, the loader's queue emptied,
     *  the off-screen buffer released: the next map starts from nothing, as the first one did. Costs a fresh read of
     *  the visible files from the card on each open. */
    if (u.started) {
        mapStore().flush();
        mapRender().release();
        u.view.dirty = true;
        std::fprintf(stderr, "vibe-hhui: map closed — flushed (files, queue, buffer)\n");
    }
    for (size_t i = 0; i < a.sheets_.size(); i++) if (a.sheets_[i] == App::S_MAP) { a.nav_.forget("map"); a.sheets_.erase(a.sheets_.begin() + (long)i); break; }
}

// ── drawing helpers ──────────────────────────────────────────────────────────────────────────────────────────────────

inline Rgba mapHsl(float h, float s, float l, float a = 1) {
    const float c = (1 - std::fabs(2 * l - 1)) * s, hp = std::fmod(h, 360.f) / 60.f, x = c * (1 - std::fabs(std::fmod(hp, 2.f) - 1));
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
    const float m = l - c / 2;
    return {r + m, g + m, b + m, a};
}
/** A polygon (points relative to its centre, nose up), rotated by `deg`, as a fan of triangles. */
inline void mapShape(Ctx& c, float x, float y, const float* pts, int n, float deg, float scale, Rgba col) {
    const float r = deg * (float)M_PI / 180.f, cs = std::cos(r), sn = std::sin(r);
    auto P = [&](int i, float& ox, float& oy) { const float px = pts[2 * i] * scale, py = pts[2 * i + 1] * scale; ox = x + px * cs - py * sn; oy = y + px * sn + py * cs; };
    float x0, y0; P(0, x0, y0);
    for (int i = 1; i + 1 < n; i++) { float x1, y1, x2, y2; P(i, x1, y1); P(i + 1, x2, y2); c.g.triangle(x0, y0, x1, y1, x2, y2, col); }
}
/** ★★ APRS ICONS (Stuart, 2026-11-11: "APRS on OWRX … uses little icons for certain things like a little red car or a
 *  house etc, can we do something similar please"). The APP'S OWN, as the web client draws them — src/services/tracks.ts
 *  aprsIcon(): a disc in the station's colour (aprsColour, below), its picture dark on it (aprs_marks.h, generated from
 *  tracks.ts APRS_GLYPH by test/gen_aprs_marks.mjs), the symbol code itself when there is no picture, a dashed ring for
 *  the alternate table, an overlay character bottom right. ×1.2 for the panel (26 px). */
inline Rgba mapAprsColour(const std::string& kind, const std::string& sym, double speed) {   // tracks.ts aprsColour, a copy
    const char code = sym.size() > 1 ? sym[1] : '/';
    if (kind == "weather" || code == '_') return rgb(0x39d0d8);
    if (kind == "object" || kind == "item") return rgb(0xffad42);
    if (code == '#' || code == '&' || code == 'r') return rgb(0xc58cff);
    if (std::isfinite(speed) && speed >= 2) return rgb(0x4fa3ff);
    return rgb(0x59c26b);
}
inline void mapAprsIcon(Ctx& c, const MapMarker& m, float x, float y) {
    using namespace aprsmark;
    const float k = kSize / 22.f;
    const Rgba ink = rgb(0x0b1014);
    const char table = m.sym.size() > 0 ? m.sym[0] : '/', code = m.sym.size() > 1 ? m.sym[1] : '/';
    c.g.circle(x, y, 10 * k + 0.7f * k, 0, ink);           // the disc's outline (stroke 1.4, half outside)
    c.g.circle(x, y, 10 * k - 0.7f * k, 0, m.col);
    if (table == '\\') {                                    // the alternate table: the ring dashed (3 / 1.6)
        const int n = 14;
        for (int i = 0; i < n; i++) if (i % 2) {
            const float a0 = i * 6.2831853f / n, a1 = (i + 1) * 6.2831853f / n;
            c.g.line(x + std::cos(a0) * 10 * k, y + std::sin(a0) * 10 * k, x + std::cos(a1) * 10 * k, y + std::sin(a1) * 10 * k, 1.6f * k, m.col);
        }
    }
    const int gi = glyphFor(code);
    if (gi >= 0) {
        static GLuint tex[kCount] = {};
        if (!tex[gi]) {
            std::vector<uint8_t> px((size_t)kSize * kSize * 4);
            for (int i = 0; i < kSize * kSize; i++) { px[(size_t)i * 4] = px[(size_t)i * 4 + 1] = px[(size_t)i * 4 + 2] = 255; px[(size_t)i * 4 + 3] = kMask[gi][i]; }
            glGenTextures(1, &tex[gi]);
            glBindTexture(GL_TEXTURE_2D, tex[gi]);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        c.g.image(tex[gi], std::round(x - kSize / 2.f), std::round(y - kSize / 2.f), (float)kSize, (float)kSize, ink);
    } else if (code > ' ' && code < 0x7f) {
        const std::string t(1, code);
        c.tc.draw(c.g, c.f.small, t, x - c.tc.width(c.f.small, t) / 2.f, y - 8, ink);
    }
    const bool overlay = table != '/' && table != '\\' && ((table >= '0' && table <= '9') || (table >= 'A' && table <= 'Z'));
    if (overlay) {
        const std::string t(1, table);
        const float ox = x + 6.5f * k, oy = y + 4 * k;
        c.g.circle(ox, oy, 5 * k, 0, ink);
        c.tc.draw(c.g, c.f.tiny, t, ox - c.tc.width(c.f.tiny, t) / 2.f, oy - 6, rgb(0xffffff));
    }
}
inline void mapGlyph(Ctx& c, const MapMarker& m, float x, float y, bool sel) {
    const Rgba k = rgb(0, 0.8f);
    const float ang = std::isfinite(m.angle) ? m.angle : 0;
    if (sel) c.g.circle(x, y, 15, 2.5f, th::focus);
    switch (m.glyph) {
        case MG_PLANE: {
            // fuselage + wings + tail (convex pieces, so the fan is exact)
            static const float body[] = {0, -1, 0.16f, -0.6f, 0.16f, 0.75f, -0.16f, 0.75f, -0.16f, -0.6f};
            static const float wing[] = {0, -0.3f, 1, 0.25f, 1, 0.42f, 0, 0.2f, -1, 0.42f, -1, 0.25f};
            static const float tail[] = {0, 0.55f, 0.45f, 0.95f, -0.45f, 0.95f};
            for (int pass = 0; pass < 2; pass++) {
                const float s = pass ? 9.5f : 11.5f; const Rgba q = pass ? m.col : k;
                mapShape(c, x, y, body, 5, ang, s, q); mapShape(c, x, y, wing, 6, ang, s, q); mapShape(c, x, y, tail, 3, ang, s, q);
            }
            break;
        }
        case MG_SHIP: {
            static const float hull[] = {0, -1, 0.5f, -0.2f, 0.5f, 0.9f, -0.5f, 0.9f, -0.5f, -0.2f};
            mapShape(c, x, y, hull, 5, ang, 10.5f, k); mapShape(c, x, y, hull, 5, ang, 8.5f, m.col);
            break;
        }
        case MG_MOORED: c.g.circle(x, y, 6, 0, k); c.g.circle(x, y, 4.5f, 0, m.col); break;
        case MG_ATON: {
            static const float d[] = {0, -1, 1, 0, 0, 1, -1, 0};
            mapShape(c, x, y, d, 4, 0, 8, k); mapShape(c, x, y, d, 4, 0, 6, m.col);
            break;
        }
        case MG_NDB: c.g.circle(x, y, 8, 0, k); c.g.circle(x, y, 7, 1.8f, m.col); c.g.circle(x, y, 2.5f, 0, m.col); break;
        case MG_PIN: c.g.circle(x, y, 9, 0, k); c.g.circle(x, y, 7, 0, m.col); c.g.circle(x, y, 2.5f, 0, rgb(0)); break;
        case MG_STATION: c.g.rect(x - 6, y - 6, 12, 12, k); c.g.rect(x - 4.5f, y - 4.5f, 9, 9, m.col); break;
        case MG_APRS: mapAprsIcon(c, m, x, y); break;
        case MG_NODE: { static const float t[] = {0, -1, 0.9f, 0.7f, -0.9f, 0.7f}; mapShape(c, x, y, t, 3, 0, 9, k); mapShape(c, x, y, t, 3, 0, 7, m.col); break; }
        default: c.g.circle(x, y, 6, 0, k); c.g.circle(x, y, 4.5f, 0, m.col); break;
    }
}
/** The receiver: THE DIRECTORY'S RADIO (Stuart, 2026-11-11: "the Radio Icon on the map is incorrect it needs to be the
 *  same one we use in the directory map for the servers") — directory/public/index.html radioGlyph(), stroke for stroke
 *  on its 24-unit box: the cabinet, the dial with its dot, three grille bars, the aerial off the top right; in the
 *  directory's "free" green, with a dark pass under it for the page's drop shadow, so it reads on land and sea. The
 *  glyph's centre (12, 12) is the spot. ~29 px (the directory's 22 px marker, scaled for the 640×480 panel). */
inline void mapHouse(Ctx& c, float x, float y) {
    const float k = 1.2f;
    const Rgba col = rgb(0x28A745), dark = rgb(0, 0.85f);
    auto P = [&](float gx, float gy, float& ox, float& oy) { ox = x + (gx - 12) * k; oy = y + (gy - 12) * k; };
    for (int pass = 0; pass < 2; pass++) {
        const Rgba cc = pass ? col : dark;
        const float t = (pass ? 1.8f : 3.8f) * k;
        auto L = [&](float ax, float ay, float bx, float by) { float x0, y0, x1, y1; P(ax, ay, x0, y0); P(bx, by, x1, y1); c.g.line(x0, y0, x1, y1, t, cc); };
        // the cabinet <rect x=3 y=8.5 w=18 h=12.5>: four sides, the corners squared off by half a stroke
        L(3 - 0.9f, 8.5f, 21 + 0.9f, 8.5f); L(3 - 0.9f, 21, 21 + 0.9f, 21); L(3, 8.5f, 3, 21); L(21, 8.5f, 21, 21);
        // the dial <circle cx=8.6 cy=14.8 r=2.9> and its dot (r 1)
        float dx, dy; P(8.6f, 14.8f, dx, dy);
        c.g.circle(dx, dy, 2.9f * k + t * 0.5f, t, cc);
        c.g.circle(dx, dy, pass ? 1.0f * k : 1.0f * k + 1.0f, 0, cc);
        // the grille: three bars, y 11.6 → 18
        L(14.2f, 11.6f, 14.2f, 18); L(16.8f, 11.6f, 16.8f, 18); L(19.2f, 11.6f, 19.2f, 18);
        // the aerial
        L(14.6f, 8.5f, 19.3f, 3.2f);
    }
}
/** A range ring (km) round a point — 72 chords, so a large ring is a circle, not a polygon. */
inline void mapRing(Ctx& c, const MapView& v, double lat, double lon, double km, Rgba col, const std::string& label, const Font& f) {
    const double dLat = km / 111.32, dLon = km / (111.32 * std::max(0.05, std::cos(lat * M_PI / 180)));
    float px0 = 0, py0 = 0;
    for (int i = 0; i <= 72; i++) {
        const double t = i * 2 * M_PI / 72;
        float x, y;
        v.toScreen(lat + dLat * std::cos(t), lon + dLon * std::sin(t), x, y, 4000);
        if (i) c.g.line(px0, py0, x, y, 1.2f, col);
        px0 = x; py0 = y;
    }
    float lx, ly;
    if (v.toScreen(lat + dLat, lon, lx, ly, 0)) c.tc.draw(c.g, f, label, lx + 3, ly + 1, col);
}

// ── the sheet ────────────────────────────────────────────────────────────────────────────────────────────────────────

namespace mapl {
constexpr float headH = 36, footH = 28, listX = 404, listW = 236;
}

inline void mapSelect(App& a, const std::string& key, bool centre) {
    MapUi& u = mapUi();
    u.sel = key;
    a.nav_.setFocus("map", "map.r." + key);
    if (!centre) return;
    for (auto& r : u.rows) if (r.key == key && r.pos) {
        float x, y;
        if (!u.view.toScreen(r.lat, r.lon, x, y, -40)) { u.view.cx = mercX(r.lon); u.view.cy = mercY(r.lat); u.view.pan(0, 0); }
    }
}

/** The pointer pressed: the marker under it (within 18 px), else the basemap POI there (a toast). */
inline void mapClick(App& a) {
    MapUi& u = mapUi();
    if (u.page == "pick") {
        const double lat = u.view.pointerLat(), lon = u.view.pointerLon();
        char b[96]; std::snprintf(b, sizeof b, "{\"lat\":%.5f,\"lon\":%.5f}", lat, lon);
        hwPostLocation(a.eng_->port(), b);
        mapClosePage(a);
        return;
    }
    const MapMarker* best = nullptr; float bd = 18 * 18;
    for (auto& m : u.markers) {
        float x, y;
        if (!u.view.toScreen(m.lat, m.lon, x, y, 0)) continue;
        const float d = (x - u.view.px) * (x - u.view.px) + (y - u.view.py) * (y - u.view.py);
        if (d < bd) { bd = d; best = &m; }
    }
    if (best) { mapSelect(a, best->key, false); return; }
    for (auto& p : mapRender().poi) {
        const float x = p.x - u.view.blitX, y = p.y - u.view.blitY;
        if (u.view.px >= x && u.view.px <= x + p.w && u.view.py >= y && u.view.py <= y + p.h) { a.toast(p.text); return; }
    }
    u.sel.clear();
}

inline void mapDrawSheet(App& a, Ctx& c, const RadioState& s) {
    MapUi& u = mapUi();
    auto it = mapPages().find(u.page);
    if (it == mapPages().end()) { mapClosePage(a); return; }
    const MapPageDef& P = it->second;
    const double now = c.t;
    const double t0 = App::threadCpu();
    u.frame++;
    if (u.pop) c.nav.layer("map.under");   // ★ the pop-up has the pad (drawn last, on "map")
    if (u.popRefocus) { u.popRefocus = false; a.nav_.setFocus("map", "map.k.gain"); }
    // ★ a page that opened before the receiver's location arrived goes there once it does
    double mlat = 0, mlon = 0;
    const bool me = mapMe(mlat, mlon);
    if (me && !u.centredOnce) { u.centredOnce = true; u.view.centreOn(mlat, mlon, P.startZ); }
    const bool pick = u.page == "pick";
    const bool full = !pick && P.listFirst && u.listFull;   // ★ the list alone, across the screen
    const bool list = !pick && (u.listOpen || full);
    u.view.area = {0, mapl::headH, list ? mapl::listX : 640.f, 480 - mapl::headH - mapl::footH};
    u.view.showPointer = !full;
    if (full) c.g.rect(0, mapl::headH, 640, 480 - mapl::headH - mapl::footH, rgb(0x0c0802));
    else u.view.draw(c, now, u.frame);

    // the page's content
    std::string status;
    u.rows.clear(); u.markers.clear();
    if (P.build) P.build(a, s, u.rows, u.markers, status);
    // ★ the D-pad's first stop is the list, not EXIT
    if (!u.rows.empty() && u.view.dpadAt == 0) {
        const std::string f = a.nav_.focus("map");
        if (f.empty() || (f.compare(0, 6, "map.k.") == 0 && f != "map.k.gain")) a.nav_.setFocus("map", "map.r." + u.rows[0].key);
    }
    if (!full) {
    c.g.clip(u.view.area.x, u.view.area.y, u.view.area.w, u.view.area.h);
    /* ★ NO LOCATION, NO RINGS (Stuart, 2026-11-08: "and the distance rings") — the rings and every distance are measured
     *  from the receiver, so without a location there is nothing to draw them round: say where to set it. */
    if (!me && !P.ringsNm.empty() && !pick) {
        const std::string t = "No location set — range rings and distances need one: MENU › HARDWARE › LOCATION";
        const float x = u.view.area.x + 8, y = u.view.area.y + (status.empty() ? 8.f : 36.f), w = std::min(420.f, u.view.area.w - 16);
        const auto ls = c.tc.wrap(c.f.small, t, (int)w - 20);
        const float lh = (float)c.f.small.lineHeight();
        c.g.rrect(x, y, w, lh * ls.size() + 10, 6, rgb(0x080601, 0.88f));
        for (size_t i = 0; i < ls.size(); i++) c.tc.draw(c.g, c.f.small, ls[i], x + 10, y + 5 + lh * i, th::amber);
    }
    if (me) {
        for (double nm : P.ringsNm) { char b[24]; std::snprintf(b, sizeof b, "%.0f NM", nm); mapRing(c, u.view, mlat, mlon, nm * 1.852, rgb(0x46e0ff, 0.45f), b, mapFonts().reg11); }
    }
    // markers: labels where there is room (a simple placer — the selected one always)
    {
        std::vector<Rect> taken;
        const Font& f = mapFonts().bold13;
        int labels = 0;
        const MapMarker* selM = nullptr;
        for (auto& m : u.markers) {
            float x, y;
            if (!u.view.toScreen(m.lat, m.lon, x, y)) continue;
            const bool sel = m.key == u.sel;
            if (sel) { selM = &m; continue; }
            mapGlyph(c, m, x, y, false);
            if (m.label.empty() || labels > 60 || u.view.z < 5) continue;
            const Rect r{x + 11, y - 9, (float)c.tc.width(f, m.label) + 4, 18};
            bool free = true;
            for (auto& t : taken) if (r.x < t.r() && r.r() > t.x && r.y < t.b() && r.b() > t.y) { free = false; break; }
            if (!free) continue;
            taken.push_back(r); labels++;
            c.tc.draw(c.g, f, m.label, r.x + 1, r.y + 1, rgb(0, 0.8f));
            c.tc.draw(c.g, f, m.label, r.x, r.y, m.col);
        }
        if (me) { float x, y; if (u.view.toScreen(mlat, mlon, x, y)) mapHouse(c, x, y); }
        if (selM) {
            float x, y;
            if (u.view.toScreen(selM->lat, selM->lon, x, y)) {
                mapGlyph(c, *selM, x, y, true);
                if (!selM->label.empty()) { c.tc.draw(c.g, f, selM->label, x + 15, y - 8, rgb(0, 0.85f)); c.tc.draw(c.g, f, selM->label, x + 14, y - 9, th::white); }
            }
        }
    }
    /* ★★ THE COUNTS SIT ON THE MAP, NOT IN THE HEADER (Stuart, 2026-11-08: "the buttons in the header cause them to be
     *  clipped off screen" — GAIN 40.2 AUTO · HIDE LIST · HOME · EXIT ADS-B left "11 aircra"). A chip at the map's
     *  top-left, over the markers; the header keeps the title and the keys. (The list alone, with no map, keeps it in
     *  the header — it has the room there.) */
    if (!status.empty()) {
        const float x = u.view.area.x + 8, y = u.view.area.y + 6;
        std::string st = status;
        const int room = (int)u.view.area.w - 36;
        while (!st.empty() && c.tc.width(c.f.small, st) > room) st.pop_back();
        const float w = (float)c.tc.width(c.f.small, st) + 20;
        c.g.rrect(x, y, w, (float)c.f.small.lineHeight() + 8, 6, rgb(0x080601, 0.85f));
        c.tc.draw(c.g, c.f.small, st, x + 10, y + 4, th::amber);
    }
    u.view.drawPointer(c, now);
    c.g.unclip();
    }

    // ── the header: title · status · keys ──
    c.g.rect(0, 0, 640, mapl::headH, rgb(0x080601, 0.92f));
    c.g.rect(0, mapl::headH - 1, 640, 1, th::barBorder);
    float hx = 10;
    hx += c.tc.draw(c.g, c.f.bodyB, P.title, hx, 7, th::btnText) + 12;
    std::vector<std::pair<std::string, std::function<void()>>> keys;
    if (P.keys) P.keys(a, s, keys);
    if (full) keys.push_back({"MAP", [&u] { u.listFull = false; u.listOpen = true; u.view.dirty = true; }});
    else if (!pick && P.listFirst) keys.push_back({"LIST", [&u] { u.listFull = true; }});
    else if (!pick) keys.push_back({list ? "HIDE LIST" : "LIST", [&u] { u.listOpen = !u.listOpen; }});
    if (me && !pick && !full) keys.push_back({"HOME", [&u, mlat, mlon] { u.view.cx = mercX(mlon); u.view.cy = mercY(mlat); u.view.pan(0, 0); }});
    // ★ the picker has no keys to highlight: its D-pad pans, the bottom face sets, the right face cancels (the hint line)
    if (!pick) keys.push_back({P.exitLabel, [&a, &s, &P] { if (P.exit) P.exit(a, s); else mapClosePage(a); }});
    float kx = 636;
    std::function<void()> pressed;
    for (size_t i = keys.size(); i-- > 0;) {
        const float w = (float)c.tc.width(c.f.tiny, keys[i].first) + 20;
        kx -= w;
        BtnStyle st; st.font = &c.f.tiny; st.radius = 6;
        // ★ a key whose label carries a reading (GAIN 37.2 AUTO) keeps one id, so the highlight stays on it as it changes
        const std::string kid = keys[i].first.rfind("GAIN ", 0) == 0 ? std::string("gain") : keys[i].first;
        if (button(c, "map.k." + kid, {kx, 5, w, 26}, keys[i].first, st)) pressed = keys[i].second;
        kx -= 6;
    }
    if (!status.empty() && full) {   // (on the map otherwise — the chip above)
        const int room = (int)(kx - hx - 8);
        std::string st = status;
        while (!st.empty() && c.tc.width(c.f.small, st) > room) st.pop_back();
        c.tc.draw(c.g, c.f.small, st, hx, 9, th::dim);
    }

    // ── the list ──
    if (list) {
        const Rect lr = full ? Rect{0, mapl::headH, 640, 480 - mapl::headH - mapl::footH}
                             : Rect{mapl::listX, mapl::headH, mapl::listW, 480 - mapl::headH - mapl::footH};
        c.g.rect(lr.x, lr.y, lr.w, lr.h, rgb(0x0c0802, 0.94f));
        c.g.rect(lr.x, lr.y, 1, lr.h, th::barBorder);
        if (u.rows.empty()) {
            float y = lr.y + 14;
            for (auto& l : c.tc.wrap(c.f.small, P.empty, (int)lr.w - 24)) { c.tc.draw(c.g, c.f.small, l, lr.x + 12, y, th::dim); y += c.f.small.lineHeight(); }
        } else {
            const float rh = 46;
            u.listScroll.contentH = rh * (float)std::min<size_t>(u.rows.size(), 150);
            c.g.clip(lr.x, lr.y, lr.w, lr.h);
            for (size_t i = 0; i < u.rows.size() && i < 150; i++) {
                const MapListRow& r = u.rows[i];
                const Rect rr{lr.x + 6, lr.y + 4 + i * rh - u.listScroll.y, lr.w - 12, rh - 4};
                const Nav::Hit h = c.nav.item("map.r." + r.key, rr);
                if (h.focused && u.view.dpadAt > u.view.ptrAt && r.key != u.dismissed) { u.sel = r.key; u.dismissed.clear(); }   // ★ the D-pad's row is the selection (its marker lit, its card up)
                if (rr.b() < lr.y - rh || rr.y > lr.b() + rh) { if (h.pressed) mapSelect(a, r.key, true); continue; }
                const bool on = r.key == u.sel;
                c.g.rrect(rr.x, rr.y, rr.w, rr.h, 6, on ? th::activeBg : rgb(0x140a00, 0.6f));
                if (on) c.g.rframe(rr.x, rr.y, rr.w, rr.h, 6, 1, th::activeBdr);
                const int rw = c.tc.width(c.f.small, r.right);
                std::string t = r.title;
                while (!t.empty() && c.tc.width(c.f.bodyB, t) > rr.w - rw - 22) t.pop_back();
                c.tc.draw(c.g, c.f.bodyB, t, rr.x + 8, rr.y + 2, r.col);
                c.tc.draw(c.g, c.f.small, r.right, rr.r() - 8, rr.y + 4, th::dim, 2);
                std::string sub = r.sub;
                while (!sub.empty() && c.tc.width(c.f.small, sub) > rr.w - 16) sub.pop_back();
                c.tc.draw(c.g, c.f.small, sub, rr.x + 8, rr.y + 23, rgb(0xffffff, r.pos ? 0.7f : 0.45f));
                if (h.focused) { focusRing(c, rr, 6); }
                if (h.pressed) mapSelect(a, r.key, true);
            }
            c.g.unclip();
            const Rect* fr = c.nav.focusedRect();
            u.listScroll.follow(fr && fr->x >= lr.x ? fr : nullptr, lr, 8);
        }
    }

    const float attrTop = full ? 480 - mapl::footH : u.view.drawAttribution(c, u.view.area.b(), P.credit);
    // ── the selected thing's card ──
    if (!u.sel.empty() && P.card) {
        const auto lines = P.card(a, s, u.sel);
        if (!lines.empty()) {
            const float w = std::min(380.f, u.view.area.w - 20), lh = (float)c.f.small.lineHeight();
            const float h = 10 + c.f.bodyB.lineHeight() + lh * (lines.size() - 1);
            const float x = 10, y = attrTop - h - 6;
            c.g.rrect(x, y, w, h, 8, rgb(0x080601, 0.9f));
            c.g.rframe(x, y, w, h, 8, 1, th::btnBorder);
            for (size_t i = 0; i < lines.size(); i++) {
                std::string l = lines[i];
                const Font& f = i == 0 ? c.f.bodyB : c.f.small;
                while (!l.empty() && c.tc.width(f, l) > w - 16) l.pop_back();
                c.tc.draw(c.g, f, l, x + 8, y + 5 + (i == 0 ? 0 : c.f.bodyB.lineHeight() + lh * (i - 1)), i == 0 ? th::activeTx : rgb(0xffffff, 0.85f));
            }
        }
    }

    // ── the readout (pick) / the loading note ──
    if (pick) {
        const double lat = u.view.pointerLat(), lon = u.view.pointerLon();
        char ll[64]; std::snprintf(ll, sizeof ll, "%.5f, %.5f", lat, lon);
        const std::string loc = maidenhead(lat, lon);
        const float bx = 10, by = mapl::headH + 8;
        c.g.rrect(bx, by, 220, 58, 7, rgb(0x080601, 0.88f));
        c.g.rframe(bx, by, 220, 58, 7, 1, th::amber);
        float lx = bx + 10;
        for (char ch : upper(loc.substr(0, 4)) + loc.substr(4)) { const std::string one(1, ch); lx += c.tc.draw(c.g, c.f.title, one, lx, by + 4, th::amber) + 3; }
        c.tc.drawTab(c.g, c.f.small, ll, bx + 10, by + 34, th::amber);
    }
    if (!full) {   // ★ no map is drawn behind a listFirst page's list: no word about loading one
        MapStore& st = mapStore();
        std::string note;
        if (!st.indexReady()) note = st.lastError().empty() ? "Loading the map…" : "The map files did not load — " + st.lastError();
        if (!note.empty()) {
            const int w = c.tc.width(c.f.body, note) + 28;
            const float x = u.view.area.cx() - w / 2.f, y = u.view.area.y + 70;
            c.g.rrect(x, y, (float)w, 36, 8, rgb(0x080601, 0.9f));
            c.g.rframe(x, y, (float)w, 36, 8, 1, th::amber);
            c.tc.draw(c.g, c.f.body, note, x + 14, y + 7, th::amber);
        } else if (st.pending() > 0) c.tc.draw(c.g, mapFonts().reg11, "loading map data…", u.view.area.r() - 8, u.view.area.y + 34, rgb(0xffffff, 0.7f), 2);
    }

    // ── the hint line ──
    c.g.rect(0, 480 - mapl::footH, 640, mapl::footH, rgb(0x080601, 0.94f));
    c.g.rect(0, 480 - mapl::footH, 640, 1, th::barBorder);
    {
        const float cy = 480 - mapl::footH / 2.f;
        float x = 8;
        const bool cardUp = !pick && !u.sel.empty() && P.card;
        x = hints(c, x, cy, {{FACE_BOTTOM, pick ? "set here" : u.pop ? "set" : "select"}, {FACE_RIGHT, pick ? "cancel" : u.pop ? "close" : cardUp ? "close card" : "back"}}, th::dim);
        c.tc.draw(c.g, c.f.small, pick ? "L stick point · R stick move · L1/R1 zoom" : full ? "D-pad the list · MAP for the map · L2/R2 band"
                                  : u.page == "mode:adsb" ? "L stick point · R stick move · L1/R1 zoom" : "L stick point · R stick move · L1/R1 zoom · L2/R2 tune",
                  x, cy - c.f.small.lineHeight() / 2.f, th::dimmer);
    }
    if (u.pop) {
        c.nav.layer("map");
        const bool back = c.nav.backPressed();
        // ★ the header was not on the pad this frame, so the highlight goes back to GAIN on the next one
        if (back || !u.pop(a, c, s)) { u.pop = nullptr; u.popRefocus = true; }
        pressed = nullptr;
    }
    if (pressed) pressed();
    /* ★★ BACK CLOSES THE CARD FIRST (Stuart, 2026-11-08: pressed the right face to close an aircraft's card and it closed
     *  the whole map). With a card up BACK takes the card down; the next BACK leaves the page. */
    if (c.nav.backPressed()) {
        if (!pick && !u.sel.empty() && P.card) { u.dismissed = u.sel; u.sel.clear(); }
        else if (P.exit) P.exit(a, s); else mapClosePage(a);
    }

    // ★ the map's own cost, beside the UI's perf line
    u.perfFrames++; u.perfDraw += App::threadCpu() - t0;
    if (now - u.perfAt >= 30) {
        if (u.perfAt > 0) {
            MapRender& R = mapRender();
            std::fprintf(stderr, "vibe-hhui: map perf — %ld frames, map page %.2f ms a frame (incl. re-renders) · %ld re-renders, %.1f ms each (last %.1f ms, %ld indices in %ld draws) · "
                         "%ld labels · resident %.1f MB in %d files + buffer %.1f MB · z%d\n",
                         u.perfFrames, u.perfDraw * 1e3 / std::max(1L, u.perfFrames), R.renders, R.renders ? R.renderMs / R.renders : 0.0, R.lastMs,
                         R.vertsLast, R.drawsLast, R.labelsLast, mapStore().residentBytes() / 1048576.0, mapStore().residentFiles(), R.gpuBytes / 1048576.0, u.view.z);
            R.renders = 0; R.renderMs = 0;
        }
        u.perfAt = now; u.perfFrames = 0; u.perfDraw = 0;
    }
}

/** (The 90 s release is gone: a closed map is flushed at once — mapClosePage.) */
inline void mapIdle(App&) {}

// ── the pad (App::press / pollInput hooks) ───────────────────────────────────────────────────────────────────────────

/** Call at the top of App::press (after the keyboard): true = the map took the button. */
inline bool mapPress(App& a, Btn b) {
    if (!mapTop(a)) return false;
    MapUi& u = mapUi();
    const double now = nowSec();
    switch (mapControl(b)) {
        case MA_ZOOM_OUT: u.view.zoomBy(-1); return true;
        case MA_ZOOM_IN: u.view.zoomBy(1); return true;
        case MA_SELECT:
            if (u.listFull || u.pop) return false;                         // ★ a pop-up: ENTER is its highlighted row                                    // ★ the list alone: no pointer, the highlight is pressed
            if (u.view.ptrAt >= u.view.dpadAt || u.page == "pick") { mapClick(a); return true; }
            return false;                                                    // the highlighted key / row (Nav)
        case MA_DPAD:
            if (u.pop) return false;                                         // ★ the pop-up's rows (Nav)
            u.view.dpadAt = now;
            if (u.page == "pick") {                                          // no list: the D-pad pans (the web's 64 px)
                u.view.pan(b == B_LEFT ? -64 : b == B_RIGHT ? 64 : 0, b == B_UP ? -64 : b == B_DOWN ? 64 : 0);
                return true;
            }
            return false;
        default: return false;                                               // L2 / R2 tune (App), BACK (Nav)
    }
}
/** Call in App::pollInput before the jog wheels: true = the map took the sticks (the jogs rest). */
inline bool mapSticks(App& a, double lx, double ly, double rx, double ry) {
    if (!mapTop(a)) return false;
    mapUi().view.sticks(lx, ly, rx, ry, nowSec());
    return true;
}

// ── SET LOCATION FROM MAP ────────────────────────────────────────────────────────────────────────────────────────────

/** R36S HARDWARE → LOCATION → SET FROM MAP: the map at the receiver's location (else the world); the bottom face sets
 *  the point under the pointer — POST /handheld/location {lat, lon}, the same route as the typed locator. */
inline void mapOpenPicker(App& a) {
    if (!mapPages().count("pick")) {
        MapPageDef d;
        d.id = "pick"; d.title = "SET LOCATION"; d.profile = "directory"; d.startZ = 8; d.maxZ = 13; d.exitLabel = "CANCEL";
        mapRegisterPage(d);
    }
    mapOpenPage(a, "pick");
}

}  // namespace hh
