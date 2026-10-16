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
import type { Skia, SkCanvas, SkPath } from '@shopify/react-native-skia';

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

/**
 * ★★★ THE IN-14's OWN DIGITS, as wire paths (Stuart, 2026-10-16: "match the real numbers, such as the
 * flat top 3 and the lack of tail on the 7"). Traced from his 4K close-up (IMG_1559, one tube through
 * all ten digits) and his photo of unlit tubes. Frequency tubes only: the Nixie One font is untouched
 * everywhere else.
 *
 * A digit is a list of strokes in a UNIT BOX: y 0 (top) … 1 (bottom), x 0 … GLYPH_W, each stroke one
 * piece of wire with round ends. Shapes:
 *   0 a plain tall oval · 1 a straight wire, no flag or foot · 2 an arc over the top, an S down to a
 *   rounded corner, a flat base with NO upturn · 3 FLAT-TOPPED (Ʒ): top bar, a straight diagonal down
 *   to a round bowl · 4 closed apex, the stem running below the line, the bar through it · 5 the 2's
 *   part turned upside down (a vertical flip) · 6 a straight stroke from the top right into a round
 *   bowl · 7 a flat top bar and a straight stem, NO tail at the foot · 8 a small loop on a larger one
 *   · 9 the 6's part turned round (rotated 180°).
 */
export const GLYPH_W = 0.6;
export type Stroke =
  | { k: 'line'; pts: Array<[number, number]> }                           // polyline
  | { k: 'arc'; cx: number; cy: number; rx: number; ry: number; a0: number; sweep: number }  // degrees, clockwise from +x
  | { k: 'cubic'; p0: [number, number]; c1: [number, number]; c2: [number, number]; p1: [number, number] };

const TWO: Stroke[] = [
  { k: 'arc', cx: 0.30, cy: 0.27, rx: 0.28, ry: 0.27, a0: 192, sweep: 212 },          // over the top, down the right
  { k: 'cubic', p0: [0.524, 0.426], c1: [0.42, 0.60], c2: [0.06, 0.70], p1: [0.02, 0.92] }, // the S into the corner
  { k: 'cubic', p0: [0.02, 0.92], c1: [0.01, 0.98], c2: [0.05, 1.0], p1: [0.12, 1.0] },  // rounded corner
  { k: 'line', pts: [[0.12, 1.0], [0.60, 1.0]] },                                       // flat base, no upturn
];
/* ★ The 6 LEANS (Stuart, 2026-10-16: "the 6 and 9 need to rotate slightly"). Measured on the 4K 6
 *   (IMG_1559 11.6 s): the stem is a STRAIGHT wire at 29° from vertical, from the top right down to a
 *   tangent with the left of a round bowl (centre 0.297, 0.721; r 0.276 of the digit height). The 9
 *   (12.7 s) is the same part turned round: its tail measures 25°. Was: a curve starting at 42° and
 *   curling to upright at the bowl. */
const SIX: Stroke[] = [
  { k: 'line', pts: [[0.384, 0.0], [0.055, 0.584]] },                                   // the stem, tangent to the bowl
  { k: 'arc', cx: 0.297, cy: 0.721, rx: 0.276, ry: 0.276, a0: 0, sweep: 360 },          // the bowl
];
const flipY = (st: Stroke[]): Stroke[] => st.map((s) => s.k === 'line' ? { ...s, pts: s.pts.map(([x, y]) => [x, 1 - y] as [number, number]) }
  : s.k === 'arc' ? { ...s, cy: 1 - s.cy, a0: -s.a0, sweep: -s.sweep }
  : { ...s, p0: [s.p0[0], 1 - s.p0[1]], c1: [s.c1[0], 1 - s.c1[1]], c2: [s.c2[0], 1 - s.c2[1]], p1: [s.p1[0], 1 - s.p1[1]] });
const rot180 = (st: Stroke[]): Stroke[] => st.map((s) => {
  const R = ([x, y]: [number, number]): [number, number] => [GLYPH_W - x, 1 - y];
  return s.k === 'line' ? { ...s, pts: s.pts.map(R) }
    : s.k === 'arc' ? { ...s, cx: GLYPH_W - s.cx, cy: 1 - s.cy, a0: s.a0 + 180 }
    : { ...s, p0: R(s.p0), c1: R(s.c1), c2: R(s.c2), p1: R(s.p1) };
});

export const CATHODE_STROKES: Record<string, Stroke[]> = {
  '0': [{ k: 'arc', cx: 0.30, cy: 0.5, rx: 0.31, ry: 0.5, a0: 0, sweep: 360 }],
  '1': [{ k: 'line', pts: [[0.30, 0.0], [0.30, 1.0]] }],
  '2': TWO,
  '3': [
    { k: 'line', pts: [[0.0, 0.0], [0.60, 0.0], [0.19, 0.39]] },                        // flat top, diagonal
    { k: 'arc', cx: 0.30, cy: 0.68, rx: 0.31, ry: 0.32, a0: 250, sweep: 262 },          // the bowl, ending low left
  ],
  '4': [
    { k: 'line', pts: [[0.0, 0.72], [0.45, -0.02], [0.45, 1.06]] },                      // diagonal up to the apex, stem down
    { k: 'line', pts: [[0.0, 0.72], [0.60, 0.72]] },                                     // the bar
  ],
  '5': flipY(TWO),
  '6': SIX,
  '7': [{ k: 'line', pts: [[0.0, 0.0], [0.60, 0.0], [0.20, 1.0]] }],
  '8': [
    { k: 'arc', cx: 0.30, cy: 0.21, rx: 0.235, ry: 0.21, a0: 0, sweep: 360 },
    { k: 'arc', cx: 0.30, cy: 0.71, rx: 0.295, ry: 0.29, a0: 0, sweep: 360 },
  ],
  '9': rot180(SIX),
};

/** One digit's wire as a Skia path, the unit box scaled to `h` (pt) and centred on (cx, cy). */
export function cathodePath(Sk: SkiaApi, d: string, cx: number, cy: number, h: number): SkPath {
  const X = (u: number) => cx + (u - GLYPH_W / 2) * h, Y = (v: number) => cy + (v - 0.5) * h;
  const p = Sk.Path.Make();
  for (const s of CATHODE_STROKES[d] ?? []) {
    if (s.k === 'line') {
      s.pts.forEach(([u, v], i) => (i === 0 ? p.moveTo(X(u), Y(v)) : p.lineTo(X(u), Y(v))));
    } else if (s.k === 'cubic') {
      p.moveTo(X(s.p0[0]), Y(s.p0[1]));
      p.cubicTo(X(s.c1[0]), Y(s.c1[1]), X(s.c2[0]), Y(s.c2[1]), X(s.p1[0]), Y(s.p1[1]));
    } else {
      const oval = Sk.XYWHRect(X(s.cx - s.rx), Y(s.cy - s.ry), 2 * s.rx * h, 2 * s.ry * h);
      if (Math.abs(s.sweep) >= 360) p.addOval(oval);
      else p.arcToOval(oval, s.a0, s.sweep, true);
    }
  }
  return p;
}

// ── The tube's inside, drawn ONCE per size into a sprite (NixieTubes) ──────────
// Pure apart from the Skia API passed in, so scripts can render it headless (canvaskit) to compare
// against the reference video.

/** The anode box: the rectangle of hex mesh the cathodes stand in, in a tw × th glass (y from the
 *  glass top, which includes the dome). */
export function meshBox(tw: number, th: number) {
  // ★ The cage fills the tube (Stuart: "the mesh needs to fill the whole tube"; his unlit photo): edge to
  //   edge across the glass, from where the dome meets the straight wall (the glass's top corners are
  //   26 % of its height) down to just above the floor shade.
  return { x: tw * 0.07, y: th * 0.25, w: tw * 0.86, h: th * 0.71 };
}
/** ★ The top spacer (Stuart, 2026-10-16: "a small piece above the number that has a slight reflection of
 *  the glow"): the IN-14's flat metal strap across the top of the cage, in the gap between the cage and
 *  the dome — no extra height. Its underside catches the digit's light. */
export function strapBox(tw: number, th: number) {
  const b = meshBox(tw, th);
  const h = Math.max(0.6, th * 0.022);
  return { x: b.x - tw * 0.01, y: b.y - th * 0.05, w: b.w + tw * 0.02, h };
}

/** ★ The tip-off pip (where the glass was sealed): a small glass nub on the dome — a round knob on a
 *  narrow neck flaring into the dome. It fits the PIP_H the stack already budgets (the knob sits in it,
 *  the flare runs down onto the dome), so the tube is no taller. */
export function pipPath(Sk: SkiaApi, cx: number, glassY: number, pipY: number, tw: number): { neck: SkPath; knob: { cx: number; cy: number; r: number } } {
  const ph = Math.max(0.5, glassY - pipY);
  const r = Math.min(tw * 0.07, ph * 0.5);
  const nw = r * 0.6, fw = tw * 0.10;
  const knobY = pipY + r;
  const neck = Sk.Path.Make();
  neck.moveTo(cx - fw, glassY + ph * 0.9);
  neck.quadTo(cx - nw, glassY + ph * 0.2, cx - nw, knobY);
  neck.lineTo(cx + nw, knobY);
  neck.quadTo(cx + nw, glassY + ph * 0.2, cx + fw, glassY + ph * 0.9);
  neck.close();
  return { neck, knob: { cx, cy: knobY, r } };
}

/** A digit's height as a fraction of the digit size nf (Nixie One's figure height, so the tubes keep
 *  their size). */
export const DIGIT_H = 0.72;
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
  // ★ Cell size from the photo and the 4K frames: about seven hexes across the tube's face.
  const r = Math.max(1.0, tw * 0.075);
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

/** The mesh's wire: fine bright metal (the photo), shared by the static cage and the sprite's shadow. */
export const meshWire = (tw: number) => Math.max(0.35, tw * 0.012);

/**
 * ★★★ EVERY CATHODE IS THE SAME FINE WIRE (Stuart, 2026-10-16: "the thickness comes from the glow,
 * not the electrode wire itself"). Lit or not, a digit is one hairline of metal. A lit digit looks bold
 * only because of the neon sheath round that wire: a hot core hugging it, then the sheath, the halo and
 * a wide bloom — so the lit weight is built by the glow layers alone, never by a fat stroke.
 * Widths and blurs are fractions of the digit size (nf); colours sampled from the 4K video off the
 * clipped core: sheath #f59306, halo #d65409, lit window #be4608 — deep neon orange, not yellow-white.
 */
export interface NeonLayer { width: number; sigma: number; color: string; alpha: number }
export const DIGIT_NEON: NeonLayer[] = [
  { width: 0.16,  sigma: 0.15,  color: 'rgb(200,62,6)',   alpha: 0.80 },   // wide bloom, fills the window
  { width: 0.09,  sigma: 0.05,  color: 'rgb(214,84,9)',   alpha: 1 },   // the gas sheath
  { width: 0.062, sigma: 0.016, color: 'rgb(240,110,8)',  alpha: 1 },      // the glow hugging the wire
  { width: 0.046, sigma: 0.006, color: 'rgb(252,150,22)', alpha: 1 },      // the hot core round the wire
  { width: 0.03,  sigma: 0.003, color: 'rgb(255,182,60)', alpha: 1 },      // white-hot next to the wire
]
/** The wire itself, white-hot in its sheath. */
export const DIGIT_CORE = '#ffb848';
/** The bare wire: hairline-fine next to the lit glow (the 4K frames, the unlit photo). Fraction of nf,
 *  never under one device pixel (so it neither vanishes nor aliases into dashes at 1x). */
export const WIRE = 0.011;
/* Unlit wire colours. ★ Stuart's photo of four UNLIT IN-14s: bright silver-grey wire, the 6 right behind
 *   the mesh clearest, the stack fading as it goes back. Neutral metal — never greenish. */
const WIRE_OFF       = 'rgba(215,215,210,0.34)';   // dimmer than the daylit photo: the recess is dark
/** An off tube's wires fade with depth (the photo): alpha × (1 − OFF_FADE·i). */
const OFF_FADE       = 0.08;
const WIRE_WARM      = 'rgba(255,160,100,0.30)';   // behind a lit digit: lit warm by its glow
const WIRE_FRONT     = 'rgba(30,26,22,0.55)';      // in front of a lit digit: dark metal across the glow
/** The anode mesh in front of the stack: it shadows the glow as fine hex lines, and is lit warm itself. */
const MESH_SHADOW    = 'rgba(40,14,4,0.17)';
const MESH_LIT       = 'rgba(230,100,30,0.20)';
const MESH_WASH      = 'rgba(190,70,8,0.36)';

/**
 * Draw ONE tube's inside — the whole cathode stack with `lit` glowing at its own depth (null = a
 * switched-off tube), the anode mesh over it — into `c`, a tw × th canvas whose origin is the glass's
 * top-left. Clipped to the glass. `px` is one device pixel in pt (the wire's floor).
 * ★★ PERF: this runs once per digit per tube SIZE (11 sprites), never per tune step: the blurred glow,
 *   up to ten wire paths and the mesh are all rasterised here, and a tune step blits one image.
 */
export function drawTubeInside(Sk: SkiaApi, c: SkCanvas, lit: string | null,
                               tw: number, th: number, nf: number, px = 1 / 3): void {
  const paint = (color: string, opts: { stroke?: number; blend?: number } = {}) => {
    const p = Sk.Paint();
    p.setAntiAlias(true);
    p.setColor(Sk.Color(color));
    if (opts.stroke) {
      p.setStyle(1 /* PaintStyle.Stroke */); p.setStrokeWidth(opts.stroke);
      p.setStrokeCap(1 /* StrokeCap.Round */); p.setStrokeJoin(1 /* StrokeJoin.Round */);
    }
    if (opts.blend !== undefined) p.setBlendMode(opts.blend);
    return p;
  };
  const { cx, cy } = digitCentre(tw, th);
  const gh = DIGIT_H * nf;
  const paths: Record<string, SkPath> = {};
  const pathOf = (d: string) => (paths[d] ??= cathodePath(Sk, d, cx, cy, gh));
  const wireW = Math.max(px, WIRE * nf);
  /** The cathode's wire in an OPAQUE colour, `w` pt wide. */
  const shape = (d: string, color: string, w: number) => c.drawPath(pathOf(d), paint(color, { stroke: w }));
  /** Draw `draw` as one layer at `alpha`, blurred by `sigma` pt — so a shape's overlapping parts never
   *  double up, and the blur is the layer's. */
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
  if (lit === null) {
    // ★ A switched-off tube: the whole stack as bare silver wire, the 6 right behind the mesh clearest,
    //   each one further back fainter (Stuart's photo of unlit IN-14s). Drawn wire by wire, back first,
    //   so the front ones read over the tangle behind them.
    const [offCol, offA] = rgbA(WIRE_OFF);
    for (let i = CATHODE_STACK.length - 1; i >= 0; i--) {
      const d = CATHODE_STACK[i];
      atDepth(d, (z) => layer(offA * z.opacity * Math.max(0.15, 1 - OFF_FADE * i), 0, () => shape(d, offCol, wireW / z.scale)));
    }
  } else {
    const [backCol, backA] = rgbA(WIRE_WARM);
    layer(backA, 0, () => {
      for (let i = CATHODE_STACK.length - 1; i > li; i--) {
        const d = CATHODE_STACK[i];
        atDepth(d, (z) => layer(z.opacity, 0, () => shape(d, backCol, wireW / z.scale)));
      }
    });
  }
  if (lit !== null) {
    atDepth(lit, (z) => {
      for (const g of DIGIT_NEON) layer(g.alpha * z.opacity, g.sigma * nf * z.soft, () => shape(lit, g.color, g.width * nf));
      layer(z.opacity, 0, () => shape(lit, DIGIT_CORE, wireW / z.scale));
    });
    const [frontCol, frontA] = rgbA(WIRE_FRONT);
    layer(frontA, 0, () => {
      for (let i = li - 1; i >= 0; i--) {
        const d = CATHODE_STACK[i];
        atDepth(d, (z) => shape(d, frontCol, wireW / z.scale));
      }
    });
  }
  if (lit !== null) {
    const b = meshBox(tw, th);
    const box = Sk.XYWHRect(b.x, b.y, b.w, b.h);
    const mesh = honeycombPath(Sk, tw, th);
    c.save();
    c.clipRect(box, 1 /* ClipOp.Intersect */, true);
    const meshW = meshWire(tw);
    // The mesh's shadow lands only on what is drawn (glow + wires): SrcATop.
    c.drawPath(mesh, paint(MESH_SHADOW, { stroke: meshW, blend: 9 /* BlendMode.SrcATop */ }));
    // Then BEHIND everything drawn (DstOver): the mesh lit by the digit, and its warm wash.
    c.drawPath(mesh, paint(MESH_LIT, { stroke: meshW, blend: 4 /* BlendMode.DstOver */ }));
    c.drawRect(box, paint(MESH_WASH, { blend: 4 }));
    c.restore();
    /* ★ NO HARD CUT-OFF AT THE CAGE TOP (Stuart, 2026-10-16): the wash carries on above the cage and fades
     *  into the dome, as the real tube's light spills upward onto the glass and the parts above. */
    const fadeTop = b.y * 0.15;
    const up = Sk.Paint();
    up.setAntiAlias(true);
    up.setBlendMode(4 /* DstOver */);
    up.setShader(Sk.Shader.MakeLinearGradient({ x: 0, y: b.y }, { x: 0, y: fadeTop },
      [Sk.Color(MESH_WASH), Sk.Color('rgba(190,70,8,0)')], [0, 1], 0 /* TileMode.Clamp */));
    c.drawRect(Sk.XYWHRect(0, fadeTop, tw, b.y - fadeTop), up);
    // The top strap catching the glow on its underside.
    const sb = strapBox(tw, th);
    const sp = Sk.Paint();
    sp.setAntiAlias(true);
    sp.setShader(Sk.Shader.MakeLinearGradient({ x: 0, y: sb.y }, { x: 0, y: sb.y + sb.h },
      [Sk.Color('rgba(255,150,70,0.12)'), Sk.Color('rgba(255,175,95,0.42)')], [0, 1], 0));
    c.drawRect(Sk.XYWHRect(sb.x, sb.y, sb.w, sb.h), sp);
    // and a faint warm sheen on the dome glass above it
    const dome = Sk.Paint();
    dome.setAntiAlias(true);
    dome.setShader(Sk.Shader.MakeRadialGradient({ x: tw / 2, y: sb.y }, tw * 0.55,
      [Sk.Color('rgba(255,120,40,0.10)'), Sk.Color('rgba(255,120,40,0)')], [0, 1], 0));
    c.drawRect(Sk.XYWHRect(0, 0, tw, sb.y), dome);
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
