/**
 * WaterfallView — 120Hz ProMotion waterfall + spectrum, v1.5 visual parity.
 *
 * Layout (top → bottom), all in dp:
 *   ┌──────────────────────────────────────────┐
 *   │ BAND_H (20)  — band plan strip           │  coloured allocations + labels
 *   ├──────────────────────────────────────────┤
 *   │ TICK_H (22)  — frequency ticker          │  green glow "7.153M" labels
 *   ├──────────────────────────────────────────┤
 *   │ specH        — spectrum trace            │  LUT-gradient fill + peak hold
 *   ├──────────────────────────────────────────┤
 *   │ wfH          — waterfall                 │  Skia image ring buffer
 *   └──────────────────────────────────────────┘
 *   Acrylic sideband panels + LED needle span band-strip-bottom → screen bottom.
 *
 * Architecture:
 *   - SignalProcessor (M9PSY pipeline + UberSDR auto-range) maps raw dBFS bins
 *     → LUT indices; this component never touches dB maths directly.
 *   - Ring buffer stores LUT *indices* (1 byte/bin); the RGBA display buffer is
 *     persistent and updated incrementally (memmove + colourise ONE new row per
 *     frame). Palette switches recolourise the whole buffer from the index ring.
 *   - TWO stacked canvases (power): the bottom one holds only the waterfall
 *     texture and is the only thing Reanimated redraws at 120Hz ProMotion; the
 *     top one (spectrum/bands/needle) repaints at the 10Hz data rate.
 *   - Needle + sideband-edge glows are Gaussian blurs — the most expensive Skia
 *     primitive — so they are pre-rendered ONCE into offscreen image strips and
 *     composited as plain textures, never re-blurred per frame.
 *   - Reanimated useDerivedValue drives the scroll translate on the UI thread
 *     at full display rate (120Hz ProMotion) — zero JS work per scroll tick.
 *   - Text (band labels, ticker, dB axis) rendered as absolutely-positioned RN
 *     <Text> overlays — crisper than Skia text and uses the expo-font faces.
 *
 * Visuals ported 1:1 from vibeWaterfall.ts v1.5 (M9PSY / Stuey3D):
 *   - BAND_COLS, label sizing rules, bottom border rgba(255,200,80,0.25)
 *   - niceTick / fmtHz ticker with #00aa33 glow text, minGap 52px
 *   - dB axis: 5 stops, amber rgba(255,180,60,0.90), faint reference lines
 *   - Spectrum fill: colormap LUT sampled at 9 stops, indices 15→235
 *   - Peak hold line: VFO colour (matches user's needle selection)
 *   - Acrylic sidebands: 4-stop gradient 0.03→0.28 alpha in VFO colour
 *   - Needle: 3-layer LED glow (28/16/6 blur), needleScale = clamp(.25,1,pxPerHz×4000)
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { AppState, Image as RNImage, PixelRatio, StyleSheet, Text, View } from 'react-native';
import {
  Canvas,
  Fill,
  Skia,
  Image as SkiaImage,
  ImageShader,
  Path,
  Rect,
  LinearGradient,
  Shader,
  BlurStyle,
  vec,
  AlphaType,
  ColorType,
  type SkData,
  type SkImage,
  type SkPath,
} from '@shopify/react-native-skia';
import {
  useSharedValue,
  useDerivedValue,
  useFrameCallback,
  withTiming,
  cancelAnimation,
  Easing,
  runOnJS,
} from 'react-native-reanimated';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import { getColorLUT } from '../assets/colormapUtils';
import type { SDRStatus } from '../services/UberSDRClient';
import { SignalProcessor, type SignalProcessorSettings } from '../assets/signalProcessor';
import { watchProvider } from '../services/watchProvider';
import { BAND_PLAN, BAND_HEX, type Band } from '../constants/bandPlan';

// ── Layout constants (vibeWaterfall.ts v1.5) ──────────────────────────────────

const BAND_H   = 20;   // band plan strip height
const TICK_H   = 22;   // frequency ticker height

// Band type → colour. Indices match v1.5 BAND_COLS: ham=red, broadcast=blue,
// utility=green, cb=orange. (Screenshot reference: 40m Ham red, 41m B/C blue.)
// Derived from BAND_HEX (bandPlan.ts) so the phone's band plan and the WATCH's band
// label can never drift apart. Same values as before: ham #CF0000, broadcast #0900FF,
// utility #07BD00, cb #FF7700, all at 0.92.
// FIXED ring depth (history frames). ★ MUST stay fixed across resize/rotation — reallocating it on a
// size change wipes the ring, and the waterfall persisting through a rotation/resize with NO redraw is
// a hero feature (the Mac app resizes fluidly where other SDR apps blank and rebuild). The DISPLAY
// mapping (how many rows fill the view height, 1 row per point) is the separate `uVisibleRows` uniform,
// so orientation changes the mapping — never the ring. Depth ≥ any view's point-height so 1 row = 1
// point holds without the mapping exceeding the ring.
const RING_ROWS = 1024;
/** Fixed waterfall texture WIDTH. Incoming rows are resampled into it, so a
 *  server changing its bin count (UberSDR does this on every zoom) can never
 *  reallocate the ring, reset the frame counter or destroy history. 1024 matches
 *  what UberSDR sends natively and what VibeServer is now asked for. */
const RING_W = 1024;

const BAND_COLS: Record<string, string> = {
  ham:       hexRgba(BAND_HEX.ham, 0.92),
  broadcast: hexRgba(BAND_HEX.broadcast, 0.92),
  utility:   hexRgba(BAND_HEX.utility, 0.92),
  cb:        hexRgba(BAND_HEX.cb, 0.92),
};

// ── Helpers (ported verbatim from v1.5) ──────────────────────────────────────

function niceTick(approx: number): number {
  const pow  = Math.pow(10, Math.floor(Math.log10(approx)));
  const norm = approx / pow;
  const nice = norm < 1.5 ? 1 : norm < 3.5 ? 2 : norm < 7.5 ? 5 : 10;
  return nice * pow;
}

function fmtHz(hz: number): string {
  if (hz >= 1e9) return (hz / 1e9).toFixed(2) + 'G';
  if (hz >= 1e6) return (hz / 1e6).toFixed(3) + 'M';
  if (hz >= 1e3) return (hz / 1e3).toFixed(hz < 1e5 ? 1 : 0) + 'k';
  return hz.toFixed(0) + 'Hz';
}

function hexRgba(hex: string, a: number): string {
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  return `rgba(${r},${g},${b},${a})`;
}

/** 11m CB special-case (typed 'utility' in bandPlan.ts but coloured orange). */
function bandColor(b: Band): string {
  if (b.name.includes('CB')) return BAND_COLS.cb;
  return BAND_COLS[b.type] ?? BAND_COLS.utility;
}

// ── Props ─────────────────────────────────────────────────────────────────────

export interface WaterfallViewProps {
  /** Hot-path frame input — the parent assigns frames into this ref's handler
   *  from onSpectrum. Frames NEVER go through React state (a setState per
   *  10–20Hz frame re-rendered the whole screen tree ≈ a full core). */
  frameSink?:  React.MutableRefObject<((bins: Float32Array, status: SDRStatus) => void) | null>;
  /** @deprecated frames come via frameSink; prop kept for compatibility. */
  bins?:       Float32Array | null;
  binCount:    number;
  centerHz:    number;
  bwHz:        number;
  tuneHz:      number;
  /** Filter edges (Hz offsets from carrier; low negative, high positive). */
  filterLow?:  number;
  filterHigh?: number;
  /** Manual range — only used when wfCoarse='manual'. */
  dbMin?:      number;
  dbMax?:      number;
  wfCoarse?:   'auto' | 'manual';
  colormap?:   string;
  width:       number;
  height:      number;
  /** Bottom strip (px) where waterfall gestures are ignored — the gap below the
   *  control pill overlaps the home-indicator zone; tuning there fought the
   *  swipe-up-to-minimise gesture. Menus (separate Modal) are unaffected. */
  bottomGuard?: number;
  ituRegion?:  number;            // 1/2/3 — filters regional band plan entries
  fontFamily?: string;            // default Atkinson Hyperlegible (accessibility skin)
  onPanDelta?:  (dxPx: number) => void;
  onZoomDelta?: (dyPx: number) => void;
  onTapTune?:   (hz: number) => void;
  onPinchZoom?: (scale: number) => void;

  // Display settings (SignalProcessor + layout)
  specShow?:       boolean;
  specFrac?:       number;        // spectrum fraction of (height − BAND_H − TICK_H)
  autoContrast?:   number;        // 0–20, default 5 (10 is too dark)
  specSmoothing?:  number;        // 1–10 → smoothingFrames
  avgFrames?:      number;        // 0–0.9 averaging weight (0 = off), OWRX-style
  specFloor?:      number;        // ±20 dB
  specPeakScale?:  number;        // 10 = 1.0×
  peakHold?:       boolean;
  spatialSmooth?:  boolean;
  wfBrightness?:   number;
  wfContrast?:     number;
  wfSharpness?:    number;
  frameRate?:      '10fps' | '20fps' | '30fps';   // legacy; superseded by wfScroll below
  /** ★★★ SCROLL SPEED AS A FIXED FRAME-GENERATION MULTIPLIER — the whole point is that it is a
   *  CONSTANT. lines-per-frame maps ALL waterfall history, so every time it changes the picture
   *  rescales; deriving it from the live data rate (round(target/dataFps)) meant it changed
   *  whenever the measured rate crossed a rounding boundary, and the waterfall visibly compressed
   *  and expanded. That was true of the ORIGINAL code, not just of my attempts to improve it.
   *  A constant cannot change, so it cannot rescale — ever.
   *    SHARP    1 row per received frame. Nothing invented; every row a whole frame's integration.
   *    DEFAULT  2 — a little interpolation.
   *    SMOOTH   4 — continuous motion on a slow feed, at the cost of largely synthesised rows.
   *  ★ Scroll SPEED therefore follows the data rate (rows/s = multiplier x fps), which is what
   *    SHARP has always done and what nobody has ever objected to. */
  wfScroll?:       'sharp' | 'default' | 'smooth';
  /** ★★ THE SLOWEST FRAME RATE THIS BACKEND EVER DELIVERS — what the fixed multiplier is sized
   *  against. It is a property of the SERVER, not a measurement, so it is known at connect and
   *  never moves: UberSDR bottoms out at 3.3 fps (→ 6x for a 20 rows/s scroll), VibeServer at
   *  4-5 (→ 5x). Sizing off the floor rather than the live rate is what keeps uN constant, and a
   *  constant uN is the only thing that stops the waterfall rescaling (Stuart, 2026-08-03). */
  feedFloorFps?:   number;
  needleColor?:    string;        // VFO colour — needle, sidebands, peak hold
  /** Needle/glow brightness 1–10 (5 = original look) — bright palettes can
   *  swallow the needle whatever colour it is. */
  needleIntensity?: number;
  /** Frosted backing 0–10 (0 = off): smoked-glass band over the passband
   *  that dims the waterfall behind the needle — contrast on bright
   *  palettes even when their colours match the needle. */
  needleFrost?: number;
  /** Instance spectrum backdrop (/api/spectrum-bg-image) — sits behind the
   *  spectrum line graph only, like the web UI. */
  bgImageUrl?:  string | null;
  /** Backdrop opacity 0–1 (0 = hidden). */
  bgOpacity?:   number;
  /** Station-ID overlay: "CALLSIGN - NAME" + location, top-right of the
   *  spectrum (web drawStationIdOverlay parity). */
  stationId?:   { line1: string; line2?: string; color: string } | null;
  // Smooth tune (variable refresh): 120Hz interpolated scroll while the user
  // is interacting; once settled the waterfall steps rows discretely (data is
  // ~10Hz — the slide is pure interpolation) and the spectrum trace eases at
  // ~30fps, so ProMotion can drop the panel rate and save battery.
  smoothTune?:     boolean;
  lastInteractAt?: React.MutableRefObject<number>;

  // VFO-lock panning (BRIEF-vfo-lock-and-panning §5.6). All default off so
  // existing callers are byte-for-byte unchanged.
  /** Boundary walls — the inclusive Hz edges the view may pan across. */
  panLoHz?:   number;
  panHiHz?:   number;
  showWalls?: boolean;            // true only when unlocked
  /** Secondary RF-centre marker (local IQ only; fed in Phase 2). */
  centerMarkerHz?:    number;
  showCenterMarker?:  boolean;
  centerMarkerColor?: string;     // default desaturated cyan
}

// ── GPU waterfall shader (the v1 WebGL design, ported to SkSL) ───────────────
// The texture holds RAW normalised intensity (Gray_8 ring buffer); the shader
// does ring addressing (scroll = uniform, no memmove), sub-pixel slide
// (uShift from Reanimated — zero JS per display frame), unsharp, S-curve
// contrast and the palette LUT lookup. Palette/sharpness/contrast changes
// recolour the ENTIRE history live. CRITICAL: this renders inside the
// on-screen Canvas — never via offscreen SkSurface snapshots (different GPU
// context → draws black; learned 2026-06-11).
const WF_SKSL = `
uniform shader wf;
uniform shader lut;
uniform float uHeadF;    // ABSOLUTE frame counter (next write index)
uniform float uFrac;     // 0..1 progress through the current frame interval
uniform float uN;        // lines per data frame (1/2/3 = native/20fps/30fps)
uniform float uQuant;    // 1 = crisp whole-line steps (settled), 0 = continuous (boost)
uniform float uRows;         // ring rows (FIXED history depth — never changes on resize)
uniform float uVisibleRows;  // rows mapped across the view height (= pixel height ⇒ 1 row/point)
uniform float uTexW;     // bins
uniform float uDrawW;    // draw width (screen px)
uniform float uDrawH;    // draw height incl. overscan row (screen px)
uniform float uSharp;    // unsharp amount (0..~1.2)
uniform float uContrast; // -1..1 S-curve mix

// Temporal interpolation (phase 2): the ring holds RAW frames; intermediate
// lines are synthesized here by blending adjacent frames at fractional depth
// — identical maths to the old JS line ticker, per-pixel, for free.
// Line algebra: with frame K = uHeadF-1 newest, lines (K-1)*uN+1..K*uN blend
// frame[K-1]->frame[K]; revealed lines this interval R = uFrac*uN; absolute
// line at display row L is A = (uHeadF-2)*uN + R - L; its source frames are
// f = floor(A/uN) and f+1 blended by frac(A/uN).
half4 main(float2 xy) {
  float Lc = xy.y / uDrawH * uVisibleRows;     // display line (1 row per point) — decoupled from ring
  float L  = uQuant > 0.5 ? floor(Lc) : Lc;    // whole-pixel rows when settled
  float R  = uFrac * uN;
  if (uQuant > 0.5) { R = floor(R + 0.0001); }
  float A  = (uHeadF - 2.0) * uN + R - L;
  float fI = floor(A / uN);
  float t  = A / uN - fI;
  float r1 = mod(fI, uRows) + 0.5;
  float r2 = mod(fI + 1.0, uRows) + 0.5;
  float tx = clamp(xy.x / uDrawW * uTexW, 0.5, uTexW - 0.5);
  float c  = mix(wf.eval(float2(tx, r1)).r, wf.eval(float2(tx, r2)).r, t);
  if (uSharp > 0.0) {
    float xl = max(tx - 1.0, 0.5);
    float xr = min(tx + 1.0, uTexW - 0.5);
    float l = mix(wf.eval(float2(xl, r1)).r, wf.eval(float2(xl, r2)).r, t);
    float r = mix(wf.eval(float2(xr, r1)).r, wf.eval(float2(xr, r2)).r, t);
    c = c + uSharp * (c - (l + r) * 0.5);
  }
  float raw = clamp(c, 0.0, 1.0);
  float s   = raw * raw * (3.0 - 2.0 * raw);
  float v   = uContrast > 0.0
    ? mix(raw, s, uContrast)
    : mix(raw, raw * 0.5 + 0.25, -uContrast);
  return lut.eval(float2(clamp(v, 0.0, 1.0) * 255.0 + 0.5, 0.5));
}`;
const WF_EFFECT = Skia.RuntimeEffect.Make(WF_SKSL);

// ── Component ─────────────────────────────────────────────────────────────────

function WaterfallView({
  frameSink, binCount, centerHz, bwHz, tuneHz,
  filterLow = -3000, filterHigh = 3000,
  dbMin = -120, dbMax = -20, wfCoarse = 'auto',
  colormap = 'gqrx', width, height, bottomGuard = 0,
  ituRegion = 1, fontFamily = 'Atkinson Hyperlegible',
  onPanDelta, onZoomDelta, onTapTune, onPinchZoom,
  specShow = true, specFrac = 0.26,
  autoContrast = 5, specSmoothing = 5, avgFrames = 0, specFloor = 0, specPeakScale = 10,
  peakHold = true, spatialSmooth = true,
  wfBrightness = 0, wfContrast = 0, wfSharpness = 0,
  frameRate = '20fps', wfScroll = 'sharp', feedFloorFps = 3.3, needleColor = '#ff2020', needleIntensity = 5, needleFrost = 0,
  bgImageUrl = null, bgOpacity = 0, stationId = null,
  smoothTune = true, lastInteractAt,
  panLoHz, panHiHz, showWalls = false,
  centerMarkerHz, showCenterMarker = false, centerMarkerColor = '#36c5f0',
}: WaterfallViewProps) {

  // ── Vertical layout ─────────────────────────────────────────────────────────
  const tickTop  = BAND_H;
  const specTop  = tickTop + TICK_H;
  const below    = Math.max(0, height - specTop);
  const specH    = specShow ? Math.round(below * Math.max(0.05, Math.min(0.65, specFrac))) : 0;
  const wfTop    = specTop + specH;
  const wfH      = height - wfTop;
  // ★ 1 DATA ROW = 1 SCREEN POINT. The ring depth tracks the waterfall's pixel height. A FIXED depth
  // (was 256) stretched to fill a tall PORTRAIT waterfall made each row span ~2 points, so signals
  // blurred and scrolled ~2× too fast vs landscape — present in EVERY version, masked by a high frame
  // rate (Stuart 2026-07-24). Depth = height ⇒ every orientation renders 1-point rows: sharp, and the
  // same points/sec scroll regardless of orientation.
  const rows      = Math.min(1024, Math.max(64, Math.round(wfH)));   // cap the ring memory/copy cost
  const wfRenderH = wfH + Math.ceil(wfH / rows) + 2; // hide bottom-edge judder
  const rowH      = wfRenderH / rows;

  // Waterfall line rate. Settled rows are drawn as WHOLE-PIXEL pushes (no
  // subpixel translate — razor-sharp lines, and no fractional-pixel shimmer,
  // the suspected portrait judder source). NATIVE = one row per data frame
  // (~10 lines/s); 20fps/30fps emit 2/3 lines per data frame, temporally
  // interpolated prev→cur so traces stay continuous (20/30 lines/s — fills
  // faster). 60fps existed briefly but 6-way interpolation smeared each data
  // line across six rows — unusably blurry in portrait; don't bring it back.
  // The smooth-tune boost overrides all of this with a vsync slide.
  // TARGET scroll rate. The interpolation multiplier is now derived per-frame from this and the LIVE
  // data rate (see handleFrame) so the chosen fps is a MINIMUM: at 5fps data '10fps' synthesises ×2 to
  // hold a 10fps scroll; at Kiwi's ~23fps it's already above target so no interpolation happens. The
  // static value here is just the nominal-at-10fps used for the interp-blur amount.
  // ★★★ FIXED FRAME GENERATION, SIZED FOR THE WORST CASE — then the excess is DISCARDED.
  //     This is the games model and it settles the argument that has run all day:
  //       • rows/sec = uN x frames/sec, and uN maps ALL waterfall history, so ANY change to it
  //         rescales the picture. That is the compressing and expanding, and it was in the
  //         original code too.
  //       • So uN must be a CONSTANT. Sized for the slowest feed we care about — UberSDR's
  //         3.3 fps — a 20 rows/sec scroll needs 6x. That constant is used on every feed.
  //       • Holding rows/sec constant with uN fixed therefore means using FEWER SOURCE FRAMES on
  //         a faster feed: exactly Stuart's "discard the excess". A 20 fps server contributes a
  //         waterfall row-set every 300 ms; the frames in between are skipped FOR THE WATERFALL.
  //     ★ The SPECTRUM TRACE is not gated and still follows every frame — the trade is time
  //       resolution in the waterfall, which is the thing being bought, not liveness elsewhere.
  const WF_TARGET_ROWS = wfScroll === 'smooth' ? 30 : wfScroll === 'default' ? 20 : 0;
  // Sized off the BACKEND'S FLOOR, so it is a constant for the session: UberSDR 20/3.3 -> 6x,
  // VibeServer 20/4 -> 5x. Never recomputed from what is arriving right now.
  const WF_ROWS = WF_TARGET_ROWS <= 0 ? 1
    : Math.max(1, Math.min(8, Math.round(WF_TARGET_ROWS / Math.max(1, feedFloorFps))));
  /** ms between waterfall row-sets to hold the target: uN rows every (uN/target) seconds. */
  const WF_MIN_GAP_MS = WF_TARGET_ROWS > 0 ? (WF_ROWS / WF_TARGET_ROWS) * 1000 : 0;
  const TARGET_FPS = frameRate === '30fps' ? 30 : frameRate === '20fps' ? 20 : 10;

  // Smooth tune: gestures count as "interacting" for this long after the last
  // touch; inside it the slide is boosted to native rate, outside it drops to
  // the selected fps and the spectrum tween smooths the trace at ~30fps.
  const SMOOTH_TUNE_TAIL_MS = 1000;
  // Below this data rate (frame interval > ~66 ms ⇒ < ~15 fps) the discrete reveal stepper's few
  // whole-line steps per frame visibly stutter, so glide CONTINUOUSLY like the watch does — even at
  // rest, not just during interaction. Pure interpolation of the scroll, so it adds NO latency; the
  // only cost is rendering between data frames. Fast rates keep the cheaper discrete stepping.
  const LOW_FPS_GLIDE_MS = 66;

  // Pan sensitivity: how far the view travels per finger-pixel. 1.0 = 1:1.
  // Bumped slightly so the drag feels lighter (the 1:1 version felt heavy/treacly).
  const PAN_GAIN = 1.5;
  const SPEC_TWEEN_MS       = 33;

  // ── Signal processor (owns all dB→index maths) ──────────────────────────────
  /** Scratch for the resample — reused, never per-frame allocated. */
  const rowBuf = useRef(new Uint8Array(RING_W));
  const proc = useRef(new SignalProcessor());
  useEffect(() => {
    // v1 webgl parity: interpolation blur grows with the display rate, so the
    // unsharp base scales with the selected fps and the slider is a multiplier
    // of it (5 = 1×; 60fps base 5 keeps existing setups looking identical).
    const sharpBase =
      // Interpolation blur scales with how many rows are synthesised, so the unsharp base follows
      // the multiplier rather than a frame rate that no longer drives anything.
      wfScroll === 'smooth' ? 3 : wfScroll === 'default' ? 2 : 1.5;
    // ★★ LINEAR ACROSS THE WHOLE SLIDER, with a ceiling about double the old
    // maximum. The curve used to be quadratic, which made the BOTTOM half do
    // nothing — slider 2 was (2/5)^2 = 0.16x base, imperceptible — so a 10-point
    // control effectively had about five useful points (Stuart, 2026-07-26).
    // Linear means every step moves something.
    //
    // ★ It reads as more effective now for a second reason: unsharp corrects
    // INTERPOLATION BLUR, so it only bites where bins are stretched across
    // pixels. At 4096 bins on a phone the row was being downsampled and there
    // was nothing to sharpen; at 1024 there finally is.
    const sharpMul = wfSharpness / 5;
    // GPU-side row effects (unsharp + S-curve) — uniforms, applied LIVE to
    // the whole waterfall history on the next draw.
    // Ceiling 2.0: unsharp amplifies NOISE along with signal and rings either
    // side of a carrier, so this is where "sharp" stops and "artefacts" start.
    uSharpSv.value    = Math.min(2.0, (wfSharpness / 10) * 1.5 * (sharpBase / 1.5));
    uContrastSv.value = Math.max(-1, Math.min(1, wfContrast / 10));
    const patch: Partial<SignalProcessorSettings> = {
      autoContrast,
      manualRange: wfCoarse === 'manual' ? { minDb: dbMin, maxDb: dbMax } : null,
      specFloor, specPeakScale,
      smoothingFrames: specSmoothing,
      avgFrames,
      spatialSmooth, peakHold,
      wfBrightness, wfContrast,
      wfSharpness: Math.min(12, sharpBase * sharpMul * 2),
    };
    proc.current.applySettings(patch);
  }, [autoContrast, wfCoarse, dbMin, dbMax, specFloor, specPeakScale,
      specSmoothing, avgFrames, spatialSmooth, peakHold, wfBrightness, wfContrast,
      wfSharpness, frameRate, wfScroll, feedFloorFps]);

  // ── Colormap LUT + derived spectrum colours (9 stops, idx 15→235) ───────────
  const lut = useMemo(() => getColorLUT(colormap), [colormap]);
  const specGradColors = useMemo(() => {
    // Sample LUT 90→235 (was 15→235): black-based palettes (Sonar etc.) are
    // near-invisible below ~idx 90, so the fill's baseline starts where the
    // palette has actually picked up colour — weak signals stay visible while
    // the trace still inherits the waterfall hue.
    const stops: string[] = [];
    for (let gi = 0; gi <= 8; gi++) {
      const idx = Math.max(0, Math.min(255, Math.round(90 + (gi / 8) * 145)));
      stops.push(`rgba(${lut[idx * 4]},${lut[idx * 4 + 1]},${lut[idx * 4 + 2]},1)`);
    }
    return stops.reverse(); // gradient runs top→bottom; hot colour at top
  }, [lut]);

  // ── Intensity ring buffer (Gray_8, ring order — the shader does the
  // display-order mapping via uHead). Each push = one row write + a 256KB
  // single-channel image (vs the old 1MB RGBA full rebuild + CPU colourise).
  const idxBuf       = useRef<Uint8Array | null>(null); // normalised intensity bytes
  const [texReady, setTexReady] = useState(false);
  const texReadyRef  = useRef(false);

  // Shader uniforms driven from the UI thread (no React render per change)
  const uHead       = useSharedValue(0); // ABSOLUTE frame counter (next write)
  const uTexW       = useSharedValue(1024);
  // The live view, readable from pushRow (a stable callback, not re-created per render).
  const viewRef = useRef({ centerHz: 0, bwHz: 0 });
  const uSharpSv    = useSharedValue(0);
  const uContrastSv = useSharedValue(0);
  // ★ Seeded from the CONFIGURED display rate, not a magic 2. A hardcoded
  // initial value guaranteed one remap shortly after connect — at the native
  // rate with ~18 fps data the first settled tick computes 1, so uN went 2→1 and
  // the (still filling) waterfall rescaled once. Barely visible because the ring
  // is nearly empty then, but it was real (Stuart, 2026-07-26).
  const uNSv        = useSharedValue(1); // lines per data frame
  /** The last SETTLED lines-per-frame, held across a gesture so uN never changes
   *  mid-interaction — a change rescales the whole waterfall vertically. */
  const lastDynRows = useRef(1);
  /** ★★★ THE MULTIPLIER IS LATCHED ONCE PER SESSION — this is the whole fix.
   *
   *  Constant SPEED needs the multiplier to follow the feed (6x on a 3.3 fps UberSDR, 1x on a
   *  20 fps VibeServer). Constant PICTURE needs it never to change, because lines-per-frame maps
   *  ALL waterfall history and any change rescales it vertically — that is the compressing and
   *  expanding, and it was in the original code too, not just my attempts at it.
   *  Both are satisfiable, because the conflict is about WHEN it is chosen, not what it is: settle
   *  on a value once the feed's rate is known, then HOLD it for the session. Constant while you
   *  are watching; correct for the server you are watching.
   *  ★ Re-latched only on a real event — a new session, or the user picking a different preset —
   *    never on measurement drift, which is what every previous attempt got wrong. */
  /** When the waterfall last took a frame — see the discard gate in handleFrame. */
  const lastWfPushAt = useRef(0);
  const uQuantSv    = useSharedValue(1); // 1 = crisp steps, 0 = boost glide
  const frameCount  = useRef(0);

  // Palette = a 256×1 LUT texture; switching recolours ALL history instantly.
  const lutImage = useMemo(() => {
    const data = Skia.Data.fromBytes(lut);
    return Skia.Image.MakeImage(
      { width: 256, height: 1, colorType: ColorType.RGBA_8888, alphaType: AlphaType.Opaque },
      data, 256 * 4,
    );
  }, [lut]);

  // ── Display state ───────────────────────────────────────────────────────────
  // Image + paths are Reanimated shared values, NOT React state: Skia nodes
  // accept them directly and update on the UI thread. The 30-lines/s
  // interpolation pushes and the 30fps spectrum tween would otherwise each
  // trigger a full React re-render per tick — that was the device-heat
  // regression. React renders stay at the ~10Hz data rate (driven by parent
  // props); empty path = draw nothing (Path doesn't take null).
  const wfImage  = useSharedValue<SkImage | null>(null);
  const specPath = useSharedValue<SkPath>(Skia.Path.Make());
  const peakPath = useSharedValue<SkPath>(Skia.Path.Make());
  const [liveRange, setLiveRange] = useState({ dbMin: -120, dbMax: -20 });
  // ── Deterministic Skia disposal ─────────────────────────────────────────────
  // Hermes only sees the tiny JS wrappers, NOT the ~1MB native buffer behind
  // each waterfall image — it feels no memory pressure and lets dead images
  // accumulate for ages (measured ~770MB resident, plus constant GC churn).
  // Retire queues delay dispose() by a couple of swaps so the UI thread can
  // never be mid-draw on a freed object.
  const wfLive    = useRef<{ img: SkImage; data: SkData } | null>(null);
  const wfPending = useRef<Set<{ img: SkImage; data: SkData }>>(new Set());
  const swapWfImage = useCallback((img: SkImage, data: SkData) => {
    wfImage.value = img;
    const old = wfLive.current;
    wfLive.current = { img, data };
    // TIME-based disposal (was count-based "keep 2"). The UI-thread Skia render
    // may still be drawing the previous image; a fast producer (OWRX FFT/zoom is
    // much faster than UberSDR) can swap past a count window before the GPU
    // renders, so dispose() freed an image still referenced → JSI "disposed"
    // throw on the render thread = hard crash. A 300ms grace is far longer than
    // any render interval, so the freed image is always off-screen first.
    if (old) {
      wfPending.current.add(old);
      setTimeout(() => {
        if (wfPending.current.delete(old)) { try { old.img.dispose(); old.data.dispose(); } catch {} }
      }, 300);
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Paths share the same hazard (the trace/peak redraw every tween + zoom frame),
  // so dispose them on the same time grace rather than a count window.
  const pathPending = useRef<Set<SkPath>>(new Set());
  const swapPath = useCallback((sv: { value: SkPath }, p: SkPath) => {
    const old = sv.value;
    sv.value = p;
    if (old) {
      pathPending.current.add(old);
      setTimeout(() => { if (pathPending.current.delete(old)) { try { old.dispose(); } catch {} } }, 300);
    }
  }, []);

  useEffect(() => () => { // unmount: flush the pending-dispose sets + the live image
    wfPending.current.forEach(r => { try { r.img.dispose(); r.data.dispose(); } catch {} });
    wfPending.current.clear();
    if (wfLive.current) { try { wfLive.current.img.dispose(); wfLive.current.data.dispose(); } catch {} wfLive.current = null; }
    pathPending.current.forEach(p => { try { p.dispose(); } catch {} });
    pathPending.current.clear();
  }, []);

  // ── Smooth scroll (UI thread) — fed to the shader as a sub-pixel sample
  // offset (uShift); no view transform, no gap at the slide edge.
  const scrollFrac  = useSharedValue(1);
  const lastFrameTs = useRef(0);
  const avgFrameMs  = useRef(150);

  const wfUniforms = useDerivedValue(() => ({
    uHeadF:    uHead.value,
    uFrac:     scrollFrac.value,
    uN:        uNSv.value,
    uQuant:    uQuantSv.value,
    uRows:        RING_ROWS,   // fixed ring depth (never changes on resize → no waterfall redraw)
    uVisibleRows: rows,        // = point-height ⇒ 1 row per point (sharp, same scroll both orientations)
    uTexW:     uTexW.value,
    uDrawW:    width,
    uDrawH:    wfRenderH,
    uSharp:    uSharpSv.value,
    uContrast: uContrastSv.value,
  }), [width, wfRenderH, rows]);

  // ── Spectrum tween (smooth tune, settled state) ────────────────────────────
  // Data frames arrive at ~10Hz (or ~3Hz under the idle divisor); setting the
  // path per frame would make the trace a slideshow. Instead the displayed
  // trace eases toward the latest frame on 33ms ticks and the timer STOPS once
  // converged, so between frames the display idles.
  // ★★★ THE TWEEN RUNS ON THE UI THREAD. It was a `setInterval`, which runs on the JS thread —
  // so the trace's smoothness was hostage to whatever else JavaScript was doing, and any big panel
  // re-rendering jerked it. Stuart's measurement is what identified it: with a panel open "it is
  // only the trace, waterfall is perfect" (2026-08-02). The waterfall is a shader whose uniforms
  // are driven from the UI thread and it never stuttered; this was the one piece of the display
  // still depending on JS being free 30 times a second.
  // ★ It was NOT that the drawing was on the CPU — Skia rasterises the trace on the GPU either
  // way. What sat on the JS thread was building the geometry: interpolating every bin and
  // constructing the SkPath. That is what moves here.
  // ★★ These are shared values, not refs: a worklet cannot reach a React ref.
  const specToSv   = useSharedValue<number[]>([]);
  const specDispSv = useSharedValue<number[]>([]);
  // Layout the worklet needs, mirrored where it can read them.
  const uSpecW    = useSharedValue(0);   // float width (the sampling ratio uses it)
  const uSpecTop  = useSharedValue(0);   // baseline = wfTop
  const uSpecHt   = useSharedValue(0);   // trace height
  const uAvgMs    = useSharedValue(150); // measured data-frame interval
  /** ★★ POINT POOL, mutated in place — the same reasoning as the JS version it replaces:
   *  allocating fresh {x,y}s per build fed the GC ~12k objects/s. It just lives on the UI
   *  thread now. */
  const specPoolSv = useSharedValue<{ x: number; y: number }[]>([]);

  // addPoly = ONE JSI call for the whole polyline, and the points come from a POOL of
  // mutated-in-place objects: allocating fresh {x,y}s per build (×30Hz) fed the Hermes GC ~12k
  // objects/s — profiling showed HadesGC at ~18% of all CPU samples. Both still hold; the spectrum
  // pool simply lives on the UI thread now (specPoolSv). Trace sampled every 2px — the data is
  // smoothed, so the look is identical.
  // ★ THE PEAK TRACE STAYS ON JS DELIBERATELY. It is rebuilt once per DATA frame, not per display
  //   frame, so it was never part of the smoothness problem — and it is only drawn at all when
  //   peak hold is on. Moving it too would be churn for no measured gain.
  const SPEC_PX_STEP = 2;
  const peakPtsPool = useRef<{ x: number; y: number }[]>([]);


  // Keep the worklet's view of the layout current.
  useEffect(() => {
    uSpecW.value = width; uSpecTop.value = wfTop; uSpecHt.value = specH;
  }, [width, wfTop, specH, uSpecW, uSpecTop, uSpecHt]);

  // ★★★ TWO PERSISTENT PATHS, ALTERNATED — NOT ONE ALLOCATED PER TICK.
  // An SkPath holds native memory Hermes cannot see, which is why the JS path swap disposes on a
  // 300 ms grace (see swapPath). A worklet has no setTimeout and cannot schedule that, so
  // allocating a fresh path 60 times a second on the UI thread would leak exactly the way this
  // file already warns about. Instead: two paths made once, reset() and rebuilt in place, and
  // handed to the shared value alternately. Nothing is allocated, nothing needs disposing, and
  // the object IDENTITY still changes every tick, which is what makes Skia repaint.
  const specPathA = useMemo(() => Skia.Path.Make(), []);
  const specPathB = useMemo(() => Skia.Path.Make(), []);
  const specFlip  = useSharedValue(false);
  /** ★★★ SET ON UNMOUNT, CHECKED BY BOTH WORKLETS FIRST. A frame callback runs on the UI thread
   *  and does NOT stop just because React tore the component down: one more tick can land after
   *  the cleanup has run, and the first thing the tween does is reset() and rebuild one of the two
   *  paths. If those had already been disposed that is a use-after-free on a native object — from
   *  the UI thread, with the JS side already gone. */
  const specDead = useSharedValue(false);
  useEffect(() => () => {
    // ★ ORDER MATTERS AND SO DOES THE DELAY.
    //   1. tell the worklets to do nothing more — they check this before touching anything;
    //   2. deactivate the callbacks so they stop being scheduled at all;
    //   3. dispose LATER. A frame may already be executing on the UI thread at this instant, and
    //      freeing the paths underneath it is precisely the race being closed. 300 ms is the same
    //      grace swapPath() has always used for exactly this reason — see it above.
    specDead.value = true;
    try { specTweenRef.current?.setActive(false); } catch {}
    try { revealRef.current?.setActive(false); } catch {}
    const a = specPathA, b = specPathB;
    setTimeout(() => { try { a.dispose(); b.dispose(); } catch {} }, 300);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [specPathA, specPathB]);

  /** Accumulated display time since the last rebuild — see the cadence note in the callback. */
  const specAccSv = useSharedValue(0);

  /** ★ Declared BEFORE the worklet on purpose. A Reanimated worklet captures its closure when it
   *  is CREATED, so a `const` defined further down would be captured in the temporal dead zone.
   *  The handle itself only exists after useFrameCallback returns, hence the ref. */
  const specTweenRef = useRef<{ setActive: (b: boolean) => void; isActive: boolean } | null>(null);
  const setSpecTweenActive = useCallback((on: boolean) => {
    const t = specTweenRef.current;
    if (t && t.isActive !== on) t.setActive(on);   // checked so a 20/s data frame cannot thrash it
  }, []);
  const stopSpecTween = useCallback(() => { setSpecTweenActive(false); }, [setSpecTweenActive]);

  const specTween = useFrameCallback((fi) => {
    'worklet';
    if (specDead.value) return;   // component is gone — see specDead
    // No busy flag needed: the callback is DEACTIVATED when settled (see below), so it is not
    // running at all rather than running and returning.
    // ★★★ HOLD THE OLD 33 ms CADENCE, do not run at the display rate. useFrameCallback fires on
    // EVERY display frame — 120 of them a second on ProMotion, against the 30 the interval gave.
    // Rebuilding the path four times as often would swap a JS-thread problem for a UI-thread one,
    // which is the version of this bug that would be much harder to see coming. The interpolation
    // is time-based, so ticking at the same rate reproduces the previous motion exactly.
    specAccSv.value += fi.timeSincePreviousFrame ?? SPEC_TWEEN_MS;
    if (specAccSv.value < SPEC_TWEEN_MS) return;
    const dtAcc = specAccSv.value;
    specAccSv.value = 0;
    const to = specToSv.value, disp = specDispSv.value;
    const n = to.length;
    if (!n || disp.length !== n) return;
    const w = uSpecW.value, baseline = uSpecTop.value, sh = uSpecHt.value;
    if (w <= 0 || sh <= 0) return;

    // Same control law as the interval version, but against the REAL elapsed time rather than a
    // nominal 33 ms: a dropped frame then eases by the right amount instead of falling behind,
    // which a fixed interval could not know about.
    const k = 1 - Math.exp(-dtAcc / Math.max(40, uAvgMs.value * 0.35));
    let maxDelta = 0;
    for (let i = 0; i < n; i++) {
      const d = to[i] - disp[i];
      disp[i] += d * k;
      const a = d < 0 ? -d : d;
      if (a > maxDelta) maxDelta = a;
    }
    // ★★ WRITE IT BACK. Whether `.value` hands out the stored array or a copy of it is an
    // implementation detail of the shared-value plumbing, and the failure mode if it is a copy is
    // silent: the interpolation would be thrown away every tick and the trace would never settle.
    // Reassigning is correct under either behaviour, and this runs on the UI thread where the
    // whole point is that spending a little is free.
    specDispSv.value = disp;

    const wi = Math.floor(w);
    const count = Math.ceil(wi / SPEC_PX_STEP) + 2;
    let pool = specPoolSv.value;
    if (pool.length !== count) {
      const fresh: { x: number; y: number }[] = [];
      for (let i = 0; i < count; i++) fresh.push({ x: 0, y: 0 });
      specPoolSv.value = fresh; pool = fresh;
    }
    let j = 0;
    pool[j].x = 0; pool[j].y = baseline; j++;
    for (let px = 0; px < wi; px += SPEC_PX_STEP) {
      const v = disp[Math.floor((px / w) * n)];
      pool[j].x = px; pool[j].y = baseline - v * sh; j++;
    }
    pool[j].x = wi; pool[j].y = baseline;

    const path = specFlip.value ? specPathA : specPathB;
    path.reset();
    path.addPoly(pool, true);
    specFlip.value = !specFlip.value;
    specPath.value = path;

    // ★★★ CONVERGED — STOP THE CALLBACK, do not merely return early from it.
    // An ACTIVE useFrameCallback drives a CADisplayLink every display frame, and that PINS a
    // ProMotion panel's refresh rate: the display can no longer drop to its low idle rate, which
    // costs battery for nothing while the trace is settled and unchanging. The interval version
    // got this right by stopping itself, and losing it would have been an invisible regression —
    // Stuart: "if we can maintain the VRR on promotion screens that is a big win for battery
    // life." So the whole callback goes inactive and the display is free to idle down again.
    // ★ runOnJS is fine here: it fires ONCE per convergence, not per frame.
    if (maxDelta < 0.002) { specAccSv.value = 0; runOnJS(setSpecTweenActive)(false); }
  }, false);   // autostart OFF — started on the first data frame
  specTweenRef.current = specTween;

  // pushRow reads the view from a ref, so keep it current.
  useEffect(() => { viewRef.current = { centerHz, bwHz }; }, [centerHz, bwHz]);

  // ── Row push + line ticker (whole-pixel waterfall advance) ─────────────────
  // pushRow advances the waterfall by exactly one pixel row: ring write,
  // incremental display shift, colourise, new SkImage. The line ticker emits
  // duplicate pushes of the latest data row between frames so the fps modes
  // reach 30/60 lines per second.
  const pushRow = useCallback((srcRow: Uint8Array, rowCenterHz = 0, rowHzPerBin = 0) => {
    // ★★ THE RING IS A FIXED SIZE IN BOTH DIMENSIONS. It used to be reallocated
    // whenever the incoming bin count changed, which also reset frameCount to 0 —
    // and the shader indexes from that ABSOLUTE counter, so the reset made it
    // read rows that had just been zeroed. The waterfall visibly squashed and
    // then popped open again as frames refilled (Stuart, 2026-07-26: "vertical
    // hitches... the whole waterfall gets squished then pops open").
    //
    // ★ A bin-count change is NOT an error to be survived — it is NORMAL: UberSDR
    // sends more bins as you zoom in (finer bins = more data/frame), and Kiwi
    // varies too. So the renderer must absorb it, not restart on it.
    //
    // ★ This is what the WEB client already does — its ring is never reallocated
    // and the GPU scales bins→width for free, which is why dragging a browser
    // window preserves history. Same idea here: resample into a FIXED width.
    const srcN = srcRow.length;
    let row = srcRow, n = srcN;
    if (srcN !== RING_W) {
      const dst = rowBuf.current;
      if (srcN > RING_W) {
        // PEAK-HOLD when downsampling — a mean would swallow narrow carriers,
        // which are exactly what a waterfall exists to show.
        const step = srcN / RING_W;
        for (let i = 0; i < RING_W; i++) {
          const a = (i * step) | 0, b = Math.min(srcN, ((i + 1) * step) | 0) || a + 1;
          let m = 0;
          for (let k = a; k < b; k++) if (srcRow[k] > m) m = srcRow[k];
          dst[i] = m;
        }
      } else {
        const step = srcN / RING_W;                      // < 1: nearest-neighbour
        for (let i = 0; i < RING_W; i++) dst[i] = srcRow[(i * step) | 0];
      }
      row = dst; n = RING_W;
    }
    // Allocated ONCE. No bin count can reallocate it, so history always survives.
    if (!idxBuf.current) {
      idxBuf.current = new Uint8Array(RING_W * RING_ROWS);
      uTexW.value = RING_W;
    }
    // The resample changes how much spectrum each stored bin covers, so the
    // alignment maths below must use the RESAMPLED scale, not the wire's.
    if (srcN !== RING_W && rowHzPerBin > 0) rowHzPerBin = rowHzPerBin * srcN / RING_W;
    // ── In-flight frame alignment (BRIEF-waterfall-frame-alignment) ──────────
    // The renderer places rows against the CURRENT view, so a frame still in
    // flight when the view moves gets drawn as though the predicted centre had
    // already come true — leaving the signal beside the VFO until a zoom flushed
    // the disagreement. Shift the row by the gap between its OWN centre and the
    // view's, so it lands where it belongs.
    //
    // ★ Deliberately done at WRITE time, in the existing view-relative ring, and
    // NOT by storing rows at absolute frequencies. Absolute placement was built
    // and reverted (see the brief §9): it invalidates the ring whenever the bin
    // scale changes, which restarted the whole waterfall on every zoom — a visual
    // feature that is not negotiable — and merely clearing the ring per scale
    // change was already enough to make the zoom drum feel sticky. This costs one
    // offset and touches nothing on zoom.
    //
    // ★ Accepted limitation: history cannot be retro-corrected, so panning while
    // data flows leaves a slight kink where the alignment changed rather than
    // today's smooth-but-wrong re-labelling. In-flight offsets are a few bins, so
    // the vacated edge strip is invisible.
    const slot = (frameCount.current % RING_ROWS) * n;
    const v = viewRef.current;
    let off = 0;
    if (rowHzPerBin > 0 && rowCenterHz > 0 && v.centerHz > 0 && v.bwHz > 0) {
      // ★ Only a frame at the SAME bin scale may be shifted. Shifting one from a
      // different span silently draws the right signal at the wrong width, so a
      // mismatch is drawn unshifted (as it is today) rather than moved.
      const viewHzPerBin = v.bwHz / n;
      if (Math.abs(rowHzPerBin - viewHzPerBin) <= viewHzPerBin * 1e-6) {
        off = Math.round((rowCenterHz - v.centerHz) / rowHzPerBin);
        if (Math.abs(off) >= n) off = 0;   // no overlap at all — nothing to place
      }
    }
    if (off === 0) {
      idxBuf.current.set(row, slot);
    } else {
      idxBuf.current.fill(0, slot, slot + n);           // vacated edge = floor
      if (off > 0) idxBuf.current.set(row.subarray(0, n - off), slot + off);
      else         idxBuf.current.set(row.subarray(-off), slot);
    }
    frameCount.current += 1;

    const data = Skia.Data.fromBytes(idxBuf.current);
    const img = Skia.Image.MakeImage(
      { width: n, height: RING_ROWS, colorType: ColorType.Gray_8, alphaType: AlphaType.Opaque },
      data,
      n,
    );
    if (img) {
      swapWfImage(img, data); // UI-thread swap + retire old pair (~256KB now)
      uHead.value = frameCount.current;
      if (!texReadyRef.current) { texReadyRef.current = true; setTexReady(true); }
    } else {
      data.dispose();
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [swapWfImage]);

  // (pushLine and the per-line slide/snap switching are gone — phase 2 moves
  // line synthesis into the shader; pushRow runs once per data frame and the
  // reveal stepper / boost withTiming drive uFrac.)

  // ── Reveal stepper (phase 2) ─────────────────────────────────────────────
  // The shader synthesizes intermediate lines from adjacent RAW frames; JS
  // only advances the reveal fraction. Settled = discrete whole-line steps
  // (one shared-value write per line, display idles between); boost = vsync
  // withTiming for the continuous glide. All the old lerp buffers, row pools
  // and per-line texture pushes are GONE.
  // ★★ THE SAME MOVE AS THE SPECTRUM TWEEN: this advanced `scrollFrac` from a JS `setInterval`,
  // so the waterfall's reveal was as hostage to a busy JS thread as the trace was. It never
  // SHOWED the fault the way the trace did, because it writes DISCRETE whole-line steps — a late
  // step lands a line early or late, where a late interpolation tick is visibly uneven motion.
  // Latent rather than harmless, so it moves for the same reason.
  // ★ Same two properties as the tween, and for the same reasons: it holds its own step cadence
  // rather than running at the display rate, and it DEACTIVATES when the reveal completes so an
  // idle waterfall never pins a ProMotion panel's refresh rate.
  const revN     = useSharedValue(0);
  const revStep  = useSharedValue(16);   // ms per line
  const revK     = useSharedValue(0);
  const revAcc   = useSharedValue(0);
  const revealRef = useRef<{ setActive: (b: boolean) => void; isActive: boolean } | null>(null);
  const setRevealActive = useCallback((on: boolean) => {
    const t = revealRef.current;
    if (t && t.isActive !== on) t.setActive(on);
  }, []);
  const stopRevealStepper = useCallback(() => { setRevealActive(false); }, [setRevealActive]);

  const revealCb = useFrameCallback((fi) => {
    'worklet';
    if (specDead.value) return;   // component is gone — see specDead
    revAcc.value += fi.timeSincePreviousFrame ?? 16;
    if (revAcc.value < revStep.value) return;
    revAcc.value = 0;
    const n = revN.value;
    if (n <= 0) { runOnJS(setRevealActive)(false); return; }
    revK.value += 1;
    scrollFrac.value = Math.min(1, revK.value / n);
    if (revK.value >= n) runOnJS(setRevealActive)(false);
  }, false);
  revealRef.current = revealCb;

  const startRevealStepper = useCallback((n: number, intervalMs: number) => {
    scrollFrac.value = 0;
    if (n <= 1) { scrollFrac.value = 1; setRevealActive(false); return; } // one whole-line step
    revN.value = n;
    revStep.value = Math.max(16, intervalMs / n);
    revK.value = 0;
    revAcc.value = 0;
    setRevealActive(true);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [setRevealActive]);

  useEffect(() => stopRevealStepper, [stopRevealStepper]); // stop on unmount

  // ── Background gate ────────────────────────────────────────────────────────
  // On background/inactive: cancel any in-flight scroll glide and kill the
  // reveal/tween steppers so the worklets + Skia redraw runtime goes fully idle
  // (see handleFrame). handleFrame early-returns while bgRef is set, so no new
  // frame can restart them. Resume is automatic — the next frame drives them.
  // `active` gates the Skia canvases OUT of the tree while backgrounded (see the
  // conditional render below). Under New Arch, a mounted <Canvas> keeps the Skia
  // render/present loop and per-frame Fabric commits alive even with static
  // uniforms — with no GL context in the background that spins the native_modules
  // queue ("EGLConsumer is not attached to an OpenGL ES context" spam) and starves
  // the audio DSP. Unmounting is the only reliable stop; it rebuilds on resume.
  // INITIALISED FROM AppState.currentState, not assumed foreground.
  //
  // A `change` event only fires on a TRANSITION. When iOS cold-launches the app
  // straight into the BACKGROUND — which is exactly what the Apple Watch does when it
  // wakes the phone — no transition ever happens, so these stayed `foreground` and the
  // whole Skia tree mounted: four Canvases, GPU surfaces, the Reanimated glide. That
  // is the "EGLConsumer is not attached to an OpenGL ES context" / starves-the-audio-
  // DSP case described above, running with nobody looking at the screen.
  //
  // Ask what state we are ACTUALLY in. Then a headless launch mounts no renderer at
  // all, and the watch is fed by the cheap raw-spectrum path that already exists for
  // the locked phone.
  const startedActive = AppState.currentState === 'active';
  const bgRef = useRef(!startedActive);
  const [active, setActive] = useState(startedActive);
  useEffect(() => {
    const sub = AppState.addEventListener('change', (s) => {
      const bg = s !== 'active';
      bgRef.current = bg;
      setActive(!bg);
      if (bg) {
        cancelAnimation(scrollFrac);
        stopRevealStepper();
        stopSpecTween();
      }
    });
    return () => sub.remove();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [stopRevealStepper, stopSpecTween]);

  // ── Frame processing (imperative hot path — NO React state per frame) ──────
  // Frames arrive through frameSink, a ref the parent fills from onSpectrum.
  // Routing 10–20Hz frames through setState re-rendered the entire screen tree
  // per frame (~a full core of CPU). Per-render config is mirrored into a ref
  // so the stable callback never closes over stale props.
  const frameCfg = useRef({ width, wfTop, specH, specShow, peakHold,
                            smoothTune, rowsPerFrame: WF_ROWS, targetFps: TARGET_FPS,
                            minGapMs: WF_MIN_GAP_MS });
  frameCfg.current = { width, wfTop, specH, specShow, peakHold,
                       smoothTune, rowsPerFrame: WF_ROWS, targetFps: TARGET_FPS,
                       minGapMs: WF_MIN_GAP_MS };

  // Geometry the watch needs to crop a VFO-centred slice out of the row.
  const watchCfg = useRef({ tuneHz, filterLow, filterHigh });
  watchCfg.current = { tuneHz, filterLow, filterHigh };

  const handleFrame = useCallback((fbins: Float32Array, fstatus: SDRStatus) => {
    // Backgrounded: audio keeps playing on its own native path, but the whole
    // visual frame path (Reanimated withTiming glide + reveal/tween setIntervals
    // driving Skia) must NOT run. Under New Arch + worklets these are no longer
    // implicitly frozen by vsync pausing, so a left-running driver busy-loops the
    // native/worklets queue on the little cores and starves the audio DSP (the v6
    // WFM background-breakup regression). Skip every visual update while inactive.
    if (bgRef.current) return;
    const cfg = frameCfg.current;
    if (!fbins || fbins.length === 0 || cfg.width < 4) return;

    // 1. M9PSY pipeline + UberSDR auto-range
    // interacting → the processor bypasses FFT smoothing (its EMA lags and would slow the spectrum
    // during a tune/zoom). Same interaction window as the boost below.
    const interacting = cfg.smoothTune && Date.now() - (lastInteractAt?.current ?? 0) < SMOOTH_TUNE_TAIL_MS;
    const frame = proc.current.process(fbins, fstatus.centerHz, fstatus.bwHz, interacting);

    // While the phone is rendering, the watch BORROWS this row rather than
    // running its own SignalProcessor over 4096 bins on this same thread — that
    // duplicate pass stole the headroom the spectrum tween needs and showed up as
    // a jerky trace. Forwarding a row we already have is nearly free, and it's
    // pixel-identical to what's on screen. (Locked phone: no renderer, so
    // watchProvider does its own DSP from SDRScreen.onSpectrum instead.)
    const wc = watchCfg.current;
    watchProvider.pushProcessedRow(frame.row, {
      // trueCenterHz = the frame's REAL bin centre. The watch INDEXES INTO these bins to crop
      // around the VFO, so it must use the real centre or the signal draws offset from the VFO
      // (the "signal next to the VFO" bug). This does NOT change the main app's own render —
      // the waterfall above still uses fstatus.centerHz (the predicted centre) for gesture
      // continuity; only what the WATCH is told to crop by changes.
      centerHz:   fstatus.trueCenterHz ?? fstatus.centerHz,
      bwHz:       fstatus.bwHz,
      tuneHz:     wc.tuneHz,
      filterLow:  wc.filterLow,
      filterHigh: wc.filterHigh,
    });
    // dB axis labels — 2dB HYSTERESIS, not just rounding: a noisy floor
    // hovering on a .5 boundary flipped the rounded value every few frames,
    // re-rendering the whole WaterfallView tree at up to 10Hz (profiled:
    // React task execution was still ~a third of all JS post-meter-bus).
    const rMin = Math.round(frame.dbMin), rMax = Math.round(frame.dbMax);
    setLiveRange(prev =>
      Math.abs(prev.dbMin - rMin) < 2 && Math.abs(prev.dbMax - rMax) < 2
        ? prev : { dbMin: rMin, dbMax: rMax });

    // 2. Frame interval + interaction state (smooth tune)
    const now = Date.now();
    // INTERACTION boost — drives the SPECTRUM TRACE (follow every data frame directly) + peak hold.
    const boost = cfg.smoothTune &&
      now - (lastInteractAt?.current ?? 0) < SMOOTH_TUNE_TAIL_MS;
    if (lastFrameTs.current > 0 && !boost) {
      // Freeze the reveal-interval estimate DURING interaction: a tune/zoom re-subscribes and frames
      // PAUSE, so the resume gap would spike the estimate and slow the scroll for a beat — the tune
      // slowdown. The jitter buffer's prefill/hold covers the pause; the estimate re-converges after.
      const dt = now - lastFrameTs.current;
      avgFrameMs.current = avgFrameMs.current * 0.8 + dt * 0.2;
      uAvgMs.value = avgFrameMs.current;   // the tween worklet reads this, not the ref
    }
    lastFrameTs.current = now;
    // WATERFALL boost — the continuous vsync glide instead of discrete whole-line steps. Also on at low
    // fps so the low-rate scroll is silky (no added latency; pure scroll interpolation). ★ NOT applied
    // to the spectrum trace: at ~10 fps the trace must keep its ~30 fps EASING TWEEN — making it follow
    // each frame directly there looks jerky (Stuart 2026-07-24). Waterfall and trace want opposite
    // things at a low data rate, so the flags are split.
    const lowFps = avgFrameMs.current > LOW_FPS_GLIDE_MS;
    const wfBoost = boost || (cfg.smoothTune && lowFps);

    // 3. Waterfall (phase 2): ONE raw frame row into the ring — the shader
    //    synthesizes the line-rate look (uN lines/frame, temporal blend of
    //    adjacent frames) and the reveal. JS just advances the fraction.
    // ★ trueCenterHz = the frame's REAL bin centre; passing the PREDICTED centre is
    // what asserted the prediction had already come true.
    // ★★★ DISCARD THE EXCESS. The multiplier is fixed for the worst-case feed, so on a FASTER one
    //     the waterfall would run that many times too quick. Holding the target means taking fewer
    //     SOURCE frames: one row-set every (uN / targetRows) seconds — 300 ms for 6x at 20 rows/s.
    //     On UberSDR at 3.3 fps nothing is ever skipped (the frames are already further apart than
    //     the gap); on a 20 fps VibeServer five of every six are.
    //     ★ ONLY the waterfall is gated. The spectrum trace, audio, RDS and the rate meters above
    //       have already run for this frame and still see every one — the trade being bought here
    //       is waterfall TIME RESOLUTION, nothing else.
    // ★★★ SKIP THE WATERFALL ONLY — NOT THE WHOLE FRAME. This was an early `return`, and the
    //     SPECTRUM TRACE is built further down (section 4), so the gate dropped the trace as well:
    //     on a fast feed five of every six trace updates vanished and the spectrum juddered badly
    //     (Stuart, build 70). My comment claimed the trace had "already run for this frame" — it
    //     had not, and I never checked the order before asserting it.
    const wfGated = cfg.minGapMs > 0 && now - lastWfPushAt.current < cfg.minGapMs;
    if (!wfGated) {
    lastWfPushAt.current = now;

    pushRow(frame.row,
            fstatus.trueCenterHz ?? fstatus.centerHz,
            fstatus.bwHz > 0 && frame.row.length > 0 ? fstatus.bwHz / frame.row.length : 0);
    // copies synchronously — no snapshot needed
    uQuantSv.value = wfBoost ? 0 : 1;
    const dur = Math.max(50, Math.min(1000, avgFrameMs.current));
    if (wfBoost) {
      // Interaction / native rate: the 120Hz vsync glide already handles smoothness, so keep uN on the
      // STABLE static multiplier. ★ Deriving it from the LIVE data rate here (which spikes and
      // fluctuates during a gesture) made uN jump every frame — jerking the scroll AND pulsing the
      // shader's temporal blend into a brighter noise band that settled back on release (Stuart
      // 2026-07-24). The dynamic target-rate interpolation is for the SETTLED low-fps path only.
      // ★★ HOLD THE LAST SETTLED uN, do not swap to a different one. uN is the
      // shader's lines-per-frame and ALL history is mapped through it, so any
      // change rescales the whole waterfall vertically at once. Using
      // cfg.rowsPerFrame here while the settled path uses dynRows meant the two
      // disagreed — at the 20fps display mode with ~18fps data, settled gives
      // round(20/18)=1 and a gesture gave 2 — so the waterfall SQUASHED at the
      // start of every zoom/tune and POPPED OPEN again when it settled
      // (Stuart, 2026-07-26).
      //
      // ★ The original reason for a static value still holds: deriving uN from
      // the LIVE data rate during a gesture made it jump every frame. Holding
      // the last settled value satisfies both — stable during the gesture, and
      // identical to what settles afterwards, so nothing rescales.
      // ★ The same constant as the settled path. The old static hold existed only because that
      //   path derived a LIVE value that jumped during a gesture — there is nothing live left.
      uNSv.value = cfg.rowsPerFrame;
      stopRevealStepper();
      scrollFrac.value = 0;
      scrollFrac.value = withTiming(1, { duration: dur, easing: Easing.linear });
    } else {
      // Settled: interpolate UP to hold at least the target scroll rate from the (now stable) data
      // rate — 5fps Low Data still scrolls at the chosen 10/20/30 fps; fast data (Kiwi) needs none.
      // A CONSTANT — never derived from the measured rate, so it can never rescale the picture.
      const dynRows = cfg.rowsPerFrame;
      lastDynRows.current = dynRows;   // the gesture branch holds the same number now
      uNSv.value = dynRows;
      startRevealStepper(dynRows, dur);
    }
    }   // end !wfGated — everything below (trace, peaks, meters) runs for EVERY frame

    // 4. Spectrum + peak paths from normalised [0,1] traces
    if (cfg.specShow && cfg.specH > 4) {
      // The trace ALWAYS eases via the ~30fps tween — including DURING interaction. The old boost
      // path snapped the trace to each DATA frame, so it redrew only at the data rate: fine at
      // 20/30fps, but at 5fps it juddered and looked slow on every tune/zoom (Stuart 2026-07-24,
      // "the tune/zoom spectrum slowdown", "worse at 5 than 10fps"). The tween runs on the display
      // clock regardless of the data rate, so the trace stays fluid at ANY rate — the way
      // VibeServer/Jr do it. The view is pinned during a gesture, so the spectrum content barely
      // shifts and the small easing lag is invisible; on release the tween eases to the new view.
      {
        // Retarget the tween at the latest frame.
        // ★ Array.from, because a worklet cannot be handed a Float32Array — the shared value has to
        //   carry a plain array. This is once per DATA frame (~20/s), replacing thirty path builds
        //   a second on this thread, so the JS thread is left far quieter than before.
        const next = Array.from(frame.spec);
        specToSv.value = next;
        if (specDispSv.value.length !== next.length) specDispSv.value = Array.from(next);
        setSpecTweenActive(true);   // idle → running; a no-op while it already is
      }

      if (cfg.peakHold && !boost) {
        const pk = frame.peak;
        const sLen = pk.length;
        const baseline = cfg.wfTop;
        const w = Math.floor(cfg.width);
        const count = Math.ceil(w / SPEC_PX_STEP);
        const pool = peakPtsPool.current;
        while (pool.length < count) pool.push({ x: 0, y: 0 });
        if (pool.length > count) pool.length = count;
        for (let k = 0; k < count; k++) {
          const px = k * SPEC_PX_STEP;
          const v = pk[Math.floor((px / cfg.width) * sLen)];
          pool[k].x = px; pool[k].y = baseline - v * cfg.specH;
        }
        const pp = Skia.Path.Make();
        pp.addPoly(pool, false);
        swapPath(peakPath, pp);
      } else {
        // Peak hold PAUSES while interacting: bin-indexed peaks detach from
        // their signals as the view moves (geometry is pinned during gestures
        // so the processor can't see the shift) and smear the display. Clear
        // and hide; it re-seeds within a frame or two of settling.
        if (boost) proc.current.resetPeakHold();
        swapPath(peakPath, Skia.Path.Make()); // empty = draw nothing
      }
    } else {
      // Spectrum hidden — nothing needs inter-frame smoothness; the panel can
      // fall all the way to the data rate.
      stopSpecTween();
      // ★★★ CLEAR IT, DO NOT swapPath IT. swapPath DISPOSES the outgoing path, and since the tween
      // moved to the UI thread the outgoing path is one of the two PERSISTENT buffers the worklet
      // resets and reuses every tick — disposing it would hand the worklet a dead native object
      // the next time the spectrum came back. Emptying in place is what "draw nothing" needs
      // anyway, and it leaves both buffers valid.
      specPathA.reset(); specPathB.reset();
      specPath.value = specPathA;
      swapPath(peakPath, Skia.Path.Make());
    }

  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ── Jitter buffer ──────────────────────────────────────────────────────────
  // Queue arrivals and DRAIN them into handleFrame on a STEADY clock, so network / interaction
  // arrival jitter never reaches the display — the reason Jr and the web client feel smooth at a
  // variable/low frame rate where the phone (rendering per-arrival) juddered. targetDepth = 1 frame of
  // banked latency (insurance against a late row); prefill/hold when dry, catch up when backed up.
  const jbQueue       = useRef<Array<{ bins: Float32Array; status: SDRStatus }>>([]);
  const jbArrivalMs   = useRef(150);   // measured arrival cadence (drives the drain rate)
  const jbLastArrival = useRef(0);
  const jbTimer       = useRef<ReturnType<typeof setTimeout> | null>(null);
  const jbPrefill     = useRef(true);
  const JB_TARGET_DEPTH = 1;
  const JB_MAX_QUEUE    = 3;

  const drainFrame = useCallback(() => {
    jbTimer.current = null;
    if (bgRef.current) { jbQueue.current.length = 0; jbPrefill.current = true; return; }
    const q = jbQueue.current;
    if (jbPrefill.current) {
      if (q.length >= JB_TARGET_DEPTH) jbPrefill.current = false;
      else return;                                   // hold until banked (a new arrival reschedules)
    }
    if (q.length === 0) { jbPrefill.current = true; return; }   // ran dry → hold, re-prefill
    const f = q.shift()!;
    handleFrame(f.bins, f.status);
    // Reschedule at the arrival cadence (steady); a little faster if backed up, to catch up.
    const dur = Math.max(40, Math.min(1000,
      q.length > JB_TARGET_DEPTH ? jbArrivalMs.current * 0.6 : jbArrivalMs.current));
    jbTimer.current = setTimeout(drainFrame, dur);
  }, [handleFrame]);

  const enqueueFrame = useCallback((bins: Float32Array, status: SDRStatus) => {
    if (bgRef.current) return;
    const now = Date.now();
    const interacting = now - (lastInteractAt?.current ?? 0) < SMOOTH_TUNE_TAIL_MS;
    // DURING interaction, BYPASS the buffer — tune/zoom must feel instant. The jitter buffer only
    // exists to smooth steady-state scroll; holding a frame here costs up to a full arrival period
    // (~200ms at 5fps) of lag on every crown move — the slowdown VibeServer/Jr don't have because
    // they render straight through. Flush the queue and render this frame now; re-bank afterwards.
    if (interacting) {
      if (jbTimer.current) { clearTimeout(jbTimer.current); jbTimer.current = null; }
      jbQueue.current.length = 0;
      jbPrefill.current = true;          // re-prefill once the gesture ends
      jbLastArrival.current = now;       // don't let the resume gap spike jbArrivalMs
      handleFrame(bins, status);         // consumed synchronously — parent's reused buffers are safe
      return;
    }
    if (jbLastArrival.current > 0) {
      const dt = now - jbLastArrival.current;
      jbArrivalMs.current = jbArrivalMs.current * 0.8 + dt * 0.2;
    }
    jbLastArrival.current = now;
    const q = jbQueue.current;
    q.push({ bins: bins.slice(), status: { ...status } });   // copy: the parent reuses its buffers
    if (q.length > JB_MAX_QUEUE) q.shift();                   // bound the latency
    if (!jbTimer.current) jbTimer.current = setTimeout(drainFrame, 0);  // kick the drain
  }, [drainFrame, handleFrame, lastInteractAt]);

  useEffect(() => () => { if (jbTimer.current) { clearTimeout(jbTimer.current); jbTimer.current = null; } }, []);

  useEffect(() => {
    if (!frameSink) return;
    frameSink.current = enqueueFrame;    // frames flow through the jitter buffer, not straight to render
    return () => { frameSink.current = null; };
  }, [frameSink, enqueueFrame]);

  // (Palette switches need no rebuild anymore — the lutImage memo swaps the
  // 256×1 LUT texture and the shader recolours the whole history next draw.)

  // ── Frequency geometry ──────────────────────────────────────────────────────
  const visStart = centerHz - bwHz / 2;
  const pxPerHz  = bwHz > 0 ? width / bwHz : 0;
  const hzToX    = useCallback((hz: number) => (hz - visStart) * pxPerHz,
    [visStart, pxPerHz]);

  // ── Band plan segments (visible, region-filtered) ───────────────────────────
  const bandSegs = useMemo(() => {
    if (!(bwHz > 0)) return [];
    const visEnd = visStart + bwHz;
    const segs: Array<{ x0: number; x1: number; color: string; label: string; key: string }> = [];
    for (const b of BAND_PLAN) {
      if (b.regions && !b.regions.includes(ituRegion)) continue;
      if (b.hi < visStart || b.lo > visEnd) continue;
      const x0 = Math.max(0, hzToX(b.lo));
      const x1 = Math.min(width, hzToX(b.hi));
      const px = x1 - x0;
      if (px <= 0) continue;
      const label = px < 28 ? '' : px < 60 ? (b.bandLabel ?? b.name.split(' ')[0]) : b.name;
      segs.push({ x0, x1, color: bandColor(b), label, key: `${b.lo}-${b.hi}` });
    }
    return segs;
  }, [visStart, bwHz, width, ituRegion, hzToX]);

  // ── Frequency ticks ─────────────────────────────────────────────────────────
  const ticks = useMemo(() => {
    if (!(bwHz > 0)) return [];
    const targetTicks  = Math.max(4, Math.min(8, Math.floor(width / 70)));
    let spacing = niceTick(bwHz / targetTicks);
    const minGapPx = 52;
    while (spacing * pxPerHz < minGapPx) spacing *= 2;
    const first = Math.ceil(visStart / spacing) * spacing;
    const out: Array<{ x: number; label: string; showLabel: boolean }> = [];
    let lastLabelX = -999;
    for (let f = first; f <= visStart + bwHz; f += spacing) {
      const x = hzToX(f);
      const showLabel = x - lastLabelX >= minGapPx;
      if (showLabel) lastLabelX = x;
      out.push({ x, label: fmtHz(f), showLabel });
    }
    return out;
  }, [visStart, bwHz, width, pxPerHz, hzToX]);

  // ── dB axis labels (5 stops over spectrum panel) ────────────────────────────
  const dbLabels = useMemo(() => {
    if (!specShow || specH < 40) return [];
    const range = liveRange.dbMax - liveRange.dbMin;
    const out: Array<{ y: number; label: string }> = [];
    for (let di = 0; di <= 4; di++) {
      const frac = di / 4;
      out.push({
        y: wfTop - frac * specH,
        label: Math.round(liveRange.dbMin + frac * range) + 'dB',
      });
    }
    return out;
  }, [specShow, specH, wfTop, liveRange]);

  // ── Needle + sideband geometry (v1.5) ───────────────────────────────────────
  const needle = useMemo(() => {
    if (!(bwHz > 0) || !(tuneHz > 0)) return null;
    const nX = hzToX(tuneHz);
    let loX = hzToX(tuneHz + filterLow);
    let hiX = hzToX(tuneHz + filterHigh);
    const minSbPx = filterLow === 0 && filterHigh === 0 ? 20 : 4;
    if (nX - loX < minSbPx) loX = nX - minSbPx;
    if (hiX - nX < minSbPx) hiX = nX + minSbPx;
    const scale = Math.max(0.25, Math.min(1.0, pxPerHz * 4000));
    // Quantised scale (0.05 steps) so the pre-rendered glow strips are not
    // re-blurred on every zoom tick — only on meaningful scale changes.
    const scaleQ = Math.round(scale * 20) / 20;
    return { nX, loXc: Math.max(0, loX), hiXc: Math.min(width, hiX), loX, hiX, scale, scaleQ };
  }, [bwHz, tuneHz, filterLow, filterHigh, hzToX, pxPerHz, width]);

  // ── VFO-lock boundary walls + RF-centre marker (BRIEF §5.6) ─────────────────
  // Geometry only; rendered on the spectrum/needle canvas via hzToX so it tracks
  // the live view. Default off → existing callers see nothing.
  const wallOverlay = useMemo(() => {
    if (!showWalls || !(bwHz > 0)) return null;
    const out: { loX: number | null; hiX: number | null } = { loX: null, hiX: null };
    if (typeof panLoHz === 'number') { const x = hzToX(panLoHz); if (x >= 0 && x <= width) out.loX = x; }
    if (typeof panHiHz === 'number') { const x = hzToX(panHiHz); if (x >= 0 && x <= width) out.hiX = x; }
    return (out.loX === null && out.hiX === null) ? null : out;
  }, [showWalls, panLoHz, panHiHz, hzToX, width, bwHz]);

  // RF-centre marker — thin dashed line drawn as short segments (no new Skia
  // imports); only meaningful on local IQ once Phase 2 feeds centerMarkerHz.
  const centerMarker = useMemo(() => {
    if (!showCenterMarker || typeof centerMarkerHz !== 'number' || !(bwHz > 0)) return null;
    const x = hzToX(centerMarkerHz);
    if (x < 0 || x > width) return null;
    const dashes: number[] = [];
    for (let y = BAND_H; y < height; y += 10) dashes.push(y);  // 6px on / 4px gap
    return { x, dashes };
  }, [showCenterMarker, centerMarkerHz, hzToX, width, height, bwHz]);

  // ── Skia paints ─────────────────────────────────────────────────────────────
  const peakPaint = useMemo(() => {
    const p = Skia.Paint();
    p.setColor(Skia.Color(hexRgba(needleColor, 0.85)));
    p.setStrokeWidth(1);
    p.setStyle(1);
    p.setAntiAlias(true);
    p.setMaskFilter(Skia.MaskFilter.MakeBlur(BlurStyle.Normal, 2, false));
    return p;
  }, [needleColor]);

  // ── Pre-rendered glow strips (power) ────────────────────────────────────────
  // Gaussian blur masks are the most expensive primitive Skia draws. Rendering
  // the needle (σ 28/16/6) and sideband edges (σ 8) live meant re-blurring
  // full-height layers on every canvas repaint. Instead they are blurred ONCE
  // here into offscreen raster strips and composited as plain textures.
  const dpr = PixelRatio.get();

  const needleStrip = useMemo(() => {
    if (height < 4) return null;
    const sc = needle?.scaleQ ?? 1;
    // σ in MakeBlur(…, false) is device px; ±3σ in dp covers the full halo.
    const halfW = Math.ceil((3 * 28 * sc) / dpr + 2 * sc + 2);
    const w = halfW * 2;
    const surface = Skia.Surface.Make(Math.ceil(w * dpr), Math.ceil(height * dpr));
    if (!surface) return null;
    const c = surface.getCanvas();
    c.scale(dpr, dpr);
    const path = Skia.Path.Make();
    path.moveTo(halfW, 0); path.lineTo(halfW, height);
    const layer = (alpha: number, blur: number, sw: number) => {
      const p = Skia.Paint();
      p.setColor(Skia.Color(alpha >= 1 ? needleColor : hexRgba(needleColor, alpha)));
      p.setStrokeWidth(sw);
      p.setStyle(1);
      p.setAntiAlias(true);
      p.setMaskFilter(Skia.MaskFilter.MakeBlur(BlurStyle.Normal, blur, false));
      c.drawPath(path, p);
    };
    // Intensity 1–10 (5 = original): scales halo alphas and, above 5, widens
    // the strokes too — a maxed needle punches through the brightest palettes
    const k = needleIntensity / 5;
    const wk = Math.max(1, k);
    layer(Math.min(1, 0.35 * k), 28 * sc, 1.5 * sc * wk);  // outer halo
    layer(Math.min(1, 0.70 * k), 16 * sc, 0.8 * sc * wk);  // mid glow
    layer(Math.min(1, 0.80 * k),  6 * sc, 0.8 * wk);       // filament glow
    // CRISP core filament — HTML canvas shadowBlur glows BEHIND a sharp
    // stroke, but MaskFilter blurs the stroke itself; the v1 acrylic look
    // is blurred halo + razor hairline on top.
    const crisp = Skia.Paint();
    crisp.setColor(Skia.Color(needleColor));
    crisp.setStrokeWidth(0.75 * wk);
    crisp.setStyle(1);
    crisp.setAntiAlias(true);
    c.drawPath(path, crisp);
    return { img: surface.makeImageSnapshot(), halfW, w };
  }, [needleColor, needleIntensity, needle?.scaleQ, height, dpr]);

  const edgeStrip = useMemo(() => {
    const h = height - BAND_H;
    if (h < 4) return null;
    const sc = needle?.scaleQ ?? 1;
    const halfW = Math.ceil((3 * 8) / dpr + sc + 2);
    const w = halfW * 2;
    const surface = Skia.Surface.Make(Math.ceil(w * dpr), Math.ceil(h * dpr));
    if (!surface) return null;
    const c = surface.getCanvas();
    c.scale(dpr, dpr);
    const path = Skia.Path.Make();
    path.moveTo(halfW, 0); path.lineTo(halfW, h);
    const p = Skia.Paint();
    p.setColor(Skia.Color(hexRgba(needleColor, 0.35)));
    p.setStrokeWidth(sc);
    p.setStyle(1);
    p.setAntiAlias(true);
    p.setMaskFilter(Skia.MaskFilter.MakeBlur(BlurStyle.Normal, 8, false));
    c.drawPath(path, p);
    // Crisp acrylic edge line on top of the glow (v1 shadow semantics).
    const pc = Skia.Paint();
    pc.setColor(Skia.Color(hexRgba(needleColor, 0.35)));
    pc.setStrokeWidth(Math.max(0.75, sc * 0.75));
    pc.setStyle(1);
    pc.setAntiAlias(true);
    c.drawPath(path, pc);
    return { img: surface.makeImageSnapshot(), halfW, w, h };
  }, [needleColor, needle?.scaleQ, height, dpr]);

  // ── Gestures (tap-to-tune / pan / pinch-zoom) ───────────────────────────────
  const lastPanX = useRef(0);
  const lastPanY = useRef(0);
  const pinchRef = useRef(1);
  // UI-thread pan state (worklet-accessible) — the pan gesture runs on the UI
  // thread so a busy/laggy JS thread (heavy incoming data) can't stall it.
  const panLastXSv   = useSharedValue(0);
  const panBlockedSv = useSharedValue(false);

  // Bottom-edge guard: the gap below the control pill overlaps the home
  // indicator; tuning/panning/zooming that started there fought the system
  // swipe-up. Reject any waterfall gesture whose touch begins in that strip.
  const guardTop = height - bottomGuard;          // y at/under which we ignore
  const panBlocked   = useRef(false);
  const pinchBlocked = useRef(false);
  const tapStartGuarded = useRef(false);          // touch-down began in the home-bar gap

  const tapGesture = useMemo(() =>
    Gesture.Tap().runOnJS(true).maxDuration(300)
      .onBegin((e: any) => {
        // A home-bar swipe-up STARTS in the guard strip but releases higher up,
        // so an onEnd-only check let it slip through and retune. Latch the
        // touch-down position and reject in onEnd.
        tapStartGuarded.current = bottomGuard > 0 && e.y >= guardTop;
      })
      .onEnd((e: any) => {
        if (!bwHz || !centerHz) return;
        if (tapStartGuarded.current) return;            // began in home-bar gap
        if (e.y < BAND_H) return; // band strip taps reserved (future: band jump)
        if (bottomGuard > 0 && e.y >= guardTop) return; // home-bar gap
        onTapTune?.(Math.round(visStart + (e.x / width) * bwHz));
      }), [bwHz, centerHz, visStart, width, onTapTune, bottomGuard, guardTop]);

  // Pan runs as a WORKLET (UI thread): event delivery + accumulation never wait
  // on the JS thread, so a laggy connection / heavy data can't make the drag
  // feel slow. Only the actual pan command hops to JS (runOnJS), where the
  // client coalesces sends. Drag = pan, always (zoom is pinch-only).
  const panGesture = useMemo(() =>
    Gesture.Pan().minDistance(4)
      .onStart((e: any) => {
        'worklet';
        panLastXSv.value = 0;
        panBlockedSv.value = bottomGuard > 0 && e.y >= guardTop;
      })
      .onUpdate((e: any) => {
        'worklet';
        if (panBlockedSv.value) return;
        // PAN_GAIN > 1 makes the waterfall travel further per finger-px (lighter).
        const dx = (e.translationX - panLastXSv.value) * PAN_GAIN;
        panLastXSv.value = e.translationX;
        if (onPanDelta) runOnJS(onPanDelta)(-dx);
      }), [onPanDelta, bottomGuard, guardTop, PAN_GAIN, panLastXSv, panBlockedSv]);

  const pinchGesture = useMemo(() =>
    Gesture.Pinch().runOnJS(true)
      .onStart((e: any) => {
        pinchRef.current = 1;
        pinchBlocked.current = bottomGuard > 0 && e.focalY >= guardTop;
      })
      .onUpdate((e: any) => {
        if (pinchBlocked.current) return;
        const delta = e.scale / pinchRef.current;
        pinchRef.current = e.scale;
        onPinchZoom?.(delta);
      }), [onPinchZoom, bottomGuard, guardTop]);

  const gesture = useMemo(() =>
    Gesture.Simultaneous(Gesture.Exclusive(tapGesture, panGesture), pinchGesture),
    [tapGesture, panGesture, pinchGesture]);

  // Needle + acrylics memoised likewise — only rebuilds when the needle
  // geometry or colour actually changes.
  const needleCanvas = useMemo(() => (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width, height }}>

      {/* ── Frosted backing (under the acrylics): smoked-glass band dims the
             waterfall across the passband so the needle keeps contrast on
             bright palettes whatever colour it is ── */}
      {needle && needleFrost > 0 && needle.hiXc > needle.loXc && (
        <Rect x={needle.loXc} y={BAND_H}
              width={needle.hiXc - needle.loXc} height={height - BAND_H}
              color={`rgba(0,0,0,${(needleFrost / 10) * 0.72})`} />
      )}

      {/* ── Acrylic sideband panels (band-strip bottom → screen bottom) ── */}
      {needle && needle.nX > needle.loXc && (
        <Rect x={needle.loXc} y={BAND_H}
              width={needle.nX - needle.loXc} height={height - BAND_H}>
          <LinearGradient
            start={vec(needle.loXc, 0)} end={vec(needle.nX, 0)}
            colors={[hexRgba(needleColor, 0.03), hexRgba(needleColor, 0.06),
                     hexRgba(needleColor, 0.14), hexRgba(needleColor, 0.28)]}
            positions={[0, 0.15, 0.55, 1]} />
        </Rect>
      )}
      {needle && needle.hiXc > needle.nX && (
        <Rect x={needle.nX} y={BAND_H}
              width={needle.hiXc - needle.nX} height={height - BAND_H}>
          <LinearGradient
            start={vec(needle.nX, 0)} end={vec(needle.hiXc, 0)}
            colors={[hexRgba(needleColor, 0.28), hexRgba(needleColor, 0.14),
                     hexRgba(needleColor, 0.06), hexRgba(needleColor, 0.03)]}
            positions={[0, 0.45, 0.85, 1]} />
        </Rect>
      )}
      {needle && needle.loXc > 0 && edgeStrip && (
        <SkiaImage image={edgeStrip.img} x={needle.loXc - edgeStrip.halfW} y={BAND_H}
                   width={edgeStrip.w} height={edgeStrip.h} fit="fill" />
      )}
      {needle && needle.hiXc < width && edgeStrip && (
        <SkiaImage image={edgeStrip.img} x={needle.hiXc - edgeStrip.halfW} y={BAND_H}
                   width={edgeStrip.w} height={edgeStrip.h} fit="fill" />
      )}

      {/* ── Boundary walls (unlocked pan limits) — solid edge line + a
             low-alpha frosted fill over the dead zone beyond it ── */}
      {wallOverlay?.loX != null && (<>
        <Rect x={0} y={BAND_H} width={wallOverlay.loX} height={height - BAND_H}
              color="rgba(0,0,0,0.45)" />
        <Rect x={wallOverlay.loX - 0.75} y={BAND_H} width={1.5} height={height - BAND_H}
              color="rgba(255,200,80,0.85)" />
      </>)}
      {wallOverlay?.hiX != null && (<>
        <Rect x={wallOverlay.hiX} y={BAND_H} width={width - wallOverlay.hiX} height={height - BAND_H}
              color="rgba(0,0,0,0.45)" />
        <Rect x={wallOverlay.hiX - 0.75} y={BAND_H} width={1.5} height={height - BAND_H}
              color="rgba(255,200,80,0.85)" />
      </>)}

      {/* ── Secondary RF-centre marker (dashed, subordinate to the needle) ── */}
      {centerMarker && centerMarker.dashes.map((y, i) => (
        <Rect key={'cm' + i} x={centerMarker.x - 0.5} y={y} width={1} height={6}
              color={centerMarkerColor} />
      ))}

      {/* ── LED needle: halo → glow → filament (cached strip) ── */}
      {needle && needleStrip && (
        <SkiaImage image={needleStrip.img} x={needle.nX - needleStrip.halfW} y={0}
                   width={needleStrip.w} height={height} fit="fill" />
      )}

    </Canvas>
  ), [width, height, needle, needleStrip, edgeStrip, needleColor, needleFrost,
      wallOverlay, centerMarker, centerMarkerColor]);

  // Static overlay (band plan/ticks/dB lines) memoised as ELEMENTS — when the
  // component re-renders for unrelated reasons React reuses the subtree and
  // skips reconciling dozens of Skia nodes.
  const staticOverlayCanvas = useMemo(() => (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width, height }}>
      {/* Opaque backing for the band/tick strips only — the spectrum graph
          area stays transparent so the instance backdrop image (an RN Image
          UNDER this canvas) can show through; without an image the root's
          #000 shows, indistinguishable from the old rgb(2,2,2) full-height
          backing (WebGL parity). */}
      <Rect x={0} y={0} width={width} height={specTop} color="rgb(2,2,2)" />
      {/* ── Band plan strip ── */}
      {bandSegs.map(s => (
        <Rect key={s.key} x={s.x0} y={0} width={s.x1 - s.x0} height={BAND_H}
              color={s.color} />
      ))}
      <Rect x={0} y={BAND_H - 1} width={width} height={1}
            color="rgba(255,200,80,0.25)" />
      {/* ── Ticker backing + tick marks ── */}
      <Rect x={0} y={tickTop} width={width} height={TICK_H}
            color="rgba(0,10,4,0.85)" />
      {ticks.map((t, i) => (
        <Rect key={i} x={t.x - 0.5} y={tickTop} width={1} height={5}
              color="rgba(0,180,60,0.45)" />
      ))}
      {/* ── Faint dB reference lines ── */}
      {specShow && dbLabels.map((d, i) => (
        <Rect key={i} x={0} y={d.y} width={width} height={0.5}
              color="rgba(255,180,0,0.12)" />
      ))}
    </Canvas>
  ), [width, height, specTop, tickTop, bandSegs, ticks, dbLabels, specShow]);

  // ── Render ──────────────────────────────────────────────────────────────────
  // Canvas 1 (bottom): waterfall texture only — the ONLY thing the 120Hz
  // Reanimated scroll repaints. Canvas 2 (top): everything else, repainted at
  // the 10Hz data rate. The canvas bounds clip the over-tall scrolling image.
  return (
    <GestureDetector gesture={gesture}>
      <View style={[styles.root, { width, height }]}>

        {/* Instance spectrum backdrop — behind the line graph only (web
            parity); the canvases above are transparent in that region */}
        {bgImageUrl != null && bgOpacity > 0 && specShow && specH > 8 && (
          <RNImage
            source={{ uri: bgImageUrl }}
            resizeMode="cover"
            style={{ position: 'absolute', left: 0, top: specTop, width,
                     height: specH, opacity: bgOpacity }}
          />
        )}

        {/* Skia canvases render ONLY while foreground — unmounted in the
            background so the render/present loop fully stops (see `active`). */}
        {active && (
        <Canvas style={{ position: 'absolute', left: 0, top: wfTop, width, height: wfH }}>
          {/* GPU waterfall: intensity ring + LUT sampled by the runtime
              shader; scroll/slide/sharpness/contrast are uniforms (UI-thread,
              zero React). Child order maps to `uniform shader wf, lut`. */}
          {texReady && lutImage && WF_EFFECT && (
            <Fill>
              <Shader source={WF_EFFECT} uniforms={wfUniforms}>
                <ImageShader image={wfImage} fit="none" />
                <ImageShader image={lutImage} fit="none" />
              </Shader>
            </Fill>
          )}
        </Canvas>
        )}

        {active && staticOverlayCanvas}

        {/* Init splash — the spectrum WS takes 1-2s to deliver its first
            frame; show intent instead of a black void. texReady flips on the
            first pushed row and never re-renders after. */}
        {!texReady && (
          <View style={wfStyles.initWrap} pointerEvents="none">
            <Text style={[wfStyles.initText, { fontFamily }]}>
              WATERFALL INITIALIZING…
            </Text>
          </View>
        )}

        {/* Canvas: LIVE spectrum trace — isolated so the ~30Hz tween repaints
            ONLY these two paths, not the band plan/ticks/needle/acrylics
            (sharing one canvas redrew the whole overlay per tween tick). */}
        {active && (
        <Canvas style={{ position: 'absolute', left: 0, top: 0, width, height: wfTop + 1 }}>
          {specShow && (
            <Path path={specPath} style="fill">
              <LinearGradient start={vec(0, specTop)} end={vec(0, wfTop)}
                              colors={specGradColors} />
            </Path>
          )}
          {specShow && peakHold && (
            <Path path={peakPath} paint={peakPaint} />
          )}
        </Canvas>
        )}

        {active && needleCanvas}

        {/* ── Text overlays (RN Text — crisp, uses expo-font faces) ── */}

        {/* Band labels — clipped to segment width, white with dark shadow */}
        {bandSegs.filter(s => s.label).map(s => (
          <View key={'bl' + s.key} pointerEvents="none"
                style={[styles.bandLabelWrap,
                        { left: s.x0 + 2, width: s.x1 - s.x0 - 4, height: BAND_H }]}>
            <Text numberOfLines={1}
                  style={[styles.bandLabel, { fontFamily }]}>{s.label}</Text>
          </View>
        ))}

        {/* Ticker labels — green LED glow */}
        {ticks.filter(t => t.showLabel).map((t, i) => (
          <Text key={'tk' + i} pointerEvents="none"
                style={[styles.tickLabel, { fontFamily, left: t.x - 40, top: tickTop + 5 }]}>
            {t.label}
          </Text>
        ))}

        {/* RF-centre marker label — subordinate to the ticks, beside its line */}
        {centerMarker && (
          <Text pointerEvents="none"
                style={[styles.tickLabel, {
                  fontFamily, color: centerMarkerColor,
                  left: Math.min(width - 96, centerMarker.x + 3), top: specTop + 3,
                  width: 96, textAlign: 'left', fontSize: 9,
                }]}>
            {'RF CENTRE: ' + fmtHz(centerMarkerHz as number)}
          </Text>
        )}

        {/* Wall labels — an unlabelled line is just clutter: say WHICH edge of the
            captured band it is, and at what frequency. (Web-client parity.) */}
        {wallOverlay?.loX != null && (
          <Text pointerEvents="none"
                style={[styles.tickLabel, {
                  fontFamily, color: 'rgba(255,200,80,0.95)',
                  left: Math.max(2, wallOverlay.loX + 3), top: specTop + 16,
                  width: 130, textAlign: 'left', fontSize: 9,
                }]}>
            {'LOWER LIMIT: ' + fmtHz(panLoHz as number)}
          </Text>
        )}
        {wallOverlay?.hiX != null && (
          <Text pointerEvents="none"
                style={[styles.tickLabel, {
                  fontFamily, color: 'rgba(255,200,80,0.95)',
                  left: Math.max(2, wallOverlay.hiX - 133), top: specTop + 16,
                  width: 130, textAlign: 'right', fontSize: 9,
                }]}>
            {'UPPER LIMIT: ' + fmtHz(panHiHz as number)}
          </Text>
        )}

        {/* The needle's own label. With an RF-centre marker on screen too, an
            unlabelled needle is ambiguous — which line am I listening to? */}
        {needle && showCenterMarker && (
          <Text pointerEvents="none"
                style={[styles.tickLabel, {
                  fontFamily, color: needleColor,
                  left: Math.min(width - 96, needle.nX + 3), top: specTop + 29,
                  width: 96, textAlign: 'left', fontSize: 9,
                }]}>
            {'LISTEN: ' + fmtHz(tuneHz)}
          </Text>
        )}

        {/* dB axis — amber, left edge of spectrum */}
        {dbLabels.map((d, i) => (
          <Text key={'db' + i} pointerEvents="none"
                style={[styles.dbLabel, { fontFamily, top: d.y - 14 }]}>
            {d.label}
          </Text>
        ))}

        {/* Station-ID overlay — top-right of the spectrum (web parity:
            bold "CALLSIGN - NAME", location at 75% beneath, drop shadow) */}
        {stationId != null && specShow && specH > 40 && (
          <View pointerEvents="none"
                style={[styles.stationId, { top: specTop + 6 }]}>
            <Text style={[styles.stationIdL1, { color: stationId.color }]} numberOfLines={1}>
              {stationId.line1}
            </Text>
            {!!stationId.line2 && (
              <Text style={[styles.stationIdL2, { color: stationId.color }]} numberOfLines={1}>
                {stationId.line2}
              </Text>
            )}
          </View>
        )}

      </View>
    </GestureDetector>
  );
}

// ── Styles (typography from v1.5 canvas calls) ───────────────────────────────

const styles = StyleSheet.create({
  root: { overflow: 'hidden', backgroundColor: '#000' },
  stationId: { position: 'absolute', right: 6, alignItems: 'flex-end' },
  stationIdL1: {
    fontSize: 13, fontWeight: 'bold',
    textShadowColor: 'rgba(0,0,0,0.55)', textShadowOffset: { width: 1, height: 1 }, textShadowRadius: 0,
  },
  stationIdL2: {
    fontSize: 11, opacity: 0.75,
    textShadowColor: 'rgba(0,0,0,0.55)', textShadowOffset: { width: 1, height: 1 }, textShadowRadius: 0,
  },
  bandLabelWrap: {
    position: 'absolute', top: 0,
    alignItems: 'center', justifyContent: 'flex-end',
    overflow: 'hidden', paddingBottom: 2,
  },
  bandLabel: {
    fontSize: 9, fontWeight: 'bold', color: '#ffffff',
    textShadowColor: 'rgba(0,0,0,0.9)', textShadowRadius: 3,
    textShadowOffset: { width: 0, height: 0 },
  },
  tickLabel: {
    position: 'absolute', width: 80, textAlign: 'center',
    fontSize: 11, fontWeight: 'bold', color: '#00aa33',
    textShadowColor: '#00cc44', textShadowRadius: 5,
    textShadowOffset: { width: 0, height: 0 },
  },
  dbLabel: {
    position: 'absolute', left: 4,
    fontSize: 11, fontWeight: 'bold', color: 'rgba(255,180,60,0.90)',
    textShadowColor: 'rgba(0,0,0,0.75)', textShadowRadius: 2.5,
    textShadowOffset: { width: 0, height: 0 },
  },
});

// Memo wall: residual SDRScreen renders stop here — every prop is a
// primitive, a stable ref, or a useCallback.
export default React.memo(WaterfallView);

const wfStyles = StyleSheet.create({
  initWrap: {
    position: 'absolute', left: 0, right: 0, top: 0, bottom: 0,
    alignItems: 'center', justifyContent: 'center',
  },
  initText: {
    color: 'rgba(255,255,255,0.55)', fontSize: 14,
    letterSpacing: 2, fontWeight: '600',
  },
});
