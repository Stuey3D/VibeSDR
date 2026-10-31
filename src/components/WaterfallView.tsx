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

import { newRowTargetState, stepRowTarget, rowTargetUnderCap, scrollForCap, rowsForArrival, type WfScroll } from '../assets/wfRowTarget';
import { newRowCombiner, combineInto, dbPerUnit, COMBINE_MAX_FRAMES, type CombFrame } from '../assets/wfCombine';
import { boostActive, boostNote, boostRowRate, boostTaper, BOOST_TAIL_MS, clockArrive, clockIdle, clockNextAt, clockRateUnderCap, clockReset, clockRowRate, clockRun, clockSetRate, newBoost, newRowClock }
  from '../assets/wfClock';
import { avHold, avNoteAudio, clockOwnMs, newAvSync } from '../assets/wfAvSync';
import { audioPlayoutMs } from '../utils/audioPlayout';
import { noteFault } from '../services/faultLog';
import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { AppState, Image as RNImage, NativeModules, PixelRatio, Platform, StyleSheet, Text, View } from 'react-native';
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
  type SkFont,
  Group,
  Text as SkText,
  useTypeface,
} from '@shopify/react-native-skia';
import {
  useSharedValue,
  useDerivedValue,
  useFrameCallback,
  runOnJS,
  runOnUI,
  withTiming,
  type SharedValue,
} from 'react-native-reanimated';
import { INACTIVE_INTENSITY, SWITCH_FADE_MS } from '../services/vfoAB';
import { Gesture, GestureDetector } from 'react-native-gesture-handler';
import { getColorLUT } from '../assets/colormapUtils';
import type { SDRStatus } from '../services/UberSDRClient';
import { FramePool, type FrameSlot } from '../services/framePool';
/** Pooled jitter-buffer slots kept for reuse: the queue's bound (3) + the push before the bound bites + the one
 *  being drawn. More would only ever sit idle. */
const JB_MAX_QUEUE_SLOTS = 5;
import { SignalProcessor, type SignalProcessorSettings } from '../assets/signalProcessor';
import { useScreenCovered } from '../hooks/useScreenCovered';
import { cadenceGap, uiDisplayMaxHz, uiFrameCapHz, uiPanelMaxHz } from '../services/uiCadence';
import { watchProvider } from '../services/watchProvider';
import { BAND_PLAN_DRAW_ORDER, BAND_HEX, type Band } from '../constants/bandPlan';
import { labelLayout, estimateLabelWidth, knownLabelHit, isLabelTap, KNOWN_LABEL_COLOUR, KNOWN_LABEL_BOX, type KnownSignal, type BlockedBox } from '../utils/knownSignals';
import { screenNavRef } from './ScreenNav';
import { needleGeometry } from '../utils/needleGeometry';
import { displayMap, fanRefresh, fanRow, motionImageRows, newFanState, newMotionState, placeRow, rowGeometry, takeFanStep, wfImageBytes } from '../utils/wfRowPlace';
import { ringCapFromDeviceClass, ringNextRows, RING_MIN_ROWS } from '../utils/wfRingSize';
import { growRing } from '../utils/wfRingGrow';
import { readNativeDeviceClass } from '../services/deviceClass';
import { hzOnRow, rowAtLine, newStampState, stampAlpha, stampColour, stampJump, stampLine, stampNoteRow, stampPrune, stampSettleRows, stampsShown,
         stampSweepRows, stampTicks, stampTimeLabel, STAMP_MIN_GAP_LABELS, type WfStamp } from '../utils/wfStamps';
import { layerWriteRow, makeToIdx, medianOf, newLayerRing, newWideState, parseWide, wideArrive, type WideState } from '../utils/wfWide';
import { setWideSink } from '../services/wideRowBus';
import { contAtWholeRow, contPace, easeAt, easeCompose, paceToCont, paceToSettled, PLAYOUT_MS, rowPace, rowRate, shownRate, traceLagMs } from '../utils/wfPace';
import { gapArrival, gapDraw, gapLogOn, gapPacerSlip } from '../utils/frameGapLog';

// ── Layout constants (vibeWaterfall.ts v1.5) ──────────────────────────────────

const BAND_H   = 20;   // band plan strip height
const TICK_H   = 22;   // frequency ticker height

// Band type → colour. Indices match v1.5 BAND_COLS: ham=red, broadcast=blue,
// utility=green, cb=orange. (Screenshot reference: 40m Ham red, 41m B/C blue.)
// Derived from BAND_HEX (bandPlan.ts) so the phone's band plan and the WATCH's band
// label can never drift apart. Same values as before: ham #CF0000, broadcast #0900FF,
// utility #07BD00, cb #FF7700, all at 0.92.
// The ring's depth (history frames). ★ Reallocating it on a size change WIPES the ring, and the waterfall persisting
// through a rotation/resize with NO redraw is a hero feature (the Mac app resizes fluidly where other SDR apps blank and
// rebuild). The DISPLAY mapping (how many rows fill the view height, 1 row per point) is the separate `uVisibleRows`
// uniform, so orientation changes the mapping — never the history.
// ★★★ GROW-ONLY SINCE 2026-10-30 (utils/wfRingSize.ts; Stuart: "the history only goes when it genuinely goes off
//     screen"): it starts at RING_ROWS (1024, as it always was) and grows, capped by the device's RAM, whenever the view
//     is taller than it holds — every row and its per-row geometry COPIED into the new ring (utils/wfRingGrow.ts), so the
//     rule above still holds: nothing is lost, nothing is redrawn from scratch, nothing ever shrinks. The live depth is
//     ringRows (a ref) and, for the shader, uRowsSv (set with the image it belongs to).
const RING_ROWS = RING_MIN_ROWS;
/** Installed on the JS runtime by the react-native-skia patch (VibeExport.cpp): a Gray_8 raster image made
 *  WITHOUT Hermes external memory pressure. Absent on web / an unpatched binary. See publishRing. */
type VibeMakeGray8Image = (bytes: Uint8Array, width: number, height: number, rowBytes: number) => SkImage | null;
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
  /** ★★★ THE VISUAL TRIM, ADDED BACK FOR THE AXIS LABELS ONLY.
   *
   *  The trim is applied by SHIFTING THE RANGE handed to the renderer — mapping a bin through
   *  `(bin - (dbMin - vg)) / span` is arithmetically identical to shifting every bin by +vg, and
   *  costs nothing per frame. But the labels are drawn from that same shifted range, so without
   *  this they would move with it and the picture would appear unchanged: the signal and its scale
   *  sliding together is exactly the illusion of doing nothing.
   *  ★ So the labels add the trim back, and the signal moves against a FIXED axis — which is what
   *    "incoming signal indicates -50db, visual gain -20db, signal now shows -70db" means. */
  dbOffset?:   number;
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
  /** ★ Minimum dB span of the auto range; DAB passes 15 (see SignalProcessorSettings.minRangeDb). */
  minRangeDb?:     number;
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
  wfScroll?:       WfScroll;   // ★ V12 'eco' = 10 rows/s, 'low' = 5, 'min2' (MINIMUM) = 2 — actual rows at that feed, else thinned/filled — wfRowTarget ROW_RULE
  /** ★★ THE SLOWEST FRAME RATE THIS BACKEND EVER DELIVERS — what the fixed multiplier is sized
   *  against. It is a property of the SERVER, not a measurement, so it is known at connect and
   *  never moves: UberSDR bottoms out at 3.3 fps (→ 6x for a 20 rows/s scroll), VibeServer at
   *  4-5 (→ 5x). Sizing off the floor rather than the live rate is what keeps uN constant, and a
   *  constant uN is the only thing that stops the waterfall rescaling (Stuart, 2026-08-03). */
  feedFloorFps?:   number;
  needleColor?:    string;        // VFO colour — needle, sidebands, peak hold
  /** ★★ V12 VFO A/B: the INACTIVE VFO — a second needle + passband in its own colour, slightly dimmer (the active one
   *  is the ordinary needle above), labelled with its letter and the ear it is heard in (null = both / stereo off). */
  secondVfo?: { hz: number; filterLow: number; filterHigh: number; letter: string; ear: string | null;
                color: string; parked?: boolean; dec?: boolean } | null;
  /** ★★ V12 VFO A/B: the ACTIVE VFO's letter and ear while A/B is on — labelled beside the main needle. `dec` (both):
   *  this VFO is decoding — "DEC" after its letter (decode on either VFO, 2026-10-27). */
  activeVfo?: { letter: string; ear: string | null; dec?: boolean } | null;
  /** ★ V12 ISM: no LISTEN needle, passband or label (the web's wf.vfoHidden) — a band capture has nothing to listen to. */
  needleHidden?:   boolean;
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
  /** ★ How far in from the right the receiver's name sits. The session clock below it is positioned against
   *  the SCREEN, this block against the waterfall, and two different numbers put them visibly out of line
   *  (Stuart, 2026-09-20: "the clock seems to be slightly offset which doesnt look professional"). */
  stationIdRight?: number;
  /** ★ The left limit for the receiver's name (the Servers chip's right edge + a gap): past it the lines end in
   *  "…" rather than run under the chip (2026-10-16, an iPhone 12). Absent = no limit. */
  stationIdLeft?: number;
  /** ★ How tall this overlay ended up, so the parent can put the session countdown UNDER it
   *  instead of on top of it. Measured rather than assumed: one line and two lines differ, and a
   *  constant here would have to be kept in step with a font size nobody would think to check. */
  onStationIdHeight?: (h: number) => void;
  // Smooth tune (variable refresh): 120Hz interpolated scroll while the user
  // is interacting; once settled the waterfall steps rows discretely (data is
  // ~10Hz — the slide is pure interpolation) and the spectrum trace eases at
  // ~30fps, so ProMotion can drop the panel rate and save battery.
  smoothTune?:     boolean;
  /** ★★ V12 2026-10-30 "SPEED UP WHILE TUNING" (default on): false = the interaction boost never raises the row rate
   *  (wfClock.ts boostRowRate `enabled`; the Android engine's EngineConfig.boost). */
  wfBoost?:        boolean;
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
  /** ★★ V12 beta25: the radio's DC spike (Hz; 0 = none) — a small red "DC Spike / Not a real signal" label at it. The
   *  caller decides when (FREE tuning on a radio with a real spike — SDRScreen dcSpikeShown). */
  dcSpikeHz?: number;
  /** ★★ KNOWN-SIGNAL LABELS (utils/knownSignals.ts): the sorted list in force (the curated table + EiBi's on-air utility
   *  entries), or null/empty = off. The caller (SDRScreen) owns the toggle; nothing is laid out or drawn while it is off. */
  knownSignals?: readonly KnownSignal[] | null;
  /** ★★ A TAP ON A LABEL'S BOX (Stuart, 2026-10-30: "can they be tapped to tune and chose the decoder?") — the caller
   *  tunes to the signal's dial and opens its decoder (SDRScreen onKnownTap). Only a TAP: a drag starting on a label pans
   *  as ever. Absent = the labels are not tappable (and not screen-nav stops). */
  onKnownTap?: (sig: KnownSignal) => void;
  /**
   * ★★ THE OPAQUE CONTROL DECK, in WINDOW coordinates (x, y, w, h, corner radius) — or null when nothing opaque
   * covers the waterfall (Transparency on, the deck hidden, mid-rotation). The waterfall shader is then drawn only
   * where it can be SEEN: the rows above the deck, the strips beside it, the gap below it. On the XCover 4s the GPU
   * was the limit (busy 70–96 %, one 720×814 waterfall draw ≈ 11.7 ms of a 16.7 ms frame) and in portrait the deck
   * hides about half the canvas — shading pixels nobody sees was half the cost. See docs/NATIVE-SPECTRUM-ENGINE.md.
   */
  deckOcclusion?: { x: number; y: number; w: number; h: number; r: number } | null;
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
uniform float uRows;         // ring rows (the history depth — grows, never shrinks, with the image: utils/wfRingSize.ts)
uniform float uVisibleRows;  // rows mapped across the view height (each a whole number of device px — devRowPx)
uniform float uTexW;     // bins
uniform float uDrawW;    // draw width (screen px)
uniform float uDrawH;    // draw height incl. overscan row (screen px)
uniform float uSharp;    // unsharp amount (0..~1.2)
uniform float uContrast; // -1..1 S-curve mix
uniform float uMoLo;     // ★ MOTION (utils/wfRowPlace.ts): absolute frames f whose pair f -> f+1 moves are uMoLo..uMoHi
uniform float uMoHi;     //   (lo > hi = none). Only rows inside it read the motion rows below the ring.
uniform float uRowPx;    // ring rows per DEVICE pixel line (a pixel's footprint, in rows)
uniform float uColPx;    // device pixels per ring column
uniform float uZA;       // ★ THE SCREEN EASE (utils/wfPace.ts): image column (0..1) shown at screen x (0..1) = uZA*x + uZB;
uniform float uZB;       //   (1, 0) at rest. Outside the image = the floor (the edge the radio has not captured yet).

// The ring at column x (0..uTexW) of ring row r — the floor outside it (a vacated edge, never a smear of the edge bin).
float px(float x, float r) {
  if (x < 0.0 || x > uTexW) { return 0.0; }
  return wf.eval(float2(clamp(x, 0.5, uTexW - 0.5), r)).r;
}

// Temporal interpolation (phase 2): the ring holds RAW frames; intermediate
// lines are synthesized here by blending adjacent frames at fractional depth
// — identical maths to the old JS line ticker, per-pixel, for free.
// Line algebra: with frame K = uHeadF-1 newest, lines (K-1)*uN+1..K*uN blend
// frame[K-1]->frame[K]; revealed lines this interval R = uFrac*uN; absolute
// line at display row L is A = (uHeadF-2)*uN + R - L; its source frames are
// f = floor(A/uN) and f+1 blended by frac(A/uN).
half4 main(float2 xy) {
  float Lc = xy.y / uDrawH * uVisibleRows;     // display line (whole device px per row — devRowPx) — decoupled from ring
  float L  = uQuant > 0.5 ? floor(Lc) : Lc;    // whole-pixel rows when settled
  float R  = uFrac * uN;
  if (uQuant > 0.5) { R = floor(R + 0.0001); }
  float A  = (uHeadF - 2.0) * uN + R - L;
  float fI = floor(A / uN);
  float t  = A / uN - fI;
  // ★★★ WHOLE ROWS WHEN SETTLED — NEVER AN AVERAGE OF TWO MOMENTS.
  //     uQuant already snapped the LINE position (L and R above), but NOT this blend: A is an
  //     integer while A/uN is not, so every synthesised line between two source frames was a
  //     per-pixel MIX of both. At uN=5 or 6 (the DEFAULT and SMOOTH scroll settings) that means
  //     most rows on screen were averages — which is vertical blur, and it is exactly the
  //     "brighter and softer" the app showed beside the web client.
  //     Snapping t to the NEAREST source frame is precisely what the web client does on the CPU
  //     (blend.set(t >= 0.5 ? cur : prev) in waterfall.ts), so every settled row is a real
  //     measurement. The scroll RATE is untouched — only what each row contains.
  //   ★ A NOTE IN THE WEB CLIENT CLAIMS THE APP ALREADY DID THIS ("what the app actually shows is
  //     raw frames — no blend"). It did not: uQuant quantises position, not the frame mix. That
  //     comment is wrong and has been corrected there.
  //   ★ Boost (uQuant=0) keeps the continuous blend — during a tune/zoom the glide is the point,
  //     and a stepped one would read as judder.
  if (uQuant > 0.5) { t = step(0.5, t); }
  float r1 = mod(fI, uRows) + 0.5;
  float r2 = mod(fI + 1.0, uRows) + 0.5;
  float tx = (uZA * (xy.x / uDrawW) + uZB) * uTexW;   // the ease is (1, 0) at rest: exactly the old column
  // ★★ MOTION-COMPENSATED INTERPOLATION (2026-10-28 — Stuart: "the chunky broken diagonal lines joined up so they looked
  //    solid", the first build's MW sweep). Each ring slot carries h: how many columns its trail moves to the next newer
  //    row (wfRowPlace.ts MOTION; 16 bits in two image planes below the ring, one column a slot — see the read below).
  //    A line between frames f and f+1 at progress t samples f at x - t*h and f+1 at x + (1-t)*h: the WHOLE row (the
  //    noise with the stations) slides between data rows instead of cross-fading two places. Settled, the ONE frame a
  //    row shows is warped the same way toward the row above it (a real measurement, displaced, never a blend), so each
  //    row's top meets the next row's place. Taps across the pixel's own footprint (one per device px it crosses, up to 6) fill a fast move; where one
  //    tap finds a station the pixel takes it (a solid trail), noise is averaged (no brightened floor).
  //  ★ Steady rows (h = 0, or outside uMoLo..uMoHi — no fetch at all) take the unchanged paths below.
  float h = 0.0;
  float g = 1.0;   // ★ the ZOOM to the next row (utils/wfRowPlace.ts pairMotion): x_b = m + g·(x_a − m) + h, m = the centre
  if (fI >= uMoLo && fI <= uMoHi && (uQuant < 0.5 || t == 0.0)) {
    // ★★ The planes (utils/wfRowPlace.ts MOTION_ROWS / motionAt): each is ceil(uRows/uTexW) image rows, slot s at column
    //    s mod uTexW of the plane's row floor(s / uTexW) — for a ring of up to 1024 rows exactly the old layout.
    float ms = mod(fI, uRows);
    float mk = ceil(uRows / uTexW);
    float mc = mod(ms, uTexW) + 0.5;
    float my = uRows + floor(ms / uTexW) + 0.5;
    float hb = wf.eval(float2(mc, my)).r;
    float lb = wf.eval(float2(mc, my + mk)).r;
    h = (floor(hb * 255.0 + 0.5) * 256.0 + floor(lb * 255.0 + 0.5) - 32768.0) / 64.0;
    float gh = wf.eval(float2(mc, my + 2.0 * mk)).r;
    float gl = wf.eval(float2(mc, my + 3.0 * mk)).r;
    g = exp2((floor(gh * 255.0 + 0.5) * 256.0 + floor(gl * 255.0 + 0.5) - 32768.0) / 4096.0);
  }
  float mid = uTexW * 0.5;
  float c, l, r;
  if (tx < 0.0 || tx > uTexW) {
    c = 0.0;                                   // the screen ease: beyond the history's span
  } else if (h != 0.0 || g != 1.0) {
    float tc = uQuant > 0.5 ? 1.0 - (Lc - L) : t;   // progress f -> f+1 at this pixel's centre
    // The move AT THIS PIXEL: h at the centre, plus the zoom's spread away from it (the fan).
    float xa0 = mid + ((tx - mid) - tc * h) / (1.0 + tc * (g - 1.0));
    float mv = (g - 1.0) * (xa0 - mid) + h;
    float K  = clamp(ceil(abs(mv) * uRowPx * uColPx), 1.0, 6.0);   // one tap per device px the trail crosses in this line
    float acc = 0.0;
    float mx  = 0.0;
    for (int k = 0; k < 6; k++) {
      if (float(k) < K) {
        float tk = clamp(tc + ((float(k) + 0.5) / K - 0.5) * uRowPx, 0.0, 1.0);
        float xa = mid + ((tx - mid) - tk * h) / (1.0 + tk * (g - 1.0));
        float xb = mid + g * (xa - mid) + h;
        // ★ At the picture's edge one frame may have no data where the other does (a pan brings new spectrum in from
        //   the side): take the frame that HAS it, never the floor — else the edge shows a black comb mid-glide.
        bool ina = xa >= 0.0 && xa <= uTexW;
        bool inb = xb >= 0.0 && xb <= uTexW;
        float v = ina ? px(xa, r1) : px(xb, r2);
        if (uQuant < 0.5 && ina && inb) { v = mix(v, px(xb, r2), tk); }
        acc += v;
        mx = max(mx, v);
      }
    }
    float mean = acc / K;
    c = mix(mean, mx, smoothstep(0.03, 0.10, mx - mean));
    if (uSharp > 0.0 && K < 1.5) {
      float xa = xa0;
      float xb = mid + g * (xa - mid) + h;
      l = px(xa - 1.0, r1);
      r = px(xa + 1.0, r1);
      if (uQuant < 0.5) { l = mix(l, px(xb - 1.0, r2), tc); r = mix(r, px(xb + 1.0, r2), tc); }
      c = c + uSharp * (c - (l + r) * 0.5);
    }
  } else {
    tx = clamp(tx, 0.5, uTexW - 0.5);
    float xl = max(tx - 1.0, 0.5);
    float xr = min(tx + 1.0, uTexW - 0.5);
    if (t == 0.0 || t == 1.0) {
      // ★★ ONE ROW, NOT TWO (2026-10-12, XCover 4s: GPU 78-96 % busy at 343 MHz and the waterfall updating far less
      //    often than the UI). Settled, the blend is snapped, so a pixel shows ONE frame — but both frames were still
      //    fetched (3 each for the unsharp) and mixed with a weight of 0 or 1. Fetch only the one shown: 7 texture reads
      //    a pixel become 4, the picture identical. The tuning glide (t between 0 and 1) still blends, as before.
      float rr = t == 0.0 ? r1 : r2;
      c = wf.eval(float2(tx, rr)).r;
      if (uSharp > 0.0) {
        l = wf.eval(float2(xl, rr)).r;
        r = wf.eval(float2(xr, rr)).r;
        c = c + uSharp * (c - (l + r) * 0.5);
      }
    } else {
      c = mix(wf.eval(float2(tx, r1)).r, wf.eval(float2(tx, r2)).r, t);
      if (uSharp > 0.0) {
        l = mix(wf.eval(float2(xl, r1)).r, wf.eval(float2(xl, r2)).r, t);
        r = mix(wf.eval(float2(xr, r1)).r, wf.eval(float2(xr, r2)).r, t);
        c = c + uSharp * (c - (l + r) * 0.5);
      }
    }
  }
  // ★★ OLDER THAN THE RING = THE FLOOR (2026-10-30, the growing ring — utils/wfRingSize.ts). A row the ring no longer
  //    holds is never drawn (it would wrap to a NEWER row). At rest the view is always inside the ring (devRowPx, the
  //    margin) and this never fires; it covers the frame or two after a resize while the grown ring's image is on its
  //    way — the bottom rows show what the grown ring will show there anyway: no history, so no jump and no blank.
  if (fI + uRows < uHeadF + 8.0) { c = 0.0; }
  float raw = clamp(c, 0.0, 1.0);
  float s   = raw * raw * (3.0 - 2.0 * raw);
  float v   = uContrast > 0.0
    ? mix(raw, s, uContrast)
    : mix(raw, raw * 0.5 + 0.25, -uContrast);
  return lut.eval(float2(clamp(v, 0.0, 1.0) * 255.0 + 0.5, 0.5));
}`;
const WF_EFFECT = Skia.RuntimeEffect.Make(WF_SKSL);

/** The native spectrum engine's host object (cpp/spectrum/spectrum_jsi.cpp). JS calls the setters and push; the UI
 *  thread calls poll. */
interface SpectrumEngineHost {
  push(bins: Float32Array, centerHz: number, bwHz: number, trueCenterHz: number | undefined, lastInteractAt: number,
       trueBwHz?: number): void;   // ★ the bins' real span (an older binary ignores it)
  setView(centerHz: number, bwHz: number): void;
  /** ★ V12 a full-capture row (SPWB, utils/wfWide.ts) the JS socket received — the engine keeps the layer (wf_wide.h).
   *  Optional: a binary built before it has none, and then the row is simply not drawn. */
  pushWide?(frame: Uint8Array): void;
  /** ★★ V12 2026-10-30 the audio's playout delay, ms (−1 = unknown) — the engine's clock waits for the sound
   *  (cpp/spectrum/wf_av_sync.h). Optional: a binary built before it holds nothing. */
  setAudioDelay?(ms: number): void;
  setConfig(c: { width: number; wfTop: number; specH: number; specShow: boolean; peakHold: boolean;
                 smoothTune: boolean; targetRows: number; displayCapHz?: number; panelHz?: number;
                 /** ★★ the scale stamps (cpp/spectrum/wf_stamps.h): rows on screen, fewest rows between two */
                 stampViewRows?: number; stampMinGapRows?: number;
                 /** ★★ the ring depth to grow to (utils/wfRingSize.ts; grow-only — an older binary ignores it and stays 1024) */
                 ringRows?: number }): void;
  setSettings(s: Record<string, number | boolean>): void;
  setBackground(bg: boolean): void;
  setCovered(c: boolean): void;
  /** ★★ the scale row `frame` is displayed at, [lo Hz, span Hz], or null (not in the ring) — a tap on an old row tunes by
   *  it (#57). Optional: an older binary has none, and then the live scale is used. */
  rowGeom?(frame: number): [number, number] | null;
  liveRange(): [number, number] | null;
  poll(): {
    image?: SkImage; head: number; texReady: boolean; quant: number; avgMs: number;
    /** ★ V12 the fixed output clock's rate, rows/s (0 = NATIVE / actual rows); absent from a binary built before it. */
    rowRate?: number;
    scrollGen: number; glide: boolean; glideDur: number;
    /** The motion range of this image (absolute frames) and the fans' screen move since the last image (x_old = c + k·x). */
    moLo?: number; moHi?: number; fanK?: number; fanC?: number;
    /** ★★ the ring depth of THIS image (the shader's uRows) — absent from an older binary: 1024. */
    ringRows?: number;
    specGen: number; specHidden: boolean; spec?: number[];
    peakGen: number; peak?: SkPath;
    /** ★★ the scale stamps (wf_stamps.h): stampGen changes when they do; `stamps` comes with the change (absent from an
     *  older binary — then there are none). */
    stampGen?: number; stamps?: WfStamp[];
  } | null;
}

// ── SCALE STAMPS (utils/wfStamps.ts) ──────────────────────────────────────────
/* ★★★ Stuart, 2026-10-30 (#57): "zoom in and a stamp of the previous frequency scale as it was is added onto the old
 *     waterfall position and it gently drops off the bottom of the screen, same with tuning too if needed" — and "a
 *     simple label at the old zoom/tune/pan position then the diagonals then the bandplan/frequency ticker at the top is
 *     then the current". A thin ruler of the old scale (the ticker's own step and units) laid on the history at the top
 *     of the rows it describes; it scrolls with them and fades off the bottom. An OVERLAY in the waterfall canvas only —
 *     no row, uniform or clock is touched.
 *  ★ COLOUR: the user's VFO colour with a dark shadow ("an opposite colour to the waterfall … i'd simply use orange like
 *    my VFO"), white if that clashes with the palette (stampColour).
 *  ★ COST: each stamp's ticks and text are built ONCE (its own memo); per display frame only its translate and opacity
 *    move, worked out on the UI thread from the same uniforms that scroll the picture (stampLine) — so it never drifts
 *    from its rows, through a resize or rotation too. No stamps = this renders nothing. At most STAMP_MAX (3).
 *  ★ A LABEL LAYER, not a stamp-only one: an item is an anchor row + the view that row was drawn at (lo, span), so x is
 *    (hz − lo) / span × width and y follows the row — a known-signal label (RTTY/WEFAX/FT8) is the same item with a
 *    frequency and text instead of a ruler. */
const STAMP_BAND = 13;          // pt — the faint band's height (the label is ~9.5 pt)
const STAMP_FONT_PT = 9.5;
const ATKINSON_TTF = require('../../assets/fonts/AtkinsonHyperlegible-Regular.ttf');

type StampSink = React.MutableRefObject<((s: WfStamp[]) => void) | null>;

const WfStampItem = React.memo(function WfStampItem({ s, width, rowPt, wfH, font, color, uHead, scrollFrac, uN, uQuant }: {
  s: WfStamp; width: number; rowPt: number; wfH: number; font: SkFont; color: string;
  uHead: SharedValue<number>; scrollFrac: SharedValue<number>; uN: SharedValue<number>; uQuant: SharedValue<number>;
}) {
  // Built once per stamp (and per width / font): the ticks, the labels and where each sits.
  const els = useMemo(() => {
    const ticks = stampTicks(s.lo, s.span, width, (sp) => {
      const target = Math.max(4, Math.min(8, Math.floor(width / 70)));
      let step = niceTick(sp / target);
      while (step * width / sp < 52) step *= 2;
      return step;
    }, (hz) => fmtHz(hz), 52);
    // ★ WHEN, at the left end (Stuart, 2026-10-30): HH:MM:SS the scale changed; frequency labels keep clear of it.
    const time = s.at ? stampTimeLabel(s.at) : null;
    const timeEnd = time ? 3 + font.measureText(time).width + 8 : 0;
    const out = ticks.map((t, i) => {
      const w = t.label ? font.measureText(t.label).width : 0;
      const lx = Math.max(2, Math.min(width - w - 2, t.x - w / 2));
      return { key: i, x: t.x, label: lx < timeEnd ? null : t.label, lx };
    });
    if (time) out.push({ key: -1, x: -10, label: time, lx: 3 });   // x off-screen: no tick for the time
    return out;
  }, [s.lo, s.span, s.at, width, font]);
  // ★ Never a NaN or an infinity into the canvas: an unusable position is off-screen and invisible.
  const tr = useDerivedValue(() => {
    const y = stampLine(uHead.value, s.frame, scrollFrac.value, uN.value, uQuant.value > 0.5) * rowPt;
    return [{ translateY: Number.isFinite(y) ? Math.max(-4 * STAMP_BAND, Math.min(wfH + STAMP_BAND, y)) : -4 * STAMP_BAND }];
  }, [rowPt, wfH, s.frame]);
  const op = useDerivedValue(() => {
    const y = stampLine(uHead.value, s.frame, scrollFrac.value, uN.value, uQuant.value > 0.5) * rowPt;
    const a = stampAlpha(y, STAMP_BAND, wfH, uHead.value - 1 - s.born);
    return Number.isFinite(a) ? Math.max(0, Math.min(1, a)) : 0;
  }, [rowPt, wfH, s.frame, s.born]);
  const ty = 4 + STAMP_FONT_PT;
  return (
    <Group transform={tr} opacity={op}>
      <Rect x={0} y={0} width={width} height={STAMP_BAND} color="rgba(0,0,0,0.55)" />
      <Rect x={0} y={0} width={width} height={0.75} color={hexRgba(color, 0.55)} />
      {els.map((e) => <Rect key={'t' + e.key} x={e.x - 0.5} y={0} width={1} height={3.5} color={hexRgba(color, 0.85)} />)}
      {els.map((e) => e.label ? <SkText key={'s' + e.key} x={e.lx} y={ty + 1} text={e.label} font={font} color="rgba(0,0,0,0.95)" /> : null)}
      {els.map((e) => e.label ? <SkText key={'l' + e.key} x={e.lx} y={ty} text={e.label} font={font} color={color} /> : null)}
    </Group>
  );
});

/** ★ A failure inside the stamps hides THE STAMPS — never the spectrum panel around them (PanelBoundary would close it).
 *  Noted in Diagnostics as a contained fault. */
class StampBoundary extends React.Component<{ children: React.ReactNode }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  componentDidCatch(e: unknown) { noteFault('waterfall', 'stamps', e); }
  render() { return this.state.failed ? null : this.props.children; }
}

const WfStampLayer = React.memo(function WfStampLayer({ sink, width, rowPt, wfH, color, top, uHead, scrollFrac, uN, uQuant }: {
  sink: StampSink; width: number; rowPt: number; wfH: number; color: string; top: number;
  uHead: SharedValue<number>; scrollFrac: SharedValue<number>; uN: SharedValue<number>; uQuant: SharedValue<number>;
}) {
  const [stamps, setStamps] = useState<WfStamp[]>([]);
  useEffect(() => { sink.current = setStamps; return () => { sink.current = null; }; }, [sink]);
  const typeface = useTypeface(ATKINSON_TTF);
  // ★★ NO setEmbolden: on the iOS/Mac build of Skia it throws "Value is true, expected a number" (Stuart's 532
  //   diagnostics, 2026-10-30) — in 531 that throw, inside the waterfall's own canvas, left the waterfall black at rest;
  //   in 532 it closed the spectrum panel. The dark shadow under each label gives it the weight instead. Never throws:
  //   no font = no stamps, never no waterfall.
  const font = useMemo(() => {
    if (!typeface) return null;
    try { return Skia.Font(typeface, STAMP_FONT_PT); } catch { return null; }
  }, [typeface]);
  if (!stamps.length || !font || !(rowPt > 0) || !(wfH > 0)) return null;
  return (
    <Canvas pointerEvents="none" style={{ position: 'absolute', left: 0, top, width, height: wfH }}>
      {stampsShown(stamps, rowPt, STAMP_BAND).map((s) => (
        <WfStampItem key={s.id} s={s} width={width} rowPt={rowPt} wfH={wfH} font={font} color={color}
                     uHead={uHead} scrollFrac={scrollFrac} uN={uN} uQuant={uQuant} />
      ))}
    </Canvas>
  );
});

// ── Component ─────────────────────────────────────────────────────────────────

function WaterfallView({
  frameSink, binCount, centerHz, bwHz, tuneHz,
  filterLow = -3000, filterHigh = 3000,
  dbMin = -120, dbMax = -20, dbOffset = 0, wfCoarse = 'auto',
  colormap = 'gqrx', width, height, bottomGuard = 0,
  ituRegion = 1, fontFamily = 'Atkinson Hyperlegible',
  onPanDelta, onZoomDelta, onTapTune, onPinchZoom,
  specShow = true, specFrac = 0.26,
  autoContrast = 5, specSmoothing = 5, avgFrames = 0, specFloor = 0, specPeakScale = 10,
  peakHold = true, spatialSmooth = true,
  wfBrightness = 0, wfContrast = 0, wfSharpness = 0, minRangeDb = 30,
  frameRate = '20fps', wfScroll = 'sharp', feedFloorFps = 3.3, needleColor = '#ff2020', needleIntensity = 5, needleFrost = 0,
  needleHidden = false, secondVfo = null, activeVfo = null,
  bgImageUrl = null, bgOpacity = 0, stationId = null, onStationIdHeight, stationIdRight = 6, stationIdLeft,
  smoothTune = true, wfBoost: wfBoostOn = true, lastInteractAt,
  panLoHz, panHiHz, showWalls = false,
  centerMarkerHz, showCenterMarker = false, centerMarkerColor = '#36c5f0', dcSpikeHz = 0, knownSignals = null,
  onKnownTap, deckOcclusion = null,
}: WaterfallViewProps) {

  // ── Vertical layout ─────────────────────────────────────────────────────────
  const tickTop  = BAND_H;
  const specTop  = tickTop + TICK_H;
  const below    = Math.max(0, height - specTop);
  const specH    = specShow ? Math.round(below * Math.max(0.05, Math.min(0.65, specFrac))) : 0;
  const wfTop    = specTop + specH;
  const wfH      = height - wfTop;
  // ★★★ WHERE THE RF-CENTRE AND LISTEN LABELS SIT, and it is NOT the top of the spectrum.
  //     They were at specTop+3 and specTop+29 — exactly where the SERVERS CHIP and the receiver
  //     name/limit block live — so on a shared server they overlapped and neither was readable
  //     (Stuart, 2026-08-05, with a screenshot from the web client; the app has the same fault).
  //     ★★ Anchored to the SPECTRUM/WATERFALL BOUNDARY, which is Stuart's own suggestion: it is
  //     the widest reliably-empty strip on the display, it moves when the split moves, and
  //     nothing else is ever drawn there.
  //     ★ Spectrum off ⇒ no boundary to hang them on, so a fixed drop that clears the chip.
  //     ★★ KEEP THIS IN STEP WITH web/client/src/waterfall.ts markerLabelY(). Two clients drawing
  //        the same two labels in different places is exactly the drift this project keeps
  //        writing rules about.
  const markerLabelBase = (specShow && specH > 40) ? wfTop - 16 : specTop + 56;
  // ★ 1 DATA ROW = 1 SCREEN POINT. The ring depth tracks the waterfall's pixel height. A FIXED depth
  // (was 256) stretched to fill a tall PORTRAIT waterfall made each row span ~2 points, so signals
  // blurred and scrolled ~2× too fast vs landscape — present in EVERY version, masked by a high frame
  // rate (Stuart 2026-07-24). Depth = height ⇒ every orientation renders 1-point rows: sharp, and the
  // same points/sec scroll regardless of orientation.
  const rows      = Math.min(1024, Math.max(64, Math.round(wfH)));   // cap the ring memory/copy cost
  const wfRenderH = wfH + Math.ceil(wfH / rows) + 2; // hide bottom-edge judder
  const rowH      = wfRenderH / rows;
  /* ★★ A ROW IS A WHOLE NUMBER OF DEVICE PIXELS (2026-10-13, Galaxy Tab A9: "a little shimmer on the waterfall … UberSDR
   *  also has it in their webclient in GPU mode"). One row per POINT is 1.33 device px on the Tab (213 dpi), so rows came
   *  out 1, 1, 2, 1, 1, 2 … px tall — and as the waterfall scrolls every row changes height as it crosses the pattern:
   *  the shimmer. The Moto (2.125) has the same fault one row in eight, and 2→3 px is a softer change than 1→2; the
   *  XCover (1.75) has it one in four. iOS and the Mac (2× / 3×) never did. So the shader maps a row to round(dpr) device
   *  px (1 on the Tab, 2 on the XCover and Moto) — every row the same height for its whole trip down the screen.
   *  ★ The trade, as rows are no longer exactly a point: the Tab scrolls ~25 % slower in points and shows ~⅓ more
   *    history; the XCover ~14 % faster; the Moto ~6 % slower. More rows than the ring holds would wrap, so a row
   *    grows a pixel before it ever maps past the ring's depth.
   *  ★★★ THE RING GROWS FIRST (2026-10-30, utils/wfRingSize.ts): a view taller than the ring asks for a deeper one (up to
   *    the device's RAM tier — 1024 under 2 GB or unknown, 2048 under 4 GB, 4096 above), and only past THAT does a row
   *    grow a pixel. ringTarget is what the ring is or is about to be (the growth runs in the effect below, between rows). */
  const natRowPx  = Math.max(1, Math.round(PixelRatio.get()));
  const ringRows  = useRef(RING_ROWS);   // the ring's live depth (pushRow, publishRing, the fan) — grows only
  const ringCap   = useMemo(() => ringCapFromDeviceClass(readNativeDeviceClass()), []);
  const ringTarget = ringNextRows(ringRows.current, wfRenderH * PixelRatio.get() / natRowPx, ringCap);
  const devRowPx  = Math.max(natRowPx, Math.ceil(wfRenderH * PixelRatio.get() / (ringTarget - 16)));
  const shaderRows = wfRenderH * PixelRatio.get() / devRowPx;   // uVisibleRows: rows across uDrawH, each devRowPx tall
  // ★★ The shader's pixel footprint, computed HERE on the JS thread: wfUniforms is a UI-thread worklet and PixelRatio.get()
  //   is not callable there — calling it inside threw on the first frame and crashed V12 beta27 on connect.
  const uRowPxV = shaderRows / Math.max(1, wfRenderH * PixelRatio.get());   // = 1 / devRowPx
  const uColPxV = width * PixelRatio.get() / RING_W;

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
  // ★★★ 20 / 30 / 40 ROWS PER SECOND (Smooth is 60 since 2026-10-13 — below). Raised from 20/30 because on a feed at or above the target
  //     the app cannot express a SLOWER setting: its rows are one POINT tall, so "one row per
  //     frame" already IS 20 rows/s, and SHARP and DEFAULT collapsed together. Making the other
  //     two FASTER is what users actually asked for ("the rate in the app is too slow", Stuart
  //     2026-08-04) and needs no change to how a row is drawn.
  //     ★ The web client gets three distinct speeds a different way — its rows are one DEVICE
  //       pixel tall and its rate is speed x dpr, so SHARP there is genuinely slower. Matching
  //       that means a device-resolution ring, which needs the full-ring upload fixed first.
  // ★★ SMOOTH IS 60, NOT 40 (Stuart, 2026-10-13: "smooth is at 60 and is working so keep it"). 40 rows/s divides evenly
  //   into no drawing rate — ⅔ of a row per frame at 60, 1⅓ at 30: a regular judder (Galaxy Tab A9, measured 2·1·2·1).
  //   60 is one row a frame at 60, two at 30, one every other at 120; Default's 30 is one at 30, one every other at 60.
  // ★★★ DEFAULT RUNS AT 30, NEVER 20 (Stuart, 2026-10-29, the Mac on a 10 fps UberSDR: "its the 20 that is causing the
  //     frame pacing issues"; "30fps here would work well"; Native 10 and Smooth 60 were perfect). wfRowTarget.ts: Default's
  //     rates are 10 / 15 / 30 (3 × the feed, rounded UP). The cap below stays 20 — it is the code the engine knows as
  //     Default (EngineConfig::targetRows), not the rate. The 10-20 note that follows is superseded:
  // ★★ DEFAULT IS 20, NOT 30 (Stuart, 2026-10-20): with 10 fps the server default, 20 and 60 divide evenly
  //   into 5/10/20 fps feeds and 60/120 Hz displays; 30 gave x1.5 alternating repeats on a 20 fps feed
  //   (1, 2, 1, 2 rows per frame). The web client now uses these same three (web/client/src/main.ts WF_PRESET).
  // ★★★ AND THESE ARE NOW CAPS (Stuart 10-20): the rows aimed for follow the measured feed — min(cap, 3x / 6x the feed),
  //     snapped to a divisor of 60 (src/assets/wfRowTarget.ts; handleFrame / spectrum_engine.cpp apply it).
  const WF_TARGET_ROWS = wfScroll === 'smooth' ? 60 : wfScroll === 'default' ? 20 : wfScroll === 'eco' ? 10 : wfScroll === 'low' ? 5 : wfScroll === 'min2' ? 2 : 0;
  // ★★ WF_ROWS (a lines-per-frame figure LATCHED from the measured feed rate) is GONE
  //     (2026-10-03). Since uN was pinned to 1 nothing read it — it only fed `cfg.rowsPerFrame`,
  //     which no code read either. Its latch never latched (handleFrame is a []-deps callback and
  //     saw the first render's null forever), so after 25 frames it called setState EVERY frame
  //     and re-rendered this whole view whenever the rounded value flipped. The row count a slow
  //     feed needs is decided by `targetRows` + rowCarry in handleFrame.
  /** ★★★ NO GATE ANY MORE — EVERY DATA FRAME BECOMES A ROW.
   *  The gate existed only because uN had to be constant: holding rows/sec fixed with a fixed
   *  multiplier meant discarding source frames on a fast feed (a 200 ms gate at 20 fps threw away
   *  FOUR FRAMES IN FIVE). uN is now pinned to 1 and the multiplier only decides how many rows to
   *  PUSH, so there is nothing left to protect and no reason to bin data the server already sent.
   *  ★ That discard was the whole of the "chunky, blurry, lacks detail" against the web client. */
  const WF_MIN_GAP_MS = 0;
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
  /** Each ring slot's placement + the move to the next row (utils/wfRowPlace.ts MOTION) — allocated once, ~22 KB. */
  const motionSt = useRef<ReturnType<typeof newMotionState> | null>(null);
  if (!motionSt.current) motionSt.current = newMotionState(ringRows.current, RING_W);   // ★ lazily: not per render
  /** THE ZOOM FAN (utils/wfRowPlace.ts): per-slot maps + the displayed ring, made only once a zoom happens. */
  const fanSt = useRef<ReturnType<typeof newFanState> | null>(null);
  if (!fanSt.current) fanSt.current = newFanState(ringRows.current);
  // ★★ SCALE STAMPS (utils/wfStamps.ts): the detector (JS path) and the layer's setter; stampCfg = what pushRow needs.
  const stampSt = useRef(newStampState());
  const stampSink: StampSink = useRef(null);
  const stampCfg = useRef({ minGapRows: 45, sweepRows: 0, keepRows: ringRows.current });
  /** Ring rows that can be on screen (the shader's visible depth, + its blend row and margin): a zoom re-makes only these. */
  const fanVisible = useRef(ringRows.current);
  fanVisible.current = Math.min(ringTarget, Math.ceil(shaderRows) + 4);
  const stampRowPt = shaderRows > 0 ? wfRenderH / shaderRows : 1;   // one ring row's height on screen, pt
  stampCfg.current.minGapRows = Math.ceil(STAMP_MIN_GAP_LABELS * STAMP_BAND / stampRowPt);
  stampCfg.current.keepRows = Math.min(ringTarget - 8, Math.ceil(shaderRows) + 8);
  stampCfg.current.sweepRows = stampSweepRows(fanSt.current!.rowRate, shaderRows);
  const proc = useRef(new SignalProcessor());
  /** ★★ V12 THE FULL-CAPTURE LAYER (utils/wfWide.ts; docs/v12/WATERFALL-FULL-CAPTURE.md): the newest picture of the whole
   *  capture (null until a VibeServer that sends SPWB has sent one — an older server allocates nothing), the detailed
   *  rows' median (its floor match) and the last row's placement (its k and m). The layer ring itself is fanSt.layer. */
  const wideSt = useRef<WideState | null>(null);
  const detP50 = useRef(NaN);
  const p50Scratch = useRef({ buf: new Float32Array(0) });
  const placedRef = useRef({ startHz: NaN, spanHz: 0 });
  /* ★★★ THE NATIVE SPECTRUM ENGINE (Android; docs/NATIVE-SPECTRUM-ENGINE.md, cpp/spectrum). On the Galaxy XCover 4s the
   *  waterfall "renders smoothly then crawls for a bit" (Stuart, 2026-10-12): every row came through the JS thread —
   *  jitter buffer, SignalProcessor, ring, a 1 MiB image per row, repeat rows on setTimeout — so whenever JS was busy
   *  (React, GC, DAB updates) rows arrived late and then in a burst. With the engine, all of that runs on its own
   *  native thread: JS hands each frame over in ONE call and forwards the state it owns, and one UI-thread callback
   *  picks up the finished image, trace and peak (enginePoll below). The JS path is untouched and remains for iOS and
   *  as the fallback when the engine cannot load. */
  const engine = useMemo<SpectrumEngineHost | null>(() => {
    if (Platform.OS !== 'android') return null;
    try {
      const g = globalThis as unknown as { __vibeSpectrumCreate?: () => SpectrumEngineHost };
      if (!g.__vibeSpectrumCreate) (NativeModules.VibePowerModule as { installSpectrumEngine?: () => boolean } | undefined)?.installSpectrumEngine?.();
      return g.__vibeSpectrumCreate ? g.__vibeSpectrumCreate() : null;
    } catch { return null; }
  }, []);
  useEffect(() => {
    // v1 webgl parity: interpolation blur grows with the display rate, so the
    // unsharp base scales with the selected fps and the slider is a multiplier
    // of it (5 = 1×; 60fps base 5 keeps existing setups looking identical).
    const sharpBase =
      // Interpolation blur scales with how many rows are synthesised, so the unsharp base follows
      // the multiplier rather than a frame rate that no longer drives anything.
      wfScroll === 'smooth' ? 3 : wfScroll === 'default' ? 2 : wfScroll === 'eco' ? 1.75 : 1.5;   // minimum = sharp's
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
      minRangeDb,
    };
    proc.current.applySettings(patch);
    engine?.setSettings({
      autoContrast, minRangeDb,
      manual: wfCoarse === 'manual', manualMinDb: dbMin, manualMaxDb: dbMax,
      specFloor, specPeakScale, smoothingFrames: specSmoothing, avgFrames, spatialSmooth, peakHold,
      wfBrightness, wfContrast, wfSharpness: Math.min(12, sharpBase * sharpMul * 2),
    });
  }, [autoContrast, wfCoarse, dbMin, dbMax, specFloor, specPeakScale,
      specSmoothing, avgFrames, spatialSmooth, peakHold, wfBrightness, wfContrast,
      wfSharpness, minRangeDb, frameRate, wfScroll, feedFloorFps]);

  // ── Colormap LUT + derived spectrum colours (9 stops, idx 15→235) ───────────
  const lut = useMemo(() => getColorLUT(colormap), [colormap]);
  const stampCol = useMemo(() => stampColour(needleColor, lut), [needleColor, lut]);
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
  // display-order mapping via uHead). Each push = one row write + a 1 MiB
  // single-channel image (RING_W x RING_ROWS = 1024 x 1024 Gray_8 — vs the old RGBA full
  // rebuild + CPU colourise).
  /** A copy of the newest row, so the stepper can push the extra rows a SLOW feed needs to hold
   *  the chosen scroll rate. frame.row is a reused buffer, hence the copy. */
  /** Fractional row credit carried between arrivals — see the emit note. Without it a target rate
   *  that is not an integer multiple of the feed rounds to the wrong rate every single frame. */
  const rowCarry     = useRef(0);
  const lastRow      = useRef<Uint8Array | null>(null);
  const lastRowMeta  = useRef<{ centerHz: number; hzPerBin: number }>({ centerHz: 0, hzPerBin: 0 });
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
  /** ★★★ THE INTERVAL THE REVEAL MUST BE PACED OVER: the time between accepted WATERFALL ROW-SETS,
   *  which on a gated feed is NOT the data-frame interval. See the note where it is measured. */
  const avgWfGapMs = useRef(0);
  const uQuantSv    = useSharedValue(1); // ASKED: 1 = whole rows (settled), 0 = continuous (a gesture, a slow Sharp feed)
  const uQuantDrawSv = useSharedValue(1); // DRAWN — the display driver's (paceTick): it switches on a whole row
  const frameCount  = useRef(0);
  /** ★ Covered by an opaque full-screen overlay (useScreenCovered): rows go into the ring, but no
   *  texture is made and none of the display-rate drivers run. `ringDirty` = rows arrived meanwhile. */
  const covered     = useScreenCovered();
  const coveredRef  = useRef(covered);
  coveredRef.current = covered;
  const ringDirty   = useRef(false);

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
  /** publishRing → the UI thread, in ONE value: the image, its newest row, its motion range, the fans' screen move. */
  const jsPending = useSharedValue<{ gen: number; img: SkImage | null; avail: number; moLo: number; moHi: number; k: number; c: number; rows: number }>(
    { gen: 0, img: null, avail: 0, moLo: 1, moHi: 0, k: 1, c: 0, rows: RING_ROWS });
  /** ★ The ring depth of the image ON SCREEN (the shader's uRows) — set with the image, never ahead of it. */
  const uRowsSv = useSharedValue(RING_ROWS);
  const jsPendGen = useRef(0);
  const uMoLoSv = useSharedValue(1);     // the motion range (utils/wfRowPlace.ts MOTION) — lo > hi = none
  const uMoHiSv = useSharedValue(0);
  const uZASv   = useSharedValue(1);     // the screen ease (utils/wfPace.ts) — (1, 0) at rest
  const uZBSv   = useSharedValue(0);
  /** Interacting (the smooth-tune window): the display driver draws every frame and holds the pace rate. */
  const interactSv = useSharedValue(false);
  const specPath = useSharedValue<SkPath>(Skia.Path.Make());
  /** ★ ONE persistent empty path for "peak hold off / paused". It was a fresh Skia.Path.Make() on
   *  EVERY data frame while peak hold was off (the common case) — an allocation, a shared-value write
   *  and a 300 ms dispose timer per frame to draw nothing. Never mutated, never disposed by swapPath
   *  (see the guard there), so the UI thread can draw it at any time. */
  const peakEmpty = useMemo(() => Skia.Path.Make(), []);
  const peakPath = useSharedValue<SkPath>(peakEmpty);
  const [liveRange, setLiveRange] = useState({ dbMin: -120, dbMax: -20 });
  // ── Deterministic Skia disposal ─────────────────────────────────────────────
  // Hermes only sees the tiny JS wrappers, NOT the ~1MB native buffer behind
  // each waterfall image — it feels no memory pressure and lets dead images
  // accumulate for ages (measured ~770MB resident, plus constant GC churn).
  // Retire queues delay dispose() by a couple of swaps so the UI thread can
  // never be mid-draw on a freed object.
  // ★ `data` is null for an image from __vibeMakeGray8Image (publishRing): it owns its pixels, there is no
  //   separate SkData host object to free.
  const wfLive    = useRef<{ img: SkImage; data: SkData | null } | null>(null);
  const wfPending = useRef<Set<{ img: SkImage; data: SkData | null }>>(new Set());
  const swapWfImage = useCallback((img: SkImage, data: SkData | null) => {
    // ★ The UI thread shows it (jsPending, taken by the pacer's frame callback) — this keeps the dispose bookkeeping.
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
        if (wfPending.current.delete(old)) { try { old.img.dispose(); old.data?.dispose(); } catch {} }
      }, 300);
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Paths share the same hazard (the trace/peak redraw every tween + zoom frame),
  // so dispose them on the same time grace rather than a count window.
  const pathPending = useRef<Set<SkPath>>(new Set());
  /* ★★★ WHICH PATH IS SHOWING IS KEPT HERE, NEVER READ BACK FROM THE SHARED VALUE (fixed 2026-10-03; the
   *  audit's shared empty path crashed Android within seconds of the first screen tour closing, "Attempted
   *  to access a disposed object", reproduced 2/2 → 0/3 by bisection on an emulator). `sv.value` read on
   *  the JS thread is not guaranteed to be the SAME JS object that was stored — Reanimated may hand back
   *  another wrapper round the same native path — so `old !== peakEmpty` said "not the empty path" about
   *  the empty path itself, disposed it, and the next frame drew a dead object. Identity is only reliable
   *  on objects we hold, so the current path lives in a ref. */
  const peakCur = useRef<SkPath | null>(null);
  const swapPath = useCallback((sv: { value: SkPath }, p: SkPath) => {
    const old = peakCur.current ?? peakEmpty;
    if (old === p) return;            // already showing it (the shared empty path) — nothing to do
    peakCur.current = p;
    sv.value = p;
    if (old !== peakEmpty) {          // ★ the shared empty path is permanent — never dispose it
      pathPending.current.add(old);
      setTimeout(() => { if (pathPending.current.delete(old)) { try { old.dispose(); } catch {} } }, 300);
    }
  }, []);

  /* ★★★ UNMOUNT FREES ON THE SAME GRACE AS EVERY SWAP. It disposed the LIVE image at once — the one the
   *  ImageShader is drawing — while the Skia mapper for it is only torn down later on the UI thread, so a
   *  frame already queued drew a disposed object: "Attempted to access a disposed object" thrown in the
   *  worklets AnimationFrameCallback (Play vitals, build 350, CppException). Hand everything over, clear
   *  the sets (so a swap's own pending timer finds nothing and never frees it twice), and free 300 ms
   *  later — the grace swapWfImage/swapPath already rely on. */
  useEffect(() => () => { // unmount: flush the pending-dispose sets + the live image
    const live = wfLive.current, imgs = [...wfPending.current], paths = [...pathPending.current];
    wfLive.current = null; wfPending.current.clear(); pathPending.current.clear();
    setTimeout(() => {
      if (live) { try { live.img.dispose(); live.data?.dispose(); } catch {} }
      imgs.forEach(r => { try { r.img.dispose(); r.data?.dispose(); } catch {} });
      paths.forEach(p => { try { p.dispose(); } catch {} });
    }, 300);
  }, []);

  // ── Smooth scroll (UI thread) — fed to the shader as a sub-pixel sample
  // offset (uShift); no view transform, no gap at the slide edge.
  const scrollFrac  = useSharedValue(1);
  const lastFrameTs = useRef(0);
  const avgFrameMs  = useRef(150);
  const rowTargetSt = useRef(newRowTargetState());   // the feed-following row target (wfRowTarget.ts)
  const rowTargetCap = useRef(-1);

  const wfUniforms = useDerivedValue(() => ({
    uHeadF:    uHead.value,
    uFrac:     scrollFrac.value,
    uN:        uNSv.value,
    uQuant:    uQuantDrawSv.value,   // what the display driver is DRAWING (it finishes a row before it settles)
    uRows:        uRowsSv.value,   // the ring's depth — grows (never shrinks) with the image it came with
    uVisibleRows: shaderRows,  // whole device px per row (see devRowPx) — same scroll both orientations
    uTexW:     uTexW.value,
    uDrawW:    width,
    uDrawH:    wfRenderH,
    uSharp:    uSharpSv.value,
    uContrast: uContrastSv.value,
    uMoLo:     uMoLoSv.value,
    uMoHi:     uMoHiSv.value,
    uRowPx:    uRowPxV,
    uColPx:    uColPxV,
    uZA:       uZASv.value,
    uZB:       uZBSv.value,
  }), [width, wfRenderH, shaderRows, uRowPxV, uColPxV]);

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
  // ★★ A ROTATION REDRAWS THE TRACE AT ONCE (the Android diet, 2026-10-11, frame-by-frame on the G35: the spectrum
  //    sat blank or squashed for ~0.5 s after every rotation, on every skin). The tween rebuilds the path on the UI
  //    thread from the last bins and parks itself when they settle — and a new width woke nothing, so the trace waited
  //    for the next DATA frame, which the JS thread delivers late while it is busy laying the screen out again. Waking
  //    the tween on a layout change redraws the bins it already has at the new size, with no JS frame needed.
  useEffect(() => { setSpecTweenActive(true); }, [width, wfTop, specH, setSpecTweenActive]);

  /** One tween tick (33 ms cadence). Returns TRUE when the trace has converged (caller stops ticking). Shared by the
   *  JS path's specTween callback and the native engine's enginePoll — the same motion either way. */
  const specTweenTick = (gapMs: number): boolean => {
    'worklet';
    // No busy flag needed: the callback is DEACTIVATED when settled (see below), so it is not
    // running at all rather than running and returning.
    // ★★★ HOLD THE OLD 33 ms CADENCE, do not run at the display rate. useFrameCallback fires on
    // EVERY display frame — 120 of them a second on ProMotion, against the 30 the interval gave.
    // Rebuilding the path four times as often would swap a JS-thread problem for a UI-thread one,
    // which is the version of this bug that would be much harder to see coming. The interpolation
    // is time-based, so ticking at the same rate reproduces the previous motion exactly.
    specAccSv.value += gapMs;   // ★ since this tick last RAN (uiCadence), so a FRAME RATE cap skips nothing
    if (specAccSv.value < SPEC_TWEEN_MS) return false;
    const dtAcc = specAccSv.value;
    specAccSv.value = 0;
    const to = specToSv.value, disp = specDispSv.value;
    const n = to.length;
    if (!n || disp.length !== n) return false;
    const w = uSpecW.value, baseline = uSpecTop.value, sh = uSpecHt.value;
    if (w <= 0 || sh <= 0) return false;

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
    // ★ A reduced frame (one point per SPEC_PX_STEP, built in handleFrame) is indexed by step directly, so it
    //   draws exactly the bins the full-frame mapping below would have picked — no float rounding between them.
    const nDec = Math.ceil(w / SPEC_PX_STEP);
    for (let px = 0; px < wi; px += SPEC_PX_STEP) {
      const v = disp[n === nDec ? px / SPEC_PX_STEP : Math.floor((px / w) * n)];
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
    if (maxDelta < 0.002) { specAccSv.value = 0; return true; }
    return false;
  };
  const specTweenLast = useSharedValue(0);
  const specTween = useFrameCallback((fi) => {
    'worklet';
    if (specDead.value) return;   // component is gone — see specDead
    const gap = cadenceGap(specTweenLast, fi.timestamp);   // ★ FRAME RATE cap (services/uiCadence)
    if (gap < 0) return;
    if (specTweenTick(gap)) runOnJS(setSpecTweenActive)(false);
  }, false);   // autostart OFF — started on the first data frame
  specTweenRef.current = specTween;

  // pushRow reads the view from a ref, so keep it current.
  useEffect(() => { viewRef.current = { centerHz, bwHz }; engine?.setView(centerHz, bwHz); }, [centerHz, bwHz, engine]);

  // ── Row push + line ticker (whole-pixel waterfall advance) ─────────────────
  // pushRow advances the waterfall by exactly one pixel row: ring write,
  // incremental display shift, colourise, new SkImage. The line ticker emits
  // duplicate pushes of the latest data row between frames so the fps modes
  // reach 30/60 lines per second.
  const pushRow = useCallback((srcRow: Uint8Array, rowCenterHz = 0, rowHzPerBin = 0, publish = true) => {
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
      // ★ + the motion rows (move + zoom) the image carries below the ring (utils/wfRowPlace.ts wfImageBytes, motionImageRows).
      const R = ringRows.current;
      idxBuf.current = new Uint8Array(RING_W * (R + motionImageRows(R, RING_W)));
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
    //
    // ★★★ THE HARD RULE — Stuart, 2026-10-27, quoted in full in utils/wfRowPlace.ts: tuning, zooming, resizing and
    //     rotating must NEVER lose or tear the history; signals glide in from the sides and settle, and a zoom fans
    //     them in and out. Each row is placed ONCE, here, against the view it lands in, and the ring is never
    //     rewritten. GitHub #57's torn trails were rows the SERVER cropped against the wrong tuner position
    //     (local_sdr_shim.cpp onSpectrum); scripts/test_wf_row_place.ts guards this half, scripts/test-web-waterfall-history.mjs the server's.
    // ★ Only a frame at the SAME bin scale may be shifted (rowViewOffset) — one from a different span is drawn as it came.
    // ★★ MOTION (2026-10-28, "the chunky broken diagonal lines joined up so they looked solid"): the row's placement is
    //    noted beside the ring with how far its trail moved from the row before, and the SHADER draws the move between the
    //    two rows (motion-compensated interpolation) — nothing stored is altered; steady rows draw exactly as before.
    //    (It replaced the glide streak, which max-smeared a moved row's signals: the "smeared" look Stuart rejected.)
    //    A move too big to bend (a jump) re-maps the history like a zoom — utils/wfRowPlace.ts MOTION + THE ZOOM FAN.
    // ★★★ THE ZOOM FAN (2026-10-28): a row at a different SPAN re-maps every stored row to the new view, so the history
    //    fans out on a zoom in and draws in on a zoom out instead of being left as slices of the old scale. The stored
    //    rows are never rewritten — the IMAGE is made from a displayed ring regenerated through per-row maps
    //    (wfImageBytes), so a zoom undone gives the edges back exactly. A tune does not touch it (the bend above).
    const v = viewRef.current;
    const placed = placedRef.current;
    const R = ringRows.current;
    const slot = placeRow(idxBuf.current, R, n, frameCount.current, row, rowCenterHz, rowHzPerBin, v.centerHz, v.bwHz,
                          motionSt.current!, fanSt.current!, fanVisible.current, placed);
    // ★★ V12 THE FULL-CAPTURE LAYER: this row's layer row at the same slot, in capture space (utils/wfWide.ts) — the
    //    blended newest picture of the whole capture through the detailed rows' own dB → index mapping. Only once a
    //    server has sent one (fanSt.layer is null until then).
    // ★★ SCALE STAMPS: the row as DISPLAYED (its placement through its fan map — eased rows included) to the detector;
    //    a jump moved every row, so it moves the stamps too. The layer hears only when the stamps change.
    {
      const fs = fanSt.current!, ss = stampSt.current, g0 = ss.gen, sc = stampCfg.current;
      if (fs.jumpHz) stampJump(ss, fs.jumpHz);
      ss.minGapRows = sc.minGapRows; ss.sweepRows = sc.sweepRows;
      const m = displayMap(motionSt.current!, fs.xfA, fs.xfB, slot, n);
      if (m) stampNoteRow(ss, frameCount.current, m[0], m[1] * n, stampSettleRows(fs.rowRate));
      if (ss.stamps.length) stampPrune(ss, frameCount.current, sc.keepRows);
      if (ss.gen !== g0) stampSink.current?.(ss.stamps.slice());
    }
    const layer = fanSt.current!.layer;
    if (layer) {
      const w = frameCfg.current.width;
      layer.fadeTx = w > 0 ? Math.max(2, Math.min(24, 6 * RING_W / w)) : 6;   // ~6 pt: the soft edge where a row's edge is on screen
      const r = proc.current.getRange(), ws = wideSt.current;
      layerWriteRow(layer, slot, ws, placed.startHz, placed.spanHz, Date.now(),
                    makeToIdx(r.dbMin, r.dbMax, proc.current.getSettings().wfBrightness || 0, ws ? ws.off : 0));
      fanRow(fanSt.current!, idxBuf.current, n, slot, R);   // ★ a zoom's eased row: drawn again now its layer row exists
    }
    frameCount.current += 1;
    // ★★ COVERED (an opaque full-screen overlay — the map, recordings, About): the ROW is kept, the
    //   PICTURE is not made. History stays continuous — closing the map shows what passed while it was
    //   open — but no texture is built or uploaded and the shader does not run for a screen nobody
    //   can see. publishRing() makes the one texture on uncover. See useScreenCovered.
    if (coveredRef.current) { ringDirty.current = true; return; }
    if (publish) publishRing(); else ringDirty.current = true;   // (the fixed clock's catch-up: several rows, ONE image)
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [swapWfImage]);

  /* ★★★ NO REFRESH HOLD (2026-10-28, reverted after a day of trials — held 120, 40, 60, the FRAME RATE setting as the
   *  rate, and none: each traded one judder for another and the holds made the app feel laggy; Stuart: "the judder is
   *  worse and the frame rate locks make the app feel laggy … we had these issues in the past and we fixed them"). The
   *  display links ask for what they always did (worklets' 60–120 on ProMotion) under the FRAME RATE cap, and the pacing
   *  is the one that was right before: whole rows on the display's beat at rest, the 120 Hz gesture mode while
   *  interacting. The native setSteadyRefresh stays, unused. */


  /** Build the waterfall texture from the ring as it stands and hand it to the shader. */
  const publishRing = useCallback(() => {
    if (!idxBuf.current) return;
    ringDirty.current = false;
    const n = RING_W;
    // ★★★ NO HERMES MEMORY PRESSURE (2026-10-28, the late frame every ~15 s on iOS/Mac at 20 fps). Stock
    //   Skia.Data.fromBytes + Skia.Image.MakeImage report BOTH host objects to Hermes as external memory —
    //   1 MiB each, ~2 MiB a row, ~40 MiB/s at 20 fps — so Hermes ran a GC every few seconds, and the big ones
    //   stalled the JS thread long enough to make a waterfall frame late. The pressure buys nothing here: every
    //   image is disposed 300 ms after it is replaced (swapWfImage). __vibeMakeGray8Image (the skia patch,
    //   VibeExport.cpp) builds the same image with ONE copy of the bytes, no SkData host object, and no
    //   pressure — what the Android engine's wrapImage already does. Same pixels, same dispose; only the GC
    //   stops being told about memory we free ourselves. The stock pair stays as the fallback (web, or a
    //   binary built before the patch).
    // ★★ The image is the ring + its motion rows tall: the motion rows ride with the rows they describe, so the shader can
    //   never see one image's rows with another's motion (utils/wfRowPlace.ts wfImageBytes). ★ Its depth rides with it
    //   too (jsPending.rows → uRowsSv): a grown ring's image and its uRows reach the shader in the same frame.
    const R = ringRows.current, H = R + motionImageRows(R, n);
    const bytes = wfImageBytes(fanSt.current!, motionSt.current!, idxBuf.current, n, R);
    const mk = (globalThis as { __vibeMakeGray8Image?: VibeMakeGray8Image }).__vibeMakeGray8Image;
    let img: SkImage | null = null;
    let data: SkData | null = null;
    if (mk) {
      img = mk(bytes, n, H, n);
    } else {
      data = Skia.Data.fromBytes(bytes);
      img = Skia.Image.MakeImage(
        { width: n, height: H, colorType: ColorType.Gray_8, alphaType: AlphaType.Opaque },
        data,
        n,
      );
      if (!img) { data.dispose(); return; }
    }
    if (!img) return;
    swapWfImage(img, data); // retire the old one (1 MiB: 1024x1026 Gray_8)
    // ★★ ONE HAND-OVER TO THE UI THREAD: the image, the newest row in it, its motion range and the screen move a fan
    //   made are taken TOGETHER by the pacer's frame callback (jsPending), so the shader never draws an image with the
    //   previous one's head, motion range or geometry for a frame. Which rows are SHOWN is the pacer's call (rowPace).
    const [k, c] = takeFanStep(fanSt.current!);
    const ms = motionSt.current!;
    jsPendGen.current += 1;
    jsPending.value = { gen: jsPendGen.current, img, avail: frameCount.current, moLo: ms.lo, moHi: ms.hi, k, c, rows: R };
    if (!texReadyRef.current) { texReadyRef.current = true; setTexReady(true); }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [swapWfImage]);

  // ★★★ THE RING GROWS (utils/wfRingSize.ts, utils/wfRingGrow.ts — Stuart, 2026-10-30: "the history only goes when it
  //     genuinely goes off screen"). The view became taller than the ring holds: every row it has, and everything kept
  //     per row (motion, the fan's maps and displayed ring, the full-capture layer), is copied into a deeper ring HERE, on
  //     the JS thread between two pushRow calls, and the new image goes out with its depth (jsPending.rows) — so the shader
  //     switches ring and uRows in one frame. Until then it draws the old image; rows older than it are the floor (the
  //     shader's guard) — exactly what the grown ring holds there. Never shrinks. The engine grows its own (setConfig).
  useEffect(() => {
    const R0 = ringRows.current;
    if (!(ringTarget > R0)) return;
    ringRows.current = ringTarget;
    if (engine) return;   // the native engine keeps its own ring — it is told the depth in its config
    idxBuf.current = growRing(idxBuf.current, motionSt.current!, fanSt.current!, RING_W, R0, ringTarget, frameCount.current);
    if (!idxBuf.current) return;
    if (coveredRef.current) ringDirty.current = true; else publishRing();
  }, [ringTarget, engine, publishRing]);

  // ★ THE ZOOM FAN makes only the rows that can be on screen; a resize that shows more (rotate to a taller view, a Mac
  //   window dragged taller) makes the newly visible ones from the stored ring before they are seen — the history the
  //   hard rule says a resize must keep, at the zoom it is now shown at.
  useEffect(() => {
    if (!idxBuf.current || !fanRefresh(fanSt.current!, idxBuf.current, RING_W, ringRows.current, frameCount.current, fanVisible.current)) return;
    if (coveredRef.current) ringDirty.current = true; else publishRing();
  }, [shaderRows, publishRing]);

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
  /** UI-thread timestamp at which the current pair started revealing, or -1 = "capture on the next
   *  frame". Absolute due-times are measured from this — see revealCb. */
  const revStartTs = useSharedValue(-1);
  /** Frames with nothing to reveal. The stepper now stays ACTIVE between pairs (stopping there is
   *  what caused the halting scroll), so this is what still releases the display when the stream
   *  genuinely stops rather than merely pausing between pairs. */
  const revIdle = useSharedValue(0);
  const revealRef = useRef<{ setActive: (b: boolean) => void; isActive: boolean } | null>(null);
  const setRevealActive = useCallback((on: boolean) => {
    const t = revealRef.current;
    if (t && t.isActive !== on) t.setActive(on);
  }, []);
  const stopRevealStepper = useCallback(() => { setRevealActive(false); }, [setRevealActive]);

  /** Push `k` more copies of the newest row. Used ONLY on a feed slower than the chosen scroll
   *  rate, to hold that rate without inventing data: a repeated row is honest (nothing new has
   *  arrived) where an interpolated one is a picture of a moment that never existed. The web
   *  client settled on exactly this — see waterfall.ts drawRow, "WHOLE ROWS, NOT BLENDS". */
  const emitRepeatRows = useCallback((k: number) => {
    const row = lastRow.current;
    if (!row) return;
    const { centerHz, hzPerBin } = lastRowMeta.current;
    for (let i = 0; i < k; i++) pushRow(row, centerHz, hzPerBin);
  }, [pushRow]);

  /** Timer handle for the extra-row chain — cancelled on every arrival. */
  const extraTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  /** Push `k` extra rows, one every `spacingMs`, so a feed slower than the chosen scroll rate
   *  still scrolls at that rate. Cancelled and re-armed by the next arrival, so it cannot run
   *  ahead of the data or pile up. */
  const scheduleExtraRows = useCallback((k: number, spacingMs: number) => {
    if (extraTimer.current) { clearTimeout(extraTimer.current); extraTimer.current = null; }
    if (k <= 0 || !(spacingMs > 0)) return;
    let left = k;
    const tick = () => {
      emitRepeatRows(1);
      if (--left > 0) extraTimer.current = setTimeout(tick, spacingMs);
      else extraTimer.current = null;
    };
    extraTimer.current = setTimeout(tick, spacingMs);
  }, [emitRepeatRows]);

  /* ★★★ THE FIXED OUTPUT CLOCK (V12 2026-10-29, src/assets/wfClock.ts — every mode but NATIVE). Stuart: "if we are only
   *  doing a straight 2x 3x interpolation an incoming rate that drifts about will make it feel juddery which is why I said
   *  to over interpolate and discard to avoid this issue". The rows used to be made PER ARRIVAL (rowsForArrival + the
   *  repeat timer above), so the supply followed the feed: a late frame left the playout buffer empty and the scroll
   *  stalled. Now the rows come off a steady clock at the mode's rate, each one the newest REAL frame presented by its
   *  tick (a repeat, never a blend); a late frame costs one more repeat, two frames in one tick drop the older. This timer
   *  only PRODUCES them into the ring, at exactly the rate; the UI thread releases them on the display's frames
   *  (utils/wfPace.ts clockPace). Frames are kept here by id until the clock has shown or dropped them. */
  const rowClock   = useRef(newRowClock());
  const boostSt    = useRef(newBoost());   // ★ the interaction boost (wfClock.ts boostRowRate)
  /** ★ What the last frame decided the clock's rate from — pumpClock walks the boost's taper with it between frames. */
  const clockRateIn = useRef({ base: 0, boosted: 0, hz: 60 });
  /** ★★ V12 2026-10-30 THE WATERFALL WAITS FOR THE SOUND (src/assets/wfAvSync.ts): the audio's playout delay (polled
   *  below) and the hold each clocked frame enters with. */
  const avSt       = useRef(newAvSync());
  const lastEaseLeft = useRef(0);
  const clockTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const clockOut   = useRef<number[]>([]);
  const clockSeq   = useRef(0);
  /** The real frames the clock may still show, by id (a few: the clock's queue is 4 deep, plus the one it holds). */
  const clockFrames = useRef(new Map<number, CombFrame>());
  const clockPool   = useRef<CombFrame[]>([]);   // their buffers, reused (no per-frame allocation)
  /** ★★ V12 2026-10-30 (src/assets/wfCombine.ts): the id of the last frame the clock has SHOWN, and the combiner that merges
   *  the frames it passed over into the next one shown — "rather than simply dropping the excess frames can we average
   *  them" (Stuart). The frames since the last shown are kept (≤ COMBINE_MAX_FRAMES) until then. */
  const clockShown  = useRef(0);
  const clockComb   = useRef(newRowCombiner());
  const clockStop = useCallback(() => {
    if (clockTimer.current) { clearTimeout(clockTimer.current); clockTimer.current = null; }
  }, []);
  const pumpClock = useCallback(() => {
    clockTimer.current = null;
    const rc = rowClock.current, now = performance.now(), out = clockOut.current;
    out.length = 0;
    // ★★★ THE BOOST'S END AND ITS TAPER, ON THE CLOCK'S OWN TICKS (wfClock.ts boostTaper, 2026-10-31): decided per frame
    //     only, the end waited for the next arrival (up to 200 ms at 5 fps) and the taper's rungs would land on arrivals.
    //     Same state, same rule as the frame path (boostActive then boostTaper), so the two never disagree.
    const cri = clockRateIn.current, bs = boostSt.current;
    if (cri.base > 0 && rc.rate > 0 && (bs.on || (bs.peak ?? 0) > 0)) {
      const tNow = Date.now();
      boostActive(bs, tNow, (fanSt.current?.easeLeft ?? 0) > 0);
      const r = boostTaper(bs, cri.base, cri.boosted, tNow, cri.hz);
      if (r > 0 && r !== rc.rate) {
        clockSetRate(rc, r, now);
        const tg = r / 1000; if (pTgt.value !== tg) pTgt.value = tg;
        if (fanSt.current) fanSt.current.rowRate = r;
      }
    }
    clockRun(rc, now, out);
    const fr = clockFrames.current;
    for (let i = 0; i < out.length; i++) {
      // ★★ A frame shown for the first time takes in the frames the clock passed over since the last one shown — combined
      //    in linear power with the peaks kept (wfCombine.ts), not dropped. A feed at or below the rate: nothing to merge.
      if (out[i] > clockShown.current) { combineInto(clockComb.current, fr, clockShown.current, out[i]); clockShown.current = out[i]; }
      const f = fr.get(out[i]);
      if (f) pushRow(f.row, f.c, f.h, i === out.length - 1);   // several due (a busy JS thread): ONE image for them all
    }
    // Forget frames the clock can no longer show or merge: shown already (but the one it holds), or — past the cap — the
    // oldest not held or queued.
    if (fr.size > 6) {
      const free = (id: number, f: CombFrame) => { fr.delete(id); if (clockPool.current.length < COMBINE_MAX_FRAMES + 4) clockPool.current.push(f); };
      for (const [id, f] of fr) if (id <= clockShown.current && id !== rc.cur) free(id, f);
      if (fr.size > COMBINE_MAX_FRAMES + 2) {
        const keep = new Set<number>(rc.q.map(f => f.id)); keep.add(rc.cur);
        for (const [id, f] of fr) { if (fr.size <= COMBINE_MAX_FRAMES + 2) break; if (!keep.has(id)) free(id, f); }   // (Map: oldest first)
      }
    }
    if (bgRef.current || coveredRef.current || clockIdle(rc, now)) return;   // a dead link stops; the next frame restarts it
    const nx = clockNextAt(rc);
    if (nx >= 0) clockTimer.current = setTimeout(pumpClock, Math.max(0, nx - performance.now()));
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [pushRow]);
  useEffect(() => clockStop, [clockStop]);   // unmount

  const revealCb = useFrameCallback((fi) => {
    'worklet';
    if (specDead.value) return;   // component is gone — see specDead
    // ★★★ COUNT FRAMES, NOT MILLISECONDS — the model proven in the web client (see
    //     memory/waterfall_refresh_locked_model.md). A waterfall line is a whole pixel, so a line
    //     rate that is not commensurate with the refresh emits 1,1,0,1,1,0… however carefully the
    //     time is accumulated: at 20 lines/s on 60 Hz the due-time drifts across a frame boundary
    //     and the cadence becomes 3,3,4,3,4. Quantising to a whole number of display frames is
    //     even by construction, and adapts to a 120 Hz panel for free.
    //     ★ revStep is still the WANTED interval in ms; it is converted to frames here, against a
    //       SMOOTHED frame time so one long frame (a GC pause) cannot change the cadence.
    // ★★★ REJECT ANYTHING THAT IS NOT A DISPLAY INTERVAL — DO NOT CLAMP IT IN.
    //     This stepper DEACTIVATES at the end of every pair (so an idle waterfall never pins a
    //     ProMotion panel) and restarts on the next data frame. On that first callback,
    //     timeSincePreviousFrame is the gap since the callback last RAN — ~250 ms — not a frame
    //     time. The first version clamped it to 100 ms and fed it to the average, which pulled
    //     revFrameMs from ~16.7 to ~25, flipped framesPerLine from 3 to 2, and let it decay back
    //     over the next few frames. So the cadence oscillated ONCE PER PAIR, which is precisely
    //     the judder Stuart saw on build 71 ("a juddery mess, no smoothness at all").
    //     ★ SHARP was unaffected and that is what identified it: n<=1 returns early in
    //       startRevealStepper, so SHARP never runs this callback at all.
    //     ★ A display frame is 8.3 ms (120 Hz) or 16.7 ms (60 Hz). Anything outside a sane band is
    //       a restart, a stall or a GC pause — it carries NO information about the refresh rate, so
    //       it must be DISCARDED rather than averaged in. Clamping was the bug; the clamp made a
    //       nonsense value look plausible instead of throwing it away.
    // ★★★ ABSOLUTE DUE-TIMES, NOT A ROUNDED FRAME COUNT. This is the third attempt and the
    //     reasoning that finally explains all three symptoms, so it is worth stating properly.
    //
    //     The web client quantises the cadence to a whole number of display frames — one row every
    //     N frames — and that works THERE because it then SIZES THE PAIR FROM THE SAME ROUNDED
    //     NUMBER (memory/waterfall_refresh_locked_model.md §2). Both halves are load-bearing.
    //     ★★ THIS RENDERER CANNOT DO THE SECOND HALF. `uN` maps ALL waterfall history through the
    //     shader, so the lines per pair is a CONSTANT and cannot be resized to match a rounded
    //     rate (§"the app needs the opposite rule"). So rounding the cadence here GUARANTEES the
    //     pair finishes early or late against the arrival interval — and that mistiming IS the
    //     start-stop, at every boundary, no matter how accurately the refresh rate is measured.
    //     Build 71 rounded against a corrupted average, 72 against a clean one, 73 kept the
    //     callback alive — all three still rounded, so all three still halted.
    //
    //     With n fixed, exactly ONE cadence fills the interval: interval/n, in TIME.
    //     ★ So each line k is due at pairStart + k*revStep, computed ABSOLUTELY. Absolute due-times
    //       cannot drift (an incremental accumulator loses its remainder on every reset, which is
    //       what the ORIGINAL pre-da29ac0 code did), and they are self-correcting under a VARIABLE
    //       refresh rate — which is the other thing that was wrong: a ProMotion panel moves between
    //       120 Hz and 60 Hz on its own, so an averaged frame time lands on ~12 ms, a value that is
    //       not a frame interval on ANY display (Stuart, 2026-08-04: "I wonder if we are mistiming
    //       it due to VRR"). Timing against the clock does not care what the panel is doing.
    //     ★ The residual quantisation is ONE FRAME — 8 ms on ProMotion, 17 ms at 60 Hz — because an
    //       emission can only land on a frame. That is inherent to any display and is far smaller
    //       than the interval-length halt it replaces.
    const n = revN.value;
    if (n <= 0 || revK.value >= n) {
      // Nothing due. Keep the callback ALIVE between pairs — stopping and restarting it cost a
      // runOnJS round trip at every boundary, which was its own halt (build 73). Release the
      // display only when the data has genuinely stopped, ~1 s.
      revIdle.value += 1;
      if (revIdle.value > 90) { revIdle.value = 0; runOnJS(setRevealActive)(false); }
      return;
    }
    revIdle.value = 0;
    // First frame of a pair: start the clock here rather than in startRevealStepper, so the
    // interval is measured on the UI thread's own timebase and never includes a JS-thread hop.
    if (revStartTs.value < 0) revStartTs.value = fi.timestamp;
    const elapsed = fi.timestamp - revStartTs.value;
    const due = Math.min(n, Math.floor(elapsed / Math.max(1, revStep.value)));
    if (due <= revK.value) return;
    // ★★★ THE STEPPER NOW PUSHES REAL ROWS — it no longer advances a reveal fraction, because
    //     there is no longer anything to reveal: uN is 1, so a row exists or it does not.
    //     This is the web client's model (waterfall.ts drawRow): the extra rows a SLOW feed needs
    //     to hold the chosen scroll rate are REAL ring rows, pushed over the interval. History is
    //     never re-mapped, so changing the scroll setting cannot rescale what is already drawn.
    //     ★ One JS hop per extra row. On a fast feed there are NONE (the arrival is the row);
    //       on a 10 fps feed at the 20-row setting it is 10 a second — the same order as the data
    //       path itself, and far less than the per-pair activation churn this replaces.
    const owed = due - revK.value;
    revK.value = due;
    runOnJS(emitRepeatRows)(owed);
  }, false);
  revealRef.current = revealCb;

  const startRevealStepper = useCallback((n: number, intervalMs: number) => {
    // ★★★ scrollFrac IS NOW PINNED AT 1, ALWAYS. With uN = 1 there is nothing to "reveal": a row
    //     either exists in the ring or it does not. The shader reads R = uFrac * uN, and R must be
    //     1 for the NEWEST row to be the one at the top — at 0 it would draw the head one row
    //     stale. The scroll now comes from rows being PUSHED, exactly as in the web client, not
    //     from a fraction being animated over a block of synthesised lines.
    // (The display driver owns scrollFrac now — paceTick.)
    // n === 0 is legitimate: nothing extra is due THIS interval, but the stepper must stay alive
    // (see the caller). Only a negative count means "switch it off".
    if (n < 0) { setRevealActive(false); return; }
    revN.value = n;
    revStep.value = Math.max(8, intervalMs / n);
    revK.value = 0;
    revIdle.value = 0;
    // ★ -1 = "start the clock on the next UI frame". Taking the timestamp HERE would bake in the
    //   JS-thread hop that got us to this function, which is exactly the latency that made the old
    //   stop/start cadence uneven.
    revStartTs.value = -1;
    // Keep the measured frame time across restarts — it is a property of the DISPLAY, not of this
    // reveal, and re-learning it every frame interval would leave the first lines mistimed.
    setRevealActive(true);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [setRevealActive]);

  useEffect(() => stopRevealStepper, [stopRevealStepper]); // stop on unmount

  // ── THE DISPLAY DRIVER: which rows are shown, how, and the screen ease (utils/wfPace.ts) ───────────────────────────
  /* ★★★ ONE PACE, TWO WAYS OF DRAWING IT (2026-10-28 — Stuart: "keep a STEADY PACE but have everything feel smooth rather
   *  than awkwardly ramp up and slow down like some of the v2 builds did"; and "the whole zooming in and out and tuning
   *  actually used to use ProMotion and ramp up to 120fps too").
   *  Settled: whole rows on the display's beat (rowPace — unchanged since 2026-10-13). Interacting, or a slow Sharp feed:
   *  the SAME rows at the SAME measured rate, the position continuous (contPace) and the shader's motion-compensated
   *  blend drawing the trail between data rows — every display frame while interacting (120 Hz on ProMotion), ≤ 60 Hz
   *  for a slow feed at rest (the 2026-10-01 power audit's rule for the old glide). The hand-over keeps the position and
   *  the buffer, so the scroll neither lurches when a gesture starts nor stalls when it ends.
   *  ★ This replaced the per-arrival GLIDE (scrollFrac 0 → 1 over each frame interval, restarted on every data row, with
   *    the pacer told to show the newest row while it ran): the waterfall jumped a buffer's worth forward at the start of
   *    every gesture, the repeat rows of a slow feed landed on top of the glide mid-way (two motions), and the release
   *    waited for the buffer to refill — the ramp. 7013cebe1's rule stands: no continuous glide at REST while repeat rows
   *    carry the scroll (uQuant decides that, unchanged) — and during a gesture the repeat rows are simply rows of the
   *    same steady pace now, not a second motion.
   *  ★ Nothing here asks any server for anything: all of it is drawn from rows that already arrived. */
  const pShown = useSharedValue(0);      // the newest row on screen while settled (uHead)
  const pAcc   = useSharedValue(0);
  const pLead  = useSharedValue(0);      // rows in hand, smoothed (the playout buffer — see rowPace)
  const pCool  = useSharedValue(0);
  const pMode  = useSharedValue(0);
  const pRate  = useSharedValue(0.03);   // rows per ms, measured (starts at Default's 30/s)
  /** ★★★ V12 (Stuart 2026-10-28: "the interpolation always over interpolates and discards"): when the rows are being
   *  FILLED IN to a target (Eco 10, Default 20, Smooth 60, Minimum 5 under a slow feed), the pace is that TARGET, rows
   *  per ms — not the measured arrival rate. The fill-in pushes rows in lumps (9 fps → 10 is 1,1,…,1,2 per arrival), and
   *  a ½ s measurement of lumps swung 8 ↔ 12 rows/s, so ECO's one-row-every-6-frames rhythm held on ~45 % of steps.
   *  Paced at the known target, the playout buffer absorbs the lumps (as the web's RowPacer does) and the rhythm holds
   *  (scripts/test_wf_speed_cap.ts). 0 = actual rows: the measured rate, as before. */
  const pTgt   = useSharedValue(0);
  /** ★ V12 2026-10-29: the rows come off the FIXED OUTPUT CLOCK (every mode but NATIVE — wfClock.ts): released by
   *  clockPace (an exact-rate phase accumulator, the buffer held by whole-frame slips), not rowPace's mode lock. */
  const pClocked = useSharedValue(false);
  const engRateSeen = useSharedValue(false);   // the engine reports its rate (poll().rowRate) — the JS mirror stands down
  const engTgtSt = useRef(newRowTargetState());   // the engine path's twin of the engine's own row target (see frameSink)
  const engTgtMs = useRef({ last: 0, avg: 0, cap: -1 });
  const pRateT = useSharedValue(0);
  const pRateN = useSharedValue(0);
  const pPos   = useSharedValue(0);      // the continuous position, rows (contPace)
  const pCLead = useSharedValue(0);
  const pCont  = useSharedValue(false);  // drawing continuously
  const pCTgt  = useSharedValue(1);      // the buffer the continuous pace keeps (the settled lead it was handed)
  const pSRate = useSharedValue(0);      // the pace the settled picture actually runs at, rows/ms (shownRate)
  const pSN    = useSharedValue(0);
  const pST    = useSharedValue(0);
  const pWrote = useSharedValue(0);      // timestamp of the last continuous write (the ≤ 60 Hz rule at rest)
  // THE SCREEN EASE (wfPace.ts easeAt/easeCompose): a fan's step, eased over one row interval.
  const zA0 = useSharedValue(1), zB0 = useSharedValue(0), zT0 = useSharedValue(0), zDur = useSharedValue(0);
  const zOn = useSharedValue(false);
  const gapOnSv  = useSharedValue(gapLogOn());   // read once at mount (dev log)
  /** A new image's screen move (x_old = c + k·x_new): start (or extend) the ease. UI thread. */
  const easeStep = (k: number, c: number, ts: number) => {
    'worklet';
    if (k === 1 && c === 0) return;
    let a = 1, b = 0;
    if (zOn.value) { const e = easeAt(zA0.value, zB0.value, zT0.value, zDur.value, ts); a = e[0]; b = e[1]; }
    const nx = easeCompose(a, b, k, c);
    zA0.value = nx[0]; zB0.value = nx[1]; zT0.value = ts;
    // One row interval at the measured rate: a pinch's chain of steps is one continuous motion.
    zDur.value = Math.max(40, Math.min(250, 1 / Math.max(1e-4, pRate.value)));
    zOn.value = true;
  };
  /** One display frame of the driver. `avail` = the newest row in the image. Returns the rows advanced (diag). */
  const paceTick = (avail: number, ts: number, gap: number): number => {
    'worklet';
    const interacting = interactSv.value;
    rowRate(avail, ts, pRate, pRateN, pRateT, interacting);   // ★ frozen while interacting (a re-subscribe's pause)
    const before = uHead.value;
    const wantCont = uQuantSv.value < 0.5;
    // the SETTLED pace, frozen while continuous — ★ but clocked rows run at the clock's rate (pTgt), which the interaction
    // boost raises while the hand is on the dial: the release follows the supply exactly, up and back down.
    const cRate = pClocked.value && pTgt.value > 0 ? pTgt.value : (pSRate.value > 0 ? pSRate.value : pRate.value);
    if (wantCont && !pCont.value) {
      paceToCont(pShown, pPos, pLead, pCLead, pCTgt);   // from where the whole-row picture is: no lurch
      pCont.value = true;
      uQuantDrawSv.value = 0;
    }
    if (pCont.value) {
      // Asked to settle: keep gliding until this frame lands on a whole row (≤ one row interval), then hand over there.
      const land = !wantCont && contAtWholeRow(pPos.value, cRate, gap);
      const pos = contPace(avail, pPos, pCLead, cRate, gap, pCTgt.value);
      if (land) {
        paceToSettled(avail, pPos, pShown, pAcc, pLead, pCLead, pMode);
        // ★ Clocked rows (wfPace clockPace): the phase starts half a frame's share from the edge, never on it.
        if (pClocked.value && pTgt.value > 0) pAcc.value = Math.min(0.5, pTgt.value * gap / 2);
        pCont.value = false;
        uQuantDrawSv.value = 1;
        if (uHead.value !== pShown.value) uHead.value = pShown.value;
        scrollFrac.value = 1;
      } else if (interacting || ts - pWrote.value >= 12) {
        // ≤ 60 Hz at rest (a slow feed's glide: sub-pixel motion — the 2026-10-01 power audit); every frame while
        // interacting (ProMotion's 120). Time-based, so a skipped write loses nothing.
        pWrote.value = ts;
        const w = Math.floor(pos);
        if (uHead.value !== w + 1) uHead.value = w + 1;
        scrollFrac.value = pos - w;
      }
    } else {
      const shown = rowPace(avail, pShown, pAcc, pLead, pCool, pMode, pTgt.value > 0 ? pTgt.value : pRate.value, gap, true,
                            pClocked.value && pTgt.value > 0);
      shownRate(shown, ts, pSRate, pSN, pST);
      if (uHead.value !== shown) uHead.value = shown;
      if (scrollFrac.value !== 1) scrollFrac.value = 1;
    }
    if (zOn.value) {
      const e = easeAt(zA0.value, zB0.value, zT0.value, zDur.value, ts);
      uZASv.value = e[0]; uZBSv.value = e[1];
      if (e[0] === 1 && e[1] === 0) zOn.value = false;
    }
    return uHead.value - before;
  };

  // ── Native spectrum engine: the UI-thread side (see `engine`) ───────────────────────────────────────────
  // ONE frame callback: take whatever the engine finished since the last display frame, then run the display driver
  // and the trace tween it would otherwise have needed JS to switch on. No runOnJS per frame — nothing here waits on JS.
  // ★ Replaced images and paths are disposed RETIRE_FRAMES later: the canvas records them on this thread and draws
  //   them on SkiaRender, but the recorded picture holds its own reference, so a few frames is ample. They were made
  //   without Hermes memory pressure, so dispose is cheap — and it must happen, or native memory waits for a GC.
  const engRetire  = useSharedValue<{ o: { dispose: () => void }; f: number }[]>([]);
  const engFrame   = useSharedValue(0);
  const engImg     = useSharedValue<SkImage | null>(null);
  const engPeak    = useSharedValue<SkPath | null>(null);
  const engSpec    = useSharedValue(-1);
  const engPeakGen = useSharedValue(-1);
  const engSpecOn  = useSharedValue(false);
  const engTexSent = useSharedValue(false);
  const engLast = useSharedValue(0);
  /* ★★ ROWS SHOWN ON THE DISPLAY'S CLOCK (2026-10-12, Galaxy Tab A9, 60 Hz: "very slight waterfall judder … almost
   *  like vsync isnt working"). The engine makes rows on its own timer (~33 ms on Default) and the screen draws every
   *  16.7 ms; when the two run nearly in phase, jitter flips which frame a row lands on — measured as a row every other
   *  frame with, about one move in eight, a frame late and then two at once (1.1 0 0 0 2.9 0 1.1). So every row is in
   *  the IMAGE the moment it is made, but uHead — which rows are SHOWN — advances at the measured row rate on the frame
   *  callback, from a playout buffer of ≈40 ms (utils/wfPace.ts rowPace). The JS path does the same (jsAvail). */
  // ── The JS path's pacer (iOS; Android without the engine). ★★ 2026-10-13: a bunched pair of frames (network jitter —
  //   the status reading 21 fps for a second) stepped the iPhone's waterfall twice in quick succession; Android's engine
  //   path already paced rows to the display. Same rule here: publishRing hands over jsPending, this shows rows on the clock.
  const jsAvail  = useSharedValue(0);
  const jsSeen   = useSharedValue(0);    // the last jsPending generation taken
  const jsLast   = useSharedValue(0);
  useFrameCallback((fi) => {
    'worklet';
    if (specDead.value) return;
    const gap = cadenceGap(jsLast, fi.timestamp);
    if (gap < 0) return;
    const pend = jsPending.value;
    if (pend.gen !== jsSeen.value && pend.img) {
      // ★ The image, its newest row, its motion range and its screen move — all in the same frame.
      jsSeen.value = pend.gen;
      wfImage.value = pend.img;
      jsAvail.value = pend.avail;
      if (uRowsSv.value !== pend.rows) uRowsSv.value = pend.rows;
      uMoLoSv.value = pend.moLo; uMoHiSv.value = pend.moHi;
      easeStep(pend.k, pend.c, fi.timestamp);
    }
    const avail = jsAvail.value;
    if (avail <= 0) return;
    const coolWas = pCool.value;
    paceTick(avail, fi.timestamp, gap);
    // DEV frame-gap log (utils/frameGapLog.ts, OFF by default): a deliberate slip — one hop to JS per slip only.
    if (gapOnSv.value && !pCont.value && coolWas > 0 && pCool.value === 0) {
      runOnJS(gapPacerSlip)(pLead.value, Math.max(1, pRate.value * PLAYOUT_MS), pRate.value);
    }
  }, engine == null);

  // DIAG (diagnostics builds only): a histogram of rows shown per drawn frame, so pacing is measured, not eyeballed.
  const paceDiag = useMemo(() => !!(NativeModules as any).VibePowerModule?.isDiagBuild?.(), []);
  const paceHist = useSharedValue<number[]>([0, 0, 0, 0, 0, 0]);
  const paceT0 = useSharedValue(0);
  const paceLog = useCallback((h: string, gap: number, rate: number, lead: number) => {
    console.log(`[VibeDiag] wf pace rows/frame 0..5: ${h} | gap ${gap} ms | rate ${rate} rows/s | lead ${lead} rows`);
  }, []);
  const engAvail = useSharedValue(0);    // the newest row in the image
  const engStampGen = useSharedValue(0);
  // ★ The engine's stamps carry no time: stamp each with the moment it first reached JS (to the second — it is placed
  //   ~0.5 s after the move lands either way).
  const engineStampAt = useRef(new Map<number, number>());
  const applyEngineStamps = useCallback((list: WfStamp[]) => {
    const seen = engineStampAt.current, now = Date.now(), keep = new Set<number>();
    const out = list.map((s) => { keep.add(s.id); let at = seen.get(s.id); if (at == null) { at = now; seen.set(s.id, at); } return s.at ? s : { ...s, at }; });
    for (const id of seen.keys()) if (!keep.has(id)) seen.delete(id);
    stampSink.current?.(out);
  }, []);
  const enginePoll = useFrameCallback((fi) => {
    'worklet';
    if (specDead.value || !engine) return;
    // ★ FRAME RATE cap (services/uiCadence): under a 30 cap this — every image, uniform and path the canvases redraw
    //   for — runs on every other vsync. Rows the engine made meanwhile are taken together on the next run.
    const gap = cadenceGap(engLast, fi.timestamp);
    if (gap < 0) return;
    const RETIRE_FRAMES = 3;
    const f = engFrame.value + 1;
    engFrame.value = f;
    const q = engRetire.value;
    if (q.length) {
      const keep: { o: { dispose: () => void }; f: number }[] = [];
      for (let i = 0; i < q.length; i++) {
        if (f - q[i].f >= RETIRE_FRAMES) { try { q[i].o.dispose(); } catch { /* already gone */ } }
        else keep.push(q[i]);
      }
      engRetire.value = keep;
    }
    const p = engine.poll();
    if (p) {
      const retire = engRetire.value;
      if (p.image) {
        const old = engImg.value;
        wfImage.value = p.image;
        engImg.value = p.image;
        engAvail.value = p.head;
        const rr = p.ringRows ?? RING_ROWS;   // ★ the depth of THIS image — with it, never ahead of it
        if (uRowsSv.value !== rr) uRowsSv.value = rr;
        // ★ The engine composes the motion range and the fans' screen move for exactly the image it hands over.
        if (p.moLo != null && p.moHi != null) { uMoLoSv.value = p.moLo; uMoHiSv.value = p.moHi; }
        easeStep(p.fanK ?? 1, p.fanC ?? 0, fi.timestamp);
        if (old) retire.push({ o: old, f });
        if (!engTexSent.value) { engTexSent.value = true; runOnJS(setTexReady)(true); }   // once, ever
      }
      // ★ quant: 0 = draw continuously (a gesture, a slow Sharp feed), 1 = whole rows. The engine's per-arrival
      //   glide command (p.glide / scrollGen) is no longer used — the display driver paces both ways.
      // ★ quant: 0 = draw continuously (a gesture, a slow Sharp feed), 1 = whole rows. (2026-10-28: briefly forced to 1
      //   — "always whole rows" — and reverted the same day: the pacing that was already right came back, Stuart: "the
      //   judder is worse … we had these issues in the past and we fixed them".)
      uQuantSv.value = p.quant;
      uAvgMs.value = p.avgMs;
      // ★ V12 THE FIXED OUTPUT CLOCK: the engine says the rate its rows come at — the pace (pTgt) and the release
      //   (clockPace) follow the engine's own number, not a JS mirror of its rule.
      if (p.rowRate != null) {
        engRateSeen.value = true;
        const tg = p.rowRate > 0 ? p.rowRate / 1000 : 0;
        if (pTgt.value !== tg) pTgt.value = tg;
        if (pClocked.value !== p.rowRate > 0) pClocked.value = p.rowRate > 0;
      }
      uNSv.value = 1;
      // ★★ SCALE STAMPS: the engine detects (wf_stamps.h), the layer draws — handed over only when the list changed.
      if (p.stampGen != null && p.stampGen !== engStampGen.value && p.stamps) {
        engStampGen.value = p.stampGen;
        runOnJS(applyEngineStamps)(p.stamps);
      }
      if (p.specGen !== engSpec.value) {
        engSpec.value = p.specGen;
        if (p.specHidden) {
          engSpecOn.value = false;
          specPathA.reset(); specPathB.reset();
          specPath.value = specPathA;
        } else if (p.spec) {
          specToSv.value = p.spec;
          if (specDispSv.value.length !== p.spec.length) specDispSv.value = p.spec.slice();
          engSpecOn.value = true;
        }
      }
      if (p.peakGen !== engPeakGen.value) {
        engPeakGen.value = p.peakGen;
        const old = engPeak.value;
        const np = p.peak ?? null;
        peakPath.value = np ?? peakEmpty;
        engPeak.value = np;
        if (old) retire.push({ o: old, f });
      }
      engRetire.value = retire;
    }
    // ── Which rows are shown (see engAvail) ──
    if (engAvail.value > 0) {
      const d = paceTick(engAvail.value, fi.timestamp, gap);
      if (paceDiag) {   // DIAG (diagnostics builds): rows shown per drawn frame, logged every 5 s
        const h = paceHist.value; h[Math.max(0, Math.min(5, d))]++; paceHist.value = h;
        if (fi.timestamp - paceT0.value > 5000) {
          paceT0.value = fi.timestamp;
          runOnJS(paceLog)(h.join(','), Math.round(gap), Math.round(pRate.value * 1000 * 10) / 10, Math.round(pLead.value * 10) / 10);
          paceHist.value = [0, 0, 0, 0, 0, 0];
        }
      }
    }
    if (engSpecOn.value && specTweenTick(gap)) engSpecOn.value = false;
  }, engine != null);

  // ── Covered by an opaque full-screen overlay (useScreenCovered) ─────────────────────────────
  // ★ Cover: stop every display-rate driver (they restart on the first data frame after uncover).
  //   Uncover: ONE texture from the ring as it now stands, so the rows that arrived while covered
  //   appear at once instead of waiting for the next frame.
  useEffect(() => {
    if (covered) { stopRevealStepper(); stopSpecTween(); }
    else if (ringDirty.current) publishRing();
  }, [covered, stopRevealStepper, stopSpecTween, publishRing]);

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
      engine?.setBackground(bg);
      if (bg) {
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
                            smoothTune, boostOn: wfBoostOn, targetRows: WF_TARGET_ROWS, targetFps: TARGET_FPS,
                            minGapMs: WF_MIN_GAP_MS });
  frameCfg.current = { width, wfTop, specH, specShow, peakHold,
                       smoothTune, boostOn: wfBoostOn, targetRows: WF_TARGET_ROWS, targetFps: TARGET_FPS,
                       minGapMs: WF_MIN_GAP_MS };

  // Geometry the watch needs to crop a VFO-centred slice out of the row.
  const watchCfg = useRef({ tuneHz, filterLow, filterHigh });
  watchCfg.current = { tuneHz, filterLow, filterHigh };

  /* ★★★ THE TRACE GOES WITH ITS ROW (V12 2026-10-29, v12-wf-no-jb). The waterfall's frames now skip the jitter buffer and
   *  go to the fixed clock on arrival (enqueueFrame) — Stuart: "if the buffer isnt needed for the waterfall improve the
   *  latency there too please". The SPECTRUM TRACE was de-jittered by that same buffer; left behind a buffer of its own
   *  it would trail the newest row by ~85 ms (simulated, scripts/test_wf_fixed_clock.ts), and on arrival it takes the
   *  raw network jitter. So the trace is retargeted on the CLOCK's de-jittered schedule instead: at the frame's
   *  presentation time plus the rows the release holds in hand (traceLagMs) — when that frame's row comes on screen.
   *  Simulated: trace → newest row 50–70 ms apart through the buffer, now 0–20 (±1 tick of the 33 ms tween), and both
   *  ~140 ms sooner. Its ~30 fps easing tween is unchanged. At once (at = 0) in a gesture, off the clock (NATIVE, a feed
   *  too slow to fill) and whenever nothing is waiting — exactly as before. Never out of order; a newer frame that is
   *  due supersedes an older one still waiting (its peak path is disposed unseen). */
  const traceQ     = useRef<{ at: number; spec: number[]; peak: SkPath | null }[]>([]);
  const traceTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const traceApply = useCallback((spec: number[], peak: SkPath | null) => {
    specToSv.value = spec;
    if (specDispSv.value.length !== spec.length) specDispSv.value = Array.from(spec);
    setSpecTweenActive(true);   // idle → running; a no-op while it already is
    swapPath(peakPath, peak ?? peakEmpty);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [setSpecTweenActive, swapPath]);
  const traceDrop = useCallback(() => {
    if (traceTimer.current) { clearTimeout(traceTimer.current); traceTimer.current = null; }
    const q = traceQ.current;
    for (const it of q) if (it.peak) { try { it.peak.dispose(); } catch {} }   // never shown: no grace needed
    q.length = 0;
  }, []);
  const traceRun = useCallback(() => {
    traceTimer.current = null;
    if (bgRef.current || coveredRef.current || specDead.value) { traceDrop(); return; }
    const q = traceQ.current, now = performance.now();
    let due: { at: number; spec: number[]; peak: SkPath | null } | null = null;
    while (q.length && q[0].at <= now + 1) {   // several due (a busy JS thread): the newest is the target
      const it = q.shift()!;
      if (due?.peak) { try { due.peak.dispose(); } catch {} }
      due = it;
    }
    if (due) traceApply(due.spec, due.peak);
    if (q.length) traceTimer.current = setTimeout(traceRun, Math.max(0, q[0].at - performance.now()));
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [traceApply, traceDrop]);
  const traceShow = useCallback((spec: number[], peak: SkPath | null, at: number, shrunk = false) => {
    if (!(at > performance.now())) { traceDrop(); traceApply(spec, peak); return; }   // now: anything waiting is older
    const q = traceQ.current;
    // ★★ V12 2026-10-30 the A/V hold shrank (wfAvSync.ts): the traces still waiting later than this one are older — their
    //   rows were discarded by the clock, so they go too (never shown after a newer one).
    if (shrunk) while (q.length && q[q.length - 1].at > at) { const o = q.pop()!; if (o.peak) { try { o.peak.dispose(); } catch {} } }
    if (q.length && at < q[q.length - 1].at) at = q[q.length - 1].at;                 // never out of order
    q.push({ at, spec, peak });
    // the clock's own depth — ★ and the frames an A/V hold keeps waiting (wfAvSync.ts): one trace per frame on the clock
    const cap = Math.max(4, rowClock.current.q.length + 1);
    while (q.length > cap) { const o = q.shift()!; if (o.peak) { try { o.peak.dispose(); } catch {} } }
    if (!traceTimer.current) traceTimer.current = setTimeout(traceRun, Math.max(0, q[0].at - performance.now()));
  }, [traceApply, traceDrop, traceRun]);
  useEffect(() => traceDrop, [traceDrop]);   // unmount

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
    // ★ V12 the full-capture layer's floor match (utils/wfWide.ts wideArrive) — only once a server has sent one.
    if (wideSt.current) detP50.current = medianOf(fbins, p50Scratch.current);

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
    if (interactSv.value !== boost) interactSv.value = boost;   // the display driver: 120 Hz + the pace rate frozen
    // WATERFALL boost — the continuous vsync glide instead of discrete whole-line steps. Also on at low
    // fps so the low-rate scroll is silky (no added latency; pure scroll interpolation). ★ NOT applied
    // to the spectrum trace: at ~10 fps the trace must keep its ~30 fps EASING TWEEN — making it follow
    // each frame directly there looks jerky (Stuart 2026-07-24). Waterfall and trace want opposite
    // things at a low data rate, so the flags are split.
    const lowFps = avgFrameMs.current > LOW_FPS_GLIDE_MS;
    // ★★★ NO GLIDE WHILE REPEAT ROWS CARRY THE SCROLL — the native engine's 7013cebe, ported to this
    //     JS path (iOS, and Android with the engine off). On a feed slower than the setting's row
    //     rate (Default/Smooth on a < 15 fps feed: UberSDR, a throttled server) the extra rows ARE
    //     the motion; a glide over the frame interval on top of them is two motions fighting. Tell:
    //     ~5 s smooth, then judder, as avgFrameMs crosses 66 ms. Sharp (no repeats) keeps its glide;
    //     a gesture (boost) always glides.
    // ★★★ THE TARGET FOLLOWS THE FEED (Stuart 10-20, src/assets/wfRowTarget.ts): cfg.targetRows is the setting's CAP
    //     (20 Default, 60 Smooth); the rows aimed for are min(cap, 3x / 6x the measured feed), snapped to a divisor of
    //     60, with a latch on the feed so a wobble cannot flap it. 0 = actual rows. rowCarry runs on across a change.
    if (rowTargetCap.current !== cfg.targetRows) { rowTargetSt.current = newRowTargetState(); rowTargetCap.current = cfg.targetRows; }
    // ★★ …and under a FRAME RATE cap below 60 it divides the cap (wfRowTarget rowTargetUnderCap — Stuart 2026-10-28: "in
    //    30fps lock mode no point running a 60fps waterfall"): 60 → 30, 20 → 15.
    // ★★★ V12 2026-10-29: every mode but NATIVE runs on the FIXED OUTPUT CLOCK (pumpClock) at wfClock's rate — the same
    //     rule, latched so it does not step DOWN while the feed still supports it (Stuart's 9–11 fps UberSDR kept Smooth
    //     at 60, not 60 ↔ 30). NATIVE keeps its rule exactly as it was (0 = actual rows: "pure unmodified lines").
    const wfScroll = scrollForCap(cfg.targetRows);
    const feedFps = 1000 / Math.max(1, avgFrameMs.current);
    const clocked = wfScroll !== 'sharp';
    const baseRowTarget = clocked
      ? (avgFrameMs.current > 0 ? clockRateUnderCap(clockRowRate(rowTargetSt.current, wfScroll, feedFps), uiFrameCapHz.value) : 0)
      : rowTargetUnderCap(avgFrameMs.current > 0
          ? stepRowTarget(rowTargetSt.current, wfScroll, 1000 / avgFrameMs.current)
          : rowTargetSt.current.target, feedFps, uiFrameCapHz.value);
    // ★★★ THE INTERACTION BOOST (src/assets/wfClock.ts boostRowRate — Stuart 2026-10-29: "increase during interaction …
    //     even if it means a 12x frame generation" / "if the app is set to 120 we do 120"). While a hand is on the dial
    //     (BOOST_TAIL_MS after the last input) or an ease is still drawing, the CLOCK's rate steps up to the display's
    //     maximum as set (the FRAME RATE cap, or the panel's), ≤ 120, ≤ 12 × the feed. The extra rows are frames already
    //     here, each at its own in-between view (fanAfterRow) — the ease gets the rows its curve needs. Nothing is asked
    //     of the server. NATIVE (baseRowTarget from its own rule, never clocked) is untouched.
    const ptNow = Date.now();   // (the boost's clock: lastInteractAt's)
    //     ★ Also while the fan is easing a move this hand did not make (a shared dial, a recentre): the rows it needs are
    //       the same, and the boost is local — nothing goes to the server.
    //       The tail runs from the last input or the START of the last ease; the boost then holds until the ease lands.
    const easeLeft = fanSt.current?.easeLeft ?? 0, easing = easeLeft > 0;
    const easeStarted = easeLeft > lastEaseLeft.current; lastEaseLeft.current = easeLeft;
    // (the input's OWN time — noting `now` while the input is recent would add a second tail)
    const inAt = lastInteractAt?.current ?? 0;
    if (inAt > boostSt.current.lastMove && ptNow - inAt < BOOST_TAIL_MS) boostNote(boostSt.current, inAt);
    if (easeStarted) boostNote(boostSt.current, ptNow);
    const boostOn = clocked && baseRowTarget > 0 && boostActive(boostSt.current, ptNow, easing);
    // ★★★ …and TAPERS OUT when it ends (wfClock.ts boostTaper, Stuart 2026-10-31: "the sudden FPS drop from 120→30 is
    //     causing what appears to be a slight pause"): 120 → 60 → 40 → 30 over BOOST_TAPER_MS, equal rows a rung. pumpClock
    //     walks the same taper between frames (clockRateIn), so a step never waits for the next arrival.
    const dispHz = uiDisplayMaxHz();
    const boostRate = clocked && baseRowTarget > 0 ? boostRowRate(baseRowTarget, feedFps, dispHz, cfg.boostOn) : baseRowTarget;
    const rowTarget = clocked && baseRowTarget > 0 ? boostTaper(boostSt.current, baseRowTarget, boostRate, ptNow, dispHz) : baseRowTarget;
    const cri = clockRateIn.current; cri.base = clocked ? baseRowTarget : 0; cri.boosted = boostRate; cri.hz = dispHz;
    const repeatsCarry = rowTarget > 0;
    const wfBoost = boost || (cfg.smoothTune && lowFps && !repeatsCarry);

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
    /** When the SPECTRUM TRACE takes this frame (performance.now() ms; 0 = at once) — see traceShow. */
    let traceAt = 0;
    let traceShrunk = false;   // ★ the A/V hold shrank on this frame (see the clock below)
    if (!wfGated) {
    // ★★★ MEASURE THE GAP BETWEEN ROW-SETS, NOT BETWEEN DATA FRAMES. This is the quantity the
    //     reveal has to be spread over, and using the wrong one is what made the waterfall "leap a
    //     section, wait, leap a section, wait" (Stuart, build 74).
    //     `avgFrameMs` is updated for EVERY data frame, above, and therefore measures the SPECTRUM
    //     rate — 50 ms at 20 fps. But the waterfall is GATED to one row-set every minGapMs (250 ms
    //     for the 20-row setting), so pacing n lines over 50 ms finished the whole pair in 80 ms
    //     and then sat still for the remaining 170 ms. Every one of my three previous attempts
    //     tuned the CADENCE while feeding it an interval that was five times too short.
    //   ★ SHARP was immune, which is why it always looked right: WF_TARGET_ROWS = 0 means no gate
    //     (minGapMs = 0) AND n = 1, so there is no interval to get wrong.
    if (lastWfPushAt.current > 0) {
      const wfDt = now - lastWfPushAt.current;
      // Same smoothing as avgFrameMs. Ignore absurd gaps (a resume, a tune) so one pause cannot
      // slow the scroll for the next several pairs.
      // ★ 2026-10-28: and NOT while interacting — a re-subscribe's pause folded in here lengthened `dur`, so every
      //   arrival after it earned extra repeat rows and the scroll SPED UP for a beat after a tune (the ramp).
      if (wfDt > 0 && wfDt < 2000 && !boost)
        avgWfGapMs.current = avgWfGapMs.current > 0 ? avgWfGapMs.current * 0.8 + wfDt * 0.2 : wfDt;
    }
    lastWfPushAt.current = now;

    // ★★★ THE ROW'S OWN GEOMETRY — centre AND span (2026-10-29, beta37 iPhone on UberSDR: "the fan out on zoom doesn't
    //     work properly", the history below a zoom out drawn as sideways smears). bwHz is the PREDICTED view while a zoom
    //     is in flight (300 ms after the last send), but the bins are at the server's rung — UberSDR snaps every request
    //     to its ladder (200/300/500/1000/2000 Hz a bin, measured). Labelled with the prediction, every pinch step and the
    //     settle's snap to the real rung were taken for zoom steps the data never made: each started a fan ease that
    //     magnified rows by the wrong ratio and joined rows of one scale as if they were another — rows stretched
    //     sideways and torn. Placed by trueBwHz, a row changes span only when its bins really do, and the fan runs once
    //     per real step. (The trace and the axis keep the predicted geometry — they move under the finger.)
    const [rowCentre, rowHzBin] = rowGeometry(fstatus, frame.row.length);   // utils/wfRowPlace.ts
    // ★★★ FILLED IN OR THINNED (2026-10-29, Stuart on a 23 fps Kiwi: "interpolate and discard"): the rows this frame
    //     becomes at the target (wfRowTarget rowsForArrival) — 0 = DISCARDED from the waterfall (a feed faster than the
    //     rate: Eco's 13 fps Kiwi thinned evenly to 10), 1 = the frame, more = the frame and repeats. The spectrum trace
    //     and everything else above still had this frame. Native (target 0) is always 1: every frame, as it comes.
    const revealMs = avgWfGapMs.current > 0 ? avgWfGapMs.current : avgFrameMs.current;
    // ★ Floor 15 ms, not 50 (2026-10-29): a feed faster than 20 fps (a 23 fps Kiwi) was counted as 50 ms a frame, so its
    //   rows over-ran the target by up to 2.5× once Default/Smooth filled such a feed in (23 fps at 30 → 34 rows/s).
    const dur = Math.max(15, Math.min(1000, revealMs));
    const onClock = clocked && rowTarget > 0 && !coveredRef.current;
    const rowsNow = onClock ? 0 : rowsForArrival(rowCarry, dur, rowTarget);
    // ★★ The zoom/tune ease is by TIME (wfRowPlace.ts zoomEaseRows): the rows/s the waterfall runs at — the clock's rate, or
    //    one row a frame in Native.
    if (fanSt.current) fanSt.current.rowRate = rowTarget > 0 ? rowTarget : (avgFrameMs.current > 0 ? 1000 / avgFrameMs.current : 20);
    if (rowsNow > 0) pushRow(frame.row, rowCentre, rowHzBin);
    if (onClock) {
      // ★★★ THE FIXED CLOCK: the frame is handed to the clock (presented on the feed's de-jittered schedule — at once
      //     during a gesture, as the jitter buffer is bypassed then too), and the clock makes the rows (pumpClock).
      // ★★★ V12 2026-10-29 (v12-wf-no-jb): the frame comes here ON ARRIVAL now, not out of the jitter buffer (see
      //     enqueueFrame) — `t` is its real arrival, and the clock's own presentation delay (0.2 × the interval, ≤ 40 ms) is
      //     the only de-jitter. Stuart: "if the buffer isnt needed for the waterfall improve the latency there too please".
      //     Simulated (scripts/test_wf_fixed_clock.ts): arrival → on screen ~220 → ~80 ms, stalls and bursts still 0.
      const id = ++clockSeq.current, t = performance.now();
      let slot = clockPool.current.pop();
      if (!slot || slot.row.length !== frame.row.length) slot = { row: new Uint8Array(frame.row.length), c: 0, h: 0, k: 0 };
      clockFrames.current.set(id, slot);
      slot.row.set(frame.row); slot.c = rowCentre; slot.h = rowHzBin; slot.k = dbPerUnit(frame.dbMin, frame.dbMax);
      clockSetRate(rowClock.current, rowTarget, t);
      // ★★★ THE WATERFALL WAITS FOR THE SOUND (src/assets/wfAvSync.ts, Stuart 2026-10-30). The frame enters the clock
      //     `hold` ms after it arrived — the audio's playout delay less what this route already takes — so the row and
      //     its trace come on screen as the sound is heard (they ran ~140 ms ahead at rest, ~320 after a hiccup). Stuart:
      //     "to prevent scrolling judder whilst the jitter buffer enables we can simply duplicate the previous lines if
      //     needed and then the waterfall still reacts to the audio in time but still stays smooth": a hold that GROWS is
      //     carried by the clock repeating the row on screen, one that SHRINKS by discards (clockArrive). ZERO while a
      //     tune or zoom is in hand (SMOOTH_TUNE_TAIL_MS after the last input, smoothTune or not) — a held row is `hold`
      //     ms from the screen, so a moving dial would never show a fresh one (the web's lesson, 2026-08-10). Only WHEN a
      //     frame enters the clock moves: its ticks, rate, boost, fan and ease are untouched.
      // ★ …and for as long as the picture is still MOVING (Stuart, 2026-10-30, build 527: "noticable delay when operating
      //   the zoom controls"): the fan's ease and the interaction boost run until a zoom or tune has landed, which can be
      //   well after the input (the server's frames at the new view arrive, then the ease draws). Held then, the new
      //   view waited for the sound. The hold comes back once it has landed (repeats carry the growth).
      const avPass = now - (lastInteractAt?.current ?? 0) < SMOOTH_TUNE_TAIL_MS || boostOn || easing;
      const holdWas = avSt.current.hold;
      // ★★★ `now`: a growth RAMPS in (wfAvSync.ts AV_GROW, 2026-10-31) — stepped, it stopped the picture and the trace for
      //     the whole step ~250 ms after every tune or zoom ("a noticeable pause", "the spectrum stops too").
      const hold = avHold(avSt.current, clockOwnMs(rowClock.current.ivl, rowTarget), avPass, now);
      const pAt = clockArrive(rowClock.current, id, t, boost, hold);
      clockStop();
      pumpClock();
      // ★★ The trace goes with its row: at the frame's presentation time plus the rows clockPace holds in hand, so the
      //   trace eases to this frame as its row comes on screen (traceLagMs). At once in a gesture (as before).
      if (!boost) traceAt = pAt + traceLagMs(rowTarget);
      // ★ The hold shrank: the trace frames still waiting later than this one went with their rows (clockArrive).
      if (hold < holdWas) traceShrunk = true;
    } else if (rowClock.current.rate > 0) {
      // Off the clock (NATIVE, a feed too slow to fill, or covered): stop it; the next clocked frame starts afresh.
      clockStop(); clockReset(rowClock.current); clockSetRate(rowClock.current, 0, 0); clockFrames.current.clear();
    }
    // Keep it for the repeat pushes on a slow feed (frame.row is reused by the caller).
    if (!lastRow.current || lastRow.current.length !== frame.row.length)
      lastRow.current = new Uint8Array(frame.row.length);
    lastRow.current.set(frame.row);
    lastRowMeta.current = { centerHz: rowCentre, hzPerBin: rowHzBin };
    // ★★ COVERED: the row is in the ring (pushRow) and the watch has had it (above). Nothing below —
    //   repeat rows, the glide, the reveal stepper, the trace tween, the peak path — exists to be
    //   SEEN, and nothing can be: stop here. (The dB axis was already updated, so it is right on
    //   uncover.) The waterfall's display clocks are stopped by the uncover/cover effect.
    if (coveredRef.current) return;
    // copies synchronously — no snapshot needed
    uQuantSv.value = wfBoost ? 0 : 1;

    // (The interval the rows are spread over, `dur` — the gap between ROW-SETS, falling back to the frame interval until
    // it has been measured — was worked out with the row count above.)

    // ★★★ ROW EMISSION HAPPENS ON BOTH PATHS — THIS IS WHAT MADE SLOW FEEDS SLOW.
    //     `wfBoost` is true whenever the feed is below ~15 fps (lowFps: avgFrameMs > 66 ms), which
    //     is EVERY UberSDR session. The boost branch only pins uN and animates scrollFrac; it never
    //     pushed extra rows — so on a slow feed the waterfall ran at the raw DATA rate whatever
    //     the scroll setting said. "UberSDR is sending the full 10 and all 3 settings the rate is
    //     extra slow" (Stuart, 2026-08-04), and no amount of tuning the emitter could show up,
    //     because on that feed the emitter was never reached.
    //     ★ It made sense in the OLD model, where the glide came from uFrac sweeping across uN
    //       synthesised lines. With uN = 1 there are no synthesised lines to sweep: the scroll IS
    //       the rows, so the rows must be pushed on every path.
    // Rows this interval has earned at the target (fractional carry; the arrival itself was row #1) — wfRowTarget
    // carryRows, proved with the display pacer in scripts/test_wf_speed_cap.ts. ★ The rule never asks for more than 6
    // rows a frame (Stuart 10-20), so its cap of 8 is headroom — the stall guard.
    const extras = onClock ? 0 : Math.max(0, rowsNow - 1);   // (rowsForArrival above — the arrival itself, when kept, was row #1; the clock makes its own)
    { const tg = rowTarget > 0 ? rowTarget / 1000 : 0; if (pTgt.value !== tg) pTgt.value = tg; }   // the pace (see pTgt)
    if (pClocked.value !== onClock) pClocked.value = onClock;   // the release for clocked rows (wfPace clockPace)
    lastDynRows.current = extras + 1;
    scheduleExtraRows(extras, dur / (extras + 1));
    // ★★★ SPREAD THE REVEAL OVER THE ROW-SET INTERVAL, NOT THE DATA-FRAME INTERVAL. On a gated
    //     feed these differ by the gate ratio (5x at the 20-row setting), and using the frame
    //     interval made every pair finish in a fifth of the time available and then stall — the
    //     "leap a section, wait" (Stuart, build 74). Falls back to the frame interval only until
    //     the row-set gap has been measured, and on SHARP where there is no gate and the two are
    //     the same thing anyway.
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
      // ★★★ uN IS NOW ALWAYS 1 — see the note at WF_TARGET_ROWS. The shader no longer maps history.
      uNSv.value = 1;
      stopRevealStepper();
      // ★★ No per-arrival glide any more (2026-10-28): uQuant = 0 tells the display driver (paceTick) to draw the SAME
      //   steady pace continuously — at display rate while interacting, ≤ 60 Hz for a slow feed at rest.
    } else {
      // Settled: interpolate UP to hold at least the target scroll rate from the (now stable) data
      // rate — 5fps Low Data still scrolls at the chosen 10/20/30 fps; fast data (Kiwi) needs none.
      // A CONSTANT — never derived from the measured rate, so it can never rescale the picture.
      // ★★★ uN = 1, ALWAYS. The shader maps ONE ring row to one display line and nothing else,
      //     so NOTHING RESCALES when the scroll setting changes — which is the whole complaint:
      //     "speeding up or slowing down the waterfall affects the history too, so the rendered
      //     waterfall shrinks or expands depending on setting, the client doesnt do that"
      //     (Stuart, 2026-08-04). The web client has never had this because it maps nothing: it
      //     pushes REAL ROWS, so a rate change affects only NEW rows and history stays as drawn.
      //     ★ And with uN out of the picture, `dynRows` is free to VARY per frame — it now only
      //       says how many rows to PUSH, which is a local decision about new data. That is what
      //       lets every frame be used (full detail) AND the scroll rate be held on a slow feed.
      uNSv.value = 1;
      // ★★★ EMIT AT THE TARGET ROW RATE, WITH A FRACTIONAL CARRY — NOT AN INTEGER MULTIPLE OF THE
      //     FEED. 30 rows/s from a 20 fps feed is 1.5 rows per frame; rounding that to 2 gives 40
      //     rows/s and SMOOTH stops meaning 30. The carry spends the halves: 1, 2, 1, 2… so the
      //     AVERAGE is exactly the rate the setting promises, on any feed.
      //     ★ Only possible because uN is 1: the row count per frame is now a local decision about
      //       new data, not a global mapping, so it may vary frame to frame at no cost.
      // (uN is 1, so R must be 1 — the display driver sets scrollFrac = 1 whenever it draws whole rows.)
      // ★★★ NEVER STOP THE STEPPER BETWEEN ARRIVALS. At 30 rows/s on a 20 fps feed the extras
      //     alternate 0,1,0,1 — so stopping on the zero frames meant a stop/start round trip
      //     through runOnJS every 50 ms, and the single extra row had 25 ms to survive it. It
      //     mostly did not, which collapsed SMOOTH back to DEFAULT's rate: "the 3 options make no
      //     difference in visible speed" (Stuart, 2026-08-04). Exactly the same stop/start latency
      //     that caused the halting scroll in build 73 — twice now.
      //     ★ It stays active with revN = 0 (nothing due) and releases the display itself after
      //       ~1 s of no data, which is what the idle counter in the worklet is for.
      // ★★★ EXTRA ROWS ARE PUSHED FROM A PLAIN TIMER, NOT FROM THE VSYNC WORKLET.
      //     The worklet had to hop back to JS (runOnJS) to push a row, and those extra rows were
      //     not arriving — every scroll setting ran at the DATA rate, which is why UberSDR at
      //     10 fps was "extra slow, slower than it was before" (Stuart, 2026-08-04).
      //     ★ The hop bought nothing: pushing a row IS a JS-thread operation, so timing it on the
      //       UI thread only to bounce straight back added latency and a failure mode. The reveal
      //       FRACTION genuinely needed vsync — rows do not.
      //     ★ Self-cancelling on the next arrival, so a slow feed can never queue a burst.
    }
    }   // end !wfGated — everything below (trace, peaks, meters) runs for EVERY frame
    if (coveredRef.current) return;   // ★ covered — see the same check above

    // 4. Spectrum + peak paths from normalised [0,1] traces
    if (cfg.specShow && cfg.specH > 4) {
      // The trace ALWAYS eases via the ~30fps tween — including DURING interaction. The old boost
      // path snapped the trace to each DATA frame, so it redrew only at the data rate: fine at
      // 20/30fps, but at 5fps it juddered and looked slow on every tune/zoom (Stuart 2026-07-24,
      // "the tune/zoom spectrum slowdown", "worse at 5 than 10fps"). The tween runs on the display
      // clock regardless of the data rate, so the trace stays fluid at ANY rate — the way
      // VibeServer/Jr do it. The view is pinned during a gesture, so the spectrum content barely
      // shifts and the small easing lag is invisible; on release the tween eases to the new view.
      let traceNext: number[] | null = null;
      {
        // Retarget the tween at the latest frame.
        // ★ Array.from, because a worklet cannot be handed a Float32Array — the shared value has to
        //   carry a plain array. This is once per DATA frame (~20/s), replacing thirty path builds
        //   a second on this thread, so the JS thread is left far quieter than before.
        /* ★★ ONLY THE POINTS THE TRACE DRAWS. The tween worklet samples ONE bin per SPEC_PX_STEP pixels
         *  (disp[floor(px / w * n)]), so handing it every bin — 1024–4096 numbers, 8–32 KB, a fresh array per
         *  data frame because Reanimated freezes what it is given — was mostly garbage: on the iPhone profile
         *  (2026-10-03) Hermes GC was 14 % of the app. Pick the same bins here, in the same order, and the
         *  worklet's own mapping lands on them exactly (floor(k·STEP / w · m) = k for m = ceil(w / STEP)):
         *  ~w/2 numbers instead of every bin, the drawing unchanged. The peak-hold trace below still reads the
         *  full frame. */
        const fs = frame.spec, fn = fs.length;
        const wDraw = Math.floor(cfg.width);
        let next: number[];
        if (wDraw > 0 && fn > Math.ceil(cfg.width / SPEC_PX_STEP)) {
          const m = Math.ceil(cfg.width / SPEC_PX_STEP);   // ★ unfloored: keeps the worklet's floor(px/w·m) == k
          next = new Array(m);
          for (let k = 0; k < m; k++) next[k] = fs[Math.floor(((k * SPEC_PX_STEP) / cfg.width) * fn)];
        } else {
          next = Array.from(fs);
        }
        traceNext = next;
      }

      let peakNext: SkPath | null = null;   // null = draw no peak trace
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
        peakNext = pp;
      } else {
        // Peak hold PAUSES while interacting: bin-indexed peaks detach from
        // their signals as the view moves (geometry is pinned during gestures
        // so the processor can't see the shift) and smear the display. Clear
        // and hide; it re-seeds within a frame or two of settling.
        if (boost) proc.current.resetPeakHold();
        // (peakNext null → swapPath(peakPath, peakEmpty): empty = draw nothing, a no-op once already empty)
      }
      // The trace and the peak path go up together — now, or with this frame's row (traceShow).
      traceShow(traceNext!, peakNext, traceAt, traceShrunk);
    } else {
      // Spectrum hidden — nothing needs inter-frame smoothness; the panel can
      // fall all the way to the data rate.
      traceDrop();
      stopSpecTween();
      // ★★★ CLEAR IT, DO NOT swapPath IT. swapPath DISPOSES the outgoing path, and since the tween
      // moved to the UI thread the outgoing path is one of the two PERSISTENT buffers the worklet
      // resets and reuses every tick — disposing it would hand the worklet a dead native object
      // the next time the spectrum came back. Emptying in place is what "draw nothing" needs
      // anyway, and it leaves both buffers valid.
      // ★★ ON THE UI THREAD: the tween's last tick may still be rebuilding these same native paths there
      //    (stopSpecTween takes effect later), and a reset from JS mid-addPoly is a native race. Queued on
      //    the UI runtime it lands after that tick. (Crash review of Play vitals, 2026-10-04.)
      runOnUI(() => {
        'worklet';
        specPathA.reset(); specPathB.reset();
        specPath.value = specPathA;
      })();
      swapPath(peakPath, peakEmpty);
    }

  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ── Jitter buffer ──────────────────────────────────────────────────────────
  // Queue arrivals and DRAIN them into handleFrame on a STEADY clock, so network / interaction
  // arrival jitter never reaches the display — the reason Jr and the web client feel smooth at a
  // variable/low frame rate where the phone (rendering per-arrival) juddered. targetDepth = 1 frame of
  // banked latency (insurance against a late row); prefill/hold when dry, catch up when backed up.
  // ★★★ V12 2026-10-29 (v12-wf-no-jb): NOT for the fixed clock's modes any more. The clock de-jitters the waterfall itself
  //     (wfClock.ts: each frame presented on the feed's smoothed schedule, a late one costs a repeat), so the buffer in
  //     front of it only added latency — 80–190 ms of the app's ~160–260 ms. Stuart: "if the buffer isnt needed for the
  //     waterfall improve the latency there too please". Those frames go straight to handleFrame on arrival (enqueueFrame)
  //     and the trace rides the clock's schedule (traceShow). Still through here: NATIVE (no clock), and a feed too slow
  //     for the clock to fill (it runs per arrival, as before).
  const jbQueue       = useRef<FrameSlot[]>([]);
  /* ★★ The queued copies come from a small pool, not a fresh `bins.slice()` + status spread per frame
   *  (framePool.ts — the biggest steady-state allocation in the 2026-10-03 iPhone profile). A slot goes back
   *  only after handleFrame has finished with it, or when it is dropped unread. */
  const jbPool        = useRef(new FramePool(JB_MAX_QUEUE_SLOTS));
  const jbClear = () => { const q = jbQueue.current; for (const f of q) jbPool.current.release(f); q.length = 0; };
  const jbArrivalMs   = useRef(150);   // measured arrival cadence (drives the drain rate)
  const jbLastArrival = useRef(0);
  const jbTimer       = useRef<ReturnType<typeof setTimeout> | null>(null);
  /** When the drain timer is due (dev frame-gap log only: how late it fired = how busy the JS thread was). */
  const jbDueAt       = useRef(0);
  const jbPrefill     = useRef(true);
  const JB_TARGET_DEPTH = 1;
  const JB_MAX_QUEUE    = 3;

  const drainFrame = useCallback(() => {
    jbTimer.current = null;
    if (bgRef.current) { jbClear(); jbPrefill.current = true; return; }
    const q = jbQueue.current;
    if (jbPrefill.current) {
      if (q.length >= JB_TARGET_DEPTH) jbPrefill.current = false;
      else return;                                   // hold until banked (a new arrival reschedules)
    }
    if (q.length === 0) { jbPrefill.current = true; return; }   // ran dry → hold, re-prefill
    const f = q.shift()!;
    // ★ DEV frame-gap log (utils/frameGapLog.ts, OFF by default): a late row, and whether the JS timer or the feed made it late.
    if (gapLogOn()) gapDraw(jbArrivalMs.current, jbDueAt.current > 0 ? Date.now() - jbDueAt.current : 0);
    handleFrame(f.bins, f.status);       // reads synchronously and keeps nothing (see framePool.ts)…
    jbPool.current.release(f);           // …so the slot is free again
    // Reschedule at the arrival cadence (steady); a little faster if backed up, to catch up.
    const dur = Math.max(40, Math.min(1000,
      q.length > JB_TARGET_DEPTH ? jbArrivalMs.current * 0.6 : jbArrivalMs.current));
    jbTimer.current = setTimeout(drainFrame, dur);
    jbDueAt.current = Date.now() + dur;
  }, [handleFrame]);

  const enqueueFrame = useCallback((bins: Float32Array, status: SDRStatus) => {
    if (bgRef.current) return;
    gapArrival();
    const now = Date.now();
    const interacting = now - (lastInteractAt?.current ?? 0) < SMOOTH_TUNE_TAIL_MS;
    // ★★★ ON THE FIXED CLOCK: straight through, on arrival (see the note above). The clock running (rate > 0) means the
    //     last frame was clocked; the first frame of a session, a covered view or a feed too slow to fill comes through
    //     the buffer once and starts it. Anything still banked from before goes — it is older than this frame.
    if (!interacting && rowClock.current.rate > 0 && scrollForCap(frameCfg.current.targetRows) !== 'sharp') {
      if (jbTimer.current) { clearTimeout(jbTimer.current); jbTimer.current = null; }
      if (jbQueue.current.length) jbClear();
      jbPrefill.current = true;
      if (jbLastArrival.current > 0) jbArrivalMs.current = jbArrivalMs.current * 0.8 + (now - jbLastArrival.current) * 0.2;
      jbLastArrival.current = now;
      if (gapLogOn()) gapDraw(jbArrivalMs.current, 0);   // the dev frame-gap log: no timer in the way any more
      handleFrame(bins, status);         // consumed synchronously — parent's reused buffers are safe
      return;
    }
    // DURING interaction, BYPASS the buffer — tune/zoom must feel instant. The jitter buffer only
    // exists to smooth steady-state scroll; holding a frame here costs up to a full arrival period
    // (~200ms at 5fps) of lag on every crown move — the slowdown VibeServer/Jr don't have because
    // they render straight through. Flush the queue and render this frame now; re-bank afterwards.
    if (interacting) {
      if (jbTimer.current) { clearTimeout(jbTimer.current); jbTimer.current = null; }
      jbClear();
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
    q.push(jbPool.current.take(bins, status));   // copy (into a pooled slot): the parent reuses its buffers
    if (q.length > JB_MAX_QUEUE) jbPool.current.release(q.shift()!);   // bound the latency
    if (!jbTimer.current) { jbTimer.current = setTimeout(drainFrame, 0); jbDueAt.current = now; }  // kick the drain
  }, [drainFrame, handleFrame, lastInteractAt]);

  useEffect(() => () => { if (jbTimer.current) { clearTimeout(jbTimer.current); jbTimer.current = null; } }, []);

  // ★★ V12 THE FULL-CAPTURE ROW (SPWB) — from the JS spectrum socket (services/wideRowBus.ts). The native engine keeps its
  //    own layer (cpp/spectrum/wf_wide.h); the JS path keeps it here, beside the ring (utils/wfWide.ts).
  useEffect(() => {
    const sink = engine
      ? (u8: Uint8Array) => { engine.pushWide?.(u8); }
      : (u8: Uint8Array) => {
          const row = parseWide(u8);
          if (!row) return;
          const st = wideSt.current ?? (wideSt.current = newWideState());
          wideArrive(st, row, Date.now(), placedRef.current.startHz, placedRef.current.spanHz, detP50.current);
          const fan = fanSt.current!, L = fan.layer;
          // ★ allocated at the FIRST row only (~0.8 MB), and again only if the server's grid ever changed
          if (!L || L.F !== row.floor.length || L.C !== row.cells) fan.layer = newLayerRing(ringRows.current, row.floor.length, row.cells);
        };
    setWideSink(sink);
    return () => setWideSink(null, sink);
  }, [engine]);

  useEffect(() => {
    if (!frameSink) return;
    // ★ With the native engine, a frame is ONE call: the engine copies it and does everything else on its own thread
    //   (its own jitter buffer, processor, ring, repeat rows). lastInteractAt rides along — read at arrival, as
    //   enqueueFrame read it.
    frameSink.current = engine
      ? (bins: Float32Array, st: SDRStatus) => {
          const at = lastInteractAt?.current ?? 0;
          engine.push(bins, st.centerHz ?? 0, st.bwHz ?? 0, st.trueCenterHz, at, st.trueBwHz);
          // ★ The FRAME RATE cap reaches the engine's row target (rowTargetUnderCap) when it changes — no React state
          //   carries it here, so it is checked per frame (one number compare) and the config re-sent on a change.
          const cap = uiFrameCapHz.value, ec = engineCfg.current;
          if (ec && (ec.displayCapHz !== cap || (ec.panelHz ?? 0) !== uiPanelMaxHz.value)) { ec.displayCapHz = cap; ec.panelHz = uiPanelMaxHz.value; engine.setConfig(ec); }
          // ★ The pace target (pTgt) — the SAME rule the engine applies (wf_row_target.h, the same feed EMA), worked out
          //   here because the engine's poll does not carry it. 0 = actual rows.
          if (ec && !engRateSeen.value) {   // (an engine binary from before the fixed clock: mirror its old rule)
            const m = engTgtMs.current, now = Date.now();
            if (m.cap !== ec.targetRows) { engTgtSt.current = newRowTargetState(); m.cap = ec.targetRows; }
            if (m.last > 0) { const d = now - m.last; if (d > 0 && d < 2000) m.avg = m.avg > 0 ? m.avg * 0.8 + d * 0.2 : d; }
            m.last = now;
            const t = m.avg > 0 ? rowTargetUnderCap(stepRowTarget(engTgtSt.current, scrollForCap(ec.targetRows), 1000 / m.avg), 1000 / m.avg, cap) : 0;
            const tg = t > 0 ? t / 1000 : 0;
            if (pTgt.value !== tg) pTgt.value = tg;
          }
          // The display driver's interaction flag (120 Hz, the pace rate frozen) — the engine's own boost rule.
          const on = frameCfg.current.smoothTune && Date.now() - at < SMOOTH_TUNE_TAIL_MS;
          if (interactSv.value !== on) interactSv.value = on;
        }
      : enqueueFrame;    // frames flow through the jitter buffer, not straight to render
    return () => { frameSink.current = null; };
  }, [frameSink, enqueueFrame, engine, lastInteractAt]);

  const engineCfg = useRef<{ width: number; wfTop: number; specH: number; specShow: boolean; peakHold: boolean;
                             smoothTune: boolean; boost: boolean; targetRows: number; displayCapHz: number; panelHz?: number;
                             stampViewRows?: number; stampMinGapRows?: number; ringRows?: number } | null>(null);
  // ── Native engine: the state JS owns, forwarded when it changes ──────────────────────────────────────────
  useEffect(() => {
    engineCfg.current = { width, wfTop, specH, specShow, peakHold, smoothTune, boost: wfBoostOn, targetRows: WF_TARGET_ROWS, displayCapHz: uiFrameCapHz.value, panelHz: uiPanelMaxHz.value,
                          stampViewRows: Math.ceil(shaderRows), stampMinGapRows: stampCfg.current.minGapRows,
                          ringRows: ringTarget };   // ★ the ring depth (grow-only; the engine copies its history across)
    engine?.setConfig(engineCfg.current);
  }, [engine, width, wfTop, specH, specShow, peakHold, smoothTune, wfBoostOn, WF_TARGET_ROWS, shaderRows, ringTarget]);
  useEffect(() => { engine?.setCovered(covered); }, [engine, covered]);
  useEffect(() => {
    if (!engine) return;
    // The dB axis labels: liveRange() answers only when the 2 dB-hysteresis window moved.
    const t = setInterval(() => { const r = engine.liveRange(); if (r) setLiveRange({ dbMin: r[0], dbMax: r[1] }); }, 200);
    return () => { clearInterval(t); engine.setBackground(true); };   // unmount: the engine stops; GC frees it
  }, [engine]);
  /* ★★ V12 2026-10-30 THE WATERFALL WAITS FOR THE SOUND (src/assets/wfAvSync.ts): the audio's playout delay, 4 times a
   *  second (a synchronous native read — utils/audioPlayout.ts) — to the JS path's clock, or to the engine's. Smoothed
   *  there; −1 (nothing playing, an older native build) is no hold. Not while backgrounded (nothing is drawn). */
  useEffect(() => {
    const t = setInterval(() => {
      if (bgRef.current) return;
      const ms = audioPlayoutMs();
      if (engine) engine.setAudioDelay?.(ms);
      else avNoteAudio(avSt.current, ms, performance.now());
    }, 250);
    return () => clearInterval(t);
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [engine]);

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
    for (const b of BAND_PLAN_DRAW_ORDER) {   // ★ widest first, so a specific band is drawn on top — see bandPlan.ts
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
        label: Math.round(liveRange.dbMin + dbOffset + frac * range) + 'dB',
      });
    }
    return out;
  }, [specShow, specH, wfTop, liveRange, dbOffset]);

  // ── Needle + sideband geometry (v1.5) ───────────────────────────────────────
  // ★ utils/needleGeometry — every renderer path below reads this one result, so `needleHidden` (V12 ISM) removes them all.
  const needle = useMemo(() => needleGeometry({ hidden: needleHidden, bwHz, tuneHz, filterLow, filterHigh, width, pxPerHz, hzToX }),
    [needleHidden, bwHz, tuneHz, filterLow, filterHigh, hzToX, pxPerHz, width]);

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

  // ★ V12 beta25: the DC spike's x, or null when there is none or it is off screen
  const dcSpikeX = useMemo(() => {
    if (!(dcSpikeHz > 0) || !(bwHz > 0)) return null;
    const x = hzToX(dcSpikeHz);
    return x >= 0 && x <= width ? x : null;
  }, [dcSpikeHz, hzToX, width, bwHz]);

  // ★★ KNOWN-SIGNAL LABELS (utils/knownSignals.ts — Stuart, 2026-10-30: "those labels should sit just in the spectrum so as
  //    to not overcrowd the waterfall, only in the waterfall when spectrum is disabled", "similar to the RF centre and DC
  //    spike and the frequency band labels"). On the two rows the boundary labels use — LISTEN's (markerLabelBase) and RF
  //    CENTRE's (14 above) — with their boxes BLOCKED, so a label is never drawn over them; the dB axis's left edge too.
  //    Laid out only when the view, the dial or the list changes; null while the toggle is off or none is in view.
  //    ★ The same layout as the web's (waterfall.ts _drawKnown). RN cannot measure text synchronously, so a label's box
  //      is estimated (estimateLabelWidth) — generous, and the box sizes itself to the text when drawn.
  const knownNX = needle && showCenterMarker ? needle.nX : null;
  const knownCX = centerMarker ? centerMarker.x : null;
  const knownPlaced = useMemo(() => {
    if (!knownSignals || !knownSignals.length || !(bwHz > 0) || !(width > 0)) return null;
    const blocked: BlockedBox[] = [{ row: 0, x0: 0, x1: 30 }, { row: 1, x0: 0, x1: 30 }];
    if (knownNX !== null) { const l = Math.min(width - 96, knownNX + 3); blocked.push({ row: 0, x0: l, x1: l + 90 }); }
    if (knownCX !== null) { const l = Math.min(width - 96, knownCX + 3); blocked.push({ row: 1, x0: l, x1: l + 96 }); }
    const p = labelLayout(knownSignals, visStart, bwHz, width, (t) => estimateLabelWidth(t, 9, 3, 0.62), tuneHz, blocked, 4);
    return p.length ? p : null;
  }, [knownSignals, visStart, bwHz, width, tuneHz, knownNX, knownCX]);

  // ── Visible tiles (deckOcclusion) ───────────────────────────────────────────
  // The deck arrives in WINDOW coordinates; this view's own window origin turns it into canvas coordinates.
  const rootRef = useRef<View | null>(null);
  const [rootWin, setRootWin] = useState<{ x: number; y: number } | null>(null);
  const onRootLayout = useCallback(() => {
    rootRef.current?.measureInWindow((x, y) => {
      setRootWin(p => (p && p.x === x && p.y === y ? p : { x, y }));
    });
  }, []);
  /** [x, y, w, h] in canvas coordinates, or null → one full-canvas Fill (as before). */
  const wfTiles = useMemo((): [number, number, number, number][] | null => {
    const d = deckOcclusion;
    if (!d || !rootWin || !(width > 0) || !(wfH > 0)) return null;
    // The hole: the deck's straight-sided middle band (the rounded corners stay drawn), pulled 1 pt further in so
    // the deck's own edge pixels always have waterfall behind them.
    const lx = d.x - rootWin.x, ly = d.y - rootWin.y - wfTop;
    const x0 = Math.max(0, Math.ceil(lx + 1)), x1 = Math.min(width, Math.floor(lx + d.w - 1));
    const y0 = Math.max(0, Math.ceil(ly + d.r + 1)), y1 = Math.min(wfH, Math.floor(ly + d.h - d.r - 1));
    // Not worth a second draw for a sliver (a squat landscape deck, a deck mostly off the canvas).
    if (x1 - x0 < 40 || y1 - y0 < 40) return null;
    const t: [number, number, number, number][] = [];
    if (y0 > 0)     t.push([0, 0, width, y0]);
    if (x0 > 0)     t.push([0, y0, x0, y1 - y0]);
    if (x1 < width) t.push([x1, y0, width - x1, y1 - y0]);
    if (y1 < wfH)   t.push([0, y1, width, wfH - y1]);
    return t;
  }, [deckOcclusion, rootWin, width, wfH, wfTop]);

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
    // ★ The snapshot owns its pixels; the raster surface and the path are scratch — free them now
    //   rather than leave a full-height dpr-scaled buffer waiting for a GC Hermes never feels.
    const img = surface.makeImageSnapshot();
    try { surface.dispose(); path.dispose(); } catch {}
    return { img, halfW, w };
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
    const img = surface.makeImageSnapshot();
    try { surface.dispose(); path.dispose(); } catch {}
    return { img, halfW, w, h };
  }, [needleColor, needle?.scaleQ, height, dpr]);

  // ★ RETIRE A REPLACED STRIP. A colour / intensity / height / rotation change rebuilds these; the
  //   old image was left for GC. Same 300 ms grace as swapWfImage: the cleanup runs after the
  //   render that swapped in the new image, and the UI thread may still be mid-draw on the old one.
  useEffect(() => {
    const s = needleStrip;
    return () => { if (s) setTimeout(() => { try { s.img.dispose(); } catch {} }, 300); };
  }, [needleStrip]);
  useEffect(() => {
    const s = edgeStrip;
    return () => { if (s) setTimeout(() => { try { s.img.dispose(); } catch {} }, 300); };
  }, [edgeStrip]);

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
  const tapStart = useRef({ x: 0, y: 0, t: 0 });  // ★ where and when the touch went down — a label takes only a TAP

  const tapGesture = useMemo(() =>
    Gesture.Tap().runOnJS(true).maxDuration(300)
      .onBegin((e: any) => {
        // A home-bar swipe-up STARTS in the guard strip but releases higher up,
        // so an onEnd-only check let it slip through and retune. Latch the
        // touch-down position and reject in onEnd.
        tapStartGuarded.current = bottomGuard > 0 && e.y >= guardTop;
        tapStart.current = { x: e.x, y: e.y, t: Date.now() };
      })
      .onEnd((e: any) => {
        if (!bwHz || !centerHz) return;
        if (tapStartGuarded.current) return;            // began in home-bar gap
        if (e.y < BAND_H) return; // band strip taps reserved (future: band jump)
        if (bottomGuard > 0 && e.y >= guardTop) return; // home-bar gap
        /* ★★ A KNOWN-SIGNAL LABEL FIRST (knownSignals.ts knownLabelHit — Stuart, 2026-10-30: "can they be tapped to tune
         *  and chose the decoder?"). Its box (RF CENTRE's 13 pt row) is hit over at least LABEL_HIT_MIN_H (30 pt), the
         *  nearer box winning where the two rows' areas overlap. Only a TAP — no further than the slop from where it went
         *  down, no longer than LABEL_TAP_MAX_MS: a drag starting on a label is the Pan gesture's, as before. A miss tunes
         *  to the point exactly as it always did. */
        if (onKnownTap && knownPlaced) {
          const hit = knownLabelHit(knownPlaced, e.x, e.y, (row) => (row === 0 ? markerLabelBase : markerLabelBase - 14), 13);
          const t0 = tapStart.current;
          if (hit && isLabelTap(e.x - t0.x, e.y - t0.y, Date.now() - t0.t)) { onKnownTap(hit.sig); return; }
        }
        // ★★ BY THE ROW UNDER THE TAP (Stuart, 2026-10-30, #57 — Gesiima tunes from the history onto a signal that is quiet
        //    now): on the waterfall, the frequency x had on THAT row (its own scale, as it was drawn — a row from before a
        //    zoom, a tune or a band change), from the same uniforms that scroll the picture. On the newest rows, the spectrum
        //    or a row with no known scale it is the live scale, exactly as before.
        let geo: [number, number] | null = null;
        if (e.y >= wfTop && stampRowPt > 0) {
          const f = rowAtLine(uHead.value, (e.y - wfTop) / stampRowPt, scrollFrac.value, uNSv.value, uQuantDrawSv.value > 0.5);
          if (engine) geo = engine.rowGeom?.(f) ?? null;
          else if (f >= 0 && f < frameCount.current && f >= frameCount.current - ringRows.current) {
            const m = displayMap(motionSt.current!, fanSt.current!.xfA, fanSt.current!.xfB, f % ringRows.current, RING_W);
            geo = m ? [m[0], m[1] * RING_W] : null;
          }
        }
        onTapTune?.(Math.round(hzOnRow(e.x / width, geo, visStart, bwHz)));
      }), [bwHz, centerHz, visStart, width, onTapTune, bottomGuard, guardTop, wfTop, stampRowPt, engine, onKnownTap, knownPlaced, markerLabelBase]);

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
  // ★★ ANDROID: NO FULL-SCREEN CANVAS FOR THE NEEDLE (the diet, Moto G35, 2026-10-11). It was a 1080 × 2292
  //    TextureView — two or three full-screen buffers of graphics memory, composited over everything on every frame —
  //    for a few rects and two cached strips. On Android the same elements draw LAST inside the two canvases they sit
  //    over: the spectrum canvas (0 … wfTop) and the waterfall canvas (shifted up by wfTop); each clips to its own
  //    band, so together they draw exactly what the one canvas did. Only while the first rows load does the
  //    INITIALIZING label now sit over the needle rather than under it. iOS keeps its own canvas (cheap Metal layers).
  // ★★ V12 VFO A/B — the brightness cross-fade: on a switch the newly tuned needle rises to full and the other falls to
  //    INACTIVE_INTENSITY over SWITCH_FADE_MS (not a snap). Off: the needle is exactly as it always was (opacity 1).
  const abFade = useSharedValue(1);
  const abLetter = activeVfo?.letter ?? '';
  useEffect(() => {
    if (!abLetter) { abFade.value = 1; return; }
    abFade.value = 0;
    abFade.value = withTiming(1, { duration: SWITCH_FADE_MS });
  }, [abLetter]);   // eslint-disable-line react-hooks/exhaustive-deps
  const abOn = !!activeVfo;
  const mainOpacity = useDerivedValue(() => (abOn ? INACTIVE_INTENSITY + (1 - INACTIVE_INTENSITY) * abFade.value : 1), [abOn]);
  const secondOpacity = useDerivedValue(() => 1 - (1 - INACTIVE_INTENSITY) * abFade.value);
  const second = useMemo(() => (secondVfo && !needleHidden
    ? needleGeometry({ bwHz, tuneHz: secondVfo.hz, filterLow: secondVfo.filterLow, filterHigh: secondVfo.filterHigh, width, pxPerHz, hzToX })
    : null), [secondVfo, needleHidden, bwHz, width, pxPerHz, hzToX]);
  const secondCol = secondVfo ? (secondVfo.parked ? '#8a8a8a' : secondVfo.color) : '#000';
  const needleEls = useMemo(() => (<>
      {/* ── ★★ V12 VFO A/B: the other VFO, UNDER the tuned one — passband, edges, a crisp needle ── */}
      {second && (
        <Group opacity={secondOpacity}>
          {second.hiXc > second.loXc && (
            <Rect x={second.loXc} y={BAND_H} width={second.hiXc - second.loXc} height={height - BAND_H}
                  color={hexRgba(secondCol, 0.16)} />
          )}
          {second.loX >= 0 && second.loX <= width && (
            <Rect x={second.loX - 0.5} y={BAND_H} width={1} height={height - BAND_H} color={hexRgba(secondCol, 0.55)} />
          )}
          {second.hiX >= 0 && second.hiX <= width && (
            <Rect x={second.hiX - 0.5} y={BAND_H} width={1} height={height - BAND_H} color={hexRgba(secondCol, 0.55)} />
          )}
          {second.nX >= 0 && second.nX <= width && (
            <Rect x={second.nX - 1} y={0} width={2} height={height} color={secondCol} />
          )}
        </Group>
      )}
      <Group opacity={mainOpacity}>

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
      </Group>
  </>), [width, height, needle, needleStrip, edgeStrip, needleColor, needleFrost,
      wallOverlay, centerMarker, centerMarkerColor, second, secondCol, secondOpacity, mainOpacity]);
  const needleCanvas = useMemo(() => (Platform.OS === 'android' ? null : (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width, height }}>{needleEls}</Canvas>
  )), [width, height, needleEls]);
  const needleInWaterfall = useMemo(() => (Platform.OS === 'android'
    ? <Group transform={[{ translateY: -wfTop }]}>{needleEls}</Group> : null), [needleEls, wfTop]);

  // Static overlay (band plan/ticks/dB lines) memoised as ELEMENTS — when the
  // component re-renders for unrelated reasons React reuses the subtree and
  // skips reconciling dozens of Skia nodes.
  // ★★★ ONLY AS TALL AS WHAT IT DRAWS (the Android diet, 2026-10-11). Everything below ends at the top of the
  //     waterfall — the band strip, the ticks, and the lowest dB line at y = wfTop — yet this canvas was the full
  //     display height. On Android every Skia Canvas is its own TextureView with 2–3 buffers (~10 MB per buffer at
  //     1080 × 2292) and its own composite pass every frame: the Moto G35 showed TWO full-screen surfaces and its
  //     RenderThread at 44 %. Sized to wfTop + 1, like the spectrum canvas beside it; nothing drawn changes.
  const staticOverlayEls = useMemo(() => (
    <Group>
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
    </Group>
  ), [width, height, wfTop, specTop, tickTop, bandSegs, ticks, dbLabels, specShow]);
  // ★★ ANDROID: NO CANVAS OF ITS OWN (the diet, 2026-10-11). Now that it is the spectrum canvas's exact size and place,
  //    its elements draw FIRST inside that canvas instead — one 1080×863 surface fewer to composite every frame and
  //    rebuild on rotation. The cost: its few dozen rects re-draw with the ~30 Hz trace tween, trivial for the GPU next
  //    to a whole extra TextureView. iOS keeps the separate canvas (cheap Metal layers; the trace repaints alone).
  const staticOverlayCanvas = useMemo(() => (
    <Canvas style={{ position: 'absolute', left: 0, top: 0, width, height: Math.min(height, wfTop + 1) }}>
      {staticOverlayEls}
    </Canvas>
  ), [width, height, wfTop, staticOverlayEls]);

  // ── Render ──────────────────────────────────────────────────────────────────
  // Canvas 1 (bottom): waterfall texture only — the ONLY thing the 120Hz
  // Reanimated scroll repaints. Canvas 2 (top): everything else, repainted at
  // the 10Hz data rate. The canvas bounds clip the over-tall scrolling image.
  return (
    <GestureDetector gesture={gesture}>
      <View ref={rootRef} onLayout={onRootLayout} collapsable={false} style={[styles.root, { width, height }]}>

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
          {texReady && lutImage && WF_EFFECT && (wfTiles ? (
            /* ★★ Only where the waterfall can be seen — see deckOcclusion. One paint (the shader), up to four rects.
               ★ antiAlias OFF: the tiles share edges, and an anti-aliased edge on a fractional device pixel would
                 leave a faint seam where two of them meet. Off, the fill rule gives each pixel to exactly one. */
            <Group antiAlias={false}>
              <Shader source={WF_EFFECT} uniforms={wfUniforms}>
                <ImageShader image={wfImage} fit="none" />
                <ImageShader image={lutImage} fit="none" />
              </Shader>
              {wfTiles.map((t, i) => <Rect key={i} x={t[0]} y={t[1]} width={t[2]} height={t[3]} />)}
            </Group>
          ) : (
            <Fill>
              <Shader source={WF_EFFECT} uniforms={wfUniforms}>
                <ImageShader image={wfImage} fit="none" />
                <ImageShader image={lutImage} fit="none" />
              </Shader>
            </Fill>
          ))}
          {needleInWaterfall}
        </Canvas>
        )}
        {/* ★★★ THE SCALE STAMPS ON THEIR OWN CANVAS (Stuart, 2026-10-30, Mac build 531: the waterfall went black at rest and
            flickered back only while it was moved — the history all there). They were drawn in the waterfall's canvas,
            after its shader; whatever a stamp did there took the whole picture with it. Now a separate transparent canvas
            over the same rectangle, mounted only while a stamp exists: the waterfall cannot be blanked by one. */}
        {active && texReady && (
          <StampBoundary>
            <WfStampLayer sink={stampSink} width={width} rowPt={stampRowPt} wfH={wfH} color={stampCol} top={wfTop}
                          uHead={uHead} scrollFrac={scrollFrac} uN={uNSv} uQuant={uQuantDrawSv} />
          </StampBoundary>
        )}

        {active && Platform.OS !== 'android' && staticOverlayCanvas}

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
          {Platform.OS === 'android' && staticOverlayEls}
          {specShow && (
            <Path path={specPath} style="fill"
                  /* ★ antiAlias OFF ON ANDROID: the fill's edge coverage was 3.5 of its 9.5 ms GPU draw on the XCover 4s;
                     the line drawn over it carries the edge, and Stuart could not see the difference (2026-10-12).
                     iOS keeps it — its GPUs never needed the saving, and its look is the reference. */
                  antiAlias={Platform.OS !== 'android'}>
              <LinearGradient start={vec(0, specTop)} end={vec(0, wfTop)}
                              colors={specGradColors} />
            </Path>
          )}
          {specShow && peakHold && (
            <Path path={peakPath} paint={peakPaint} />
          )}
          {/* clipped at wfTop: this canvas overlaps the waterfall's by 1 pt, and the translucent panels must not
              draw twice on that row */}
          {Platform.OS === 'android' && <Group clip={Skia.XYWHRect(0, 0, width, wfTop)}>{needleEls}</Group>}
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
                  left: Math.min(width - 96, centerMarker.x + 3), top: markerLabelBase - 14,
                  width: 96, textAlign: 'left', fontSize: 9,
                }]}>
            {'RF CENTRE: ' + fmtHz(centerMarkerHz as number)}
          </Text>
        )}

        {/* ★★ V12 beta25 — THE DC SPIKE, LABELLED (Stuart, 2026-10-27: "put a little red label on the DC spike, we know where
            it should be"; his wording, two lines). Small and to the LEFT of the spike so it never sits on the RF-centre
            label, which reads to the right of the same place on a dongle. A short red tick marks the exact frequency. */}
        {dcSpikeX !== null && (
          <View pointerEvents="none" accessible accessibilityLabel="DC Spike. Not a real signal."
                style={{ position: 'absolute', left: Math.max(0, dcSpikeX - 92), top: markerLabelBase - 44, width: 92 }}>
            <Text style={[styles.tickLabel, styles.dcSpikeTop, { fontFamily }]}>DC Spike</Text>
            <Text style={[styles.tickLabel, styles.dcSpikeSub, { fontFamily }]}>Not a real signal</Text>
          </View>
        )}
        {dcSpikeX !== null && (
          <View pointerEvents="none" style={[styles.dcSpikeTick, { left: dcSpikeX - 0.5, top: markerLabelBase - 44 }]} />
        )}

        {/* ★★ KNOWN-SIGNAL LABELS (utils/knownSignals.ts): a thin tick at the signal down to the spectrum's floor (beside the
            box on the waterfall when the spectrum is hidden), a bar along a digital mode's span, and the decoder's name on
            RF CENTRE's dark box in a fixed pale lilac — never the VFO colour, which LISTEN uses on the same row. */}
        {knownPlaced && knownPlaced.map((p, i) => {
          const top = p.row === 0 ? markerLabelBase : markerLabelBase - 14;
          const tickBot = specShow && specH > 40 ? wfTop : top + 13;
          const s = p.sig;
          const bx0 = s.lo != null ? Math.max(0, hzToX(s.lo)) : 0, bx1 = s.hi != null ? Math.min(width, hzToX(s.hi)) : 0;
          return (
            <React.Fragment key={'ks' + i + ':' + s.hz}>
              <View pointerEvents="none" style={[styles.knownTick, { left: Math.round(p.x), top, height: Math.max(1, tickBot - top) }]} />
              {bx1 - bx0 >= 3 && (
                <View pointerEvents="none" style={[styles.knownSpan, { left: bx0, width: bx1 - bx0, top: tickBot - 2 }]} />
              )}
              {/* ★ A screen-nav stop when tappable (ScreenNav — Enter = the tap); one id per slot, LABEL_MAX at most. */}
              <View pointerEvents="none" accessible accessibilityLabel={s.label.replace(' · ', ', ')}
                    accessibilityRole={onKnownTap ? 'button' : undefined}
                    accessibilityHint={onKnownTap ? (s.dec ? 'Tunes to it and opens its decoder' : 'Tunes to it') : undefined}
                    onAccessibilityTap={onKnownTap ? () => onKnownTap(s) : undefined}
                    ref={onKnownTap ? screenNavRef('known' + i, () => onKnownTap(s)) : undefined}
                    style={[styles.knownBox, { left: p.lx, top }]}>
                <Text numberOfLines={1} style={[styles.knownText, { fontFamily }]}>{s.label}</Text>
              </View>
            </React.Fragment>
          );
        })}

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
                  left: Math.min(width - 96, needle.nX + 3), top: markerLabelBase,
                  width: 96, textAlign: 'left', fontSize: 9,
                }]}>
            {'LISTEN: ' + fmtHz(tuneHz)}
          </Text>
        )}

        {/* ── ★★ V12 VFO A/B: each needle's letter and the ear it is heard in, at the top of the spectrum ── */}
        {activeVfo && needle && needle.nX >= 0 && needle.nX <= width && (
          <Text pointerEvents="none" numberOfLines={1}
                style={[styles.tickLabel, styles.vfoTag, { fontFamily, color: needleColor,
                  left: Math.max(0, Math.min(width - (activeVfo.dec ? 64 : 34), needle.nX + 3)), top: specTop + 2 }]}>
            {activeVfo.letter + (activeVfo.ear ? ' ' + activeVfo.ear : '') + (activeVfo.dec ? ' DEC' : '')}
          </Text>
        )}
        {secondVfo && second && second.nX >= 0 && second.nX <= width && (
          <Text pointerEvents="none" numberOfLines={1}
                style={[styles.tickLabel, styles.vfoTag, { fontFamily, color: secondCol, opacity: INACTIVE_INTENSITY,
                  left: Math.max(0, Math.min(width - (secondVfo.dec ? 64 : 34), second.nX + 3)), top: specTop + 2 }]}>
            {secondVfo.letter + (secondVfo.ear ? ' ' + secondVfo.ear : '') + (secondVfo.dec ? ' DEC' : '')}
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
                onLayout={(e) => onStationIdHeight?.(e.nativeEvent.layout.height)}
                style={[styles.stationId, { top: specTop + 6, right: stationIdRight },
                        stationIdLeft != null ? { left: stationIdLeft } : null]}>
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
  /* ★★ paddingRight MATCHES THE SESSION CLOCK'S INNER EDGE (12 padding + 1 border, SDRScreen's
     rxClock). Both blocks are pinned to the same `rightInset`, so their BOXES lined up — but the
     clock's numerals sit 13px inside its border while the receiver name ran to the very edge, and
     the eye reads the two text columns, not the invisible box: "the clock seems to be slightly
     offset which doesnt look professional" (Stuart). Indenting the name by the clock's own inset
     puts the name, the locator and the countdown on ONE right-hand column.
     ★ If rxClock's padding or border ever changes, this changes with it — see the note there. */
  stationId: { position: 'absolute', right: 6, alignItems: 'flex-end', paddingRight: 13 },
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
  // ★★ V12 VFO A/B — a needle's letter and ear ("A L", "B R"); a dark halo so it reads over any palette.
  vfoTag: { width: 34, textAlign: 'left', fontSize: 10, textShadowColor: 'rgba(0,0,0,0.85)', textShadowRadius: 3 },
  // ★★ V12 beta25 — the DC spike's label: red, small, a dark halo so it reads over any palette (as vfoTag); in flow inside its box
  dcSpikeTop: { position: 'relative', width: 90, textAlign: 'right', fontSize: 9, fontWeight: 'bold', color: '#ff4d4d',
                textShadowColor: 'rgba(0,0,0,0.85)', textShadowRadius: 3 },
  dcSpikeSub: { position: 'relative', width: 90, textAlign: 'right', fontSize: 8, fontWeight: 'normal', color: 'rgba(255,110,110,0.9)',
                textShadowColor: 'rgba(0,0,0,0.85)', textShadowRadius: 3 },
  dcSpikeTick: { position: 'absolute', width: 1, height: 22, backgroundColor: '#ff4d4d' },
  // ★★ known-signal labels (utils/knownSignals.ts) — RF CENTRE's size and dark box, a fixed pale lilac
  knownTick: { position: 'absolute', width: 1, backgroundColor: KNOWN_LABEL_COLOUR, opacity: 0.8 },
  knownSpan: { position: 'absolute', height: 2, backgroundColor: KNOWN_LABEL_COLOUR, opacity: 0.55 },
  knownBox: { position: 'absolute', height: 13, paddingHorizontal: 3, backgroundColor: KNOWN_LABEL_BOX, justifyContent: 'center' },
  knownText: { fontSize: 9, fontWeight: 'bold', color: KNOWN_LABEL_COLOUR },
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
