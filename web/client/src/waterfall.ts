/**
 * waterfall.ts — canvas waterfall + spectrum trace (browser).
 *
 * This is the one load-bearing piece with no donor: the app renders its
 * waterfall in a Skia SkSL shader (WaterfallView.tsx), which doesn't port. The
 * DSP in front of it does, though — SignalProcessor and the palette LUTs are
 * imported from the app unchanged, so the picture matches the phone.
 *
 * Pipeline per frame:
 *   dBFS bins -> SignalProcessor (EMA, auto-range, peak hold) -> row: u8 0..255
 *             -> peak-preserving downsample to canvas width
 *             -> palette LUT -> ImageData row -> blit at top, scroll down
 *
 * Downsampling takes the MAX of each bucket, not the mean: at 4096 bins into
 * ~1200px a narrow carrier lands in one bin, and averaging would bury it in its
 * own noise floor. Peak-picking keeps it visible.
 */

import { RowPacer } from './wfPacing';
import type { WfScroll } from '../../../src/assets/wfRowTarget.ts';
import { WfRowMerge, dbPerUnit } from '../../../src/assets/wfCombine.ts';
import { SignalProcessor, type SignalProcessorSettings } from '../../../src/assets/signalProcessor';

/** Fixed ring width — see the note at the GPU push. Must never depend on bin count. */
const RING_W = 1024;

/**
 * Resample one row to RING_W.
 * ★ PEAK-HOLD when downsampling, never a mean: a narrow carrier occupies one bin, and
 * averaging it with its quiet neighbours is exactly how a weak signal disappears from the
 * display it exists to reveal. Nearest-neighbour when upsampling — there is no detail to
 * invent, and interpolating would only blur what the sharpening then tries to undo.
 */
function resampleRow(src: Uint8Array, n: number, dst: Uint8Array): Uint8Array {
  if (n === RING_W) return src;
  const step = n / RING_W;
  if (n > RING_W) {
    for (let i = 0; i < RING_W; i++) {
      const a = (i * step) | 0;
      const b = Math.min(n, ((i + 1) * step) | 0) || a + 1;
      let m = 0;
      for (let k = a; k < b; k++) if (src[k] > m) m = src[k];
      dst[i] = m;
    }
  } else {
    for (let i = 0; i < RING_W; i++) dst[i] = src[(i * step) | 0];
  }
  return dst;
}
import { getColorLUT } from '../../../src/assets/colormapUtils';
import { WaterfallGL } from './wfgl';
import { spanChanged, mapRunsX, visibleSpan } from './wfZoomFan';
import { TUNE_EASE_MIN_FRAC } from '../../../src/utils/wfRowPlace.ts';
import { WIDE_TEX, wideExtent, wideRowInto, rowInCapture, percentile, floorAt, floorIdx, cellsOf, denseRow, encodeM, type WideRow, type DenseRow } from './wfWide';
import { newGlideScratch, easedRowAt, joinedRowAt, newRowEase, rowView, type RowView } from './wfGlide';
import { STAMP_MIN_GAP_LABELS, newStampState, stampAlpha, stampJump, stampNoteRow, stampPrune, stampReset, stampSettleRows, stampSweepRows, stampsShown, stampTicks, stampColour, stampTimeLabel }
  from '../../../src/utils/wfStamps.ts';
import { tickStep, fmtTick } from './freqScale';

export interface WaterfallOpts {
  /** Fraction of the canvas given to the spectrum trace (0 = waterfall only). */
  specRatio?: number;
  palette?: string;
}

/** Spectrum share of the canvas. Never 1.0 — the waterfall must survive. */
function clampRatio(r: number): number {
  return Math.max(0, Math.min(0.8, r));
}

/** ★ `#noglow` — MEASUREMENT SWITCH, not a user setting. Turns off the canvas shadowBlur halos
 *  (peak-hold line blur 2, sideband edges blur 8, needle layers 28/16/6) so their per-frame cost
 *  can be measured against the default look. Read once at load, like #nowf / #noaudio in main.ts.
 *  Default (no hash) = the look exactly as before. */
const NO_GLOW = typeof location !== 'undefined' && location.hash.includes('noglow');

/** '#rrggbb' + alpha -> 'rgba(...)'. Mirrors the app's hexRgba(). */
function rgba(hex: string, a: number): string {
  const h = hex.replace('#', '');
  const n = parseInt(h.length === 3 ? h.split('').map(c => c + c).join('') : h, 16);
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
}

/**
 * Canvas pixel ratio — the single biggest lever on render cost, because everything scales with the
 * PIXEL COUNT and dpr 2 is four times the pixels of dpr 1.
 *
 * A waterfall is noisy data, not text, so it survives dpr 1 far better than a UI would — and on a
 * machine that is struggling, four times less work is worth more than crisper noise. Default is
 * capped at 2 (the old behaviour); `setRenderScale` lowers it.
 */
let RENDER_SCALE = 2;
export function setRenderScale(max: number) { RENDER_SCALE = Math.max(1, Math.min(2, max)); }
export function renderDpr(): number { return Math.min(RENDER_SCALE, window.devicePixelRatio || 1); }

/** GPU ring height. Kept generously TALLER than any display so a window resize never reallocates it
 *  (history preserved for free) — the display just shows more/fewer of its newest rows. Grows only if a
 *  display is somehow taller than this. */
const GL_RING_ROWS = 2560;

export class Waterfall {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  /** Off-screen waterfall image; blitted to the visible canvas each frame. Also carries the ring
   *  DIMENSIONS (w × wfH) in GPU mode, where its pixels go unused. */
  private wf: HTMLCanvasElement;
  /** GPU path: ring-texture renderer + its offscreen canvas. Null → CPU (2D) fallback. */
  private gl: WaterfallGL | null = null;
  private glCanvas: HTMLCanvasElement | null = null;
  /** Ring width the GL texture is currently allocated at (= last frame's bin count). */
  private glCols = 0;
  /** Ring head: the row index holding the NEWEST line. Walks backwards; see addRow/draw. */
  private head = 0;
  private wfCtx: CanvasRenderingContext2D;

  private proc = new SignalProcessor();
  private lut: Uint8Array;
  private paletteName: string;

  private rowImg: ImageData | null = null;
  private specRatio: number;


  // ── Temporal line synthesis ────────────────────────────────────────────────
  // The waterfall scrolls at a FIXED rate regardless of how fast frames arrive:
  // between two received rows we synthesise the intermediate lines by blending
  // them. So a server throttled to 5fps still produces a smooth 20-rows/sec
  // waterfall instead of a chunky one — which is what makes the idle power
  // saving free rather than a trade.
  //
  // At 20fps in = 20 rows/sec out, exactly one row per frame and this is a no-op.
  //
  // ★ SCREEN-RELATIVE, NOT pixel-relative. `screenSpeed` is the user's Waterfall Speed in
  // SCREEN rows/sec (10/20/30); `rowsPerSec` is the pixel-row emit rate = screenSpeed × dpr. Without
  // the × dpr, Sharp (dpr 2) has twice the pixel rows for the same screen height, so a fixed pixel
  // rate scrolled the SCREEN at half the speed — which is why "Sharp" felt slow and "Standard" fast
  // (Stuart 2026-07-24, same class of bug as the iPhone portrait scroll). Recomputed on speed or dpr
  // (Detail) change, so scroll speed is now independent of render resolution.
  private screenSpeed = 20;
  private rowsPerSec = 20;
  /** ★★★ SHARP: one waterfall row per RECEIVED FRAME — no synthesised lines, so every row is a
   *  whole frame's integration and the scroll speed IS the data rate. This is what the APP has
   *  always drawn, and why its waterfall looks better at the same data rate.
   *  ★ It changes NOTHING about how a row is rendered — it only stops extra rows being invented
   *    between real ones (emitTotal is forced to 1, the state this already reaches naturally when
   *    the screen speed matches the data rate). The SPECTRUM TRACE is untouched either way: this
   *    governs waterfall row emission only, and the trace must stay smooth at all times. */
  setSharpRows(on: boolean) { this.sharpRows = on; if (!this.scroll) this.pacer.setRate(this.rowsPerSec, on, renderDpr()); }
  /** ★★★ THE SCROLL MODE, and the rows follow the FEED (Stuart 10-20, src/assets/wfRowTarget.ts — the app's rule):
   *  Sharp = actual rows; Default = min(20, 3 x feed); Smooth = min(60, 6 x feed); snapped to a divisor of 60. */
  setScroll(scroll: WfScroll) {
    this.scroll = scroll; this.sharpRows = scroll === 'sharp';
    this.pacer.setScroll(scroll, renderDpr());
  }
  private scroll: WfScroll | null = null;
  private sharpRows = false;



  setSpeed(screenRowsPerSec: number) {
    this.screenSpeed = Math.max(1, screenRowsPerSec);
    this.recomputeRate();
  }
  // ★ framesPerRow = 0: a new rate re-decides the cadence from scratch (its hysteresis is only for
  //   measurement wobble at a FIXED rate — carrying Smooth's N=1 into Default would keep 60 rows/s).
  private recomputeRate() {
    this.rowsPerSec = this.screenSpeed * renderDpr();
    if (this.scroll) this.pacer.setScroll(this.scroll, renderDpr());     // a Detail (dpr) change
    else this.pacer.setRate(this.rowsPerSec, this.sharpRows, renderDpr());
  }
  private prevRow: Uint8Array | null = null;   // last received row
  /** ★ The Hz extent each of prevRow/curRow was measured over (lo edge, span) — drawRow places the history by it. */
  private prevLo = 0; private prevSpan = 0; private curLo = 0; private curSpan = 0;
  /** The row last drawn and the view it was drawn at — the row below the next one (its join, its continued edges). */
  private outRow: Uint8Array | null = null; private outLo = 0; private outSpan = 0;
  /** ★★ THE EASE (wfGlide.ts rowView — the app's zoom/tune ease, in device rows): which view each new row is drawn at. */
  private ease = newRowEase();
  private rv: RowView = { lo: 0, span: 0, jumpHz: 0 };
  /** Per ring row, the span (Hz) it was drawn at — a jump moves every row by the same Hz in ITS OWN scale. 0 = blank. */
  private xfS = new Float64Array(0);
  /** ★★ Per ring row, the Hz at the left edge it was drawn at (with xfS its whole scale) — a tap on an OLD row tunes by
   *  THAT row's scale (xyToHz; Stuart, 2026-10-30, #57). */
  private xfL = new Float64Array(0);
  /** GPU path: per ring row, the full-capture layer's m (where the row's u = 0 sits in the capture, 0..1) — the shader
   *  reads it from its own texture; kept here too so the coverage can be checked (scripts/test-web-waterfall-history.mjs). */
  private xfM = new Float64Array(0);
  /** ★★ THE ZOOM FAN (wfZoomFan.ts): per ring row, the map from screen x to the row's own u (u = xfA·x + xfB). */
  private xfA = new Float64Array(0);
  private xfB = new Float64Array(0);
  /** Extent of the newest row WRITTEN to the ring — the view the history is currently drawn against. */
  private lastLo = 0; private lastSpan = 0;
  private runs = { buf: new Float64Array(80) };
  /** ★★ V12 THE FULL-CAPTURE LAYER (wfWide.ts). Per ring row `xfX` is the layer's key: on the GPU path k (the row's span
   *  ÷ the capture's — the shader's u_capture = k·u + m, m per row in its own texture), on the 2D path the twin's extent X;
   *  0 = no layer. The newest full-capture row (floor + per-cell peaks) and the one before it, for the interpolation in
   *  time between them (every 4th frame — drawn as it came, the layer stepped in blocks of rows); the floor offset that
   *  matches the layer to the detailed rows. */
  private xfX = new Float64Array(0);
  private wide: WideRow | null = null;
  private wideAt = 0;
  private wideGap = 400;
  private wideEver = false;
  private wideOff = 0; private wideOffSet = false;
  private detP50 = NaN;
  private pScratch = { buf: new Float32Array(1024) };
  // the newest and previous rows, per floor bin and per cell (dB, NaN = none), and the blend drawn for one row
  private wFloor = new Float32Array(0); private wFloorP: Float32Array | null = null;
  private wSpread = new Float32Array(0); private wSpreadP: Float32Array | null = null; private bSpread = new Float32Array(0);
  private wCell = new Float32Array(0); private wShape = new Uint8Array(0);
  private wCellP: Float32Array | null = null; private wShapeP: Uint8Array | null = null;
  private bFloor = new Float32Array(0); private bCell = new Float32Array(0); private bShape = new Uint8Array(0);
  // the GPU row: floor indices, peak (level, shape) pairs, the row's m
  private gFloor = new Uint8Array(0); private gPeak = new Uint8Array(0); private gM = new Uint8Array(4);
  // the 2D twin
  private coarseRow = new Uint8Array(WIDE_TEX);
  private dense: DenseRow = { centreHz: 0, spanHz: 0, db: new Float32Array(4096) };
  private fIdx = new Float32Array(0); private cIdx = new Float32Array(0);
  private wfC: HTMLCanvasElement | null = null;
  private wfCCtx: CanvasRenderingContext2D | null = null;
  private rowImgC: ImageData | null = null;
  /** ★ GLIDE (wfGlide.ts): scratch for the in-between rows. */
  private glideScratch = newGlideScratch(1024);
  // ★★ SCALE STAMPS (src/utils/wfStamps.ts — Stuart, 2026-10-30, #57): the old scale laid on the history when a zoom or a
  //    tune lands. rowSeq counts every row drawn (the newest is rowSeq); a stamp sits rowSeq − its row device rows down.
  private stamps = newStampState();
  private rowSeq = 0;
  private stampTickCache = new Map<number, { W: number; lo: number; span: number; dpr: number; ticks: { x: number; label: string | null; w: number }[] }>();
  private curRow: Uint8Array | null = null;    // newest received row
  /** ★★ V12 2026-10-30 (src/assets/wfCombine.ts): rows never drawn before a newer one landed (the thinning's surplus) are
   *  COMBINED into the next row drawn — linear power, peaks kept — not dropped. Stuart: "rather than simply dropping the
   *  excess frames can we average them". Only on the clock; NATIVE never merges. curK: the newest row's dB per unit. */
  private merge = new WfRowMerge();
  private curK = 0;
  private blendRow: Uint8Array | null = null;  // scratch for the synthesised line
  /** ★ Arrival gaps, display rate, cadence and the per-pair row budget — all in wfPacing.ts's
   *  RowPacer, pure, so scripts/test_wf_pacing.ts proves the cadence the screen gets. */
  private pacer = new RowPacer();

  // Geometry of the last frame — used for click-to-tune and the axis.
  centerHz = 0;
  spanHz = 0;
  /** Hook so the host can draw the dB axis over the spectrum region. */
  onDrawAxis: ((ctx: CanvasRenderingContext2D, w: number, h: number) => void) | null = null;

  /** VFO marker position, Hz (the needle). */
  vfoHz = 0;
  /** ★ Appended to the RF CENTRE label — e.g. " — controlled by VibeClarity" when the suite is
   *  choosing where the tuner sits, so a centre that moves on its own explains itself. */
  rfCenterNote = '';
  /** Demod passband relative to the VFO, Hz — drives the acrylic sidebands.
   *  Negative low = below the carrier (SSB sits entirely on one side). */
  filterLow = 0;
  filterHigh = 0;

  /** Unlocked view only: the pan limits (beyond them is dead capture) and the RF
   *  centre — where the dongle actually is, vs where you're looking. Null = hide,
   *  which is what LOCK does. */
  wallLoHz: number | null = null;
  wallHiHz: number | null = null;
  rfCenterHz: number | null = null;
  /** Edges of the CAPTURED band right now (dongle ± Fs/2). Distinct from the
   *  walls: the walls are how far you may ever pan, this is what the radio is
   *  actually receiving at this moment — outside it there is no spectrum, and
   *  panning there forces the RF centre to move. */
  captureLoHz: number | null = null;
  captureHiHz: number | null = null;
  /** Timestamp of the last time a pan was CLAMPED at a wall. The wall flashes for
   *  a moment so you know why the view stopped moving — otherwise, zoomed in with
   *  the wall off-screen, the drag just silently stops dead. */
  wallHitAt = 0;
  wallHitSide: 'lo' | 'hi' | null = null;

  /** Spectrum trace visible? (The waterfall keeps the space either way.) */
  // ★ Hiding the trace COLLAPSES the split — it does not merely skip the draw. Gating only the
  // draw left the spectrum's slice of canvas reserved and painted flat black, so "trace off" gave
  // a dead strip above the waterfall. Dragging the ratio slider to 0 already did the right thing,
  // which made these two controls disagree about what "no spectrum" means. Now they are one
  // mechanism: the user's chosen ratio is remembered and restored when the trace comes back.
  private _showSpec = true;
  get showSpec() { return this._showSpec; }
  set showSpec(v: boolean) {
    if (this._showSpec === v) return;
    this._showSpec = v;
    this.resize();
  }
  /** The split actually used for layout — zero while the trace is hidden. */
  private effectiveRatio(): number { return this._showSpec ? this.specRatio : 0; }
  /** ★ Where the spectrum ends and the waterfall begins, as a fraction of the wrapper's height.
   *  Published because the IF-passband overlay hangs its label on that join — see updateIfGap. */
  specJoinFrac(): number { return this.effectiveRatio(); }
  /** Fill opacity of the trace (the app's bgOpacity). */
  specAlpha = 0.85;

  /** VFO needle colour — also used for the peak-hold line, as in the app. */
  vfoColor = '#ff2020';
  /** 1–10 halo brightness (5 = the app's original look). */
  vfoIntensity = 5;
  /** 0–10 smoked-glass backing under the passband (0 = off). */
  vfoFrost = 0;
  /** Latest normalised spectrum trace + peak hold, and the previous pair — the
   *  trace is blended between them so it GLIDES between server frames instead of
   *  stepping. This matters more than the waterfall: slow waterfall rows just
   *  read as texture, but a live trace visibly jumps. */
  private spec: Float32Array | null = null;
  private peak: Float32Array | null = null;
  /** Scratch for the ring resample — allocated once, never per frame. */
  private ringBuf = new Uint8Array(RING_W);
  private prevSpec: Float32Array | null = null;
  private prevPeak: Float32Array | null = null;
  private drawSpec: Float32Array | null = null;
  private drawPeak: Float32Array | null = null;

  constructor(canvas: HTMLCanvasElement, opts: WaterfallOpts = {}) {
    this.canvas = canvas;
    // NB `desynchronized: true` was tried here and REVERTED — it is a low-latency surface hint,
    // it did not move Chromium's CPU or GPU figures at all, and it has a history of odd behaviour
    // on macOS. Unproven changes do not earn a place in the render path.
    this.ctx = canvas.getContext('2d', { alpha: false })!;
    this.wf = document.createElement('canvas');
    this.wfCtx = this.wf.getContext('2d', { alpha: false })!;
    this.head = 0;
    // Clamp here too: a bad value (e.g. a raw 0-60 slider position mistaken for a
    // fraction) would drive the waterfall height to zero and silently eat it.
    this.specRatio = clampRatio(opts.specRatio ?? 0.25);
    this.paletteName = opts.palette ?? 'gqrx';
    this.lut = getColorLUT(this.paletteName);
    // Try the GPU path. If WebGL isn't available (ancient browser/device) we silently keep the 2D
    // path, which is the right fallback anyway — those devices don't want a heavy sharp waterfall.
    try {
      this.glCanvas = document.createElement('canvas');
      this.gl = new WaterfallGL(this.glCanvas);
      this.gl.sharpness = this.proc.getSettings().wfSharpness;   // a saved pref must apply at start
      this.gl.setLUT(this.lut);
    } catch { this.gl = null; this.glCanvas = null; }
    this.resize();
  }

  setPalette(name: string) {
    this.paletteName = name;
    this.lut = getColorLUT(name);
    this.gl?.setLUT(this.lut);
    this.specGrad = null;   // gradient is built from the LUT — rebuild it
  }

  /** Vertical gradient sampled from the palette, so the spectrum trace is
   *  coloured by the same LUT as the waterfall: floor colour at the bottom,
   *  peak colour at the top. Cached — rebuilt only on palette/height change. */
  private specGrad: CanvasGradient | null = null;
  private specGradH = 0;

  private specGradient(ctx: CanvasRenderingContext2D, H: number): CanvasGradient {
    if (this.specGrad && this.specGradH === H) return this.specGrad;
    const g = ctx.createLinearGradient(0, 0, 0, H);
    const lut = this.lut;
    // Sample the LUT 90 -> 235 in 9 stops, NOT 0 -> 255 (the app does exactly
    // this, WaterfallView specGradColors). Black-based palettes — Sonar, Night
    // Vision — are near-invisible below index ~90, so a fill that starts at 0
    // fades into the black background and the base of the trace disappears. The
    // baseline instead starts where the palette has actually picked up colour, so
    // weak signals stay visible while the trace still inherits the waterfall hue.
    for (let gi = 0; gi <= 8; gi++) {
      const idx = Math.max(0, Math.min(255, Math.round(90 + (gi / 8) * 145)));
      const o = idx << 2;
      // Gradient runs top->bottom, hot colour at the top.
      g.addColorStop(1 - gi / 8, `rgb(${lut[o]},${lut[o + 1]},${lut[o + 2]})`);
    }
    this.specGrad = g;
    this.specGradH = H;
    return g;
  }

  /** Palette colour at the top of the range — used for the trace outline. */
  private peakColour(): string {
    const o = 255 << 2;
    return `rgb(${this.lut[o]},${this.lut[o + 1]},${this.lut[o + 2]})`;
  }
  get palette() { return this.paletteName; }

  setSpecRatio(r: number) {
    this.specRatio = clampRatio(r);
    this.resize();
  }

  getSpecRatio(): number { return this.specRatio; }

  applySettings(patch: Partial<SignalProcessorSettings>) {
    this.proc.applySettings(patch);
    // ★ THE SHARPNESS SLIDER USED TO GO NOWHERE. It was stored in the processor's
    // settings and never read by this renderer, so the control moved and nothing
    // happened — while the app, which sharpens in its SkSL shader, looked visibly
    // crisper on the same signal (Stuart, 2026-07-28). The GPU path now takes it.
    if (patch.wfSharpness !== undefined && this.gl) this.gl.sharpness = patch.wfSharpness;
    // ★★ AND CONTRAST — the same omission, one slider along. Stored in the processor and never
    // read here, so the manual contrast control did nothing from end to end (Stuart, 2026-07-31,
    // seen on both the Pi and Android because they share this client). If a third slider is ever
    // added, it needs a line HERE as well as a setting: the processor is a store, not a renderer.
    if (patch.wfContrast !== undefined && this.gl) this.gl.contrast = patch.wfContrast;
  }
  getSettings(): SignalProcessorSettings { return this.proc.getSettings(); }
  getRange() { return this.proc.getRange(); }

  resize() {
    const dpr = renderDpr();
    const w = Math.max(1, Math.round(this.canvas.clientWidth * dpr));
    const h = Math.max(1, Math.round(this.canvas.clientHeight * dpr));
    const wfH = Math.max(1, Math.round(h * (1 - this.effectiveRatio())));

    // NB: do NOT early-out on canvas size alone. Changing the spectrum/waterfall
    // split leaves the canvas exactly the same size and only moves the boundary
    // inside it — an early-out here is what made the split slider do nothing.
    if (this.canvas.width === w && this.canvas.height === h
        && this.wf.height === wfH && this.wf.width === w) return;

    this.canvas.width = w;
    this.canvas.height = h;
    // GPU path: the ring texture (glCols × wfH) is the history; re-allocate it at the new height,
    // which preserves the existing waterfall (WaterfallGL.ensureRing renders the old ring into the
    // new). this.wf is only kept for its DIMENSIONS here — its 2D pixels go unused.
    if (this.gl) {
      // Only this.wf's DIMENSIONS matter here (w × wfH); the ring texture is taller and is NOT touched
      // on resize, so the waterfall history survives a window drag. head is left alone. If the display
      // grew taller than the ring, drawRow grows the ring on the next row.
      this.wf.width = w;
      this.wf.height = wfH;
      this.rowImg = null;
      return;
    }
    // Preserve history across a resize where we can — a re-layout shouldn't
    // wipe the waterfall.
    const old = this.wf.width && this.wf.height
      ? this.wfCtx.getImageData(0, 0, this.wf.width, this.wf.height) : null;
    this.wf.width = w;
    this.wf.height = wfH;
    this.wfCtx.fillStyle = '#000';
    this.wfCtx.fillRect(0, 0, w, wfH);
    if (old) {
      // Unwrap the ring as we copy, so the preserved history lands in reading order and the head
      // can start again at 0. Keeping a wrapped buffer across a RESIZE would need the old head and
      // the old height to stay consistent with the new ones — needless bookkeeping for a rare event.
      const tmp = document.createElement('canvas');
      tmp.width = old.width; tmp.height = old.height;
      const tc = tmp.getContext('2d')!;
      tc.putImageData(old, 0, 0);
      const oh = old.height;
      const firstRows = oh - this.head;
      if (firstRows > 0) this.wfCtx.drawImage(tmp, 0, this.head, old.width, firstRows,
                                              0, 0, w, wfH * (firstRows / oh));
      if (this.head > 0)  this.wfCtx.drawImage(tmp, 0, 0, old.width, this.head,
                                              0, wfH * (firstRows / oh), w, wfH * (this.head / oh));
      // ★ The zoom fan's maps follow their rows into the unwrapped, re-heighted buffer (row j ← reading row j·oh/wfH).
      if (this.xfA.length === oh) {
        const a = new Float64Array(wfH), b = new Float64Array(wfH), x = new Float64Array(wfH), sp = new Float64Array(wfH), lo = new Float64Array(wfH);
        const hasX = this.xfX.length === oh, hasS = this.xfS.length === oh, hasL = this.xfL.length === oh;
        for (let j = 0; j < wfH; j++) { const r = (this.head + Math.min(oh - 1, Math.floor(j * oh / wfH))) % oh; a[j] = this.xfA[r]; b[j] = this.xfB[r]; x[j] = hasX ? this.xfX[r] : 0; sp[j] = hasS ? this.xfS[r] : 0; lo[j] = hasL ? this.xfL[r] : 0; }
        this.xfA = a; this.xfB = b; this.xfX = x; this.xfS = sp; this.xfL = lo;
      }
      // ★ V12 and the full-capture layer's rows follow theirs, the same way (unwrapped, re-heighted).
      if (this.wfC && this.wfCCtx && this.wfC.height === oh) {
        const tc2 = document.createElement('canvas'); tc2.width = WIDE_TEX; tc2.height = oh;
        tc2.getContext('2d')!.drawImage(this.wfC, 0, 0);
        this.wfC.height = wfH;
        this.wfCCtx.fillStyle = '#000'; this.wfCCtx.fillRect(0, 0, WIDE_TEX, wfH);
        if (firstRows > 0) this.wfCCtx.drawImage(tc2, 0, this.head, WIDE_TEX, firstRows, 0, 0, WIDE_TEX, wfH * (firstRows / oh));
        if (this.head > 0) this.wfCCtx.drawImage(tc2, 0, 0, WIDE_TEX, this.head, 0, wfH * (firstRows / oh), WIDE_TEX, wfH * (this.head / oh));
        this.rowImgC = this.wfCCtx.createImageData(WIDE_TEX, 1);
      }
    }
    // ★ The scale stamps follow their rows into the re-heighted buffer (row j ← reading row j·oh/wfH), as the maps do.
    if (old && old.height > 0) {
      const oh = old.height;
      for (const st of this.stamps.stamps) {
        st.frame = this.rowSeq - Math.round((this.rowSeq - st.frame) * wfH / oh);
        st.born = this.rowSeq - Math.round((this.rowSeq - st.born) * wfH / oh);
      }
      this.stamps.refFrame = Math.min(this.stamps.refFrame, this.rowSeq);
    } else stampReset(this.stamps);
    this.head = 0;
    // drawRow() bails without this, so the waterfall silently never draws.
    this.rowImg = this.ctx.createImageData(w, 1);
  }

  /**
   * Wipe the waterfall history to black.
   *
   * History is frequency-indexed and deliberately never re-aligned (see draw()),
   * which is right for pan/zoom — old rows stay meaningful. But after a
   * DISCONTINUOUS retune (648 kHz → 96.6 MHz) every stored row belongs to a
   * band you're no longer looking at, and at ~18 rows/sec a full-height
   * waterfall takes ~30 seconds to scroll itself clean. That reads as "the
   * spectrum froze and wouldn't zoom out" — it hadn't; it was showing valid but
   * utterly stale history. Clearing on a jump makes new rows appear at once.
   */
  clearHistory() {
    if (!this.wf.width || !this.wf.height) return;
    if (this.gl) this.gl.clear();
    else { this.wfCtx.fillStyle = '#000'; this.wfCtx.fillRect(0, 0, this.wf.width, this.wf.height); }
    this.head = 0;      // an all-black ring has no meaningful head
    this.xfA.fill(1); this.xfB.fill(0);   // a blank ring has nothing to fan
    this.xfX.fill(0); this.xfS.fill(0); this.xfL.fill(0);
    this.ease = newRowEase(); this.outRow = null;   // ★ the next row starts afresh: nothing below it to join to or ease from
    stampReset(this.stamps); this.stampTickCache.clear();   // ★ no history, nothing to label
    this.wide = null; this.wFloorP = null; this.wCellP = null;   // ★ a jump: the old full-capture row is another band
    if (this.wfCCtx && this.wfC) { this.wfCCtx.fillStyle = '#000'; this.wfCCtx.fillRect(0, 0, this.wfC.width, this.wfC.height); }
  }

  /** ★★ V12 a full-capture row (SPWB) arrived — the layer under the history (wfWide.ts). Not held with the detailed rows:
   *  it is only ever the newest picture of the rest of the band. */
  pushWide(row: WideRow) {
    const now = performance.now(), old = this.wide;
    const F = row.floor.length, C = row.cells;
    const same = !!old && old.centreHz === row.centreHz && old.spanHz === row.spanHz && this.wFloor.length === F && this.wCell.length === C;
    if (same) {
      // ★ from what is on screen NOW (the blend part-way), so a row that comes early does not jump
      if (!this.wFloorP || this.wFloorP.length !== F) { this.wFloorP = new Float32Array(F); this.wSpreadP = new Float32Array(F); this.wCellP = new Float32Array(C); this.wShapeP = new Uint8Array(C); }
      if (this.bFloor.length === F && this.bCell.length === C) { this.wFloorP.set(this.bFloor); this.wSpreadP!.set(this.bSpread); this.wCellP!.set(this.bCell); this.wShapeP!.set(this.bShape); }
      else { this.wFloorP.set(this.wFloor); this.wSpreadP!.set(this.wSpread); this.wCellP!.set(this.wCell); this.wShapeP!.set(this.wShape); }
      const gap = now - this.wideAt;
      if (gap > 0 && gap < 5000) this.wideGap += (gap - this.wideGap) * 0.3;
    } else { this.wFloorP = null; this.wSpreadP = null; this.wCellP = null; this.wShapeP = null; }
    if (this.wFloor.length !== F) { this.wFloor = new Float32Array(F); this.bFloor = new Float32Array(F); this.gFloor = new Uint8Array(F);
      this.wSpread = new Float32Array(F); this.bSpread = new Float32Array(F); }
    if (this.wCell.length !== C) { this.wCell = new Float32Array(C); this.wShape = new Uint8Array(C); this.bCell = new Float32Array(C); this.bShape = new Uint8Array(C); this.gPeak = new Uint8Array(2 * C); }
    this.wFloor.set(row.floor); this.wSpread.set(row.spread);
    cellsOf(row, this.wCell, this.wShape);
    this.wide = row; this.wideAt = now;
    this.wideEver = true;
    // ★★ THE FLOOR MATCH: the layer's floor (medians of the wide FFT) against the detailed rows' own (the MEDIAN of the
    //    latest row) over the frequencies the view covers — so a row reads as ONE row across the detailed strip's edge,
    //    through the detailed rows' own palette mapping. Followed slowly so the outside does not flicker.
    if (this.curSpan > 0 && this.detP50 === this.detP50) {
      const wLo = row.centreHz - row.spanHz / 2;
      let sum = 0, n = 0;
      for (let i = 0; i < 16; i++) {
        const f = floorAt(row.floor, (this.curLo + (i + 0.5) / 16 * this.curSpan - wLo) / row.spanHz);
        if (f === f) { sum += f; n++; }
      }
      if (n >= 8) {
        const off = Math.max(-30, Math.min(30, this.detP50 - sum / n));
        this.wideOff = this.wideOffSet ? this.wideOff + (off - this.wideOff) * 0.3 : off;
        this.wideOffSet = true;
      }
    }
  }

  /** ★ For the tests: the layer's floor (dB, with its offset) against the detailed rows' median over the same view, and
   *  over the part of the capture OUTSIDE the view (a flat band must read the same there). */
  layerFloorCheck(): { det: number; inView: number; outside: number } | null {
    const w = this.wide;
    if (!w || !(this.curSpan > 0)) return null;
    const wLo = w.centreHz - w.spanHz / 2, at = (f: number) => floorAt(w.floor, (f - wLo) / w.spanHz) + this.wideOff;
    let a = 0, na = 0, b = 0, nb = 0;
    for (let i = 0; i < 16; i++) { const v = at(this.curLo + (i + 0.5) / 16 * this.curSpan); if (v === v) { a += v; na++; } }
    for (let i = 0; i < 64; i++) { const f = wLo + (i + 0.5) / 64 * w.spanHz;
      if (f > this.curLo - this.curSpan && f < this.curLo + 2 * this.curSpan) continue;
      if (f < wLo + w.spanHz * 0.1 || f > wLo + w.spanHz * 0.9) continue;   // the capture's roll-off skirts
      const v = at(f); if (v === v) { b += v; nb++; } }
    return { det: this.detP50, inView: na ? a / na : NaN, outside: nb ? b / nb : NaN };
  }

  /** ★ V12 the band the history holds rows for beyond the view — the newest full-capture row's extent (null = none fresh). */
  captureWindow(): { lo: number; hi: number } | null {
    const w = this.wide;
    if (!w || performance.now() - this.wideAt > 4000) return null;
    return { lo: w.centreHz - w.spanHz / 2, hi: w.centreHz + w.spanHz / 2 };
  }

  /** Feed one raw dBFS frame. Rows are NOT drawn here — see tick(). */
  /** ★★★ THE SPECTRUM JITTER BUFFER — rows are HELD briefly, then released on the display clock.
   *
   *  The pacing below already covers uneven arrivals WITHOUT latency: it sizes each pair against
   *  the WORST recent gap (`slowGap`) and synthesises interpolated rows to bridge it. That is
   *  genuinely a jitter buffer's job done for free, and it is why this was left alone for so long.
   *
   *  ★★ WHERE IT RUNS OUT. `slowGap` decays (x0.98 per arrival, so ~3 s of memory). An ISOLATED
   *     long hole after a quiet spell finds it sized for the quiet spell: measured on the public
   *     demo through the tunnel, arrival gaps to 248 ms against a slowGap of ~65 ms, so the
   *     synthesised rows run out ~65 ms in and the scroll STOPS for the remaining ~180 ms. That
   *     stall is the "sticky waterfall" — not a slow link, a bridge built too short.
   *
   *  ★★★ AND HOLDING ROWS IS THE A/V SYNC FIX, NOT A COST TO IT. The audio carries its own
   *      cushion (see audio.ts JITTER_SEC), so the picture has always run AHEAD of the sound by
   *      exactly that much. Delaying the waterfall by the same amount is the long-standing
   *      "delay the WATERFALL" item — one change, two problems, and they pull in the SAME
   *      direction. Set this from the audio path's adapted depth and they stay locked together.
   *
   *  ★ Default 0 = the previous behaviour exactly, so a receiver on a LAN pays nothing.
   */
  private holdMs = 0;
  private held: { bins: Float32Array; centerHz: number; bwHz: number; at: number }[] = [];

  /** Match the picture's delay to the sound's. Called with the audio buffer's current depth. */
  setHoldMs(ms: number) { this.holdMs = Math.max(0, Math.min(500, ms)); }

  /** ★★★ AND GET OUT OF THE WAY WHILE THE DIAL IS MOVING. The first attempt at this froze the
   *  display completely while tuning (Stuart, 2026-08-10) and the reason is structural, not a
   *  detail: a hold delays every row by holdMs, and a retune must discard rows computed at the old
   *  centre. If the centre changes FASTER than holdMs — which is exactly what dragging is — no row
   *  ever survives long enough to be released, so the picture stops until the dial does.
   *  ★★ Tagging rows with their centre does NOT fix that, which was my first idea: the freshest
   *     row is still holdMs away from being drawn. The only thing that works is to stop holding
   *     while the user is interacting, which is also the moment smoothing matters least — nobody
   *     is judging the noise floor mid-drag, they are watching the dial.
   *  ★ So: a retune drops what is queued AND opens a passthrough window. Rows go straight to the
   *    display, exactly as they did before any of this existed, until the dial has been still for
   *    TUNE_PASSTHROUGH_MS. */
  private passthroughUntil = 0;

  /** ★ Recycled hold-queue buffers. push() used to allocate a fresh Float32Array copy of every
   *  frame (~20/s × bins × 4 bytes) for the GC to collect a moment later; a queue of at most
   *  `cap` entries only ever needs that many buffers. Trade-off: a few bins-sized buffers stay
   *  resident (≤ HELD_POOL_MAX) in exchange for no per-frame allocation / GC churn. */
  private heldPool: Float32Array[] = [];
  private static readonly HELD_POOL_MAX = 32;
  private _heldCopy(src: Float32Array): Float32Array {
    let b = this.heldPool.pop();
    if (!b || b.length !== src.length) b = new Float32Array(src.length);   // bin count changed → drop it
    b.set(src);
    return b;
  }
  private _heldRecycle(b: Float32Array) {
    if (this.heldPool.length < Waterfall.HELD_POOL_MAX) this.heldPool.push(b);
  }

  flushHeld() {
    for (const f of this.held) this._heldRecycle(f.bins);
    this.held.length = 0;
    this.passthroughUntil = performance.now() + 400;
  }

  push(bins: Float32Array, centerHz: number, bwHz: number) {
    // ★ Interacting: straight through, no queue, no delay. This is the pre-buffer code path.
    if (this.holdMs > 0 && performance.now() < this.passthroughUntil) {
      this._pushNow(bins, centerHz, bwHz);
      return;
    }
    if (this.holdMs > 0) {
      // ★ COPY. `bins` is a reused buffer owned by the caller — queueing the reference would hand
      //   the renderer whatever happened to be in it by release time, which is the NEXT row.
      this.held.push({ bins: this._heldCopy(bins), centerHz, bwHz,
                       at: performance.now() });
      // Bound it: a link that stalls for a second must not queue a second of catch-up to grind
      // through afterwards. Oldest goes first — it is the stalest picture.
      const cap = Math.max(4, Math.ceil(this.holdMs / 20) + 4);
      while (this.held.length > cap) this._heldRecycle(this.held.shift()!.bins);
      return;
    }
    this._pushNow(bins, centerHz, bwHz);
  }

  /** Release any held rows now due. Called from tick(), so it runs on the display clock.
   *
   *  ★★★ THE DEPTH RAMPS IN, it is never applied in one step. Coming out of a passthrough window
   *      the queue is EMPTY, so switching straight to a full hold would stall the picture for
   *      holdMs while it filled — a fresh stutter after every tune, which is the complaint this
   *      is supposed to cure. Growing the effective hold at ~15% of real time means each row waits
   *      a little longer than the last: the waterfall scrolls ~15% slow for about a second and
   *      then sits at full depth, and nothing ever stops. Sub-perceptual, and no gap.
   */
  private _effectiveHold(now: number) {
    const since = now - this.passthroughUntil;      // >0 once the dial has been still
    if (since <= 0) return 0;
    return Math.min(this.holdMs, since * 0.15);
  }

  private _releaseHeld(now: number) {
    const eff = this._effectiveHold(now);
    while (this.held.length && now - this.held[0].at >= eff) {
      const f = this.held.shift()!;
      this._pushNow(f.bins, f.centerHz, f.bwHz);
      // ★ Safe to reuse: proc.process() copies what it keeps (dbAvg/specSmooth .set) and holds no
      //   reference to `bins` after it returns.
      this._heldRecycle(f.bins);
    }
  }

  private _pushNow(bins: Float32Array, centerHz: number, bwHz: number) {
    this.centerHz = centerHz;
    this.spanHz = bwHz;

    const frame = this.proc.process(bins, centerHz, bwHz);
    // ★ V12 the full-capture layer's floor match (pushWide) — only once a server has sent one.
    if (this.wideEver) this.detP50 = percentile(bins, 0, bins.length, 0.5, this.pScratch);

    // Roll the trace: keep the OLD frame so draw() can blend towards the new one.
    if (!this.spec || this.spec.length !== frame.spec.length) {
      this.prevSpec = new Float32Array(frame.spec);
      this.prevPeak = new Float32Array(frame.peak);
      this.spec = new Float32Array(frame.spec);
      this.peak = new Float32Array(frame.peak);
      this.drawSpec = new Float32Array(frame.spec.length);
      this.drawPeak = new Float32Array(frame.peak.length);
    } else {
      this.prevSpec!.set(this.spec);
      this.prevPeak!.set(this.peak!);
      this.spec.set(frame.spec);
      this.peak!.set(frame.peak);
    }

    const now = performance.now();
    const row = frame.row;

    // ★★★ DROP THE UNEMITTED REMAINDER — DO NOT FLUSH IT. This used to dump up to EIGHT leftover
    //     rows in one go so that "every pair contributes exactly emitTotal lines", and that burst
    //     IS the judder: a frame arriving a little early emptied the queue in a single animation
    //     frame, so the waterfall lurched forward and then resumed. Over-generating and DISCARDING
    //     the excess is the point (Stuart, 2026-08-03) — the rows we drop were synthetic anyway,
    //     and dropping them costs nothing real.
    //     ★ The original worry that motivated the flush — an early frame "eating its own lines" so
    //       the waterfall stalls — is handled instead by SIZING for the worst gap below, which
    //       guarantees there are always enough rows to reach the next arrival.

    // ★★ An undrawn cur joins the pending group before it rolls to prev (wfCombine.ts WfRowMerge).
    this.merge.arrive(this.pacer.clocked, this.curRow, this.curLo, this.curSpan, this.curK);
    this.curK = dbPerUnit(frame.dbMin, frame.dbMax);
    // Roll cur -> prev, and copy in the new row (frame.row is a reused buffer).
    if (!this.curRow || this.curRow.length !== row.length) {
      this.prevRow = new Uint8Array(row);
      this.curRow = new Uint8Array(row);
      this.blendRow = new Uint8Array(row.length);
      this.prevLo = this.curLo = centerHz - bwHz / 2;
      this.prevSpan = this.curSpan = bwHz;
    } else {
      this.prevRow!.set(this.curRow);
      this.curRow.set(row);
      this.prevLo = this.curLo; this.prevSpan = this.curSpan;
      this.curLo = centerHz - bwHz / 2; this.curSpan = bwHz;
    }

    // How many lines to synthesise before the next frame lands. Derived from the
    // OBSERVED arrival gap, so it adapts to whatever rate the server is running —
    // no need to be told, and it self-corrects across a throttle change.
    // ★★ HEADROOM: size against the WORST recent gap, not this one. Server frames do not arrive
    //    evenly, and a pair sized for an average gap runs out of rows on a long one — the scroll
    //    stops dead until the next frame. Jumps straight to any slower gap and decays back gently,
    //    so it tracks the worst case rather than the mean; the surplus on a normal gap is simply
    //    discarded above.
    // ★★★ PACE OFF THE SMOOTHED GAP, NOT THIS ONE. The row interval was derived from the
    //     INSTANTANEOUS arrival gap, so a server frame arriving 10 ms early or late changed the
    //     scroll rate for that whole pair — visible as the waterfall speeding up and slowing down
    //     again, which is exactly the band Stuart photographed. The feed genuinely varies ("the
    //     server dropping or increasing the odd frame here or there"), so the renderer must not
    //     follow it frame by frame: it follows the AVERAGE and lets the row accumulator carry the
    //     difference. That is what a jitter buffer does, without the added latency of one.
    // Clamp: a stalled link mustn't queue up hundreds of lines to catch up on.
    // ★★★ ROWS TO SYNTHESISE FOR THIS PAIR, TO HIT THE TARGET ROW RATE. Derived from the OBSERVED
    //     gap, and that is correct HERE: this renderer draws rows as they are produced and keeps no
    //     history mapping, so the count may vary per pair without rescaling anything.
    //     ★ The APP is the opposite — its shader maps ALL history through lines-per-frame, so there
    //       the number must be a CONSTANT or the picture rescales. Same feature, opposite rule,
    //       because the renderers are not the same. Do not "unify" these two without re-reading
    //       this: giving the web client the app's fixed count made it draw 80 rows/s on a 20 fps
    //       feed and judder (2026-08-03).
    // ★★★ THE ROW COUNT AND THE FRAME CADENCE MUST COME FROM THE SAME NUMBER. They used to be
    //     worked out independently: the cadence quantises to one row every N display frames
    //     (refresh/N rows per second), while the count per pair was computed from the RAW target.
    //     At 40 device rows/s on 60 Hz the cadence rounds to N=2, i.e. 30 rows/s — but each pair
    //     still queued rows for 40/s, so a quarter of them were dropped at every arrival, and
    //     unevenly. That is the DEFAULT-only hitch (Stuart, 2026-08-03: "all rates need to be tied
    //     to the screen refresh rate"). Snap the rate to what the display can actually deliver
    //     FIRST, then size the pair from that. SHARP is exempt: one row per frame, no cadence.
    // ★★★ 2026-10-20: the refresh is the NOMINAL panel rate from the MEDIAN interval, and N keeps
    //     hysteresis — 60 Hz / 40 rows/s is exactly 1.5, and a raw EMA put N on that knife-edge
    //     (1 ↔ 2 every arrival: 43-60 rows/s, 24 hitches/s, Stuart's "sticky" Default on Safari).
    //     The pair is sized with ceil(): round() ran it dry a frame early on a gap just over the
    //     worst seen. Both in wfPacing.ts, proved by scripts/test_wf_pacing.ts.
    // Smoothed, so arrival jitter does not become scroll-rate jitter. Falls back to the observed
    // gap until the average has anything in it.
    // ★★★ THE INTERACTION BOOST (wfPacing.ts RowPacer.noteMove — wfClock.ts boostRowRate): a frame placed somewhere new (a
    //     tune or zoom, ours or a shared dial's) steps the clock's row rate up until the move has settled. Nothing is
    //     asked of the server; the extra rows are this frame, drawn at their in-between views.
    if (this.prevSpan > 0 && this.curSpan > 0 &&
        (spanChanged(this.prevSpan, this.curSpan) || Math.abs(this.curLo - this.prevLo) > this.curSpan * TUNE_EASE_MIN_FRAC))
      this.pacer.noteMove(now);
    this.pacer.onArrival(now);
  }

  /** Emit any waterfall lines now due. Call once per animation frame.
   *  At 20fps in this draws exactly one row per frame (emitTotal === 1) and the
   *  blending collapses to a straight copy of the newest row. */
  tick(frameTs?: number) {
    // ★ Release BEFORE the early-return: on a cold start there is no curRow yet, and the very
    //   first held row is the one that creates it. Draining after the guard would deadlock — no
    //   row because no curRow, no curRow because no row.
    const now = performance.now();
    if (this.held.length) this._releaseHeld(now);
    // (No early return before the pacer: it times the display even before the first row lands.)

    // ★★★ PACED BY VSYNC, NOT BY THE CLOCK. tick() is already called from requestAnimationFrame,
    //     but the EMISSION used to ask "is row k due yet?" against wall-clock times. At 20 rows/s
    //     on a 60 Hz display that wants a row every 3 frames — and whenever a due-time drifted
    //     across a frame boundary it became 3, 3, 4, 3, 4… That beat between the row rate and the
    //     refresh rate is the residual hitching (Stuart, 2026-08-03: "can we not tie it to device
    //     Vsync?").
    //     Now: accumulate rows-per-FRAME and emit whole rows as they come due. The cadence is
    //     locked to the display, so it is even by construction and adapts to 120 Hz for free.

    // ★★★ ONE ROW EVERY N DISPLAY FRAMES — N A WHOLE NUMBER. This is the crux, and it is why every
    //     pacing scheme before it failed. A waterfall row is a whole DEVICE pixel, and the row rate
    //     is screenSpeed x devicePixelRatio: 20 rows/s on a Retina panel is 40 device rows/s, which
    //     against a 60 Hz refresh is 0.667 rows per frame — so the emission pattern is 1, 1, 0,
    //     1, 1, 0… No amount of smoothing fixes that, because the QUANTITY is not commensurate with
    //     the display. Quantising the cadence to a whole number of frames makes the motion even by
    //     construction, at the cost of the rate landing on 60/N (30, 20, 15, 12…) instead of
    //     exactly what was asked for — a difference nobody can see, unlike the stutter.
    //     ★ Recomputed from the MEDIAN frame interval snapped to the panel's nominal rate (RowPacer),
    //       so a 120 Hz panel simply gets N twice as large and the same even result.
    // Decided at arrival, from the SAME refresh measurement that sized emitTotal — see there.
    // ★ AT MOST ONE ROW PER DISPLAY FRAME (rowsDueThisFrame). The old loop banked a missed slot and
    //   repaid it as TWO rows in one frame — at N = 1 that was a visible lurch after every stall.
    // ★ Display interval: the MEDIAN of recent ticks, from the rAF timestamp when the caller has it
    //   (vsync-aligned; performance.now() inside the callback carries the callback's own jitter).
    const t = this.pacer.onFrame(frameTs ?? now, !!(this.curRow && this.prevRow));
    // ★ rowsNow device rows of the SAME real row: Smooth on Retina (2 a frame), Sharp (one screen row).
    if (t > 0) for (let i = 0; i < this.pacer.rowsNow; i++) this.drawRow(t);


    // ★★★ DO NOT ZERO THE CARRY WHEN A PAIR COMPLETES. That is what an accumulator is FOR: the
    //     fraction left over is what keeps the cadence even across arrivals. Clearing it made every
    //     row wait a fresh interval measured from the NEXT arrival, so the arrival jitter came
    //     straight back through — which is the residual hitch, and it was mine (2026-08-03).
    //     At 20 rows/s on a 20 fps feed emitTotal is 1, so this fired on EVERY frame: the pacing
    //     did nothing at all in exactly the case being complained about.
  }

  /** Scroll down one line and draw the row blended t of the way from prev to cur. */
  private drawRow(t: number) {
    const wfH = this.wf.height;
    const prev = this.prevRow;
    const cur = this.curRow;
    const blend = this.blendRow;
    if (!wfH || !prev || !cur || !blend) return;
    const n = cur.length;

    // ★★★ WHOLE ROWS, NOT BLENDS — this is what makes the app's waterfall sharp and ours soft.
    //     Synthesised lines used to be a per-pixel average of the two frames either side, so at
    //     14 fps in and 20 rows/sec out MOST of the rows on screen were averages of two moments.
    //     Vertically that is a blur, and it is the "brighter and softer" in Stuart's side-by-side.
    //     ★★ CORRECTED 2026-08-03: this used to say the app already showed raw frames because its
    //     shader's `uQuant` gives "whole LINE steps when settled". It does not follow. uQuant
    //     quantises the line POSITION; the frame MIX is separate, and with lines-per-frame at 5 or
    //     6 most of the app's rows were averages of two source frames — the same blur described
    //     here, which is why the app looked softer too. The app now snaps that blend to the nearest
    //     source frame (WaterfallView.tsx, WF_SKSL) to match this line.
    //     ★ Repeating the newest row here costs nothing: the scroll rate is unchanged (so idle
    //     power saving is still free visually) and every row on screen is a real measurement.
    //     ★ `prev` is kept: it is what a future in-shader interpolation would need, and the app's
    //     continuous mode is worth porting if a tune ever looks steppy.
    // ★★★ WHERE THIS ROW IS DRAWN — the app's rule (wfGlide.ts rowView; src/utils/wfRowPlace.ts fanNoteRow/fanAfterRow):
    //     the row is the newest REAL frame the pacer presents (whole rows — Stuart's rule, never a blend of two frames),
    //     drawn at its own placement — except just after a zoom step or a tune step, when it is drawn at an in-between
    //     view on a short ease-out (the app's zoomEaseRows / tuneEaseRows screen rows; each device row its own step), so
    //     a station's trail fans out on a zoom in, draws in on a zoom out and slides across on a tune, and the history
    //     stays AS DRAWN above it. A move of a whole view or more is a JUMP: the history moves into place (below).
    //     ✗ The cross-fade glide (two frames mixed by t, 0fa2c5d2f/d6b99aa1f) and the 400 ms time-based zoom ease
    //       (f6139764b/7db21a2d5) are gone: the first blended rows; the second drew a fast fan as a dashed near-horizontal
    //       line with no join — the hard break under a zoom out on Stuart's Pi 500 recording (2026-10-29).
    const useCur = this.pacer.useCur;
    // ★★ The first draw of a row takes in the rows the thinning passed over (WfRowMerge) — combined, peaks kept.
    if (useCur) this.merge.drawCur(cur, this.curLo, this.curSpan, this.curK); else this.merge.drawPrev(prev);
    const src = useCur ? cur : prev;
    const sLo = useCur ? this.curLo : this.prevLo, sSpan = useCur ? this.curSpan : this.prevSpan;
    const v = rowView(this.ease, sLo, sSpan, this.pacer.rowPx, this.rv, this.pacer.easeRowRate());
    const cLo = v.lo, cSpan = v.span;
    this.pacer.easing = this.ease.eN > 0;   // ★ the boost holds until the ease has landed
    // ★★★ A JUMP KEEPS THE HISTORY AS DRAWN TOO (Stuart, 2026-10-30, #57: "can we preserve the history on longer tunes and
    //     zooms so it is not lost" — Gesiima tunes from the history onto a signal that is quiet now). It used to move every
    //     row into place (_jumpHistory — rows slid off, the edge went dark); now nothing is moved or blanked: the old rows
    //     stay where they were drawn, a scale stamp marks the boundary, and a tap on them tunes by THEIR scale (xyToHz).
    //     (_jumpHistory and stampJump are kept for a re-mapping caller; v.jumpHz still stops the join across the jump.)
    // ★★ THE JOIN (wfGlide.ts joinedRowAt — the app's shader motion taps): a row drawn somewhere other than the row below
    //    is drawn across its footprint toward it, so the trail is ONE solid line through the ease, not dashes. A steady row
    //    (the same view as the row below) is the frame exactly; a jump is not joined (the history moved instead).
    const below = this.outRow && this.outRow.length === n ? this.outRow : null;
    if (v.jumpHz || !below || !(this.outSpan > 0)) {
      if (cLo === sLo && cSpan === sSpan) blend.set(src); else easedRowAt(src, sLo, sSpan, cLo, cSpan, blend, null, this.glideScratch);
    } else if (cLo === this.outLo && cSpan === this.outSpan && cLo === sLo && cSpan === sSpan) {
      blend.set(src);
    } else {
      joinedRowAt(src, sLo, sSpan, cLo, cSpan, this.outLo, this.outSpan, blend, below, this.glideScratch);
    }
    this._noteRowView(cLo, cSpan);
    if (!this.outRow || this.outRow.length !== n) this.outRow = new Uint8Array(n);
    this.outRow.set(blend); this.outLo = cLo; this.outSpan = cSpan;
    // ★★ The scale stamps watch every row as DRAWN (the eased ones too): one stamp of the old scale once a move lands.
    this.rowSeq++;
    this.stamps.minGapRows = STAMP_MIN_GAP_LABELS * Math.round(15 * renderDpr());   // ★ three label heights, in device rows
    this.stamps.sweepRows = stampSweepRows(this.pacer.easeRowRate(), wfH);   // ★ a long tune/pan sweep: now and then
    stampNoteRow(this.stamps, this.rowSeq, cLo, cSpan, stampSettleRows(this.pacer.easeRowRate()));
    if (this.stamps.stamps.length) stampPrune(this.stamps, this.rowSeq, wfH + 64);

    // ★ NO SELF-COPY. This used to scroll with `wfCtx.drawImage(this.wf, 0, 1)` — drawing the
    // waterfall canvas onto ITSELF one pixel down, every frame. Safari optimises that; Chromium
    // does NOT, and on a GPU-backed canvas it forces a full texture readback and copy per frame.
    // MEASURED on an M4 with the same page: Edge 38.7% CPU and 89°C against Safari's 5.2% and
    // 63°C. Instead the history is a RING: the newest row is written at a moving head and nothing
    // else ever moves. Cost per frame drops from "copy the whole canvas" to "write one row".

    // GPU path: upload the RAW n-wide row (dB indices) into the ring texture — palette and the
    // peak-preserving bins→pixels downsample both happen on the GPU in the shader. No per-pixel JS.
    // The ring is TALLER than the display (fixed/generous), so `head` walks its rows, not the display's
    // — that's what lets a resize keep history (the ring is never touched).
    if (this.gl) {
      // ★★ THE RING IS A FIXED WIDTH, and rows are resampled into it — ported from the
      // app (2026-07-26). It used to be allocated at the LIVE bin count, so any change in
      // bin count reallocated the texture and threw the whole scroll-back away. Worse, it
      // did so silently and only sometimes, which is what made the waterfall appear to
      // "squish" on a zoom: the history was not being rescaled, it was being destroyed and
      // refilled at a different width.
      // 1024 ≈ one bin per pixel on a phone and comfortably more than a browser column
      // count, so resampling into it costs nothing visible and history now survives
      // ANY bin count the server sends.
      const rowIn = resampleRow(blend, n, this.ringBuf);
      if (this.glCols !== RING_W || this.gl.rows < wfH) {
        this.gl.ensureRing(RING_W, Math.max(GL_RING_ROWS, wfH));
        this.glCols = RING_W;
      }
      const rows = this.gl.rows;
      this._ensureXf(rows);
      this.head = (this.head - 1 + rows) % rows;
      this.gl.pushRow(rowIn, this.head);
      this.xfA[this.head] = 1; this.xfB[this.head] = 0; this.xfS[this.head] = cSpan; this.xfL[this.head] = cLo;
      // ★★ V12 the full-capture layer: this row's coarse twin, at the same ring row (wfWide.ts).
      this.xfX[this.head] = 0;
      if (this.wideEver && this.gFloor.length && this.gPeak.length) {
        this.gl.ensureLayer(this.gFloor.length, this.gPeak.length >> 1);
        this.xfX[this.head] = this._layerGL(cLo, cSpan);
        this.xfM[this.head] = this.lastM;
        this.gl.pushLayer(this.gFloor, this.gPeak, this.gM, this.head);
      }
      return;
    }

    // 2D fallback: peak-preserving downsample to canvas width + palette LUT + putImageData.
    const H = wfH;
    this._ensureXf(H);
    this.head = (this.head - 1 + H) % H;
    this.xfA[this.head] = 1; this.xfB[this.head] = 0; this.xfS[this.head] = cSpan; this.xfL[this.head] = cLo;
    this.xfX[this.head] = 0;
    // ★★ V12 the full-capture layer on the 2D path: a coloured WIDE_TEX-wide row in its own ring canvas, same row index.
    if (this.wideEver) {
      this.xfX[this.head] = this._coarseFor(cLo, cSpan);
      this._ensureWfC(H);
      if (this.rowImgC && this.wfCCtx) {
        const d = this.rowImgC.data, lut = this.lut, cr = this.coarseRow;
        for (let x = 0; x < WIDE_TEX; x++) { const o = cr[x] << 2, q = x << 2; d[q] = lut[o]; d[q + 1] = lut[o + 1]; d[q + 2] = lut[o + 2]; d[q + 3] = 255; }
        this.wfCCtx.putImageData(this.rowImgC, 0, this.head);
      }
    }
    const W = this.wf.width;
    if (!this.rowImg) return;
    const img = this.rowImg.data;
    const lut = this.lut;
    for (let x = 0; x < W; x++) {
      // Peak-preserving downsample: the max over this pixel's bin bucket.
      // Averaging would bury a narrow carrier in its own noise floor.
      const b0 = Math.floor((x * n) / W);
      const b1 = Math.max(b0 + 1, Math.floor(((x + 1) * n) / W));
      let v = 0;
      for (let bi = b0; bi < b1 && bi < n; bi++) if (blend[bi] > v) v = blend[bi];
      const o = v << 2;
      const p = x << 2;
      img[p]     = lut[o];
      img[p + 1] = lut[o + 1];
      img[p + 2] = lut[o + 2];
      img[p + 3] = 255;
    }
    this.wfCtx.putImageData(this.rowImg, 0, this.head);
  }

  /** The history's per-row maps, one per ring row; a new size (first row, a ring that grew and so was cleared) starts
   *  every row at the identity. */
  private _ensureXf(rows: number) {
    if (this.xfA.length === rows) return;
    this.xfA = new Float64Array(rows).fill(1);
    this.xfB = new Float64Array(rows);
    this.xfX = new Float64Array(rows);
    this.xfS = new Float64Array(rows);
    this.xfL = new Float64Array(rows);
    this.xfM = new Float64Array(rows);
  }

  /** ★★ A JUMP (wfGlide.ts rowView: a move of a whole view or more — a bookmark, a typed frequency, a spin faster than the
   *  radio): every row on the ring moves into place by `hz`, each in ITS OWN scale (rows keep the zoom they were drawn
   *  at), so the history stays visible where it still belongs and is blank only at the newly exposed edge — the full-
   *  capture layer fills that where the radio was capturing (black ONLY where nothing was). Two multiply-adds a row; the
   *  stored pixels are never rewritten (the app's fanNoteRow jump, src/utils/wfRowPlace.ts). */
  private _jumpHistory(hz: number, span: number) {
    const A = this.xfA, B = this.xfB, S = this.xfS;
    for (let r = 0; r < B.length; r++) {
      const sp = S[r] > 0 ? S[r] : span;
      let nb = B[r] + hz / sp;
      if (A[r] === 1 && Math.abs(nb) < 1e-9) nb = 0;
      B[r] = nb;
    }
  }

  /** ★ V12 the layer as drawn for a row emitted NOW: the floor and the cells interpolated from the previous full-capture
   *  row to the newest across the interval they come at. A cell with a peak in only one of the two fades between that
   *  peak and the floor under it. Returns false when there is no fresh row. */
  private _blendLayer(): boolean {
    const w = this.wide;
    if (!w || performance.now() - this.wideAt > 4000) return false;
    const F = this.wFloor.length, C = this.wCell.length;
    const pF = this.wFloorP, pC = this.wCellP, pS = this.wShapeP, pG = this.wSpreadP;
    const t = pF ? Math.max(0, Math.min(1, (performance.now() - this.wideAt) / Math.max(50, this.wideGap))) : 1;
    if (!pF || !pC || !pS || !pG || t >= 1) { this.bFloor.set(this.wFloor); this.bSpread.set(this.wSpread); this.bCell.set(this.wCell); this.bShape.set(this.wShape); return true; }
    for (let i = 0; i < F; i++) { const a = pF[i], b = this.wFloor[i]; this.bFloor[i] = a === a && b === b ? a + (b - a) * t : b;
      this.bSpread[i] = pG[i] + (this.wSpread[i] - pG[i]) * t; }
    for (let c = 0; c < C; c++) {
      const p = pC[c], q = this.wCell[c];
      if (p !== p && q !== q) { this.bCell[c] = NaN; continue; }
      const fl = floorAt(this.bFloor, (c + 0.5) / C);
      if (p === p && q === q) { this.bCell[c] = p + (q - p) * t; this.bShape[c] = this.wShape[c]; }
      else if (q === q) { this.bCell[c] = fl + Math.max(0, q - fl) * t; this.bShape[c] = this.wShape[c]; }
      else { this.bCell[c] = fl + Math.max(0, p - fl) * (1 - t); this.bShape[c] = pS[c]; }
    }
    return true;
  }

  /** dB → the detailed rows' own palette index (SignalProcessor's mapping: brightness, the auto range, the 14.97 clip). */
  private _idxFn(): (db: number) => number {
    const r = this.proc.getRange(), bright = this.proc.getSettings().wfBrightness || 0;
    const range = r.dbMax - r.dbMin > 0 ? r.dbMax - r.dbMin : 1, CLIP = 14.97, off = this.wideOff;
    return (db: number) => {
      const nrm = Math.max(0, Math.min(1, (db + off + bright - r.dbMin) / range));
      const mag = nrm * 255;
      return Math.round(mag < CLIP ? 0 : (mag - CLIP) / (255 - CLIP) * 255);
    };
  }

  /** ★ GPU path: the layer's row for a detailed row placed at (lo, span) — floor indices, (level, shape) per cell, and
   *  the row's m — into gFloor / gPeak / gM. Returns k (0 = no layer: no fresh row, or the row covers the capture). */
  private lastM = 0;
  private _layerGL(lo: number, span: number): number {
    const w = this.wide;
    if (!w || !(span > 0) || span >= w.spanHz * 0.98 || !rowInCapture(lo, span, w.centreHz, w.spanHz) || !this._blendLayer()) { this.gFloor.fill(0); this.gPeak.fill(0); return 0; }
    const idx = this._idxFn();
    for (let i = 0; i < this.bFloor.length; i++) { const v = this.bFloor[i]; this.gFloor[i] = v === v ? Math.max(1, floorIdx(v, this.bSpread[i], idx)) : 0; }
    const C = this.bCell.length, g = this.gPeak;
    for (let c = 0; c < C; c++) {
      const v = this.bCell[c];
      if (v === v) { g[2 * c] = Math.max(1, idx(v)); g[2 * c + 1] = this.bShape[c]; } else { g[2 * c] = 0; g[2 * c + 1] = 0; }
    }
    this.lastM = (lo - (w.centreHz - w.spanHz / 2)) / w.spanHz;
    encodeM(this.lastM, this.gM, 0);
    return span / w.spanHz;
  }

  /** ★ 2D path: the coarse twin for a detailed row placed at (lo, span) into this.coarseRow; returns its extent X (0 =
   *  none). Built from the dense row (the floor with every peak drawn in) through the detailed rows' own mapping. */
  private _coarseFor(lo: number, span: number): number {
    const w = this.wide;
    const X = w && rowInCapture(lo, span, w.centreHz, w.spanHz) ? wideExtent(w.spanHz, span) : 0;
    if (!X || !w || !this._blendLayer()) { this.coarseRow.fill(0); return 0; }
    // ★ built in palette INDICES (the floor as its speckle's average — floorIdx — which has no single dB to stand for it)
    this.dense.centreHz = w.centreHz; this.dense.spanHz = w.spanHz;
    const idx = this._idxFn(), F = this.bFloor.length;
    if (this.fIdx.length !== F) this.fIdx = new Float32Array(F);
    for (let i = 0; i < F; i++) { const v = this.bFloor[i]; this.fIdx[i] = v === v ? floorIdx(v, this.bSpread[i], idx) : NaN; }
    if (this.cIdx.length !== this.bCell.length) this.cIdx = new Float32Array(this.bCell.length);
    for (let c = 0; c < this.bCell.length; c++) { const v = this.bCell[c]; this.cIdx[c] = v === v ? idx(v) : NaN; }
    denseRow(this.fIdx, this.cIdx, this.bShape, this.dense.db);
    wideRowInto(this.coarseRow, this.dense, lo, span, X, 0, (v) => Math.round(v));
    return X;
  }

  /** The 2D path's coarse ring canvas, WIDE_TEX × the waterfall's height. */
  private _ensureWfC(H: number) {
    if (this.wfC && this.wfC.height === H) return;
    if (!this.wfC) { this.wfC = document.createElement('canvas'); this.wfCCtx = this.wfC.getContext('2d'); }
    this.wfC.width = WIDE_TEX; this.wfC.height = H;
    if (this.wfCCtx) { this.wfCCtx.fillStyle = '#000'; this.wfCCtx.fillRect(0, 0, WIDE_TEX, H); this.rowImgC = this.wfCCtx.createImageData(WIDE_TEX, 1); }
  }

  private _noteRowView(lo: number, span: number) {
    if (!(span > 0)) return;
    this.lastLo = lo; this.lastSpan = span;
  }

  /** Composite waterfall + spectrum trace + markers to the visible canvas. */
  draw() {
    const W = this.canvas.width;
    const H = this.canvas.height;
    if (!W || !H) return;
    const ctx = this.ctx;
    const specH = H - this.wf.height;

    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, W, specH);
    // ★★★ THE HARD RULE (Stuart, 2026-10-27, verbatim): "the waterfall is what I always wanted to make sure was perfect, it
    //     was a fully buffered waterfall which when rotated or resized never lost any content it expanded or shrunk as
    //     needed. Zooming and tuning also never lost the history and it was seemless to the history too so signals always
    //     glided in from the sides and settled and when zooming in and out you had a cool fanning in and out of the
    //     signals. Those 2 things were the major things that we nailed in our first native build and I made a hard rule
    //     that these must be preserved at all costs".
    //     That look IS this blit: every row drawn once against the view it arrived in, the history never rewritten, so a
    //     trail bends while the dial moves and settles straight after. GitHub #57's torn trails were rows the SERVER
    //     cropped against the wrong tuner position (local_sdr_shim.cpp onSpectrum) — scripts/test-web-waterfall-history.mjs
    //     guards both halves (tune/zoom continuity and resize keeping the history) against the real server.
    // Straight blit, in BIN space — exactly what the app does (its shader maps
    // display x directly to bins: tx = xy.x / uDrawW * uTexW). No pan offset, no
    // frequency-indexed history, no re-alignment ON A TUNE. Old rows keep their old bins
    // and scroll away.
    // ★★ A ZOOM DOES NOT RE-MAP THE HISTORY (Stuart 2026-10-28): the rows stay as drawn and the new ones ease
    //    in (drawRow, wfGlide.ts). The per-row maps (wfZoomFan.ts), drawn per run of rows below, move the
    //    history only on a JUMP (_jumpHistory). The stored pixels are never rewritten.
    //
    // Both of the clever alternatives were worse: sliding the history sideways
    // fought the frames still in flight (snap-back), and cropping a
    // frequency-space buffer rescaled the whole waterfall every frame (the
    // "redraws itself on every movement" blur). The app doesn't compensate at
    // all — it sends the view change and lets the frames arrive.
    // The ring is unwrapped HERE, at present time, with two blits: head→bottom, then top→head.
    // Two GPU-side draws of untouched texture, versus the old full-canvas self-copy every frame.
    const wfH = this.wf.height;
    if (this.gl && this.glCanvas && this.glCols > 0) {
      // GPU path: the shader unwraps the ring from `head` and colours it on the GPU. One draw into
      // the GL canvas, one blit onto the visible canvas — no per-row CPU work, dpr scaling is free.
      // ★ The zoom fan: runs of rows sharing one map (one run, the identity, until somebody zooms).
      const nRuns = this.xfA.length === this.gl.rows ? mapRunsX(this.xfA, this.xfB, this.xfX, this.head, wfH, this.runs) : 0;
      this.gl.render(this.head, wfH, W, wfH, this.runs.buf, nRuns);
      ctx.drawImage(this.glCanvas, 0, 0, W, wfH, 0, specH, W, wfH);
    } else {
      const nRuns = this.xfA.length === wfH ? mapRunsX(this.xfA, this.xfB, this.xfX, this.head, wfH, this.runs) : 0;
      const rb = this.runs.buf;
      if (nRuns <= 1 && (nRuns === 0 || (rb[2] === 1 && rb[3] === 0 && rb[4] === 0))) {
        // 2D fallback: unwrap the ring with two blits (head→bottom, then top→head).
        const first = wfH - this.head;            // rows from head to the end of the buffer
        if (first > 0) ctx.drawImage(this.wf, 0, this.head, W, first, 0, specH, W, first);
        if (this.head > 0) ctx.drawImage(this.wf, 0, 0, W, this.head, 0, specH + first, W, this.head);
      } else {
        // ★ The zoom fan on the 2D path: each run is blitted horizontally scaled to its map, blank where it has no data.
        //   A few drawImage calls per run (two if it wraps the ring) — the stored pixels are never rewritten.
        ctx.fillStyle = '#000';
        ctx.fillRect(0, specH, W, wfH);
        for (let i = 0; i < nRuns; i++) {
          const j0 = rb[i * 5], j1 = rb[i * 5 + 1], ra = rb[i * 5 + 2], rbb = rb[i * 5 + 3], X = rb[i * 5 + 4];
          // ★★ V12 the full-capture layer first (wfWide.ts): the run's coarse rows through the same map, extended by X.
          if (X > 0 && this.wfC && this.wfC.height === wfH) {
            const e = 1 + 2 * X;
            const cs = visibleSpan(ra / e, (rbb + X) / e);
            if (cs) {
              const sx = cs[2] * WIDE_TEX, sw = (cs[3] - cs[2]) * WIDE_TEX, dx = cs[0] * W, dw = (cs[1] - cs[0]) * W;
              let j = j0;
              while (j < j1) { const r = (this.head + j) % wfH; const cnt = Math.min(j1 - j, wfH - r);
                ctx.drawImage(this.wfC, sx, r, sw, cnt, dx, specH + j, dw, cnt); j += cnt; }
            }
          }
          const vs = visibleSpan(ra, rbb);
          if (!vs) continue;
          const sx = vs[2] * W, sw = (vs[3] - vs[2]) * W, dx = vs[0] * W, dw = (vs[1] - vs[0]) * W;
          let j = j0;
          while (j < j1) {
            const r = (this.head + j) % wfH;
            const cnt = Math.min(j1 - j, wfH - r);
            ctx.drawImage(this.wf, sx, r, sw, cnt, dx, specH + j, dw, cnt);
            j += cnt;
          }
        }
      }
    }

    if (this.stamps.stamps.length) this._drawStamps(ctx, W, specH, wfH);

    if (specH > 4 && this.spec && this._showSpec) {
      this._drawSpec(ctx, W, specH);
      this.onDrawAxis?.(ctx, W, specH);   // dB axis, drawn by main (owns the labels)
    }
    this._drawVfo(ctx, W, H);
  }

  /** ★★ THE SCALE STAMPS (src/utils/wfStamps.ts; Stuart, 2026-10-30, #57: "zoom in and a stamp of the previous frequency
   *  scale as it was is added onto the old waterfall position and it gently drops off the bottom of the screen, same with
   *  tuning too if needed"). Each is a thin ruler of the scale its rows were drawn at — the live scale's own step and
   *  units (freqScale.ts), in the user's VFO colour with a dark shadow (stampColour — white if that clashes with the
   *  palette; Stuart: "an opposite colour to the waterfall … i'd simply use orange like my VFO") — on a faint dark band, laid on the top of those rows: rowSeq − its row device rows down,
   *  so it scrolls with them exactly. It fades in over a few rows and out over the bottom of the waterfall. An overlay
   *  only: nothing in the ring is touched. A few fillRect/fillText calls a stamp; nothing at all when there are none. */
  private _drawStamps(ctx: CanvasRenderingContext2D, W: number, top: number, wfH: number) {
    const dpr = renderDpr();
    const band = Math.round(15 * dpr);
    const shown = stampsShown(this.stamps.stamps, 1, band);
    ctx.save();
    ctx.beginPath(); ctx.rect(0, top, W, wfH); ctx.clip();
    ctx.font = `${10 * dpr}px ui-monospace, Menlo, monospace`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    const hair = Math.max(1, Math.round(dpr * 0.75)), tickW = Math.max(1, Math.round(dpr)), tickH = Math.round(4 * dpr);
    const live = new Set<number>();
    const col = stampColour(this.vfoColor, this.lut);
    ctx.shadowColor = 'rgba(0,0,0,0.95)';
    ctx.shadowOffsetX = 0; ctx.shadowOffsetY = Math.max(1, Math.round(dpr));
    ctx.shadowBlur = NO_GLOW ? 0 : Math.round(2 * dpr);
    for (const s of shown) {
      const yr = this.rowSeq - s.frame;                 // device rows from the top of the waterfall to the stamp's rows
      const a = stampAlpha(yr, band, wfH, this.rowSeq - s.born);
      if (!(a > 0.01)) continue;
      live.add(s.id);
      let c = this.stampTickCache.get(s.id);
      if (!c || c.W !== W || c.lo !== s.lo || c.span !== s.span || c.dpr !== dpr) {
        const t = stampTicks(s.lo, s.span, W, (sp) => tickStep(sp, 8), fmtTick, 56 * dpr);
        c = { W, lo: s.lo, span: s.span, dpr, ticks: t.map(k => ({ ...k, w: k.label ? ctx.measureText(k.label).width : 0 })) };
        this.stampTickCache.set(s.id, c);
      }
      const y = top + yr;
      ctx.globalAlpha = a;
      ctx.shadowColor = 'transparent';
      ctx.fillStyle = 'rgba(0,0,0,0.55)';
      ctx.fillRect(0, y, W, band);
      ctx.shadowColor = 'rgba(0,0,0,0.95)';
      ctx.fillStyle = rgba(col, 0.55);                 // the line where the scale changed
      ctx.fillRect(0, y, W, hair);
      // ★ WHEN, at the left end (Stuart, 2026-10-30): HH:MM:SS the scale changed; frequency labels keep clear of it.
      let timeEnd = 0;
      if (s.at) {
        const tl = stampTimeLabel(s.at);
        ctx.textAlign = 'left';
        ctx.fillStyle = rgba(col, 0.95);
        ctx.fillText(tl, Math.round(3 * dpr), y + tickH + Math.round(dpr));
        timeEnd = Math.round(3 * dpr) + ctx.measureText(tl).width + Math.round(8 * dpr);
        ctx.textAlign = 'center';
      }
      for (const k of c.ticks) {
        ctx.fillStyle = rgba(col, 0.85);
        ctx.fillRect(Math.round(k.x - tickW / 2), y, tickW, tickH);
        if (!k.label) continue;
        // ★ kept whole at the edges (the live scale's end labels are cut; a stamp's would read as a glitch)
        const x = Math.max(k.w / 2 + 2, Math.min(W - k.w / 2 - 2, k.x));
        if (x - k.w / 2 < timeEnd) continue;           // under the time: the time wins
        ctx.fillStyle = rgba(col, 0.95);
        ctx.fillText(k.label, x, y + tickH + Math.round(dpr));
      }
    }
    ctx.restore();
    for (const id of this.stampTickCache.keys()) if (!live.has(id) && !this.stamps.stamps.some(s => s.id === id)) this.stampTickCache.delete(id);
  }

  /** Trace blended prev->cur by elapsed time. Runs at the display's refresh rate,
   *  NOT the server's frame rate, so at 5fps the trace glides instead of jumping.
   *  At 20fps the blend completes within a frame and it looks as it did before. */
  private interpolatedTrace(): { spec: Float32Array; peak: Float32Array } {
    const spec = this.spec!;
    const peak = this.peak!;
    const prevSpec = this.prevSpec;
    const prevPeak = this.prevPeak;
    const ds = this.drawSpec;
    const dp = this.drawPeak;
    const P = this.pacer;
    if (!prevSpec || !prevPeak || !ds || !dp || !P.emitInterval) return { spec, peak };

    const span = P.emitInterval * P.emitTotal;   // observed gap between frames
    const t = Math.max(0, Math.min(1, (performance.now() - P.emitStart) / span));
    if (t >= 1) return { spec, peak };

    for (let i = 0; i < spec.length; i++) ds[i] = prevSpec[i] + (spec[i] - prevSpec[i]) * t;
    // Peak hold only rises — take the max, so an interpolated peak line never dips
    // below the peak it is meant to be holding.
    for (let i = 0; i < peak.length; i++) dp[i] = Math.max(prevPeak[i], peak[i]);
    return { spec: ds, peak: dp };
  }

  private _drawSpec(ctx: CanvasRenderingContext2D, W: number, H: number) {
    const { spec, peak } = this.interpolatedTrace();
    const n = spec.length;

    // Trace, filled to the floor — same shape as the app's signal display.
    ctx.beginPath();
    ctx.moveTo(0, H);
    for (let x = 0; x < W; x++) {
      const b0 = Math.floor((x * n) / W);
      const b1 = Math.max(b0 + 1, Math.floor(((x + 1) * n) / W));
      let v = 0;
      for (let b = b0; b < b1 && b < n; b++) if (spec[b] > v) v = spec[b];
      ctx.lineTo(x, H - v * H);
    }
    ctx.lineTo(W, H);
    ctx.closePath();
    // Filled from the palette LUT, like the app: the trace is shaded by the same
    // colours the waterfall uses, so a signal reads the same in both halves.
    ctx.globalAlpha = this.specAlpha;
    ctx.fillStyle = this.specGradient(ctx, H);
    ctx.fill();
    ctx.globalAlpha = 1;
    ctx.strokeStyle = this.peakColour();
    ctx.lineWidth = 1;
    ctx.stroke();

    if (peak) {
      ctx.beginPath();
      for (let x = 0; x < W; x++) {
        const b0 = Math.floor((x * n) / W);
        const b1 = Math.max(b0 + 1, Math.floor(((x + 1) * n) / W));
        let v = 0;
        for (let b = b0; b < b1 && b < n; b++) if (peak[b] > v) v = peak[b];
        const y = H - v * H;
        if (x === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      // Peak-hold line takes the VFO colour, matching the app (WaterfallView
      // peakPaint: needleColor at 0.85, blur 2).
      ctx.strokeStyle = rgba(this.vfoColor, 0.85);
      ctx.shadowColor = rgba(this.vfoColor, 0.85);
      ctx.shadowBlur = NO_GLOW ? 0 : 2;
      ctx.lineWidth = 1;
      ctx.stroke();
      ctx.shadowBlur = 0;
    }
  }

  /**
   * VFO needle + acrylic sidebands — ported 1:1 from the app's WaterfallView
   * (:798 geometry, :847 glow strips, :1007 acrylic gradients).
   *
   * The sidebands are the point: they show the PASSBAND sitting on the signal, so
   * bandwidth is something you see rather than a number you read.
   *
   * The app renders the halo with a Skia MaskFilter, which blurs the stroke
   * ITSELF — and its own comment notes that canvas shadowBlur instead glows
   * BEHIND a sharp stroke, which is the original v1 look it was imitating. We're
   * in canvas, so we get the v1 semantics for free: blurred halo + razor hairline.
   */
  private _drawVfo(ctx: CanvasRenderingContext2D, W: number, H: number) {
    if (!this.spanHz || !this.vfoHz) return;

    const nX = this.hzToX(this.vfoHz, W);
    let loX = this.hzToX(this.vfoHz + this.filterLow, W);
    let hiX = this.hzToX(this.vfoHz + this.filterHigh, W);

    // Keep the sidebands visible when zoomed out — a passband narrower than a few
    // pixels would otherwise vanish entirely.
    const minSb = (this.filterLow === 0 && this.filterHigh === 0) ? 20 : 4;
    if (nX - loX < minSb) loX = nX - minSb;
    if (hiX - nX < minSb) hiX = nX + minSb;
    const loXc = Math.max(0, loX);
    const hiXc = Math.min(W, hiX);

    const col = this.vfoColor;
    const k = this.vfoIntensity / 5;          // 5 = the app's original look
    const wk = Math.max(1, k);

    ctx.save();

    // Frosted backing: dims the waterfall across the passband so the needle keeps
    // contrast on bright palettes, whatever colour it is.
    if (this.vfoFrost > 0 && hiXc > loXc) {
      ctx.fillStyle = `rgba(0,0,0,${(this.vfoFrost / 10) * 0.72})`;
      ctx.fillRect(loXc, 0, hiXc - loXc, H);
    }

    // Acrylic sideband panels — 4-stop gradients rising toward the needle.
    // ★ CACHED: the same gradient object is reused while its endpoints and colour are unchanged
    //   (i.e. whenever the dial is still — nearly every frame). A CanvasGradient is immutable once
    //   its stops are added, so reusing one with identical endpoints/stops paints identically.
    if (nX > loXc) {
      ctx.fillStyle = this.sbGradient(ctx, 0, loXc, nX, col);
      ctx.fillRect(loXc, 0, nX - loXc, H);
    }
    if (hiXc > nX) {
      ctx.fillStyle = this.sbGradient(ctx, 1, nX, hiXc, col);
      ctx.fillRect(nX, 0, hiXc - nX, H);
    }

    // Sideband edges: soft glow + crisp acrylic line (both at 0.35).
    const edge = (x: number) => {
      if (x <= 0 || x >= W) return;
      ctx.strokeStyle = rgba(col, 0.35);
      ctx.lineWidth = Math.max(0.75, 0.75);
      ctx.shadowColor = rgba(col, 0.35);
      ctx.shadowBlur = NO_GLOW ? 0 : 8;
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, H);
      ctx.stroke();
      ctx.shadowBlur = 0;
      ctx.stroke();          // razor line on top of the halo
    };
    edge(loX);
    edge(hiX);

    this._drawCapture(ctx, W, H);
    this._drawWalls(ctx, W, H);
    this._drawRfCentre(ctx, W, H);

    // LED needle: three glow layers (28/16/6) + a crisp core filament.
    if (nX >= 0 && nX <= W) {
      const layer = (alpha: number, blur: number, sw: number) => {
        if (NO_GLOW) return;
        ctx.strokeStyle = rgba(col, Math.min(1, alpha));
        ctx.lineWidth = sw;
        ctx.shadowColor = rgba(col, Math.min(1, alpha));
        ctx.shadowBlur = blur;
        ctx.beginPath();
        ctx.moveTo(nX + 0.5, 0);
        ctx.lineTo(nX + 0.5, H);
        ctx.stroke();
      };
      layer(0.35 * k, 28, 1.5 * wk);   // outer halo
      layer(0.70 * k, 16, 0.8 * wk);   // mid glow
      layer(0.80 * k, 6,  0.8 * wk);   // filament glow
      ctx.shadowBlur = 0;
      ctx.strokeStyle = col;           // crisp core
      ctx.lineWidth = 0.75 * wk;
      ctx.beginPath();
      ctx.moveTo(nX + 0.5, 0);
      ctx.lineTo(nX + 0.5, H);
      ctx.stroke();

      // Say what it is. With an RF-centre marker on screen too, an unlabelled
      // needle is ambiguous — which line is the one you're listening to?
      const dpr2 = renderDpr();
      void dpr2;
      this.markerLabel(ctx, nX, W, this.markerLabelY(H, 0),
        `LISTEN ${(this.vfoHz / 1e6).toFixed(3)}M`, col);
    }

    ctx.restore();
  }

  /** Sideband gradient cache — [0] lower (rises toward the needle), [1] upper (falls away). */
  private sbGrad: (CanvasGradient | null)[] = [null, null];
  private sbGradX0 = [NaN, NaN];
  private sbGradX1 = [NaN, NaN];
  private sbGradCol = ['', ''];
  private sbGradient(ctx: CanvasRenderingContext2D, side: 0 | 1, x0: number, x1: number,
                     col: string): CanvasGradient {
    const hit = this.sbGrad[side];
    if (hit && this.sbGradX0[side] === x0 && this.sbGradX1[side] === x1
        && this.sbGradCol[side] === col) return hit;
    const g = ctx.createLinearGradient(x0, 0, x1, 0);
    if (side === 0) {
      g.addColorStop(0,    rgba(col, 0.03));
      g.addColorStop(0.15, rgba(col, 0.06));
      g.addColorStop(0.55, rgba(col, 0.14));
      g.addColorStop(1,    rgba(col, 0.28));
    } else {
      g.addColorStop(0,    rgba(col, 0.28));
      g.addColorStop(0.45, rgba(col, 0.14));
      g.addColorStop(0.85, rgba(col, 0.06));
      g.addColorStop(1,    rgba(col, 0.03));
    }
    this.sbGrad[side] = g;
    this.sbGradX0[side] = x0; this.sbGradX1[side] = x1; this.sbGradCol[side] = col;
    return g;
  }

  /** The live capture window (dongle ± Fs/2) — a bracket showing the 2.4 MHz the
   *  radio is actually receiving. Pan past it and the RF centre has to move. */
  private _drawCapture(ctx: CanvasRenderingContext2D, W: number, H: number) {
    if (this.captureLoHz == null || this.captureHiHz == null) return;
    const dpr = renderDpr();
    const x0 = this.hzToX(this.captureLoHz, W);
    const x1 = this.hzToX(this.captureHiHz, W);
    if (x1 < 0 || x0 > W) return;

    ctx.save();
    ctx.strokeStyle = 'rgba(120,200,255,0.45)';
    ctx.lineWidth = 1;
    for (const x of [x0, x1]) {
      if (x < 0 || x > W) continue;
      ctx.beginPath();
      ctx.moveTo(x + 0.5, 0);
      ctx.lineTo(x + 0.5, H);
      ctx.stroke();
    }
    // Only worth labelling when the whole window is visible (i.e. zoomed out).
    if (x0 > 0 && x1 < W && x1 - x0 > 90 * dpr) {
      const span = (this.captureHiHz - this.captureLoHz) / 1e6;
      const label = `CAPTURE ${span.toFixed(1)} MHz`;
      ctx.font = `${10 * dpr}px ui-monospace, Menlo, monospace`;
      const tw = ctx.measureText(label).width;
      const cx = (x0 + x1) / 2 - tw / 2;
      const y = H - 8 * dpr;
      ctx.fillStyle = 'rgba(0,0,0,0.6)';
      ctx.fillRect(cx - 3 * dpr, y - 9 * dpr, tw + 6 * dpr, 13 * dpr);
      ctx.fillStyle = 'rgba(120,200,255,0.8)';
      ctx.fillText(label, cx, y);
    }
    ctx.restore();
  }

  /** A small boxed label pinned to a vertical marker. Flips side near the edge so
   *  it never runs off-screen. */
  /** ★★★ WHERE A MARKER LABEL SITS, and it is NOT the top of the canvas.
   *  RF CENTRE and LISTEN used to be drawn 12 and 28 device pixels down, which is exactly where
   *  the receiver name, the listener count and the session timer live — so on a shared server
   *  they overlapped and neither was readable (Stuart, 2026-08-05, with a screenshot).
   *  ★★ Anchored to the SPECTRUM/WATERFALL BOUNDARY instead, which is Stuart's own suggestion and
   *  the right one: it is the widest reliably-empty strip on the display, it moves when the split
   *  moves, and nothing else is ever drawn there.
   *  ★ With the spectrum switched off there is no boundary to hang them on, so fall back to a
   *  fixed drop that clears the name block — low enough to be out of the way, high enough not to
   *  be lost in the waterfall.
   *  @param stack 0 for the first label, 1 for a second stacked under it. */
  private markerLabelY(H: number, stack = 0): number {
    const dpr = renderDpr();
    const specH = H - this.wf.height;
    const row = 14 * dpr;
    if (specH > 40 * dpr && this._showSpec) return specH - 8 * dpr - stack * row;
    return 96 * dpr + stack * row;
  }

  private markerLabel(
    ctx: CanvasRenderingContext2D, x: number, W: number, y: number,
    text: string, colour: string,
  ) {
    const dpr = renderDpr();
    ctx.font = `${10 * dpr}px ui-monospace, Menlo, monospace`;
    const tw = ctx.measureText(text).width;
    const lx = x + 6 * dpr + tw > W ? x - tw - 6 * dpr : x + 6 * dpr;
    ctx.fillStyle = 'rgba(0,0,0,0.65)';
    ctx.fillRect(lx - 3 * dpr, y - 9 * dpr, tw + 6 * dpr, 13 * dpr);
    ctx.fillStyle = colour;
    ctx.textBaseline = 'alphabetic';
    ctx.fillText(text, lx, y);
  }

  /**
   * Pan boundary walls. Drawn ONLY when a wall is actually in view — i.e. when
   * you're zoomed out far enough to see the limit of the reachable band. They are
   * not permanent chrome.
   *
   * Zoomed IN, a wall is usually off-screen, so hitting it would just make the
   * drag stop dead for no visible reason. So when a pan gets clamped we flash the
   * edge of the screen on that side instead: same information, no clutter.
   */
  private _drawWalls(ctx: CanvasRenderingContext2D, W: number, H: number) {
    if (this.wallLoHz == null && this.wallHiHz == null) return;
    ctx.save();

    const dpr = renderDpr();
    const WALL = 'rgba(255,200,80,0.95)';

    if (this.wallLoHz != null) {
      const x = this.hzToX(this.wallLoHz, W);
      if (x > 0 && x < W) {
        ctx.fillStyle = 'rgba(0,0,0,0.45)';
        ctx.fillRect(0, 0, x, H);
        ctx.fillStyle = 'rgba(255,200,80,0.85)';
        ctx.fillRect(x - 0.75, 0, 1.5, H);
        this.markerLabel(ctx, x, W, H - 24 * dpr,
          `LOWER LIMIT ${(this.wallLoHz / 1e6).toFixed(3)}M`, WALL);
      }
    }
    if (this.wallHiHz != null) {
      const x = this.hzToX(this.wallHiHz, W);
      if (x < W && x > 0) {
        ctx.fillStyle = 'rgba(0,0,0,0.45)';
        ctx.fillRect(x, 0, W - x, H);
        ctx.fillStyle = 'rgba(255,200,80,0.85)';
        ctx.fillRect(x - 0.75, 0, 1.5, H);
        this.markerLabel(ctx, x, W, H - 24 * dpr,
          `UPPER LIMIT ${(this.wallHiHz / 1e6).toFixed(3)}M`, WALL);
      }
    }

    // Hit-the-limit feedback: a brief glow at the screen edge you pushed against.
    const age = performance.now() - this.wallHitAt;
    if (this.wallHitSide && age < 500) {
      const a = (1 - age / 500) * 0.5;
      const lo = this.wallHitSide === 'lo';
      const g = ctx.createLinearGradient(lo ? 0 : W, 0, lo ? 60 : W - 60, 0);
      g.addColorStop(0, `rgba(255,200,80,${a})`);
      g.addColorStop(1, 'rgba(255,200,80,0)');
      ctx.fillStyle = g;
      ctx.fillRect(lo ? 0 : W - 60, 0, 60, H);
    }
    ctx.restore();
  }

  /** RF-centre marker — a thin dashed line where the DONGLE is tuned, which is
   *  not where you're looking once the view is unlocked. Labelled, because an
   *  unexplained line is just clutter: it needs to say what it is and where. */
  private _drawRfCentre(ctx: CanvasRenderingContext2D, W: number, H: number) {
    if (this.rfCenterHz == null) return;
    const x = this.hzToX(this.rfCenterHz, W);
    if (x < 0 || x > W) return;
    const dpr = renderDpr();

    ctx.save();
    ctx.strokeStyle = 'rgba(120,200,255,0.75)';
    ctx.lineWidth = 1;
    ctx.setLineDash([6, 4]);
    ctx.beginPath();
    ctx.moveTo(x + 0.5, 0);
    ctx.lineTo(x + 0.5, H);
    ctx.stroke();
    ctx.setLineDash([]);

    this.markerLabel(ctx, x, W, this.markerLabelY(H, 1),
      `RF CENTRE ${(this.rfCenterHz / 1e6).toFixed(3)}M${this.rfCenterNote}`,
      'rgba(120,200,255,0.95)');
    ctx.restore();
  }

  // ── Geometry helpers (shared with click-to-tune / drag-to-pan) ─────────────

  /** The centre actually on screen. This is the FRAME's centre — the app makes
   *  no attempt to render ahead of the server, and neither do we now. */
  displayCenterHz(): number {
    return this.centerHz;
  }

  hzToX(hz: number, W = this.canvas.width): number {
    const lo = this.displayCenterHz() - this.spanHz / 2;
    return ((hz - lo) / this.spanHz) * W;
  }

  /** CSS pixel x -> Hz. `cssW` lets a per-frame caller (the pointer-frequency label) pass a cached
   *  width instead of reading clientWidth — a layout read — every frame; same arithmetic either way. */
  /** ★★ THE FREQUENCY UNDER A POINT, BY THE ROW UNDER IT (Stuart, 2026-10-30, #57 — Gesiima: "I look back in the waterfall
   *  to see where exactly the signal was, and then I tune onto that position"). On the waterfall, the row's OWN scale (the
   *  view it was drawn at, through its map) — a trace drawn before a zoom or a tune tunes to that signal's frequency, not
   *  to whatever the live scale has there now. Above the waterfall, or on a row with no geometry, the live scale (xToHz):
   *  a row at the live scale gives exactly what xToHz does. */
  xyToHz(cssX: number, cssY: number, cssW = this.canvas.clientWidth, cssH = this.canvas.clientHeight): number {
    const H = this.canvas.height, wfH = this.wf.height, rows = this.xfA.length;
    const y = cssY * H / Math.max(1, cssH), specH = H - wfH;
    if (rows > 0 && wfH > 0 && y >= specH && y < H) {
      const j = Math.floor(y - specH);
      if (j < this.rowSeq) {
        const r = (this.head + j) % rows;
        const S = this.xfS[r], L = this.xfL[r];
        if (S > 0 && L > 0) return L + S * (this.xfA[r] * (cssX / Math.max(1, cssW)) + this.xfB[r]);
      }
    }
    return this.xToHz(cssX, cssW);
  }

  xToHz(cssX: number, cssW = this.canvas.clientWidth): number {
    const lo = this.displayCenterHz() - this.spanHz / 2;
    const frac = cssX / Math.max(1, cssW);
    return lo + frac * this.spanHz;
  }

  /** Hz per CSS pixel — for drag-to-pan. */
  hzPerPx(): number {
    return this.spanHz / Math.max(1, this.canvas.clientWidth);
  }
}
