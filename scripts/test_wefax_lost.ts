// test_wefax_lost.ts — lost WEFAX lines are DRAWN as the line above (src/utils/wefaxCrisp fillLostLines, 2026-10-05).
import { WEFAX_FILL_MAX, fillLostLines } from '../src/utils/wefaxCrisp.ts';
let pass = 0, fail = 0;
const ok = (c: boolean, m: string) => { if (c) pass++; else { fail++; console.log('  FAIL ' + m); } };
/** Rows as numbers (each row's "content"); undefined = never received. Returns what gets DRAWN. */
const run = (got: (number | undefined)[], y0 = 0, y1 = got.length - 1, drawn = got.map((v) => v ?? 0)) => {
  fillLostLines((j) => got[j] !== undefined, (y) => { drawn[y] = drawn[y - 1]; }, y0, y1);
  return drawn;
};
ok(run([1, 2, 3]).join() === '1,2,3', 'nothing lost → nothing changed');
ok(run([1, undefined, 3]).join() === '1,1,3', 'one lost line is the line above');
ok(run([1, 2, undefined, undefined, 5]).join() === '1,2,2,2,5', 'a short run repeats the last received line');
ok(run([undefined, undefined, 3]).join() === '0,0,3', 'nothing above (joined mid-chart) → left blank');
const long: (number | undefined)[] = [7, ...Array(WEFAX_FILL_MAX + 1).fill(undefined), 9];
ok(run(long).slice(1, -1).every((v) => v === 0), `a run longer than ${WEFAX_FILL_MAX} is an outage → left blank`);
const edge: (number | undefined)[] = [7, ...Array(WEFAX_FILL_MAX).fill(undefined), 9];
ok(run(edge).slice(1, -1).every((v) => v === 7), `a run of exactly ${WEFAX_FILL_MAX} is filled`);
// a late line arriving inside a filled run re-seeds the rows under it
const g: (number | undefined)[] = [1, undefined, undefined, 4];
const d = run(g); ok(d.join() === '1,1,1,4', 'run filled from 1');
g[1] = 2; d[1] = 2; run(g, 2, 3, d); ok(d.join() === '1,2,2,4', 'late line 2 → the row under it now repeats 2');
// the received rows (what the aligner and histogram read) are never touched
const got: (number | undefined)[] = [1, undefined, 3]; run(got);
ok(got[1] === undefined, 'the received set keeps its hole (findChartAlign never sees a filled row)');
console.log(`wefaxLost: ${pass} passed, ${fail} failed`);
if (fail) process.exit(1);
