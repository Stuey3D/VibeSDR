// sec_display.h — MENU → DISPLAY (inventory §4b + §4c.1) (one of the menu's parts; see app.h for the App's state and
// helpers, ui.h for the widgets).
//
// Rows, in the web's order: the handheld's own (BRIGHTNESS · SPECTRUM RATE · SCREEN OFF AFTER), the view rows moved from
// RADIO (VFO CENTRE · CENTRE / FIT), then the page's DISPLAY (COLOUR MAP — with CUSTOM: five colours of your own — and
// RESET), WATERFALL, SPECTRUM and VFO.
// Left out (inventory, AGENTS.md "a control that does nothing should not be there"):
//   DRAWING RATE (the loop's 30 fps cap is fixed in app_impl.h) · SPECTRUM / WATERFALL split (the layout is fixed) ·
//   TRACE show/hide and FILL OPACITY (the spectrum is always drawn, the fill has no opacity) · SIGNAL LABELS (no labels
//   on the native waterfall) · GLOW / FROSTED BACKING (the needle is a plain line) · CONTROLS colours (ui.h's theme is
//   constant) · MOUSE WHEEL / FREQUENCY AT POINTER (no wheel, no pointer) · SAVE FOR ALL SERVERS (never on 127.0.0.1) ·
//   the hidden WATERFALL DATA / SPEED UP WHILE TUNING rows and their orphaned notes.
#pragma once
// (included by app.h after App is declared)
#include "disp_settings.h"
#include "hw_pump.h"
#include "hw_state.h"

namespace hh {

/** A sub-heading inside the section (the web's <h2> WATERFALL / SPECTRUM / VFO). */
inline float dispHead(Ctx& c, Rect a, float y, const char* t) {
    y += 6;
    float x = a.x + 6;
    for (const char* p = t; *p; p++) { const std::string one(1, *p); x += c.tc.draw(c.g, c.f.small, one, x, y, th::dim) + 3; }
    c.g.rect(a.x + 6, y + c.f.small.lineHeight() + 4, a.w - 12, 1, th::barBorder);
    return y + c.f.small.lineHeight() + 14;
}

/** A colour the D-pad steps through (left / right): the chip, then "‹ Name ›". Returns true when it changed. */
inline bool dispSwatchStepper(Ctx& c, const std::string& id, Rect row, Rect r, uint32_t& rgbv) {
    const auto& sw = dispSwatches();
    const Nav::Hit h = c.nav.item(id, row, true);
    bool changed = false;
    int i = dispSwatchIndex(rgbv);
    if (h.adjust || h.pressed) {
        i = (i + (h.adjust ? h.adjust : 1) + (int)sw.size()) % (int)sw.size();
        rgbv = sw[(size_t)i].rgb; changed = true;
    }
    c.g.rrect(r.x, r.y + 4, 46, r.h - 8, 6, rgb(rgbv));
    c.g.rframe(r.x, r.y + 4, 46, r.h - 8, 6, 1, th::btnBorder);
    c.tc.draw(c.g, c.f.body, std::string("‹ ") + sw[(size_t)i].name + " ›", r.r() - 4, r.cy() - c.f.body.lineHeight() / 2.f, th::btnText, 2);
    if (h.focused) focusRing(c, r, 6, true);
    return changed;
}

inline float App::secDisplay(Ctx& c, const RadioState& s, Rect a, float y) {
    static double lastDrawn = 0;
    hwOnOpen(*this, lastDrawn);   // the backlight's figure (refreshDeviceRows)
    hwPumpUi(*this);
    DispSettings& d = dispSet();
    auto look = [&] { applyLook(); dispApply(*this); };
    const float valY = 22 - c.f.body.lineHeight() / 2.f;

    // a label + slider + value row; returns true when it moved
    auto sliderRow = [&](const char* id, const std::string& label, const std::string& help, double& v, double lo, double hi,
                         double step, const std::string& shown) -> bool {
        const float y0 = y;
        y = menuRow(c, a, y, label, help);
        // ★ ONE COLUMN for every slider on the page (Stuart, 2026-11-07: "can you align them please") — the widest label's,
        //   not each its own, which left them starting at four different places
        (void)label;
        const float lw = std::max(150.f, (float)c.tc.width(c.f.bodyB, "WATERFALL BRIGHTNESS") + 24);
        const bool ch = hwRowSlider(c, id, {a.x, y0, a.w, 44}, {a.x + lw, y0 + 2, a.w - lw - 76, 40}, v, lo, hi, step);
        c.tc.draw(c.g, c.f.body, shown, a.r() - 6, y0 + valY, th::dim, 2);
        return ch;
    };
    auto fmt = [](const char* f, double v) { char b[32]; std::snprintf(b, sizeof b, f, v); return std::string(b); };

    // ── the handheld's own rows ──
    // BRIGHTNESS (the backlight, 1–255 — the supervisor clamps, so it is never black)
    {
        double v = brightness_;
        if (sliderRow("d.bri", "BRIGHTNESS", "", v, 5, 255, 10, std::to_string(brightness_))) {
            brightness_ = (int)v; const std::string m = "{\"set\":\"brightness\",\"value\":" + std::to_string(brightness_) + "}";
            sendLatest("d.bri", [this, m] { eng_->device(m); });   // ★ a held slider: ≤ 10 POSTs a second, the last always
        }
    }
    // DISPLAY RATE (display_rate.h — Stuart, 2026-11-07: screen noise on FM)
    y = dispRateRow(*this, c, a, y);
    // ★★ (SPECTRUM RATE is gone — Stuart, 2026-11-07: "loose the 5/10/20 raw data and simply stick with 5FPS and use the
    //    interpolator for scrolling": the engine always sends 5, WATERFALL SPEED below says how it scrolls)
    float y0 = y;
    // IDLE POWER SAVE (App::idleNow_)
    // ★ Stuart, 2026-11-07: "give an option to disable the powersave with the warning that it will reduce battery life"
    y = menuRow(c, a, y, "IDLE POWER SAVE", set_.idleSave
        ? "Untouched for a minute, or behind a menu or a decoder: the waterfall drops to its NATIVE 5 rows a second — any button brings your speed straight back. Longer on battery; the audio is not touched"
        : "OFF — the waterfall and the screen run at full speed all the time. This WILL shorten the battery life: switch it back on for the longest listening");
    {
        const int hit = segmented(c, "d.idle", {a.x + 6, y - 4, a.w - 12, 38}, {"OFF", "ON"}, set_.idleSave ? 1 : 0);
        if (hit >= 0) { set_.idleSave = hit; lastInput_ = nowSec(); dirty(); }
        y += 46;
    }
    // SCREEN OFF AFTER
    y = menuRow(c, a, y, "SCREEN OFF AFTER", "Dims at half the time; audio keeps playing; any button wakes it");
    {
        const int mins[5] = {0, 1, 2, 5, 10};
        int sel = 0; for (int i = 0; i < 5; i++) if (mins[i] == set_.screenOffMin) sel = i;
        const int hit = segmented(c, "d.off", {a.x + 6, y - 4, a.w - 12, 38}, {"NEVER", "1 m", "2 m", "5 m", "10 m"}, sel);
        if (hit >= 0) { set_.screenOffMin = mins[hit]; lastInput_ = nowSec(); dirty(); }
        y += 46;
    }
    // ── the view rows (RADIO's .mOnly, moved here) ──
    // VFO CENTRE (not on an Icom: its own VFO drives the view) · CENTRE · FIT — one row of three under the label, so the
    // D-pad reaches each (two half-width keys under a row-wide one were skipped by DOWN)
    {
        const bool icom = s.hwDriver == "icom";
        y = menuRow(c, a, y, icom ? "VIEW" : "VFO CENTRE",
                    icom ? "" : "LOCKED: the view stays centred on the dial. FREE: the view stays where it is while you tune.");
        const int n = icom ? 2 : 3;
        const float gap = 8, bw = (a.w - 12 - gap * (n - 1)) / n;
        float x = a.x + 6;
        if (!icom) {
            if (toggle(c, "d.vfoc", {x, y - 4, bw, 44}, set_.followVfo, "LOCKED", "FREE")) { set_.followVfo = !set_.followVfo; dirty(); }
            x += bw + gap;
        }
        BtnStyle st; st.font = &c.f.btn;
        if (button(c, "d.centre", {x, y - 4, bw, 44}, "CENTRE", st) && s.spanHz > 0) {
            // spec.pan(spec.frequency): the view back onto the dial, at the span it has
            eng_->zoom(s.vfoHz, s.spanHz, !set_.followVfo);
            viewPending_ = false;
            wfp_.setView(s.vfoHz, s.spanHz); wfp_.notifyInteraction(nowSec());
        }
        x += bw + gap;
        if (button(c, "d.fit", {x, y - 4, bw, 44}, "FIT", st)) { eng_->zoomReset(); viewPending_ = false; wfp_.notifyInteraction(nowSec()); }
        y += 52;
    }

    // ── DISPLAY ──
    // COLOUR MAP: left / right through the app's palettes, then CUSTOM (five colours of your own, below)
    y0 = y;
    y = menuRow(c, a, y, "COLOUR MAP", "");
    {
        const int n = wf::paletteCount();
        int v = d.custom ? n : wf::paletteByName(set_.palette);
        const Rect pr{a.x + 150, y0 + 6, a.w - 156, 34};
        const Nav::Hit h = c.nav.item("d.pal", {a.x, y0, a.w, 44}, true);
        if (h.adjust) {
            v = (v + h.adjust + n + 1) % (n + 1);
            d.custom = v == n;
            if (!d.custom) { set_.palette = wf::paletteName(v); dirty(); }
            dispDirty();
            look();
        }
        const wf::Palette p = d.custom ? wf::paletteFromStops({d.stops[0], d.stops[1], d.stops[2], d.stops[3], d.stops[4]}) : wf::builtinPalette(v);
        const float sw = pr.w - 190;
        for (int i = 0; i < 64; i++) {
            const uint8_t* px = &p.rgba[(i * 4) * 4];
            c.g.rect(pr.x + sw * i / 64.f, pr.y + 4, sw / 64.f + 0.5f, pr.h - 8, {px[0] / 255.f, px[1] / 255.f, px[2] / 255.f, 1});
        }
        c.g.rframe(pr.x, pr.y + 4, sw, pr.h - 8, 2, 1, th::btnBorder);
        c.tc.draw(c.g, c.f.body, std::string("‹ ") + (d.custom ? std::string("Custom") : set_.palette) + " ›", pr.r() - 4, pr.cy() - c.f.body.lineHeight() / 2.f, th::btnText, 2);
        if (h.focused) focusRing(c, pr, 6, true);
    }
    // CUSTOM: the five stops, weakest first (Stuart's names). Guard rail: each should be brighter than the one before,
    // or a weak signal hides in the noise floor.
    if (d.custom) {
        const char* const* names = dispStopNames();
        for (int i = 0; i < 5; i++) {
            y0 = y;
            const bool wide = i == 4;   // "ABSOLUTE BEAST OF A SIGNAL" takes the whole label line
            y = menuRow(c, a, y, names[i], "");
            const Rect r = wide ? Rect{a.x + 150, y - 2, a.w - 156, 40} : Rect{a.x + 150, y0 + 3, a.w - 156, 38};
            if (wide) y += 44;
            if (dispSwatchStepper(c, "d.stop" + std::to_string(i), {a.x, y0, a.w, r.b() - y0}, r, d.stops[i])) { dispDirty(); look(); }
        }
        bool rising = true;
        for (int i = 1; i < 5; i++) if (dispLuma(d.stops[i]) <= dispLuma(d.stops[i - 1])) rising = false;
        if (!rising) {
            for (auto& l : c.tc.wrap(c.f.small, "Each colour should be brighter than the one above it — otherwise a weak signal "
                                                "can look stronger than a strong one, or vanish into the floor.", (int)a.w - 12)) {
                c.tc.draw(c.g, c.f.small, l, a.x + 6, y, th::red); y += c.f.small.lineHeight();
            }
            y += 8;
        }
    }
    // RESET: back to the defaults (the look only — the rates, the screen timer and the backlight stay)
    y0 = y;
    y = menuRow(c, a, y, "RESET", "");
    {
        BtnStyle st; st.font = &c.f.body; st.radius = 7;
        if (hwRowButton(c, "d.reset", {a.x, y0, a.w, 44}, {a.r() - 206, y0 + 3, 200, 38}, "DEFAULTS", st)) {
            const Settings def;
            set_.palette = def.palette; set_.contrast = def.contrast; set_.sharpness = def.sharpness; set_.wfBrightness = def.wfBrightness;
            const uint32_t keepStops[5] = {d.stops[0], d.stops[1], d.stops[2], d.stops[3], d.stops[4]};
            d = DispSettings();
            for (int i = 0; i < 5; i++) d.stops[i] = keepStops[i];   // your colours are kept for next time; the map is the default
            dirty(); dispDirty(); look();
            toast("Display back to its defaults");
        }
    }

    // ── WATERFALL ──
    y = dispHead(c, a, y, "WATERFALL");
    // WATERFALL SPEED (the native loop's own: rows a second the waterfall scrolls at)
    // ★★ NATIVE APART, THE INTERPOLATED THREE BOXED (Stuart, 2026-11-07: "native sounds like it should be higher than ECO so
    //    What i would like is Native as its own separate button … then the eco default smooth are boxed and called
    //    interpolated for smoothness"). wf::Scroll: Sharp 2 (NATIVE) · Eco 3 (10) · Default 4 (15) · Smooth 5 (30) — the
    //    app's and the web's rules for the engine's fixed 5 frames a second.
    y = menuRow(c, a, y, "WATERFALL SPEED", "NATIVE draws the 5 lines a second the radio sends. INTERPOLATED scrolls smoothly between them — every row a real one, repeated, never blended.");
    {
        const float bh = 38, cap = 18, pad = 6;
        const Rect nat{a.x + 6, y - 4 + cap + pad, 132, bh};
        BtnStyle ns; ns.lit = set_.speed == 2; ns.font = &c.f.body; ns.radius = 7;
        if (button(c, "d.spd#n", nat, "NATIVE", ns)) { set_.speed = 2; look(); dirty(); }
        const Rect box{nat.r() + 14, y - 4, a.r() - 6 - (nat.r() + 14), cap + pad + bh + pad};
        c.g.rframe(box.x, box.y, box.w, box.h, 9, 1, th::btnBorder);
        c.tc.draw(c.g, c.f.small, "INTERPOLATED", box.x + 10, box.y + 3, th::dim);
        const int order[3] = {3, 4, 5};
        int sel = -1; for (int i = 0; i < 3; i++) if (order[i] == set_.speed) sel = i;
        const int hit = segmented(c, "d.spd", {box.x + pad, box.y + cap + pad, box.w - 2 * pad, bh}, {"ECO 10", "DEFAULT 15", "SMOOTH 30"}, sel);
        if (hit >= 0) { set_.speed = order[hit]; look(); dirty(); }
        y += box.h + 8;
    }
    // RANGE: AUTO / MANUAL
    y0 = y;
    y = menuRow(c, a, y, "RANGE", "");
    {
        // two choices, one highlight box the width of the row (DOWN skipped a two-key segment here): left / right pick,
        // press flips
        const Rect sr{a.x + 150, y0 + 3, a.w - 156, 38};
        const Nav::Hit h = c.nav.item("d.range", {a.x, y0, a.w, 44}, true);
        bool manual = d.rangeManual;
        if (h.adjust) manual = h.adjust > 0;
        if (h.pressed) manual = !manual;
        if (manual != d.rangeManual) { d.rangeManual = manual; dispDirty(); look(); }
        const float gw = (sr.w - 6) / 2;
        const char* opt[2] = {"AUTO", "MANUAL"};
        for (int i = 0; i < 2; i++) {
            const Rect b{sr.x + i * (gw + 6), sr.y, gw, sr.h};
            const bool lit = (i == 1) == d.rangeManual;
            c.g.rrect(b.x, b.y, b.w, b.h, 7, lit ? th::onBg : th::btnBg);
            c.g.rframe(b.x, b.y, b.w, b.h, 7, 1, lit ? th::onBg : th::btnBorder);
            textIn(c, c.f.body, opt[i], b, lit ? th::onText : th::btnText);
        }
        if (h.focused) focusRing(c, sr, 7, true);
    }
    if (!d.rangeManual) {
        if (sliderRow("d.ac", "AUTO CONTRAST", "", d.autoContrast, 0, 20, 1, fmt("%g", d.autoContrast))) { dispDirty(); look(); }
    } else {
        if (sliderRow("d.min", "MIN dB", "", d.minDb, -160, -60, 5, fmt("−%g", -d.minDb))) { dispDirty(); look(); }
        if (sliderRow("d.max", "MAX dB", "", d.maxDb, -100, 0, 5, d.maxDb < 0 ? fmt("−%g", -d.maxDb) : std::string("0"))) { dispDirty(); look(); }
    }
    // BRIGHTNESS · CONTRAST · SHARPNESS (the waterfall's — "WATERFALL BRIGHTNESS", as the backlight's row is BRIGHTNESS)
    if (sliderRow("d.wbr", "WATERFALL BRIGHTNESS", "", set_.wfBrightness, -20, 20, 2, fmt("%g", set_.wfBrightness))) { look(); dirty(); }
    if (sliderRow("d.con", "CONTRAST", "", set_.contrast, -10, 10, 1, fmt("%g", set_.contrast))) { look(); dirty(); }
    if (sliderRow("d.shp", "SHARPNESS", "", set_.sharpness, 0, 10, 1, fmt("%g", set_.sharpness))) { look(); dirty(); }
    // SPATIAL SMOOTH
    y0 = y;
    y = menuRow(c, a, y, "SPATIAL SMOOTH", "");
    if (hwRowToggle(c, "d.ss", {a.x, y0, a.w, 44}, {a.r() - 96, y0 + 3, 90, 38}, d.spatialSmooth)) { d.spatialSmooth = !d.spatialSmooth; dispDirty(); look(); }

    // ── SPECTRUM ──
    y = dispHead(c, a, y, "SPECTRUM");
    if (sliderRow("d.sm", "SMOOTHING", "", d.smoothing, 1, 10, 1, fmt("%g", d.smoothing))) { dispDirty(); look(); }
    if (sliderRow("d.fl", "FLOOR", "", d.specFloor, -20, 20, 1, fmt("%g", d.specFloor))) { dispDirty(); look(); }
    if (sliderRow("d.pk", "PEAK SCALE", "", d.peakScale, 1, 30, 1, fmt("%.1f×", d.peakScale / 10))) { dispDirty(); look(); }
    y0 = y;
    y = menuRow(c, a, y, "PEAK HOLD", "");
    if (hwRowToggle(c, "d.ph", {a.x, y0, a.w, 44}, {a.r() - 96, y0 + 3, 90, 38}, d.peakHold)) { d.peakHold = !d.peakHold; dispDirty(); look(); }

    // ── VFO ──
    y = dispHead(c, a, y, "VFO");
    y0 = y;
    y = menuRow(c, a, y, "NEEDLE COLOUR", "");
    if (dispSwatchStepper(c, "d.needle", {a.x, y0, a.w, 44}, {a.x + 190, y0 + 3, a.w - 196, 38}, d.needle)) { dispDirty(); look(); }
    return y;
}


}  // namespace hh
