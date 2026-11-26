/**
 * test-only-mode-clients.ts — V12 THE ONLY MODE in the clients, any whole-radio mode (Stuart, 2026-11-18: "the same thing we
 * did for DAB earlier where it is the only allowed mode, we need to do for all modes as the Almere server has an ADSB only
 * radio and I've accidentally closed the ADSB a couple of times meaning that it has to train the gain again").
 *   · ONE RULE, TWO READERS: the web page's onlyModeFrom and the app's give the same answer for every case — the server's
 *     word when it gave one (a new server; "" for the signed-in admin), an OLDER server's blocked modes among the modes it
 *     offers otherwise, never for the admin; and the two files are the same rule, word for word;
 *   · the web page: every EXIT key the rule removes, the note in its place, every way out guarded, the server's word only
 *     when sent; the dial message does not point at a key that is not there;
 *   · the app: the server's word only when sent, every way out guarded (EXIT, Back, the Mode sheet), the boxes' EXIT turned
 *     CLOSE with the note, the map's EXIT turned CLOSE MAP with the note;
 *   · the R36S: the same rule, no EXIT key on the map, BACK only closes it;
 *   · the setup page: the note under the modes list names whichever mode is left alone (its own function, run here).
 * Run: npx tsx scripts/test-only-mode-clients.ts
 */
import { readFileSync } from 'node:fs';
import { onlyModeFrom as web, onlyModeNote as webNote, onlyModeWhy as webWhy } from '../web/client/src/onlyMode';
import { onlyModeFrom as app, onlyModeNote as appNote, onlyModeWhy as appWhy } from '../src/services/onlyMode';

let fails = 0, passes = 0;
const ok = (c: boolean, what: string) => { if (c) passes++; else { fails++; console.log('  FAIL ' + what); } };
const read = (p: string) => readFileSync(new URL('../' + p, import.meta.url), 'utf8');
const S = (...a: string[]) => new Set(a);
const DEMODS_OLD = ['wfm', 'nfm', 'am', 'usb', 'lsb', 'cwu', 'cwl', 'drm'];
const ALL = S('dab', 'adsb', 'ism', 'ndb', 'ais', 'avdata', 'sondes');
const adsbOnlyOld = S(...DEMODS_OLD, 'dab', 'ism', 'ndb', 'ais', 'avdata', 'sondes');
const cases: Array<[string | null, Set<string>, boolean, Set<string>, string, string]> = [
  ['adsb', S(), false, ALL, 'adsb', 'a new server says ADS-B only'],
  ['', adsbOnlyOld, false, ALL, '', 'a new server says none (its word wins over the list)'],
  ['', adsbOnlyOld, true, ALL, '', 'the admin on a new server (the server says "")'],
  ['sondes', S(), false, ALL, 'sondes', 'a new server says SONDES only'],
  ['dab', S(), false, ALL, 'dab', 'a new server says DAB only (one case of the rule)'],
  ['bogus', S(), false, ALL, '', 'a word that is not a mode is none'],
  [null, adsbOnlyOld, false, ALL, 'adsb', '★ an OLDER server locked to ADS-B: worked out from its blocked modes'],
  [null, adsbOnlyOld, true, ALL, '', '★ …but never for the signed-in admin'],
  [null, S(...DEMODS_OLD, 'dab'), false, S('dab', 'adsb'), 'adsb', 'an older server offering only DAB and ADS-B, DAB off: ADS-B only'],
  [null, S(...DEMODS_OLD, 'dab'), false, ALL, '', '…but one that offers more modes, left on: none'],
  [null, S(...DEMODS_OLD), false, S('dab', 'adsb'), '', 'an older server with DAB and ADS-B both on: none'],
  [null, S('wfm', 'nfm', 'am', 'usb', 'dab', 'ism', 'ndb', 'ais', 'avdata', 'sondes'), false, ALL, '', 'LSB still on: none'],
  [null, S(...DEMODS_OLD, 'dab', 'adsb', 'ndb', 'ais', 'avdata', 'sondes'), false, ALL, 'ism', 'an older server locked to ISM'],
  [null, S(), false, ALL, '', 'nothing blocked'],
];
for (const [said, blocked, admin, offered, want, what] of cases) {
  ok(web(said, blocked, admin, offered) === want, `web: ${what}`);
  ok(app(said, blocked, admin, offered) === want, `app: ${what}`);
}
for (const m of ['adsb', 'ism', 'ndb', 'ais', 'avdata', 'sondes', 'vormap']) ok(webNote(m) === appNote(m) && webWhy(m) === appWhy(m), `the same words for ${m}`);
ok(webWhy('vormap') === 'This receiver is VOR / ILS map only \u2014 its owner has set it up for VOR / ILS map listeners.', 'the VOR map\'s reason is the server\'s words');
ok(webNote('adsb') === 'ADS-B only on this receiver' && webNote('avdata') === 'DATALINK only on this receiver', 'the note names the EXIT key\'s mode');
ok(webWhy('adsb') === 'This receiver is ADS-B only \u2014 its owner has set it up for ADS-B listeners.', 'the reason is the server\'s');
const body = (p: string) => read(p).split('\n').filter((l) => !l.startsWith('//')).join('\n');
ok(body('web/client/src/onlyMode.ts') === body('src/services/onlyMode.ts'), '★ the two files are the same rule, word for word');

// ── the web page ──
const main = read('web/client/src/main.ts'), spec = read('web/client/src/spectrum.ts');
ok(/if \(typeof msg\.onlyMode === 'string'\) this\.cb\.onOnlyMode\?\.\(msg\.onlyMode\);/.test(spec) && /onOnlyMode: \(m\) => \{ hwOnlyMode = m; syncOnlyMode\(\); \}/.test(main),
   '★ web: the server word only when sent (an older server is read from its list)');
ok(/return onlyModeFrom\(hwOnlyMode, blockedModes, adminUnlocked, offered\);/.test(main), 'web: the rule from the server word, the blocked list, the admin and the offer');
// ★ beta96 ADS-B, AIS and the DATALINK share the AIR & SEA map's one EXIT key — whichever is live owns it
for (const [id, k] of [['adsb', 'adsbOn ? trk(asView) : null'], ['ndb', "document.getElementById('ndbExit')"], ['ism', "document.getElementById('ismExit')"],
                       ['ais', 'aisOn ? trk(asView) : null'], ['avdata', 'avOn ? trk(asView) : null'], ['sondes', 'trk(sdView)'], ['vormap', 'trk(vmView)']])
  ok(main.includes(`['${id}', ${k}]`), `web: EXIT for ${id} is one the rule removes`);
ok(/ex\.style\.display = here \? 'none' : '';/.test(main) && /note\.textContent = onlyModeNote\(m\);/.test(main), '★ web: EXIT not drawn, the note in its place');
for (const [f, k] of [['adsbSetMode', 'adsb'], ['ismSetMode', 'ism'], ['ndbSetMode', 'ndb'], ['aisSetMode', 'ais'], ['avSetMode', 'avdata'], ['sdSetMode', 'sondes'], ['vmSetMode', 'vormap']])
  ok(new RegExp(`function ${f}\\(on: boolean\\) \\{\\n  if \\(!on && onlyModeRefuses\\('${k}'\\)\\) return;`).test(main), `web: every way out of ${k} goes through the guard (${f})`);
for (const f of ['adsbUiOn', 'ismUiOn', 'ndbUiOn', 'aisUiOn', 'avUiOn', 'sdUiOn', 'vmUiOn'])
  ok(new RegExp(`function ${f}\\(\\) \\{\\n(?:  wholeMapYield\\('[a-z]+'\\);[^\\n]*\\n)?[^\\n]*\\n  setTimeout\\(syncOnlyMode, 0\\);`).test(main), `web: ${f} re-reads the rule once its view is drawn`);
ok(/onlyModeNow\(\) === 'adsb' \? 'This receiver is ADS-B only/.test(main), 'web: the locked-dial message does not say "press EXIT ADS-B" on an ADS-B-only receiver');

// ── the app ──
const scr = read('src/screens/SDRScreen.tsx'), wsc = read('src/services/VibeServerWsClient.ts'), mo = read('src/components/MapOverlay.tsx');
ok(/if \(typeof msg\.onlyMode === 'string'\) this\.callbacks\.onHwOnlyMode\?\.\(msg\.onlyMode\);/.test(wsc), '★ app: only a server that SENDS onlyMode is believed');
ok(/const onlyModeHere = onlyModeFrom\(hwOnlyMode, blockedModes, adminOk,/.test(scr) && /onlyModeRef\.current = onlyModeHere;/.test(scr), 'app: the rule, the server word, the list, the admin');
ok(/setHwOnlyMode\(null\); onlyToldRef\.current\.clear\(\);/.test(scr), 'app: a new server starts as "has not said"');
for (const k of ['adsb', 'ism', 'ndb', 'ais', 'sondes', 'avdata', 'vormap'])
  ok((scr.match(new RegExp(`onlyRefuses\\('${k}'`, 'g')) || []).length >= 2, `app: EXIT/Back and the Mode sheet both refuse to leave ${k}`);
ok(/appAlert\(onlyModeNote\(k\), onlyModeWhy\(k\)\)/.test(scr), 'app: Back explains instead of leaving');
for (const f of ['AdsbPanel', 'IsmPanel', 'AvdataPanel', 'SondesPanel']) {
  const t = read(`src/components/${f}.tsx`);
  ok(/label=\{p\.on && !p\.only \? '[^']*' : 'CLOSE'\}/.test(t) && /\{!!p\.only && <Text/.test(t), `app: ${f} draws CLOSE with the note in place of EXIT`);
}
for (const k of ['adsb', 'ism', 'sondes', 'avdata']) ok(scr.includes(`only={onlyModeHere === '${k}' ? onlyModeNote('${k}') : undefined}`), `app: the ${k} box is told`);
// ★ beta96 the AIR & SEA map ('airsea') is told for the whole-radio mode it is the screen of (AIS now; ADS-B at stage 6)
ok(/only=\{shownMap === 'airsea' \? \(aisViewOpen && onlyModeHere === 'ais' \? onlyModeNote\('ais'\) : undefined\) : shownMap && onlyModeHere === shownMap \? onlyModeNote\(shownMap\) : undefined\}/.test(scr), 'app: the map is told');
ok(/\$\{opts\?\.only\s*\? `<button id="back"[^`]*>CLOSE MAP<\/button>`/.test(mo), '★ app: the map\'s EXIT becomes CLOSE MAP');

// ── the R36S ──
const mm = read('handheld-ui/src/map_modes.h'), mp = read('handheld-ui/src/map_page.h');
ok(/if \(h\.has\("onlyMode"\)\)/.test(mm) && /blocked\("wfm"\) && blocked\("nfm"\) && blocked\("am"\) && blocked\("usb"\) && blocked\("lsb"\)/.test(mm) && /return n == 1 \? left : std::string\(\);/.test(mm),
   '★ R36S: the same rule (the server word, else an older server\'s blocked modes among those offered)');
ok(/if \(!pick && onlyNote\.empty\(\)\) keys\.push_back\(\{P\.exitLabel,/.test(mp), 'R36S: no EXIT key on the map when it is the only mode');
ok(/if \(onlyModeHere\(s\) == id\) \{\s*mapClosePage\(a\);/.test(mm), 'R36S: BACK only closes the map, and says why');
ok(/" only on this receiver"/.test(mm), 'R36S: the header says it');

// ── the setup page (its own function, run here) ──
const setup = read('android/app/src/main/cpp/vibe_setup_page.h');
const grab = (re: RegExp) => { const m = setup.match(re); if (!m) throw new Error('not found: ' + re); return m[0]; };
const src = [grab(/const DAB_ONLY_OTHERS = \[[^\]]*\];/), grab(/function dabOnlyModes\(off, tree\) \{[\s\S]*?\n\}/),
             grab(/const ONLY_MODE_WHOLE = \{[^}]*\};/), grab(/function onlyModeOf\(off, tree\) \{[\s\S]*?\n\}/)].join('\n');
const onlyModeOf = new Function(src + '\nreturn onlyModeOf;')() as (off: Set<string>, tree: Array<{ id: string }>) => string;
const tree = ['wfm', 'nfm', 'am', 'usb', 'lsb', 'cwu', 'cwl', 'drm', 'dab', 'adsb', 'ism', 'ais', 'avdata', 'sondes'].map((id) => ({ id }));
const offBut = (keep: string) => new Set(tree.map((m) => m.id).filter((id) => id !== keep));
ok(onlyModeOf(offBut('adsb'), tree) === 'adsb', '★ setup page: ADS-B alone → the ADS-B note');
ok(onlyModeOf(offBut('dab'), tree) === 'dab', 'setup page: DAB alone → DAB\'s own note, as before');
ok(onlyModeOf(offBut('sondes'), tree) === 'sondes', 'setup page: SONDES alone');
ok(onlyModeOf(new Set(['wfm', 'nfm', 'am', 'usb', 'lsb', 'cwu', 'cwl', 'drm', 'dab', 'ism', 'ais', 'avdata']), tree) === '', 'setup page: ADS-B AND SONDES on → no note');
ok(onlyModeOf(new Set(['nfm', 'am', 'usb', 'lsb', 'cwu', 'cwl', 'drm', 'dab', 'ism', 'ais', 'avdata', 'sondes']), tree) === '', 'setup page: a demodulator on → no note');
ok(/" only: listeners land in " \+ w \+ " and cannot leave it\.<\/b> There is no EXIT " \+ w/.test(setup), 'setup page: the note names the mode left alone');

// ── the words ──
ok(/ONLY MODE \\u2014/.test(read('src/components/AboutOverlay.tsx')), 'About says it');

console.log(`${fails ? 'FAIL' : 'ok'}  the only mode (clients): ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
