/**
 * THE APP'S WATERFALL HISTORY (src/utils/wfRowPlace.ts, WaterfallView's ring) — GitHub #57 and Stuart's hard rule:
 * "Zooming and tuning also never lost the history and it was seemless to the history too so signals always glided in
 * from the sides and settled and when zooming in and out you had a cool fanning in and out of the signals."
 *
 * The Skia shader cannot run in node, so this drives the ring the way pushRow does — a fixed RING_W × RING_ROWS byte ring,
 * one write per row, placed against the view it lands in — with rows built as the SERVER builds them (a steady carrier at
 * one RF frequency, bins measured from the row's own centre, the row arriving a frame or two after the view moved):
 *   • a LOCKED VFO tuned in steps: the carrier's column moves smoothly row to row (no zig-zag) and ends where the view
 *     says; once the dial stops the trail is straight (every later row in one column);
 *   • a ZOOM in and out: a row at another span is NOT slid (it would draw the right signal at the wrong width), and the
 *     trail fans — out as the span narrows, back as it widens — without a reversal inside either half;
 *   • NOTHING already written is ever rewritten: the history before the tune is byte-for-byte what it was;
 *   • a FAST SPIN (the view runs ahead of the rows by several steps): rows still land in place, the vacated edge is the
 *     floor (0, a "slight gap"), never a smear of the edge bin, and nothing older is touched;
 *   • the guards: unknown centres, no overlap → drawn as they came.
 *
 * Run: node --no-warnings scripts/test_wf_row_place.ts
 */
import { rowViewOffset, writeRow } from '../src/utils/wfRowPlace.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++; console.log(`  FAIL ${what}${detail ? `\n       ${detail}` : ''}`);
}

const N = 1024, ROWS = 256, CARRIER = 1_050_000;
const ring = new Uint8Array(N * ROWS);
let frame = 0;
/** A server row: noise floor 40, the carrier 230 at its true bin for (centre, span). */
function serverRow(centre: number, span: number): Uint8Array {
  const r = new Uint8Array(N).fill(40);
  const bin = Math.round((CARRIER - centre) / (span / N)) + N / 2;
  if (bin >= 0 && bin < N) r[bin] = 230;
  return r;
}
/** pushRow: one row into the ring, placed against the view. */
function push(row: Uint8Array, rowCentre: number, rowSpan: number, view: { c: number; bw: number }) {
  const slot = (frame % ROWS) * N;
  writeRow(ring, slot, row, N, rowViewOffset(rowCentre, rowSpan / N, view.c, view.bw, N));
  frame++;
}
const colOf = (f: number) => { const s = (f % ROWS) * N; let b = -1, x = -1; for (let i = 0; i < N; i++) if (ring[s + i] > b) { b = ring[s + i]; x = i; } return b >= 200 ? x : -1; };
function zigzags(cols: number[], px: number) {
  let z = 0;
  for (let i = 1; i < cols.length - 1; i++) {
    if (cols[i] < 0 || cols[i - 1] < 0 || cols[i + 1] < 0) continue;
    const a = cols[i] - cols[i - 1], b = cols[i] - cols[i + 1];
    if (Math.sign(a) === Math.sign(b) && Math.abs(a) > px && Math.abs(b) > px) z++;
  }
  return z;
}

// ── a straight start: 30 rows at 1.000 MHz, 200 kHz span ──
let view = { c: 1_000_000, bw: 200_000 };
for (let i = 0; i < 30; i++) push(serverRow(view.c, view.bw), view.c, view.bw, view);
const startCol = colOf(frame - 1);
ok('the start: the carrier where the view puts it', startCol === N / 2 + Math.round(50_000 / (200_000 / N)), `col ${startCol}`);
const before = ring.slice(0, 30 * N);

// ── a LOCKED tune: 40 steps of 1 kHz, the view moves at once, each row arrives built for the view TWO steps back ──
const tuneFrom = frame;
const centres: number[] = [];
for (let i = 1; i <= 40; i++) {
  view = { c: 1_000_000 + i * 1000, bw: 200_000 };
  centres.push(view.c);
  const rc = centres[Math.max(0, centres.length - 3)];
  push(serverRow(rc, 200_000), rc, 200_000, view);
  push(serverRow(rc, 200_000), rc, 200_000, view);            // two rows per step, as a 20 fps feed under 10 steps/s
}
for (let i = 0; i < 20; i++) push(serverRow(view.c, view.bw), view.c, view.bw, view);   // the dial stops
const tuneCols: number[] = []; for (let f = tuneFrom; f < frame; f++) tuneCols.push(colOf(f));
ok('a locked tune: the trail never zig-zags', zigzags(tuneCols, 2) === 0, tuneCols.join(','));
ok('a locked tune: every row moved the carrier the same way or not at all (one bending line)',
   tuneCols.every((c, i) => i === 0 || c <= tuneCols[i - 1]), tuneCols.join(','));
const endCol = N / 2 + Math.round((CARRIER - view.c) / (200_000 / N));
ok('…and it settles straight where the view puts it', tuneCols.slice(-20).every((c) => c === endCol), `${tuneCols.slice(-20)} vs ${endCol}`);
ok('the history from before the tune is untouched', before.every((v, i) => v === ring[i]));

// ── a ZOOM in and back out round the dial — the fan ──
const zoomFrom = frame;
for (const bw of [160_000, 130_000, 100_000, 80_000, 100_000, 130_000, 160_000, 200_000]) {
  const was = view;
  view = { c: view.c, bw };
  push(serverRow(was.c, was.bw), was.c, was.bw, view);        // a frame from the OLD span still in flight: not slid
  for (let i = 0; i < 3; i++) push(serverRow(view.c, bw), view.c, bw, view);
}
const zc: number[] = []; for (let f = zoomFrom; f < frame; f++) zc.push(colOf(f));
const inHalf = zc.slice(0, 16), outHalf = zc.slice(16);
ok('a zoom in: the carrier fans OUT from the centre, never back', inHalf.every((c, i) => i === 0 || c >= inHalf[i - 1]), inHalf.join(','));
ok('a zoom out: it fans back IN, never out', outHalf.every((c, i) => i === 0 || c <= outHalf[i - 1]), outHalf.join(','));
ok('a row from another span is drawn as it came (not slid to the wrong width)',
   rowViewOffset(1_040_000, 200_000 / N, 1_040_000, 100_000, N) === 0);
ok('the history from before the tune is still untouched after the zoom', before.every((v, i) => v === ring[i]));

// ── a FAST SPIN: the view 6 steps of 4 kHz ahead of every row ──
const spinFrom = frame;
const vs: number[] = [];
for (let i = 1; i <= 20; i++) {
  view = { c: view.c - 4000, bw: 200_000 };
  vs.push(view.c);
  const rc = vs[Math.max(0, vs.length - 7)];
  push(serverRow(rc, 200_000), rc, 200_000, view);
}
const sc: number[] = []; for (let f = spinFrom; f < frame; f++) sc.push(colOf(f));
ok('a fast spin: no zig-zag', zigzags(sc, 2) === 0, sc.join(','));
ok('a fast spin: every row places the carrier at its TRUE column under the view it landed in (±1 bin of rounding)',
   sc.every((c, i) => Math.abs(c - (N / 2 + (CARRIER - vs[i]) / (200_000 / N))) <= 1), sc.join(','));
{
  // the row that lagged most: its vacated edge (the side the view moved towards) is floor, not the edge bin repeated
  const f = frame - 1, s = (f % ROWS) * N;
  const lag = 6 * 4000 / (200_000 / N);   // bins the row was slid by
  const edge = ring.subarray(s, s + Math.floor(lag));
  ok(`the newly exposed edge is a slight gap of floor (${Math.round(lag)} bins), not a smear`, edge.every((v) => v === 0));
}
ok('…and the history from before is STILL untouched', before.every((v, i) => v === ring[i]));

// ── guards ──
ok('unknown centres are drawn as they came', rowViewOffset(0, 195, 1e6, 2e5, N) === 0 && rowViewOffset(1e6, 195, 0, 2e5, N) === 0);
ok('no overlap at all is drawn as it came', rowViewOffset(2_000_000, 200_000 / N, 1_000_000, 200_000, N) === 0);

console.log(`  waterfall row placement: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
