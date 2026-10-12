/**
 * "Use as server" in the user's colours (Stuart, 2026-10-12: "Follow the text colour except for when the colours have
 * a meaning … yes colours apply everywhere") — src/constants/serverPalette.ts, ServerModeScreen and the editors only it
 * draws, plus SDRScreen's still-listening card, idle-terms pill and listener chip.
 *
 * Proves:
 *   • the roles: text roles are the TEXT colour, key roles the CONTROLS colour, meanings by the clash rule;
 *   • CONTRAST: every text and key role ≥ 4.5:1 on the surface it sits on, swept over every controls × text × display
 *     (the RESOLVED text colour — neon under Nixie), and for VibeServer Lite's fixed amber tokens;
 *   • the screen and its editors hold no old gold / amber / hard-coded meaning literal any more;
 *   • ★★ LITE STAYS BUNDLEABLE: ServerModeScreen and its editors never import PopupShell / FaceplateContext (Lite runs
 *     RN 0.73 with no Reanimated); the main app's route goes through ServerModeThemed.
 *
 * Run: node --no-warnings scripts/test_server_colours.ts
 */
import { readFileSync } from 'node:fs';
import { CONTROLS, TEXTS, DISPLAYS, LED, resolveTextColour } from '../src/constants/faceplate.ts';
import { popupTokensFor } from '../src/constants/popupTokens.ts';
import { serverPalette, LITE_TOKENS, SERVER_BG, SERVING_GREEN, STOP_RED, type ServerPalette } from '../src/constants/serverPalette.ts';
import { meaningsFor } from '../src/constants/decoderTokens.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
}

// ── Colour maths (test_popup's) ──────────────────────────────────────────────
type RGB = [number, number, number];
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
const over = (fg: RGBA, bg: RGB): RGB => [0, 1, 2].map(i => fg[i] * fg[3] + bg[i] * (1 - fg[3])) as RGB;
function lum(c: RGB): number {
  const f = (v: number) => { const x = v / 255; return x <= 0.04045 ? x / 12.92 : ((x + 0.055) / 1.055) ** 2.4; };
  return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2]);
}
const ratio = (a: RGB, b: RGB) => { const la = lum(a), lb = lum(b); return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05); };
const contrast = (text: string, bg: RGB) => ratio(over(parse(text), bg), bg);
const rgb = (c: string) => parse(c).slice(0, 3) as RGB;
const on = (fill: string, bg: RGB) => over(parse(fill), bg);

const src = (f: string) => readFileSync(new URL(`../${f}`, import.meta.url), 'utf8');
const code = (f: string) => src(f).split('\n').filter(l => !/^\s*(\*|\/\/|\/\*|\{\/\*)/.test(l)).join('\n');

// ── 1. Roles (Stuart's example: blue keys, white text) ───────────────────────
{
  const C = serverPalette(popupTokensFor('default', LED.blue.rgb, LED.white.rgb, 'hyper'));
  const W = `rgba(${LED.white.rgb},`, B = `rgba(${LED.blue.rgb},`;
  for (const k of ['text', 'heading', 'section', 'textDim', 'textOnSel', 'placeholder'] as const)
    ok(`blue/white: ${k} is the text colour`, C[k].startsWith(W), C[k]);
  for (const k of ['key', 'keyDim', 'keyAct', 'keyBorder', 'keyBorderAct', 'keyBgAct', 'border', 'borderBright',
                   'trackOff', 'trackOn', 'thumb', 'fill'] as const)
    ok(`blue/white: ${k} is the controls colour`, C[k].startsWith(B), C[k]);
  const m = meaningsFor(LED.white.rgb);
  ok('meanings come from the clash helper', C.good === m.good && C.warn === m.warn && C.bad === m.bad);
  ok('START / STOP keep the directory\'s serving green and the stop red', C.ok === SERVING_GREEN && C.danger === STOP_RED);
  // The clash rule reaches this screen: red text makes `bad` white bold, amber text makes `warn` white,
  // green text makes the selected landing station white.
  const R = serverPalette(popupTokensFor('default', LED.blue.rgb, LED.red.rgb, 'hyper'));
  ok('red text: errors white bold', R.bad === meaningsFor(LED.red.rgb).bad && R.m.badBold);
  const A = serverPalette(popupTokensFor('default', LED.blue.rgb, LED.amber.rgb, 'hyper'));
  ok('amber text: warnings white', A.warn === meaningsFor(LED.amber.rgb).warn && A.warn !== C.warn);
  const G = serverPalette(popupTokensFor('default', LED.blue.rgb, LED.green.rgb, 'hyper'));
  ok('green text: the selected landing station is not green', !/125,255,154/.test(G.selected), G.selected);
  // theme.ts literals this module mirrors without importing.
  const theme = src('src/constants/theme.ts');
  ok('SERVER_BG = Colors.bg', theme.includes(`bg:           '${SERVER_BG}'`));
  ok('SERVING_GREEN = Colors.green', theme.includes(`green:        '${SERVING_GREEN}'`));
  ok('STOP_RED = Colors.red', theme.includes(`red:          '${STOP_RED}'`));
  ok('okBg is the serving green', parse(C.okBg).slice(0, 3).join() === rgb(SERVING_GREEN).join());
  ok('dangerBg is the stop red', parse(C.dangerBg).slice(0, 3).join() === rgb(STOP_RED).join());
}

// ── 2. Contrast: the server screen ───────────────────────────────────────────
/** Every text and key role on the surface it is drawn on (all on the near-black screen; fills composited over it). */
function serverChecks(tag: string, C: ServerPalette) {
  const BG = rgb(C.bg);
  const plain: Record<string, string> = {
    text: C.text, heading: C.heading, section: C.section, textDim: C.textDim, placeholder: C.placeholder,
    key: C.key, keyDim: C.keyDim, keyAct: C.keyAct, good: C.good, warn: C.warn, bad: C.bad,
    ok: C.ok, danger: C.danger, selected: C.selected,
  };
  const r: Array<[string, string, RGB]> = Object.entries(plain).map(([k, v]) => [k, v, BG]);
  r.push(['keyAct on its fill', C.keyAct, on(C.keyBgAct, BG)]);
  r.push(['hint inside a selected card', C.textOnSel, on(C.keyBgAct, BG)]);
  r.push(['ok on the START fill', C.ok, on(C.okBg, BG)]);
  r.push(['danger on the refused-START fill', C.danger, on(C.dangerBg, BG)]);
  r.push(['selected station on its fill', C.selected, on(C.selectedBg, BG)]);
  for (const [k, v, bg] of r) {
    const x = contrast(v, bg);
    ok(`server ${k} [${tag}] ${x.toFixed(2)}:1 ≥ 4.5`, x >= 4.5, v);
  }
}

// ── 3. Contrast: SDRScreen's still-listening card and idle-terms pill ─────────
const WF_BUSY: RGB = [34, 80, 18];    // test_popup's busy waterfall
const WHITE: RGB = [255, 255, 255];   // the worst a near-opaque card can sit over
let chipWorst = { r: Infinity, what: '' };
function sdrChecks(tag: string, pt: ReturnType<typeof popupTokensFor>) {
  for (const [under, base] of [['waterfall', WF_BUSY], ['white', WHITE]] as const) {
    const card = on('rgba(10,8,4,0.96)', base as RGB);
    const pill = on('rgba(14,10,2,0.92)', base as RGB);
    const rows: Array<[string, string, RGB]> = [
      ['still-here title (busy)', pt.ink.busy, card],
      ['still-here body (text)', pt.ink.text, card],
      ['still-here key (YES)', pt.gold.sel, on(pt.ink.keyBgAct, card)],
      ['idle-terms pill (busy)', pt.ink.busy, pill],
    ];
    for (const [k, v, bg] of rows) {
      const x = contrast(v, bg);
      ok(`sdr ${k} over ${under} [${tag}] ${x.toFixed(2)}:1 ≥ 4.5`, x >= 4.5, v);
    }
  }
  // The listener chip: the clock's fill (rgba(8,6,2,.72)) over the live waterfall. Asserted over the busy waterfall;
  // over a WHITE-hot waterfall (a .72 fill cannot guarantee that) it is printed, as test_popup prints its sheets.
  const chipX = contrast(pt.ink.textSoft, on('rgba(8,6,2,0.72)', WF_BUSY));
  ok(`sdr listener chip over waterfall [${tag}] ${chipX.toFixed(2)}:1 ≥ 4.5`, chipX >= 4.5, pt.ink.textSoft);
  const x = contrast(pt.ink.textSoft, on('rgba(8,6,2,0.72)', WHITE));
  if (x < chipWorst.r) chipWorst = { r: x, what: tag };
}

const CONTROL_RGBS: Array<[string, string]> = [...CONTROLS.map(c => [c, LED[c].rgb] as [string, string]),
                                               ['today-green', '0,230,0']];
let n = 0;
for (const [cn, crgb] of CONTROL_RGBS) for (const disp of DISPLAYS) for (const t of TEXTS) {
  const text = resolveTextColour(disp, t);
  const pt = popupTokensFor('default', crgb, text.rgb, disp);
  const tag = `${cn} keys, ${disp}/${text.name} text`;
  serverChecks(tag, serverPalette(pt));
  sdrChecks(tag, pt);
  n++;
}
serverChecks('Lite (amber/amber)', serverPalette(LITE_TOKENS));
ok('no tokens = Lite', serverPalette() === serverPalette(LITE_TOKENS));
console.log(`  ${n} controls × display × text combinations + Lite`);
console.log(`  note: listener chip over a WHITE-hot waterfall (not asserted): worst ${chipWorst.r.toFixed(2)}:1 (${chipWorst.what})`);

// ── 4. Source: no old colours left; Lite stays bundleable ─────────────────────
const FILES = ['src/screens/ServerModeScreen.tsx', 'src/components/BandLimitEditor.tsx', 'src/components/AntennaBandsEditor.tsx',
               'src/components/AntennaSwitchSetup.tsx', 'src/components/LiteUpdate.tsx'];
const OLD = /\bC\.(amber|gold|goldDim|green|red)\b|#ff5c5c|#ff8a7d|#ffcc66|#7bd88f|#ffb833|255,\s*184,\s*51|255,\s*160,\s*0|\+ '[0-9a-f]{2}'|C\.[a-z]+\}[0-9a-f]{2}`/i;
for (const f of FILES) {
  const hits = code(f).split('\n').filter(l => OLD.test(l));
  ok(`${f}: no old gold / amber / literal meaning (${hits.length})`, hits.length === 0, hits.join('\n'));
  ok(`${f}: no PopupShell / FaceplateContext import (Lite has no Reanimated)`,
     !/from '[^']*(PopupShell|FaceplateContext)'/.test(src(f)));
}
ok('ServerModeScreen reads serverPalette', /serverPalette\(tokens\)/.test(src('src/screens/ServerModeScreen.tsx')));
ok('ServerModeThemed passes useGlassTheme', /useGlassTheme\(\)/.test(src('src/screens/ServerModeThemed.tsx')));
ok('App.tsx routes ServerMode through ServerModeThemed', /from '\.\/src\/screens\/ServerModeThemed'/.test(src('App.tsx')));
const sdr = src('src/screens/SDRScreen.tsx');
for (const k of ['stillHereCard', 'stillHereTitle', 'stillHereBody', 'stillHereBtn', 'stillHereBtnTxt', 'rxListeners', 'rxListenersTxt']) {
  const m = sdr.match(new RegExp(`\\n  ${k}: \\{[\\s\\S]*?\\n?\\s*\\},`));
  ok(`SDRScreen ${k}: no literal colour in the style`, !!m && !/(?<!background)Color: '|\bcolor: '/.test(m[0].replace(/shadowColor: '#000'/, '')), m?.[0]);
}

console.log(`test_server_colours: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
