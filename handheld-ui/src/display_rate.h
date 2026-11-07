// display_rate.h — DISPLAY RATE: pick the panel's mode live, as ROCKNIX does (Stuart, 2026-11-07: "can you not do what
// rocknix does and offer a display rate switcher in the app?"), for FM interference from the screen.
//
// ★★ THE MODES are the panel overlay's: ROCKNIX's twelve plus four of ours with the 80 Hz mode's proven geometry and only
//    the pixel clock changed (image/r36s/build-image.sh — the 30 MHz default puts its 3rd harmonic at 90 MHz, in the FM
//    band). SDL lists them; libdrm says each one's real pixel clock and timings, so the row can name them honestly.
// ★★★ KEEP OR REVERT. Two of ROCKNIX's modes garbled this panel (2026-11-06) — a garbled screen cannot reach the menu to
//     undo itself. So every change asks "Keep …?" for 10 s: the bottom button keeps it; anything else, or nothing,
//     puts the previous mode back. Only a KEPT mode is remembered (Settings.dispClock) and applied at the next start.
#pragma once
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcntl.h>
#include <unistd.h>

namespace hh {

struct DispMode {
    SDL_DisplayMode sdl{};
    int clockKHz = 0, hz10 = 0;          // pixel clock; refresh × 10
    bool proven = false;                 // the 80 Hz mode's geometry (640,46,2,44 / 480,16,2,14) — the picture is identical
    bool preferred = false;              // the overlay's default
    std::string label;
};
struct DispRate {
    bool listed = false;
    std::vector<DispMode> modes;
    int cur = -1;                        // the mode on the panel now
    int pick = -1;                       // the row's highlighted choice
    int prev = -1; double deadline = 0;  // a change waiting to be kept
};
inline DispRate& dispRate() { static DispRate d; return d; }

/** Fill the list once: SDL's modes, each matched to the kernel's (its index is SDL KMSDRM's driverdata). */
inline void dispRateList(App& a) {
    DispRate& d = dispRate();
    if (d.listed) return;
    d.listed = true;
    const char* drv = SDL_GetCurrentVideoDriver();
    if (!drv || std::strcmp(drv, "KMSDRM") != 0) return;   // tests (offscreen): one fixed mode, no row
    std::vector<drmModeModeInfo> km;
    const int fd = ::open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        if (drmModeRes* res = drmModeGetResources(fd)) {
            for (int i = 0; i < res->count_connectors && km.empty(); i++) {
                drmModeConnector* con = drmModeGetConnector(fd, res->connectors[i]);
                if (con && con->connection == DRM_MODE_CONNECTED) km.assign(con->modes, con->modes + con->count_modes);
                if (con) drmModeFreeConnector(con);
            }
            drmModeFreeResources(res);
        }
        ::close(fd);
    }
    const int n = SDL_GetNumDisplayModes(0);
    SDL_DisplayMode now{};
    SDL_GetCurrentDisplayMode(0, &now);
    for (int i = 0; i < n; i++) {
        DispMode m;
        if (SDL_GetDisplayMode(0, i, &m.sdl) != 0 || m.sdl.w != a.W || m.sdl.h != a.H) continue;
        const int idx = m.sdl.driverdata ? *(const int*)m.sdl.driverdata : -1;   // SDL_DisplayModeData { int mode_index; }
        if (idx >= 0 && idx < (int)km.size()) {
            const drmModeModeInfo& k = km[(size_t)idx];
            m.clockKHz = (int)k.clock;
            m.hz10 = (int)std::lround(k.clock * 10000.0 / ((double)k.htotal * k.vtotal));
            m.proven = k.hsync_start - k.hdisplay == 46 && k.hsync_end - k.hsync_start == 2 && k.htotal - k.hsync_end == 44
                    && k.vsync_start - k.vdisplay == 16 && k.vsync_end - k.vsync_start == 2 && k.vtotal - k.vsync_end == 14;
            m.preferred = (k.type & DRM_MODE_TYPE_PREFERRED) != 0;
        } else m.hz10 = m.sdl.refresh_rate * 10;
        char b[96];
        if (m.clockKHz) std::snprintf(b, sizeof b, "%.1f Hz · %.1f MHz%s", m.hz10 / 10.0, m.clockKHz / 1000.0, m.preferred ? " (default)" : m.proven ? "" : " · other timing");
        else std::snprintf(b, sizeof b, "%d Hz", m.sdl.refresh_rate);
        m.label = b;
        d.modes.push_back(m);
    }
    // proven geometry first (by clock), then ROCKNIX's others
    std::stable_sort(d.modes.begin(), d.modes.end(), [](const DispMode& x, const DispMode& y) {
        if (x.proven != y.proven) return x.proven;
        return x.clockKHz < y.clockKHz;
    });
    for (size_t i = 0; i < d.modes.size(); i++)
        if (d.modes[i].sdl.driverdata == now.driverdata || (d.cur < 0 && d.modes[i].preferred)) d.cur = (int)i;
    d.pick = d.cur;
}

inline bool dispRateSet(App& a, int i) {
    DispRate& d = dispRate();
    if (i < 0 || i >= (int)d.modes.size()) return false;
    if (SDL_SetWindowDisplayMode(a.win_, &d.modes[(size_t)i].sdl) != 0) {
        std::fprintf(stderr, "vibe-hhui: display mode %s refused: %s\n", d.modes[(size_t)i].label.c_str(), SDL_GetError());
        return false;
    }
    std::fprintf(stderr, "vibe-hhui: display mode %s\n", d.modes[(size_t)i].label.c_str());
    d.cur = i;
    return true;
}

/** At start: the KEPT mode (Settings.dispClock), if there is one. */
inline void dispRateStartup(App& a) {
    dispRateList(a);
    DispRate& d = dispRate();
    if (a.set_.dispClock <= 0) return;
    for (size_t i = 0; i < d.modes.size(); i++)
        if (d.modes[i].clockKHz == a.set_.dispClock && d.modes[i].proven == (a.set_.dispProven != 0)) { if ((int)i != d.cur) dispRateSet(a, (int)i); d.pick = d.cur; return; }
}

/** A button while a change waits: the bottom one keeps it, anything else puts the old mode back. True = taken. */
inline bool dispRatePress(App& a, Btn b) {
    DispRate& d = dispRate();
    if (d.prev < 0) return false;
    if (b == B_ENTER) {
        const DispMode& m = d.modes[(size_t)d.cur];
        a.set_.dispClock = m.preferred ? 0 : m.clockKHz;
        a.set_.dispProven = m.proven ? 1 : 0;
        a.dirty();
        a.toast("Display rate kept: " + m.label);
    } else {
        dispRateSet(a, d.prev);
        a.toast("Display rate put back");
    }
    d.prev = -1; d.pick = d.cur;
    return true;
}

inline void dispRateTick(App& a) {
    DispRate& d = dispRate();
    if (d.prev >= 0 && nowSec() > d.deadline) { dispRateSet(a, d.prev); d.prev = -1; d.pick = d.cur; a.toast("Display rate put back"); }
}

/** The keep-or-revert card, over everything. */
inline void dispRateDraw(App& a, Ctx& c) {
    DispRate& d = dispRate();
    if (d.prev < 0) return;
    const int left = std::max(0, (int)std::ceil(d.deadline - nowSec()));
    const Rect r{60, 150, (float)a.W - 120, 150};
    c.g.rect(0, 0, (float)a.W, (float)a.H, rgb(0, 0.6f));
    c.g.rrect(r.x, r.y, r.w, r.h, 14, rgb(0x0c0802, 0.98f));
    c.g.rframe(r.x, r.y, r.w, r.h, 14, 2, th::amber);
    c.tc.draw(c.g, c.f.title, "Keep this display rate?", r.cx(), r.y + 16, th::white, 1);
    c.tc.draw(c.g, c.f.body, d.modes[(size_t)d.cur].label, r.cx(), r.y + 52, th::btnText, 1);
    c.tc.draw(c.g, c.f.small, "Putting the previous one back in " + std::to_string(left) + " s", r.cx(), r.y + 84, th::dim, 1);
    const float hw = 360;
    hints(c, r.cx() - hw / 2, r.y + 122, {{FACE_BOTTOM, "keep"}, {-1, "any other button: put it back"}}, th::white);
}

/** The DISPLAY section's row. Returns the y after it. */
inline float dispRateRow(App& a, Ctx& c, Rect ar, float y) {
    dispRateList(a);
    DispRate& d = dispRate();
    if (d.modes.size() < 2) return y;   // a display with one mode: no row (AGENTS.md — never a dead control)
    const float y0 = y;
    y = a.menuRow(c, ar, y, "DISPLAY RATE",
                  "Screen noise on FM? Try another rate: left / right to choose, press to switch. The modes without "
                  "\"other timing\" keep the default's picture exactly and move only its clock — the 30 MHz default's "
                  "3rd harmonic is 90 MHz. Each change asks to be kept, or goes back by itself after 10 s.");
    const Rect pr{ar.x + 170, y0 + 6, ar.w - 176, 34};
    const Nav::Hit h = c.nav.item("d.rate", {ar.x, y0, ar.w, 44}, true);
    if (h.adjust) d.pick = (d.pick + h.adjust + (int)d.modes.size()) % (int)d.modes.size();
    if (h.pressed && d.pick != d.cur && d.prev < 0) {
        const int was = d.cur;
        if (dispRateSet(a, d.pick)) { d.prev = was; d.deadline = nowSec() + 10; }
    }
    const DispMode& m = d.modes[(size_t)std::max(0, d.pick)];
    const bool on = d.pick == d.cur;
    c.tc.draw(c.g, c.f.body, std::string("‹ ") + m.label + " ›", pr.r() - 4, pr.cy() - c.f.body.lineHeight() / 2.f,
              on ? th::activeTx : th::btnText, 2);
    if (h.focused) focusRing(c, pr, 6, true);
    return y;
}

}  // namespace hh
