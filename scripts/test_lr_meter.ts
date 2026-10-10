// The L/R deviation meters' display rules (src/services/lrMeter.ts) — one copy for the app and the web.
import { LR_FULL_KHZ, lrParts } from '../src/services/lrMeter.ts';
let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => { const ok = JSON.stringify(got) === JSON.stringify(want); ok ? pass++ : fail++; if (!ok) console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`); };
eq('older server / no stereo: nothing drawn', lrParts(undefined, undefined), null);
eq('a negative (−1 = none) is nothing, never a zero', lrParts(-1, -1), null);
eq('54 / 52 kHz: both green', lrParts(54, 52), { l: { pct: 54, text: '54 kHz', tone: 'ok' }, r: { pct: 52, text: '52 kHz', tone: 'ok' } });
eq('at full modulation (67.5) still green; just past is amber', [lrParts(LR_FULL_KHZ, 70)!.l.tone, lrParts(LR_FULL_KHZ, 70)!.r.tone], ['ok', 'warn']);
eq('one channel alone over 75: red', lrParts(80, 40)!.l.tone, 'bad');
eq('the bar never runs off the end', lrParts(140, 0)!.l.pct, 100);
console.log(`lr meter: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
