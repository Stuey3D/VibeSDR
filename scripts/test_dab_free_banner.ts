/**
 * test_dab_free_banner.ts — V12 beta83 (Stuart, 2026-11-18, five own stations on the Pi 2: "users don't know they can now
 * actually choose any station within that multiplex"). In DAB with own stations on, the SHARED TUNER banner says what is
 * shared — "ASK TO TUNE / EXIT DAB" — and alternates with "ANY STATION IN THE LIST IS FREE TO CHOOSE"; the station list
 * says "Every user can choose their own station on this multiplex". Pure rule + wiring, both clients.
 * Run: node --no-warnings scripts/test_dab_free_banner.ts
 */
import { readFileSync } from 'node:fs';
import { sharedTunerText, dabFreePhase, dabFreeText, DAB_FREE_FLIP_MS } from '../src/services/dialChat.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const src = (p: string) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');

ok(sharedTunerText(false, 5, 5) === 'SHARED TUNER · ASK TO TUNE · 5/5 👤', `not DAB: unchanged (${sharedTunerText(false, 5, 5)})`);
ok(sharedTunerText(false, 5, 5, false, true) === 'SHARED TUNER · ASK TO TUNE / EXIT DAB · 5/5 👤', `DAB own: ${sharedTunerText(false, 5, 5, false, true)}`);
ok(sharedTunerText(true, 1, 5, false, true) === 'SHARED TUNER · FREE TO TUNE', 'alone in DAB: FREE TO TUNE, as ever');
ok(sharedTunerText(false, 3, 5, true, true) === 'ASK TO TUNE / EXIT DAB · 3/5 👤', 'tight: the prefix goes, never the words');
ok(dabFreeText() === 'ANY STATION IN THE LIST IS FREE TO CHOOSE', `the stations line: ${dabFreeText()}`);
ok(!dabFreePhase(0) && dabFreePhase(DAB_FREE_FLIP_MS) && !dabFreePhase(2 * DAB_FREE_FLIP_MS), `alternates every ${DAB_FREE_FLIP_MS / 1000} s`);

const bar = src('src/components/ControlsBar.tsx'), scr = src('src/screens/SDRScreen.tsx');
const fn = bar.slice(bar.indexOf('function sharedBannerText'), bar.indexOf('function sharedBannerLabel'));
ok(fn.indexOf('dabFreeText()') > fn.indexOf('st.said') && fn.indexOf('dabFreeText()') < fn.indexOf('askBannerText'),
   'app: a scan and a chat line still outrank it; it alternates with the ask-lock text too (a pick is not a dial move)');
ok(/sharedTunerText\(false, st\.listeners, st\.max, false, !!st\.dabOwn\)/.test(fn), 'app: the ask line names EXIT DAB in DAB');
ok(/dabOwnFree \? \{ dabOwn: true, dabFree: dabFreeOn \}/.test(scr) && /const dabOwnFree = !!dabState\?\.own\?\.on/.test(scr),
   'app: SDRScreen passes own.on and the phase');
ok(/pane === 'stations' && !!p\.d\?\.own\?\.on[\s\S]{0,200}Every user can choose their own station on this multiplex/.test(src('src/components/DabPanel.tsx')),
   'app: the note tops the station list while own stations are on');
const web = src('web/client/src/main.ts');
ok(/dabOwn && listenerCount > 1 && dabFreePhase\(Date\.now\(\)\)/.test(web) && /sharedTunerText\(false, listenerCount, listenerMax, false, dabOwn\)/.test(web),
   'web: the banner alternates the same way and names EXIT DAB');
ok(/id="dabOwnNote" hidden>Every user can choose their own station on this multiplex/.test(src('web/client/index.html'))
   && /dabOn && dabPane === 'stations' && !!dabState\?\.own\?\.on/.test(web), 'web: the note tops the station list while own stations are on');
ok(/const listening = d\.own\?\.on \? /.test(src('src/components/DabPanel.tsx')) && /const pn = d\.own\?\.on \? /.test(web),
   'the "▶ n" count shows only with own stations on (off, the station is everyone\'s and it read as multi-station) — app and web');
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
