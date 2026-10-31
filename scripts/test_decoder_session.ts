/**
 * ★★★ AUTO first, manual for the session only (src/services/decoderSession.ts; Stuart, 2026-10-31: "decoder default to
 * auto when activated, unless a user has set manual settings in the same session"). The helper's rule, and the app's and
 * the web's AUTO decoders using it — none of them writes its mode to storage any more.
 */
import { readFileSync } from 'node:fs';
import { sessionChoice, setSessionChoice, resetDecoderSession } from '../src/services/decoderSession';

let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) { pass++; console.log('   ok  ', m); } else { fail++; console.log('   FAIL', m); } };

console.log('the rule');
resetDecoderSession();
ok(sessionChoice('paging.mode', 'auto') === 'auto', 'first open in a session: AUTO');
setSessionChoice('paging.mode', 'pocsag1200', 'auto');
ok(sessionChoice('paging.mode', 'auto') === 'pocsag1200', 'a manual choice is kept when the decoder is opened again in the same session');
setSessionChoice('paging.mode', 'auto', 'auto');
ok(sessionChoice('paging.mode', 'auto') === 'auto', 'choosing AUTO again forgets the manual choice');
const AUTO = { shift: 170, baud: 45.45, auto: true };
setSessionChoice('rtty', { shift: 450, baud: 50, auto: false }, AUTO);
ok(sessionChoice('rtty', AUTO).shift === 450 && sessionChoice('rtty', AUTO).auto === false, 'RTTY: hand-picked settings kept for the session');
ok(sessionChoice('textmode.mfsk', 'auto') === 'auto', 'one decoder\'s choice does not touch another\'s');
resetDecoderSession();
ok(sessionChoice('rtty', AUTO).auto === true && sessionChoice('paging.mode', 'auto') === 'auto', 'a new session (app launch / page load): AUTO again');

console.log('nothing stored');
const app = readFileSync(new URL('../src/screens/SDRScreen.tsx', import.meta.url), 'utf8');
const web = readFileSync(new URL('../web/client/src/main.ts', import.meta.url), 'utf8');
for (const k of ['lsv_rtty', 'lsv_time_station', 'lsv_textmode_', 'lsv_paging_mode'])
  ok(!new RegExp(`AsyncStorage\\.(set|get)Item\\('${k}`).test(app), `app: ${k} is not read or written`);
ok(!/localStorage\.(set|get)Item\('vs_textmode_/.test(web), 'web: the text-mode choice is not read or written');
for (const k of ["'rtty'", "'time.station'", "'paging.mode'", "'textmode.' + fam"])
  ok(app.includes(`sessionChoice(${k}`) || app.includes(`sessionChoice<RttySettings>(${k}`), `app: ${k} comes from the session`);
ok(web.includes("sessionChoice('textmode.' + fam"), 'web: the text-mode choice comes from the session');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
