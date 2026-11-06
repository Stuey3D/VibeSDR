// disp_settings.h — MENU → DISPLAY's own look settings (the ones settings.h does not hold): the waterfall's RANGE and
// AUTO CONTRAST, the spectrum's processing, the needle colour and the custom colour map. Kept in
// $HOME/.vibe-hhui-display.json, written with a temp file + rename (Stuart: "every write = temp file + rename").
// ★ The DEVICE's look, whichever radio is plugged in (the web keeps most of these per radio; on the handheld the screen
//   is the device's — one look, not one per dongle).
// ★ Included by sec_display.h (after App is declared). dispApply(App&) is the one place these reach the waterfall.
#pragma once
#include "json.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace hh {

/** A named colour the D-pad steps through (a <input type=color> has no D-pad form; a short list does). Dark → bright. */
struct DispSwatch { const char* name; uint32_t rgb; };
inline const std::vector<DispSwatch>& dispSwatches() {
    static const std::vector<DispSwatch> s = {
        {"Black", 0x000000}, {"Charcoal", 0x202020}, {"Navy", 0x000050}, {"Deep blue", 0x0010a0}, {"Purple", 0x6a00b0},
        {"Crimson", 0xa00030}, {"Teal", 0x008080}, {"Blue", 0x0060ff}, {"Grey", 0x808080}, {"Green", 0x00c040},
        {"Red", 0xff2020}, {"Magenta", 0xff00c8}, {"Orange", 0xff6000}, {"Sky", 0x3d9bff}, {"Amber", 0xffa000},
        {"Cyan", 0x00e5ff}, {"Lime", 0x9cff2e}, {"Yellow", 0xffe000}, {"White", 0xffffff},
    };
    return s;
}
inline int dispSwatchIndex(uint32_t rgb) {
    const auto& s = dispSwatches();
    for (size_t i = 0; i < s.size(); i++) if (s[i].rgb == rgb) return (int)i;
    return 0;
}
/** Relative luminance (WCAG, sRGB) 0..1 — the custom map's guard rail: each stop should be brighter than the last. */
inline double dispLuma(uint32_t rgb) {
    auto lin = [](double c) { c /= 255.0; return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin((rgb >> 16) & 255) + 0.7152 * lin((rgb >> 8) & 255) + 0.0722 * lin(rgb & 255);
}

/** The five custom colour stops — Stuart's names (memory custom_waterfall_palette_idea, 2026-10-01: keep that label). */
inline const char* const* dispStopNames() {
    static const char* const n[5] = {"FLOOR", "WEAK", "MEDIUM", "STRONG", "ABSOLUTE BEAST OF A SIGNAL"};
    return n;
}

struct DispSettings {
    // WATERFALL (the web's defaults)
    bool rangeManual = false;      // RANGE: AUTO / MANUAL
    double autoContrast = 5;       // 0..20
    double minDb = -120;           // −160..−60
    double maxDb = -20;            // −100..0
    bool spatialSmooth = true;
    // SPECTRUM
    double smoothing = 5;          // 1..10 (smoothingFrames)
    double specFloor = 0;          // −20..20
    double peakScale = 10;         // 1..30, shown ÷10 as "1.0×"
    bool peakHold = true;
    // VFO
    uint32_t needle = 0xff2020;    // NEEDLE COLOUR (also the spectrum's peak line, as the app)
    // the custom colour map (COLOUR MAP → CUSTOM)
    bool custom = false;
    uint32_t stops[5] = {0x000000, 0x0010a0, 0x00e5ff, 0xffe000, 0xffffff};

    static std::string path() {
        const char* home = std::getenv("HOME");
        return std::string(home && *home ? home : "/tmp") + "/.vibe-hhui-display.json";
    }
    void load() {
        FILE* f = std::fopen(path().c_str(), "r");
        if (!f) return;
        std::string s;
        char b[2048];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        std::fclose(f);
        const Json j = Json::parse(s);
        if (j.isNull()) return;
        rangeManual = j["rangeManual"].boolean(rangeManual);
        autoContrast = j["autoContrast"].num(autoContrast);
        minDb = j["minDb"].num(minDb);
        maxDb = j["maxDb"].num(maxDb);
        spatialSmooth = j["spatialSmooth"].boolean(spatialSmooth);
        smoothing = j["smoothing"].num(smoothing);
        specFloor = j["specFloor"].num(specFloor);
        peakScale = j["peakScale"].num(peakScale);
        peakHold = j["peakHold"].boolean(peakHold);
        needle = (uint32_t)j["needle"].num(needle);
        custom = j["custom"].boolean(custom);
        if (j["stops"].size() == 5) for (int i = 0; i < 5; i++) stops[i] = (uint32_t)j["stops"][(size_t)i].num(stops[i]);
    }
    void save() const {
        const std::string p = path(), tmp = p + ".tmp";
        FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "{\"rangeManual\":%s,\"autoContrast\":%g,\"minDb\":%g,\"maxDb\":%g,\"spatialSmooth\":%s,"
                        "\"smoothing\":%g,\"specFloor\":%g,\"peakScale\":%g,\"peakHold\":%s,\"needle\":%u,\"custom\":%s,"
                        "\"stops\":[%u,%u,%u,%u,%u]}\n",
                     rangeManual ? "true" : "false", autoContrast, minDb, maxDb, spatialSmooth ? "true" : "false",
                     smoothing, specFloor, peakScale, peakHold ? "true" : "false", (unsigned)needle, custom ? "true" : "false",
                     (unsigned)stops[0], (unsigned)stops[1], (unsigned)stops[2], (unsigned)stops[3], (unsigned)stops[4]);
        std::fflush(f);
        std::fclose(f);
        std::rename(tmp.c_str(), p.c_str());
    }
};

/** The one copy (loaded at first use). */
inline DispSettings& dispSet() {
    static DispSettings d;
    static bool loaded = false;
    if (!loaded) { d.load(); loaded = true; }
    return d;
}
/** Save soon (≤ 1 s after the last change; dispSaveTick does it). */
inline double& dispDirtyAt() { static double t = 0; return t; }
inline void dispDirty() { dispDirtyAt() = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void dispSaveTick(double now) {
    double& t = dispDirtyAt();
    if (t > 0 && now - t > 1.0) { dispSet().save(); t = 0; }
}

/** The needle colour for drawOverlays (the dial line in the spectrum and the waterfall). */
inline Rgba dispNeedle(float a = 1.f) { return rgb(dispSet().needle, a); }

/** ★ Everything DISPLAY sets that applyLook() does not: the waterfall's range and smoothing, the spectrum's processing,
 *  the needle (peak line) colour, and the custom colour map over the built-in one applyLook() just set. Call it after
 *  applyLook() — at the start and on every change. */
inline void dispApply(App& a) {
    const DispSettings& d = dispSet();
    wf::WaterfallConfig c = a.wfp_.config();
    c.sig.autoContrast = d.autoContrast;
    c.sig.manual = d.rangeManual;
    c.sig.manualMinDb = std::min(d.minDb, d.maxDb - 1);
    c.sig.manualMaxDb = d.maxDb;
    c.sig.spatialSmooth = d.spatialSmooth;
    c.sig.smoothingFrames = d.smoothing;
    c.sig.specFloor = d.specFloor;
    c.sig.specPeakScale = d.peakScale;
    c.sig.peakHold = d.peakHold;
    c.peakHold = d.peakHold;
    a.wfp_.setConfig(c);
    a.wfp_.setPeakColour(d.needle);
    if (d.custom) a.wfp_.setCustomPalette(wf::paletteFromStops({d.stops[0], d.stops[1], d.stops[2], d.stops[3], d.stops[4]}));
}

}  // namespace hh
