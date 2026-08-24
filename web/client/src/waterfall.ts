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

export interface WaterfallOpts {
  /** Fraction of the canvas given to the spectrum trace (0 = waterfall only). */
  specRatio?: number;
  palette?: string;
}

/** Spectrum share of the canvas. Never 1.0 — the waterfall must survive. */
function clampRatio(r: number): number {
  return Math.max(0, Math.min(0.8, r));
}

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
  setSharpRows(on: boolean) { this.sharpRows = on; }
  private sharpRows = false;



  setSpeed(screenRowsPerSec: number) {
    this.screenSpeed = Math.max(1, screenRowsPerSec);
    this.recomputeRate();
  }
  private recomputeRate() { this.rowsPerSec = this.screenSpeed * renderDpr(); }
  private prevRow: Uint8Array | null = null;   // last received row
  private curRow: Uint8Array | null = null;    // newest received row
  private blendRow: Uint8Array | null = null;  // scratch for the synthesised line
  private lastArrival = 0;
  private slowGap = 0;        // worst recent arrival gap — see the sizing note
  private avgGap = 0;         // SMOOTHED arrival gap — what the row pacing follows
  private lastTickAt = 0;     // vsync pacing — see tick()
  private frameMs = 0;        // smoothed display frame interval
  private frameTick = 0;      // display frames counted toward the next row — see tick()
  private framesPerRow = 1;   // display frames per waterfall row — snapped to the refresh rate
  private emitStart = 0;
  private emitInterval = 0;   // ms between synthesised rows for this pair
  private emitTotal = 0;      // rows to synthesise between prev and cur
  private emitted = 0;

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
    }
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

  flushHeld() {
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
      this.held.push({ bins: new Float32Array(bins), centerHz, bwHz,
                       at: performance.now() });
      // Bound it: a link that stalls for a second must not queue a second of catch-up to grind
      // through afterwards. Oldest goes first — it is the stalest picture.
      const cap = Math.max(4, Math.ceil(this.holdMs / 20) + 4);
      while (this.held.length > cap) this.held.shift();
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
    }
  }

  private _pushNow(bins: Float32Array, centerHz: number, bwHz: number) {
    this.centerHz = centerHz;
    this.spanHz = bwHz;

    const frame = this.proc.process(bins, centerHz, bwHz);

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
    this.emitted = this.emitTotal;

    // Roll cur -> prev, and copy in the new row (frame.row is a reused buffer).
    if (!this.curRow || this.curRow.length !== row.length) {
      this.prevRow = new Uint8Array(row);
      this.curRow = new Uint8Array(row);
      this.blendRow = new Uint8Array(row.length);
    } else {
      this.prevRow!.set(this.curRow);
      this.curRow.set(row);
    }

    // How many lines to synthesise before the next frame lands. Derived from the
    // OBSERVED arrival gap, so it adapts to whatever rate the server is running —
    // no need to be told, and it self-corrects across a throttle change.
    const gap = this.lastArrival ? now - this.lastArrival : 1000 / this.rowsPerSec;
    this.lastArrival = now;
    // ★★ HEADROOM: size against the WORST recent gap, not this one. Server frames do not arrive
    //    evenly, and a pair sized for an average gap runs out of rows on a long one — the scroll
    //    stops dead until the next frame. Jumps straight to any slower gap and decays back gently,
    //    so it tracks the worst case rather than the mean; the surplus on a normal gap is simply
    //    discarded above.
    this.slowGap = Math.max(gap, this.slowGap * 0.98);
    // ★★★ PACE OFF THE SMOOTHED GAP, NOT THIS ONE. The row interval was derived from the
    //     INSTANTANEOUS arrival gap, so a server frame arriving 10 ms early or late changed the
    //     scroll rate for that whole pair — visible as the waterfall speeding up and slowing down
    //     again, which is exactly the band Stuart photographed. The feed genuinely varies ("the
    //     server dropping or increasing the odd frame here or there"), so the renderer must not
    //     follow it frame by frame: it follows the AVERAGE and lets the row accumulator carry the
    //     difference. That is what a jitter buffer does, without the added latency of one.
    this.avgGap = this.avgGap ? this.avgGap * 0.85 + gap * 0.15 : gap;

    // Clamp: a stalled link mustn't queue up hundreds of lines to catch up on.
    const clamped = Math.max(20, Math.min(1000, gap));
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
    const refreshHz = this.frameMs > 0 ? 1000 / this.frameMs : 60;
    const framesPerRow = Math.max(1, Math.round(refreshHz / Math.max(1, this.rowsPerSec)));
    const trueRowsPerSec = refreshHz / framesPerRow;
    this.framesPerRow = framesPerRow;
    this.emitTotal = this.sharpRows ? 1
                   : Math.max(1, Math.round(Math.max(clamped, this.slowGap) / (1000 / trueRowsPerSec)));
    // Smoothed, so arrival jitter does not become scroll-rate jitter. Falls back to the observed
    // gap until the average has anything in it.
    this.emitInterval = (this.avgGap || clamped) / this.emitTotal;
    this.emitStart = now;
    this.emitted = 0;
  }

  /** Emit any waterfall lines now due. Call once per animation frame.
   *  At 20fps in this draws exactly one row per frame (emitTotal === 1) and the
   *  blending collapses to a straight copy of the newest row. */
  tick() {
    // ★ Release BEFORE the early-return: on a cold start there is no curRow yet, and the very
    //   first held row is the one that creates it. Draining after the guard would deadlock — no
    //   row because no curRow, no curRow because no row.
    const now = performance.now();
    if (this.held.length) this._releaseHeld(now);
    if (!this.curRow || !this.prevRow) return;

    // ★★★ PACED BY VSYNC, NOT BY THE CLOCK. tick() is already called from requestAnimationFrame,
    //     but the EMISSION used to ask "is row k due yet?" against wall-clock times. At 20 rows/s
    //     on a 60 Hz display that wants a row every 3 frames — and whenever a due-time drifted
    //     across a frame boundary it became 3, 3, 4, 3, 4… That beat between the row rate and the
    //     refresh rate is the residual hitching (Stuart, 2026-08-03: "can we not tie it to device
    //     Vsync?").
    //     Now: accumulate rows-per-FRAME and emit whole rows as they come due. The cadence is
    //     locked to the display, so it is even by construction and adapts to 120 Hz for free.
    const dt = this.lastTickAt ? now - this.lastTickAt : 16.7;
    this.lastTickAt = now;
    // Smoothed refresh interval — one long frame (a GC pause, a tab switch) must not spike the
    // row rate, which would look exactly like the burst this replaces.
    this.frameMs = this.frameMs ? this.frameMs * 0.9 + Math.min(dt, 100) * 0.1 : dt;

    // ★★★ ONE ROW EVERY N DISPLAY FRAMES — N A WHOLE NUMBER. This is the crux, and it is why every
    //     pacing scheme before it failed. A waterfall row is a whole DEVICE pixel, and the row rate
    //     is screenSpeed x devicePixelRatio: 20 rows/s on a Retina panel is 40 device rows/s, which
    //     against a 60 Hz refresh is 0.667 rows per frame — so the emission pattern is 1, 1, 0,
    //     1, 1, 0… No amount of smoothing fixes that, because the QUANTITY is not commensurate with
    //     the display. Quantising the cadence to a whole number of frames makes the motion even by
    //     construction, at the cost of the rate landing on 60/N (30, 20, 15, 12…) instead of
    //     exactly what was asked for — a difference nobody can see, unlike the stutter.
    //     ★ Recomputed from the SMOOTHED frame interval, so a 120 Hz panel simply gets N twice as
    //       large and the same even result.
    // Decided at arrival, from the SAME refresh measurement that sized emitTotal — see there.
    const framesPerRow = this.sharpRows ? 1 : this.framesPerRow;
    let guard = 0;
    if (framesPerRow > 0) {
      this.frameTick++;
      while (this.emitted < this.emitTotal && this.frameTick >= framesPerRow && guard++ < 4) {
        this.frameTick -= framesPerRow;
        this.emitted++;
        this.drawRow(this.emitted / this.emitTotal);
      }
      // Do not bank more than one row's worth of credit: a stall must not repay as a burst.
      if (this.frameTick > framesPerRow) this.frameTick = framesPerRow;
    }


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
    blend.set(t >= 0.5 ? cur : prev);
    void n;

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
      this.head = (this.head - 1 + rows) % rows;
      this.gl.pushRow(rowIn, this.head);
      return;
    }

    // 2D fallback: peak-preserving downsample to canvas width + palette LUT + putImageData.
    const H = wfH;
    this.head = (this.head - 1 + H) % H;
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

  /** Composite waterfall + spectrum trace + markers to the visible canvas. */
  draw() {
    const W = this.canvas.width;
    const H = this.canvas.height;
    if (!W || !H) return;
    const ctx = this.ctx;
    const specH = H - this.wf.height;

    ctx.fillStyle = '#000';
    ctx.fillRect(0, 0, W, specH);
    // Straight blit, in BIN space — exactly what the app does (its shader maps
    // display x directly to bins: tx = xy.x / uDrawW * uTexW). No pan offset, no
    // frequency-indexed history, no re-alignment. Old rows keep their old bins
    // and scroll away.
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
      this.gl.render(this.head, wfH, W, wfH);
      ctx.drawImage(this.glCanvas, 0, 0, W, wfH, 0, specH, W, wfH);
    } else {
      // 2D fallback: unwrap the ring with two blits (head→bottom, then top→head).
      const first = wfH - this.head;            // rows from head to the end of the buffer
      if (first > 0) ctx.drawImage(this.wf, 0, this.head, W, first, 0, specH, W, first);
      if (this.head > 0) ctx.drawImage(this.wf, 0, 0, W, this.head, 0, specH + first, W, this.head);
    }

    if (specH > 4 && this.spec && this._showSpec) {
      this._drawSpec(ctx, W, specH);
      this.onDrawAxis?.(ctx, W, specH);   // dB axis, drawn by main (owns the labels)
    }
    this._drawVfo(ctx, W, H);
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
    if (!prevSpec || !prevPeak || !ds || !dp || !this.emitInterval) return { spec, peak };

    const span = this.emitInterval * this.emitTotal;   // observed gap between frames
    const t = Math.max(0, Math.min(1, (performance.now() - this.emitStart) / span));
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
      ctx.shadowBlur = 2;
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
    if (nX > loXc) {
      const g = ctx.createLinearGradient(loXc, 0, nX, 0);
      g.addColorStop(0,    rgba(col, 0.03));
      g.addColorStop(0.15, rgba(col, 0.06));
      g.addColorStop(0.55, rgba(col, 0.14));
      g.addColorStop(1,    rgba(col, 0.28));
      ctx.fillStyle = g;
      ctx.fillRect(loXc, 0, nX - loXc, H);
    }
    if (hiXc > nX) {
      const g = ctx.createLinearGradient(nX, 0, hiXc, 0);
      g.addColorStop(0,    rgba(col, 0.28));
      g.addColorStop(0.45, rgba(col, 0.14));
      g.addColorStop(0.85, rgba(col, 0.06));
      g.addColorStop(1,    rgba(col, 0.03));
      ctx.fillStyle = g;
      ctx.fillRect(nX, 0, hiXc - nX, H);
    }

    // Sideband edges: soft glow + crisp acrylic line (both at 0.35).
    const edge = (x: number) => {
      if (x <= 0 || x >= W) return;
      ctx.strokeStyle = rgba(col, 0.35);
      ctx.lineWidth = Math.max(0.75, 0.75);
      ctx.shadowColor = rgba(col, 0.35);
      ctx.shadowBlur = 8;
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

  /** CSS pixel x -> Hz. */
  xToHz(cssX: number): number {
    const lo = this.displayCenterHz() - this.spanHz / 2;
    const frac = cssX / Math.max(1, this.canvas.clientWidth);
    return lo + frac * this.spanHz;
  }

  /** Hz per CSS pixel — for drag-to-pan. */
  hzPerPx(): number {
    return this.spanHz / Math.max(1, this.canvas.clientWidth);
  }
}
