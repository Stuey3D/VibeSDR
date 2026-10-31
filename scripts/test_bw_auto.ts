/**
 * test_bw_auto.ts — src/services/bwAuto.ts, the rules the app's ModeSelector and the web client's bandwidth row both read
 * for the V12 AUTO key (docs/v12/AM-SYNC.md §11), and the wiring that keeps the row unchanged where AUTO is not offered.
 *   Run: node --no-warnings --max-old-space-size=512 scripts/test_bw_auto.ts
 * ★ SILENT: no audio, no network.
 */
import { readFileSync } from 'node:fs';
import { autoBwOffered, parseAutoBwModes, parseBwAuto, shownEdges } from '../src/services/bwAuto.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { (c ? pass++ : fail++); console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };

console.log('── offered only where the server lists the mode ──');
ok(parseAutoBwModes(undefined).length === 0 && parseAutoBwModes('usb').length === 0, 'no list / not a list (an older server) → none');
ok(JSON.stringify(parseAutoBwModes(['USB', 'cwu', 7, '<x>', 'wfm'])) === JSON.stringify(['usb', 'cwu', 'wfm']), 'cleaned and lower-cased');
ok(!autoBwOffered([], 'usb') && !autoBwOffered(null, 'usb'), 'an older server: no AUTO key — the row is SYNC alone');
ok(autoBwOffered(['usb', 'lsb'], 'USB') && !autoBwOffered(['usb', 'lsb'], 'nfm'), 'the current mode, and only it');

console.log('── the bwauto message ──');
ok(parseBwAuto({ type: 'amsync', on: true }) === null && parseBwAuto({ type: 'bwauto' }) === null, 'not one → null');
const cut = parseBwAuto({ type: 'bwauto', on: true, cut: true, lo: 125, hi: 2675 });
ok(!!cut && cut.cut && cut.lo === 125 && cut.hi === 2675, 'a cut, with its edges');
const nul = parseBwAuto({ type: 'bwauto', on: true, cut: true, lo: null, hi: 4500 });
ok(!!nul && nul.lo === null && nul.hi === 4500, 'null = that edge is the listener\'s own (ECSS-U cuts only its top)');
const wide = parseBwAuto({ type: 'bwauto', on: true, cut: false, lo: 1, hi: 2 });
ok(!!wide && wide.lo === null && wide.hi === null, 'not cutting → no edges, whatever was sent');
ok(parseBwAuto({ type: 'bwauto', on: true, cut: true, lo: 1e9, hi: 'x' })?.lo === null, 'nonsense edges dropped');

console.log('── what the sliders show ──');
const own = { low: 50, high: 2700 };
ok(JSON.stringify(shownEdges(own, true, cut)) === JSON.stringify({ low: 125, high: 2675 }), 'AUTO on and cutting → AUTO\'s edges');
ok(JSON.stringify(shownEdges(own, false, cut)) === JSON.stringify(own), 'AUTO off → the listener\'s own');
ok(JSON.stringify(shownEdges(own, true, wide)) === JSON.stringify(own), 'AUTO on, not cutting → the listener\'s own');
ok(JSON.stringify(shownEdges(own, true, nul)) === JSON.stringify({ low: 50, high: 4500 }), 'a null edge keeps the listener\'s');
ok(JSON.stringify(shownEdges(own, true, { on: true, cut: true, lo: 3000, hi: 2000 })) === JSON.stringify(own), 'an impossible pair is ignored');

console.log('── wiring ──');
const ms = readFileSync('src/components/ModeSelector.tsx', 'utf8');
ok(/\{autoBw \? \(/.test(ms) && /\) : \(\s*\n\s*<NavRow><NavItem onPress=\{\(\) => setBwSync/.test(ms), 'app: without autoBw the middle column is the old SYNC key alone');
ok((ms.match(/dragBw\(/g) || []).length >= 4 && /autoBw\.onToggle\(false\)/.test(ms), 'app: every slider drag goes through the manual override');
ok(/label="AUTO"/.test(ms) && />AUTO<\/Text>/.test(ms), 'app: AUTO in both faceplate styles (PopupKey and the default key)');
const scr = readFileSync('src/screens/SDRScreen.tsx', 'utf8');
ok(/autoBw=\{autoBwOffered\(icom \? \[\] : autoBwModes, status\.mode\)/.test(scr), 'app: offered from the server\'s list, never on an Icom');
const html = readFileSync('web/client/index.html', 'utf8'), main = readFileSync('web/client/src/main.ts', 'utf8');
ok(/<button class="btn" id="bwAuto" hidden/.test(html) && !/id="amAutoBwRow"/.test(html), 'web: the AUTO key starts hidden; the AM AUTO BW row is gone (moved)');
ok(/setHidden\(\$\('bwAuto'\), !autoBwOffered\(autoBwModesSrv, spec\.mode\)\)/.test(main), 'web: shown from the server\'s list');
ok((main.match(/autoOffForDrag\(\);/g) || []).length === 2, 'web: both sliders are a manual override');
const shim = readFileSync('android/app/src/main/cpp/local_sdr_shim.cpp', 'utf8');
ok(/"autoBwModes\\":/.test(shim) && /\{\\"type\\":\\"bwauto\\"/.test(shim), 'server: lists the modes and reports what AUTO did');
ok(!/"nfm"/.test(shim.slice(shim.indexOf('const char* fam[]'), shim.indexOf('const char* fam[]') + 200)), 'server: NFM is not listed (no automatic width helped it)');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
