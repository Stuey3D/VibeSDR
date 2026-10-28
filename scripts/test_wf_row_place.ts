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
 *   • MOTION (2026-10-28, replaced the glide streak): every pair of rows carries h, the move of its trail on screen, for the
 *     shader's motion-compensated blend — exact for a tuner retune (VibeServer, #57) AND a view-only pan over a fixed capture
 *     (UberSDR/Kiwi/OWRX); the image's tail rows decode to it; steady rows carry none (byte-for-byte as stored); a JUMP is
 *     re-mapped (no slice), black only at the newly exposed edge; a pinch carries no motion (the fan); the CPU reference of
 *     the shader's sample draws a moving station at every intermediate place, one full-strength copy (control: the old
 *     same-x blend shows two half-bright ones).
 *
 *   • THE ZOOM FAN (2026-10-28): through placeRow (WaterfallView's whole placement) and the DISPLAYED ring (fanImage):
 *     a zoom in / out / anchored zoom / pinch chain puts EVERY history row's carrier at the column the live view puts it;
 *     a zoom out leaves blank (0) edges and keeps a one-bin carrier (max under a pixel); zoom in then out gives back the
 *     stored ring byte for byte; a tune does not fan (the bend); unzoomed, the display IS the stored ring (steady rows
 *     byte-identical, no second buffer); a fast tune and a ×2 drum step both stay one connected trail on screen.
 *
 * Run: node --no-warnings scripts/test_wf_row_place.ts
 */
import { applyViewChange, decodeMotion, encodeMotion, fanImage, fanRefresh, MOTION_MAX_FRAC, MOTION_ROWS, MOTION_ZERO, motionSample, newFanState,
         newMotionState, placeRow, rowViewOffset, takeFanStep, wfImageBytes, writeRow } from '../src/utils/wfRowPlace.ts';

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

// ── MOTION: the move from each row to the next, for the shader's motion-compensated blend (2026-10-28) ──
{
  const MR = 256;
  const M = new Uint8Array(N * (MR + MOTION_ROWS));
  const ms = newMotionState(MR, N), mf = newFanState(MR);
  let mfr = 0;
  let seed = 7;
  const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
  /** A noisy server row: floor 34..46, an AM-ish station 5 bins wide peaking at 230. */
  function noisyRow(centre: number, span: number): Uint8Array {
    const r = new Uint8Array(N);
    for (let i = 0; i < N; i++) r[i] = 34 + Math.floor(rnd() * 13);
    const bin = Math.round((CARRIER - centre) / (span / N)) + N / 2;
    for (let d = -2; d <= 2; d++) if (bin + d >= 0 && bin + d < N) r[bin + d] = d === 0 ? 230 : 200;
    return r;
  }
  const mpush = (row: Uint8Array, rc: number, span: number, vc: number, vbw: number) => {
    placeRow(M, MR, N, mfr, row, rc, span / N, vc, vbw, ms, mf); mfr++;
  };
  const img = () => wfImageBytes(mf, ms, M, N, MR);
  const mcol = (f: number) => { const d = img(), s = (f % MR) * N; for (let i = 0; i < N; i++) if (d[s + i] === 230) return i; return -1; };
  let vc = 1_000_000; const vbw = 200_000;
  for (let i = 0; i < 10; i++) mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
  { seed = 99; const ref = noisyRow(vc, vbw); const slot = (mfr % MR) * N; seed = 99; mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
    ok('motion: a steady row is stored byte-for-byte (nothing is streaked any more)', ref.every((x, i) => x === M[slot + i]));
    let mv = 0; for (let f = 0; f < mfr; f++) if (ms.h[f % MR] !== 0) mv++;
    ok('motion: steady rows carry no motion and no range (the shader takes its unchanged path)', mv === 0 && ms.lo > ms.hi); }
  ok('motion: the 16-bit code round-trips at 1/64 column, both signs',
     [0, 1 / 64, -1 / 64, 7.5, -38.25, 127.984375, -128].every((h) => { const v = encodeMotion(h); return Math.abs(decodeMotion(v >> 8, v & 255) - h) < 1e-9; })
     && encodeMotion(0) === MOTION_ZERO);
  // a FAST tune on a VibeServer (the TUNER moves; rows built for the view two steps back — #57): 38 bins a row
  { const from = mfr; const cs: number[] = [];
    for (let i = 1; i <= 15; i++) { vc += 7_500; cs.push(vc); const rc = cs[Math.max(0, cs.length - 3)]; mpush(noisyRow(rc, 200_000), rc, 200_000, vc, vbw); }
    for (let i = 0; i < 10; i++) mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
    let bad = 0, moving = 0;
    for (let f = from - 1; f < mfr - 1; f++) { const h = ms.h[f % MR], a = mcol(f), b = mcol(f + 1); if (h !== 0) moving++; if (a < 0 || b < 0 || Math.abs(a + h - b) > 1.01) bad++; }
    ok('motion (tuner retune): every pair\'s h is exactly the carrier\'s move on screen', bad === 0 && moving >= 14, `${bad} bad, ${moving} moving`);
    ok('motion: the moving range covers the tune and nothing after it', ms.lo <= from && ms.hi >= from + 13 && ms.hi < mfr - 1, `[${ms.lo}, ${ms.hi}]`);
    ok('motion: the newest row has no motion (no newer row yet)', ms.h[(mfr - 1) % MR] === 0);
    const d = img(); let same = true;
    for (let r = 0; r < MR; r++) if (Math.abs(decodeMotion(d[MR * N + r], d[(MR + 1) * N + r]) - ms.h[r]) > 1 / 64) same = false;
    ok('motion: the image\'s two tail rows decode to every slot\'s h (the shader reads these)', same); }
  // a VIEW-ONLY pan over a fixed capture (UberSDR / Kiwi / OWRX): each row arrives already for its view — same rule
  { const from = mfr;
    for (let i = 1; i <= 12; i++) { vc -= 2_300; mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw); }
    let bad = 0; for (let f = from - 1; f < mfr - 1; f++) { const h = ms.h[f % MR], a = mcol(f), b = mcol(f + 1); if (Math.abs(a + h - b) > 1.01 || h === 0) bad++; }
    ok('motion (view pan over a fixed capture): every pair carries the pan', bad === 0, `${bad}`); }
  // a JUMP (bookmark, typed frequency, a spin faster than the radio): re-mapped, never a slice
  { for (let i = 0; i < 5; i++) mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
    const jf = mfr; vc += vbw * MOTION_MAX_FRAC * 1.5; mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
    const live = N / 2 + (CARRIER - vc) / (vbw / N);
    let off = 0; for (let f = jf - 5; f < jf; f++) if (Math.abs(mcol(f) - live) > 1.01) off++;
    ok('NO SLICE on a jump: every history row is shifted into place (re-mapped like a zoom)', off === 0, `${off} rows`);
    ok('…and the jump is not interpolated (a gap is honest, a smear is not)', ms.h[(jf - 1) % MR] === 0);
    const d = img(), s0 = ((jf - 1) % MR) * N, shift = Math.floor(vbw * MOTION_MAX_FRAC * 1.5 / (vbw / N));
    let edge = 0, kept = 0; for (let x = 0; x < N; x++) { if (x >= N - shift) { if (d[s0 + x] !== 0) edge++; } else if (d[s0 + x] !== 0) kept++; }
    ok('GAPS ONLY ON A JUMP: black exactly where the radio has not captured yet, the rest of the row kept', edge === 0 && kept === N - shift, `${edge} edge, ${kept} kept`);
    const st = takeFanStep(mf);
    ok('…and the UI is handed the slide to ease (k 1, c = the jump\'s share of the width)', st[0] === 1 && Math.abs(st[1] - MOTION_MAX_FRAC * 1.5) < 1e-9, `${st}`); }
  // a small tune is a BEND, not a re-map (the history stays where it was drawn — #57)
  { const from = mfr; vc += 2_000; mpush(noisyRow(vc, vbw), vc, vbw, vc, vbw);
    const st = takeFanStep(mf);
    ok('a small tune bends (motion), the history is not re-mapped', ms.h[(from - 1) % MR] !== 0 && st[0] === 1 && st[1] === 0, `${ms.h[(from - 1) % MR]} ${st}`); }
  // a PINCH never carries motion — the fan placed the history at the new scale
  { const from = mfr; let bw = vbw;
    for (let i = 0; i < 12; i++) { bw *= 0.95; mpush(noisyRow(vc, bw), vc, bw, vc, bw); }
    let mv = 0; for (let f = from - 1; f < mfr; f++) if (ms.h[f % MR] !== 0) mv++;
    ok('motion: a pinch carries no motion (the fan, not the blend, moves the history)', mv === 0, `${mv}`);
    const st = takeFanStep(mf);
    ok('…and the 12 pinch steps hand the UI one composed fan (0.95^12, about the centre)', Math.abs(st[0] - 0.95 ** 12) < 1e-9 && Math.abs(st[1] - (1 - 0.95 ** 12) / 2) < 1e-9, `${st}`); }

  // THE SHADER'S SAMPLE (motionSample, the CPU reference): a station moving 30 columns between two rows is drawn at
  // every intermediate place as t goes 0 → 1 — continuous, one copy, never two half-bright ghosts.
  { const W = 200, rowA = new Float64Array(W), rowB = new Float64Array(W);
    rowA.fill(0.2); rowB.fill(0.2); for (let d = -2; d <= 2; d++) { rowA[60 + d] = 0.9; rowB[90 + d] = 0.9; }
    const rd = (r: number, x: number) => { const R = r === 0 ? rowA : rowB; const i = Math.floor(x); return i < 0 || i >= W ? 0 : R[i]; };
    let jumps = 0, ghosts = 0, dim = 0; let prev = -1;
    for (let k = 0; k <= 20; k++) {
      const t = k / 20; const lit: number[] = [];
      for (let x = 0; x < W; x++) if (motionSample(rd, 0, 1, x + 0.5, t, 30, true) > 0.6) lit.push(x);
      const c = lit.length ? (lit[0] + lit[lit.length - 1]) / 2 : -1;
      if (lit.length === 0 || lit[lit.length - 1] - lit[0] > 6) ghosts++;
      if (lit.length && Math.max(...lit.map((x) => motionSample(rd, 0, 1, x + 0.5, t, 30, true))) < 0.85) dim++;
      if (prev >= 0 && Math.abs(c - prev) > 2.01) jumps++;
      if (Math.abs(c - (60 + 30 * t)) > 1.01) jumps++;
      prev = c;
    }
    ok('the motion-compensated blend: the station sits at 60 + 30·t for every t, moving ≤ 1.5 px a step (continuous)', jumps === 0, `${jumps}`);
    ok('…one copy at full strength, no ghost pair and no half-bright cross-fade', ghosts === 0 && dim === 0, `${ghosts} ghosts, ${dim} dim`);
    // CONTROL: the old blend at the same x (h = 0) cross-fades two places — the "chunky broken diagonal"
    let two = 0; for (let k = 5; k <= 15; k++) { const t = k / 20; const a = motionSample(rd, 0, 1, 60.5, t, 0, true), b = motionSample(rd, 0, 1, 90.5, t, 0, true); if (a > 0.3 && b > 0.3 && a < 0.85 && b < 0.85) two++; }
    ok('control: the same-x blend shows two half-bright copies mid-way (the fault the test must see)', two >= 9, `${two}`);
    // settled (blend false): ONE frame warped — at t the station is at 60 + 30·t too, at full strength
    let sbad = 0; for (let k = 0; k <= 10; k++) { const t = k / 10; let best = -1, bv = 0; for (let x = 0; x < W; x++) { const v = motionSample(rd, 0, 1, x + 0.5, t, 30, false); if (v > bv) { bv = v; best = x; } }
      if (Math.abs(best - (60 + 30 * t)) > 2.01 || bv < 0.9) sbad++; }
    ok('settled: the one frame a row shows is warped toward the row above — a real measurement, displaced', sbad === 0, `${sbad}`); }
}

// ── THE ZOOM FAN: a zoom re-maps the history (Stuart 2026-10-27/28 — the app was left in slices of the old scale) ──
{
  const R = 256;
  const S = new Uint8Array(N * (R + MOTION_ROWS));
  const gl = newMotionState(R, N), fan = newFanState(R);
  let fr = 0;
  let v = { c: 1_030_000, bw: 200_000 };
  /** One row as the server builds it — a ONE-bin carrier (the hardest to keep on a zoom out) on a flat floor — placed
   *  as pushRow does, against the view `v`. */
  const fpush = (rc: number, span: number) => {
    const r = new Uint8Array(N).fill(40);
    const bin = Math.round((CARRIER - rc) / (span / N)) + N / 2;
    if (bin >= 0 && bin < N) r[bin] = 230;
    placeRow(S, R, N, fr, r, rc, span / N, v.c, v.bw, gl, fan); fr++;
  };
  const D = () => fanImage(fan, S);
  /** The displayed row f's lit run [first, last] (≥ 200) or null. */
  const dlit = (f: number): [number, number] | null => {
    const d = D(), s = (f % R) * N; let a = -1, b = -1;
    for (let i = 0; i < N; i++) if (d[s + i] >= 200) { if (a < 0) a = i; b = i; }
    return a < 0 ? null : [a, b];
  };
  /** The column the LIVE view puts the carrier at. */
  const liveCol = () => (CARRIER - (v.c - v.bw / 2)) / (v.bw / N);
  /** Rows f0..f1-1 whose displayed carrier is not at the live column (± tol px), as text. */
  const offCol = (f0: number, f1: number, tol = 1.5) => {
    const bad: string[] = []; const want = liveCol();
    for (let f = f0; f < f1; f++) { const l = dlit(f); if (!l || l[0] > want + tol || l[1] < want - tol - 1) bad.push(`row ${f}: ${l} want ${want.toFixed(1)}`); }
    return bad;
  };
  const conn = (f0: number, f1: number) => {
    const g: string[] = [];
    for (let f = f0 + 1; f < f1; f++) { const p = dlit(f - 1), q = dlit(f); if (!p || !q || q[0] > p[1] + 1 || q[1] < p[0] - 1) g.push(`row ${f}: ${q} vs ${p}`); }
    return g;
  };

  for (let i = 0; i < 60; i++) fpush(v.c, v.bw);
  ok('the fan: unzoomed, the display IS the stored ring (no second buffer, steady rows byte-identical)', D() === S && fan.disp === null);
  const stored0 = S.slice(0, 60 * N);

  // (rows 60–63 are the tune below — they BEND, as they must, so the column checks skip them)
  // a TUNE (same span) does not fan: the history stays where it was drawn (the bend, #57)
  v = { c: v.c + 2_000, bw: v.bw }; for (let i = 0; i < 4; i++) fpush(v.c, v.bw);
  ok('the fan: a tune does not touch the history (no maps, display = stored)', D() === S && fan.xfA.every((a) => a === 1) && fan.xfB.every((b) => b === 0));
  v = { c: 1_030_000, bw: 200_000 }; for (let i = 0; i < 4; i++) fpush(v.c, v.bw);
  const preZoom = fr;

  // ×2 ZOOM IN about the dial (the drum): one frame from the old span still in flight, then the new span
  v = { c: 1_030_000, bw: 100_000 };
  fpush(1_030_000, 200_000);
  for (let i = 0; i < 6; i++) fpush(v.c, v.bw);
  { const bad = offCol(0, 60).concat(offCol(64, fr));
    ok('the fan: after a ×2 zoom in, EVERY history row shows the carrier at the live view\'s column (no slices)', bad.length === 0, bad.slice(0, 4).join('; ')); }
  ok('the fan: the stored rows are never rewritten by a zoom', stored0.every((x, i) => x === S[i]));
  { // control: what the app drew before the fan — the stored row as it came — is a slice at the OLD scale
    let c0 = -1; for (let x = 0; x < N; x++) if (S[x] >= 200) c0 = x;
    ok('control: without the fan the history row sits at its old column (the slice the test must see)', Math.abs(c0 - liveCol()) > 50, `${c0} vs ${liveCol()}`); }
  { const g = conn(preZoom, fr); ok('the fan: a ×2 drum step is ONE connected trail on screen (was a gap)', g.length === 0, g.join('; ')); }

  // a ZOOM OUT past the start (×4 from here): old rows draw in, the new edges are blank, the one-bin carrier survives
  v = { c: 1_030_000, bw: 400_000 };
  for (let i = 0; i < 6; i++) fpush(v.c, v.bw);
  { const bad = offCol(0, 60).concat(offCol(64, fr));
    ok('the fan: after a zoom OUT, every row\'s carrier is at the live column — a one-bin carrier is kept (max under a pixel)', bad.length === 0, bad.slice(0, 4).join('; ')); }
  { const d = D(); let edge = 0;
    for (let f = 0; f < 60; f++) { const s = f * N; for (let x = 0; x < N / 4 - 1; x++) if (d[s + x] !== 0 || d[s + N - 1 - x] !== 0) edge++; }
    ok('the fan: a zoom out leaves the never-seen edges blank (0), not smeared', edge === 0, `${edge} non-blank edge px`); }

  // back to 200 kHz: every row from before the zoom is the stored row again, byte for byte (lossless)
  v = { c: 1_030_000, bw: 200_000 };
  fpush(v.c, v.bw);
  { const d = D(); let diff = 0; for (let i = 0; i < 60 * N; i++) if (d[i] !== stored0[i]) diff++;
    ok('the fan: zoom in then back out restores the history exactly (edges and all)', diff === 0, `${diff} bytes differ`); }
  { const bad = offCol(0, 60).concat(offCol(64, fr)); ok('the fan: …and every row since, at every zoom it was drawn at, lines up', bad.length === 0, bad.slice(0, 4).join('; ')); }

  // an ANCHORED zoom (about the carrier — a pinch about the finger): the carrier stays put on screen
  { const xs = liveCol(); const span = 120_000; const lo = CARRIER - xs * (span / N);
    v = { c: lo + span / 2, bw: span }; fpush(v.c, v.bw);
    const bad = offCol(0, 60).concat(offCol(64, fr)); ok('the fan: an anchored zoom keeps the carrier pinned, history and all', bad.length === 0 && Math.abs(liveCol() - xs) < 1e-6, bad.slice(0, 4).join('; ')); }

  // a PINCH chain (5 % a row, about the centre): connected on screen, every row at the live column
  v = { c: 1_030_000, bw: 200_000 }; fpush(v.c, v.bw);
  { const from = fr - 1;
    for (let i = 0; i < 12; i++) { v = { c: v.c, bw: v.bw * 0.95 }; fpush(v.c, v.bw); }
    const g = conn(from, fr), bad = offCol(0, 60).concat(offCol(64, fr));
    ok('the fan: a pinch fans the carrier out in one connected line, the history with it', g.length === 0 && bad.length === 0, g.concat(bad).slice(0, 4).join('; ')); }

  // MOTION with zoomed history on screen: every moved pair's h predicts the displayed move
  { const from = fr; const cs: number[] = [];
    for (let i = 1; i <= 15; i++) { v = { c: v.c + 1_500, bw: v.bw }; cs.push(v.c); const rc = cs[Math.max(0, cs.length - 3)]; fpush(rc, v.bw); }
    let bad = 0; for (let f = from; f < fr - 1; f++) { const p = dlit(f), q = dlit(f + 1); if (!p || !q || Math.abs(p[0] + gl.h[f % R] - q[0]) > 1.01) bad++; }
    ok('the fan + motion: a fast tune over zoomed history — each pair\'s h is the move on screen', bad === 0, `${bad}`); }

  // RING rows after the last zoom, every row is the identity: the second buffer is dropped
  for (let i = 0; i < R; i++) fpush(v.c, v.bw);
  ok('the fan: once the zoomed history has scrolled away, the display buffer is freed', fan.disp === null && D() === S);

  // only the rows that can be ON SCREEN are made at a zoom; a resize that shows more makes the rest (fanRefresh)
  { const S2 = new Uint8Array(N * (R + MOTION_ROWS)), g2 = newMotionState(R, N), f2 = newFanState(R); let k = 0, w = { c: 1_030_000, bw: 200_000 };
    const p2 = (vis: number) => { const r = new Uint8Array(N).fill(40); const bin = Math.round((CARRIER - w.c) / (w.bw / N)) + N / 2; r[bin] = 230;
                                   placeRow(S2, R, N, k++, r, w.c, w.bw / N, w.c, w.bw, g2, f2, vis); };
    for (let i = 0; i < 200; i++) p2(64);
    w = { c: 1_030_000, bw: 100_000 }; p2(64);
    const want = Math.round((CARRIER - (w.c - w.bw / 2)) / (w.bw / N));
    const col = (f: number) => { const d = fanImage(f2, S2), s0 = (f % R) * N; for (let x = 0; x < N; x++) if (d[s0 + x] >= 200) return x; return -1; };
    let shown = 0, below = 0; for (let f = k - 64; f < k; f++) if (Math.abs(col(f) - want) <= 1) shown++; for (let f = 0; f < k - 70; f++) if (Math.abs(col(f) - want) <= 1) below++;
    ok('the fan: a zoom makes the rows on screen (64 visible) and leaves the off-screen rest for later', shown === 64 && below === 0, `${shown} shown, ${below} below`);
    const did = fanRefresh(f2, S2, N, R, k, R); let all = 0; for (let f = 0; f < k; f++) if (Math.abs(col(f) - want) <= 1) all++;
    ok('the fan: a resize that shows the whole ring makes every row at the live zoom (fanRefresh)', did && all === k && !fanRefresh(f2, S2, N, R, k, R), `${all}/${k}`); }

  // the pure maps: a pinch in and back out returns to the EXACT identity
  { const A = new Float64Array([1]), B = new Float64Array([0]); let lo = 900_000, sp = 200_000;
    for (const s2 of [160_000, 130_000, 100_000, 80_000, 100_000, 130_000, 160_000, 200_000]) { const l2 = 1_000_000 - s2 / 2; applyViewChange(A, B, lo, sp, l2, s2); lo = l2; sp = s2; }
    ok('the fan: a pinch in and out snaps back to the exact identity', A[0] === 1 && B[0] === 0, `${A[0]} ${B[0]}`); }

  // cost: a full 1024-row ring, a ×2 zoom step (in then out), timed
  { const RR = 1024, SS = new Uint8Array(N * (RR + MOTION_ROWS)); const g2 = newMotionState(RR, N), f2 = newFanState(RR); let k = 0;
    const row = new Uint8Array(N); for (let i = 0; i < N; i++) row[i] = 40 + (i * 7919 % 50);
    for (let i = 0; i < RR; i++) placeRow(SS, RR, N, k++, row, 1e6, 200_000 / N, 1e6, 200_000, g2, f2);
    const steps = 20; let bw = 200_000, t0 = performance.now();
    for (let i = 0; i < steps; i++) { bw *= 0.97; placeRow(SS, RR, N, k++, row, 1e6, bw / N, 1e6, bw, g2, f2); }
    const tin = (performance.now() - t0) / steps;
    t0 = performance.now();
    for (let i = 0; i < steps * 2; i++) { bw /= 0.97; placeRow(SS, RR, N, k++, row, 1e6, bw / N, 1e6, bw, g2, f2); }
    const tout = (performance.now() - t0) / (steps * 2);
    console.log(`  zoom fan cost (1024-row ring, every row re-made, this JS engine): ${tin.toFixed(2)} ms a zoom-in step, ${tout.toFixed(2)} ms a zoom-out step`); }
}

console.log(`  waterfall row placement: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
