# R36S native receiver UI — plan (2026-11-06)

**Status:** plan, agreed in principle with Stuart ("I suspect a nice lightweight native engine will be better";
"concentrate on getting the basic radio working first then the map afterwards"). The browser image stays the
working image until the native one does the basic radio better.

## Why (measured on Stuart's R36S, handheld5–8 debug logs)

| | CPU (of 400 % = four A35 cores) |
|---|---|
| WPEWebProcess with the browser playing audio (handheld5–7) | 177–186 % |
| …with the engine playing the audio (handheld8, speaker mode) | 113 % (main thread 81 %, record-buffer tap ~45 %) |
| Skia GPU painting instead of CPU (handheld6) | no change (the G31 here has ONE shader core) |
| vibeserver, HF+ at 912 kS/s (handheld8) | 113 % — 44 % of it libairspyhf's IQ balancer on its DESKTOP settings (fixed: `__arm__` is 32-bit only) |

All four cores at 1296 MHz and 74 °C; the battery estimate at 76 % was one hour. The browser is the part that no
setting fixes: layout, JavaScript and painting for a 640×480 screen.

## What it is

`vibe-hhui`: one C++ program on the R36S that draws straight to the panel — **SDL2 (KMSDRM) + OpenGL ES 2 on
Panfrost**, no compositor, no browser. It is a CLIENT of the same engine on 127.0.0.1:48000, over the same two
WebSockets every VibeSDR client uses (spectrum + control; audio stays in the engine — `speaker=1`,
`vibe_local_speaker.h`). The engine, the supervisor, vibe-hwd and the image are unchanged; the kiosk unit starts
`vibe-hhui` instead of Cog.

★ Nothing in the engine is special-cased for it: it speaks the shared-dial contract like the app does (server sends
full state, client has no vote, client sends only what the person did).

## First version — the basic radio (Stuart: radio first, map after)

- Waterfall + spectrum: each row is uploaded once as one texture line and the waterfall SCROLLS on the GPU (a ring
  texture, one `glTexSubImage2D` per row). Target 30 fps at a few % of one core.
- Frequency readout (display font), tuning: D-pad step, left stick jog (Stuart's scheme), L / R zoom.
- Mode and bandwidth; volume / mute (the engine's speaker level); battery and clock in the status row.
- R36S HARDWARE: location (locator / lat-lon typed with the pad keyboard), card, brightness, spectrum rate,
  power off / restart — through vibe-hwd and the supervisor's existing endpoints.
- Bookmarks: the list, tune to one, add the current frequency.
- "Please connect an SDR" and the radio-starting states, from the supervisor's session endpoint.

### Controls (memory: gamepad_mapping)
- BY POSITION: bottom face = enter / select, right face = back. D-pad = highlight; L / R = zoom.
- ★★★ Glyphs: **Nintendo Switch style** — a diamond of four circles, three hollow and the meant one SOLID; never a
  letter or a PlayStation shape (ABXY layouts differ between pads). Same rule as `web/client/src/padFace.ts`.
- SDL GameController for the pad (`r36s_Gamepad` through the same udev rule); the volume and power keys stay
  vibe-hwd's.

## Later, one at a time
SET FROM MAP (the offline map) · RDS / station names · decoders (FT8, NAVTEX, …) · DAB · recordings (the engine
records to the card, since the native UI has no page to tap) · the clock from MSF / DAB / RDS.

## Measure, both images, same radio
`vibe.debuglog` on each: per-process and per-thread CPU, cpufreq residency, battery drain over 30 min at a fixed
brightness. The native image replaces the browser one only when it does the basic radio at a fraction of the CPU.

## Seen elsewhere (read for inspiration only — SDR++ is GPL-3, nothing copied)
jasiek/r36s-sdrplusplus (2026-11-06): SDR++ as a PortMaster port on dArkOSRE (BSP 4.4 kernel, ARM's Mali blob),
drawn through Westonpack + GL4ES faking GL 2.1/3.0 over GLES, the left stick mapped to a MOUSE pointer (gptokeyb).
What it tells us:
- A native GL receiver UI on this SoC is workable — and our path is lighter (SDL2 KMSDRM + GLES2 straight to the
  panel: no Weston, no Xwayland, no GL translation layer).
- Its 640×480 problems (labels and the frequency readout clipping; UI scale only 100/200 %) are a desktop UI shrunk
  to fit. Ours is designed AT 640×480, pad-first, no pointer.
- Its default is an 8192-point FFT at 15 fps for CPU — the engine already does the FFT; our UI only draws rows.
- An RTL-SDR (~300 mA) needed a powered OTG hub there; the HF+ ran from the port on Stuart's unit.
- Candidate for menus: **Dear ImGui (MIT)** with its SDL2 + OpenGL ES 2 backends and built-in gamepad navigation —
  a permissive licence, so usable as a library (still: our own layout, glyphs and scheme).
