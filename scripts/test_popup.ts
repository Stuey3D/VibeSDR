/**
 * Popups take the chassis (faceplates brief §10.3) — src/constants/popupTokens.ts and the popups
 * that read it.
 *
 * Proves:
 *   • the DEFAULT chassis is today, value for value: every gold role is the literal the popup drew
 *     (§10.3 "Unchanged … pixel for pixel"), and the default tokens do not move with any colour;
 *   • no popup still holds a hard-coded gold (C.gold / #ffb833 / rgba(255,184,51,…)) — the 83 moved
 *     into usePopupTheme() and must stay there;
 *   • CONTRAST (§10.3, the decoder test's pattern): every ENGRAVED role that carries information
 *     clears 4.5:1 on the plate at its worst lighting, on silver and black, under every controls ×
 *     text colour; window text (text colour / neon) clears 4.5:1 on the #070605 window; unlit
 *     legends clear it on their cap;
 *   • the tune entry follows the Display (Hyperlegible / Nixie One neon / Doto / DSEG14), and the
 *     Nixie rule holds (Nixie One is only ever neon);
 *   • the toSegCells feed is an identity for a frequency (so the 14-segment entry can be a live
 *     TextInput);
 *   • Transparency OFF in popups (source checks, as test_transparency does): every scrim reads
 *     `scrimOpacity`, no popup draws a BlurView without the switch.
 *
 * Run: node --no-warnings scripts/test_popup.ts   (run-tests.sh does)
 */
import { readFileSync, readdirSync } from 'node:fs';
import { CONTROLS, TEXTS, DISPLAYS, LED, resolveTextColour, SILVER_CHASSIS, BLACK_CHASSIS } from '../src/constants/faceplate.ts';
import {
  buildPopupTokens, popupTokensFor, TODAY_ACCENT, TODAY_ACCENT_DIM, TODAY_AMBER, CONTRAST_FIX,
  SCROLL_LANE, scrollLane, scrollLaneOutset, type PopupTokens,
} from '../src/constants/popupTokens.ts';
import { toSegCells } from '../src/constants/displayText.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
}
const eq = (what: string, got: unknown, want: unknown) =>
  ok(what, JSON.stringify(got) === JSON.stringify(want), `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

// ── Colour maths (the decoder contrast test's) ───────────────────────────────
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

// ── 1. The default chassis is today ──────────────────────────────────────────
{
  const d = popupTokensFor('default');
  eq('default: not metal', [d.metal, d.silver], [false, false]);
  eq('default gold roles = today\'s literals', {
    sel: d.gold.sel, selBorder: d.gold.selBorder, fill: d.gold.fill, thumb: d.gold.thumb, value: d.gold.value,
    readout: d.gold.readout, glyph: d.gold.glyph, notice: d.gold.notice, noticeBorder: d.gold.noticeBorder,
    close: d.gold.close, amber: d.gold.amber, amber45: d.gold.amberA(0.45), amber80: d.gold.amberA(0.8),
  }, {
    sel: '#ffe566', selBorder: 'rgba(255,229,102,0.70)', fill: '#ffe566', thumb: '#ffe566', value: '#ffe566',
    readout: '#ffe566', glyph: '#ffe566', notice: '#ffe566', noticeBorder: 'rgba(255,229,102,0.70)',
    close: 'rgba(255,229,102,0.70)', amber: '#ffb833', amber45: 'rgba(255,184,51,0.45)', amber80: 'rgba(255,184,51,0.8)',
  });
  eq('the two golds are the files\' own', [TODAY_ACCENT, TODAY_ACCENT_DIM, TODAY_AMBER],
     ['#ffe566', 'rgba(255,229,102,0.70)', '#ffb833']);
  // ★ The default chassis never reads a colour: one object whatever the controls / text / display.
  let same = true;
  for (const c of CONTROLS) for (const t of TEXTS) for (const disp of DISPLAYS) {
    if (popupTokensFor('default', LED[c].rgb, LED[t].rgb, disp) !== d) same = false;
  }
  ok('default tokens do not move with any colour or display', same);
  ok('memoised per setting', popupTokensFor('silver', LED.red.rgb, LED.teal.rgb, 'seg') === popupTokensFor('silver', LED.red.rgb, LED.teal.rgb, 'seg'));
}

// ── 2. No hard-coded gold left in a popup ────────────────────────────────────
const src = (f: string) => readFileSync(new URL(`../src/${f}`, import.meta.url), 'utf8');
const POPUPS = ['components/AudioSheet.tsx', 'components/MenuSheet.tsx', 'components/ChatDrawer.tsx',
                'components/RecordingsOverlay.tsx'];
for (const f of POPUPS) {
  const hits = (src(f).match(/C\.gold\b|#ffb833|255,\s*184,\s*51/gi) ?? []);
  ok(`${f}: no hard-coded gold (${hits.length})`, hits.length === 0, hits.join(' '));
}

// ── 3. Contrast ──────────────────────────────────────────────────────────────
/** The plate under engraved text at the point of its lighting WORST for that text (decoder test). */
function plateWorst(silver: boolean): RGB {
  const base = rgb((silver ? SILVER_CHASSIS : BLACK_CHASSIS).plate!.base);
  // Black: the lighting's brightest white band, read from the tokens (darkened in build 356).
  const band = Math.max(...(BLACK_CHASSIS.plate!.lightColors.map(c => {
    const m = /^rgba\(255,255,255,([\d.]+)\)$/.exec(c.replace(/\s/g, '')); return m ? +m[1] : 0; })));
  return silver ? over([0, 0, 0, 0.16], base) : over([255, 255, 255, band], base);
}
const WINDOW: RGB = [7, 6, 5];
const rows: string[] = [];
for (const chassis of ['silver', 'black'] as const) {
  const plate = plateWorst(chassis === 'silver');
  for (const c of CONTROLS) for (const disp of DISPLAYS) for (const t of TEXTS) {
    const text = resolveTextColour(disp, t);
    const pt = buildPopupTokens(chassis, LED[c].rgb, text.rgb, disp);
    const tag = `${chassis}/${c}/${disp}/${t}`;
    for (const r of ['label', 'note', 'value', 'chatOwnPlate', 'chatOtherPlate'] as const) {
      const v = contrast(pt[r], plate);
      ok(`${tag} plate ${r} ${v.toFixed(2)}:1`, v >= 4.5);
    }
    // Unlit legends on their own cap (silver #c4c1ba under its sheen's darkest foot; black #141517).
    const cap: RGB = chassis === 'silver' ? over([0, 0, 0, 0.22], rgb('#c9c6bf')) : rgb('#1d1e20');
    ok(`${tag} unlit legend on cap`, contrast(pt.legend, cap) >= 4.5, contrast(pt.legend, cap).toFixed(2));
    // Window text: the text colour (neon under Nixie) and the meaning colours of chat.
    for (const r of ['winText', 'readout', 'chatOwn', 'chatOther', 'chatText'] as const) {
      const v = contrast(pt[r], WINDOW);
      ok(`${tag} window ${r} ${v.toFixed(2)}:1`, v >= 4.5);
    }
    ok(`${tag} window dim ≥ 4.5`, contrast(pt.winDim, WINDOW) >= 4.5, contrast(pt.winDim, WINDOW).toFixed(2));
    ok(`${tag} tune entry in window`, contrast(pt.entry.color, WINDOW) >= 4.5);
  }
  const one = buildPopupTokens(chassis, LED.green.rgb, LED.green.rgb, 'hyper');
  rows.push(`${chassis.padEnd(6)} label ${contrast(one.label, plate).toFixed(2)}  note ${contrast(one.note, plate).toFixed(2)}  value ${contrast(one.value, plate).toFixed(2)}`);
}
eq('the contrast fixes are the ones the header documents', CONTRAST_FIX,
   { silverNote: '#3c3935', blackNote: '#9ea0a4', silverOwn: '#173f62', silverOther: '#503808' });

// ── 4. The tune entry follows the Display; the Nixie rule ────────────────────
{
  const fam = (d: 'hyper' | 'nixie' | 'dot' | 'seg') => buildPopupTokens('black', LED.amber.rgb, resolveTextColour(d, 'teal').rgb, d).entry;
  eq('entry fonts', ['hyper', 'nixie', 'dot', 'seg'].map(d => fam(d as any).fontFamily),
     ['Atkinson Hyperlegible', 'Nixie One', 'Doto', 'DSEG14 Classic']);
  eq('Nixie entry is neon (§2)', [fam('nixie').color, fam('nixie').glow], ['#ffc48a', '#ff6410']);
  eq('seg entry is fed through toSegCells', fam('seg').seg, true);
  eq('dot entry in the text colour', fam('dot').color, `rgba(${LED.teal.rgb},1)`);
  // ★ Every metal token that is Nixie One is neon, whatever colours are asked for.
  let nixieOk = true;
  for (const ch of ['silver', 'black'] as const) for (const c of CONTROLS) for (const t of TEXTS) {
    const e = buildPopupTokens(ch, LED[c].rgb, resolveTextColour('nixie', t).rgb, 'nixie').entry;
    if (e.fontFamily === 'Nixie One' && e.color !== '#ffc48a') nixieOk = false;
  }
  ok('Nixie One is only ever neon in a popup', nixieOk);
  for (const f of ['1035.000', '14230', '96.6', '0.648']) eq(`toSegCells identity for ${f}`, toSegCells(f), f);
}

// ── 5. Danger legends (a meaning colour) read on their cap ───────────────────
for (const chassis of ['silver', 'black'] as const) {
  const pt = buildPopupTokens(chassis, LED.green.rgb, LED.green.rgb, 'hyper');
  const cap: RGB = chassis === 'silver' ? over([0, 0, 0, 0.22], rgb('#c9c6bf')) : rgb('#1d1e20');
  ok(`${chassis} danger legend on its cap ≥ 4.5`, contrast(pt.danger, cap) >= 4.5, contrast(pt.danger, cap).toFixed(2));
}
// ★ Lit legends on the SILVER cap are REPORTED, not asserted (the decoder test's rule): an LED legend
//   on a light cap is the mockup's and the deck's own silver key; no same-hue lift takes green or
//   white to 4.5:1 on #c4c1ba, and the pip carries the selection either way.
{
  const cap: RGB = over([0, 0, 0, 0.22], rgb('#c9c6bf'));
  const lit = CONTROLS.map(c => `${c} ${contrast(buildPopupTokens('silver', LED[c].rgb, LED.green.rgb, 'hyper').legendLit, cap).toFixed(1)}`);
  rows.push(`note: silver lit legends on the cap (reported): ${lit.join(', ')}`);
}

// ── 6. Transparency OFF in popups (source checks — a render test would need a device) ──────────
{
  const shell = src('components/PopupShell.tsx');
  ok('PopupScrim reads scrimOpacity (no dim with OFF, the view stays)', /scrimOpacity > 0/.test(shell) && /onPress=\{onPress\}/.test(shell));
  ok('usePopupFrame drops the drop shadow with OFF', /surface\.dropShadow \?[\s\S]*?: NO_DROP_SHADOW/.test(shell));
  ok('usePopupSurface: fill + no shadow with OFF', /shadow: surface\.dropShadow \? null : NO_DROP_SHADOW/.test(shell));
  ok('PopupShell draws no BlurView', !/<BlurView\b/.test(shell));
  const scrims: Array<[string, RegExp]> = [
    ['components/FreqModal.tsx',        /<PopupScrim /],
    ['components/AudioSheet.tsx',       /<PopupScrim /],
    ['components/KeyboardShortcuts.tsx', /<PopupScrim /],
    ['components/ModeSelector.tsx',     /<PopupScrim /],
    ['components/ChatDrawer.tsx',       /!surf\.opaque && cd\.backdrop/],
    ['components/StepPicker.tsx',       /!surf\.opaque && \{ backgroundColor: BACKDROP \}/],
    ['components/CityPickerModal.tsx',  /dim && s\.backdropDim/],
    ['components/PasswordModal.tsx',    /dim && styles\.overlayDim/],
    ['components/IdentModal.tsx',       /dim && styles\.overlayDim/],
    ['components/LocalHardwarePanel.tsx', /fp\.opaque && styles\.backdropNone/],
  ];
  for (const [f, re] of scrims) ok(`${f}: its dim follows Transparency OFF`, re.test(src(f)));
  // ★★ No corner screws on a decoder box (2026-10-04, silver: they sat on the status dot and the × key).
  ok('DecoderShell: the plate has no screws (popupPlate)', /const plate = deckPlate \? popupPlate\(deckPlate\)/.test(src('components/DecoderShell.tsx')));
  // ★★ The bottom sheets carry NO dim at all (Stuart, 2026-10-04: the Audio/Mode scrim darkened only the space above the
  //   sheet; "I don't mind not having the transparency layer when the menu is up" — and it saves a full-screen blend).
  ok('MenuSheet: no dim behind the menu', !/styles\.backdrop\b/.test(src('components/MenuSheet.tsx')));
  for (const f of ['components/AudioSheet.tsx', 'components/ModeSelector.tsx'])
    ok(`${f}: its scrim is transparent (tap-to-close only)`, /<PopupScrim[^>]*color="transparent"/.test(src(f)));
  // ★ The popups that moved their dim INTO PopupScrim carry no colour in the style any more (the
  //   scrim owns it), so nothing can bring an unconditional dim back through that style.
  for (const f of ['components/FreqModal.tsx', 'components/AudioSheet.tsx', 'components/KeyboardShortcuts.tsx',
                   'components/ModeSelector.tsx', 'components/StepPicker.tsx', 'components/CityPickerModal.tsx',
                   'components/PasswordModal.tsx', 'components/IdentModal.tsx']) {
    const bare = src(f).match(/(backdrop|overlay):\s*\{[^}]*backgroundColor/g) ?? [];
    ok(`${f}: the scrim's style holds no colour`, bare.length === 0, bare.join(' '));
  }
  // Glass popups go opaque with OFF (fill), and the one with a drop shadow drops it.
  for (const f of ['components/FreqModal.tsx', 'components/AudioSheet.tsx', 'components/ChatDrawer.tsx',
                   'components/KeyboardShortcuts.tsx', 'components/StepPicker.tsx', 'components/ModeSelector.tsx']) {
    ok(`${f}: glass made opaque with OFF`, /surf\.opaque[^\n]*surf\.fill\(/.test(src(f)));
  }
  ok('ChatDrawer drops its drop shadow with OFF', /surf\.shadow/.test(src('components/ChatDrawer.tsx')));
  // ★★ Silver / black are opaque plates: MenuSheet's two BlurViews go on metal as well as with OFF.
  const menu = src('components/MenuSheet.tsx');
  eq('MenuSheet: both blur layers skip metal', (menu.match(/!opaque && !pt\.metal &&/g) ?? []).length, 2);
}

// ── 7. Every popup reads the chassis ─────────────────────────────────────────
for (const f of ['FreqModal', 'AudioSheet', 'MenuSheet', 'ChatDrawer', 'RecordingsOverlay', 'KeyboardShortcuts',
                 'PasswordModal', 'IdentModal', 'CityPickerModal', 'AboutOverlay', 'StepPicker', 'LocalHardwarePanel',
                 'ModeSelector']) {
  const s = src(`components/${f}.tsx`);
  ok(`${f} reads usePopupTheme`, /usePopupTheme\(\)/.test(s));
  ok(`${f} draws the plate on metal`, /<PopupPlate\b/.test(s));
}
// ★★ The DEMODULATOR sheet (build 356: "still the old gold / glass style"): the demodulators are pip
//    keys on metal, CLOSE is a plain one, the bandwidth sliders are faders, and its gold glass fill is
//    never laid over the plate.
{
  const ms = src('components/ModeSelector.tsx');
  ok('ModeSelector: the demodulator keys carry the pip on metal',
     /pick\(m\.id\)\}>\{\(navFocused, navRef\) => pt\.metal \? \(\s*<MetalKey[^>]*active=\{m\.id === current\} pip/.test(ms));
  ok('ModeSelector: CLOSE is a plain dome key (no pip)', /<PopupKey ref=\{nr as any\} label="CLOSE"(?![^>]*\bpip\b)/.test(ms));
  ok('ModeSelector: slide faders on metal', /<PopupFader\b/.test(ms));
  ok('ModeSelector: the glass fill never reaches the plate', /surf\.opaque && !pt\.metal && \{ backgroundColor: surf\.fill\(SHEET_BG\) \}/.test(ms));
  ok('ModeSelector: no front-panel click (PopupKey is silent)', /useDomeKey\(\{ silent: true \}\)/.test(src('components/PopupShell.tsx')));
}
// ★ The CONTROL CUSTOMISATION pane's one swap point is a pip key on metal.
ok('SelectorKey is a PopupKey with a pip on metal', /function SelectorKey[\s\S]*?pt\.metal[\s\S]*?<PopupKey[^>]*\bpip\b/.test(src('components/MenuSheet.tsx')));

// ── 8. The scroll indicator's lane ───────────────────────────────────────────
// ★★★ A SCROLL INDICATOR MUST NEVER SIT ON A BUTTON (Stuart, 2026-10-01: the AUDIO popup on a Mac with
//   scroll bars always shown had it over the NR readout and every right-hand key). Every VERTICAL
//   ScrollView / FlatList in a component (and the radio list on SDRScreen) that shows its indicator
//   either takes `scrollLane` / `scrollLaneOutset` / SCROLL_LANE in its opening tag, or says in a
//   `★ scroll lane:` comment there what already keeps its content clear. A new scroller with neither
//   fails here — that is the point: the lane has to be a decision, not a default.
{
  ok('SCROLL_LANE is 12 pt', SCROLL_LANE === 12);
  ok('scrollLane pads the right by the lane', scrollLane.paddingRight === SCROLL_LANE);
  ok('scrollLaneOutset lends the lane from the parent', scrollLaneOutset.marginRight === -SCROLL_LANE);
  const files = readdirSync(new URL('../src/components/', import.meta.url))
    .filter((f) => f.endsWith('.tsx')).map((f) => `components/${f}`).concat(['screens/SDRScreen.tsx']);
  /** The opening tag from `<Name` to its closing `>`, skipping `>` inside {…}, strings and comments. */
  function openingTag(s: string, at: number): string {
    let depth = 0, i = at + 1, q = '';
    for (; i < s.length; i++) {
      const c = s[i];
      if (q) { if (c === q && s[i - 1] !== '\\') q = ''; continue; }
      if (c === '/' && s[i + 1] === '*') { i = s.indexOf('*/', i + 2) + 1; continue; }
      if (c === '/' && s[i + 1] === '/' ) { i = s.indexOf('\n', i); continue; }
      if (depth > 0 && (c === '"' || c === "'" || c === '`')) { q = c; continue; }
      if (c === '{') depth++;
      else if (c === '}') depth--;
      else if (c === '>' && depth === 0) break;
    }
    return s.slice(at, i + 1);
  }
  let scrollers = 0;
  for (const f of files) {
    const s = src(f);
    for (const m of s.matchAll(/(?<![\w.])<(ScrollView|FlatList|SectionList|DraggableFlatList)\b(?!\s*[|>,])/g)) {
      const tag = openingTag(s, m.index!);
      if (/\bhorizontal\b(?!\s*=\s*\{\s*false)/.test(tag)) continue;
      if (/showsVerticalScrollIndicator=\{false\}/.test(tag)) continue;
      scrollers++;
      const line = s.slice(0, m.index!).split('\n').length;
      ok(`${f}:${line} <${m[1]}> keeps its indicator off the content`,
         /\bscrollLane(Outset)?\b|\bSCROLL_LANE\b|★ scroll lane:/.test(tag), tag.slice(0, 160).replace(/\s+/g, ' '));
      // An outset without the lane would only move the content INTO the indicator.
      if (/\bscrollLaneOutset\b/.test(tag)) ok(`${f}:${line} outset comes with the lane`, /\bscrollLane\b/.test(tag));
    }
  }
  ok('the lane check found the popup scrollers', scrollers >= 15, `found ${scrollers}`);
  // ★★ The AUDIO sheet's key rows WRAP (A-BW was cut off at the right edge): no key row may push a
  //    flex spacer and then its keys, which is how they overflowed.
  const audio = src('components/AudioSheet.tsx');
  ok('AudioSheet: BCAST FM keys wrap', /BCAST FM<\/Text>[\s\S]{0,400}<View style=\{st\.keyWrapEnd\}>/.test(audio));
  ok('AudioSheet: DE-EMPH keys wrap', /DE-EMPH<\/Text>\s*<View style=\{st\.keyWrapEnd\}>/.test(audio));
  ok('AudioSheet: keyWrapEnd wraps', /keyWrapEnd:\s*\{[^}]*flexWrap: 'wrap'/.test(audio));
}

console.log(rows.join('\n'));
console.log(`popup: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
