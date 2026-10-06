/**
 * ★★★ EVERY BIG ON-SCREEN ELEMENT FOLLOWS THE MAIN DISPLAY'S FONT (Stuart, 2026-10-06, RC18 on Nixie: "the bottom bar
 * and the FM filter message should be in the Nixie font. Whatever font the main display is set to is for all the big
 * on screen elements should follow, only decoder boxes etc need to have the hyperlegible as those have super small
 * text").
 *
 *   1. THE ONE SOURCE — faceplate.ts ScreenText (theme.screen), on every Display × chassis × text colour: the font is
 *      the display's, the deck's banner / mode roles and the VTS agree with it, the status row's type comes from it,
 *      Nixie is neon, one-weight faces are never bold.
 *   2. EVERY ELEMENT READS IT — the source of each listed element is held to the shared reader (a stray fontFamily
 *      is how the bug happened), and the dense small-text places are held to Hyperlegible.
 *   3. screenString() — what each font can draw.
 *   4. THE FONT FILES — Nixie One has every glyph those elements write; ScreenText.charEm is not an under-estimate.
 *   5. NIXIE FITS — the status row in Nixie One (and Atkinson) at every width, metal and default chassis, with the
 *      same drops VCR / DOT use (lib_status_row: statusFit / PortraitStats' order).
 *
 * Run: npx tsx scripts/test_faceplate_screenfont.ts
 */
import { readFileSync } from 'node:fs';
import {
  CHASSIS, DISPLAYS, FONT_DOTO, FONT_HYPER, FONT_NIXIE, FONT_SEG14, LED, NEON_TEXT, TEXTS, DEFAULT_SETTINGS,
  resolveFaceplate, screenInk, screenOneWeight, statusDisplayFor, withDisplay,
  type DisplayStyle, type FaceplateSettings,
} from '../src/constants/faceplate.ts';
import { foldForDot, screenString, toSegCells } from '../src/constants/displayText.ts';
import { HYPER, NIXIE, DOTO } from './lib_font_metrics.ts';
import * as SR from './lib_status_row.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);
const src = (rel: string) => readFileSync(new URL(`../${rel}`, import.meta.url), 'utf8');

// ── 1. The one source ─────────────────────────────────────────────────────────
const FONT_OF: Record<DisplayStyle, string> = { hyper: FONT_HYPER, nixie: FONT_NIXIE, dot: FONT_DOTO, seg: FONT_SEG14 };
for (const display of DISPLAYS) for (const chassis of CHASSIS) for (const text of TEXTS) {
  const s: FaceplateSettings = { ...withDisplay({ ...DEFAULT_SETTINGS, chassis }, display), text };
  const fp = resolveFaceplate(s);
  const sc = fp.screen;
  const tag = `${display}/${chassis}/${text}`;
  eq(`${tag}: the display's font`, sc.font, FONT_OF[display]);
  eq(`${tag}: style`, sc.style, display);
  eq(`${tag}: VCR / DOT draw cells, the others text`, sc.face, display === 'seg' || display === 'dot' ? display : null);
  eq(`${tag}: the SHARED TUNER banner takes it`, fp.deck.bannerFont, sc.font);
  // The mode box: its own cells on VCR (modeFont never reaches the glass there); the display's font everywhere else.
  if (display !== 'seg') eq(`${tag}: the mode box takes it`, fp.deck.modeFont, sc.font);
  eq(`${tag}: the VTS strip agrees`, fp.vts.font, sc.font);
  eq(`${tag}: one weight unless Hyperlegible`, sc.oneWeight, display !== 'hyper');
  eq(`${tag}: no bold asked of a one-weight face`, screenOneWeight(sc), display === 'hyper' ? null : { fontWeight: 'normal' });
  if (display === 'nixie') {
    // ★★★ THE NIXIE RULE (§2): anything in Nixie One is neon — whatever colour the element asked for.
    eq(`${tag}: Nixie One is neon`, [sc.color, sc.glow, sc.rgb], [NEON_TEXT.core, NEON_TEXT.glow, LED.neon.rgb]);
    eq(`${tag}: an element's own colour gives way to neon`, screenInk(sc, '#ffd479'), NEON_TEXT.core);
    eq(`${tag}: the mode box is neon`, [fp.deck.mode, fp.deck.reading], [NEON_TEXT.mode, NEON_TEXT.reading]);
  } else if (display === 'hyper') {
    eq(`${tag}: Hyperlegible keeps the element's own colour`, screenInk(sc, '#ffd479'), '#ffd479');
  } else {
    eq(`${tag}: a one-colour VFD lights everything in its text colour`, screenInk(sc, '#ffd479'), fp.text.core);
  }
  // The status row: the display's font and colour on every faceplate; only the default deck on HYPER is today's footer.
  const metal = chassis !== 'default';
  const sd = statusDisplayFor(sc, metal, 12, 8);
  if (!metal && display === 'hyper') eq(`${tag}: default deck + Hyperlegible keeps today's footer`, sd, null);
  else {
    ok(`${tag}: the status row has a type`, sd !== null);
    eq(`${tag}: the status row is in the display's font`, sd?.font, sc.font);
    eq(`${tag}: …its colour and glow`, [sd?.color, sd?.glow], [sc.color, sc.glow]);
    eq(`${tag}: …its cells`, sd?.face, sc.face);
    ok(`${tag}: …never Doto unless the display is DOT (the RC18 bug)`, sd?.font !== FONT_DOTO || display === 'dot');
  }
}
{
  const nx = resolveFaceplate({ ...withDisplay({ ...DEFAULT_SETTINGS, chassis: 'black' }, 'nixie') }).screen;
  eq('Nixie on metal: §8.1\'s 12 pt, 10 pt floor', [statusDisplayFor(nx, true, 12, 8)?.size, statusDisplayFor(nx, true, 6, 8)?.size],
     [12, 10]);
  eq('Nixie on the default deck: the footer\'s own size (the bar does not grow)', statusDisplayFor(nx, false, 12, 8)?.size, 8);
  const dot = resolveFaceplate(withDisplay(DEFAULT_SETTINGS, 'dot')).screen;
  eq('DOT cells on the default deck keep the 10 pt floor', statusDisplayFor(dot, false, 12, 8)?.size, 10);
}

// ── 2. Every listed element reads the shared source ───────────────────────────
{
  const cb = src('src/components/ControlsBar.tsx');
  const well = cb.slice(cb.indexOf('function StatusWell('), cb.indexOf('// ── PORTRAIT'));
  ok('StatusWell: the status type is statusDisplayFor(fp.screen …)', /statusDisplayFor\(screen,/.test(well) && /fp\.screen/.test(well));
  ok('StatusWell: no hard-wired Doto (the RC18 bug)', !/FONT_DOTO/.test(well));
  const text = cb.slice(cb.indexOf('function StatusText('), cb.indexOf('function GainArrow('));
  ok('StatusText: draws in sd.font', /fontFamily: sd\.font/.test(text));
  // Each SHARED TUNER banner: the banner font, the string made drawable, no bold on a one-weight face.
  const banners = cb.match(/<Text style=\{\[(?:pm\.sharedTxt|cd\.bannerTxt)[\s\S]*?<\/Text>/g) ?? [];
  eq('three SHARED TUNER banners in the deck', banners.length, 3);
  for (const [i, b] of banners.entries()) {
    ok(`banner ${i}: bannerFont`, /bannerFont/.test(b));
    ok(`banner ${i}: screenString`, /screenString\(/.test(b));
    ok(`banner ${i}: screenOneWeight`, /screenOneWeight\(fp\.screen\)/.test(b));
    ok(`banner ${i}: sizeK`, /fp\.screen\.sizeK/.test(b));
  }
  const mode = cb.slice(cb.indexOf('function ModeReadout('), cb.indexOf('const sharedBannerFont'));
  ok('the mode box draws in dk.modeFont', (mode.match(/fontFamily: dk\.modeFont/g) ?? []).length === 3);
  ok('the mode box never asks a one-weight face for bold', /oneWeight = dot \|\| dk\.style === 'nixie'/.test(mode)
     && !/fontWeight: dot \?/.test(mode));
  ok('the mode box is sized for Nixie One (modeBox face)', /dk\.style === 'nixie' \? 'nixie'/.test(cb));
  const dab = src('src/components/DabMeter.tsx');
  ok('DabMeter: fontFamily is the display\'s', /fontFamily: screen\.font/.test(dab) && /const screen = fp\.screen/.test(dab));
  ok('DabMeter: strings made drawable', (dab.match(/screenString\(screen\.style/g) ?? []).length === 2);
  ok('DabMeter: no hard-wired font', !/FONT_(DOTO|HYPER|NIXIE)/.test(dab));
  const ant = src('src/components/AntennaBandNotice.tsx');
  ok('AntennaBandNotice: through DisplayFontText', /<DisplayFontText/.test(ant) && !/fontFamily/.test(ant) && !/FONT_DOTO/.test(ant));
  const dft = src('src/components/DisplayFontText.tsx');
  ok('DisplayFontText: font, ink, weight and string all from fp.screen', /useFaceplate\(\)\.screen/.test(dft)
     && /fontFamily: sc\.font/.test(dft) && /screenInk\(sc/.test(dft) && /screenOneWeight\(sc\)/.test(dft)
     && /screenString\(sc\.style/.test(dft));
  const sdr = src('src/screens/SDRScreen.tsx');
  // ★ Stuart 2026-10-06: "only the main controls and VTS need this, the rest of the menus etc can be standard" —
  //   the idle-terms pill and the rotate hints are NOT controls, so they stay in the standard font.
  ok('SDRScreen: the idle-terms notice stays standard',
     /<Text style=\{\[styles\.powersavePillText/.test(sdr) && !/<DisplayFontText wrap style=\{\[styles\.powersavePillText/.test(sdr));
  eq('SDRScreen: both rotate hints stay standard',
     (sdr.match(/<Text style=\{styles\.rotateBannerText\}>/g) ?? []).length, 2);
  const vts = src('src/components/VTSBar.tsx');
  ok('VTSBar: the strip draws in its resolved font (COL.font = vts.font)', /fontFamily: COL\.font/.test(vts));

  // ★ LEFT ON HYPERLEGIBLE, on purpose: dense small text (brief §10).
  for (const f of ['src/components/DecoderShell.tsx', 'src/components/DecoderPanel.tsx', 'src/components/DabPanel.tsx',
                   'src/components/MenuSheet.tsx']) {
    let t = '';
    try { t = src(f); } catch { continue; }
    ok(`${f}: stays off the display font`, !/\.screen\b|DisplayFontText|bannerFont|statusDisplayFor/.test(t));
  }
}

// ── 3. screenString ───────────────────────────────────────────────────────────
eq('hyper: as written', screenString('hyper', 'FM band-stop filter fitted'), 'FM band-stop filter fitted');
eq('nixie: as written', screenString('nixie', 'Outside this antenna’s range — reception reduced'), 'Outside this antenna’s range — reception reduced');
eq('dot: Doto keeps its dash and curly quote', screenString('dot', 'antenna’s — range'), 'antenna’s — range');
eq('dot: exactly the VTS\'s fold', screenString('dot', 'Rádio Ω — ok'), foldForDot('Rádio Ω — ok'));
eq('seg: the cells, upper case', screenString('seg', 'FM band-stop'), toSegCells('FM band-stop'));
ok('seg: no lower case reaches DSEG14', !/[a-z]/.test(screenString('seg', 'Shared tuner · free to tune')));
eq('seg + wrap: a break after every blank cell, none after a pointed one', screenString('seg', 'A B. C', { wrap: true }),
   toSegCells('A B. C').replace(/!(?!\.)/g, '!​'));
eq('seg + wrap: the same cells, only break points added', screenString('seg', 'FM band-stop filter fitted', { wrap: true })
   .replace(/​/g, ''), toSegCells('FM band-stop filter fitted'));

// ── 4. The font files ─────────────────────────────────────────────────────────
{
  const N = NIXIE(), H = HYPER(), D = DOTO();
  // Everything the status row and the notices WRITE as text (the logos ⛛ ⚡ ⚿ ↑ ↓ are drawn — SectionIcon, GainArrow).
  const written = [SR.WORST.utc, SR.WORST.srv, SR.WORST.srvShort, SR.WORST.rec, 'SHARED TUNER · ASK TO TUNE · 12/20',
    SR.WORST.rate, '· GAIN 44.5dB (held)', SR.WORST.if, 'Direct Sample', 'Admin Mode', 'STORMS', 'NR NB AN',
    'FM band-stop filter fitted — reception here is deliberately reduced', 'Outside this antenna’s range',
    'This receiver disconnects idle listeners after 5 min — it will ask first', 'ROTATE TO PORTRAIT TO VIEW DECODER',
    'Multiplex moderate · Expect occasional audio break-ups · 14 % frames lost', 'WFM: WHISPER USB: RTTY S9+40 -120 dBf SQL'];
  for (const t of written) {
    eq(`Nixie One draws every glyph of "${t.slice(0, 30)}…"`, N.missing(t), []);
    eq(`Atkinson draws every glyph of "${t.slice(0, 30)}…"`, H.missing(t), []);
  }
  // ScreenText.charEm: what DabMeter plans with. An under-estimate means a sentence it thought fitted is shrunk.
  const dabLines = ['Multiplex weak · No or heavily broken audio · 14 % frames lost',
    'Multiplex moderate · Expect occasional audio break-ups', 'Multiplex strong · Clear audio', 'No signal · Searching for the multiplex',
    'Weak · 14 % frames lost', 'Moderate · 2 % frames lost'];
  const per = (f: typeof N, ls: number) => Math.max(...dabLines.map(l => (f.width(l, 10, ls) / 10) / [...l].length));
  const screens = Object.fromEntries(DISPLAYS.map(d => [d, resolveFaceplate(withDisplay(DEFAULT_SETTINGS, d)).screen]));
  // RN draws Atkinson bold synthesised (~ +8 %, modeBox ATKINSON_BOLD_K); Nixie / Doto one weight, spacing 0.4 at 10 pt.
  const nix = per(N, 0.4), hyp = per(H, 0.2) * 1.08, dot = per(D, 0.4);
  console.log(`  charEm measured (worst DAB line): Nixie ${nix.toFixed(3)} · Atkinson bold ${hyp.toFixed(3)} · Doto ${dot.toFixed(3)}`);
  ok(`Nixie charEm ${screens.nixie.charEm} ≥ measured ${nix.toFixed(3)}`, screens.nixie.charEm >= nix);
  ok(`Atkinson charEm ${screens.hyper.charEm} ≥ measured ${hyp.toFixed(3)}`, screens.hyper.charEm >= hyp);
  ok(`Doto charEm ${screens.dot.charEm} ≥ measured ${dot.toFixed(3)}`, screens.dot.charEm >= dot);
  ok('DSEG14 charEm ≥ its 0.816 em cell + spacing', screens.seg.charEm >= 0.816);
  ok('…and none wildly over (a sentence dropped that would have fitted)', DISPLAYS.every(d => screens[d].charEm <= 0.9));
}

// ── 5. The status row in Nixie One (and Atkinson) fits at every width ─────────
for (const face of ['nixie', 'hyper'] as const) for (const chassis of ['metal', 'default'] as const) {
  if (face === 'hyper' && chassis === 'default') continue;           // today's footer, untouched
  const name = `${face === 'nixie' ? 'NIXIE' : 'HYPER'} ${chassis}`;
  for (const W of SR.LANDSCAPE_WIDTHS) {
    const f = SR.landscapeFit(face, W, chassis);
    ok(`${name} landscape ${W} pt: the row fits after its drops`, f.fits);
    ok(`${name} landscape ${W} pt: the recording timer is never dropped`, !f.hidden.includes('rec'));
    console.log(`  ${name} landscape ${W} pt (status ${f.size.toFixed(1)} pt): drops ${f.hidden.length ? f.hidden.join(', ') : 'nothing'}${f.sharedShort ? ' (SHARED TUNER → SHARED)' : ''}`);
  }
  ok(`${name}: a full-screen Mac shows everything`, SR.landscapeFit(face, 1920, chassis).hidden.length === 0);
  for (const W of SR.PORTRAIT_WIDTHS) {
    const p = SR.portraitStatsFit(face, W, chassis), c = SR.portraitClockRow(face, W, chassis);
    ok(`${name} portrait ${W} pt: the stats line fits after its drops`, p.fits);
    ok(`${name} portrait ${W} pt: clocks + recording + three DSP badges fit row 4 (${c.need.toFixed(0)} of ${c.avail})`, c.need <= c.avail);
    console.log(`  ${name} portrait ${W} pt (status ${p.size.toFixed(1)} pt): stats line drops ${p.hidden.length ? p.hidden.join(', ') : 'nothing'}; row 4 ${c.need.toFixed(0)} of ${c.avail}`);
  }
}
{
  // ★ Against what RC18 drew under the Nixie tubes — the Doto run at the same size: Nixie One is the narrower face, so
  //   the row drops nothing it did not drop before.
  for (const W of [...SR.LANDSCAPE_WIDTHS]) {
    const n = SR.landscapeFit('nixie', W), d = SR.landscapeFit('doto', W);
    ok(`landscape ${W} pt: Nixie drops no more than the RC18 Doto run (${n.hidden.length} ≤ ${d.hidden.length})`,
       n.hidden.length <= d.hidden.length);
  }
  for (const W of SR.PORTRAIT_WIDTHS) {
    ok(`portrait ${W} pt: Nixie's row 4 no wider than the RC18 Doto run`,
       SR.portraitClockRow('nixie', W).need <= SR.portraitClockRow('doto', W).need);
  }
}

console.log(`${fails ? 'FAIL' : 'ok'}  faceplate screen font: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
