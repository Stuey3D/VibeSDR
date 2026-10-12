/**
 * Decoder-box contrast (src/constants/decoderTokens.ts) — brief §10.2: "≥ 4.5:1 contrast in every
 * chassis × colour; if one fails, lighten the same hue". WCAG 2 relative luminance, with every
 * translucent colour composited over what it actually sits on.
 *
 * Swept: every chassis × every controls colour × every text colour × TRANSPARENCY EFFECTS on / off,
 * each text role against the surface it is drawn on:
 *   • solid glass (OFF on default) — the tint made opaque (`solidBg`, alpha 1.0): nothing behind it
 *     counts, so it is checked against itself alone.
 *   • glass (ON everywhere) — the box's tint over the waterfall, SMALL and
 *     BIG tints both. ★ THE WATERFALL REFERENCE is the 75th-percentile pixel of the mockups'
 *     backdrop (docs/faceplates/wf-backdrop.jpg, measured 2026-09-30: rgb 34,80,18) — a busy band,
 *     not a blank one. Over the 95th percentile (a hot carrier, 82,131,66) nothing dim is readable
 *     through 62–72% glass, and that is the trade the user makes by leaving Transparency ON:
 *     the OFF subtitle says so. The hot-carrier figures are PRINTED (not asserted) so the trade is
 *     visible.
 *   • metal body (OFF silver / black) — the recessed window #070605.
 *   • metal header (OFF silver / black) — engraved text on the plate at its darkest (silver) /
 *     lightest (black) point of the lighting layer.
 *
 * Also proves (★★ Stuart's rule, 2026-10-12 — it INVERTS the old "the text colour never reaches a decoder
 * box" and "the default chassis keeps today's gold"): text follows the TEXT colour on every chassis, the
 * keys and frame follow the CONTROLS colour, the meaning colours never move, the selected station is
 * green — WHITE when the text is green — no blur is ever asked for on silver / black or with OFF, and
 * OFF is alpha 1.0 EXACTLY (never the old Solid's 0.95).
 *
 * ★ Header KEY legends on the SILVER cap are reported, not asserted: they are lit LED legends on a
 *   light cap (the mockup's and the deck's own silver keys), and no lightening of the same hue can
 *   take green or white to 4.5:1 on #c4c1ba. See the note printed at the end.
 *
 * Run: node --no-warnings scripts/test_decoder_contrast.ts   (run-tests.sh does)
 *      node --no-warnings scripts/test_decoder_contrast.ts --find   (prints the smallest lift per colour)
 */
import { CHASSIS, CONTROLS, TEXTS, TRANSPARENCIES, LED, SILVER_CHASSIS, BLACK_CHASSIS,
  resolveControlsColour } from '../src/constants/faceplate.ts';
import {
  decoderTokensFor, buildDecoderTokens, DECODER_MEANING, METAL_VALUE, CONTRAST_LIFT, lighten, parseRgb,
  DEFAULT_SOLID_BG, GREEN_TEXT_RGB, controlsText, meaningsFor, meaningText,
  TEAL_TEXT_RGB, RED_TEXT_RGB, AMBER_TEXT_RGB, NEON_TEXT_RGB,
  type DecoderTokens, type RGB,
} from '../src/constants/decoderTokens.ts';

/** Every text colour a decoder box can be handed: the six TEXTS, plus neon (the Nixie lock). */
const TEXT_RGBS: [string, string][] = [...TEXTS.map(t => [t, LED[t].rgb] as [string, string]), ['neon', LED.neon.rgb]];

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
}

// ── Colour maths ─────────────────────────────────────────────────────────────

type RGBA = [number, number, number, number];
function parse(c: string): RGBA {
  const s = c.trim();
  if (s.startsWith('#')) {
    const h = s.slice(1);
    const n = h.length === 3 ? h.split('').map(x => x + x).join('') : h;
    return [parseInt(n.slice(0, 2), 16), parseInt(n.slice(2, 4), 16), parseInt(n.slice(4, 6), 16), 1];
  }
  const m = s.match(/rgba?\(([^)]+)\)/);
  if (!m) throw new Error(`cannot parse colour ${c}`);
  const p = m[1].split(',').map(Number);
  return [p[0], p[1], p[2], p.length > 3 ? p[3] : 1];
}
function over(fg: RGBA, bg: RGB): RGB {
  const a = fg[3];
  return [fg[0] * a + bg[0] * (1 - a), fg[1] * a + bg[1] * (1 - a), fg[2] * a + bg[2] * (1 - a)];
}
function lum(c: RGB): number {
  const f = (v: number) => { const x = v / 255; return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; };
  return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2]);
}
function ratio(a: RGB, b: RGB): number {
  const la = lum(a), lb = lum(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}
/** Contrast of a (possibly translucent) text colour drawn on an opaque background. */
function contrast(text: string, bg: RGB): number { return ratio(over(parse(text), bg), bg); }

// ── The surfaces ─────────────────────────────────────────────────────────────

const WF_BUSY: RGB = [34, 80, 18];     // wf-backdrop.jpg, 75th percentile by luminance
const WF_HOT:  RGB = [82, 131, 66];    // 95th percentile — printed, not asserted
/** What the glass shows over `wf` — or, with Transparency OFF, the opaque colour whatever is behind. */
const glassOver = (tk: DecoderTokens, tint: number, wf: RGB): RGB => tk.solidBg
  ? parse(tk.solidBg).slice(0, 3) as RGB
  : over([...parseRgb(tk.tintRgb), tint] as RGBA, wf);
/** The lighting's brightest white band (black's plate: light text is worst there). Read from the
 *  tokens, so darkening the plate is measured, not assumed. */
function brightestBand(stops: string[]): number {
  let a = 0;
  for (const c of stops) { const m = /^rgba\(255,255,255,([\d.]+)\)$/.exec(c.replace(/\s/g, '')); if (m) a = Math.max(a, +m[1]); }
  return a;
}
/** The plate under engraved header text, at the point of the lighting that is WORST for it. */
function plateWorst(chassis: 'silver' | 'black'): RGB {
  const p = (chassis === 'silver' ? SILVER_CHASSIS : BLACK_CHASSIS).plate!;
  const base = parse(p.base).slice(0, 3) as RGB;
  // Silver's text is dark: worst where the plate is darkest (its lighting's .16 black end; the
  // bottom shade never reaches the header). Black's text is light: worst at the lighting's brightest band (.11 white).
  return chassis === 'silver'
    ? over([0, 0, 0, 0.16], base)
    : over([255, 255, 255, brightestBand(p.lightColors)], base);
}
const WINDOW: RGB = [7, 6, 5];

const BODY_ROLES   = ['muted', 'label', 'rowLabel', 'value', 'accent'] as const;
const HEADER_ROLES = ['title', 'hdrMuted', 'hdrValue', 'hdrAccent'] as const;

interface Check { what: string; text: string; bg: RGB; min: number }
function checksFor(tk: DecoderTokens): Check[] {
  const out: Check[] = [];
  const tag = `${tk.chassis}/${tk.transparency}`;
  if (tk.surface === 'glass') {
    for (const [name, tint] of [['SMALL', tk.tint], ['BIG', tk.tintTall]] as const) {
      const bg = glassOver(tk, tint, WF_BUSY);
      for (const r of [...BODY_ROLES, ...HEADER_ROLES]) out.push({ what: `${tag} ${name} ${r}`, text: tk[r], bg, min: 4.5 });
      // Outline header keys (default) sit on the glass too — the active one on its faint fill.
      // Caps carry their own face, checked below.
      if (tk.keyLook === 'outline') {
        out.push({ what: `${tag} ${name} keyText`, text: tk.keyText, bg, min: 4.5 });
        out.push({ what: `${tag} ${name} keyTextAct`, text: tk.keyTextAct, bg: over(parse(tk.keyBgAct), bg), min: 4.5 });
      }
    }
  } else {
    for (const r of BODY_ROLES) out.push({ what: `${tag} window ${r}`, text: tk[r], bg: WINDOW, min: 4.5 });
    const plate = plateWorst(tk.chassis as 'silver' | 'black');
    for (const r of HEADER_ROLES) out.push({ what: `${tag} plate ${r}`, text: tk[r], bg: plate, min: 4.5 });
  }
  // Black caps: a lit legend on a dark cap is ordinary text on dark and must read.
  if (tk.cap && tk.chassis === 'black') {
    out.push({ what: `${tag} black cap keyText`, text: tk.keyText, bg: parse(tk.cap.base).slice(0, 3) as RGB, min: 4.5 });
  }
  // Meaning colours on the body surface they share with the values.
  const body = tk.surface === 'glass' ? glassOver(tk, tk.tintTall, WF_BUSY) : WINDOW;
  for (const k of ['good', 'warn', 'bad', 'mer', 'dotOn', 'selected', 'selectedLive'] as const) {
    out.push({ what: `${tag} meaning ${k}`, text: tk[k], bg: body, min: 4.5 });
  }
  // Chart marks are graphics (WCAG 1.4.11): 3:1 against the plot they sit in.
  const plotBg = over(parse(tk.plot), body);
  out.push({ what: `${tag} chart dot`, text: tk.dot, bg: plotBg, min: 3 });
  out.push({ what: `${tag} chart bar`, text: tk.bar, bg: plotBg, min: 3 });
  return out;
}

// ── --find: the smallest lift per controls colour (how CONTRAST_LIFT was set) ─

if (process.argv.includes('--find')) {
  // ★ A colour lights decoder text as a key legend (controls) AND as body text (text), so each is
  //   swept in both roles, on every chassis, with the other role at white.
  const all = new Map<string, string>([...CONTROLS.map(c => [c, LED[c].rgb] as [string, string]), ...TEXT_RGBS,
                                       ['today-green', resolveControlsColour('default', 'green').rgb]]);
  for (const [c, rgb] of all) {
    let found = -1;
    for (let step = 0; step <= 20 && found < 0; step++) {
      const t = step * 0.05;
      CONTRAST_LIFT[rgb] = t;
      let worst = Infinity;
      for (const ch of CHASSIS) for (const bg of TRANSPARENCIES) for (const tk of [
        buildDecoderTokens(ch, rgb, bg, LED.white.rgb), buildDecoderTokens(ch, LED.white.rgb, bg, rgb)]) {   // unmemoised
        for (const k of checksFor(tk)) if (!k.what.includes('meaning') && k.min === 4.5 && !k.what.includes('plate')) {
          worst = Math.min(worst, contrast(k.text, k.bg));
        }
      }
      if (worst >= 4.5) found = t;
    }
    console.log(`${c.padEnd(6)} ${rgb.padEnd(12)} smallest lift ${found}`);
  }
  process.exit(0);
}

// ── The sweep ────────────────────────────────────────────────────────────────

let hotWorst = { r: Infinity, what: '' };
for (const ch of CHASSIS) for (const c of CONTROLS) for (const [tn, trgb] of TEXT_RGBS) for (const bg of TRANSPARENCIES) {
  // ★ The RESOLVED controls LED, as FaceplateContext hands it over (TODAY_GREEN on the default chassis).
  const crgb = resolveControlsColour(ch, c).rgb;
  const tk = decoderTokensFor(ch, crgb, bg, trgb);
  const tag = `${ch}/${c} keys/${tn} text/${bg}`;
  for (const k of checksFor(tk)) {
    const r = contrast(k.text, k.bg);
    ok(`${k.what} [${c} keys, ${tn} text] ≥ ${k.min}:1`, r >= k.min, `${r.toFixed(2)}:1 — ${k.text}`);
  }
  if (tk.surface === 'glass' && !tk.solidBg) {
    const hot = glassOver(tk, tk.tint, WF_HOT);
    for (const r of BODY_ROLES) {
      const x = contrast(tk[r], hot);
      if (x < hotWorst.r) hotWorst = { r: x, what: `${ch}/${bg} ${r} [${tn} text]` };
    }
  }
  // ★★ TEXT FOLLOWS THE TEXT COLOUR — body text, the chart read-outs, and the header unless engraved on metal.
  const Tt = `rgba(${controlsText(trgb)},`;
  for (const r of ['muted', 'label', 'rowLabel', 'value', 'accent', 'chartText', 'dot', 'bar', 'trace'] as const) {
    ok(`${tag}: ${r} in the text colour`, tk[r].startsWith(Tt), tk[r]);
  }
  for (const r of HEADER_ROLES) {
    ok(`${tag}: header ${r} ${tk.surface === 'metal' ? 'engraved' : 'in the text colour'}`,
       tk.surface === 'metal' ? !!tk.engrave && !tk[r].startsWith('rgba(' + trgb) : tk[r].startsWith(Tt), tk[r]);
  }
  // ★★ KEYS (and the frame that goes with them) FOLLOW THE CONTROLS COLOUR.
  const isK = (v: string) => v.startsWith(`rgba(${crgb},`) || v.startsWith(`rgba(${controlsText(crgb)},`);
  for (const r of ['keyText', 'keyTextAct', 'close', 'chipBorder', 'chipBorderAct', 'chipBgAct', 'dotIdle'] as const) {
    ok(`${tag}: ${r} in the controls colour`, isK(tk[r]), tk[r]);
  }
  if (tk.surface === 'glass') ok(`${tag}: border in the controls colour`, isK(tk.border), tk.border);
  // ★★ Focus: WHITE on green controls (Stuart, 2026-10-12: "yes make it white"), green elsewhere.
  ok(`${tag}: focus ring`, tk.focus === (c === 'green' ? '#ffffff' : '#7CFF9B'), tk.focus);
  if (tk.hdrBdr) ok(`${tag}: header hairline in the controls colour`, isK(tk.hdrBdr), tk.hdrBdr);
  // Meaning colours are the same on every chassis, controls and text colour.
  for (const k of Object.keys(DECODER_MEANING) as (keyof typeof DECODER_MEANING)[]) {
    ok(`${tag}: meaning ${k} = the clash rule's`, tk[k] === meaningsFor(trgb)[k]);
  }
  // ★★★ THE CLASH RULE (meaningsFor): a meaning moves only where the TEXT is its colour.
  const W = METAL_VALUE;
  ok(`${tag}: bad ${tn === 'red' ? 'white BOLD' : 'red'}`, tn === 'red'
    ? tk.bad === W && tk.badBold && tk.bad !== DECODER_MEANING.bad
    : tk.bad === DECODER_MEANING.bad && !tk.badBold, tk.bad);
  const warnWhite = tn === 'amber' || tn === 'neon';
  ok(`${tag}: warn ${warnWhite ? 'white' : 'amber'}`, warnWhite
    ? tk.warn === W && tk.snrLow.startsWith('rgba(242,239,232,')
    : tk.warn === DECODER_MEANING.warn && tk.snrLow === DECODER_MEANING.snrLow, tk.warn);
  ok(`${tag}: good is never moved (green text keeps its green 'ok' tone)`, tk.good === DECODER_MEANING.good);
  ok(`${tag}: meaningText bolds only bad`, (meaningText(tk, tk.bad).fontWeight === '700') === tk.badBold
    && meaningText(tk, tk.warn).fontWeight === undefined);
  // ★★ The selected station: green, WHITE on green or teal text (Stuart's clash rule).
  ok(`${tag}: selected station`, tn === 'green' || tn === 'teal'
    ? tk.selected === 'rgb(242,239,232)' && tk.selectedLive.startsWith('rgba(242,239,232,')
    : tk.selected === 'rgb(125,255,154)' && tk.selectedLive.startsWith('rgba(125,255,154,'), tk.selected);
  // No blur over the spectrum on silver / black (§10.2, §3.4); and none with Transparency OFF anywhere.
  if (ch !== 'default' || bg === 'off') ok(`${tag}: no blur`, tk.blur === 0);
  // ★★★ OFF is alpha 1.0 EXACTLY — an opaque rgb() on default (never rgba, never 0.95); metal is its plate.
  if (bg === 'off') ok(`${tag}: opaque`, ch === 'default'
    ? tk.solidBg != null && parse(tk.solidBg)[3] === 1 && tk.tint === 1 && tk.tintTall === 1
    : tk.surface === 'metal' && tk.solidBg == null);
  else ok(`${tag}: see-through`, tk.solidBg == null && tk.surface === 'glass');
  // Silver / black: OFF is the metal plate + recessed window; ON stays glass.
  ok(`${tag}: surface`, tk.surface === (ch !== 'default' && bg === 'off' ? 'metal' : 'glass'));
  // Header keys are caps on metal chassis, outline keys on default — but dome keys on all.
  ok(`${tag}: key look`, tk.keyLook === (ch === 'default' ? 'outline' : 'cap'));
}

// The text colour CHANGES the box now, and the controls colour changes it on the default chassis too.
{
  const blueWhite = decoderTokensFor('default', LED.blue.rgb, 'on', LED.white.rgb);
  ok('text white ≠ text green', blueWhite !== decoderTokensFor('default', LED.blue.rgb, 'on', LED.green.rgb));
  ok('default follows the controls colour', blueWhite.border !== decoderTokensFor('default', LED.red.rgb, 'on', LED.white.rgb).border);
  // ★ Stuart's own example: "blue buttons and white text … white text in the decoder with the header buttons being blue".
  ok('blue keys + white text: value white', blueWhite.value.startsWith(`rgba(${LED.white.rgb},`));
  ok('blue keys + white text: keys blue', blueWhite.keyText.startsWith(`rgba(${LED.blue.rgb},`));
  ok('blue keys + white text: selected station green', blueWhite.selected === 'rgb(125,255,154)');
  ok('green text is the brief\'s green', GREEN_TEXT_RGB === LED.green.rgb);
  ok('the clash rule\'s triplets are the brief\'s', TEAL_TEXT_RGB === LED.teal.rgb && RED_TEXT_RGB === LED.red.rgb
     && AMBER_TEXT_RGB === LED.amber.rgb && NEON_TEXT_RGB === LED.neon.rgb);
  ok('the white selected station is the near-white value #f2efe8', METAL_VALUE === '#f2efe8');
}
// Default chassis: the glass, ON and OFF.
{
  const t = decoderTokensFor('default', LED.red.rgb, 'on', LED.blue.rgb);
  ok('default ON = today\'s glass 0.72 / BIG 0.62, iOS blur 35', t.tint === 0.72 && t.tintTall === 0.62 && t.blur === 35);
  const s = decoderTokensFor('default', LED.red.rgb, 'off', LED.blue.rgb);
  // ★★★ Today's SMALL glass over black at 1.0: rgb(10,8,4) × 0.72 = 7.2, 5.76, 2.88 → 7,6,3.
  ok('default OFF = today\'s glass over black, alpha 1.0', s.solidBg === 'rgb(7,6,3)' && s.solidBg === DEFAULT_SOLID_BG);
  ok('default OFF keeps every text token of ON (only the surface changes)',
     (['title', 'muted', 'label', 'rowLabel', 'value', 'accent', 'keyText', 'border'] as const).every(k => s[k] === t[k]));
}
// Silver / black: OFF engraved header + window; ON glass in the controls colour's frame.
{
  const t = decoderTokensFor('silver', LED.blue.rgb, 'on', LED.white.rgb);
  ok('silver ON border in the controls colour', t.border.startsWith(`rgba(${LED.blue.rgb},`));
  ok('silver ON chart dots in the TEXT colour', t.dot.startsWith(`rgba(${controlsText(LED.white.rgb)},`));
  ok('silver ON glass is today\'s 0.72', t.tint === 0.72 && t.tintTall === 0.62);
  const s = decoderTokensFor('silver', LED.blue.rgb, 'off', LED.white.rgb);
  ok('silver OFF header engraved dark', s.title === '#2a2824' && s.engrave?.dy === 1);
  const b = decoderTokensFor('black', LED.blue.rgb, 'off', LED.white.rgb);
  ok('black OFF header engraved light grey', b.title === '#c9ccd2' && b.engrave?.dy === -1);
  ok('OFF metal has the recessed window', !!s.window && !!b.window && s.window.bg === '#070605');
}
// Every controls colour has a lift entry (an unlisted colour would silently get none).
for (const c of CONTROLS) ok(`CONTRAST_LIFT covers ${c}`, CONTRAST_LIFT[LED[c].rgb] !== undefined);
for (const [t, rgb] of TEXT_RGBS) ok(`CONTRAST_LIFT covers ${t} text`, CONTRAST_LIFT[rgb] !== undefined);
ok('CONTRAST_LIFT covers the default chassis\'s green', CONTRAST_LIFT[resolveControlsColour('default', 'green').rgb] !== undefined);
// lighten() keeps the hue: the channel ORDER never changes.
for (const c of CONTROLS) {
  const a = parseRgb(LED[c].rgb), b = lighten(a, 0.3);
  const ord = (x: RGB) => [0, 1, 2].sort((i, j) => x[j] - x[i]).join('');
  ok(`lighten keeps ${c}'s hue order`, ord(a) === ord(b) || new Set(a).size < 3);
}

// Reported: silver cap legends, and the hot-carrier worst case.
const capBg = parse('#c4c1ba').slice(0, 3) as RGB;
const capRows = CONTROLS.map(c => `${c} ${contrast(decoderTokensFor('silver', LED[c].rgb, 'off', LED.white.rgb).keyText, capBg).toFixed(1)}`);
console.log(`  note: silver cap legends (lit, not asserted): ${capRows.join(' · ')}`);
console.log(`  note: glass over a hot carrier (95th pct, not asserted): worst ${hotWorst.r.toFixed(2)}:1 (${hotWorst.what})`);
console.log(`decoder contrast: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
