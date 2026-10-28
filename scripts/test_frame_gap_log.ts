/**
 * The dev frame-gap log (src/utils/frameGapLog.ts): off = silent; on, a late row is blamed on the JS thread when the
 * draw timer fired late, on the network when it fired on time with nothing to draw, and lists the marks before it.
 * Run: node --no-warnings scripts/test_frame_gap_log.ts
 */
import { _resetGapLog, _setGapLogIo, _setGcStats, gapArrival, gapDraw, jsMark, jsTimed } from '../src/utils/frameGapLog.ts';

let fails = 0, passes = 0;
const ok = (what: string, cond: boolean, detail = '') => { if (cond) passes++; else { fails++; console.log(`  FAIL ${what}${detail ? ` — ${detail}` : ''}`); } };

let now = 1000;
const lines: string[] = [];
_setGapLogIo(() => now, (l) => lines.push(l));

// OFF: nothing happens
_resetGapLog();
for (let i = 0; i < 10; i++) { now += 50; gapArrival(); gapDraw(50, 0); }
now += 300; gapArrival(); ok('off: a late row says nothing', gapDraw(50, 250) === '' && lines.length === 0);

// ON
(globalThis as { __vibeWfGapLog?: boolean }).__vibeWfGapLog = true;
_resetGapLog();
for (let i = 0; i < 20; i++) { now += 50; gapArrival(); ok('steady rows are on time', gapDraw(50, 2) === ''); }
// the JS thread blocks: a 120 ms job, the timer fires 120 ms late
now += 30; jsTimed('fake hwinfo poll', () => { now += 120; });
now += 20; gapArrival();
ok('a blocked JS thread is blamed on JS', gapDraw(50, 120) === 'js', lines.at(-1));
ok('…and names what ran before it', /fake hwinfo poll 120ms/.test(lines.at(-1) ?? ''), lines.at(-1));
// the network holds a row: timer on time, nothing there, row arrives 200 ms late
for (let i = 0; i < 5; i++) { now += 50; gapArrival(); gapDraw(50, 1); }
now += 220; gapArrival(); jsMark('chat');
ok('a late arrival is blamed on the network', gapDraw(50, 3) === 'net', lines.at(-1));
ok('two late rows logged', lines.length === 2, lines.join('\n'));
// a GC between two rows is named on the late row
{
  let gcN = 10, gcMs = 100;
  _setGcStats(() => ({ n: gcN, ms: gcMs }));
  for (let i = 0; i < 3; i++) { now += 50; gapArrival(); gapDraw(50, 0); }
  gcN += 1; gcMs += 95; now += 145; gapArrival();
  gapDraw(50, 95);
  ok('a GC between rows is named', /GC ×1 \(95 ms\)/.test(lines.at(-1) ?? ''), lines.at(-1));
  _setGcStats(() => null);
}
// marks older than the look-back are not listed
now += 1000; jsMark('old');
for (let i = 0; i < 3; i++) { now += 50; gapArrival(); gapDraw(50, 0); }
now += 600; gapArrival();
gapDraw(50, 500);
ok('marks past the look-back are not blamed', !/old/.test(lines.at(-1) ?? ''), lines.at(-1));

console.log(lines.map((l) => `    ${l}`).join('\n'));
console.log(`  frame-gap log: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
