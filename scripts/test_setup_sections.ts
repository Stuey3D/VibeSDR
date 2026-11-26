// test_setup_sections.ts — ★★★ beta96 b96-setupux the app's server screen in COLLAPSIBLE SECTIONS, in STUART'S ORDER
// (src/utils/setupSections.ts + src/screens/ServerModeScreen.tsx). Silent, no phone.
//   node --no-warnings scripts/test_setup_sections.ts
import { readFileSync } from 'node:fs';
import { EMPTY_MEMO, RADIO_ORDER, SERVER_ORDER, headerLine, locationMust, mustCleared, opModeSummary, orderSections, parseStored,
         sectionOpen, setAllSections, toggleSection, secState, startRefusal, triedAfterStart, triedPrune, reopenFor, RULES, rulesAgree,
         FULL_TABS, radioTabLabel } from '../src/utils/setupSections.ts';

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
ok(/\{tabNow === 'server' && secRender\(serverSecs, SERVER_ORDER\)\}/.test(S) && /\{tabNow === 'radio' && secRender\(radioSecs, RADIO_ORDER\)\}/.test(S),
   'both groups are drawn through the order — each on its own TAB (SERVER | RADIO)');
ok(/serverChoiceSections\.devicePassword \? \[serverChoiceSections\.devicePassword\]/.test(S), 'b96-serverchoice: the device password section has its slot');
const top = S.indexOf('{serverChoiceTop}'), all = S.indexOf('<SectionAllRow'), firstSec = S.indexOf("{tabNow === 'server' && secRender(serverSecs");
ok(top > 0 && top < all && all < firstSec, 'b96-serverchoice: the TOP BAR slot is above OPEN ALL / CLOSE ALL and every section');
ok(/AsyncStorage\.getItem\(SECTIONS_KEY\)/.test(S) && /AsyncStorage\.setItem\(SECTIONS_KEY/.test(S), 'remembered per device (AsyncStorage)');
const bodies = [...S.matchAll(/const secBody_([a-zA-Z]+) =/g)].map((m) => m[1]);
for (const b of bodies) {
  const uses = S.split(`secBody_${b}`).length - 2;
  ok(uses >= 1, `secBody_${b} is drawn (${uses})`);
}
ok(!/>WATERFALL RATE</.test(S) && /title: 'Frame rate'/.test(S), 'the waterfall rate is the Frame rate section');
ok(/must: blockOf\('security'\)/.test(S) && /must: blockOf\('adsbFeed'\)/.test(S) && /must: blockOf\('hardware'\)/.test(S)
   && /must: blockOf\('background'\)/.test(S) && /advise: locationMust\(/.test(S) && /advise: pskrOn && !pskrCall\.trim\(\)/.test(S)
   && /advise: remoteAccess \? '' :/.test(S),
   'blocking = Start\'s own refusals (admin password, flight trackers, Icom CI-V, background); amber = location, PSK Reporter name, remote settings off');

// ── ★★★ beta96 (Stuart 11-26) THREE STATES, RED ONLY ON START ──
{
  const g = secState({ summary: 'Opus', block: '', advise: '', tried: false });
  ok(g.tone === 'green' && g.line === 'Opus' && !g.must, 'nothing to say: GREEN, the summary');
  const a = secState({ summary: 'Not set', block: '', advise: 'Location not set: satellites and maps need it', tried: true });
  ok(a.tone === 'amber' && /^Location not set/.test(a.line) && !a.must, 'advice: AMBER with the reason, never red (even after START), never opens');
  const b0 = secState({ summary: 'No admin password', block: 'No admin password: the server will not start without one', advise: '', tried: false });
  ok(b0.tone === 'amber' && !b0.must && !b0.line.startsWith('!'), 'a blocker BEFORE any START: amber and calm — browsing never shouts');
  const b1 = secState({ summary: '', block: 'No admin password: the server will not start without one', advise: '', tried: true });
  ok(b1.tone === 'red' && b1.must && b1.line === '! No admin password: the server will not start without one', 'after a refused START: RED with "!" and the reason');
  ok(sectionOpen(EMPTY_MEMO, 'security', b1.must) && !sectionOpen(EMPTY_MEMO, 'security', b0.must), '…and only then does it open by itself');
  // a fresh config: the app's classification
  const fresh = [
    { key: 'security', group: 'server' as const, title: 'Admin password and PIN', block: 'No admin password: the server will not start without one' },
    { key: 'adsbFeed', group: 'server' as const, title: 'Share ADS-B with flight trackers', block: '' },
    { key: 'hardware', group: 'radio' as const, title: 'Icom transceiver', block: 'The Icom\u2019s CI-V address is two hex digits' },
    { key: 'background', group: 'radio' as const, title: 'Background decoding', block: '' },
  ];
  const r = startRefusal(fresh)!;
  ok(r && r.first.key === 'security' && r.first.group === 'server' && r.all.length === 2, 'START on a fresh config: refused, the SERVER tab\'s blocker first, both marked');
  ok(/^Not started — Admin password and PIN: No admin password/.test(r.line) && /1 more, marked red/.test(r.line), `the one line names what is missing: "${r.line}"`);
  ok(startRefusal(fresh.map((x) => ({ ...x, block: '' }))) === null, 'nothing missing: START goes');
  const onlyRadio = startRefusal(fresh.map((x) => (x.key === 'security' ? { ...x, block: '' } : x)))!;
  ok(onlyRadio.first.group === 'radio', 'a blocker only on the RADIO tab: that tab is selected');
  let tried = triedAfterStart({}, r.all.map((x) => x.key));
  ok(tried.security && tried.hardware, 'every blocker marked tried');
  tried = triedPrune(tried, (k) => k === 'hardware');
  ok(!tried.security && tried.hardware, 'fixed = forgotten (red until fixed, then calm); the other stays red');
  ok(triedPrune(tried, () => true) === tried, 'nothing fixed: the same object (no re-render)');
  const m = reopenFor({ ...EMPTY_MEMO, dismissed: { security: 'x' } }, ['security']);
  ok(!('security' in m.dismissed), 'a refused START reopens a section closed by hand while red');
  // one rule set with the web page
  ok(rulesAgree().length === 0, `a rule on both sides is the same kind on both (${rulesAgree().join() || 'all agree'})`);
  ok(RULES.web.advise.includes('pskr-no-callsign') && RULES.app.advise.includes('pskr-no-callsign') && RULES.web.advise.includes('location') && RULES.app.advise.includes('location')
     && RULES.web.block.includes('flight-trackers') && RULES.app.block.includes('flight-trackers'), 'the shared rules: location + PSK Reporter name amber, flight trackers blocking — on both');
  const page = readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
  const m2 = page.match(/const SEC_RULES = \{ block: \[([^\]]*)\],\s*advise: \[([^\]]*)\] \}/);
  const pl = (x: string) => [...x.matchAll(/"([a-z-]+)"/g)].map((y) => y[1]).join();
  ok(!!m2 && pl(m2[1]) === RULES.web.block.join() && pl(m2[2]) === RULES.web.advise.join(), 'the web page\'s SEC_RULES is this file\'s RULES.web, word for word');
  ok(/const SEC_BLOCK = \{/.test(page) && /const SEC_ADVISE = \{/.test(page) && /function secSaveStops\(master\)/.test(page) && !/const SEC_MUST/.test(page),
     'the web page: blocking / advisory tables and the Save gate (the old always-red SEC_MUST is gone)');
  // the tabs
  ok(FULL_TABS.map((t) => t.label).join() === 'SERVER,RADIO' && radioTabLabel('RSP1B') === 'RADIO · RSP1B' && radioTabLabel('') === 'RADIO', 'the tabs: SERVER | RADIO · <the radio>');
  ok(!/const secBody_mode =/.test(S) && !/setAdvanced/.test(S) && /const advanced = webServer;/.test(S), 'Full\'s Simple / Advanced toggle is gone (advanced follows the web client)');
  ok(!/startBlockedBy=/.test(S) && /refusal=\{startRefused/.test(S) && /onPress=\{pressStart\} disabled=\{starting\}/.test(S),
     'no red START while browsing: both START keys are green and run pressStart; the refusal line only after a press');
}
// the hooks live before the screen's early returns
const hookAt = S.indexOf('useState<SectionMemo>'), early = S.indexOf("if (Platform.OS !== 'android' || !vibeServerSupported)");
ok(hookAt > 0 && hookAt < early, 'the section memory is a hook declared before the early returns');

console.log(`setup sections (app): ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
