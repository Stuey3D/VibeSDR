// The L/R deviation meters' display rules (src/services/lrMeter.ts) — one copy for the app and the web.
import { LR_EMPTY, LR_LIMIT_KHZ, lrDisplay, lrParts } from '../src/services/lrMeter.ts';
let pass = 0, fail = 0;
const eq = (what: string, got: unknown, want: unknown) => { const ok = JSON.stringify(got) === JSON.stringify(want); ok ? pass++ : fail++; if (!ok) console.error(`FAIL ${what}\n   got  ${JSON.stringify(got)}\n   want ${JSON.stringify(want)}`); };
eq('older server / no stereo: nothing drawn', lrParts(undefined, undefined), null);
eq('a negative (−1 = none) is nothing, never a zero', lrParts(-1, -1), null);
eq('54 / 52 kHz: both green', lrParts(54, 52), { l: { pct: 54, text: '54 kHz', tone: 'ok' }, r: { pct: 52, text: '52 kHz', tone: 'ok' } });
eq('Heart: 70 kHz per channel under a "nominal" 74 kHz bar is GREEN (the screenshot)', lrParts(70, 70)!.l.tone, 'ok');
eq('at the limit (75) green; just past amber, as the deviation bar', [lrParts(LR_LIMIT_KHZ, 78)!.l.tone, lrParts(LR_LIMIT_KHZ, 78)!.r.tone], ['ok', 'warn']);
eq('past the hedge band (82): red', lrParts(84, 40)!.l.tone, 'bad');
eq('the bar never runs off the end', lrParts(140, 0)!.l.pct, 100);
eq('weak station: a gap after a reading keeps the rows, empty (no flashing)', lrDisplay(undefined, undefined, true), LR_EMPTY);
eq('never seen (older server / mono): not drawn', lrDisplay(undefined, undefined, false), null);
eq('a reading always shows', lrDisplay(60, 58, false)!.l.text, '60 kHz');
console.log(`lr meter: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
