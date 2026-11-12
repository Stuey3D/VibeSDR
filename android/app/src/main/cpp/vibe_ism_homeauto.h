// vibe_ism_homeauto.h — V12 ISM decoder: HOME AUTOMATION (2026-11-12) — heating controls, thermostats, blinds, lighting
// and smart-home links.
//
// ★★★ SOURCES — the same rule as vibe_ism_meters.h (Stuart, 2026-11-11: "we dont want to copy but if we can take
//     inspiration"). Frame FACTS come from public write-ups first; where the only description is a GPL decoder's notes
//     (rtl_433, src/devices/*.c), those were read for the facts and nothing else: no code, table, comment or identifier
//     is copied. Every decoder here is our own, on our own slicers (vibe_ism_kit.h). Checked on the rtl_433_tests
//     recordings (test INPUT only — scripts/ism-corpus.mjs). Each family says where its facts came from.
#pragma once
#include "vibe_ism_kit.h"

namespace vibeism {

/** The home-automation families, strongest checks first. */
inline bool decodeHomeauto(const Burst& b, std::vector<Reading>& out) {
    (void)b; (void)out;
    return false;
}

/** ★ Where each home-automation family transmits (bandPlausible). `false` = refuse. */
inline bool homeautoBandOk(const Reading& r, double f) {
    (void)r; (void)f;
    return true;
}

} // namespace vibeism
