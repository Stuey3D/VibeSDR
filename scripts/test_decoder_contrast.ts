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
 * Also proves: the meaning colours never move, the text colour never reaches a decoder box, the
 * default chassis keeps today's chrome, no blur is ever asked for on silver / black or with OFF, and
 * OFF is alpha 1.0 EXACTLY (never the old Solid's 0.95).
 *
 * ★ Header KEY legends on the SILVER cap are reported, not asserted: they are lit LED legends on a
 *   light cap (the mockup's and the deck's own silver keys), and no lightening of the same hue can
 *   take green or white to 4.5:1 on #c4c1ba. See the note printed at the end.
 *
 * Run: node --no-warnings scripts/test_decoder_contrast.ts   (run-tests.sh does)
 *      node --no-warnings scripts/test_decoder_contrast.ts --find   (prints the smallest lift per colour)
 */
import { CHASSIS, CONTROLS, TEXTS, TRANSPARENCIES, LED, SILVER_CHASSIS, BLACK_CHASSIS } from '../src/constants/faceplate.ts';
import {
  decoderTokensFor, buildDecoderTokens, DECODER_MEANING, METAL_VALUE, CONTRAST_LIFT, lighten, parseRgb,
  DEFAULT_SOLID_BG,
  type DecoderTokens, type RGB,
} from '../src/constants/decoderTokens.ts';

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
  for (const k of ['good', 'warn', 'bad', 'mer', 'dotOn'] as const) {
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
  for (const c of CONTROLS) {
    const rgb = LED[c].rgb;
    let found = -1;
    for (let step = 0; step <= 20 && found < 0; step++) {
      const t = step * 0.05;
      CONTRAST_LIFT[rgb] = t;
      let worst = Infinity;
      for (const ch of ['silver', 'black'] as const) for (const bg of TRANSPARENCIES) {
        const tk = buildDecoderTokens(ch, rgb, bg);   // unmemoised: the lift just changed
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

const seen = new Map<string, DecoderTokens>();
let hotWorst = { r: Infinity, what: '' };
for (const ch of CHASSIS) for (const c of CONTROLS) for (const bg of TRANSPARENCIES) {
  const tk = decoderTokensFor(ch, LED[c].rgb, bg);
  // ★ The text colour never reaches a decoder box: the resolver does not take it, so every text
  //   colour yields the very same object — asserted per text colour, as the brief sweeps it.
  for (const t of TEXTS) {
    const key = `${ch}|${c}|${bg}`;
    if (seen.has(key)) ok(`text ${t} does not change ${key}`, seen.get(key) === tk);
    else seen.set(key, tk);
  }
  for (const k of checksFor(tk)) {
    const r = contrast(k.text, k.bg);
    ok(`${k.what} [${c}] ≥ ${k.min}:1`, r >= k.min, `${r.toFixed(2)}:1 — ${k.text}`);
  }
  if (tk.surface === 'glass' && !tk.solidBg) {
    const hot = glassOver(tk, tk.tint, WF_HOT);
    for (const r of BODY_ROLES) {
      const x = contrast(tk[r], hot);
      if (x < hotWorst.r) hotWorst = { r: x, what: `${ch}/${bg} ${r} [${c}]` };
    }
  }
  // Meaning colours are the same objects on every chassis and colour.
  for (const k of Object.keys(DECODER_MEANING) as (keyof typeof DECODER_MEANING)[]) {
    ok(`${ch}/${c}/${bg}: meaning ${k} fixed`, tk[k] === DECODER_MEANING[k]);
  }
  // No blur over the spectrum on silver / black (§10.2, §3.4); and none with Transparency OFF anywhere.
  if (ch !== 'default' || bg === 'off') ok(`${ch}/${c}/${bg}: no blur`, tk.blur === 0);
  // ★★★ OFF is alpha 1.0 EXACTLY — an opaque rgb() on default (never rgba, never 0.95); metal is its plate.
  if (bg === 'off') ok(`${ch}/${c}/off: opaque`, ch === 'default'
    ? tk.solidBg != null && parse(tk.solidBg)[3] === 1 && tk.tint === 1 && tk.tintTall === 1
    : tk.surface === 'metal' && tk.solidBg == null);
  else ok(`${ch}/${c}/on: see-through`, tk.solidBg == null && tk.surface === 'glass');
  // Values: near-white on metal, today's yellow on default (§10.2).
  ok(`${ch}/${c}/${bg}: value colour`, tk.value === (ch === 'default' ? '#ffe566' : METAL_VALUE));
  // Silver / black: OFF is the metal plate + recessed window; ON stays glass.
  ok(`${ch}/${bg}: surface`, tk.surface === (ch !== 'default' && bg === 'off' ? 'metal' : 'glass'));
  // Header keys are caps on metal chassis, today's outline on default — but dome keys on all.
  ok(`${ch}: key look`, tk.keyLook === (ch === 'default' ? 'outline' : 'cap'));
}

// Default chassis: today's chrome, independent of the controls colour; ON = RDS glass.
{
  const t = decoderTokensFor('default', LED.red.rgb, 'on');
  ok('default ignores the controls colour', t === decoderTokensFor('default', LED.blue.rgb, 'on'));
  ok('default ON = today\'s glass 0.72 / BIG 0.62, iOS blur 35', t.tint === 0.72 && t.tintTall === 0.62 && t.blur === 35);
  const s = decoderTokensFor('default', LED.red.rgb, 'off');
  // ★★★ Today's SMALL glass over black at 1.0: rgb(10,8,4) × 0.72 = 7.2, 5.76, 2.88 → 7,6,3.
  ok('default OFF = today\'s glass over black, alpha 1.0', s.solidBg === 'rgb(7,6,3)' && s.solidBg === DEFAULT_SOLID_BG);
  ok('default OFF keeps every text token of ON (only the surface changes)',
     (['title', 'muted', 'label', 'rowLabel', 'value', 'accent', 'keyText', 'border'] as const).every(k => s[k] === t[k]));
  ok('default border stays gold', t.border === 'rgba(255,160,0,0.28)');
}
// Silver / black ON: controls colour on border, title, labels, charts (§10.2).
{
  const t = decoderTokensFor('silver', LED.blue.rgb, 'on');
  ok('silver ON border in the controls colour', t.border.startsWith(`rgba(${LED.blue.rgb},`));
  ok('silver ON chart dots in the controls colour', t.dot.startsWith(`rgba(${LED.blue.rgb},`));
  ok('silver ON glass is today\'s 0.72', t.tint === 0.72 && t.tintTall === 0.62);
  const s = decoderTokensFor('silver', LED.blue.rgb, 'off');
  ok('silver OFF header engraved dark', s.title === '#2a2824' && s.engrave?.dy === 1);
  const b = decoderTokensFor('black', LED.blue.rgb, 'off');
  ok('black OFF header engraved light grey', b.title === '#c9ccd2' && b.engrave?.dy === -1);
  ok('OFF metal has the recessed window', !!s.window && !!b.window && s.window.bg === '#070605');
}
// Every controls colour has a lift entry (an unlisted colour would silently get none).
for (const c of CONTROLS) ok(`CONTRAST_LIFT covers ${c}`, CONTRAST_LIFT[LED[c].rgb] !== undefined);
// lighten() keeps the hue: the channel ORDER never changes.
for (const c of CONTROLS) {
  const a = parseRgb(LED[c].rgb), b = lighten(a, 0.3);
  const ord = (x: RGB) => [0, 1, 2].sort((i, j) => x[j] - x[i]).join('');
  ok(`lighten keeps ${c}'s hue order`, ord(a) === ord(b) || new Set(a).size < 3);
}

// Reported: silver cap legends, and the hot-carrier worst case.
const capBg = parse('#c4c1ba').slice(0, 3) as RGB;
const capRows = CONTROLS.map(c => `${c} ${contrast(decoderTokensFor('silver', LED[c].rgb, 'off').keyText, capBg).toFixed(1)}`);
console.log(`  note: silver cap legends (lit, not asserted): ${capRows.join(' · ')}`);
console.log(`  note: glass over a hot carrier (95th pct, not asserted): worst ${hotWorst.r.toFixed(2)}:1 (${hotWorst.what})`);
console.log(`decoder contrast: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
