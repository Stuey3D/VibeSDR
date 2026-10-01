/**
 * keyLight.ts — the light that comes up out of the PANEL GAP around every front-panel key (faceplates
 * §5, §6.2). Pure: no React, no React Native, and Skia only as TYPES — the draw function takes the
 * Skia API as an argument, so scripts/test_faceplate_wells.ts checks the numbers and the headless
 * renders draw the very same light. components/KeyLight.tsx is the one place that turns it into a
 * sprite on screen.
 *
 * ★★★ Stuart, 2026-10-01: "try removing the outline ring that surrounds both buttons and replace it
 *   with a glow coming up in the panel gap around each button, also apply that same lighting to all
 *   the buttons please." So the well's ring is GONE (constants/drumWell.ts) and EACH KEY is lit on its
 *   own: as if it sits in a cut-out in the front panel with a lamp behind it, a thin band of warm
 *   light leaks round its edges — brightest right at the key's edge, in the gap, and falling off
 *   within a few points onto the panel. A touch stronger along the bottom edge (the lamp sits low).
 *   Not a neon outline, not a box: there is no stroke on the key itself, only light that dies away.
 * ★★ PERF (the B9 power pass stopped the deck redrawing every frame): rasterised ONCE per key size ×
 *   reach × colour × chassis into a sprite and blitted — no live blur, nothing animated, no frame
 *   callback; a press moves the cap over a light that never changes and fades in the ready-made
 *   pressed images (KEY_PRESS_LIGHT) on the press's own animation, so nothing is ever rebuilt.
 * ★ Transparency effects OFF (chosen, or a low-end device's default): NO light at all — the keys
 *   exactly as they were before the ring was lit.
 */

import type { SkCanvas, Skia as SkiaApi } from '@shopify/react-native-skia';

/** One layer of the light, in points at the design reach (KEY_LIGHT.reach). `o` is where the band's
 *  centre sits OUTSIDE the cut-out's edge, `w` its width, `blur` the CSS blur radius (sigma = blur/2),
 *  `a` the controls colour's alpha. */
export interface LightBand { o: number; w: number; blur: number; a: number }

/**
 * The light at its design reach. Alphas are of the controls colour (ledA); `edge` is the LED's warm
 * white-hot centre (hotA), where the lamp is seen most directly.
 *   • cap:    IN the gap, hugging the cap's edge — the light coming up from under the key, brightest
 *             against the cap and dying away across the slot (a metal key's 2 pt rim, the default tuner
 *             keys' dark slot). Measured from the CAP's edge and never scaled: the slot is the key's own
 *             geometry, not the deck's gap. ★ A flat even fill here read as a brown OUTLINE round every key
 *             (render 2026-10-01) — light has a direction; it comes from under the cap.
 *   • edge:   a narrow soft band ON the cut-out's edge — the light leaking past the panel's edge.
 *   • spill:  wider and fainter, outside the edge — the falloff onto the panel.
 *   • bottom: the spill again along the bottom edge only, nudged down — the lamp sits low. Subtle.
 */
export const KEY_LIGHT = {
  cap:    { o: 0.3,  w: 0.6, blur: 1.2, a: 0.95 },
  edge:   { o: 0.3,  w: 1.0, blur: 1.4, a: 0.24 },
  spill:  { o: 1.0,  w: 1.6, blur: 2.2, a: 0.20 },
  bottom: { o: 0.6,  w: 1.2, blur: 2.2, a: 0.10, dy: 0.6 },
  /** How far the light reaches past the cut-out's edge (pt) at full size — every layer's band plus
   *  its blur ends inside it (keyLightExtent), and the sprite is drawn this much bigger on each side. */
  reach: 4,
  /** The smallest reach worth drawing: under this the gap is too tight for light to read as light. */
  minReach: 1.5,
  /**
   * ★★ The share of the clear space to the nearest neighbour the light may take. Two neighbours' lights
   *   meet in the middle of the gap between them; at this share, the light where they meet stays well
   *   under the light at either key's edge (test_faceplate_wells: KEY_LIGHT_MEET), so the gap reads as
   *   two lit edges and never as one glowing bar. The tightest deck gap is the SE's 4 pt landscape row.
   */
  gapShare: 0.75,
} as const;

/**
 * ★★★ PRESSED: THE GAP OPENS AND THE LAMP FLOODS OVER THE KEY (Stuart, 2026-10-01: "when the button is
 * depressed the backlight glow needs to flood over the button like it would do in real life as the
 * panel gaps increase when the button is pushed into the body of the unit"). Two more images, made with
 * the rest light and never on a press, faded in by the key's OWN press progress (DomeKey's `dim`
 * style — the snap's withTiming, which settles and stops):
 *   • open: a brighter, wider band in the gap and on the panel's edge — more of the lamp is seen as the
 *           key sinks. Drawn with the rest light, under the cap. Ends inside `reach` like every layer.
 *   • wash: the lamp's light thrown up over the key's own face, strongest at its edges and fading to
 *           nothing towards the middle — a soft stroke round the cap's edge, clipped to the cap, drawn
 *           over the cap and under its legend, travelling with it.
 */
export const KEY_PRESS_LIGHT = {
  open: { o: 0.4, w: 1.4, blur: 1.8, a: 0.34 },
  /** `w` the band hugging the cap's inner edge and `blur` its softness — each a fraction of the cap's
   *  shorter side, capped, so a small key's middle stays dark too (the SE's 9 pt landscape keys). */
  wash: { w: 2.0, wFrac: 0.12, blurFrac: 0.30, blurMax: 9, a: 0.34 },
} as const;

/** The etched glyph between the tuner keys (TunerKeys useEtchGlow): `glow` is today's halo behind the
 *  strokes, exactly (α .55, 2.6 wide, blur 3); `bleed` the light escaping the etching onto the case —
 *  wider and gentle, the keys' lamp seen through a cut. `reach`: how far past a stroke either goes. */
export const ETCH_LIGHT = {
  glow:  { width: 2.6, blur: 3, a: 0.55 },
  bleed: { width: 3.2, blur: 6, a: 0.16 },
  reach: 1.6 + 6,
} as const;

/** The tightest gap the deck ever leaves between a control and its neighbour (the SE's 4 pt landscape row
 *  gap, faceplates §9) — for a key that is not told the deck's gaps (TunerKeys). test_faceplate_wells
 *  proves no layout goes under it. */
export const DECK_MIN_GAP = 4;

/** The reach (pt) for a key whose nearest neighbour is `space` pt away — or 0 when there is no room
 *  for light at all (the light is then not drawn). Never NaN: an unmeasured key gets 0. */
export function keyLightReach(space: number): number {
  if (!Number.isFinite(space) || space <= 0) return 0;
  const r = Math.min(KEY_LIGHT.reach, space * KEY_LIGHT.gapShare);
  return r >= KEY_LIGHT.minReach ? Math.round(r * 4) / 4 : 0;
}

const scaled = (b: LightBand, reach: number): LightBand => {
  const k = reach / KEY_LIGHT.reach;
  return { o: b.o * k, w: b.w * k, blur: b.blur * k, a: b.a };
};

/** The light's layers scaled to `reach` — a tighter gap gets the same light, smaller. */
export function keyLightLayers(reach: number) {
  const k = reach / KEY_LIGHT.reach;
  const s = (b: LightBand): LightBand => ({ o: b.o * k, w: b.w * k, blur: b.blur * k, a: b.a });
  return { cap: KEY_LIGHT.cap as LightBand, edge: s(KEY_LIGHT.edge), spill: s(KEY_LIGHT.spill),
           bottom: { ...s(KEY_LIGHT.bottom), dy: KEY_LIGHT.bottom.dy * k } };
}

/** How far past the cut-out's edge a band's light goes: its outer edge plus its blur RADIUS (2 sigma —
 *  past it, under 2.5 % of the band is left, and the sprite's edge cuts nothing anyone can see). */
export function bandExtent(b: LightBand, dy = 0): number {
  return b.o + b.w / 2 + b.blur + Math.abs(dy);
}
/** The furthest any layer reaches at `reach`. ≤ reach, or the sprite would clip the light. */
export function keyLightExtent(reach: number, pressed = false): number {
  if (pressed) return Math.max(keyLightExtent(reach), bandExtent(scaled(KEY_PRESS_LIGHT.open, reach)));
  const L = keyLightLayers(reach);
  return Math.max(bandExtent(L.edge), bandExtent(L.spill), bandExtent(L.bottom, L.bottom.dy));
}

// ── The 1-D profile: how bright the light is `d` pt out from the cut-out's edge ──────────
/** The normal CDF (Abramowitz & Stegun 7.1.26 erf — plenty for a light's falloff). */
function phi(x: number): number {
  const z = Math.abs(x) / Math.SQRT2, t = 1 / (1 + 0.3275911 * z);
  const y = 1 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * Math.exp(-z * z);
  return x >= 0 ? (1 + y) / 2 : (1 - y) / 2;
}
/** A band [o − w/2, o + w/2] blurred by sigma = blur/2, at distance d — what Skia's blur mask gives. */
function bandAt(b: LightBand, d: number): number {
  const sg = Math.max(1e-6, b.blur / 2), lo = b.o - b.w / 2, hi = b.o + b.w / 2;
  return b.a * (phi((d - lo) / sg) - phi((d - hi) / sg));
}
/**
 * The light's alpha (0..1, alpha-composited) at `d` pt outside the cut-out's edge, on the sides and top
 * (d < 0: inside the gap). `gap`: how far inside the cut-out the cap's edge is (0 = no slot — the outline
 * key — and then there is no cap light, the edge and spill being the whole of it).
 */
export function keyLightAt(reach: number, d: number, gap = 0): number {
  const L = keyLightLayers(reach);
  const parts = [gap > 0.5 ? bandAt(L.cap, d + gap) : 0, bandAt(L.edge, d), bandAt(L.spill, d)];
  return 1 - parts.reduce((m, v) => m * (1 - Math.max(0, v)), 1);
}

// ── The geometry ───────────────────────────────────────────────────────────────────
export interface RR { x: number; y: number; w: number; h: number; r: number }

/**
 * Where the light is drawn, in the KEY's own coordinates (0,0 = the key's top-left):
 *   cut — the panel's cut-out the key sits in (its slot, or the key itself when it has none);
 *   cap — the cap, the one part the light must never paint on (it is lit from behind, not over).
 * And the sprite: the cut-out plus `reach` on every side, placed at (−reach, −reach).
 */
export function keyLightSprite(cut: RR, reach: number): { x: number; y: number; w: number; h: number } {
  return { x: cut.x - reach, y: cut.y - reach, w: cut.w + 2 * reach, h: cut.h + 2 * reach };
}

/** The cap of a key at rest inside its slot: DomeKey's cap (inset 2 at the sides, 1.5 at the top, the
 *  slot less 4 pt tall), or the key itself when it is the outline key (no slot: cut = cap). */
export function domeCap(w: number, h: number, radius: number, outline: boolean): RR {
  if (outline) return { x: 0, y: 0, w, h, r: radius };
  return { x: 2, y: 1.5, w: Math.max(0, w - 4), h: Math.max(8, h - 4), r: Math.max(2, radius - 2) };
}

// ── The draw ─────────────────────────────────────────────────────────────────────────
type Sk = typeof SkiaApi;

/**
 * Draws the light into `c`, in SPRITE coordinates: the sprite's top-left is keyLightSprite(cut, reach)'s,
 * so the cut-out lands at (reach, reach) whatever its own x, y (cut and cap are given in the key's
 * coordinates, as everywhere else). `led`/`hot` are the
 * controls colour at alpha 1 as `(a) => colour string` (ledA / hotA); `Sk` is the Skia API (the app's,
 * or the headless one in the renders). The cap is clipped OUT first: nothing here ever lands on a key.
 */
export function drawKeyLight(Sk: Sk, c: SkCanvas, cut: RR, cap: RR, reach: number,
                             led: (a: number) => string, hot: (a: number) => string, pressed = false,
                             /** Pressed only: how far the cap has SUNK (DomeKey's DOME_TRAVEL on a cap key; 0 on
                              *  the outline key, whose face stays put). The opened light hugs the cap where it
                              *  now is — so it floods out of the WIDER gap the press opens above it. */
                             travel = 0): void {
  const L = keyLightLayers(reach);
  const ox = reach - cut.x, oy = reach - cut.y;
  const at = (b: RR, d: number, dy = 0) => Sk.RRectXY(
    Sk.XYWHRect(ox + b.x - d, oy + b.y - d + dy, b.w + 2 * d, b.h + 2 * d), Math.max(0, b.r + d), Math.max(0, b.r + d));
  const paint = (col: string, blur: number, stroke?: number) => {
    const p = Sk.Paint();
    p.setAntiAlias(true);
    p.setColor(Sk.Color(col));
    // BlurStyle.Normal = 0; respectCTM so the blur is in points at any pixel ratio.
    if (blur > 0) p.setMaskFilter(Sk.MaskFilter.MakeBlur(0 as any, blur / 2, true));
    if (stroke != null) { p.setStyle(1 as any); p.setStrokeWidth(stroke); }
    return p;
  };
  if (pressed) {
    // ★ The OPENED gap only — its own image, faded in OVER the rest light as the key goes down. Never on
    //   the SUNK cap (the press geometry is the snap's; this only follows it).
    const sunk = { ...cap, y: cap.y + travel };
    const o = scaled(KEY_PRESS_LIGHT.open, reach);
    c.save();
    c.clipRRect(at(sunk, 0), 0 as any, true);
    if (cut.w - cap.w > 0.5 || cut.h - cap.h > 0.5) {
      // The slot round the sunk cap, lit through: brightest against the cap, as at rest but more of it.
      c.drawRRect(at(sunk, L.cap.o), paint(led(L.cap.a), L.cap.blur * 1.4, L.cap.w * 2));
    }
    c.drawRRect(at(cut, o.o), paint(hot(o.a), o.blur, o.w));
    c.restore();
    return;
  }
  c.save();
  // ★ Never on the cap. ClipOp.Difference = 0.
  c.clipRRect(at(cap, 0), 0 as any, true);
  // In the gap, hugging the cap (only where the cut-out is bigger than the cap: a key in a slot).
  if (cut.w - cap.w > 0.5 || cut.h - cap.h > 0.5) c.drawRRect(at(cap, L.cap.o), paint(led(L.cap.a), L.cap.blur, L.cap.w));
  // The falloff onto the panel, then the bottom's extra, then the warm edge on top.
  c.drawRRect(at(cut, L.spill.o), paint(led(L.spill.a), L.spill.blur, L.spill.w));
  c.save();
  // ClipOp.Intersect = 1: the lower half of the sprite only.
  c.clipRect(Sk.XYWHRect(0, reach + cut.h / 2, cut.w + 2 * reach, cut.h / 2 + reach), 1 as any, true);
  c.drawRRect(at(cut, L.bottom.o, L.bottom.dy), paint(led(L.bottom.a), L.bottom.blur, L.bottom.w));
  c.restore();
  c.drawRRect(at(cut, L.edge.o), paint(hot(L.edge.a), L.edge.blur, L.edge.w));
  c.restore();
}

/**
 * The pressed WASH over the cap, in the CAP's own coordinates (0,0 = its top-left; the image is the cap's
 * size). A soft band round the inside of its edge, clipped to it: the edges catch the lamp, the middle
 * stays the key.
 */
export function drawKeyWash(Sk: Sk, c: SkCanvas, cap: RR, led: (a: number) => string): void {
  const W = KEY_PRESS_LIGHT.wash;
  const m = Math.min(cap.w, cap.h);
  const blur = Math.min(W.blurMax, m * W.blurFrac), band = Math.min(W.w, m * W.wFrac);
  const rr = Sk.RRectXY(Sk.XYWHRect(0, 0, cap.w, cap.h), cap.r, cap.r);
  const p = Sk.Paint();
  p.setAntiAlias(true);
  p.setColor(Sk.Color(led(W.a)));
  if (blur > 0) p.setMaskFilter(Sk.MaskFilter.MakeBlur(0 as any, blur / 2, true));
  p.setStyle(1 as any); p.setStrokeWidth(band * 2);   // centred on the edge: half of it lies inside
  c.save();
  c.clipRRect(rr, 1 as any, true);
  c.drawRRect(rr, p);
  c.restore();
}
