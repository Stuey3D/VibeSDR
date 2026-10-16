/**
 * nixie.ts — the Nixie frequency window's RULES and GEOMETRY (faceplates brief §7, Deck.mockup
 * `nixie` / `tb` / `narrow()`), pure so scripts/test_faceplate_nixie.ts can hold every rule down.
 *
 * ★★★ FIXED TUBE LAYOUT — real hardware never adds or removes tubes:
 *     [MHz tubes] (bulb A) [3 kHz tubes] (bulb B) [3 Hz tubes]
 *       kHz → 14 230•000   bulb B lit        MHz → 14•230 000   bulb A lit
 *       Hz  → 14 230 000   both bulbs OFF (the gap is intentional — Stuart)
 *   Units only change WHICH BULB IS LIT; no digit ever moves. The MHz-tube count belongs to the
 *   connected radio (network HF 2 → 8 tubes; a local radio to 2 GHz 4 → 10 tubes at 8/10 width), never
 *   to the current frequency. The FM tuner screen is its own fixed layout: 3 tubes + bulb + 3, MHz.
 * ★★★ THE TUBE SHRINKS, THE WINDOW DOESN'T. pip + glass + collar + 4 pt of clearance fill the window;
 *   the glass is capped at its design height and shrinks when the window does. The same in width: the
 *   group narrows to fit, it never pushes the window wider.
 * ★★★ THE TUBES STAND ON THE LINE BELOW THEM (Stuart, 2026-10-01: "the tubes need to be anchored to
 *   the line underneath them so they look attached to the radio"). The collar's foot IS the window's
 *   floor — the lip line drawn under the window. The brief's 2 pt of clearance under the collar read as
 *   a gap on a 17 Pro Max (the dark collar over a dark floor made it look like more), so the whole
 *   clearance budget now sits ABOVE the pip. Bottom-aligned, not resized: the glass is the size it was.
 * ★ The decimal point is its OWN tube (an INS-1 bulb), never a dot inside a digit tube.
 */

// ★ Types only: this module stays loadable by node (scripts/test_faceplate_nixie.ts) with no React Native.
import type { Skia, SkCanvas, SkFont, SkPath } from '@shopify/react-native-skia';

/**
 * ★★★ THE IN-14 CATHODE STACK, FRONT to back: 6 5 4 7 3 9 8 2 0 1 — MEASURED off Stuart's 4K close-up of real
 *   IN-14s (IMG_1559, 2026-10-16: tube 1 steps through all ten digits at 9.7-12.7 s). No published depth
 *   order is trustworthy (datasheets give only the pinout), so this is what the video shows.
 *   Two independent cues, every digit in the SAME tube, same exposure:
 *    · size (perspective — a deeper cathode is further from the lens): lit-stroke height in px of the
 *      full-height digits 6 338 · 5 334 · 7 327 · 3 326 · 9 323 · 8 322 · 2 315 · 1 289 · 0 288
 *      (the 4 runs below the line by design, so its height does not compare);
 *    · occlusion — dark wire silhouettes across the lit stroke, over and above the hex mesh:
 *      6 5 4 none · 7 one · 3 9 two-three · 8 three · 2 0 1 four or more.
 *   Confidence: 6 5 front and 0 1 back HIGH; 4 7 3 9 8 2 medium; within the pairs 6/5, 3/9, 8/2, 0/1 low.
 *   (Superseded: the "3 8 9 4 0 5 7 2 6 1" an AI summary offered, its reverse, and a read of the
 *   low-resolution IMG_0043 — "6 4 7 9 1 0 3 8 5 2".)
 */
export const CATHODE_STACK = ['6', '5', '4', '7', '3', '9', '8', '2', '0', '1'] as const;

export interface CathodeDepth { scale: number; dy: number; opacity: number; soft: number }

/**
 * Stack index i (0 = front) → a slightly smaller, lower, dimmer and softer digit:
 * `scale 1 − 0.010·i`, `dy 0.15·i`, `opacity 1 − 0.015·i`, glow radius × `1 + 0.05·i`.
 * ★ The back digit is ~9 % smaller and a touch softer (the 4K video measures ~15 % at macro range;
 *   the dimming and scale are held where Stuart last saw them until he decides). (The pre-2026-10-16 0.028 per step made the back digit 25 % smaller.)
 */
export function cathodeDepth(d: string): CathodeDepth {
  const i = Math.max(0, CATHODE_STACK.indexOf(d as any));
  return { scale: 1 - 0.010 * i, dy: 0.15 * i, opacity: 1 - 0.015 * i, soft: 1 + 0.05 * i };
}

/** The unlit cathodes around a lit digit: EVERY one behind it (faint wire, lit warm by its glow) and
 *  EVERY one in front of it (dark wire crossing the glow), in stack order. A switched-off tube shows
 *  the whole stack as bare metal. */
export function cathodeNeighbours(d: string | null): { back: string[]; front: string[] } {
  if (d === null) return { back: [...CATHODE_STACK], front: [] };
  const i = CATHODE_STACK.indexOf(d as any);
  return { back: CATHODE_STACK.slice(i + 1) as string[], front: CATHODE_STACK.slice(0, i) as string[] };
}

/** The glyph a digit's cathode is cut as. ★ On the IN-14 the 5 is the 2's cathode turned upside down
 *  and back to front (rotated 180°) — so the frequency tubes draw a '2' rotated 180°. Only here: any
 *  other Nixie-font text keeps the font's own 5. */
export function cathodeGlyph(d: string): { glyph: string; flipY: boolean; bar: boolean } {
  if (d === '5') return { glyph: '2', flipY: true, bar: false };
  // ★ The IN-14's 1 is a plain straight wire — no flag, no foot (the font's 1 has both).
  if (d === '1') return { glyph: '0', flipY: false, bar: true };
  return { glyph: d, flipY: false, bar: false };
}

// ── The tube's inside, drawn ONCE per size into a sprite (NixieTubes) ──────────
// Pure apart from the Skia API passed in, so scripts can render it headless (canvaskit) to compare
// against the reference video.

/** The anode box: the rectangle of hex mesh the cathodes stand in, in a tw × th glass (y from the
 *  glass top, which includes the dome). */
export function meshBox(tw: number, th: number) {
  return { x: tw * 0.12, y: th * 0.24, w: tw * 0.76, h: th * 0.64 };
}
/** Where a digit's centre sits in the glass (the anode box's middle). */
export function digitCentre(tw: number, th: number) {
  const b = meshBox(tw, th);
  return { cx: tw / 2, cy: b.y + b.h / 2 };
}

type SkiaApi = typeof Skia;

type Corner = [number, number];
/** A box with elliptical per-corner radii — CSS `border-radius: a b c d / e f g h`. */
export function cornerPath(Sk: SkiaApi, x: number, y: number, w: number, h: number,
                           tl: Corner, tr: Corner, br: Corner, bl: Corner): SkPath {
  const p = Sk.Path.Make();
  const R = (cx: number, cy: number, rx: number, ry: number) => Sk.XYWHRect(cx, cy, 2 * rx, 2 * ry);
  p.moveTo(x, y + tl[1]);
  p.arcToOval(R(x, y, tl[0], tl[1]), 180, 90, false);
  p.lineTo(x + w - tr[0], y);
  p.arcToOval(R(x + w - 2 * tr[0], y, tr[0], tr[1]), 270, 90, false);
  p.lineTo(x + w, y + h - br[1]);
  p.arcToOval(R(x + w - 2 * br[0], y + h - 2 * br[1], br[0], br[1]), 0, 90, false);
  p.lineTo(x + bl[0], y + h);
  p.arcToOval(R(x, y + h - 2 * bl[1], bl[0], bl[1]), 90, 90, false);
  p.close();
  return p;
}
/** The digit tube's glass: `48% 48% 3 3 / 26% 26% 3 3`. */
export const tubeGlassPath = (Sk: SkiaApi, x: number, y: number, w: number, h: number) =>
  cornerPath(Sk, x, y, w, h, [0.48 * w, 0.26 * h], [0.48 * w, 0.26 * h], [3, 3], [3, 3]);

/** The anode's honeycomb (flat-topped hexagons, edge `r`) filling the anode box, offset by (ox, oy).
 *  One path of line segments — stroked thin, clipped to meshBox by the caller. */
export function honeycombPath(Sk: SkiaApi, tw: number, th: number, ox = 0, oy = 0): SkPath {
  const b = meshBox(tw, th);
  const r = Math.max(0.9, tw * 0.05);
  const dx = 1.5 * r, dy = Math.sqrt(3) * r;
  const p = Sk.Path.Make();
  for (let col = 0, x = b.x; x <= b.x + b.w + r; col++, x += dx) {
    for (let y = b.y + (col % 2 ? dy / 2 : 0); y <= b.y + b.h + dy; y += dy) {
      // three edges per cell (left, upper-left, lower-left) tile the plane without repeats
      p.moveTo(ox + x - r, oy + y);
      p.lineTo(ox + x - r / 2, oy + y - dy / 2);
      p.lineTo(ox + x + r / 2, oy + y - dy / 2);
      p.moveTo(ox + x - r, oy + y);
      p.lineTo(ox + x - r / 2, oy + y + dy / 2);
    }
  }
  return p;   // ★ overruns the box by a cell: the caller CLIPS to meshBox (a path op on open lines is empty)
}

/**
 * ★★ THE NEON GLOW (Stuart, 2026-10-16: "a lovely neon gas glow around them", the reference video).
 * The gas glows in a sheath round the wire: brightest at the wire, falling off over a few stroke widths,
 * spilling warm light on the mesh and the wires next to it. Each layer is the cathode shape FATTENED
 * (a thin Nixie One stroke blurred on its own gives almost no halo — that was the old look) and then
 * blurred, outermost first. `sigma` is a fraction of the digit size, `fat` is in stroke widths.
 */
export interface NeonLayer { sigma: number; fat: number; color: string; alpha: number }
export const DIGIT_NEON: NeonLayer[] = [
  // ★ Colours sampled from the 4K video (IMG_1559) off the stroke, never from its clipped yellow core:
  //   sheath #f59306, halo #d65409, lit window #be4608. Deep neon orange, not yellow-white.
  { sigma: 0.17,  fat: 4,   color: 'rgb(200,62,6)',   alpha: 0.65 },   // wide bloom, fills the window
  { sigma: 0.07,  fat: 3,   color: 'rgb(214,84,9)',   alpha: 0.75 },   // the gas sheath
  { sigma: 0.026, fat: 2,   color: 'rgb(240,110,8)',  alpha: 0.95 },    // hugging the wire
  { sigma: 0.008, fat: 1.1, color: 'rgb(250,145,14)', alpha: 1 },      // hot rim
];
/** The wire itself: hot orange-amber (the camera clips it to yellow; the eye sees amber). */
export const DIGIT_CORE = '#ffaa2c';
/** The core is drawn this much fatter than Nixie One's stroke (in stroke widths): the lit wire reads
 *  about twice the font's weight in the 4K video. */
export const CORE_FAT = 0.6;
/** Unlit cathodes: bare metal in a switched-off tube; lit warm from the side by a lit digit behind
 *  the ones in front; a dark wire where one crosses the glow. */
const WIRE_OFF       = 'rgba(160,140,120,0.13)';
const WIRE_WARM      = 'rgba(255,150,90,0.12)';
const WIRE_FRONT     = 'rgba(70,26,6,0.36)';
/** The anode mesh in front of the stack: it shadows the glow as fine hex lines, and is lit warm itself. */
/** Nixie One's stroke weight, as a fraction of the font size (measured at 1000 px: 29-30 px — the 0's
 *  ring, the 2's base and tick). The 1's plain wire is drawn this wide. */
export const STEM = 0.03;
/**
 * ★ THE 2's BASE IS A STRAIGHT WIRE (Stuart, 2026-10-16, his photo): Nixie One ends the 2's flat base
 * with a 90° upturn — a second short stroke the IN-14 does not have. Measured in Nixie One at 1000 px
 * from the text origin: base y −30…−1 (centre −15.5), tick x 500…529 rising to y −170, base from x 61.
 * The tube cuts everything right of x 0.485 below y −0.25 and caps the base there with a round end of
 * the stroke's own width. The 5 is the flipped 2, so its flat top loses the matching down-tick.
 * Only the frequency tubes draw this; the font is untouched everywhere else.
 */
export const TWO_TRIM = { cutX: 0.485, cutTop: -0.25, capY: -0.0155 };
const MESH_SHADOW    = 'rgba(40,14,4,0.17)';
const MESH_LIT       = 'rgba(230,100,30,0.20)';
const MESH_WASH      = 'rgba(190,70,8,0.28)';

/**
 * Draw ONE tube's inside — the whole cathode stack with `lit` glowing at its own depth (null = a
 * switched-off tube), the anode mesh over it — into `c`, a tw × th canvas whose origin is the glass's
 * top-left. Clipped to the glass.
 * ★★ PERF: this runs once per digit per tube SIZE (11 sprites), never per tune step: blurred text,
 *   up to ten glyph draws and the mesh are all rasterised here, and a tune step blits one image.
 */
export function drawTubeInside(Sk: SkiaApi, c: SkCanvas, font: SkFont, lit: string | null,
                               tw: number, th: number, nf: number): void {
  const paint = (color: string, opts: { stroke?: number; blend?: number } = {}) => {
    const p = Sk.Paint();
    p.setAntiAlias(true);
    p.setColor(Sk.Color(color));
    if (opts.stroke) { p.setStyle(1 /* PaintStyle.Stroke */); p.setStrokeWidth(opts.stroke); }
    if (opts.blend !== undefined) p.setBlendMode(opts.blend);
    return p;
  };
  const { cx, cy } = digitCentre(tw, th);
  const stem = nf * STEM;
  /** The cathode's shape in an OPAQUE colour, fattened by `fat` pt — the 1 as a plain wire, the 5 as the
   *  flipped 2, the 2's base without the font's upturn. */
  const shape = (d: string, color: string, fat: number) => {
    const g = cathodeGlyph(d);
    const b = font.measureText(g.glyph);
    const p = paint(color);
    if (g.bar) {                                          // the 1: a wire the height of a 0
      const w = stem + fat, len = b.height + w;
      c.drawRRect(Sk.RRectXY(Sk.XYWHRect(cx - w / 2, cy - len / 2, w, len), w / 2, w / 2), p);
      return;
    }
    const x = cx - (b.x + b.width / 2), y = cy - (b.y + b.height / 2);
    const trim = g.glyph === '2';
    if (g.flipY) { c.save(); c.translate(0, 2 * cy); c.scale(1, -1); }
    if (trim) {
      c.save();
      c.clipRect(Sk.XYWHRect(x + TWO_TRIM.cutX * nf, y + TWO_TRIM.cutTop * nf, 0.3 * nf, -TWO_TRIM.cutTop * nf + 0.1 * nf),
        0 /* ClipOp.Difference */, true);
    }
    c.drawText(g.glyph, x, y, p, font);
    if (fat > 0) {
      const ps = paint(color, { stroke: fat });
      ps.setStrokeJoin(1 /* StrokeJoin.Round */);
      c.drawText(g.glyph, x, y, ps, font);
    }
    if (trim) {
      c.restore();
      c.drawCircle(x + TWO_TRIM.cutX * nf, y + TWO_TRIM.capY * nf, (stem + fat) / 2, p);
    }
    if (g.flipY) c.restore();
  };
  /** Draw `draw` as one layer at `alpha`, blurred by `sigma` pt — so a shape's overlapping parts (the
   *  trimmed 2's cap, a fattened outline) never double up, and the blur is the layer's, not cut by the
   *  trim's clip. */
  const layer = (alpha: number, sigma: number, draw: () => void) => {
    const lp = Sk.Paint();
    lp.setAlphaf(alpha);
    if (sigma > 0) lp.setImageFilter(Sk.ImageFilter.MakeBlur(sigma, sigma, 3 /* TileMode.Decal */, null));
    c.saveLayer(lp);
    draw();
    c.restore();
  };
  const rgbA = (rgba: string): [string, number] => {
    const m = /rgba\((\d+),(\d+),(\d+),([\d.]+)\)/.exec(rgba.replace(/\s/g, ''));
    return m ? [`rgb(${m[1]},${m[2]},${m[3]})`, +m[4]] : [rgba, 1];
  };
  const atDepth = (d: string, draw: (z: CathodeDepth) => void) => {
    const z = cathodeDepth(d);
    c.save();
    c.translate(cx, cy + z.dy);
    c.scale(z.scale, z.scale);
    c.translate(-cx, -cy);
    draw(z);
    c.restore();
  };

  c.save();
  c.clipPath(tubeGlassPath(Sk, 0, 0, tw, th), 1 /* ClipOp.Intersect */, true);
  const li = lit === null ? CATHODE_STACK.length : CATHODE_STACK.indexOf(lit as any);
  /* Back to front: the cathodes behind, the lit one, the cathodes in front — each covering what is
   * behind it, the glow included. ★ The unlit wires are drawn as ONE layer per side, opaque inside it,
   * the layer at the wire's tint: where two wires overlap (the 3, 8 and 0 share curves) they do not
   * stack up into a solid dark band over the digit — a wire is a wire, however many lie on that line. */
  const [backCol, backA] = rgbA(lit === null ? WIRE_OFF : WIRE_WARM);
  layer(backA, 0, () => {
    for (let i = CATHODE_STACK.length - 1; i > li; i--) {
      const d = CATHODE_STACK[i];
      atDepth(d, (z) => layer(z.opacity, 0, () => shape(d, backCol, 0)));
    }
  });
  if (lit !== null) {
    atDepth(lit, (z) => {
      for (const g of DIGIT_NEON) layer(g.alpha * z.opacity, g.sigma * nf * z.soft, () => shape(lit, g.color, g.fat * stem));
      layer(z.opacity, 0, () => shape(lit, DIGIT_CORE, CORE_FAT * stem));
    });
    const [frontCol, frontA] = rgbA(WIRE_FRONT);
    layer(frontA, 0, () => {
      for (let i = li - 1; i >= 0; i--) {
        const d = CATHODE_STACK[i];
        atDepth(d, () => shape(d, frontCol, 0));
      }
    });
  }
  if (lit !== null) {
    const b = meshBox(tw, th);
    const box = Sk.XYWHRect(b.x, b.y, b.w, b.h);
    const mesh = honeycombPath(Sk, tw, th);
    c.save();
    c.clipRect(box, 1 /* ClipOp.Intersect */, true);
    const meshW = Math.max(0.3, tw * 0.018);
    // The mesh's shadow lands only on what is drawn (glow + wires): SrcATop.
    c.drawPath(mesh, paint(MESH_SHADOW, { stroke: meshW, blend: 9 /* BlendMode.SrcATop */ }));
    // Then BEHIND everything drawn (DstOver): the mesh lit by the digit, and its warm wash.
    c.drawPath(mesh, paint(MESH_LIT, { stroke: meshW, blend: 4 /* BlendMode.DstOver */ }));
    c.drawRect(box, paint(MESH_WASH, { blend: 4 }));
    c.restore();
  }
  c.restore();
}

// ── Layouts ──────────────────────────────────────────────────────────────────

/**
 * `hf`   network radios (MAX_HZ 30 MHz): 2 MHz tubes → 8 digit tubes + 2 bulbs.
 * `wide` a local radio to 2 GHz: 4 MHz tubes → 10 digit tubes (narrowed to 8/10, digits 0.9×).
 * `fm`   the FM tuner screen: 3 tubes + bulb + 3, in MHz (` 96•600`), the bulb always lit.
 */
export type NixieLayout = 'hf' | 'wide' | 'fm';

export interface NixieSpec {
  mhzTubes:  number;
  fracTubes: number;
  /** Bulb positions as the index of the digit AFTER each bulb. */
  bulbAt:    number[];
}

export function nixieSpec(layout: NixieLayout, mhzTubes?: number): NixieSpec {
  if (layout === 'fm') return { mhzTubes: 3, fracTubes: 3, bulbAt: [3] };
  const m = mhzTubes ?? (layout === 'wide' ? 4 : 2);
  return { mhzTubes: m, fracTubes: 6, bulbAt: [m, m + 3] };
}

/** How many MHz tubes a frequency needs (≥ 1). */
export function mhzDigitsFor(hz: number): number {
  return Math.max(1, String(Math.floor(Math.max(0, hz) / 1e6)).length);
}

export type NixieUnit = 'hz' | 'khz' | 'mhz';

export interface NixieReadout {
  /** One entry per digit tube: the digit it lights, or null for a tube that is switched OFF. */
  tubes: (string | null)[];
  /** Lit state per bulb, in bulbAt order. */
  bulbs: boolean[];
}

/**
 * The readout for `hz` on a fixed layout. Leading zeros are SWITCHED OFF (bare cathodes, no glow),
 * not zero-filled — except the digit just left of a lit point, which always lights (0•648 000 MHz).
 * ★ A frequency the layout cannot hold keeps its LOW digits (the caller widens the layout instead;
 *   see NixieTubes' latch) — it never shifts the digits sideways.
 */
export function nixieReadout(hz: number, spec: NixieSpec, unit: NixieUnit): NixieReadout {
  const n = spec.mhzTubes + spec.fracTubes;
  // Resolution of the last tube: 1 Hz for the 6-fraction layouts, 1 kHz for the FM screen's 3.
  const res = spec.fracTubes === 6 ? 1 : 1000;
  const v = Math.round(Math.max(0, hz) / res);
  const digits = String(v).padStart(n, '0').slice(-n);
  let litAt = -1;
  if (spec.bulbAt.length === 1) litAt = spec.bulbAt[0];              // fm: always MHz
  else if (unit === 'mhz') litAt = spec.bulbAt[0];
  else if (unit === 'khz') litAt = spec.bulbAt[1];
  const firstSig = digits.search(/[1-9]/);
  const keepFrom = Math.min(firstSig < 0 ? n - 1 : firstSig, litAt > 0 ? litAt - 1 : n - 1);
  const tubes = [...digits].map((d, k) => (k < keepFrom ? null : d));
  return { tubes, bulbs: spec.bulbAt.map((at) => at === litAt) };
}

// ── Geometry ─────────────────────────────────────────────────────────────────

/** A tube's DESIGN size at scale 1 (Deck.mockup fb / fv tables): tube width, glass height, digit px. */
export interface TubeDesign { tw: number; th: number; nf: number }

/** Deck.mockup: bar `fb` (shared: th 34, nf 28), VU/analogue `fv0` / shared, landscape `fbL` / `fvL`. */
export const TUBE_DESIGN = {
  bar:           { tw: 21, th: 40, nf: 32 },
  barShared:     { tw: 21, th: 34, nf: 28 },
  barLand:       { tw: 19, th: 32, nf: 26 },
  meter:         { tw: 22, th: 36, nf: 30 },
  meterShared:   { tw: 18, th: 29, nf: 26 },
  meterLand:     { tw: 15, th: 27, nf: 23 },
} as const;

/** Stack pieces (pt at scale 1): tip-off pip, socket collar, and the clearance — 2 × CLEAR, all of it
 *  above the pip (the collar stands on the floor, see the ★★★ above). */
export const PIP_H = 2;
export const COLLAR_H = 4;
export const CLEAR = 2;
/** Floor for the glass: below this the dome cannot be drawn at all. The stack then CLIPS rather than
 *  lie — only reachable in a window under 18 pt, which no layout has. */
export const MIN_GLASS = 8;

export interface TubeBox { x: number; w: number }
export interface NixieGeometry {
  tubes:     TubeBox[];
  /** The INS-1 bulbs: `w` is the glass; the collar is w + 2 and the slot 1 pt more each side. */
  bulbs:     TubeBox[];
  /** Glass height of a digit tube and of a bulb. */
  glassH:    number;
  bulbH:     number;
  /** Digit size (px) for the cathodes. */
  nf:        number;
  gap:       number;
  /** y of the collar's top edge (the socket floor line) — everything stands on it. The collar's foot
   *  (collarY + COLLAR_H) is the window's bottom edge: the tubes stand on the line under the window. */
  collarY:   number;
  /** ★★ How far the anchoring moved the row DOWN from the brief's layout (the 2 pt that used to sit
   *  under the collar). Everything that belongs to the tubes but is not drawn from their boxes — the
   *  neon spill on the back wall behind them — moves by this too, so the row still reads as set INTO
   *  the panel, not standing in front of a glow left behind (Stuart, 2026-10-01). */
  drop:      number;
  /** The natural (unshrunk) group width, for the window to ASK for. */
  naturalW:  number;
  /** 0..1 — how much the group was narrowed to fit. */
  fit:       number;
}

/**
 * Lay the fixed row out in a w × h window.
 * @param bar   the bar-meter window (§7 TRAP: narrower, 16 pt tubes, 1 pt gaps).
 * @param scale the UI scale (s.r) applied to every design number.
 * ★ The group is CENTRED IN THE TUBE WINDOW, which excludes the unit label — so kHz / MHz / Hz can
 *   never move a tube (§7 TRAP: anchor the group, do not centre it with the label).
 */
export function nixieGeometry(w: number, h: number, spec: NixieSpec, design: TubeDesign,
                              opts: { bar: boolean; scale?: number }): NixieGeometry {
  const sc = opts.scale ?? 1;
  const n = spec.mhzTubes + spec.fracTubes;
  const nb = spec.bulbAt.length;
  const k10 = Math.min(1, 8 / n);
  const barK = opts.bar ? Math.min(1, 16 / design.tw) : 1;
  const k = barK * k10;
  let tw = design.tw * k * sc;
  let nf = (k < 1 ? design.nf * (0.5 + k / 2) : design.nf) * sc;
  const gap = (k < 1 ? 1 : 2) * sc;
  const bw0 = Math.round(tw * 0.45);
  const slot = (bw: number) => bw + 2 * sc + 2 * sc;          // collar (w + 2) + 1 pt margin each side
  const naturalW = n * tw + nb * slot(bw0) + (n + nb - 1) * gap;
  // ★ The bulbs' collar overhang and margins are fixed points, not proportional — fit the rest.
  const fixed = nb * 4 * sc;
  const fit = naturalW > fixed && w > fixed ? Math.min(1, (w - fixed) / (naturalW - fixed)) : 1;
  tw *= fit;
  const bw = Math.max(2, bw0 * fit);
  const glassMax = design.th * sc;
  const glassH = Math.max(MIN_GLASS * sc, Math.min(glassMax, h - (PIP_H + COLLAR_H + 2 * CLEAR) * sc));
  nf = Math.min(nf * fit, glassH * 0.8);
  const bulbH = glassH * 0.62;
  const gapF = gap * fit;
  const groupW = n * tw + nb * (bw + 4 * sc) + (n + nb - 1) * gapF;
  let x = Math.max(0, (w - groupW) / 2);
  const tubes: TubeBox[] = [];
  const bulbs: TubeBox[] = [];
  for (let d = 0; d < n; d++) {
    const b = spec.bulbAt.indexOf(d);
    if (b >= 0) {
      bulbs[b] = { x: x + 2 * sc, w: bw };                     // 1 pt margin + the collar's 1 pt overhang
      x += bw + 4 * sc + gapF;
    }
    tubes.push({ x, w: tw });
    x += tw + gapF;
  }
  // ★★★ Anchored: the collar's foot on the window floor, no clearance under it (see the header).
  const collarY = h - COLLAR_H * sc;
  return { tubes, bulbs, glassH, bulbH, nf, gap: gapF, collarY, drop: CLEAR * sc, naturalW, fit };
}

/** The whole stack's height for a glass height — what must fit in the window (its clearance included,
 *  all of it above the pip). */
export function stackHeight(glassH: number, scale = 1): number {
  return glassH + (PIP_H + COLLAR_H + 2 * CLEAR) * scale;
}
