/**
 * drumWell.ts — the drum well's geometry and the chassis rules that are not plain colours
 * (faceplates brief §6.1, §6.2). Pure: no React, no Skia, so scripts/test_faceplate_wells.ts checks
 * every rule directly. DrumWheel and TunerKeys draw from here; components/DrumWell.tsx is the one
 * place that turns it into Skia.
 *
 * Numbers are Deck.mockup's (`W_FACE`, `w.poolA/poolB`, `tk`), which wins over the brief.
 */

import type { ChassisTokens } from './faceplate';

// ── §6.1 The LED pool that replaces the index needle ─────────────────────────────
/**
 * ★★ The red index needle is gone on EVERY chassis. In its place only the LED glowing through from
 * behind the drum: `radial-gradient(60% 75% at 50% 18%, L(.16), L(.05) 55%, transparent)` over the
 * drum body. CSS sizes a radial by its RADII, as fractions of the box, and centres it in the box.
 */
export const POOL = {
  rx: 0.60, ry: 0.75, cx: 0.50, cy: 0.18,
  alphas: [0.16, 0.05] as const,
  positions: [0, 0.55, 1],
};

export interface Ellipse { cx: number; cy: number; rx: number; ry: number }

/** The pool's ellipse over the drum box (x, y, w, h — the drum body, not the well). */
export function poolEllipse(x: number, y: number, w: number, h: number): Ellipse {
  return { cx: x + w * POOL.cx, cy: y + h * POOL.cy, rx: w * POOL.rx, ry: h * POOL.ry };
}

// ── §6.1 The well's edge ─────────────────────────────────────────────────────────
/** CSS `box-shadow: 0 0 8px` — the blur radius of the metal wells' outer glow. */
export const WELL_GLOW_BLUR = 8;

/**
 * ★★★ THE RING IS A LIGHT PIPE, NOT A LINE (Stuart, 2026-10-01: "the rings surrounding the button
 * controls — can you make them look like they are gently lit up like a real radio would be; right now
 * they look just like random rectangle boxes"). On every chassis the well's ring — the default's lit
 * border, the metal's ring outside its dark gap — gets the light a backlit bezel spills: a wide faint
 * SPILL and a tighter HALO either side of it (low alpha, soft falloff), and a hair of the LED's
 * white-hot core along the ring's OUTER edge, where light leaks from behind the panel. Warm, in the
 * controls colour; subtle, not neon. Alphas are of the LED colour.
 * ★★ PERF: rasterised ONCE per well size × colour (DrumWell.tsx useRingLight → glowSprite makeSprite)
 *   and blitted — no live blur, nothing animated; the drums' rolling canvas never touches it.
 * ★ Transparency effects OFF (chosen, or a low-end device's default): the flat ring, as before.
 */
export const RING_LIGHT = {
  // ★ Stronger since B9: with the hard outline gone (lit), the light alone has to mark the edge.
  spill: { width: 3,   blur: 10, a: 0.22 },
  halo:  { width: 1.5, blur: 4,  a: 0.42 },
  /** The hot hair on the ring's outer edge: hotA(led, a), this wide. Softer than a stroke — light, not a line. */
  edge:  { width: 0.6, a: 0.28 },
  /** How far the light reaches past the ring (pt) — the edge canvas must reach at least this far. */
  reach: 8,
} as const;

/**
 * How far past its own box the well draws (points): the metal ring and its 8 pt glow sit OUTSIDE
 * the face, as a box-shadow does; and the LIT ring's light (RING_LIGHT) spills past the default's
 * border too. 0 only for an unlit default well — its edge is its lit border, drawn inside, exactly
 * as before, so that well's canvas is its own size.
 * @param lit the ring is lit (RING_LIGHT) — Transparency effects on.
 */
export function wellOutset(t: Pick<ChassisTokens, 'wellRingA' | 'wellGlowA'>, lit = false): number {
  const own = t.wellGlowA > 0 ? WELL_GLOW_BLUR + 4 : t.wellRingA > 0 ? 2 : 0;
  return lit ? Math.max(own, RING_LIGHT.reach + 2) : own;
}

/** Where the ring itself runs, in the well's own coordinates (x, y, w, h, corner radius): the
 *  default's lit border half a point inside the box, the metal ring half a point outside it. */
export function ringRect(t: Pick<ChassisTokens, 'wellRingA'>, W: number, H: number, r: number) {
  return t.wellRingA > 0
    ? { x: -0.5, y: -0.5, w: W + 1, h: H + 1, r: r + 0.5 }
    : { x: 0.5, y: 0.5, w: W - 1, h: H - 1, r };
}

/** The well's corner radius (Deck.mockup `border-radius: 6px`, today's DrumWheel r = 6). */
export const WELL_R = 6;

// ── §6.1 TRAP: notch draw order ─────────────────────────────────────────────────
export type NotchLayer = 'pair' | 'minor' | 'med' | 'major';

/** The order the notch paths are drawn in: the pair (shadow on rubber, highlight on aluminium)
 *  under the notches, or over them. A chassis token, never a fixed order in the component. */
export function notchOrder(t: Pick<ChassisTokens, 'notchPairUnder'>): NotchLayer[] {
  return t.notchPairUnder ? ['pair', 'minor', 'med', 'major'] : ['minor', 'med', 'major', 'pair'];
}

// ── §6.2 Tuner-keys mode ─────────────────────────────────────────────────────────
/** Deck.mockup `tk`: the keys are 31 % of the well's inner width (34 % in landscape), full height,
 *  inside an 8 pt padding (6 in landscape). */
export const TK_KEY_FRAC  = 0.31;
export const TK_KEY_FRAC_LAND = 0.34;
export const TK_PAD       = 8;
export const TK_PAD_LAND  = 6;
/** The key slot's corner radius (`border-radius: 8px`). */
export const TK_SLOT_R    = 8;

export interface TunerKeysLayout {
  pad:    number;
  keyW:   number;
  keyH:   number;
  leftX:  number;
  rightX: number;
  /** The glyph between the keys: centred in the well. */
  glyphCx: number;
  glyphCy: number;
  /** The glyph's size — bounded by the gap between the keys, so it never touches a cap. */
  glyphSz: number;
}

/**
 * [key] [glyph] [key] inside the well. `r` scales the design points (useUiScale's r), so the padding
 * shrinks with the rest of the deck on a small screen. The keys share the space as the mockup's
 * flex row does: `width: 31%` of the content box, pushed to the edges.
 */
export function tunerKeysLayout(W: number, H: number, landscape: boolean,
                                r: (n: number) => number = n => n): TunerKeysLayout {
  const pad  = Math.max(2, r(landscape ? TK_PAD_LAND : TK_PAD));
  const inner = Math.max(0, W - pad * 2);
  const keyW = inner * (landscape ? TK_KEY_FRAC_LAND : TK_KEY_FRAC);
  const keyH = Math.max(0, H - pad * 2);
  const gap  = Math.max(0, inner - keyW * 2);
  // The mockup's icon is 24 pt; today's TunerKeys glyph was 42 % of the well. Take the smaller of
  // those and what fits between the keys with a little air.
  const glyphSz = Math.max(9, Math.min(Math.round(H * 0.42), r(24), gap - 6));
  return { pad, keyW, keyH, leftX: pad, rightX: W - pad - keyW, glyphCx: W / 2, glyphCy: H / 2, glyphSz };
}
