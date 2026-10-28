/**
 * WEB WATERFALL ZOOM FAN (web/client/src/wfZoomFan.ts) — Stuart, 2026-10-27 23:51: zooming the web waterfall left the
 * history SLICED into bands of different zoom. The hard rule: a zoom fans the history in and out; a tune does not move it.
 *
 * Proves, on the pure maps:
 *   • a carrier stored in an old row lands at its TRUE frequency's screen x after a zoom in, a zoom out, an anchored
 *     (off-centre) zoom, and a chain of pinch steps — the same x a new row at the new span puts it at (no slicing);
 *   • a zoom in then back out returns every row to the identity (lossless — the edges come back);
 *   • a zoom out leaves the newly exposed edges blank (visibleSpan) and never smears the edge bin;
 *   • rows written between zooms form separate runs (mapRuns), identity history is ONE run (the old single draw).
 * Run: node --no-warnings scripts/test_wf_zoom_fan.ts
 */
import { applyViewChange, spanChanged, mapRuns, visibleSpan } from '../web/client/src/wfZoomFan.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const near = (a: number, b: number, e = 1e-9) => Math.abs(a - b) <= e;

const CARRIER = 1_050_000;
/** Screen x (0..1) where a row with map (a, b), measured over (lo, span), shows the carrier. */
const screenX = (a: number, b: number, lo: number, span: number) => (((CARRIER - lo) / span) - b) / a;

// One row stored at view (lo0, span0); then a chain of view changes; it must show the carrier where the live view does.
function chain(views: [number, number][]) {
  const A = new Float64Array([1]), B = new Float64Array([0]);
  const [lo0, s0] = views[0];
  let [plo, ps] = views[0];
  for (const [lo, s] of views.slice(1)) { if (spanChanged(ps, s)) applyViewChange(A, B, plo, ps, lo, s); plo = lo; ps = s; }
  const [loN, sN] = views[views.length - 1];
  return { got: screenX(A[0], B[0], lo0, s0), want: (CARRIER - loN) / sN, a: A[0], b: B[0] };
}
const v = (centre: number, span: number): [number, number] => [centre - span / 2, span];

{ const r = chain([v(1_040_000, 200_000), v(1_040_000, 80_000)]);
  ok(near(r.got, r.want), `zoom in 200→80 kHz about the dial: the old row's carrier at x ${r.got.toFixed(4)} = the live view's ${r.want.toFixed(4)}`); }
{ const r = chain([v(1_040_000, 80_000), v(1_040_000, 200_000)]);
  ok(near(r.got, r.want), `zoom out 80→200 kHz: x ${r.got.toFixed(4)} = ${r.want.toFixed(4)}`); }
{ // anchored at the carrier (a wheel zoom about the pointer): the carrier must stay where it was on screen
  const before = v(1_040_000, 200_000), xs = (CARRIER - before[0]) / before[1];
  const span = 50_000, lo = CARRIER - xs * span;
  const r = chain([before, [lo, span]]);
  ok(near(r.got, xs) && near(r.got, r.want), `anchored zoom about the carrier keeps it pinned (x ${r.got.toFixed(4)})`); }
{ const steps = [200_000, 160_000, 130_000, 100_000, 80_000, 100_000, 130_000, 160_000, 200_000].map((s) => v(1_040_000, s));
  const r = chain(steps);
  ok(near(r.got, r.want, 1e-9) && near(r.a, 1, 1e-12) && near(r.b, 0, 1e-12), `a pinch in and back out returns the row to the identity (a ${r.a}, b ${r.b})`); }
{ // a tune alone does not move the history (the bend) — spanChanged is false, the map stays the identity
  const r = chain([v(1_000_000, 200_000), v(1_010_000, 200_000), v(1_040_000, 200_000)]);
  ok(r.a === 1 && r.b === 0, 'a tune alone leaves the history where it was drawn (the bend, #57)'); }
{ // zoom out 2×: the row covers the middle half; the edges are blank
  const r = chain([v(1_000_000, 100_000), v(1_000_000, 200_000)]);
  const vs = visibleSpan(r.a, r.b);
  ok(!!vs && near(vs[0], 0.25) && near(vs[1], 0.75) && near(vs[2], 0) && near(vs[3], 1), `zoom out 2×: the old row fills x 0.25–0.75, blank either side (${vs?.map((x) => x.toFixed(3)).join(' ')})`); }
{ // zoom in 2×: the row's middle half fills the screen
  const r = chain([v(1_000_000, 200_000), v(1_000_000, 100_000)]);
  const vs = visibleSpan(r.a, r.b);
  ok(!!vs && near(vs[0], 0) && near(vs[1], 1) && near(vs[2], 0.25) && near(vs[3], 0.75), `zoom in 2×: u 0.25–0.75 stretched over the screen`); }
ok(visibleSpan(1, 2) === null && visibleSpan(1, -3) === null, 'a row zoomed wholly off screen draws nothing (blank)');

{ // runs: 10 identity rows → one run; a zoom, 3 more rows → two runs, newest first
  const rows = 16, A = new Float64Array(rows).fill(1), B = new Float64Array(rows);
  const out = { buf: new Float64Array(4) };
  let head = 0;
  const write = () => { head = (head - 1 + rows) % rows; A[head] = 1; B[head] = 0; };
  for (let i = 0; i < 10; i++) write();
  let n = mapRuns(A, B, head, 10, out);
  ok(n === 1 && out.buf[2] === 1 && out.buf[3] === 0, 'unzoomed history is one identity run (the old single draw)');
  applyViewChange(A, B, 900_000, 200_000, 950_000, 100_000);
  for (let i = 0; i < 3; i++) write();
  n = mapRuns(A, B, head, 13, out);
  ok(n === 2 && out.buf[0] === 0 && out.buf[1] === 3 && out.buf[4] === 3 && out.buf[5] === 13 && near(out.buf[6], 0.5) && near(out.buf[7], 0.25),
     `after a zoom: newest 3 rows identity, the 10 before fanned (${n} runs: ${[...out.buf.slice(0, n * 4)].join(',')})`);
}

console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
