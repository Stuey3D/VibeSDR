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
  CHASSIS_CHOICES, DISPLAY_CHOICES, METER_CHOICES, TRANSPARENCY_CHOICES, CHASSIS, METERS, TRANSPARENCIES,
  COLOUR_NAMES, textChoices, controlsDot, feelRows,
  chassisTokens,
  type FaceplateSettings,
} from '../src/constants/faceplate.ts';
import { createDomeClick, DOME_PRESS_MS, DOME_RELEASE_MS } from '../src/components/domeClick.ts';

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
        if (d.freqFont === FONT_NIXIE) ok(`${tag}: Nixie freq is neon`, isNeonish(d.freq));
        if (d.unitFont === FONT_NIXIE) ok(`${tag}: Nixie unit is neon`, isNeonish(d.unit));
        ok(`${tag}: the unit label is never Nixie One unless neon`, d.unitFont !== FONT_NIXIE || isNeonish(d.unit));
        eq(`${tag}: the frequency window follows the display`, d.style, display);
        if (display === 'nixie') ok(`${tag}: nixie mark is neon`, isNeonish(th.vts.mark) && th.vts.font === FONT_NIXIE);
        if (display === 'dot' || display === 'seg') {
          ok(`${tag}: §13.4 no white on a VFD (deck)`, d.core !== LED.white.core && d.freq !== LED.white.core && d.freq !== LED.white.hot);
          ok(`${tag}: §13.4 no white on a VFD (VTS)`, th.vts.core !== LED.white.core && th.vts.mark !== LED.white.core);
          eq(`${tag}: VFD VTS is upper case and ignores colour overrides`, [th.vts.upper, th.vts.allowOverride], [true, false]);
        }
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
eq('default key legend is today\'s white Atkinson, no glow', def.keyLegend,
   { color: '#ffffff', font: 'Atkinson Hyperlegible', glow: null, hot: '#ffffff', shade: null });
// DrumWheel/TunerKeys drew hsl(120,100%,45%) (and 78% for the lit symbol) — the default green must
// produce the identical strings.
const g = resolveControlsColour('default', 'green');
eq('default green glow = the old G(0.3)', ledA(g, 0.3), 'hsla(120,100%,45%,0.3)');
eq('default green glow clamps like G()', ledA(g, 1.4), 'hsla(120,100%,45%,1)');
eq('default green hot = the old hsl(120,100,78)', hotA(g, 0.95), 'hsla(120,100%,78%,0.95)');
eq('silver green is the brief\'s LED', resolveControlsColour('silver', 'green').rgb, LED.green.rgb);
eq('neon controls on the default chassis light the legends neon', resolveFaceplate({ ...DEFAULT_SETTINGS, controls: 'neon' }).keyLegend.color, NEON_TEXT.core);

// ── §1: the CONTROL CUSTOMISATION pane's rules ───────────────────────────────
eq('pane offers every chassis', CHASSIS_CHOICES.map(c => c.value).sort(), [...CHASSIS].sort());
eq('pane offers every display, in the mockup\'s order', DISPLAY_CHOICES.map(c => c.label), ['NIXIE', 'HYPER', 'DOT', 'VCR']);
eq('pane offers every display', DISPLAY_CHOICES.map(c => c.value).sort(), [...DISPLAYS].sort());
eq('pane offers every meter', METER_CHOICES.map(c => c.value), METERS);
eq('pane offers TRANSPARENCY EFFECTS on and off', TRANSPARENCY_CHOICES.map(c => c.value), TRANSPARENCIES);
eq('TRANSPARENCY EFFECTS keys read ON / OFF', TRANSPARENCY_CHOICES.map(c => c.label), ['ON', 'OFF']);
eq('Nixie: the TEXT row is the locked note, not keys', textChoices('nixie'), null);
for (const d of ['dot', 'seg'] as const) {
  ok(`${d}: TEXT never offers white (§13.4)`, !textChoices(d)!.includes('white'));
  eq(`${d}: TEXT offers the VFD colours, teal first`, textChoices(d), ['teal', 'green', 'blue', 'amber', 'red']);
}
eq('hyper: TEXT offers all six', textChoices('hyper')!.length, 6);
// Every colour the pane can pick must survive withText — or the key would light and do nothing.
for (const d of DISPLAYS) for (const t of textChoices(d) ?? []) {
  eq(`${d}: picking ${t} sticks`, withText(withDisplay(DEFAULT_SETTINGS, d), t).text, t);
}
eq('HAPTICS hidden without a motor; STEADY LEDS and MOTION EFFECTS always', feelRows(false), ['steadyLeds', 'motion']);
eq('FEEL order with a motor', feelRows(true), ['haptics', 'steadyLeds', 'motion']);
eq('default-chassis green dot is today\'s drum green', controlsDot('default', 'green'), 'rgb(0,230,0)');
eq('silver green dot is the brief\'s LED', controlsDot('silver', 'green'), LED.green.core);
ok('every colour key has a spoken name', [...CONTROLS, ...TEXTS].every(c => !!COLOUR_NAMES[c]));
eq('steadyLeds defaults off', DEFAULT_SETTINGS.steadyLeds, false);
eq('steadyLeds round-trips', parseSettings(JSON.stringify({ ...DEFAULT_SETTINGS, steadyLeds: true })).steadyLeds, true);
eq('steadyLeds: garbage → off', parseSettings(JSON.stringify({ steadyLeds: 'yes' })).steadyLeds, false);

// ── §3 / §5: chassis tokens ───────────────────────────────────────────────────
eq('default chassis has no plate (today\'s glass island)', chassisTokens('default').plate, null);
eq('default keys keep today\'s outline look', chassisTokens('default').dome.look, 'outline');
for (const c of ['silver', 'black'] as const) {
  const t = chassisTokens(c);
  ok(`${c} has an opaque plate`, !!t.plate && t.plate.texture === c);
  eq(`${c} keys are caps`, t.dome.look, 'cap');
  eq(`${c} lighting stops line up`, t.plate!.lightColors.length, t.plate!.lightPos.length);
  const th = resolveFaceplate({ ...DEFAULT_SETTINGS, chassis: c, controls: 'red' });
  eq(`${c} legend is the controls colour, hot when clicked`, [th.keyLegend.color, th.keyLegend.hot], [LED.red.core, LED.red.hot]);
  ok(`${c} legend is never Nixie One`, resolveFaceplate({ ...DEFAULT_SETTINGS, chassis: c, display: 'nixie', controls: 'neon' }).keyLegend.font !== FONT_NIXIE);
}
eq('screws on silver only (§3.2 / §3.3)', [chassisTokens('silver').plate!.screws, chassisTokens('black').plate!.screws], [true, false]);
eq('gloss panel on black only', [chassisTokens('silver').plate!.gloss, chassisTokens('black').plate!.gloss], [false, true]);
eq('press dim .84 silver / .82 black (§5)', [chassisTokens('silver').dome.pressDim, chassisTokens('black').dome.pressDim], [0.84, 0.82]);
eq('silver engraving shadow; black none', [resolveFaceplate({ ...DEFAULT_SETTINGS, chassis: 'silver' }).keyLegend.shade,
   resolveFaceplate({ ...DEFAULT_SETTINGS, chassis: 'black' }).keyLegend.shade], ['rgba(0,0,0,0.6)', null]);

// ── Row 4: the default deck's VTS and frequency window are today's ───────────
eq('default VTS keeps today\'s palette', [def.vts.onTune, def.vts.offTune, def.vts.band, def.vts.font, def.vts.upper, def.vts.markGlow],
   ['rgba(80,220,100,0.95)', 'rgba(255,200,80,0.95)', '#ffe566', 'Atkinson Hyperlegible', false, null]);
eq('default frequency: today\'s spacing and glow', [def.deck.style, def.deck.freqSpacing, def.deck.modeGlow], ['hyper', 1.5, 'rgba(255,160,0,0.6)']);
eq('silver hyper: letter-spacing 2.5 in the text colour', (() => { const t = resolveFaceplate({ ...DEFAULT_SETTINGS, chassis: 'silver', text: 'amber' });
   return [t.deck.freqSpacing, t.deck.freq, t.vts.mark]; })(), [2.5, LED.amber.hot, LED.amber.core]);
eq('default chassis, red text: the VTS goes red, the digits stay white', (() => { const t = resolveFaceplate({ ...DEFAULT_SETTINGS, text: 'red' });
   return [t.vts.onTune, t.deck.freq]; })(), [LED.red.core, '#ffffff']);

// ── §5: when a dome key clicks (domeClick.ts, with a fake clock) ──────────────
{
  let t = 0; const q: { at: number; f: () => void; id: number }[] = []; let nid = 0;
  const log: string[] = [];
  const run = (until: number) => {
    for (;;) {
      q.sort((a, b) => a.at - b.at);
      const n = q[0];
      if (!n || n.at > until) break;
      q.shift(); t = n.at; n.f();
    }
    t = until;
  };
  const dc = createDomeClick({
    press: () => log.push(`press@${t}`), release: () => log.push(`release@${t}`),
    setTimer: (ms, f) => { const id = ++nid; q.push({ at: t + ms, f, id }); return id; },
    clearTimer: (id) => { const i = q.findIndex(x => x.id === id); if (i >= 0) q.splice(i, 1); },
    now: () => t,
  });
  dc.down(); run(44);
  eq('no click before the snap', log, []);
  run(45);
  eq('the press click lands at the END of the 45 ms curve', log, ['press@45']);
  run(200); dc.up();
  eq('release clicks at once after a full press', log, ['press@45', 'release@200']);
  log.length = 0; t = 1000;
  dc.down(); run(1010); dc.up();
  eq('a 10 ms tap: press on lift, release one release-curve later', log, ['press@1010']);
  run(1045);
  eq('… and the release 35 ms after', log, ['press@1010', `release@${1010 + DOME_RELEASE_MS}`]);
  log.length = 0;
  dc.up(); dc.up();
  eq('a second release for the same press never clicks', log, []);
  dc.down(); run(t + 10); dc.dispose(); run(t + 500);
  eq('dispose drops a queued click silently', log, []);
  eq('§5 timings', [DOME_PRESS_MS, DOME_RELEASE_MS], [45, 35]);
}

// ── §4.6 SQL in the mode box, on every meter: red, neon #ff9a55 under Nixie (the rule outranks red) ──
for (const chassis of CHASSIS) for (const display of DISPLAYS) for (const text of TEXTS) for (const meter of METERS) {
  const d = resolveFaceplate({ ...DEFAULT_SETTINGS, chassis, display, text, meter }).deck;
  const tag = `${chassis}/${display}/${text}/${meter}`;
  if (display === 'nixie') {
    eq(`${tag}: SQL neon`, [d.sqlClosed, d.sqlGlow], ['#ff9a55', 'rgba(255,90,10,0.8)']);
  } else {
    eq(`${tag}: SQL red`, d.sqlClosed, '#ff4040');
    // The default deck's SQL has no glow today; silver / black / VFD take the mockup's red glow.
    eq(`${tag}: SQL glow`, d.sqlGlow, chassis === 'default' && display === 'hyper' ? null : 'rgba(255,64,64,0.6)');
  }
}

// ── ICON & ART colour (2026-10-04) ───────────────────────────────────────────
{
  const { parseSettings: ps, DEFAULT_SETTINGS: D, ICON_COLOURS: IC } = await import('../src/constants/faceplate.ts');
  ok('icon & art: the default is the shipped green', D.iconColour === 'green');
  ok('icon & art: a stored pick is kept', ps(JSON.stringify({ iconColour: 'blue' })).iconColour === 'blue');
  ok('icon & art: an unknown value falls back to green', ps(JSON.stringify({ iconColour: 'mauve' })).iconColour === 'green');
  ok('icon & art: older stores (no field) read green', ps(JSON.stringify({ controls: 'blue' })).iconColour === 'green');
  ok('icon & art: every illumination colour offered, teal included', IC.join() === 'green,red,amber,blue,white,teal,neon');
}

console.log(`${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
