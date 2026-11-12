// vibe-hhui — the R36S's native receiver UI (docs/v12/R36S-NATIVE-UI.md). SDL2 on KMSDRM + OpenGL ES 3 on Panfrost,
// drawn straight to the 640×480 panel; a client of the handheld engine on 127.0.0.1:48000 like every VibeSDR client.
//
// ★★★ THE LOOK IS THE WEB HANDHELD PAGE'S (Stuart, 2026-11-06: "replicate the look we chose for the web so the portrait
//     view minus the tune/zoom buttons … slightly larger … so it is easier to read on the 640x480 3.5" screen"): the
//     spectrum and waterfall full width, the portrait control island at the foot — the pill (signal gradient behind the
//     frequency, mode and reading), then STEP · MENU · AUDIO · DEVICE — and the web's sheets over it.
//     Text is ATKINSON HYPERLEGIBLE everywhere (Stuart, same day).
// ★★ CONTROLS — Stuart's scheme, BY POSITION (memory: gamepad_mapping), two-stick pads (the R36S):
//    D-pad = move the highlight · bottom face = enter · right face = back
//    LEFT stick turned like a jog wheel = tune (clockwise up) · RIGHT stick turned = zoom (clockwise in)
//    L / R shoulders = zoom out / in · L2 / R2 = tune down / up one step · SELECT = the menu
// ★★★ Hints use Nintendo Switch-style glyphs: four circles, the meant one solid (padFace) — never a letter.
// ★ Draws at most 30 frames a second (the handheld's drawing rate), nothing at all while the screen is off.
#include "app.h"

int main(int argc, char** argv) { return hh::runApp(argc, argv); }
