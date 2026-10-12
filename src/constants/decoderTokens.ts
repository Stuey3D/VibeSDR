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
 * ★★★ TEXT IN THE TEXT COLOUR, KEYS IN THE CONTROLS COLOUR, MEANINGS FIXED — on EVERY chassis
 *   (Stuart, 2026-10-12, after a blue-text default deck sat under an amber DAB box: "Follow the text
 *   colour except for when the colours have a meaning. So blue buttons and white text set in the
 *   controls would show white text in the decoder with the header buttons being blue. Green for the
 *   selected station, amber and red for other status updates."). This REPLACES two older rules: the
 *   default chassis's fixed gold (§10.2 / §13.1 "today's look"), and "decoder accents = the controls
 *   colour" (§1). Now:
 *   • TEXT — titles, header read-outs, labels, values, decoded output, row text, codec labels, the
 *     chart marks (they are read-outs too): the user's TEXT colour (`textRgb`, the RESOLVED one —
 *     neon under Nixie).
 *   • KEYS — the header keys and the chips in the body, and with them the frame (border, header
 *     hairline, glow, the idle status dot), so the box reads as one with its keys: the CONTROLS colour.
 *   • MEANINGS — good / warn / bad, MER pink, SNR green, the sync dot, and the SELECTED / PLAYING
 *     station (`selected`): fixed, except where the TEXT colour is the meaning's own colour. Stuart named
 *     the first: "if someone has set green text … maybe use amber or white as the selected station" —
 *     WHITE, because amber is already `warn`. The rest (red text → bad white bold; amber / neon text →
 *     warn white; teal → selected white) are `meaningsFor`, the ONE helper every surface uses.
 *   ★ On metal with Transparency OFF the HEADER text stays engraved (dark on silver, light grey on
 *     black): it is cut into the plate, not lit, and no lit colour reads on a silver plate.
 *
 * ★ CONTRAST (§10.2: ≥ 4.5:1 in every chassis × colour; "if one fails, lighten the same hue"):
 *   `controlsText()` lightens a controls OR text colour toward white — same hue — only as far as each
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
  /** Field labels in label/value rows: the text colour a step under the values (.85). */
  rowLabel:   string;
  value:      string;
  /** Emphasis (a spots callsign, DAB's block). */
  accent:     string;
  /** A faint text-colour fill (a meter's track). */
  rowActive:  string;
  /** ★★ The SELECTED / PLAYING station (a meaning, not the look): green, or white when the text is
   *  green (`selectedFor`). `selectedLive` its live text under it, `selectedBg` its row's fill. */
  selected:     string;
  selectedLive: string;
  selectedBg:   string;
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
  /** Keyboard / D-pad focus ring (focusFor: white on green controls). */
  focus:      string;
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
  /** A spot's SNR below 0 dB — weak but decoded (DecoderPanel's spots table). */
  snrLow:     string;
  /** ★ `bad` is drawn BOLD as well (red text: white bold — see `meaningsFor`). Use `meaningWeight`. */
  badBold:    boolean;
}

/** The meaning colours (§10.2 TRAP) — fixed on every chassis and controls colour; only the TEXT colour can
 *  swap one, and only where the two would read alike (`meaningsFor`). */
export const DECODER_MEANING = {
  good:  '#7dff9a',
  warn:  '#ffd479',
  bad:   '#ff8a7d',
  dotOn: '#55d98d',
  mer:   '#ff8fa3',
  snrLow: 'rgba(255,160,0,0.65)',
} as const;

/** Near-white: the old silver / black value colour, and now the selected station when the text is green. */
export const METAL_VALUE = '#f2efe8';

/** The brief's green TEXT colour (faceplate.ts LED.green.rgb — not imported: this file stays pure;
 *  scripts/test_decoder_contrast.ts proves they agree). */
export const GREEN_TEXT_RGB = '61,255,114';
const SELECTED_GREEN = '125,255,154';   // = DECODER_MEANING.good — the browser's `.dabSvc.on`
const SELECTED_WHITE = '242,239,232';   // = METAL_VALUE

/**
 * ★★ The selected / playing station's colour, as a triplet. GREEN — the meaning Stuart gave it ("Green for the
 * selected station") — except when the TEXT is green, where a green station among green rows would not stand
 * out. ★ Then WHITE, not amber (Stuart offered "amber or white"): amber is already `warn`, and a selected
 * station that looks like a warning would be the clash all over again.
 */
export function selectedFor(textRgb: string): string {
  // ★ Teal too (2026-10-12): teal text (70,255,215) beside a green station read as one colour.
  return textRgb === GREEN_TEXT_RGB || textRgb === TEAL_TEXT_RGB ? SELECTED_WHITE : SELECTED_GREEN;
}

/** The brief's text-colour triplets the clash rule tests for (faceplate.ts LED — the test proves they agree). */
export const TEAL_TEXT_RGB  = '70,255,215';
export const RED_TEXT_RGB   = '255,58,46';
export const AMBER_TEXT_RGB = '255,174,26';
export const NEON_TEXT_RGB  = '255,106,20';   // Nixie: the text is locked to neon

export interface Meanings {
  good: string; warn: string; bad: string; dotOn: string; mer: string; snrLow: string;
  badBold: boolean;
  /** The selected / playing station, as a triplet (selectedFor). */
  selectedRgb: string;
}

/**
 * ★★★ THE CLASH RULE — the meaning colours for a given TEXT colour, shared by every surface (decoder boxes,
 * popups, the directory). Stuart, 2026-10-12: "Follow the text colour except for when the colours have a
 * meaning" — and where the user's text colour IS a meaning's colour, the meaning moves, never the text:
 *   • green / teal text → the selected station WHITE (selectedFor);
 *   • red text          → `bad` WHITE BOLD: a red value among red values would say nothing, and bold keeps
 *                          it from reading as an ordinary white figure;
 *   • amber text, neon text (Nixie) → `warn` WHITE, and the weak-SNR amber with it.
 *   ★ The green-text "good" tone is LEFT green (Stuart approved): good is the unremarkable case, and a value
 *     that merely looks normal when all is well is no loss.
 */
export function meaningsFor(textRgb: string): Meanings {
  const red   = textRgb === RED_TEXT_RGB;
  const amber = textRgb === AMBER_TEXT_RGB || textRgb === NEON_TEXT_RGB;
  return {
    ...DECODER_MEANING,
    warn:   amber ? METAL_VALUE : DECODER_MEANING.warn,
    snrLow: amber ? `rgba(${SELECTED_WHITE},0.65)` : DECODER_MEANING.snrLow,
    bad:    red ? METAL_VALUE : DECODER_MEANING.bad,
    badBold: red,
    selectedRgb: selectedFor(textRgb),
  };
}

/** Keyboard / D-pad focus green (PanelNav's historic NAV_FOCUS). */
export const FOCUS_GREEN = '#7CFF9B';
/** The brief's green controls LED and the default chassis's TODAY_GREEN (faceplate.ts — the test proves it). */
const GREEN_CONTROLS = new Set(['61,255,114', '0,230,0']);

/**
 * ★★ Keyboard / D-pad FOCUS for a controls colour (Stuart, 2026-10-12: "yes make it white"). Focus is a meaning
 * ("you are here") and was always green #7CFF9B — which vanished among GREEN keys once the keys took the controls
 * colour. WHITE on green controls; green everywhere else, where it still stands apart (★ not white on WHITE
 * controls, where white would vanish in the same way).
 */
export function focusFor(controlsRgb: string): string {
  return GREEN_CONTROLS.has(controlsRgb) ? '#ffffff' : FOCUS_GREEN;
}
/** The focus colour as a row's tint (was rgba(124,255,155,a)). */
export function focusTint(controlsRgb: string, a: number): string {
  return GREEN_CONTROLS.has(controlsRgb) ? `rgba(255,255,255,${a})` : `rgba(124,255,155,${a})`;
}

/** A meaning colour as a text style: the colour, and bold when it is `bad` under red text. */
export function meaningText(m: { bad: string; badBold: boolean }, colour: string | undefined):
    { color?: string; fontWeight?: '700' } {
  if (!colour) return {};
  return m.badBold && colour === m.bad ? { color: colour, fontWeight: '700' } : { color: colour };
}

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
  '70,255,215':  0,      // teal (a text colour only)
  '0,230,0':     0,      // TODAY_GREEN — the default chassis's green controls (faceplate.ts)
};

/** A controls or text colour as it lights decoder TEXT (lifted as far as contrast needs), as a triplet. */
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

/** Engraved header text on the metal plate (Transparency OFF, silver / black) — cut in, not lit. */
function engraved(silver: boolean) {
  return silver
    ? { title: '#2a2824', titleMin: 'rgba(42,40,36,0.60)', hdrMuted: '#35332e',
        engrave: { color: 'rgba(255,255,255,0.60)', dy: 1 } }
    : { title: '#c9ccd2', titleMin: 'rgba(201,204,210,0.60)', hdrMuted: '#a3a6ac',
        engrave: { color: 'rgba(0,0,0,0.90)', dy: -1 } };
}

/**
 * The tokens. `controlsRgb` is the RESOLVED controls LED's triplet (TODAY_GREEN on default), `textRgb`
 * the RESOLVED text colour's (neon under Nixie). See the header for which role takes which.
 */
function tokens(chassis: Chassis, controlsRgb: string, transparency: Transparency, textRgb: string): DecoderTokens {
  const off    = transparency === 'off';
  const def    = chassis === 'default';
  const metal  = !def && off;
  const silver = chassis === 'silver';
  const K  = (a: number) => `rgba(${controlsRgb},${a})`;                 // controls, graphics + borders
  const Kt = (a: number) => `rgba(${controlsText(controlsRgb)},${a})`;   // controls, as a legend
  const Tr = (a: number) => `rgba(${textRgb},${a})`;                     // text colour, chart graphics
  const Tt = (a: number) => `rgba(${controlsText(textRgb)},${a})`;       // text colour, as text
  const m = meaningsFor(textRgb);
  const sel = m.selectedRgb;

  // Header text: engraved on the metal (OFF, silver / black); the lit TEXT colour everywhere else.
  const e = metal ? engraved(silver) : null;
  const hdr = e
    ? { ...e, hdrValue: e.title, hdrAccent: e.title }
    : { title: Tt(0.92), titleMin: Tt(0.50), hdrMuted: Tt(0.85), hdrValue: Tt(1), hdrAccent: Tt(1), engrave: null };

  return {
    chassis, transparency, surface: metal ? 'metal' : 'glass',
    // ★★ Default ON = the RDS panel's glass, which the brief calls "today's glass" (0.72, BIG 0.62, iOS
    //   blur under SMALL); default OFF = that glass made opaque (solidBg). Silver / black ON: the same
    //   glass, never blurred (§10.2, §3.4 — a metal deck over a blurred spectrum would pay the one cost
    //   the opaque plate exists to avoid); OFF: the plate + recessed window.
    solidBg:  def && off ? DEFAULT_SOLID_BG : null,
    tint:     def && off ? 1 : DEFAULT_TINT,
    tintTall: def && off ? 1 : 0.62,
    blur:     def && !off ? 35 : 0,
    tintRgb:  DEFAULT_TINT_RGB,
    // ── Frame: the KEYS' colour, so the box reads as one with its keys. Metal OFF: the chassis edge.
    border:  metal ? (silver ? '#8b8983' : '#3a3c40') : K(0.40),
    glow:    def || metal ? null : K(0.18),   // the silver / black glass glow (mockup `0 0 8px L(.18)`)
    hdrBdr:  metal ? null : K(0.18),
    window:  metal ? { bg: '#070605', lip: silver ? 'rgba(255,255,255,0.80)' : 'rgba(255,255,255,0.14)',
                       inset: 8, radius: 6 } : null,
    ...hdr,
    // ── Body: the TEXT colour. Hierarchy by alpha and size — labels a step under the values.
    muted: Tt(0.85), label: Tt(0.92), rowLabel: Tt(0.85),
    value: Tt(1), accent: Tt(1), rowActive: Tt(0.12),
    selected: `rgb(${sel})`, selectedLive: `rgba(${sel},0.80)`, selectedBg: `rgba(${sel},0.14)`,
    // The teleprinter glow is the default glass's; the metal chassis never had it.
    outputGlow: def ? Tr(0.35) : 'rgba(0,0,0,0)',
    divider: Tr(0.10),
    // ── Keys: the CONTROLS colour. Default: today's outline key; silver / black: the cap (§5).
    ...(def
      ? { keyLook: 'outline' as const,
          keyBorder: K(0.35), keyBorderAct: K(0.65), keyBgAct: K(0.14),
          keyText: Kt(0.85), keyTextAct: Kt(1), keyGlow: null, keyShade: null,
          close: Kt(0.85), cap: null }
      : { keyLook: 'cap' as const,
          keyBorder: silver ? '#8d8a83' : '#050505', keyBorderAct: silver ? '#8d8a83' : '#050505',
          keyBgAct: 'transparent',
          // ★ Silver caps are LIGHT: lifting toward white would only fade the legend, so they take the
          //   pure colour (the mockup's `L(.9)` / `L(1)`) with the deck keys' dark shade under it (§5,
          //   FaceplateTheme keyLegend.shade). Black caps are dark, so they take the lifted colour.
          keyText:    silver ? K(0.90) : Kt(0.92),
          keyTextAct: silver ? K(1) : Kt(1),
          keyGlow:    K(0.55),
          keyShade:   silver ? 'rgba(0,0,0,0.6)' : null,
          close:      silver ? K(0.90) : Kt(0.90),
          cap: silver
            ? { base: '#c4c1ba', hi: 'rgba(255,255,255,0.40)', lo: 'rgba(0,0,0,0.10)', border: '#8d8a83',
                topLine: 'rgba(255,255,255,0.80)', cast: '#000000', castOpacity: 0.6, pressDim: 0.84 }
            : { base: '#111214', hi: 'rgba(255,255,255,0.05)', lo: 'rgba(0,0,0,0.25)', border: '#050505',
                topLine: 'rgba(255,255,255,0.22)', cast: '#000000', castOpacity: 0.85, pressDim: 0.82 } }),
    // Buttons inside the body are keys too.
    chipBorder: K(0.35), chipBorderAct: K(0.70), chipBgAct: K(0.14),
    dotIdle: K(0.35), focus: focusFor(controlsRgb),
    // ── Charts: read-outs, so the TEXT colour.
    plot: Tr(0.05), axis: Tr(0.25), axisStrong: Tr(0.35), axisFaint: Tr(0.07), chartText: Tt(0.85),
    dot: Tt(0.85), bar: Tt(0.70), trace: Tt(0.85),
    good: m.good, warn: m.warn, bad: m.bad, dotOn: m.dotOn, mer: m.mer, snrLow: m.snrLow, badBold: m.badBold,
  };
}

/** Unmemoised — the contrast test's `--find` sweep changes CONTRAST_LIFT under it. */
export function buildDecoderTokens(chassis: Chassis, controlsRgb: string, transparency: Transparency,
                                   textRgb: string = GREEN_TEXT_RGB): DecoderTokens {
  return tokens(chassis, controlsRgb, transparency, textRgb);
}

const cache = new Map<string, DecoderTokens>();

/**
 * ★ The resolver. Memoised on its inputs, so every panel on the screen shares ONE object per
 * setting and a style sheet built from it (DecoderShell `useDecoderStyles`) is built once, not per
 * render — a spectrum frame never touches it.
 */
export function decoderTokensFor(chassis: Chassis = 'default', controlsRgb = '61,255,114',
                                 transparency: Transparency = 'on', textRgb: string = GREEN_TEXT_RGB): DecoderTokens {
  const key = `${chassis}|${controlsRgb}|${transparency}|${textRgb}`;
  let t = cache.get(key);
  if (!t) {
    t = buildDecoderTokens(chassis, controlsRgb, transparency, textRgb);
    cache.set(key, t);
  }
  return t;
}
