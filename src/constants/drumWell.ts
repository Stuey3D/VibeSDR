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
/*
 * ★★★ THE RING IS GONE (Stuart, 2026-10-01: "try removing the outline ring that surrounds both buttons
 * and replace it with a glow coming up in the panel gap around each button, also apply that same
 * lighting to all the buttons please"). It was a light pipe for one afternoon (RING_LIGHT, 40f9b077 /
 * c003d0ea) and before that a flat ring + glow — both read as a box round the GROUP. Now no well has a
 * ring or a glow: the tuner keys are lit one by one (constants/keyLight.ts, DomeKey). The DRUM keeps
 * only the metal's dark cut (DrumWell.tsx WellEdge): its seams are already backlit from behind
 * (DrumWheel), and light round the whole well box would be the box itself again.
 */

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
  /** The keys' top edge — `pad`, or below the label row when there is one. */
  keyY:   number;
  /** ★ DAB: the label row ABOVE the keys (0 tall when there is none). */
  labelY: number;
  labelH: number;
}

/** ★★ DAB (2026-10-05): the multiplex label's row above the keys — a share of the well, within these
 *  bounds, and the air above it and between it and the keys' tops (1 pt: every point the row does not
 *  take is the keys' — a 40 pt landscape band keeps 24 of its 32 pt keys, a 60 pt portrait well 36 of 44). */
export const TK_LABEL_FRAC = 0.24;
export const TK_LABEL_MIN  = 10;
export const TK_LABEL_MAX  = 15;
export const TK_LABEL_AIR  = 1;

/**
 * [key] [glyph] [key] inside the well. `r` scales the design points (useUiScale's r), so the padding
 * shrinks with the rest of the deck on a small screen. The keys share the space as the mockup's
 * flex row does: `width: 31%` of the content box, pushed to the edges.
 *
 * ★★ `label` (DAB, 2026-10-05): a row ABOVE the keys for the multiplex ("MUX 12B"). Stuart asked for the
 *  multiplex "above the < >" (as the web client's multiplex bar sits above its controls); it had been
 *  squeezed BETWEEN them because the keys filled the well. The row takes the top padding and a share of
 *  the height (TK_LABEL_*), and the keys give up that much height — the well itself never grows, so
 *  the zoom control beside it and every row of the deck stay exactly where they are.
 */
export function tunerKeysLayout(W: number, H: number, landscape: boolean,
                                r: (n: number) => number = n => n, label = false): TunerKeysLayout {
  const pad  = Math.max(2, r(landscape ? TK_PAD_LAND : TK_PAD));
  const inner = Math.max(0, W - pad * 2);
  const keyW = inner * (landscape ? TK_KEY_FRAC_LAND : TK_KEY_FRAC);
  const gap  = Math.max(0, inner - keyW * 2);
  // The label row starts a little inside the top edge (the padding is where it gets its room from).
  const labelY = label ? Math.min(pad, TK_LABEL_AIR) : 0;
  const labelH = label ? Math.round(Math.max(TK_LABEL_MIN, Math.min(TK_LABEL_MAX, H * TK_LABEL_FRAC))) : 0;
  const keyY = label ? labelY + labelH + TK_LABEL_AIR : pad;
  const keyH = Math.max(0, H - pad - keyY);
  // The mockup's icon is 24 pt; today's TunerKeys glyph was 42 % of the well. Take the smaller of
  // those and what fits between the keys with a little air (and, with a label row, the keys' height).
  const glyphSz = Math.max(9, Math.min(Math.round(H * 0.42), r(24), gap - 6, label ? keyH * 0.6 : Infinity));
  return { pad, keyW, keyH, leftX: pad, rightX: W - pad - keyW, glyphCx: W / 2,
           glyphCy: keyY + keyH / 2, glyphSz, keyY, labelY, labelH };
}
