// map_view.h — one map on the screen: where it looks, the pad that moves it, the copy of the basemap buffer, the
// pointer, the attribution. The pages (map_page.h) put their own markers on top through toScreen().
//
// ★★★ THE CONTROLS (Stuart, 2026-11-07 — binding; ONE table, mapControl() below, so a swap is a one-line change):
//     LEFT STICK  a POINTER on the map — speed with deflection past a small dead zone, faster when held at the rim,
//                 clamped to the map; pushed against an edge it scrolls the map that way
//     BOTTOM FACE select what is under the pointer (an aircraft, a ship, a beacon, a station — or the location to set)
//     RIGHT STICK pan · L1 / R1 zoom out / in (the main screen's shoulders) · L2 / R2 tune down / up (tuneSteps, as on
//     the main screen — the radio plays on behind the map) · D-pad the page's own list / keys · RIGHT FACE back
// ★★ The pointer and the D-pad share the bottom face: it means "select under the pointer" when the pointer moved last,
//    "press the highlighted key" when the D-pad did — and the pointer dims while the D-pad has the say.
// ★ Zoom is whole steps (the web's zoomSnap 1) about the pointer, so what you point at stays under the pointer.
#pragma once
#include "map_geo.h"
#include "map_render.h"
#include "ui.h"

#include <cmath>
#include <string>

namespace hh {

/** What a pad input means on the map — the ONE place the scheme lives. */
enum MapAct { MA_NONE, MA_ZOOM_OUT, MA_ZOOM_IN, MA_TUNE_DOWN, MA_TUNE_UP, MA_SELECT, MA_BACK, MA_DPAD };
inline MapAct mapControl(Btn b) {
    switch (b) {
        case B_L1: return MA_ZOOM_OUT;  case B_R1: return MA_ZOOM_IN;     // ★ the main screen's zoom shoulders
        case B_L2: return MA_TUNE_DOWN; case B_R2: return MA_TUNE_UP;     // ★ …and its tune triggers
        case B_ENTER: return MA_SELECT; case B_BACK: return MA_BACK;
        case B_UP: case B_DOWN: case B_LEFT: case B_RIGHT: return MA_DPAD;
        default: return MA_NONE;
    }
}
enum { MAP_STICK_POINTER = 0, MAP_STICK_PAN = 1 };   // which stick does what: [left, right]
constexpr int kMapLeftStick = MAP_STICK_POINTER, kMapRightStick = MAP_STICK_PAN;

inline MapFonts& mapFonts() { static MapFonts f; return f; }
inline MapRender& mapRender() { static MapRender r; return r; }

class MapView {
public:
    double cx = 0.5, cy = 0.42;          // the centre, world units
    int z = 2;
    std::string profile = "directory";
    Rect area{0, 0, 640, 480};           // where the map is visible (a page's panel may cover the rest)
    float px = 320, py = 240;            // the pointer, screen pixels
    double ptrAt = 0, dpadAt = 0;        // which of the two moved last
    bool showPointer = true;
    int maxZ = 12;
    bool dirty = true;                   // force a re-render (a profile change)

    void centreOn(double lat, double lon, int zoom) {
        cx = mercX(wrapLon(lon)); cy = mercY(lat); z = std::max(kMapMinZ, std::min(maxZ, zoom));
        px = area.cx(); py = area.cy(); clampY(); dirty = true;
    }
    double latAt(float y) const { return mercLat(cy + (y - H / 2.0) / worldPx(z)); }
    double lonAt(float x) const { return wrapLon(mercLon(cx + (x - W / 2.0) / worldPx(z))); }
    double pointerLat() const { return latAt(py); }
    double pointerLon() const { return lonAt(px); }
    bool pointerLive(double now) const { return showPointer && ptrAt >= dpadAt && now - ptrAt < 600; }
    /** A point's screen position on the copy of the world nearest the centre. False when it is off the screen. */
    bool toScreen(double lat, double lon, float& x, float& y, float slack = 30) const {
        double d = mercX(lon) - cx; d -= std::round(d);
        x = (float)(W / 2.0 + d * worldPx(z)); y = (float)(H / 2.0 + (mercY(lat) - cy) * worldPx(z));
        return x > area.x - slack && x < area.r() + slack && y > area.y - slack && y < area.b() + slack;
    }
    void pan(double dx, double dy) {
        cx += dx / worldPx(z); cy += dy / worldPx(z);
        cx -= std::floor(cx);
        clampY();
    }
    /** A whole zoom step about the pointer (it stays over the same ground). */
    void zoomBy(int d) {
        const int nz = std::max(kMapMinZ, std::min(maxZ, z + d));
        if (nz == z) return;
        const double wx = cx + (px - W / 2.0) / worldPx(z), wy = cy + (py - H / 2.0) / worldPx(z);
        z = nz;
        cx = wx - (px - W / 2.0) / worldPx(z); cy = wy - (py - H / 2.0) / worldPx(z);
        cx -= std::floor(cx);
        clampY();
    }
    /** The sticks, once a loop: the pointer (left) and the pan (right) — see the header for the feel. */
    void sticks(double lx, double ly, double rx, double ry, double now) {
        const double dt = last_ > 0 ? std::min(0.1, now - last_) : 0;
        last_ = now;
        if (dt <= 0) return;
        const double sx[2] = {lx, rx}, sy[2] = {ly, ry};
        for (int k = 0; k < 2; k++) {
            const int role = k == 0 ? kMapLeftStick : kMapRightStick;
            const double mag = std::hypot(sx[k], sy[k]);
            const double dz = 0.14;
            if (mag < dz) { if (role == MAP_STICK_POINTER) rimSince_ = 0; continue; }
            const double f = std::pow((mag - dz) / (1 - dz), 1.7);
            const double ux = sx[k] / mag, uy = sy[k] / mag;
            if (role == MAP_STICK_POINTER) {
                // ★ acceleration held at the rim: 1× → 2.4× over a second after 0.35 s
                if (mag > 0.93) { if (!rimSince_) rimSince_ = now; } else rimSince_ = 0;
                const double acc = rimSince_ ? 1 + 1.4 * std::min(1.0, std::max(0.0, (now - rimSince_ - 0.35) / 1.0)) : 1;
                const double v = f * 420 * acc;
                const float nx = (float)(px + ux * v * dt), ny = (float)(py + uy * v * dt);
                const float x0 = area.x + 6, x1 = area.r() - 6, y0 = area.y + 6, y1 = area.b() - 6;
                px = std::max(x0, std::min(x1, nx)); py = std::max(y0, std::min(y1, ny));
                // ★ pushed against an edge: the map scrolls under it
                const double ex = nx < x0 ? nx - x0 : nx > x1 ? nx - x1 : 0, ey = ny < y0 ? ny - y0 : ny > y1 ? ny - y1 : 0;
                if (ex || ey) pan(ex * 1.5, ey * 1.5);
                ptrAt = now;
            } else {
                const double v = f * 950;
                pan(ux * v * dt, uy * v * dt);
            }
        }
    }

    /** Copy the basemap to the screen, re-rendering it first if the view left the buffer (or new data came). */
    void draw(Ctx& c, double now, uint64_t frame) {
        MapRender& R = mapRender();
        MapStore& st = mapStore();
        st.pump(frame);
        if (!R.ready()) { c.g.rect(area.x, area.y, area.w, area.h, mp::sea); return; }
        const double wpx = worldPx(z);
        double dx = cx - R.fcx; dx -= std::round(dx);
        const double ox = dx * wpx, oy = (cy - R.fcy) * wpx;
        const bool outside = std::fabs(ox) > R.margin - 2 || std::fabs(oy) > R.margin - 2;
        const bool gen = st.generation() != gen_ && now - renderedAt_ > 0.25;
        const bool settle = (std::fabs(ox) > 0.5 || std::fabs(oy) > 0.5) && now - movedAt_ > 0.35 && now - renderedAt_ > 0.35;
        if (std::fabs(ox - lastOx_) > 0.01 || std::fabs(oy - lastOy_) > 0.01 || R.fz != z) movedAt_ = now;
        lastOx_ = ox; lastOy_ = oy;
        /* ★★ ONE MORE RENDER WHEN THE LOADING ENDS (Stuart, 2026-11-08: the map reopened as "a zoomed in UK over a zoomed
         *  out USA"). handheld44's log: with no clock changed at all, Panfrost still reset the GPU in the first ~12 s of
         *  a map, while its files were going up — and a reset throws away the render in flight, leaving the buffer's old
         *  picture under the new one. A render made while files were still loading is redone 1.5 s after the last one
         *  arrived, so whatever a reset cut short is drawn again whole. */
        const bool loadingNow = st.pending() > 0;
        const bool repair = loadRender_ && !loadingNow && now - renderedAt_ > 1.5;
        if (dirty || R.fz != z || outside || gen || settle || repair || frame == 0) {
            c.g.flush();
            const uint64_t g0 = st.generation();
            R.render(st, c.tc, mapFonts(), cx, cy, z, mapProfile(profile), frame);
            gen_ = g0; renderedAt_ = now; dirty = false; loadRender_ = loadingNow;
            glViewport(0, 0, W, H);
            dx = 0; lastOx_ = lastOy_ = 0;
        }
        double ddx = cx - R.fcx; ddx -= std::round(ddx);
        const float sx = (float)std::round(R.margin + ddx * wpx), sy = (float)std::round(R.margin + (cy - R.fcy) * wpx);
        const float u0 = sx / R.fboW, u1 = (sx + W) / R.fboW, v0 = 1 - sy / R.fboH, v1 = 1 - (sy + H) / R.fboH;
        c.g.imageUV(R.tex, 0, 0, (float)W, (float)H, u0, v0, u1, v1, rgb(0xffffff));
        blitX = sx; blitY = sy;
    }
    /** Where the buffer's pixel (0,0) is on the screen: a basemap POI box at (bx, by) is at (bx − blitX, by − blitY). */
    float blitX = 0, blitY = 0;

    /** The pointer: a clear crosshair with a dark edge (and a ring at its middle). */
    void drawPointer(Ctx& c, double now, Rgba col = th::amber) const {
        if (!showPointer) return;
        const float a = pointerLive(now) || ptrAt >= dpadAt ? 1.f : 0.45f;
        const Rgba k = rgb(0, 0.75f * a), cc{col.r, col.g, col.b, col.a * a};
        for (int pass = 0; pass < 2; pass++) {
            const float t = pass ? 2 : 4; const Rgba q = pass ? cc : k;
            c.g.rect(px - 16, py - t / 2, 10, t, q); c.g.rect(px + 6, py - t / 2, 10, t, q);
            c.g.rect(px - t / 2, py - 16, t, 10, q); c.g.rect(px - t / 2, py + 6, t, 10, q);
            c.g.circle(px, py, pass ? 4.5f : 5.5f, pass ? 1.6f : 3.2f, q);
        }
    }

    /** The data credit (a CONDITION of the CC BY / ODbL layers, not a courtesy): bottom-right of the map area. */
    /** Returns the top of what it drew (a card above it starts there). */
    /** `extra`: the page's own data credit, a line of its own above the map's (the ADS-B page: Mictronics, ODC-By). */
    float drawAttribution(Ctx& c, float bottom, const std::string& extra = "") const {
        const MapIndex& ix = mapStore().index();
        static std::string base;
        if (base.empty() && ix.loaded) {
            base = "Natural Earth · NOAA ETOPO";
            for (auto& a : ix.attribution) base += " · " + a;
            base += " · OurAirports · NGA WPI";
        }
        if (base.empty() && extra.empty()) return bottom;
        const std::string text = extra.empty() ? base : base.empty() ? extra : extra + "\n" + base;
        const Font& f = mapFonts().reg11;
        std::vector<std::string> ls;
        size_t from = 0;
        while (from <= text.size()) {   // ★ each credit starts its own line
            const size_t nl = text.find('\n', from);
            for (auto& l : c.tc.wrap(f, text.substr(from, nl == std::string::npos ? std::string::npos : nl - from), (int)area.w - 16)) ls.push_back(l);
            if (nl == std::string::npos) break;
            from = nl + 1;
        }
        const float lh = (float)f.lineHeight();
        float y = bottom - lh * ls.size() - 4;
        c.g.rect(area.x, y - 2, area.w, lh * ls.size() + 6, rgb(0x000000, 0.45f));
        const float top = y - 2;
        for (auto& l : ls) { c.tc.draw(c.g, f, l, area.r() - 6, y, rgb(0xd8e8dc, 0.78f), 2); y += lh; }
        return top;
    }

private:
    static constexpr int W = 640, H = 480;
    double last_ = 0, rimSince_ = 0, renderedAt_ = 0, movedAt_ = 0, lastOx_ = 0, lastOy_ = 0;
    bool loadRender_ = false;   // the last render was made while files were still loading (the repair render)
    uint64_t gen_ = ~0ull;
    void clampY() {
        const double half = H / 2.0 / worldPx(z);
        if (half * 2 >= 1) { cy = 0.5; return; }
        cy = std::max(half, std::min(1 - half, cy));
    }
};

}  // namespace hh
