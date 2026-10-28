/**
 * WEB WATERFALL GLIDE (web/client/src/wfGlide.ts) — Stuart 2026-10-28: a fast tune drew a station's trail as dashes.
 * Proves on the pure row: a slide moves the row (vacated edge = floor 0); a carrier is painted across every slide
 * between the previous row's and this row's, so consecutive rows touch; the noise floor is NOT smeared; a move wider
 * than SMEAR_MAX is left as a jump.
 * Run: node --no-warnings scripts/test_wf_glide.ts
 */
import { glideRow, SMEAR_MAX } from '../web/client/src/wfGlide.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const N = 1024;
const row = () => { const r = new Uint8Array(N); for (let i = 0; i < N; i++) r[i] = 40 + (i * 7919 % 13); r[500] = 250; return r; };
const lit = (d: Uint8Array) => [...d].map((v, i) => [v, i]).filter(([v]) => v >= 250).map(([, i]) => i);

{ const s = row(), d = new Uint8Array(N); glideRow(s, d, N, 0, 0);
  ok(d.every((v, i) => v === s[i]), 'no shift, no smear: the row exactly as it came'); }
{ const s = row(), d = new Uint8Array(N); glideRow(s, d, N, 10, 10);
  ok(lit(d).join() === '510' && d[0] === 0 && d[9] === 0 && d[10] === s[0], 'slide 10: the carrier at 510, the vacated edge the floor'); }
{ const s = row(), d = new Uint8Array(N); glideRow(s, d, N, -12, 0);
  const l = lit(d);
  ok(l.length === 13 && l[0] === 488 && l[l.length - 1] === 500, `smear back to the previous row's place: the carrier spans 488–500 (${l[0]}–${l[l.length - 1]})`);
  let same = 0; for (let i = 0; i < N; i++) { const j = i + 12; if (j < N && i !== 500 && (i < 488 || i > 500) && d[i] === s[j]) same++; }
  ok(same > N - 40, `the noise floor is slid, never smeared (${same} bins untouched by the smear)`); }
{ const s = row(), d = new Uint8Array(N); glideRow(s, d, N, SMEAR_MAX + 5, 0);
  ok(lit(d).length === 1, 'a move wider than SMEAR_MAX is a jump: no smear'); }

console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
