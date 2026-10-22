// test_admin_bg.ts — the admin page's background-decoding line (web/client/src/adminBg.ts, V12 beta 4).
import { bgLines, bgVfoLine, bgDuration } from '../web/client/src/adminBg.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };

ok(bgLines(null) === null && bgLines({ state: 'off', profiles: 0 }) === null, 'not set up → nothing shown');
ok(bgLines({ state: 'off', profiles: 1 })!.head === 'Background decoding \u00b7 off', 'switched off');
ok(bgLines({ state: 'idle', profiles: 1, profile: 0 })!.head.includes('no profile covers this time of day'), 'idle');
ok(bgLines({ state: 'waiting', profiles: 1, profile: 1, startsIn: 200 })!.head.includes('profile 1 starts in 3 min 20 s unless somebody arrives'), 'waiting countdown');
const b = bgLines({ state: 'busy', profiles: 1, profile: 1, retryIn: 45 })!;
ok(b.head.includes('another program has the radio') && b.head.includes('45 s') && b.warn, 'busy (shared by time), warned');
const run = {
  state: 'running' as const, profiles: 1, profile: 1,
  runner: { running: true, cpuPct: 3.14, channelsCpuPct: 0.8, vfos: [
    { hz: 490000, dec: 'navtex', label: 'NAVTEX', state: 'running' as const, lock: 'locked', last: 1792707180, cpuPct: 1.4 },
    { hz: 518000, dec: 'navtex', label: 'NAVTEX', state: 'paused' as const, why: 'a listener needed a decoder slot', cpuPct: 0 },
    { hz: 4610000, dec: 'rtty', label: 'RTTY', state: 'out' as const },
  ] } };
const l = bgLines(run)!;
ok(l.head === 'Background decoding \u00b7 profile 1 \u00b7 1 of 3 VFOs running \u00b7 3.1 % of one core', 'running head: ' + l.head);
ok(/^490\.0 NAVTEX · locked · last message \d\d:\d\d · 1\.4 %$/.test(l.rows[0]), 'running VFO: ' + l.rows[0]);
ok(l.rows[1] === '518.0 NAVTEX \u00b7 paused (a listener needed a decoder slot)', 'paused VFO: ' + l.rows[1]);
ok(l.rows[2] === "4610.0 RTTY \u00b7 out of range (outside the listener's window)", 'out of range VFO');
ok(l.rows[3] === 'channels (one FFT for every VFO) \u00b7 0.8 %', 'the shared FFT, said as such');
const lis = bgLines({ ...run, state: 'listener' })!;
ok(lis.head.includes('carrying on with a listener on \u00b7 1 of 3 VFOs running'), 'carrying on with a listener');
ok(bgLines({ state: 'paused', profiles: 1, profile: 1 })!.head.includes('paused \u2014 a listener is on'), 'paused for a listener');
ok(bgLines({ ...run, runner: { ...run.runner, cpuPct: null } })!.head.endsWith('measuring\u2026'), 'no CPU yet → measuring…');
ok(bgVfoLine({ hz: 490000, dec: 'navtex', label: 'NAVTEX', state: 'running', last: 0 }).includes('nothing yet'), 'nothing decoded yet');
ok(bgVfoLine({ hz: 1840000, dec: 'ft8', label: 'FT8', state: 'running', spots: 142, countries: 23, cpuPct: 0.9 })
   === '1840.0 FT8 \u00b7 running \u00b7 142 spots, 23 countries so far \u00b7 0.9 %', 'FT8 live count');
ok(bgVfoLine({ hz: 1840000, dec: 'ft8', label: 'FT8', state: 'running', spots: 1, countries: 1 }).includes('1 spot, 1 country so far'), 'FT8 singulars');
ok(bgDuration(45) === '45 s' && bgDuration(120) === '2 min', 'durations');

console.log(`admin bg: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
