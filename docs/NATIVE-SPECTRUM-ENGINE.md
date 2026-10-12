# Native spectrum engine — working plan (2026-10-12)

**Why (Stuart):** "may as well build it, it's worth doing right. Especially given that our audience seems to be the
guys that modern tech left behind." The app's control side (layout, menus, skins) stays React Native — one codebase
for iPhone, iPad, Mac, Android, TV. The DATA side — spectrum frames at 20–30/s → waterfall rows, spectrum trace,
meters — moves into shared C++, off the JS thread, the way VibeServer was made to run on a Pi 2.

## The target (Stuart, 2026-10-12)
"The XCover I would say is a realistic low end target — if we can get that running at a reliable 60 fps then the
30 fps toggle then exists to help the slower and older devices." **Pass mark: XCover 4s, reliable 60 fps** on the
default skin main screen and the DAB list, including the seconds after connecting. The AUTO/60/30 frame-rate row
(next_build_after_rc39) is the safety net for anything older, not the plan for the XCover.

## Evidence (Galaxy XCover 4s, 2× A73 + 6× A53, Perfetto + Hermes sampling profiler)
- JS thread 50–70 % busy on the main/DAB screens. Profile: React render/commit ~35 %, young GC 8–12 %, Skia
  `dispose` ~9 % self (~1 ms a call — Hermes external-memory accounting, not the bytes), createSerializable ~7 %.
- After connecting to a DAB mux the JS thread hit 70–95 % with 100–210 ms GC bursts for ~5 s, and the waterfall's
  redraws went 37–82 per 0.5 s (erratic) against a steady 54–58 — Stuart: "renders smoothly then crawls for a bit".
  The UI thread held 60 fps throughout: the waterfall stalls because its ROWS come through JS.
- Already done (main, unpushed): logo decode (3764296a), rotation (3dcc3627), thread placement + SkiaRender
  render thread (359aec9e). Parked: ring as 8 strips (~/VibeSDR-backups/waterfall-ring-strips-2026-10-12.patch —
  no frame-rate change; ÷8 memory traffic).

## Today's per-frame JS work (map: see the survey summary in the commit that lands stage 1)
Decode (4 formats: SPEC u8/f32 full/delta for VibeServer + UberSDR + local USB via the localhost shim; OWRX
IMA-ADPCM or f32; Kiwi W/F ADPCM) → SDRScreen onSpectrum (meters, squelch, watch) → WaterfallView jitter buffer →
handleFrame: SignalProcessor (src/assets/signalProcessor.ts, 592 lines) → setLiveRange → pushRow (resample to 1024,
in-flight shift, ring write) → publishRing (1 MiB Data + Image) → repeat rows (≤ 8/frame) → spectrum points →
peak Path per frame.

## Stages
1. **Engine does the per-frame work.** JS still decodes, then ONE call hands the Float32 bins (zero-copy view) and
   the frame geometry to the engine. On its own thread the engine runs the SignalProcessor (ported, bit-for-bit
   against the TS on recorded frames), the resample + shift + ring, builds the waterfall SkImage, the spectrum
   points and the peak SkPath natively (react-native-skia's prefab C++ — no JS copy, no JS dispose), and the
   canvases pick them up per UI frame. JS gets only what the UI shows (dB labels, meter values) at a low rate.
   Works for every backend at once.
2. **Native transport for VibeServer + UberSDR** (and so local USB — the localhost round trip goes): frames never
   touch JS.
3. **OWRX + Kiwi decoders native** — only if stage 2 shows it is worth it.

## Rules
- Measured on the XCover at every stage: normal AND confined to four A53s (the CPU floor of a budget phone), same
  screens (default skin main; DAB list), hands-off, repeated runs (the phone throttles: ±15 points a run).
- Never more RAM than today (the store app runs on a 32-bit 1 GB phone and must keep doing so).
- iOS gets the same C++; it must stay exactly as smooth as it is.
- Look and feel unchanged — identical waterfall, trace, colours, scroll, glide.
