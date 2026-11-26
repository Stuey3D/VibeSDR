// test_setup_sections.ts — ★★★ beta96 b96-setupux the app's server screen in COLLAPSIBLE SECTIONS, in STUART'S ORDER
// (src/utils/setupSections.ts + src/screens/ServerModeScreen.tsx). Silent, no phone.
//   node --no-warnings scripts/test_setup_sections.ts
import { readFileSync } from 'node:fs';
import { EMPTY_MEMO, RADIO_ORDER, SERVER_ORDER, headerLine, locationMust, mustCleared, opModeSummary, orderSections, parseStored,
         sectionOpen, setAllSections, toggleSection } from '../src/utils/setupSections.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };

// ── Stuart's order ──
ok(RADIO_ORDER.slice(0, 11).join() === 'name,opMode,listeners,antenna,hardware,whereTune,gainLimits,modes,rawIq,landing,background',
   'radio: name · operating mode · listeners · antenna · rate+gain · where listeners may tune · gain limits · modes · raw IQ · landing · background');
ok(SERVER_ORDER.indexOf('bench') < SERVER_ORDER.indexOf('devicePassword') && SERVER_ORDER.indexOf('devicePassword') < SERVER_ORDER.indexOf('location')
   && SERVER_ORDER.indexOf('location') < SERVER_ORDER.indexOf('community') && SERVER_ORDER.indexOf('community') < SERVER_ORDER.indexOf('audio')
   && SERVER_ORDER.indexOf('audio') < SERVER_ORDER.indexOf('frameRate') && SERVER_ORDER.indexOf('frameRate') < SERVER_ORDER.indexOf('eibi'),
   'server: benchmark · [device password hook] · networking incl. location · sharing · audio · frame rate · EiBi');
{
  const got = orderSections([{ key: 'eibi' }, { key: 'zzNew' }, { key: 'bench' }, { key: 'audio' }, { key: 'aaNew' }], SERVER_ORDER).map((s) => s.key);
  ok(got.join() === 'bench,audio,eibi,zzNew,aaNew', `unknown keys after the listed ones, in their given order (${got.join()})`);
}

// ── open / closed ──
{
  let m = EMPTY_MEMO;
  ok(!sectionOpen(m, 'audio', ''), 'closed by default');
  ok(sectionOpen(m, 'location', 'Location not set'), 'a red "!" opens its section by itself');
  m = toggleSection(m, 'location', 'Location not set');
  ok(!sectionOpen(m, 'location', 'Location not set'), 'closed by hand while red: stays closed for that reason');
  ok(sectionOpen(m, 'location', 'Another reason'), '…a different reason opens it again');
  m = toggleSection(m, 'audio', '');
  ok(sectionOpen(m, 'audio', '') && m.stored.audio === 1, 'a press opens it and remembers (1)');
  m = toggleSection(m, 'audio', '');
  ok(!sectionOpen(m, 'audio', '') && m.stored.audio === 0, 'pressed again: closed, remembered (0)');
  // the "!" cleared while open: it stays open under the owner's hands
  let r = EMPTY_MEMO;
  ok(sectionOpen(r, 'pskr', 'PSK Reporter needs a name'), 'PSK Reporter red: open');
  r = mustCleared(r, 'pskr');
  ok(sectionOpen(r, 'pskr', ''), 'the name typed: the "!" clears and the section stays open');
  r = toggleSection(r, 'pskr', '');
  ok(!sectionOpen(r, 'pskr', '') && !r.stay.pskr, 'closed by hand: the stay is gone');
  // all
  const items = [{ key: 'a', must: '' }, { key: 'b', must: 'x' }];
  let all = setAllSections(EMPTY_MEMO, items, true);
  ok(items.every((i) => sectionOpen(all, i.key, i.must)), 'OPEN ALL opens every section');
  all = setAllSections(all, items, false);
  ok(items.every((i) => !sectionOpen(all, i.key, i.must)), 'CLOSE ALL closes every section (a red one too, until its reason changes)');
  ok(JSON.stringify(parseStored(JSON.stringify(all.stored))) === JSON.stringify({ a: 0, b: 0 }), 'the stored map round-trips');
  ok(JSON.stringify(parseStored('garbage')) === '{}' && JSON.stringify(parseStored('[1]')) === '{}' && JSON.stringify(parseStored('{"a":7,"b":1}')) === '{"b":1}',
     'an unreadable store is "all closed", odd values dropped');
}

// ── the one line ──
ok(opModeSummary('single', '', '', 0, 0) === 'Unlocked', 'Operating mode · Unlocked');
ok(opModeSummary('locked', '', '', 98e6, 20e6) === 'Locked 88–108 MHz', `Operating mode · ${opModeSummary('locked', '', '', 98e6, 20e6)}`);
ok(opModeSummary('locked', 'general', '', 98e6, 0) === 'Locked', 'locked with the listener\'s span: Locked');
ok(opModeSummary('single', 'adsb', 'ADS-B', 0, 0) === 'Single use · ADS-B', 'Operating mode · Single use · ADS-B');
ok(headerLine('AIS', '') === 'AIS' && headerLine('AIS', 'No location') === '! No location', 'the header line: the summary, or "! " + the reason');
ok(locationMust('off', false, false) === 'Location not set: satellites and maps need it', 'no location: the "!"');
ok(locationMust('manual', false, false) !== '' && locationMust('manual', true, false) === '' && locationMust('off', false, true) === '',
   'a town typed, or on the move: no "!"');

// ── the screen ──
const S = readFileSync(new URL('../src/screens/ServerModeScreen.tsx', import.meta.url), 'utf8');
const keysOf = (name: string) => { const a = S.indexOf(`const ${name}: SecDef[] =`); const b = S.indexOf('];', a); return [...S.slice(a, b).matchAll(/\{ key: '([a-zA-Z]+)'/g)].map((m) => m[1]); };
const srv = keysOf('serverSecs'), rad = keysOf('radioSecs');
ok(srv.length >= 15 && srv.every((k) => SERVER_ORDER.includes(k)), `every server section has a place in SERVER_ORDER (${srv.join(' ')})`);
ok(rad.length >= 17 && rad.every((k) => RADIO_ORDER.includes(k)), `every radio section has a place in RADIO_ORDER (${rad.join(' ')})`);
ok(orderSections(rad.map((key) => ({ key })), RADIO_ORDER).map((s) => s.key).join() === RADIO_ORDER.filter((k) => rad.includes(k)).join(),
   'the radio sections come out in Stuart\'s order');
ok(/\{secRender\(serverSecs, SERVER_ORDER\)\}/.test(S) && /\{secRender\(radioSecs, RADIO_ORDER\)\}/.test(S), 'both groups are drawn through the order');
ok(/serverChoiceSections\.devicePassword \? \[serverChoiceSections\.devicePassword\]/.test(S), 'b96-serverchoice: the device password section has its slot');
const top = S.indexOf('{serverChoiceTop}'), all = S.indexOf('<SectionAllRow'), firstSec = S.indexOf('{secRender(serverSecs');
ok(top > 0 && top < all && all < firstSec, 'b96-serverchoice: the TOP BAR slot is above OPEN ALL / CLOSE ALL and every section');
ok(/AsyncStorage\.getItem\(SECTIONS_KEY\)/.test(S) && /AsyncStorage\.setItem\(SECTIONS_KEY/.test(S), 'remembered per device (AsyncStorage)');
const bodies = [...S.matchAll(/const secBody_([a-zA-Z]+) =/g)].map((m) => m[1]);
for (const b of bodies) {
  const uses = S.split(`secBody_${b}`).length - 2;
  ok(uses >= 1, `secBody_${b} is drawn (${uses})`);
}
ok(!/>WATERFALL RATE</.test(S) && /title: 'Frame rate'/.test(S), 'the waterfall rate is the Frame rate section');
ok(/must: needsAdminPw \?/.test(S) && /must: afProblem/.test(S) && /must: locationMust\(/.test(S),
   'the red "!" comes from Start\'s own refusals (admin password, flight trackers) and the satellite engine\'s location rule');
// the hooks live before the screen's early returns
const hookAt = S.indexOf('useState<SectionMemo>'), early = S.indexOf("if (Platform.OS !== 'android' || !vibeServerSupported)");
ok(hookAt > 0 && hookAt < early, 'the section memory is a hook declared before the early returns');

console.log(`setup sections (app): ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
