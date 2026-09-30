/**
 * Faceplate rules (src/constants/faceplate.ts) — the Nixie rule, the allowed-colour table, the
 * Display side effects, the font migration and the "default renders today" tokens.
 *
 * Brief: docs/BRIEF-faceplates.md §1, §2, §12, acceptance §13.1 / §13.3 / §13.4.
 *
 * Run: node --no-warnings scripts/test_faceplate.ts   (Node 26 runs the TypeScript itself; run-tests.sh does)
 */
import {
  DEFAULT_SETTINGS, DISPLAYS, TEXTS, CONTROLS, TEXT_ALLOWED, NEON_TEXT, FONT_NIXIE,
  resolveTextColour, resolveFaceplate, resolveControlsColour, withDisplay, withText,
  parseSettings, migrateLegacyFont, ledA, hotA, LED,
  type FaceplateSettings,
} from '../src/constants/faceplate.ts';

let fails = 0, passes = 0;
function eq(what: string, got: unknown, want: unknown) {
  const g = JSON.stringify(got), w = JSON.stringify(want);
  if (g === w) { passes++; return; }
  fails++;
  console.error(`FAIL ${what}\n   got  ${g}\n   want ${w}`);
}
const ok = (what: string, cond: boolean) => eq(what, cond, true);

// ── §2: Nixie is neon whatever the text colour ───────────────────────────────
for (const t of TEXTS) {
  const r = resolveTextColour('nixie', t);
  eq(`nixie + ${t} → neon`, [r.name, r.core, r.isNeon], ['neon', NEON_TEXT.core, true]);
}

// ── §1 / §13.4: dot and seg never resolve to white; every result is allowed ──
for (const d of DISPLAYS) {
  for (const t of TEXTS) {
    const r = resolveTextColour(d, t);
    if (d === 'nixie') continue;
    ok(`${d} + ${t} resolves to an allowed colour (${r.name})`, TEXT_ALLOWED[d].includes(r.name as any));
    if (d === 'dot' || d === 'seg') ok(`${d} + ${t} is never white`, r.name !== 'white');
    if (TEXT_ALLOWED[d].includes(t)) eq(`${d} keeps an allowed ${t}`, r.name, t);
  }
}
eq('dot + white falls back to the first allowed (teal)', resolveTextColour('dot', 'white').name, 'teal');

// ── §13.3: no Nixie One glyph in any colour but neon, over EVERY combination ─
const isNeonish = (c: string) => c === NEON_TEXT.core || c === NEON_TEXT.mode || c === NEON_TEXT.reading
  || c.startsWith('rgba(255,106,20,');
for (const chassis of ['default', 'silver', 'black'] as const) {
  for (const display of DISPLAYS) {
    for (const controls of CONTROLS) {
      for (const text of TEXTS) {
        const th = resolveFaceplate({ ...DEFAULT_SETTINGS, chassis, display, controls, text });
        const tag = `${chassis}/${display}/${controls}/${text}`;
        const d = th.deck;
        if (d.freqFont === FONT_NIXIE) ok(`${tag}: Nixie freq is neon`, isNeonish(d.freq) && isNeonish(d.unit));
        if (d.bannerFont === FONT_NIXIE) ok(`${tag}: Nixie banner is neon`, isNeonish(d.bannerFree) && isNeonish(d.bannerAsk));
        if (d.modeFont === FONT_NIXIE) ok(`${tag}: Nixie mode is neon`, isNeonish(d.mode) && isNeonish(d.reading) && isNeonish(d.sqlClosed));
        if (th.keyLegend.font === FONT_NIXIE) ok(`${tag}: Nixie key legend is neon`, isNeonish(th.keyLegend.color));
        if (th.vts.font === FONT_NIXIE) {
          ok(`${tag}: Nixie VTS is neon`, [th.vts.onTune, th.vts.offTune, th.vts.band, th.vts.sub, th.vts.offset, th.vts.dim].every(isNeonish));
          ok(`${tag}: Nixie VTS ignores colour overrides`, th.vts.allowOverride === false);
        }
        if (display === 'dot' || display === 'seg') ok(`${tag}: §2 TRAP — dot/seg key legend is not Nixie One`, th.keyLegend.font !== FONT_NIXIE);
      }
    }
  }
}

// ── §1 side effects ───────────────────────────────────────────────────────────
const base: FaceplateSettings = { ...DEFAULT_SETTINGS };
const nix = withDisplay(base, 'nixie');
eq('choosing Nixie switches controls to neon', nix.controls, 'neon');
eq('leaving Nixie with neon controls resets them to amber', withDisplay(nix, 'hyper').controls, 'amber');
eq('leaving Nixie with changed controls keeps them', withDisplay({ ...nix, controls: 'blue' }, 'hyper').controls, 'blue');
const white = withText(base, 'white');
eq('hyper takes white', white.text, 'white');
const toDot = withDisplay(white, 'dot');
eq('hyper-white → dot falls back to teal', toDot.text, 'teal');
eq('… and back to hyper restores white (remembered per display)', withDisplay(toDot, 'hyper').text, 'white');
eq('withText refuses white on dot', withText(toDot, 'white').text, 'teal');
eq('withText refuses anything under Nixie', withText(nix, 'red'), nix);
eq('red survives hyper → seg (allowed there)', withDisplay(withText(base, 'red'), 'seg').text, 'red');

// ── §1 migration ──────────────────────────────────────────────────────────────
eq('legacy Atkinson (white) → hyper', migrateLegacyFont('white'), 'hyper');
eq('legacy Nixie One (amber) → nixie', migrateLegacyFont('amber'), 'nixie');
eq('nothing stored, legacy white → the §1 defaults', parseSettings(null, 'white'), DEFAULT_SETTINGS);
const migAmber = parseSettings(null, 'amber');
eq('nothing stored, legacy amber → nixie with neon controls', [migAmber.display, migAmber.controls], ['nixie', 'neon']);
eq('garbage JSON → defaults', parseSettings('{not json', 'white'), DEFAULT_SETTINGS);
const tampered = parseSettings(JSON.stringify({ display: 'seg', text: 'white', chassis: 'gold', meter: 'vu',
                                                textByDisplay: { dot: 'white', hyper: 'blue' } }));
eq('stored white on seg is clamped on load (§13.4)', tampered.text, 'teal');
eq('unknown chassis → default', tampered.chassis, 'default');
eq('stored per-display white on dot is dropped', tampered.textByDisplay, { hyper: 'blue' });
eq('round trip', parseSettings(JSON.stringify(tampered)), tampered);

// ── §13.1: the default deck is today's deck ──────────────────────────────────
const def = resolveFaceplate(DEFAULT_SETTINGS);
eq('default freq/unit/mode/reading are today\'s white-theme values',
   [def.deck.freq, def.deck.freqGlow, def.deck.unit, def.deck.mode, def.deck.reading, def.deck.sqlClosed, def.deck.freqFont],
   ['#ffffff', 'rgba(255,255,255,0.50)', '#b0b8c8', '#ffffff', '#b0b8c8', '#ff4040', 'Atkinson Hyperlegible']);
eq('default key legend is today\'s white Atkinson, no glow', def.keyLegend, { color: '#ffffff', font: 'Atkinson Hyperlegible', glow: null });
// DrumWheel/TunerKeys drew hsl(120,100%,45%) (and 78% for the lit symbol) — the default green must
// produce the identical strings.
const g = resolveControlsColour('default', 'green');
eq('default green glow = the old G(0.3)', ledA(g, 0.3), 'hsla(120,100%,45%,0.3)');
eq('default green glow clamps like G()', ledA(g, 1.4), 'hsla(120,100%,45%,1)');
eq('default green hot = the old hsl(120,100,78)', hotA(g, 0.95), 'hsla(120,100%,78%,0.95)');
eq('silver green is the brief\'s LED', resolveControlsColour('silver', 'green').rgb, LED.green.rgb);
eq('neon controls on the default chassis light the legends neon', resolveFaceplate({ ...DEFAULT_SETTINGS, controls: 'neon' }).keyLegend.color, NEON_TEXT.core);

console.log(`${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
