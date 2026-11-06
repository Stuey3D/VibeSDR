// sec_display.h — MENU → DISPLAY (one of the menu's parts; see app.h for the App's state and helpers, ui.h for the widgets).
#pragma once
// (included by app.h after App is declared)

namespace hh {

inline float App::secDisplay(Ctx& c, const RadioState& s, Rect a, float y) {
    (void)s;
    // SPECTRUM RATE
    float y0 = y;
    y = menuRow(c, a, y, "SPECTRUM RATE", "Real frames from the radio each second — every waterfall row is real");
    {
        const int rates[3] = {5, 10, 20};
        int sel = 0; for (int i = 0; i < 3; i++) if (rates[i] == set_.specRate) sel = i;
        const int hit = segmented(c, "d.rate", {a.r() - 210, y0 + 4, 204, 38}, {"5", "10", "20"}, sel);
        if (hit >= 0) { set_.specRate = rates[hit]; eng_->fftRate(set_.specRate); dirty(); }
    }
    // SCREEN OFF AFTER
    y0 = y;
    y = menuRow(c, a, y, "SCREEN OFF AFTER", "Dims at half the time; audio keeps playing; any button wakes it");
    {
        const int mins[5] = {0, 1, 2, 5, 10};
        int sel = 0; for (int i = 0; i < 5; i++) if (mins[i] == set_.screenOffMin) sel = i;
        const int hit = segmented(c, "d.off", {a.x + 6, y, a.w - 12, 40}, {"NEVER", "1 m", "2 m", "5 m", "10 m"}, sel);
        if (hit >= 0) { set_.screenOffMin = mins[hit]; dirty(); }
        y += 52;
    }
    // VFO CENTRE
    y0 = y;
    y = menuRow(c, a, y, "VFO CENTRE", "LOCKED: the view follows the dial. FREE: the view stays put until the dial leaves it.");
    if (toggle(c, "d.vfoc", {a.r() - 116, y0 + 4, 110, 38}, set_.followVfo, "LOCKED", "FREE")) { set_.followVfo = !set_.followVfo; dirty(); }
    // PALETTE (left / right step through the app's 26)
    y0 = y;
    y = menuRow(c, a, y, "PALETTE", "");
    {
        double v = wf::paletteByName(set_.palette);
        const Rect pr{a.x + 150, y0 + 6, a.w - 160, 34};
        const Nav::Hit h = c.nav.item("d.pal", pr, true);
        if (h.adjust) {
            const int n = wf::paletteCount();
            v = ((int)v + h.adjust + n) % n;
            set_.palette = wf::paletteName((int)v); applyLook(); dirty();
        }
        const wf::Palette p = wf::builtinPalette((int)v);
        const float sw = pr.w - 170;
        for (int i = 0; i < 64; i++) {
            const uint8_t* px = &p.rgba[(i * 4) * 4];
            c.g.rect(pr.x + sw * i / 64.f, pr.y + 4, sw / 64.f + 0.5f, pr.h - 8, {px[0] / 255.f, px[1] / 255.f, px[2] / 255.f, 1});
        }
        c.g.rframe(pr.x, pr.y + 4, sw, pr.h - 8, 2, 1, th::btnBorder);
        c.tc.draw(c.g, c.f.body, std::string("‹ ") + set_.palette + " ›", pr.r() - 4, pr.cy() - c.f.body.lineHeight() / 2.f, th::btnText, 2);
        if (h.focused) focusRing(c, pr, 6, true);
    }
    // CONTRAST · SHARPNESS · BRIGHTNESS (the app's ranges)
    struct Sl { const char* id; const char* label; double* v; double lo, hi, step; };
    Sl sls[3] = {{"d.con", "CONTRAST", &set_.contrast, -10, 10, 1}, {"d.shp", "SHARPNESS", &set_.sharpness, 0, 10, 1},
                 {"d.wbr", "WATERFALL BRIGHTNESS", &set_.wfBrightness, -20, 20, 2}};
    for (auto& sl : sls) {
        y0 = y;
        y = menuRow(c, a, y, sl.label, "");
        const float lw = (float)c.tc.width(c.f.bodyB, sl.label) + 30;
        if (slider(c, sl.id, {a.x + std::max(150.f, lw), y0 + 2, a.w - std::max(150.f, lw) - 70, 40}, *sl.v, sl.lo, sl.hi, sl.step)) { applyLook(); dirty(); }
        char b[16]; std::snprintf(b, sizeof b, "%g", *sl.v);
        c.tc.draw(c.g, c.f.body, b, a.r() - 6, y0 + 22 - c.f.body.lineHeight() / 2.f, th::dim, 2);
    }
    // WATERFALL SPEED
    y0 = y;
    y = menuRow(c, a, y, "WATERFALL SPEED", "Rows a second the waterfall scrolls at. NATIVE draws every frame as it arrives.");
    {
        // wf::Scroll: Min2 0, Low 1, Sharp(NATIVE) 2, Eco 3, Default 4, Smooth 5 — shown slowest → fastest
        const int order[5] = {1, 3, 4, 5, 2};
        int sel = 2; for (int i = 0; i < 5; i++) if (order[i] == set_.speed) sel = i;
        const int hit = segmented(c, "d.spd", {a.x + 6, y, a.w - 12, 40}, {"LOW", "ECO", "DEFAULT", "SMOOTH", "NATIVE"}, sel);
        if (hit >= 0) { set_.speed = order[hit]; applyLook(); dirty(); }
        y += 52;
    }
    return y;
}


}  // namespace hh
