/**
 * decoderTokens.ts — the decoder boxes' palette, per chassis × controls colour × TRANSPARENCY EFFECTS
 * (faceplates brief §10.2; numbers from docs/faceplates/Decoder.mockup.dc.html `renderVals()`).
 *
 * Every box over the waterfall (DabPanel, DecoderPanel + AircraftPanel, AdvRdsPanel, and the frame of
 * LocalHardwarePanel) takes its colours from HERE, through DecoderShell's `useDecoderTokens()`. No
 * panel holds a palette of its own any more — that was the drift §10.1 unified.
 *
 * ★★★ THE TWO SURFACES. What a piece of text sits on decides its colour, not which chassis it is:
 *   • `glass` — a tint over the waterfall (every box with Transparency ON), or that same tint made
 *     OPAQUE on the default chassis with Transparency OFF (`solidBg`: alpha 1.0, no blur).
 *   • `metal` — Transparency OFF on silver / black: a brushed plate. The HEADER sits on the metal (engraved
 *     text); the BODY sits in a recessed dark window, so body text is the same as on glass.
 *   Header text therefore has its own tokens (`title`, `hdrMuted`, `hdrValue`, `hdrAccent`) and body
 *   text its own (`muted`, `label`, `rowLabel`, `value`) — a body read-out drawn in an engraved
 *   header colour would be dark grey on the black window, i.e. invisible.
 *
 * ★★★ MEANING COLOURS ARE NOT TOKENS OF THE LOOK (§10.2 TRAP): good / warn / bad, the MER pink, the
 *   SNR green and the sync dot are the same on every chassis and every colour. A red-controls user
 *   must still see good from bad.
 *
 * ★★ THE TEXT COLOUR NEVER REACHES A DECODER BOX. The mockup lights labels, titles, keys and charts
 *   in the CONTROLS colour (`L()`); the text colour is the deck's readouts (§1: "Controls colour
 *   lights … decoder accents"). The resolver does not even take it, and the contrast test proves
 *   the result is identical under every text colour.
 *
 * ★ CONTRAST (§10.2: ≥ 4.5:1 in every chassis × colour; "if one fails, lighten the same hue"):
 *   `controlsText()` lightens a controls colour toward white — same hue — only as far as each
 *   surface needs. Measured in scripts/test_decoder_contrast.ts; the lifts it forced are listed at
 *   CONTRAST_LIFT below.
 *
 * Pure: type-only imports, so Node runs the test straight from this file.
 */

import type { Chassis, Transparency } from './faceplate';

export interface DecoderTokens {
  chassis:    Chassis;
  transparency: Transparency;
  /** `glass` (a tint, see-through or made solid) or `metal` (OFF on silver / black: plate + recessed window). */
  surface:    'glass' | 'metal';
  /** ★★★ Transparency OFF on the default chassis: the glass tint composited over black at alpha 1.0
   *  EXACTLY — drawn on the box itself, no tint layer, no BlurView. Null when see-through (and on
   *  metal, whose plate is its own opaque base). */
  solidBg:    string | null;

  // ── Frame ──
  /** Alpha of the glass tint (SMALL / BIG). Unused on metal. */
  tint:       number;
  tintTall:   number;
  /** iOS backdrop blur under the SMALL glass, 0 for none. ★ Never on silver / black (§10.2, §3.4). */
  blur:       number;
  tintRgb:    string;
  border:     string;
  /** The controls-colour glow round a silver / black see-through box (mockup `0 0 8px L(.18)`). */
  glow:       string | null;
  /** The hairline under the header; null on metal (the mockup's metal header has none). */
  hdrBdr:     string | null;
  /** Metal: the recessed data window (mockup `#070605`, margin 0 8 8, radius 6) and its lip. */
  window:     { bg: string; lip: string; inset: number; radius: number } | null;

  // ── Header text ──
  title:      string;
  titleMin:   string;
  hdrMuted:   string;
  hdrValue:   string;
  hdrAccent:  string;
  /** Engraved header text on metal (§10.3): silver dark with a white lip below, black light grey
   *  with a dark lip above. */
  engrave:    { color: string; dy: number } | null;

  // ── Body text ──
  /** Status text, field labels' dim cousin, plot captions. */
  muted:      string;
  /** Section heads. */
  label:      string;
  /** Field labels in label/value rows. ★ Today's default is the MUTED gold (the panels drew their
   *  row labels with `muted`); the mockup's silver / black rows are `L(.86)`. */
  rowLabel:   string;
  value:      string;
  /** Emphasis (a spots callsign, DAB's block). */
  accent:     string;
  rowActive:  string;
  /** DecoderPanel's teleprinter output glow (a soft shadow, not a font effect). */
  outputGlow: string;
  /** Hairlines between list rows. */
  divider:    string;

  // ── Header keys (dome keys on every chassis, §5 / §10) ──
  keyLook:      'outline' | 'cap';
  keyBorder:    string;
  keyBorderAct: string;
  keyBgAct:     string;
  keyText:      string;
  keyTextAct:   string;
  /** The lit legend's glow on a cap (mockup `text-shadow: 0 0 4px L(.55)`). */
  keyGlow:      string | null;
  /** Silver: a dark copy under the legend, as the deck's silver keys have (keyLegend.shade). */
  keyShade:     string | null;
  close:        string;
  /** Buttons INSIDE the body (DecoderPanel's filter / speed chips): outline chips on every chassis —
   *  they sit on the glass or in the dark window, never on the metal. */
  chipBorder:    string;
  chipBorderAct: string;
  chipBgAct:     string;
  cap:          { base: string; hi: string; lo: string; border: string; topLine: string;
                  cast: string; castOpacity: number; pressDim: number } | null;

  // ── Charts ──
  dotIdle:    string;
  plot:       string;
  axis:       string;
  axisStrong: string;
  axisFaint:  string;
  chartText:  string;
  /** Constellation points. */
  dot:        string;
  /** Impulse-response bars. */
  bar:        string;
  /** Scope traces (the RDS eye, the MPX trace). */
  trace:      string;

  // ── Meaning colours: never recoloured (§10.2) ──
  good:       string;
  warn:       string;
  bad:        string;
  dotOn:      string;
  mer:        string;
}

/** The meaning colours, fixed (§10.2 TRAP). Exported so the test can prove no chassis moves them. */
export const DECODER_MEANING = {
  good:  '#7dff9a',
  warn:  '#ffd479',
  bad:   '#ff8a7d',
  dotOn: '#55d98d',
  mer:   '#ff8fa3',
} as const;

/** The value colour on silver / black (§10.2 "values stay near-white"). Default keeps `#ffe566`. */
export const METAL_VALUE = '#f2efe8';

const GOLD = (a: number) => `rgba(255,160,0,${a})`;

// ── Colour maths (shared with the contrast test) ─────────────────────────────

export type RGB = [number, number, number];

export function parseRgb(rgb: string): RGB {
  const p = rgb.split(',').map(Number);
  return [p[0], p[1], p[2]];
}

/** Mix a colour toward white by `t` (0 = itself, 1 = white): the "same hue, lighter" of §10.2. */
export function lighten(c: RGB, t: number): RGB {
  return [c[0] + (255 - c[0]) * t, c[1] + (255 - c[1]) * t, c[2] + (255 - c[2]) * t]
    .map(v => Math.round(v)) as RGB;
}

/**
 * ★ The contrast lift per controls colour: how far toward white its TEXT roles go on a decoder box
 * (graphics and borders keep the pure colour). Found by scripts/test_decoder_contrast.ts — the
 * smallest step (of 0.05) at which every text role clears 4.5:1 on every surface it sits on.
 * Keyed by the triplet so a new colour cannot slip past it: an unlisted colour gets no lift and
 * the test fails until it has one.
 */
export const CONTRAST_LIFT: Record<string, number> = {
  '61,255,114':  0,      // green
  // ★★ Red is the dark one (relative luminance .24 against green's .74): pure #ff3a2e measured
  //   3.7:1 as a label on BIG's glass. .25 gives rgb 255,107,98 — still plainly red, and still
  //   distinct from the `bad` meaning colour #ff8a7d, which it must never be mistaken for.
  '255,58,46':   0.25,   // red
  '255,174,26':  0,      // amber
  '61,155,255':  0,      // blue
  '215,228,255': 0,      // white
  '255,106,20':  0.05,   // neon
};

/** The controls colour as it lights decoder TEXT (lifted as far as contrast needs), as a triplet. */
export function controlsText(controlsRgb: string): string {
  const lift = CONTRAST_LIFT[controlsRgb] ?? 0;
  return lighten(parseRgb(controlsRgb), lift).join(',');
}

// ── The resolver ─────────────────────────────────────────────────────────────

/** The default box's glass: its tint triplet and alpha (SMALL). */
const DEFAULT_TINT_RGB = '10,8,4';
const DEFAULT_TINT = 0.72;

/**
 * ★★★ Transparency OFF on the default chassis: today's SMALL glass composited over black, at alpha
 * 1.0. Not the old Solid's 0.95 ("I thought solid would be 1.0 fully solid for max GPU savings") —
 * and at 0.95 iOS would still draw the box's drop shadow per pixel (see DecoderShell). Same maths as
 * faceplate.ts `solidOver` (no runtime import here, so Node can run the test on this file alone);
 * scripts/test_transparency.ts proves the two agree.
 */
export const DEFAULT_SOLID_BG = `rgb(${parseRgb(DEFAULT_TINT_RGB).map(v => Math.round(v * DEFAULT_TINT)).join(',')})`;

/** Today's gold chrome — the mockup's `isDef` branch, with the literals the panels drew. */
function defaultTokens(transparency: Transparency): DecoderTokens {
  const solid = transparency === 'off';
  return {
    chassis: 'default', transparency, surface: 'glass',
    // ★★ ON = the RDS panel's glass, which the brief calls "today's glass" (0.72, BIG 0.62, iOS
    //   blur under SMALL). OFF = that glass made opaque (solidBg) — same colour on a dark
    //   waterfall, nothing behind it drawn through it. See DecoderShell.
    tint: solid ? 1 : DEFAULT_TINT, tintTall: solid ? 1 : 0.62, blur: solid ? 0 : 35,
    tintRgb: DEFAULT_TINT_RGB, solidBg: solid ? DEFAULT_SOLID_BG : null,
    border: GOLD(0.28), glow: null, hdrBdr: GOLD(0.12), window: null,
    title: GOLD(0.86), titleMin: GOLD(0.40),
    hdrMuted: GOLD(0.72), hdrValue: '#ffe566', hdrAccent: '#ffb833', engrave: null,
    // ★★ .60 → .72 for the dim gold (status, row labels, key legends): the ONLY default values the
    //   contrast rule moved. At .60 they measured 4.0:1 on the SMALL glass and 3.5:1 on BIG's 0.62
    //   over a busy band; .72 clears 4.5:1 on both and still sits well below the .86 titles, so
    //   the hierarchy AdvRdsPanel's note defends ("still clearly SUBORDINATE") is kept.
    muted: GOLD(0.72), label: GOLD(0.86), rowLabel: GOLD(0.72),
    value: '#ffe566', accent: '#ffb833', rowActive: GOLD(0.14),
    outputGlow: 'rgba(255,220,100,0.35)', divider: GOLD(0.08),
    keyLook: 'outline',
    keyBorder: GOLD(0.28), keyBorderAct: GOLD(0.55), keyBgAct: GOLD(0.12),
    keyText: GOLD(0.72), keyTextAct: '#ffb833', keyGlow: null, keyShade: null,
    close: 'rgba(255,100,100,0.70)', cap: null,
    chipBorder: GOLD(0.28), chipBorderAct: GOLD(0.55), chipBgAct: GOLD(0.12),
    dotIdle: GOLD(0.35),
    plot: GOLD(0.05), axis: GOLD(0.18), axisStrong: 'rgba(255,160,60,0.35)',
    axisFaint: 'rgba(255,170,60,0.07)', chartText: 'rgba(255,190,110,0.85)',
    dot: 'rgba(125,255,154,0.75)', bar: 'rgba(255,190,90,0.85)', trace: 'rgba(125,255,154,0.85)',
    ...DECODER_MEANING,
  };
}

/** Silver / black. `controlsRgb` is the controls colour's triplet (LED[controls].rgb). */
function metalTokens(chassis: 'silver' | 'black', controlsRgb: string, transparency: Transparency): DecoderTokens {
  const solid = transparency === 'off';
  const L  = (a: number) => `rgba(${controlsRgb},${a})`;
  const Lt = (a: number) => `rgba(${controlsText(controlsRgb)},${a})`;
  const silver = chassis === 'silver';

  // Header text: engraved on the metal when OFF; the lit controls colour over the glass otherwise.
  const hdr = solid
    ? (silver
      ? { title: '#2a2824', titleMin: 'rgba(42,40,36,0.60)', hdrMuted: '#35332e',
          engrave: { color: 'rgba(255,255,255,0.60)', dy: 1 } }
      : { title: '#c9ccd2', titleMin: 'rgba(201,204,210,0.60)', hdrMuted: '#a3a6ac',
          engrave: { color: 'rgba(0,0,0,0.90)', dy: -1 } })
    : { title: Lt(0.92), titleMin: Lt(0.50), hdrMuted: Lt(0.85), engrave: null };

  return {
    chassis, transparency, surface: solid ? 'metal' : 'glass', solidBg: null,
    // ★★ No blur on silver / black in either mode (§10.2, §3.4) — a metal deck over a blurred
    //   spectrum would pay the one cost the opaque plate exists to avoid.
    tint: 0.72, tintTall: 0.62, blur: 0, tintRgb: '10,8,4',
    border:  solid ? (silver ? '#8b8983' : '#3a3c40') : L(0.40),
    glow:    solid ? null : L(0.18),
    hdrBdr:  solid ? null : L(0.18),
    window:  solid ? { bg: '#070605', lip: silver ? 'rgba(255,255,255,0.80)' : 'rgba(255,255,255,0.14)',
                       inset: 8, radius: 6 } : null,
    ...hdr,
    hdrValue:  solid ? hdr.title : METAL_VALUE,
    hdrAccent: solid ? hdr.title : Lt(1),
    muted: Lt(0.85), label: Lt(0.92), rowLabel: Lt(0.92),
    value: METAL_VALUE, accent: Lt(1), rowActive: L(0.14),
    outputGlow: 'rgba(0,0,0,0)', divider: L(0.10),
    keyLook: 'cap',
    keyBorder: silver ? '#8d8a83' : '#050505', keyBorderAct: silver ? '#8d8a83' : '#050505',
    keyBgAct: 'transparent',
    // ★ Silver caps are LIGHT: lifting toward white would only fade the legend, so they take the
    //   pure colour (the mockup's `L(.9)` / `L(1)`) with the deck keys' dark shade under it (§5,
    //   FaceplateTheme keyLegend.shade). Black caps are dark, so they take the lifted text colour.
    keyText:    silver ? L(0.90) : Lt(0.92),
    keyTextAct: silver ? L(1) : Lt(1),
    keyGlow:    L(0.55),
    keyShade:   silver ? 'rgba(0,0,0,0.6)' : null,
    close:      silver ? L(0.90) : Lt(0.90),
    chipBorder: L(0.35), chipBorderAct: L(0.70), chipBgAct: L(0.14),
    cap: silver
      ? { base: '#c4c1ba', hi: 'rgba(255,255,255,0.40)', lo: 'rgba(0,0,0,0.10)', border: '#8d8a83',
          topLine: 'rgba(255,255,255,0.80)', cast: '#000000', castOpacity: 0.6, pressDim: 0.84 }
      : { base: '#141517', hi: 'rgba(255,255,255,0.05)', lo: 'rgba(0,0,0,0.25)', border: '#050505',
          topLine: 'rgba(255,255,255,0.22)', cast: '#000000', castOpacity: 0.85, pressDim: 0.82 },
    dotIdle: L(0.35),
    plot: L(0.05), axis: L(0.25), axisStrong: L(0.35), axisFaint: L(0.07), chartText: Lt(0.85),
    dot: Lt(0.85), bar: Lt(0.70), trace: Lt(0.85),
    ...DECODER_MEANING,
  };
}

/** Unmemoised — the contrast test's `--find` sweep changes CONTRAST_LIFT under it. */
export function buildDecoderTokens(chassis: Chassis, controlsRgb: string, transparency: Transparency): DecoderTokens {
  return chassis === 'default' ? defaultTokens(transparency) : metalTokens(chassis, controlsRgb, transparency);
}

const cache = new Map<string, DecoderTokens>();

/**
 * ★ The resolver. Memoised on its inputs, so every panel on the screen shares ONE object per
 * setting and a style sheet built from it (DecoderShell `useDecoderStyles`) is built once, not per
 * render — a spectrum frame never touches it.
 */
export function decoderTokensFor(chassis: Chassis = 'default', controlsRgb = '61,255,114',
                                 transparency: Transparency = 'on'): DecoderTokens {
  const key = chassis === 'default' ? `default|${transparency}` : `${chassis}|${controlsRgb}|${transparency}`;
  let t = cache.get(key);
  if (!t) {
    t = buildDecoderTokens(chassis, controlsRgb, transparency);
    cache.set(key, t);
  }
  return t;
}
