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
