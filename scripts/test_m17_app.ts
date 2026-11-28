/**
 * test_m17_app.ts — the app's own half of M17 as a mode (m17.ts holds the shared rule; test_m17.ts tests it): the passband
 * an M17 pick starts with, the mode box label, the bookmark list, the tuned-station window, and that the screen offers the
 * key only through m17Offered, with no width sliders and a ghost report dropped.
 *   Run: npx tsx scripts/test_m17_app.ts
 * ★ SILENT: no audio, no network.
 */
import { readFileSync } from 'node:fs';
import { modeDefaultPassband, MODE_BANDWIDTHS, MODE_MAX_BW_HZ } from '../src/services/sdrProtocol.ts';
import { composeModeLabel, modeDisplay } from '../src/constants/modeBox.ts';
import { tunedPassband } from '../src/utils/tunedStation.ts';
import { BOOKMARK_MODES } from '../src/utils/bookmarkEdit.ts';
import { ASK_SHARED_SETTINGS } from '../src/services/dialChat.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { (c ? pass++ : fail++); console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const eq = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);

console.log('── the passband, labels, bookmarks ──');
ok(eq(modeDefaultPassband('m17', 433_425_000), [-4500, 4500]) && eq(MODE_BANDWIDTHS.m17, [-4500, 4500]), 'M17 starts on its 9 kHz channel, either side of the dial');
ok(MODE_MAX_BW_HZ.m17 === 9_000, 'its width is fixed (the server keeps the receiver\'s own 12.5 kHz)');
ok(modeDisplay('m17') === 'M17', 'the mode box reads M17');
ok(composeModeLabel('m17', 'whisper') === 'M17: WHISPER', 'with a decoder riding it: M17: … (as short as USB)');
ok(BOOKMARK_MODES.includes('m17'), 'a bookmark can carry M17 (the server says no where it is not offered)');
const tp = tunedPassband('m17');
ok(tp.lo < 0 && tp.hi > 0, 'the tuned-station window: an NFM channel either side of the dial');
ok(!ASK_SHARED_SETTINGS.has('m17_status'), 'asking for the report is not gated (it changes nothing the room hears)');

console.log('── the screen offers it only through m17Offered ──');
const scr = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
const vsc = readFileSync(new URL('../src/services/VibeServerClient.ts', import.meta.url), 'utf8');
ok(/setM17Cap\(m17Offered\(o\)\)/.test(scr) && /setM17Cap\(false\)/.test(scr), 'the per-radio answer (vibeserver.json) decides the key, reset per radio');
ok(/withM17\(withFreeDv\(/.test(scr), 'the key is added to the VibeServer list only (after NFM)');
ok(/getStatus\(\)\.mode === 'm17'\) setM17State/.test(scr), 'a report after leaving the mode is a ghost');
ok((scr.match(/\(status\.mode === 'm17' && m17Cap\) (\|\| \(status\.mode === 'dvsig' && dvsigCap\) )?\? undefined : (askWrap\()?onFilterBoth/g) || []).length === 2, 'no width sliders in M17 (both places they are drawn)');
ok(/m17Status\?\.\(\)/.test(scr) && /case 'm17': \{ const f = parseM17\(msg\)/.test(vsc), 'entering the mode and a reconnect ask for the report; the client parses it with parseM17');
// ★ the tour's mode card is an overview now (Stuart 2026-11-16): it names no key's place, so it cannot misdirect — the About note says where M17 is
ok(!/M17/.test((() => { const t = scr; const i = t.indexOf("{ id: 'mode', title:"); return i < 0 ? '' : t.slice(i, t.indexOf("{ id: '", i + 5)); })()) && /M17 DIGITAL VOICE[^']*a new mode beside NFM/.test(readFileSync(new URL('../src/components/AboutOverlay.tsx', import.meta.url), 'utf8')),
  'the tour\'s mode card stays an overview (no M17 place to go stale); About says M17 sits beside NFM');

console.log(`\n${fail ? 'FAIL' : 'ok  '} m17 app: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
