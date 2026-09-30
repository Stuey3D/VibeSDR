/**
 * meters.ts — the SIGNAL METERS' rules (faceplates brief §4; Deck.mockup `segs`, `edge`, the `L` layout).
 *
 * Everything here is pure (no React, no Skia) so scripts/test_faceplate_meters.ts can check it, and
 * everything the UI thread runs per frame is a `'worklet'` so the meters can call it there.
 *
 *   §4.1  portraitDeck()    ONE DECK HEIGHT per chassis: a fixed block with the display flexing inside
 *   §4.3  the LED table     ten segments, their colours, thresholds, and the squelch ring's segment
 *   §4.4  edge brightness   Φ((μ − T)/σ), the σ window, the eye filter, the steady-LED hysteresis
 *   §4.5  the needles       scale points, spring ballistics, the peak needle's hold and drift
 *
 * ★★★ ONE SCALE FOR EVERYTHING. The meter bus carries `level`, `peak` and the squelch `sql` in the SAME
 *   bar-normalised 0..1 scale — the bar's own contract (MeterValues.sql), and the only scale in which
 *   every backend's squelch line is known to sit where its gate compares (Kiwi dBm, a dongle's dBFS,
 *   radiod's SNR). The LEDs and the needles map that ONE number through ONE table (`vuPos`), and the
 *   squelch ring and the red hand go through the SAME table (§4.3 TRAP: "place the ring with the same
 *   S-unit table the segments use, or it sits one LED off from where audio really opens").
 * ★ The table is uniform — the mockup's own model: level 0..10 lights segments linearly, and the
 *   bar's squelch line sits at `(sq + 0.5) × 10 %` for a ring on segment `sq`. The printed labels
 *   (S1 … +60) are the mockup's scale card. See VU_LABELS for why they are nominal.
 */

// ── §4.1 One deck height ──────────────────────────────────────────────────────

export type MeterKind = 'bar' | 'vu' | 'edge';

/** Scale-1 sizes from Deck.mockup's portrait `L` + §4.1's table. */
export const DECK = {
  /** The mockup's bar (72) and bar-meter key slot (58): what the fixed block is BUILT from. */
  barH:          72,
  barKey:        58,
  /** Today's bar on the DEFAULT chassis (ControlsBar SIG_H; tablet 62) and today's 44 pt key. */
  todayBarH:     40,
  todayBarHTab:  62,
  todayKey:      44,
  /** LED / analogue keys: 44 (shared 34), legends 88 % (shared 80 %) — Deck.mockup `capH`, `scale()`. */
  compactKey:    44,
  compactKeyShared: 34,
  legendScale:   0.88,
  legendScaleShared: 0.8,
  /** The SHARED TUNER banner above the frequency window, and its gap. */
  bannerH:       18,
  bannerGap:     5,
  /** Frequency window ↔ meter gap (`gap: 6px` in the VU column). */
  meterGap:      6,
  /** LED housing: padding 6 (shared 3) top, 3 bottom; LED 13; 2 gap; label 8 (§4.3). */
  ledPadTop:     6,
  ledPadTopShared: 3,
  ledPadBottom:  3,
  ledH:          13,
  ledLabelGap:   2,
  ledLabel:      8,
  /** Edgewise housing: padding 3 (shared 2) around a 28 pt window (§4.5: 34 pt housing). */
  edgePad:       3,
  edgePadShared: 2,
  edgeWindow:    28,
} as const;

export interface DeckLayout {
  /** Bar meter or the compact LED / analogue display column. */
  compact:     boolean;
  /** The display area's height: the bar frame, or banner + frequency window + meter housing. */
  displayH:    number;
  keySlot:     number;
  /** Legend scale on the keys (1 on the bar deck). */
  legendScale: number;
  /** Compact only (0 otherwise). */
  bannerH:     number;
  bannerGap:   number;
  meterGap:    number;
  housingH:    number;
  /** What the flexing frequency window ends up — derived, never set (the brief: no per-state magic). */
  freqH:       number;
  /** The fixed block: displayH + rowGap + keySlot. The same for every meter × shared on a chassis,
   *  except the default chassis's BAR, which is today's (see portraitDeck). */
  blockH:      number;
}

/**
 * ★★★ ONE DECK HEIGHT (§4.1): "Make the deck one size so it's not growing or shrinking."
 *
 * The block (display area + row gap + keys) is FIXED per chassis at the mockup's bar deck —
 * 72 + gap + 58 — and every compact layout is carved out of it: the keys take their §4.1 slot, the
 * meter housing and (shared) banner take theirs, and the FREQUENCY WINDOW FLEXES into what is left.
 * The §4.1 window sizes (48 / 38 / 46 / 35) are the result, not an input — the test proves it.
 *
 * ★★ THE ONE PLACE THE BRIEF CANNOT BE MET WHOLE — the default chassis's bar. The mockup draws
 *   "Default (today)" with a 72 pt bar and 58 pt keys; the app's bar is 40 pt (62 on a tablet) with
 *   44 pt keys, and §4.2 / §13.1 require it pixel-identical. Today's 91 pt block cannot hold an LED
 *   strip, a 48 pt window and 44 pt keys, so on the DEFAULT chassis the bar keeps today's height and
 *   the LED / analogue deck is the (taller) fixed block — constant across vu × edge × shared, but not
 *   equal to the bar's. Silver and black follow the mockup exactly: their bar frame is the mockup's
 *   72 pt, so all three meters and both shared states are ONE height.
 *
 * @param cap     the chassis draws cap keys (silver / black) — its bar deck is the mockup's
 * @param rowGap  the gap between the display and the keys (ControlsBar ROW_GAP, already scaled)
 * @param r       the UI scale's rounding (s.r)
 */
export function portraitDeck(o: { cap: boolean; meter: MeterKind; shared: boolean; tablet: boolean;
                                  rowGap: number; r: (n: number) => number }): DeckLayout {
  const { r, rowGap } = o;
  const fixedBlock = r(DECK.barH) + rowGap + r(DECK.barKey);
  if (o.meter === 'bar') {
    if (!o.cap) {
      const displayH = r(o.tablet ? DECK.todayBarHTab : DECK.todayBarH);
      const keySlot = r(DECK.todayKey);
      return { compact: false, displayH, keySlot, legendScale: 1, bannerH: 0, bannerGap: 0, meterGap: 0,
               housingH: 0, freqH: 0, blockH: displayH + rowGap + keySlot };
    }
    // ★ Metal: the bar frame is whatever the fixed block leaves above the 58 pt keys (= the mockup's 72;
    //   a tablet's 62 is below it, so the tablet gets the same).
    const keySlot = r(DECK.barKey);
    const displayH = fixedBlock - rowGap - keySlot;
    return { compact: false, displayH, keySlot, legendScale: 1, bannerH: 0, bannerGap: 0, meterGap: 0,
             housingH: 0, freqH: 0, blockH: fixedBlock };
  }
  const keySlot = r(o.shared ? DECK.compactKeyShared : DECK.compactKey);
  const displayH = fixedBlock - rowGap - keySlot;
  const bannerH = o.shared ? r(DECK.bannerH) : 0;
  const bannerGap = o.shared ? r(DECK.bannerGap) : 0;
  const meterGap = r(DECK.meterGap);
  const housingH = o.meter === 'vu'
    ? r(o.shared ? DECK.ledPadTopShared : DECK.ledPadTop) + r(DECK.ledH) + r(DECK.ledLabelGap)
      + r(DECK.ledLabel) + r(DECK.ledPadBottom)
    : 2 * r(o.shared ? DECK.edgePadShared : DECK.edgePad) + r(DECK.edgeWindow);
  const freqH = displayH - bannerH - bannerGap - meterGap - housingH;
  return { compact: true, displayH, keySlot, legendScale: o.shared ? DECK.legendScaleShared : DECK.legendScale,
           bannerH, bannerGap, meterGap, housingH, freqH, blockH: fixedBlock };
}

/**
 * ★ §4.1 TRAP: 34 pt keys are below 44 pt — extend hitSlop into the gaps. Upward it may take the whole
 * row gap (the meter housing above is not a touch target); downward only half (the drums below are
 * drag targets); sideways half the column gap each way, so neighbours never overlap.
 */
export function compactKeyHitSlop(slot: number, rowGap: number, colGap: number) {
  const top = rowGap, bottom = rowGap / 2;
  return { top, bottom, left: colGap / 2, right: colGap / 2, reach: slot + top + bottom };
}


/** Is the squelch muting? The gate's own verdict when the backend gives one, else bar geometry — the
 *  one rule the bar, the mode box's SQL and the LED / analogue meters all read (MeterValues.gate). */
export function sqlClosedOf(sql: number | undefined, gate: boolean | undefined, level: number): boolean {
  const on = sql != null && sql >= 0;
  return on && (gate ?? (level < (sql as number)));
}

// ── The LED table (§4.3) ──────────────────────────────────────────────────────

export const VU_SEGMENTS = 10;
/**
 * The printed scale — S9 is the top of the green (§4.3).
 * ★ NOMINAL, like the mockup: the bar's 0..1 scale is SNR-compressed in SNR mode and 90 dB of dBFS in
 *   S-meter / dBFS mode, and neither is S-units in 12 / 10 dB steps. A calibrated S-unit table would
 *   need the squelch threshold in the same dB on every backend (it is not on the bus today) — change
 *   `VU_THRESHOLDS` and both the LEDs and the ring follow, because both read it.
 */
export const VU_LABELS = ['S1', 'S3', 'S5', 'S7', 'S9', '+10', '+20', '+30', '+40', '+60'] as const;
export type LedColourName = 'green' | 'orange' | 'red';
/** 5 green / 3 orange / 2 red. ★ Never either colour setting — these are the LEDs' own colours. */
export function ledColourOf(i: number): LedColourName {
  return i < 5 ? 'green' : i < 8 ? 'orange' : 'red';
}

/** §4.3 hot / hi / base / dark / offCentre / offEdge, and the glow (Deck.mockup COLS). */
export const LED_SPEC: Record<LedColourName, { hot: string; hi: string; base: string; dark: string;
    offCentre: string; offEdge: string; glow: string; glowRgb: string }> = {
  green:  { hot: '#e9ffe9', hi: '#7dff9c', base: '#22d24e', dark: '#0c7a26', offCentre: '#1d3a23', offEdge: '#0b170e',
            glow: 'rgba(47,224,90,0.70)',  glowRgb: '47,224,90' },
  orange: { hot: '#fff3dc', hi: '#ffc36b', base: '#ff8a12', dark: '#a34a05', offCentre: '#3d2811', offEdge: '#170f06',
            glow: 'rgba(255,148,22,0.70)', glowRgb: '255,148,22' },
  red:    { hot: '#fff0ee', hi: '#ff8a80', base: '#f2231a', dark: '#8d0c07', offCentre: '#3e1613', offEdge: '#180807',
            glow: 'rgba(255,45,34,0.75)',  glowRgb: '255,45,34' },
};
/** The squelch ring: green open, red closed, full strength always (§4.3). */
export const RING_OPEN = '#3dff72';
export const RING_CLOSED = '#ff3a2e';

/** Segment units per dB-ish: the bar spans 90 dB in dBFS mode, so a segment is 9 dB. It sets what
 *  "σ floor 1.5 dB" and "~1 dB hysteresis" mean on the bar-normalised scale. */
export const DB_PER_SEG = 9;

/** Each segment's threshold, in segment units: its CENTRE (i + 0.5), which is where the bar's squelch
 *  line sits for a ring on that segment (Deck.mockup `sqlPct: (sq + 0.5) × 10 %`). */
export const VU_THRESHOLDS: readonly number[] = Array.from({ length: VU_SEGMENTS }, (_, i) => i + 0.5);

/** Bar-normalised level (0..1) → segment position (0..10). THE one mapping (see the header). */
export function vuPos(norm: number): number {
  'worklet';
  return 10 * Math.max(0, Math.min(1, norm));
}

/**
 * The squelch ring's segment: the one whose THRESHOLD is nearest the squelch position — the LED that
 * is half-lit at the level where the gate opens. −1 = squelch off (−1 on the bus): no ring.
 * ★ Reads VU_THRESHOLDS, the table the segments light from, so the two can never disagree.
 */
export function ringSegment(sqlNorm: number | undefined | null, thresholds: readonly number[] = VU_THRESHOLDS): number {
  'worklet';
  if (sqlNorm == null || !(sqlNorm >= 0)) return -1;
  const p = vuPos(sqlNorm);
  let best = 0, bestD = Infinity;
  for (let i = 0; i < thresholds.length; i++) {
    const d = Math.abs(thresholds[i] - p);
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

/** §4.3 peak hold: one segment, at full brightness, above the level; drops after ~1 s. */
export const PEAK_HOLD_MS = 1000;
export interface PeakHold { idx: number; at: number }
/** `top` = the highest segment that is (at least half) lit now, −1 for none. Returns the held peak
 *  segment, or −1 when it is not above the level (nothing extra to draw). */
export function peakStep(p: PeakHold, top: number, nowMs: number, holdMs = PEAK_HOLD_MS): number {
  'worklet';
  if (top >= p.idx) { p.idx = top; p.at = nowMs; }
  else if (nowMs - p.at > holdMs) { p.idx = top; p.at = nowMs; }
  return p.idx > top ? p.idx : -1;
}

// ── §4.4 The edge LED: partial brightness ─────────────────────────────────────

/** Φ, the standard normal CDF (Abramowitz & Stegun 7.1.26, |ε| < 1.5e-7). */
export function phi(x: number): number {
  'worklet';
  const z = Math.abs(x) / Math.SQRT2;
  const t = 1 / (1 + 0.3275911 * z);
  const y = 1 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * Math.exp(-z * z);
  return x >= 0 ? 0.5 * (1 + y) : 0.5 * (1 - y);
}

/** σ's floor: 1.5 dB (§4.4). A steady carrier gets a crisp edge, never an infinitely sharp one. */
export const SIGMA_FLOOR_DB = 1.5;
/** The running window for σ: ~0.5 s. */
export const SIGMA_WINDOW_MS = 500;

/** A running window of samples (ms, value). Mutated in place; the meter keeps one per mount. */
export interface SampleWindow { t: number[]; v: number[] }
export function makeWindow(): SampleWindow { return { t: [], v: [] }; }
/** Add a sample, drop what is older than `spanMs`; returns the window's population std-dev. */
export function pushSample(w: SampleWindow, tMs: number, v: number, spanMs = SIGMA_WINDOW_MS): number {
  w.t.push(tMs); w.v.push(v);
  while (w.t.length > 1 && tMs - w.t[0] > spanMs) { w.t.shift(); w.v.shift(); }
  const n = w.v.length;
  let m = 0;
  for (const x of w.v) m += x;
  m /= n;
  let s = 0;
  for (const x of w.v) s += (x - m) * (x - m);
  return Math.sqrt(s / n);
}

/**
 * brightness(T) = Φ((μ − T) / σ): the fraction of time a signal of mean μ and spread σ spends above
 * the threshold — which is what the eye sees of a real LM3915 LED switching fully on and off (§4.4).
 * All in dB; σ is floored. ★ No linear ramp between thresholds (§4.4 TRAP): a steady carrier between
 * two thresholds lights the lower one fully and the upper one not at all.
 */
export function edgeBrightness(muDb: number, thresholdDb: number, sigmaDb: number): number {
  'worklet';
  const s = Math.max(SIGMA_FLOOR_DB, sigmaDb);
  return phi((muDb - thresholdDb) / s);
}

/** §4.4 eye filter: exponential, τ ≈ 100 ms, and at most 0.35 change per frame. ★ The ONLY easing. */
export const EYE_TAU_MS = 100;
export const EYE_MAX_STEP = 0.35;
export function eyeStep(b: number, target: number, dtMs: number, tauMs = EYE_TAU_MS, maxStep = EYE_MAX_STEP): number {
  'worklet';
  const dt = Math.max(0, Math.min(250, dtMs));   // a stalled frame must not become one giant jump
  let d = (target - b) * (1 - Math.exp(-dt / tauMs));
  if (d > maxStep) d = maxStep; else if (d < -maxStep) d = -maxStep;
  return b + d;
}

/** "Steady LEDs": solid on / off with ~1 dB of hysteresis — on above T + ½, off below T − ½. */
export const STEADY_HYST_DB = 1;
export function steadyLit(wasLit: boolean, muDb: number, thresholdDb: number, hystDb = STEADY_HYST_DB): boolean {
  'worklet';
  return wasLit ? muDb > thresholdDb - hystDb / 2 : muDb > thresholdDb + hystDb / 2;
}

/**
 * One segment's TARGET brightness this frame (before the eye filter).
 * • steady (setting, or OS Reduce Motion): hysteresis, 0 / 1.
 * • squelch muting: the dim level only — no σ shimmer under the red ring (§4.4) — so the plain
 *   threshold, 0 / 1.
 * • otherwise: Φ((μ − T)/σ).
 */
export function segmentTarget(i: number, muPos: number, sigmaPos: number, steady: boolean, muting: boolean,
                              wasLit: boolean, thresholds: readonly number[] = VU_THRESHOLDS): number {
  'worklet';
  const mu = muPos * DB_PER_SEG, T = thresholds[i] * DB_PER_SEG;
  if (steady) return steadyLit(wasLit, mu, T) ? 1 : 0;
  if (muting) return mu > T ? 1 : 0;
  return edgeBrightness(mu, T, sigmaPos * DB_PER_SEG);
}

// ── §4.5 The edgewise needles ─────────────────────────────────────────────────

/** Scale inset each side (§4.5 "scale points at 8 + (i + 0.5) × (width − 16) / 10"). */
export const EDGE_INSET = 8;
/** x of a scale point `i` (label / major tick) in a window `w` wide. */
export function scalePointX(i: number, w: number): number {
  return EDGE_INSET + (i + 0.5) * (w - 2 * EDGE_INSET) / VU_SEGMENTS;
}
/** x of a segment POSITION (0..10) — the same table: position T_i lands on scale point i. */
export function needleX(pos: number, w: number): number {
  'worklet';
  const p = Math.max(0, Math.min(VU_SEGMENTS, pos));
  return EDGE_INSET + p * (w - 2 * EDGE_INSET) / VU_SEGMENTS;
}

export interface SpringParams { mass: number; stiffness: number; damping: number }
/**
 * Signal ballistics (§4.5): a real VU movement — 99 % in 300 ms with ~1 % overshoot; with Reduce
 * Motion, CRITICALLY damped (no overshoot, same 300 ms). Mass 1, so ω = √k and ζ = c / 2ω.
 *   ζ = 0.82 → overshoot e^(−ζπ/√(1−ζ²)) ≈ 1.1 %; ω chosen so the envelope is inside 1 % at 300 ms.
 *   ζ = 1   → (1 + ωt)·e^(−ωt) = 0.01 at ωt ≈ 6.64.
 */
export function needleSpring(reduceMotion: boolean): SpringParams {
  if (reduceMotion) {
    const w = 6.64 / 0.3;
    return { mass: 1, stiffness: w * w, damping: 2 * w };
  }
  const z = 0.82;
  const w = Math.log(100 / Math.sqrt(1 - z * z)) / (z * 0.3);
  return { mass: 1, stiffness: w * w, damping: 2 * z * w };
}

/** Peak needle: holds ~1 s, then drifts down ~6 dB/s, EASING IN (the drift speeds up over ~0.3 s). */
export const PEAK_NEEDLE_HOLD_MS = 1000;
export const PEAK_NEEDLE_DB_PER_S = 6;
export const PEAK_NEEDLE_EASE_MS = 300;
export interface PeakNeedle { pos: number; heldMs: number }
/**
 * One frame of the peak needle. ★ It is PUSHED by the signal needle's ON-SCREEN position (§4.5:
 * `peak = max(peak, signalNeedleAnimatedPos)`), never the raw level, so it can never jump ahead.
 */
export function peakNeedleStep(p: PeakNeedle, needlePos: number, dtMs: number): number {
  'worklet';
  const dt = Math.max(0, Math.min(250, dtMs));
  if (needlePos >= p.pos) { p.pos = needlePos; p.heldMs = 0; return p.pos; }
  p.heldMs += dt;
  if (p.heldMs <= PEAK_NEEDLE_HOLD_MS) return p.pos;
  const since = p.heldMs - PEAK_NEEDLE_HOLD_MS;
  const ease = Math.min(1, since / PEAK_NEEDLE_EASE_MS);
  const rate = (PEAK_NEEDLE_DB_PER_S / DB_PER_SEG) * ease * ease;   // segment units per second
  p.pos = Math.max(needlePos, p.pos - rate * dt / 1000);
  return p.pos;
}

// ── §9 Landscape ──────────────────────────────────────────────────────────────

/**
 * Deck.mockup's landscape `L` (scale 1): grid `minmax(0,1fr) 62 360 62 minmax(0,1fr)`, rows 28 / 28 /
 * auto, column gap 8, row gap 6 — a 62 pt control band — and the display column's LED strip (padding
 * 3 6 2, LEDs 9, labels 6.5) or edgewise window (padding 2, window 24 with the 28 pt print shifted up
 * 2). `today*` are TODAY's LandscapeBar (ControlsBar: DRUM_H 44, SIG_H 40 / tablet 62, BTN_W 56, the
 * 340 pt display, GAP 6).
 */
export const LAND = {
  band:          62,
  keyW:          62,
  dispW:         360,
  colGap:        8,
  rowGap:        6,
  todayDrum:     44,
  todayBar:      40,
  todayBarTab:   62,
  todayKeyW:     56,
  todayDispW:    340,
  todayGap:      6,
  /** Frequency window ↔ meter gap (the VU column's `gap: 6px`), and the squeezed one. */
  meterGap:      6,
  meterGapTight: 3,
  ledPadTop:     3,
  ledPadBottom:  2,
  /** Without the labels the strip keeps a symmetric 3 pt below the LEDs. */
  ledPadBottomBare: 3,
  ledPadX:       6,
  ledH:          9,
  ledLabelGap:   2,
  ledLabel:      6.5,
  edgePad:       2,
  edgeWindow:    24,
  /** The print is designed on a 28 pt card and drawn 2 pt up in the 24 pt window (`svgTop: -2px`). */
  edgePrint:     28,
  edgePrintTop:  -2,
  /** Below this the frequency window is not worth having: the tube stack's floor is 18 (MIN_GLASS 8 +
   *  pip, collar, clearances), and the digits need a little air over it. */
  minFreq:       20,
  /** Below this an edgewise card is only ticks — show the bar instead. */
  minEdgeWindow: 14,
  /** §9 TRAP: below ~740 pt the LED strip loses its labels and the analogue meter becomes the bar. */
  smallW:        740,
  /** Hyperlegible digits 25; mode box 15 / 11 in a 70 pt box (as portrait). */
  digit:         25,
  modeBox:       70,
  modeFont:      15,
  readingFont:   11,
  /** Legibility floors for the mode box (absolute pt — a floor does not scale down). */
  minModeFont:   9,
  minReadingFont: 7,
  /** §9 legends 78 % (the mockup's `transform: scale(0.78)`), on the cap keys. */
  legendScale:   0.78,
  /** Black's gloss panel: padding 4 — drawn OUTSIDE the display column, in the gaps, so it costs no
   *  height (see landscapeDeck). */
  glossPad:      4,
  /** SDRScreen `pillWrap`: 8 pt each side of the bar. */
  screenMargin:  8,
  /** ControlsBar's drum columns: `minWidth: s.r(80)`. */
  drumMin:       80,
} as const;

export interface LandscapeLayout {
  /** The control band — TODAY's (the taller of the 44 pt drum and the bar frame), on every chassis,
   *  meter and shared state. §9: "never gets taller than today's bar". */
  bandH:       number;
  /** Every key is exactly this tall (§11): half the band less the row gap. */
  keyH:        number;
  rowGap:      number;
  colGap:      number;
  keyW:        number;
  dispW:       number;
  /** Each drum column's width (the `minmax(0,1fr)` tracks), for the geometry test. */
  drumW:       number;
  /** The plate's side padding (silver 24 so the screws clear the drums, §9). */
  padH:        number;
  /** The meter actually drawn — the setting, or the bar where the analogue card cannot fit (§9 TRAP). */
  meter:       MeterKind;
  /** The bar frame's height (bar meter only): today's on the default chassis, the whole band on metal. */
  barH:        number;
  /** Compact (LED / analogue) column; zeros on the bar. */
  freqH:       number;
  meterGap:    number;
  housingH:    number;
  ledPadTop:   number;
  ledPadBottom: number;
  ledPadX:     number;
  ledH:        number;
  /** 0 = the strip has no labels (small screens, §9 TRAP). */
  labelH:      number;
  labelGap:    number;
  edgePad:     number;
  edgeWindow:  number;
  /** The edgewise print's design height and its offset in the window (the mockup's 28 at −2 in 24). */
  edgePrintH:  number;
  edgePrintTop: number;
  digit:       number;
  modeFont:    number;
  readingFont: number;
  /** Key legend scale: today's on the default deck, 78 % on the cap keys. */
  legendScale: number;
  /** Black: the gloss panel's reach beyond the display column on every side. */
  glossOut:    number;
}

/**
 * ★★★ THE LANDSCAPE DECK (§9). One band of controls — `[VFO drum] [step / cog] [display] [audio / chat]
 * [zoom drum]` — then the status row, and ★ NEVER TALLER THAN TODAY'S BAR.
 *
 * ★★ WHERE THE BRIEF CANNOT BE MET WHOLE: the mockup's band is 62 pt and it calls that "today". It is
 *   today's band on a TABLET (the 62 pt bar frame), but on a phone today's band is 44 pt (the 44 pt drum
 *   over a 40 pt bar). "Never taller than today's" is the hard rule (§9, A2's own caption, §4.1's one
 *   deck height), so the band is today's everywhere and the mockup's column is FITTED into it: the
 *   frequency window flexes (as in portrait), the gap tightens, the LED labels go, and last — where the
 *   mode box could no longer be read (the SE's 32 pt band) — the meter gives way to the bar, §9's own
 *   "or the bar". On a tablet every mockup number comes out exactly.
 *
 * Columns: the default chassis keeps today's (56 / 340 / 6), so its bar deck is pixel-for-pixel today's
 * and switching meter moves nothing sideways; silver and black take the mockup's grid (62 / 360 / 8).
 *
 * @param plate  null on the default chassis; silver has screws, black the gloss panel
 * @param W      the window width (pt)
 * @param scale  the UI scale's factor (the 6.5 pt label is not rounded); `r` its rounding (s.r)
 */
export function landscapeDeck(o: { plate: { screws: boolean; gloss: boolean } | null; meter: MeterKind;
                                   tablet: boolean; W: number; scale: number; r: (n: number) => number;
                                   singleDrum?: boolean }): LandscapeLayout {
  const { r, plate } = o;
  const bandH = Math.max(r(LAND.todayDrum), r(o.tablet ? LAND.todayBarTab : LAND.todayBar));
  const rowGap = r(LAND.rowGap);
  const keyH = (bandH - rowGap) / 2;
  const colGap = r(plate ? LAND.colGap : LAND.todayGap);
  const keyW = r(plate ? LAND.keyW : LAND.todayKeyW);
  const dispW = r(plate ? LAND.dispW : LAND.todayDispW);
  const padH = !plate ? r(12) : r(plate.screws ? 24 : 12);
  const inner = o.W - 2 * LAND.screenMargin - 2 * padH;
  const drums = o.singleDrum ? 1 : 2;
  const drumW = (inner - 2 * keyW - dispW - (drums + 2) * colGap) / drums;
  const small = o.W < LAND.smallW;
  const minFreq = r(LAND.minFreq);
  const zero = { freqH: 0, meterGap: 0, housingH: 0, ledPadTop: 0, ledPadBottom: 0, ledPadX: 0, ledH: 0,
                 labelH: 0, labelGap: 0, edgePad: 0, edgeWindow: 0, edgePrintH: 0, edgePrintTop: 0 };
  const base = { bandH, keyH, rowGap, colGap, keyW, dispW, drumW, padH,
                 legendScale: plate ? LAND.legendScale : 1, glossOut: plate?.gloss ? r(LAND.glossPad) : 0 };
  // The window's type: capped at the mockup's sizes, shrunk to the window. The mode box stacks the mode
  // over the reading, so the reading takes what the mode's line leaves.
  // (ModeReadout sets each line at round(size × 1.15).)
  const lh = (n: number) => Math.round(n * 1.15);
  const fit = (cap: number, room: number) => { let v = cap; while (v > 1 && lh(v) > room) v--; return v; };
  const fonts = (freqH: number) => {
    const modeFont = fit(Math.min(r(LAND.modeFont), Math.floor(freqH * 0.5)), freqH - lh(LAND.minReadingFont));
    return {
      digit:       Math.min(r(LAND.digit), Math.floor((freqH - 2) / 1.12)),
      modeFont,
      readingFont: fit(r(LAND.readingFont), freqH - lh(modeFont)),
    };
  };
  /** ★ Legible, or not drawn: a mode box below 9 / 7 pt cannot be read, and the bar (today's, which
   *  has room for both) is the honest fallback — §9's own "or the bar". */
  const legible = (f: { modeFont: number; readingFont: number }) =>
    f.modeFont >= LAND.minModeFont && f.readingFont >= LAND.minReadingFont;
  const bar = (): LandscapeLayout => ({
    ...base, ...zero, meter: 'bar', digit: 0, modeFont: 0, readingFont: 0,
    // ★ Default: today's frame, top-aligned in the band. Metal: the bar fills the band (the mockup's
    //   `barH: 62px` = its band).
    barH: plate ? bandH : r(o.tablet ? LAND.todayBarTab : LAND.todayBar),
  });
  if (o.meter === 'bar') return bar();

  if (o.meter === 'vu') {
    const padTop = r(LAND.ledPadTop), ledH = r(LAND.ledH), ledPadX = r(LAND.ledPadX);
    const labelled = r(LAND.ledPadBottom) + r(LAND.ledLabelGap) + LAND.ledLabel * o.scale;
    let meterGap = r(LAND.meterGap);
    const labels = !small && bandH - meterGap - (padTop + ledH + labelled) >= minFreq;
    const housingH = padTop + ledH + (labels ? labelled : r(LAND.ledPadBottomBare));
    if (bandH - meterGap - housingH < minFreq) meterGap = r(LAND.meterGapTight);
    const freqH = bandH - meterGap - housingH;
    const f = fonts(freqH);
    if (freqH < minFreq || !legible(f)) return bar();
    return { ...base, ...zero, meter: 'vu', barH: 0, freqH, meterGap, housingH,
             ledPadTop: padTop, ledPadBottom: labels ? r(LAND.ledPadBottom) : r(LAND.ledPadBottomBare), ledPadX, ledH,
             labelH: labels ? LAND.ledLabel * o.scale : 0, labelGap: labels ? r(LAND.ledLabelGap) : 0,
             ...f };
  }

  // Analogue. ★ §9 TRAP: below ~740 pt the card cannot be read — the bar, not a smear of ticks.
  if (small) return bar();
  const edgePad = r(LAND.edgePad);
  let meterGap = r(LAND.meterGap);
  let edgeWindow = r(LAND.edgeWindow);
  if (bandH - meterGap - 2 * edgePad - edgeWindow < minFreq) {
    meterGap = r(LAND.meterGapTight);
    edgeWindow = Math.min(edgeWindow, bandH - meterGap - 2 * edgePad - minFreq);
  }
  if (edgeWindow < r(LAND.minEdgeWindow)) return bar();
  const housingH = 2 * edgePad + edgeWindow;
  const freqH = bandH - meterGap - housingH;
  const f = fonts(freqH);
  if (!legible(f)) return bar();
  const k = edgeWindow / LAND.edgeWindow;
  return { ...base, ...zero, meter: 'edge', barH: 0, freqH, meterGap, housingH, edgePad, edgeWindow,
           edgePrintH: LAND.edgePrint * k, edgePrintTop: LAND.edgePrintTop * k, ...f };
}
