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

## Stage 1 design (decided 2026-10-12)
- **react-native-skia patch adds `vibe_rnskia` (cpp/rnskia/VibeExport.h)**: makeGray8Image / makePolyPath on ANY
  thread (raster, inside librnskia where Skia's symbols are visible — they are hidden from outside), and
  wrapImage / wrapPath on the UI runtime WITHOUT Hermes external memory pressure and with a cheap dispose
  (stock dispose's setExternalMemoryPressure measured ~1 ms a call). Platform context captured by RNSkManager.
- **Engine thread** (`SpectrumEngine`, C++): the jitter buffer, handleFrame, pushRow (resample, in-flight
  shift, ring), repeat rows, publishing the ring image, the spectrum target array and the peak path — every
  timer on its own clock, nothing on the JS thread. JS pushes each decoded frame (one call) and the view /
  settings / covered / background state; it gets the dB labels back only when they change.
- **One UI-thread poll** (a frame callback) asks the engine "anything new?", wraps the new image/path, writes
  the shared values the canvases already read, disposes the previous objects after a frame grace, and runs the
  glide and spectrum tween it already ran — no runOnJS.
- **Android first; iOS keeps the JS path** until the same C++ is built there. The JS path stays in the code
  as the fallback (engine missing or failing) and for iOS.

## WaterfallView per-frame behaviour — the reference the engine must match
(Survey of HEAD 7906a9fe, WaterfallView.tsx line numbers.) Quirks found: startRevealStepper is never called
(revealCb dead; extra rows come only from the setTimeout chain); uN is always 1; WF_MIN_GAP_MS = 0 so the gate
never fires; TARGET_FPS / feedFloorFps / binCount unused; manualRange is a NEW object per settings-effect run, so
in manual mode any dep change flushes (the engine compares values); extraTimer is never cleared on unmount /
background / cover (up to 8 stray rows and undisposed images after unmount — the engine fixes this);
proc.reset() and the ring are never reset.
- Jitter buffer: enqueue drops in background; interacting (< 1 s since lastInteractAt, NOT gated by smoothTune)
  bypasses — clear queue, prefill = true, handle synchronously. Else arrival EMA 0.8/0.2 (init 150), queue ≤ 3
  (drop oldest), drain via timer: prefill gate, handle one, re-arm max(40, min(1000, q>1 ? 0.6·arr : arr)).
  ★ V12 2026-10-29 (v12-wf-no-jb): on the fixed clock (the clock running, any mode but NATIVE) frames SKIP the buffer
  — handled on arrival, the clock's own presentation delay the only de-jitter — and the spectrum target waits until
  its row is on screen (presentation + traceLagMs; WaterfallView traceShow, engine traceQ_/runTrace).
  NATIVE skips the buffer too (one real row per frame on arrival; the UI's rowPace releases them, unchanged).
- handleFrame: skip bg / empty / width < 4; interacting = smoothTune && < 1 s; process(bins, centerHz (predicted),
  bwHz, interacting); liveRange = round(dbMin/dbMax) with 2 dB hysteresis; avgFrameMs EMA 0.8/0.2 unless boost
  (→ uAvgMs); lowFps = avg > 66; wfBoost = boost || (smoothTune && lowFps); avgWfGapMs EMA (0 < dt < 2000);
  rowCentre = trueCenterHz ?? centerHz; rowHzBin = bwHz / row.length; pushRow; lastRow (unresampled) + meta;
  if covered stop here; uQuant = wfBoost ? 0 : 1; dur = max(50, min(1000, avgWfGapMs || avgFrameMs)); row carry
  (targetRows 40/30/0 by wfScroll: carry += dur/(1000/target); extras = max(0, floor(carry) − 1); carry −=
  floor(carry); extras ≤ 8) → extras repeat rows at dur/(extras+1) spacing; wfBoost ? startGlide(dur) :
  (stopGlide; scrollFrac = 1).
- pushRow: resample to 1024 (max-pool a = (i·step)|0, b = min(srcN, ((i+1)·step)|0) || a+1; nearest when smaller);
  hzPerBin scaled by srcN/1024; slot = (frameCount % 1024)·1024; shift off = round((rowCentre − view.centerHz) /
  hzPerBin) only if hzPerBin matches view.bwHz/1024 within 1e-6 and |off| < 1024; vacated edge zero; frameCount++;
  covered → ringDirty, else publish (uHead = frameCount; texReady once).
- Spectrum: if specShow && specH > 4: m = ceil(width/2) points, next[k] = spec[floor(((2k)/width)·fn)] when
  fn > m, else the spec as is; tween every 33 ms: k = 1 − exp(−dt / max(40, 0.35·uAvgMs)); closed polygon
  (0,base) → (px, base − v·specH) every 2 px → (floor(w), base); sleep when maxΔ < 0.002.
- Peak: if peakHold && !boost: ceil(floor(w)/2) points (px = 2k, v = peak[floor((px/width)·sLen)]), OPEN poly;
  boost → resetPeakHold + empty path; spectrum hidden → empty paths.
