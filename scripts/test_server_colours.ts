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
import { readFileSync, existsSync, statSync } from 'node:fs';
import path from 'node:path';
import { CONTROLS, TEXTS, DISPLAYS, LED, resolveTextColour } from '../src/constants/faceplate.ts';
import { popupTokensFor } from '../src/constants/popupTokens.ts';
import { serverPalette, LITE, LITE_TOKENS, SERVER_BG, SERVING_GREEN, STOP_RED, type ServerPalette } from '../src/constants/serverPalette.ts';
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
  for (const k of ['text', 'heading', 'section', 'textDim', 'textMuted', 'textOnSel', 'placeholder', 'placeholderDim'] as const)
    ok(`blue/white: ${k} is the text colour`, C[k].startsWith(W), C[k]);
  for (const k of ['key', 'keyQuiet', 'keyDim', 'keyRemove', 'keyAct', 'keySel', 'keyBorder', 'keyBorderAct', 'keySelBorder',
                   'keyBgAct', 'keyBgCard', 'keyBgStart', 'keySelBg', 'border', 'borderBright',
                   'trackOff', 'trackOn', 'thumb', 'fill', 'sliderFill', 'sliderTrack', 'checkFill'] as const)
    ok(`blue/white: ${k} is the controls colour`, C[k].startsWith(B), C[k]);
  const m = meaningsFor(LED.white.rgb);
  ok('meanings come from the clash helper', C.good === m.good && C.warn === m.warn && C.bad === m.bad
     && C.gradeGood === m.good && C.gradeWarn === m.warn && C.warnBorder === m.warn && C.badPublish === m.bad && C.badSoft === m.bad);
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
    text: C.text, heading: C.heading, section: C.section, textDim: C.textDim, textMuted: C.textMuted,
    placeholder: C.placeholder, placeholderDim: C.placeholderDim,
    key: C.key, keyQuiet: C.keyQuiet, keyDim: C.keyDim, keyRemove: C.keyRemove, keyAct: C.keyAct, keySel: C.keySel,
    good: C.good, warn: C.warn, bad: C.bad, gradeGood: C.gradeGood, gradeWarn: C.gradeWarn,
    badPublish: C.badPublish, badSoft: C.badSoft,
    ok: C.ok, danger: C.danger, selected: C.selected,
  };
  const r: Array<[string, string, RGB]> = Object.entries(plain).map(([k, v]) => [k, v, BG]);
  r.push(['keyAct on its fill', C.keyAct, on(C.keyBgAct, BG)]);
  r.push(['keyAct on the protocol card fill', C.keyAct, on(C.keyBgCard, BG)]);
  r.push(['keyAct on the RTL-TCP start fill', C.keyAct, on(C.keyBgStart, BG)]);
  r.push(['keySel on a selected card', C.keySel, on(C.keySelBg, BG)]);
  r.push(['hint inside a selected card (keySelBg)', C.textOnSel, on(C.keySelBg, BG)]);
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
serverChecks('amber/amber tokens', serverPalette(LITE_TOKENS));

// ── 2b. ★★★ VIBESERVER LITE LOOKS EXACTLY AS BEFORE (Stuart, 2026-10-13: "no colour-based UI … must look EXACTLY as
//   before"). Every role against the literal drawn at that place before 034143e6 — old line numbers from
//   `git show 034143e6^:<file>`: SMS = src/screens/ServerModeScreen.tsx, BLE = BandLimitEditor, ABE = AntennaBandsEditor,
//   ASS = AntennaSwitchSetup, LU = LiteUpdate. C.* there = theme.ts Colors (amber / gold #ffb833, goldDim
//   rgba(255,184,51,0.55), textDim rgba(255,160,0,0.45), border rgba(255,160,0,0.22), borderBright rgba(255,160,0,0.55),
//   green #28A745, red #E05050, bg #080601); `C.x + '18'` = the 8-digit hex it built.
{
  ok('no tokens = LITE', serverPalette() === LITE && serverPalette(undefined) === LITE);
  const LITE_WAS: Record<keyof ServerPalette, string | null> = {
    bg:             '#080601',                // SMS 1731 C.bg
    text:           '#ffb833',                // SMS 622 / 1733 / 1755 C.amber (inputs, titles, values, switch labels)
    heading:        '#ffb833',                // SMS 2209 / 2626 C.gold (SERVER / RADIO SETTINGS)
    section:        'rgba(255,160,0,0.45)',   // SMS 616 / 633 / 1788 C.textDim
    textDim:        'rgba(255,160,0,0.45)',   // SMS 2106 C.textDim (+ every hint / row name, unchanged lines)
    textMuted:      'rgba(255,184,51,0.55)',  // SMS 1809 / 1951 / 1952 C.goldDim (share end time, idle telemetry rows)
    textOnSel:      'rgba(255,160,0,0.45)',   // SMS 2599 / 2652 / 3020 / 3727 C.textDim (hint inside a selected card)
    placeholder:    'rgba(255,184,51,0.55)',  // SMS 1793 / 1817 / 2182 C.goldDim; BLE 372 / 378 / 385
    placeholderDim: 'rgba(255,160,0,0.45)',   // SMS 621 / 2311 / 2530 C.textDim; ABE 66; ASS 41
    key:            '#ffb833',                // SMS 1738 / 2283 / 2562 C.gold; LU 90; ASS 79
    keyQuiet:       'rgba(255,160,0,0.45)',   // SMS 2021 / 2255 / 2690 C.textDim (logs link, SHOW, unselected chips); LU 79-82
    keyDim:         'rgba(255,184,51,0.55)',  // SMS 3725 C.goldDim (protocol card tag)
    keyRemove:      'rgba(255,160,0,0.45)',   // BLE 472 C.textDim (×)
    keyAct:         '#ffb833',                // SMS 1992 / 2897 / 3518 / 3736 / 3750 / 3757 C.amber (pills, option rows, ✓)
    keySel:         '#28A745',                // SMS 2596 / 2649 / 2972 C.green (selector cards); BLE 295; ABE 62; ASS 36
    keyBorder:      'rgba(255,160,0,0.45)',   // SMS 3515 C.textDim (a blocked mode, off)
    keyBorderAct:   '#ffb833',                // SMS 1991 / 2896 / 3515 / 3681 / 3722 C.amber; LU 89 C.gold
    keySelBorder:   '#28A745',                // SMS 2591 / 2647 / 2668 C.green; BLE 274; ABE 61; ASS 35 / 90
    keyBgAct:       '#ffb83322',              // SMS 3735 C.amber + '22' (pill)
    keyBgCard:      '#ffb83314',              // SMS 3722 C.amber + '14' (protocol card)
    keyBgStart:     '#ffb83318',              // SMS 3681 C.amber + '18' (Start RTL-TCP)
    keySelBg:       '#28A74518',              // SMS 2592 / 2648 / 2689 C.green + '18'; BLE 275; ABE 61; ASS 35
    border:         'rgba(255,160,0,0.22)',   // SMS 622 / 1794 / 1818 C.border
    borderBright:   'rgba(255,160,0,0.55)',   // SMS 1762 C.borderBright
    trackOff:       'rgba(255,160,0,0.22)',   // SMS 1806 / 1921 / 2192 Switch false: C.border
    trackOn:        '#28A745',                // SMS 1806 / 1921 / 2192 Switch true: C.green
    thumb:          '#ffb833',                // SMS 1806 / 1921 / 2192 thumbColor C.amber; BLE 355 / 449 C.gold
    fill:           '#28A745',                // SMS 2120 C.green (measuring bar)
    sliderFill:     '#ffb833',                // BLE 354 / 448 C.gold
    sliderTrack:    'rgba(255,255,255,0.25)', // BLE 354 / 448
    checkFill:      '#28A74555',              // BLE 437 `${C.green}55`
    good:           '#28A745',                // SMS 1924 / 2376 C.green
    gradeGood:      '#7bd88f',                // SMS 2106
    warn:           '#ffb833',                // SMS 1771 (open, no PIN) / 2740 / 2743 / 3612 C.amber
    warnBorder:     'rgba(255,184,51,0.55)',  // SMS 3611 C.goldDim
    gradeWarn:      '#ffcc66',                // SMS 2105
    bad:            '#E05050',                // SMS 1776 / 3631 / 3632 C.red
    badPublish:     '#ff5c5c',                // SMS 1934
    badSoft:        '#ff8a7d',                // SMS 2064 / 2065 / 2075 / 2077 / 2105
    selected:       '#28A745',                // SMS 2722 / 2724 C.green (DAB landing station)
    selectedBg:     '#28A74518',              // SMS 2723 C.green + '18'
    ok:             '#28A745',                // SMS 1771 / 1951 / 2002 / 2004 / 2012 / 2014 / 3663 / 3674 / 3675 C.green
    okBg:           '#28A74518',              // SMS 3663 C.green + '18'
    danger:         '#E05050',                // SMS 1998 / 2000 / 2007 / 2009 / 3662 / 3675 C.red
    dangerBg:       '#E0505018',              // SMS 3662 C.red + '18'
    m:              null,                     // (not a colour — only meaningText's bold rule, below)
  };
  for (const k of Object.keys(LITE) as Array<keyof ServerPalette>) {
    ok(`Lite role ${k} is listed`, k in LITE_WAS);
    if (k === 'm') continue;
    ok(`Lite ${k} = its pre-change literal`, LITE[k] === LITE_WAS[k], `got ${String(LITE[k])} want ${LITE_WAS[k]}`);
  }
  for (const k of Object.keys(LITE_WAS)) ok(`Lite role ${k} exists`, k in LITE);
  ok('Lite never bolds an error (meaningText)', LITE.m.badBold === false);
  // The TV text field (Lite runs on TV boxes): its pre-change literals (034143e6^ TvTextInput.tsx styles).
  const tv = src('src/components/TvTextInput.tsx');
  ok('TvTextInput Lite = old literals (focused #ffb833, edges #5a4520, label #c9922e, DONE #ffb833)',
     tv.includes("TV_LITE: TvFieldColours = { focus: '#ffb833', border: '#5a4520', label: '#c9922e', doneBorder: '#5a4520', done: '#ffb833' }"));
  ok('TvTextInput: no tokens -> the Lite styles', /if \(!pt\) return LITE_STYLES;/.test(tv) && /createContext<PopupTokens \| null>\(null\)/.test(tv));
}
console.log(`  ${n} controls × display × text combinations; Lite held to its pre-change literals`);
console.log(`  note: listener chip over a WHITE-hot waterfall (not asserted): worst ${chipWorst.r.toFixed(2)}:1 (${chipWorst.what})`);

// ── 4. Source: no old colours left; Lite stays bundleable ─────────────────────
const FILES = ['src/screens/ServerModeScreen.tsx', 'src/components/BandLimitEditor.tsx', 'src/components/AntennaBandsEditor.tsx',
               'src/components/AntennaSwitchSetup.tsx', 'src/components/LiteUpdate.tsx', 'src/components/TvTextInput.tsx'];
const OLD = /\bC\.(amber|gold|goldDim|green|red)\b|#ff5c5c|#ff8a7d|#ffcc66|#7bd88f|#ffb833|255,\s*184,\s*51|255,\s*160,\s*0|\+ '[0-9a-f]{2}'|C\.[a-z]+\}[0-9a-f]{2}`/i;
for (const f of FILES) {
  // ★ TvTextInput keeps its old literals ON PURPOSE — they are its Lite table (TV_LITE, checked in 2b).
  if (f.endsWith('TvTextInput.tsx')) continue;
  const hits = code(f).split('\n').filter(l => OLD.test(l));
  ok(`${f}: no old gold / amber / literal meaning (${hits.length})`, hits.length === 0, hits.join('\n'));
}
// ★★★ LITE'S WHOLE IMPORT GRAPH from ServerModeScreen: no PopupShell / FaceplateContext, and no package Lite does not
//   ship (lite/app/package.json — so no Reanimated, no Skia, no Expo). Type-only imports are erased and skipped.
{
  const liteDeps = new Set(Object.keys(JSON.parse(src('lite/app/package.json')).dependencies));
  const root = new URL('../', import.meta.url).pathname;
  const seen = new Set<string>(); const pkgs = new Set<string>();
  const resolveRel = (from: string, spec: string) => {
    const base = path.resolve(path.dirname(from), spec);
    return [base, `${base}.tsx`, `${base}.ts`, `${base}.js`, `${base}/index.ts`, `${base}/index.tsx`]
      .find(c => existsSync(c) && statSync(c).isFile());
  };
  const walk = (file: string) => {
    if (seen.has(file)) return; seen.add(file);
    const text = readFileSync(file, 'utf8').split('\n').filter(l => !/^\s*(\*|\/\/|\/\*)/.test(l)).join('\n');
    for (const m of text.matchAll(/^\s*(?:import|export)\s+(type\s+)?(?:[^'"]*?\sfrom\s+)?['"]([^'"]+)['"]/gm)) {
      if (m[1]) continue;
      const spec = m[2];
      if (spec.startsWith('.')) {
        if (/(^|\/)App$/.test(spec)) continue;   // a TYPE from App.tsx — Lite stubs it (lite/app/metro.config.js)
        const r = resolveRel(file, spec);
        ok(`Lite graph: ${spec} resolves from ${path.relative(root, file)}`, !!r);
        if (r) walk(r);
      } else pkgs.add(spec.startsWith('@') ? spec.split('/').slice(0, 2).join('/') : spec.split('/')[0]);
    }
  };
  walk(path.join(root, 'src/screens/ServerModeScreen.tsx'));
  const banned = [...seen].filter(f => /PopupShell|FaceplateContext/.test(f)).map(f => path.relative(root, f));
  console.log(`  Lite graph: ${seen.size} files, packages ${[...pkgs].sort().join(", ")}`);
  ok(`Lite graph (${seen.size} files): no PopupShell / FaceplateContext`, banned.length === 0, banned.join(' '));
  const foreign = [...pkgs].filter(p => !liteDeps.has(p));
  ok(`Lite graph: only Lite's own packages (${[...pkgs].join(', ')})`, foreign.length === 0, foreign.join(' '));
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
