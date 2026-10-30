/**
 * SCALE STAMPS ON THE WATERFALL HISTORY (src/utils/wfStamps.ts) — GitHub discussion #57, Stuart 2026-10-30: "zoom in and a
 * stamp of the previous frequency scale as it was is added onto the old waterfall position and it gently drops off the
 * bottom of the screen, same with tuning too if needed".
 *
 * Drives the detector the way the three paths do — one call a row with the row's DISPLAYED geometry — through the real
 * zoom/tune ease (wfRowPlace.ts placeRow, so the rows in between are the eased ones the picture draws), and proves:
 *   • a zoom places ONE stamp, once the ease has landed, on the newest row at the OLD scale, carrying the old scale;
 *   • a tune of more than a twentieth of the view places one; a small nudge (history visibly continuous) places none;
 *   • a sweep (a drum spin: a step every few rows) places ONE stamp when it stops, not one per step;
 *   • a jump moves the stamps with the history (it moved into place) and places none for itself;
 *   • several moves → several stamps, newest first, capped at STAMP_MAX; a stamp squeezed by the next is hidden;
 *   • y follows the scroll exactly (rows newer than it × row height), at any row height — a resize keeps it at its rows;
 *   • it fades in, fades out over the bottom of the view, and is pruned once its rows have gone.
 * Run: node --no-warnings scripts/test_wf_stamps.ts
 */
import { newStampState, stampNoteRow, stampJump, stampPrune, stampReset, stampSettleRows, stampsShown, stampAlpha, stampTicks,
         stampMeaningful, stampLine, STAMP_MAX, type StampState } from '../src/utils/wfStamps.ts';
import { newFanState, newMotionState, placeRow, displayMap } from '../src/utils/wfRowPlace.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const N = 1024, ROWS = 1024, RATE = 30;
const SETTLE = stampSettleRows(RATE);

/** The app's ring, driven as pushRow drives it: each row placed at its own centre/span, noted with its displayed map. */
function rig() {
  const ring = new Uint8Array(N * (ROWS + 4));
  const ms = newMotionState(ROWS, N), fan = newFanState(ROWS);
  fan.rowRate = RATE;
  const st = newStampState();
  const row = new Uint8Array(N);
  let f = 0, events = 0;
  const push = (centre: number, span: number) => {
    const slot = placeRow(ring, ROWS, N, f, row, centre, span / N, centre, span, ms, fan, ROWS);
    if (fan.jumpHz) stampJump(st, fan.jumpHz);
    const m = displayMap(ms, fan.xfA, fan.xfB, slot, N);
    if (m && stampNoteRow(st, f, m[0], m[1] * N, SETTLE)) events++;
    f++;
  };
  const hold = (centre: number, span: number, rows: number) => { for (let i = 0; i < rows; i++) push(centre, span); };
  return { st, push, hold, get f() { return f; }, get events() { return events; } };
}

console.log('A ZOOM');
{
  const r = rig();
  r.hold(7_100_000, 200_000, 100);
  const lastOld = r.f - 1;
  r.hold(7_100_000, 100_000, 3);
  ok(r.st.stamps.length === 0, 'no stamp while the ease is still running');
  r.hold(7_100_000, 100_000, 60);
  ok(r.st.stamps.length === 1, `ONE stamp once the zoom has landed (${r.st.stamps.length})`);
  const s = r.st.stamps[0];
  ok(s && s.frame === lastOld, `on the newest row at the OLD scale (frame ${s?.frame}, want ${lastOld})`);
  ok(s && Math.abs(s.lo - 7_000_000) < 1 && Math.abs(s.span - 200_000) < 1, `carrying the old scale (${s?.lo} + ${s?.span})`);
  ok(s && s.born > lastOld + SETTLE, `placed after the ease and the settle (born ${s?.born})`);
  r.hold(7_100_000, 100_000, 300);
  ok(r.st.stamps.length === 1, 'and no more while it holds');
}

console.log('A TUNE');
{
  const r = rig();
  r.hold(7_100_000, 200_000, 100);
  r.hold(7_120_000, 200_000, 60);      // a tenth of the view
  ok(r.st.stamps.length === 1, `a tune of 1/10 of the view places a stamp (${r.st.stamps.length})`);
  ok(r.st.stamps[0] && Math.abs(r.st.stamps[0].lo - 7_000_000) < 1, 'at the old scale');
  r.hold(7_123_000, 200_000, 60);      // 1.5 % — the history visibly continues
  ok(r.st.stamps.length === 1, 'a nudge of 1.5 % of the view places none');
  // …but nudges that ADD UP to a meaningful move are measured from the last settled scale, each separately: by design a
  // settled nudge resets the reference (each block's labels are within a nudge of its neighbour's).
}

console.log('A SWEEP');
{
  const r = rig();
  r.hold(7_100_000, 200_000, 100);
  const lastOld = r.f - 1;
  for (let k = 1; k <= 40; k++) r.hold(7_100_000 + k * 2_000, 200_000, 4);   // a drum spin: a step every 4 rows
  ok(r.st.stamps.length === 0, 'no stamp during the spin');
  r.hold(7_180_000, 200_000, 60);
  ok(r.st.stamps.length === 1, `ONE stamp when the spin stops (${r.st.stamps.length})`);
  ok(r.st.stamps[0]?.frame === lastOld, 'on the rows from before the spin');
  // a pinch: many small zoom steps
  const r2 = rig();
  r2.hold(7_100_000, 200_000, 100);
  for (let k = 1; k <= 20; k++) r2.hold(7_100_000, 200_000 * Math.pow(0.95, k), 2);
  r2.hold(7_100_000, 200_000 * Math.pow(0.95, 20), 60);
  ok(r2.st.stamps.length === 1, `a pinch sweep: ONE stamp (${r2.st.stamps.length})`);
}

console.log('A JUMP');
{
  const r = rig();
  r.hold(7_100_000, 200_000, 100);
  r.hold(7_100_000, 100_000, 60);                       // a zoom: stamp of 200 kHz at lo 7.0 MHz
  ok(r.st.stamps.length === 1, 'the zoom stamp');
  r.hold(9_500_000, 100_000, 60);                       // a bookmark: the history moves into place
  ok(r.st.stamps.length === 1, `the jump places none of its own (${r.st.stamps.length})`);
  const s = r.st.stamps[0];
  ok(s && Math.abs(s.lo - (7_000_000 + 2_400_000)) < 1, `the stamp moved with the history (+2.4 MHz: ${s?.lo})`);
}

console.log('SEVERAL, THE CAP, THE SQUEEZE');
{
  const r = rig();
  let span = 400_000;
  r.hold(7_100_000, span, 50);
  for (let k = 0; k < 9; k++) { span /= 1.5; r.hold(7_100_000, span, 60); }
  ok(r.st.stamps.length === STAMP_MAX, `capped at ${STAMP_MAX} (${r.st.stamps.length})`);
  ok(r.st.stamps.every((s, i, a) => i === 0 || a[i - 1].frame > s.frame), 'newest first');
  ok(r.st.stamps.every((s, i, a) => i === 0 || a[i - 1].span < s.span), 'each with its own block\'s scale (zooming in: older = wider)');
  const st: StampState = newStampState();
  st.stamps = [{ id: 3, frame: 500, lo: 0, span: 1, born: 0 }, { id: 2, frame: 495, lo: 0, span: 2, born: 0 },
               { id: 1, frame: 400, lo: 0, span: 3, born: 0 }];
  const shown = stampsShown(st.stamps, 1, 14);
  ok(shown.length === 2 && shown[0].id === 3 && shown[1].id === 1, 'a stamp 5 rows under the next (14 px tall) is hidden');
}

console.log('Y FOLLOWS THE SCROLL; FADE; PRUNE; RESIZE');
{
  // y = rows newer than the stamp × row height — at 1 px and at 2 px rows (a rotation), the same rows
  const frame = 1000, head = 1101;
  for (const rowPx of [1, 2, 0.75]) {
    const y = (head - 1 - frame) * rowPx;
    ok(Math.abs(y - 100 * rowPx) < 1e-9, `100 newer rows at ${rowPx} px a row: y ${y}`);
  }
  // the app's shader: the line stampLine gives is row `frame`'s first, the line above it the next newer row's
  const shaderFrame = (head: number, frac: number, uN: number, quant: boolean, Lc: number) => {
    const L = quant ? Math.floor(Lc) : Lc;
    let R = frac * uN; if (quant) R = Math.floor(R + 0.0001);
    return Math.floor(((head - 2) * uN + R - L) / uN);
  };
  for (const [frac, uN] of [[1, 1], [0.4, 1], [1, 2], [0.5, 3]] as const) {
    const L = stampLine(head, frame, frac, uN, true);
    ok(shaderFrame(head, frac, uN, true, L + 0.5) === frame && shaderFrame(head, frac, uN, true, L - 0.5) === frame + 1,
       `settled (frac ${frac}, uN ${uN}): line ${L} is the top of row ${frame}, the line above it row ${frame + 1}`);
    const Lg = stampLine(head, frame, frac, uN, false);
    ok(shaderFrame(head, frac, uN, false, Lg + 0.01) === frame && shaderFrame(head, frac, uN, false, Lg - 0.01) === frame + 1,
       `gliding (frac ${frac}, uN ${uN}): the boundary at line ${Lg.toFixed(2)}`);
  }
  ok(stampLine(head + 5, frame, 1, 1, true) - stampLine(head, frame, 1, 1, true) === 5, 'five rows later it is five lines lower');
  ok(stampAlpha(0, 14, 600, 0) === 0, 'fades in: invisible the row it is placed');
  ok(stampAlpha(100, 14, 600, 8) === 1, 'fully shown mid-screen');
  const a1 = stampAlpha(500, 14, 600, 100), a2 = stampAlpha(560, 14, 600, 100);
  ok(a1 > a2 && a2 > 0 && a1 < 1, `fades out over the bottom (${a1.toFixed(2)} → ${a2.toFixed(2)})`);
  ok(stampAlpha(590, 14, 600, 100) === 0, 'gone at the bottom edge');
  const r = rig();
  r.hold(7_100_000, 200_000, 100); r.hold(7_100_000, 100_000, 60);
  ok(!stampPrune(r.st, r.f - 1, 600), 'kept while its rows are on screen');
  r.hold(7_100_000, 100_000, 700);
  ok(stampPrune(r.st, r.f - 1, 600) && r.st.stamps.length === 0, 'pruned once they have gone');
  r.hold(7_100_000, 50_000, 60);
  ok(r.st.stamps.length === 1, 'a new one after');
  stampReset(r.st);
  ok(r.st.stamps.length === 0, 'reset (history cleared) drops them');
}

console.log('THE THRESHOLD, THE TICKS');
{
  ok(stampMeaningful(0, 100, 0, 70.7), 'a ×√2 zoom is meaningful');
  ok(!stampMeaningful(0, 100, 1, 100), 'a 1 % tune is not');
  ok(stampMeaningful(0, 100, 6, 100), 'a 6 % tune is');
  const t = stampTicks(7_000_000, 200_000, 400, () => 20_000, (hz) => (hz / 1e6).toFixed(2) + 'M', 52);
  ok(t.length === 11 && t[0].x === 0 && Math.abs(t[10].x - 400) < 1e-9, `ticks at the step across the width (${t.length})`);
  ok(t.filter(k => k.label).length === 6 && t[0].label === '7.00M', `labels no closer than the gap (${t.filter(k => k.label).map(k => k.label).join(' ')})`);
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
