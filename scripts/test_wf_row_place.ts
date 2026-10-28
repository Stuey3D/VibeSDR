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
 *   • THE GLIDE (2026-10-28): a fast tune's trail is ONE CONNECTED line — each row's lit run touches the row before's —
 *     where without it the rows were dots tens of bins apart; steady rows are byte-for-byte unstreaked; the floor is
 *     never raised; a jump past GLIDE_MAX_FRAC and a zoom-drum step are left as they were; a pinch fans connected.
 *
 * Run: node --no-warnings scripts/test_wf_row_place.ts
 */
import { glideMap, glideRow, GLIDE_MAX_FRAC, newGlideState, rowViewOffset, writeRow } from '../src/utils/wfRowPlace.ts';

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

// ── THE GLIDE: a fast tune drawn as one continuous trail ──
{
  const G = new Uint8Array(N * ROWS);
  const st = newGlideState(N);
  let gf = 0;
  /** A noisy server row: floor 40 ± 6, an AM-ish station 5 bins wide peaking at 230. */
  let seed = 7;
  const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
  function noisyRow(centre: number, span: number): Uint8Array {
    const r = new Uint8Array(N);
    for (let i = 0; i < N; i++) r[i] = 34 + Math.floor(rnd() * 13);
    const bin = Math.round((CARRIER - centre) / (span / N)) + N / 2;
    for (let d = -2; d <= 2; d++) if (bin + d >= 0 && bin + d < N) r[bin + d] = d === 0 ? 230 : 200;
    return r;
  }
  function gpush(row: Uint8Array, rc: number, span: number, v: { c: number; bw: number }, glide = true): number {
    const slot = (gf % ROWS) * N;
    const off = rowViewOffset(rc, span / N, v.c, v.bw, N);
    writeRow(G, slot, row, N, off);
    const sp = glide ? glideRow(G, slot, N, rc, span / N, off, st) : 0;
    gf++;
    return sp;
  }
  /** The lit run (≥ 200) of a row: [first, last] or null. */
  const lit = (f: number): [number, number] | null => {
    const s = (f % ROWS) * N; let a = -1, b = -1;
    for (let i = 0; i < N; i++) if (G[s + i] >= 200) { if (a < 0) a = i; b = i; }
    return a < 0 ? null : [a, b];
  };
  const connected = (from: number, to: number) => {
    const gaps: string[] = [];
    for (let f = from + 1; f < to; f++) {
      const p = lit(f - 1), q = lit(f);
      if (!p || !q) { gaps.push(`row ${f}: none`); continue; }
      if (q[0] > p[1] + 1 || q[1] < p[0] - 1) gaps.push(`row ${f}: [${q}] vs [${p}]`);
    }
    return gaps;
  };
  let v = { c: 1_000_000, bw: 200_000 };
  for (let i = 0; i < 10; i++) gpush(noisyRow(v.c, v.bw), v.c, v.bw, v);
  const steadyBefore = G.slice(0, 10 * N);
  // steady rows: identical to a plain write
  {
    const plain = new Uint8Array(N); seed = 99; const r = noisyRow(v.c, v.bw); plain.set(r);
    const slot = (gf % ROWS) * N; seed = 99; gpush(noisyRow(v.c, v.bw), v.c, v.bw, v);
    ok('the glide: a steady row is byte-for-byte as written', plain.every((x, i) => x === G[slot + i]));
  }
  // a FAST tune: 30 kHz a second at 20 rows/s = 1.5 kHz a row = 7.7 bins a row; rows arrive built for the view 2 back
  const fastFrom = gf;
  const cs: number[] = [];
  for (let i = 1; i <= 15; i++) {
    v = { c: v.c + 7_500, bw: 200_000 };                 // 7.5 kHz per row: ~38 bins between rows, dots without the glide
    cs.push(v.c);
    const rc = cs[Math.max(0, cs.length - 3)];
    gpush(noisyRow(rc, 200_000), rc, 200_000, v);
  }
  for (let i = 0; i < 10; i++) gpush(noisyRow(v.c, v.bw), v.c, v.bw, v);
  const gaps = connected(fastFrom, gf);
  ok('the glide: a fast tune is ONE connected trail (every row\'s lit run touches the one before)', gaps.length === 0, gaps.join('; '));
  ok('the glide: the trail ends straight where the view puts it',
     (() => { const want = N / 2 + Math.round((CARRIER - v.c) / (200_000 / N)); for (let f = gf - 9; f < gf; f++) { const l = lit(f); if (!l || l[0] !== want - 2 || l[1] !== want + 2) return false; } return true; })());
  // control: the same tune WITHOUT the glide has gaps — proves the test can see the fault
  {
    const save = gf; let w = { c: 1_000_000, bw: 200_000 }; st.has = false;
    for (let i = 0; i < 3; i++) gpush(noisyRow(w.c, w.bw), w.c, w.bw, w, false);
    const ctlFrom = gf; const cc: number[] = [];
    for (let i = 1; i <= 15; i++) { w = { c: w.c + 7_500, bw: 200_000 }; cc.push(w.c); const rc = cc[Math.max(0, cc.length - 3)]; gpush(noisyRow(rc, 200_000), rc, 200_000, w, false); }
    ok('control: without the glide the same tune is broken into dots', connected(ctlFrom, gf).length >= 12, `${connected(ctlFrom, gf).length} gaps`);
    st.has = false; void save;
  }
  // the floor is never raised: in streaked rows, every column below the threshold is what the server sent
  {
    let w = { c: 1_000_000, bw: 200_000 };
    for (let i = 0; i < 3; i++) gpush(noisyRow(w.c, w.bw), w.c, w.bw, w);
    let raisedFloor = 0;
    for (let i = 1; i <= 20; i++) {
      w = { c: w.c - 5_000, bw: 200_000 };
      seed = 1000 + i; const ref = noisyRow(w.c, w.bw);
      const slot = (gf % ROWS) * N;
      seed = 1000 + i; gpush(noisyRow(w.c, w.bw), w.c, w.bw, w);
      for (let x = 0; x < N; x++) if (G[slot + x] !== ref[x] && G[slot + x] < 200) raisedFloor++;
    }
    ok('the glide: only signal streaks — no floor column is changed to a non-signal value', raisedFloor === 0, `${raisedFloor} columns`);
  }
  // a JUMP past GLIDE_MAX_FRAC is left as a gap, not a bar
  {
    let w = { c: 1_000_000, bw: 200_000 };
    for (let i = 0; i < 3; i++) gpush(noisyRow(w.c, w.bw), w.c, w.bw, w);
    w = { c: w.c + 200_000 * GLIDE_MAX_FRAC * 1.5, bw: 200_000 };
    const sp = gpush(noisyRow(w.c, w.bw), w.c, w.bw, w);
    const l = lit(gf - 1);
    ok('the glide: a jump (bookmark, typed frequency) is not streaked', sp === 0 && !!l && l[1] - l[0] === 4, `span ${sp} lit ${l}`);
  }
  // a zoom-drum step (×2) is not streaked; a pinch (5 % a row) fans connected
  ok('the glide: a ×2 zoom step is not streaked', glideMap(0, 195.3125, 0, 97.65625, N) === null);
  {
    let w = { c: 1_000_000, bw: 200_000 };
    for (let i = 0; i < 3; i++) gpush(noisyRow(w.c, w.bw), w.c, w.bw, w);
    const pinchFrom = gf - 1;
    for (let i = 0; i < 12; i++) { w = { c: w.c, bw: w.bw * 0.95 }; gpush(noisyRow(w.c, w.bw), w.c, w.bw, w); }
    const pg = connected(pinchFrom, gf);
    ok('the glide: a pinch fans the carrier out in one connected line', pg.length === 0, pg.join('; '));
  }
  ok('the glide: the history before the tune is untouched', steadyBefore.every((x, i) => x === G[i]));
  // cost: one streaked row, timed
  {
    const t0 = performance.now(); let w = { c: 1_000_000, bw: 200_000 };
    for (let i = 0; i < 2000; i++) { w = { c: w.c + 7_500 * (i % 2 ? 1 : -1), bw: 200_000 }; gpush(noisyRow(w.c, w.bw), w.c, w.bw, w); }
    const per = (performance.now() - t0) / 2000;
    console.log(`  glide cost (incl. building the test row): ${(per * 1000).toFixed(1)} µs per streaked row`);
  }
}

console.log(`  waterfall row placement: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
