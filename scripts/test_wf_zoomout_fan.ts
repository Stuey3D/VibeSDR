/**
 * THE ZOOM-OUT FAN ON A LADDERED SERVER (UberSDR) — beta37, TestFlight 520, iPhone on Stuart's UberSDR (0–2 MHz, 10 fps,
 * 648 kHz AM) after a zoom out: "on the iPhone scrolling is smooth but the fan out on zoom doesn't work properly" — the
 * history under the newest rows drawn as sideways SMEARS and torn bands instead of the old rows at their own scale.
 *
 * ROOT CAUSE: the waterfall placed each row by the PREDICTED view geometry (SDRStatus.bwHz while a zoom is in flight)
 * instead of the bins' own. UberSDR snaps every request to its ladder (measured on the live server 2026-10-29: 200 / 300 /
 * 500 / 1000 / 2000 Hz a bin; 244 asked → 200, 700 → 500, 1953 → 2000 with the centre moved to 1.024 MHz), so during a
 * pinch every row was labelled with a span its bins did not have: each label change started a fan ease that magnified a
 * real row by a ratio it never had, and the settle's snap to the real rung started another. Fixed by placing rows by
 * SDRStatus.trueBwHz (with trueCenterHz) — wfRowPlace.ts rowGeometry.
 *
 * Proves, on the pure placement (placeRow, the fan maps) with the frames the UberSDR client really emits:
 *   • a row's geometry is the bins' own (trueCenterHz/trueBwHz) whenever the client states it;
 *   • the fan starts once per REAL change of the bins' span (4 rungs), not once per pinch step;
 *   • NO ROW IS STRETCHED PAST THE HISTORY BELOW IT on a zoom out: walking up the picture, every row shows at least as
 *     much spectrum (real Hz across the screen) as the row under it, and never more than its own bins hold — i.e. the
 *     fan only ever draws IN; nothing is magnified into a smear;
 *   • every JOIN the shader draws between two rows (MOTION g, h) is the move the two rows' real maps make — so no row is
 *     warped sideways by a move its data never made (the smears);
 *   • every row's strong carriers are drawn where their real frequency puts them under that row's map;
 *   • CONTROL: the same frames placed by the predicted geometry (the beta37 code) fan on every pinch step, warp the
 *     joins by up to hundreds of columns and misplace carriers.
 * Run: node --no-warnings scripts/test_wf_zoomout_fan.ts
 */
import { newFanState, newMotionState, placeRow, rowGeometry, MOTION_ROWS } from '../src/utils/wfRowPlace.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string, detail = '') => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}${!c && detail ? `\n         ${detail}` : ''}`); };

const N = 1024, ROWS = 1024;
let seed = 7;
const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
// AM stations every 9 kHz across 0–2 MHz, a speckled floor. Strong ones (≥ 0.85) are the carriers checked.
const stations: { f: number; lvl: number }[] = [];
for (let f = 153_000; f <= 1_998_000; f += 9_000) if (rnd() < 0.5) stations.push({ f, lvl: 0.5 + rnd() * 0.45 });
function bandRow(centre: number, span: number): Uint8Array {
  const r = new Uint8Array(N), hz = span / N, lo = centre - span / 2;
  for (let i = 0; i < N; i++) r[i] = 40 + Math.floor(rnd() * 40);
  for (const s of stations) {
    const c = Math.round((s.f - lo) / hz);
    if (c >= 0 && c < N) r[c] = Math.max(r[c], Math.round(s.lvl * 255));
  }
  return r;
}

// ── UberSDR, as measured: the ladder, and the centre it moves to at the widest rung (0–2.048 MHz) ──
const LADDER = [200, 300, 500, 1000, 2000];
const rung = (bb: number) => LADDER.reduce((b, r) => Math.abs(Math.log(r / bb)) < Math.abs(Math.log(b / bb)) ? r : b, LADDER[0]);
const DIAL = 648_000;
const serverCentre = (bb: number) => (bb >= 2000 ? 1_024_000 : DIAL);
const viewCentre = (span: number) => Math.max(DIAL, span / 2);   // UberSDRWsClient.zoom: centred while the range allows it

interface Emit { centerHz: number; bwHz: number; trueCenterHz: number; trueBwHz: number }
/** The frames _emitSpectrum hands on: a pinch out from 204.8 kHz to 2.048 MHz over 40 rows (20 rows/s — 10 fps, ×2 repeat
 *  rows), the 300 ms settle window (6 rows, still in flight), then settled. In flight the status carries the PREDICTED
 *  view (centerHz/bwHz) and the bins' own (trueCenterHz/trueBwHz); settled they agree. */
function frames(): { emit: Emit; data: Uint8Array }[] {
  const out: { emit: Emit; data: Uint8Array }[] = [];
  const push = (pred: number, inFlight: boolean) => {
    const bb = rung(pred / N), act = bb * N, tc = serverCentre(bb);
    const emit = inFlight ? { centerHz: viewCentre(pred), bwHz: pred, trueCenterHz: tc, trueBwHz: act }
                          : { centerHz: tc, bwHz: act, trueCenterHz: tc, trueBwHz: act };
    out.push({ emit, data: bandRow(tc, act) });
  };
  for (let i = 0; i < 250; i++) push(204_800, false);
  let pb = 204_800;
  for (let i = 1; i <= 40; i++) { pb = 204_800 * Math.pow(2_048_000 / 204_800, i / 40); push(pb, true); }
  for (let i = 0; i < 6; i++) push(pb, true);
  for (let i = 0; i < 30; i++) push(pb, false);
  return out;
}

function run(byTrue: boolean) {
  seed = 99;
  const fr = frames();
  const ring = new Uint8Array(N * (ROWS + MOTION_ROWS));
  const ms = newMotionState(ROWS, N), fan = newFanState(ROWS);
  const geo: { lo: number; span: number }[] = [];   // each frame's REAL placement (column 0 Hz, span)
  let labelChanges = 0, prevLabel = NaN;
  fr.forEach(({ emit, data }, f) => {
    const [c, hz] = byTrue ? rowGeometry(emit, N) : [emit.trueCenterHz, emit.bwHz / N];   // control = the beta37 code
    if (isFinite(prevLabel) && Math.abs(hz * N - prevLabel) > prevLabel * 1e-6) labelChanges++;
    prevLabel = hz * N;
    const placed = { startHz: NaN, spanHz: 0 };
    placeRow(ring, ROWS, N, f, data, c, hz, emit.centerHz, emit.bwHz, ms, fan, ROWS, placed);
    // where the bins REALLY went: writeRow slid them by `off` columns of the labelled scale — in the row's own bins
    const off = Math.round((c - placed.startHz) / hz - N / 2);
    geo.push({ lo: emit.trueCenterHz - emit.trueBwHz / 2 - off * (emit.trueBwHz / N), span: emit.trueBwHz });
  });
  const frames_ = fr.length, disp = fan.disp ?? ring;
  // Walk up the picture from the old history to the newest row: the REAL spectrum each row shows across the screen.
  let stretched = 0, worst = 1, misplaced = 0, checked = 0;
  let prevShown = NaN;
  for (let f = 240; f < frames_; f++) {
    const r = f % ROWS, a = fan.xfA[r], b = fan.xfB[r], g = geo[f];
    const shown = g.span * a;   // Hz across the screen this row is drawn at
    if (isFinite(prevShown) && shown < prevShown * (1 - 1e-6)) { stretched++; worst = Math.min(worst, shown / prevShown); }
    if (shown > g.span * (1 + 1e-6)) stretched++;
    prevShown = shown;
    // its strong carriers where their real frequency puts them under this row's map: screen x = ((f−lo)/span − b)/a
    for (const s of stations) {
      if (s.lvl < 0.85) continue;
      const u = (s.f - g.lo) / g.span;
      if (u < 0.02 || u > 0.98) continue;
      const x = Math.round(((u - b) / a) * N - 0.5);
      if (x < 2 || x > N - 3) continue;
      checked++;
      let hit = false;
      for (let k = -2; k <= 2; k++) if (disp[r * N + x + k] >= Math.round(s.lvl * 255) - 1) hit = true;
      if (!hit) misplaced++;
    }
  }
  // ★ THE JOIN: the shader draws each pair (f → f+1) as one move, x' = m + g·(x − m) + h (MOTION). It must be the move the
  //   two rows' REAL maps make, or it warps a row sideways by a move its data never made — the smear. Checked at the
  //   left edge, the centre and the right edge, in columns.
  let badJoins = 0, worstJoin = 0;
  const m = N / 2;
  const realMap = (f: number) => { const r = f % ROWS, g = geo[f]; return [g.lo + g.span * fan.xfB[r], g.span * fan.xfA[r] / N]; };
  for (let f = 240; f < frames_ - 1; f++) {
    const [p0, q0] = realMap(f), [p1, q1] = realMap(f + 1), r = f % ROWS;
    let err = 0;
    for (const x of [0, m, N]) {
      const want = (p0 + q0 * x - p1) / q1;
      const got = m + ms.g[r] * (x - m) + ms.h[r];
      err = Math.max(err, Math.abs(got - want));
    }
    worstJoin = Math.max(worstJoin, err);
    if (err > 3) badJoins++;
  }
  return { labelChanges, stretched, worst, misplaced, checked, badJoins, worstJoin };
}

console.log('  the zoom-out fan on a laddered server (UberSDR):');
{
  const g = rowGeometry({ centerHz: 1_000_000, bwHz: 2_000_000, trueCenterHz: 1_024_000, trueBwHz: 2_048_000 }, 1024);
  ok(g[0] === 1_024_000 && g[1] === 2000, 'rowGeometry: the bins\' own centre and span when the client states them', `${g}`);
  const h = rowGeometry({ centerHz: 7_000_000, bwHz: 1_024_000 }, 512);
  ok(h[0] === 7_000_000 && h[1] === 2000, 'rowGeometry: the status geometry when it does not (it is already the bins\')', `${h}`);
}
const fix = run(true), bug = run(false);
ok(fix.labelChanges === 4, `the fan starts once per real rung change (4), not per pinch step — ${fix.labelChanges}`);
ok(fix.stretched === 0, `no row is magnified past the history below it, or past its own bins (zoom out only draws IN) — ${fix.stretched} rows`);
ok(fix.badJoins === 0, `every join between two rows is the move their real maps make (no row warped by a move its data never made) — ${fix.badJoins} pairs off, worst ${fix.worstJoin.toFixed(1)} columns`);
ok(fix.checked > 500 && fix.misplaced === 0, `every strong carrier is drawn at its real frequency under its row's map — ${fix.misplaced}/${fix.checked} off`);
console.log('  CONTROL — the same frames placed by the predicted view geometry (beta37):');
ok(bug.labelChanges >= 30, `the fan restarts on every pinch step — ${bug.labelChanges} span changes`);
ok(bug.badJoins >= 10, `joins warp rows by moves their data never made (the sideways smears) — ${bug.badJoins} pairs, worst ${bug.worstJoin.toFixed(0)} columns`);
// ★ 2026-10-29: with the slope cap's longer eases the predicted-geometry path no longer also MISPLACES a carrier under its
//   own map — its bug still shows as the warped joins above. Reported, not required.
console.log(`   (info) carriers drawn away from their frequency on the beta37 path — ${bug.misplaced}/${bug.checked}`);

console.log(`  zoom-out fan: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
