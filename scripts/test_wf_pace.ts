/**
 * THE WATERFALL'S PACE THROUGH A TUNE (src/utils/wfPace.ts — WaterfallView's display driver). Stuart, 2026-10-28: "I would
 * rather keep a STEADY PACE but have everything feel smooth rather than awkwardly ramp up and slow down like some of the
 * v2 builds did."
 *
 * Simulates what the UI thread does each display frame (paceTick): rows arrive at 20/s with network jitter; the display
 * runs at 120 Hz (ProMotion) or 60 Hz; the user tunes for 2 s (uQuant = 0 → contPace, drawn continuously, rate frozen),
 * then lets go (rowPace, whole rows). Proves:
 *   • the scroll pace (rows per second down the screen) is the SAME settled, interacting and after — every ½ s window;
 *   • no lurch at either hand-over (settled → continuous → settled moves at most one row in a frame);
 *   • continuous really is continuous: every 120 Hz frame advances (no held frames between rows), by an even amount;
 *   • a feed that PAUSES mid-tune (a re-subscribe) holds the picture still, then the pace comes back to the same rate —
 *     not slower (the rate is not re-measured while interacting) and not a catch-up burst;
 *   • the screen ease: a pinch's chain of steps composes into one continuous geometry (no snap), back at rest after.
 * Run: node --no-warnings scripts/test_wf_pace.ts
 */
import { contAtWholeRow, contPace, shownRate, easeAt, easeCompose, paceToCont, paceToSettled, rowPace, rowRate } from '../src/utils/wfPace.ts';

let fails = 0, passes = 0;
function ok(what: string, cond: boolean, detail = '') {
  if (cond) { passes++; return; }
  fails++; console.log(`  FAIL ${what}${detail ? `\n       ${detail}` : ''}`);
}
const sv = (v: number) => ({ value: v });

interface Run { t: number[]; pos: number[]; cont: boolean[] }
/** Drive the pacer like WaterfallView's paceTick. `interact(t)` = the smooth-tune window; `feedGap(t)` = a pause. */
function simulate(hz: number, durMs: number, interact: (t: number) => boolean, pause: (t: number) => boolean, rowsPerS = 20, seed = 3,
                  old = false): Run {
  let s = seed; const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff; };
  const frame = 1000 / hz;
  const shown = sv(0), acc = sv(0), lead = sv(0), cool = sv(0), mode = sv(0), rate = sv(0.03), rT = sv(0), rN = sv(0);
  const pos = sv(0), cLead = sv(0), cTgt = sv(1), sRate = sv(0), sN = sv(0), sT = sv(0);
  let cont = false, avail = 0, nextRow = 0;
  const out: Run = { t: [], pos: [], cont: [] };
  for (let t = 0; t < durMs; t += frame) {
    // rows that arrived by now (20/s ± 12 ms jitter), none while paused
    while (nextRow <= t) { if (!pause(nextRow)) avail++; nextRow += 1000 / rowsPerS + (rnd() - 0.5) * 24; }
    const inter = interact(t);
    rowRate(avail, t, rate, rN, rT, inter);
    if (!cont) shownRate(shown.value, t, sRate, sN, sT);   // the pace the settled picture actually runs at
    if (old) {   // BEFORE 2026-10-28: a gesture showed the newest row (rowPace settled=false), the glide on top
      const p = rowPace(avail, shown, acc, lead, cool, mode, rate.value, frame, !inter);
      out.t.push(t); out.pos.push(p); out.cont.push(inter); continue;
    }
    const c = inter;                                   // uQuant 0 while interacting (a gesture always glides)
    const cRate = sRate.value > 0 ? sRate.value : rate.value;
    if (c && !cont) { paceToCont(shown, pos, lead, cLead, cTgt); cont = true; }
    else if (!c && cont && contAtWholeRow(pos.value, cRate, frame)) {
      contPace(avail, pos, cLead, cRate, frame, cTgt.value);   // this frame's advance lands on the row…
      paceToSettled(avail, pos, shown, acc, lead, cLead, mode); cont = false;
      out.t.push(t); out.pos.push(shown.value); out.cont.push(false); continue;   // …which is shown, whole
    }
    let p: number;
    if (cont) p = contPace(avail, pos, cLead, cRate, frame, cTgt.value);
    else p = rowPace(avail, shown, acc, lead, cool, mode, rate.value, frame, true);
    out.t.push(t); out.pos.push(p); out.cont.push(cont);
  }
  return out;
}
/** Rows per second over [a, b) ms. */
const paceOver = (r: Run, a: number, b: number) => {
  const i = r.t.findIndex((t) => t >= a), j = r.t.findIndex((t) => t >= b);
  return (r.pos[j] - r.pos[i]) / ((r.t[j] - r.t[i]) / 1000);
};

for (const hz of [120, 60]) {
  const r = simulate(hz, 12000, (t) => t >= 6000 && t < 8000, () => false);
  const windows: string[] = []; let worst = 0;
  for (let a = 3000; a < 11000; a += 1000) { const p = paceOver(r, a, a + 1000); windows.push(p.toFixed(1)); worst = Math.max(worst, Math.abs(p / 20 - 1)); }
  ok(`${hz} Hz: a STEADY PACE — every 1 s window before, during and after the tune within 6 % of 20 rows/s (one settled slip is 5 %)`, worst < 0.06, windows.join(' '));
  let lurch = 0;
  for (let i = 1; i < r.t.length; i++) if (r.t[i] > 3000 && r.pos[i] - r.pos[i - 1] > 1.01) lurch++;
  ok(`${hz} Hz: no lurch — never more than one row in a frame, hand-overs included`, lurch === 0, `${lurch}`);
  let back = 0; for (let i = 1; i < r.t.length; i++) if (r.pos[i] < r.pos[i - 1] - 1e-9) back++;
  ok(`${hz} Hz: never backwards`, back === 0, `${back}`);
  // continuous: every interacting frame advances, evenly
  const steps: number[] = []; for (let i = 1; i < r.t.length; i++) if (r.cont[i] && r.cont[i - 1] && r.t[i] > 6200) steps.push(r.pos[i] - r.pos[i - 1]);
  const want = 20 / hz, held = steps.filter((d) => d < 1e-9).length, uneven = steps.filter((d) => Math.abs(d / want - 1) > 0.06).length;
  ok(`${hz} Hz: interacting, the scroll is CONTINUOUS — every frame moves ${want.toFixed(3)} rows ±6 % (no held frames, no steps)`,
     held === 0 && uneven <= steps.length * 0.02, `${held} held, ${uneven}/${steps.length} uneven`);
}
// CONTROL — the OLD gesture path (snap to the newest row, then back to the paced beat): the lurch the test must see
{
  const r = simulate(120, 12000, (t) => t >= 6000 && t < 8000, () => false, 20, 3, true);
  let lurch = 0; for (let i = 1; i < r.t.length; i++) if (r.t[i] > 3000 && r.pos[i] - r.pos[i - 1] > 1.01) lurch++;
  ok('control: the old gesture path lurches forward a buffer\'s worth when the tune starts', lurch >= 1, `${lurch}`);
}
// a PAUSE mid-tune (a re-subscribe): held still, then the same pace — not slower, not a burst
{
  const r = simulate(120, 14000, (t) => t >= 6000 && t < 9000, (t) => t >= 7000 && t < 7300);
  let burst = 0; for (let i = 1; i < r.t.length; i++) if (r.cont[i] && r.cont[i - 1] && r.pos[i] - r.pos[i - 1] > 0.34) burst++;
  ok('a paused feed mid-tune: held still, then no catch-up burst (never 2× a frame\'s share)', burst === 0, `${burst}`);
  const after = [paceOver(r, 7800, 8800), paceOver(r, 9500, 10500), paceOver(r, 11000, 12000)];
  ok('…and the pace after it is the same 20 rows/s, during the tune and after it (the rate was not re-measured)',
     after.every((p) => Math.abs(p / 20 - 1) < 0.08), after.map((p) => p.toFixed(1)).join(' '));
}
// a SLOW feed (UberSDR ~5 fps, Sharp: continuous at rest too) keeps its pace as well
{
  const r = simulate(120, 16000, (t) => t >= 8000 && t < 10000, () => false, 5);
  let worst = 0; const w: string[] = [];
  for (let a = 5000; a < 15000; a += 1000) { const p = paceOver(r, a, a + 1000); w.push(p.toFixed(2)); worst = Math.max(worst, Math.abs(p / 5 - 1)); }
  ok('a 5 rows/s feed: the same steady pace through a tune', worst < 0.1, w.join(' '));
}

// THE SCREEN EASE: a pinch of 12 steps of 5 % (one per row, 50 ms apart) eases continuously — the geometry never jumps
{
  let a0 = 1, b0 = 0, t0 = 0, on = false; const dur = 50;
  const shownAt = (ts: number) => (on ? easeAt(a0, b0, t0, dur, ts) : [1, 0]);
  // the image column under screen x = 0.9 must move smoothly in SCREEN terms: the stored-view column it maps to
  let worst = 0, prevScreen = NaN, viewK = 1, viewC = 0;   // the current image ↔ the starting view: x_start = viewC + viewK·u
  for (let ts = 0; ts <= 1000; ts += 1000 / 120) {
    const step = Math.round(ts / 50);
    if (ts > 0 && ts % 50 < 1000 / 120 && step <= 12) {
      const k = 0.95, c = 0.025;           // about the centre
      const [a, b] = shownAt(ts); const n = easeCompose(a, b, k, c); a0 = n[0]; b0 = n[1]; t0 = ts; on = true;
      viewC = viewC + viewK * c; viewK *= k;
    }
    const [a, b] = shownAt(ts);
    const u = a * 0.9 + b;                   // image column at screen 0.9
    const screenInStartView = viewC + viewK * u;
    if (!isNaN(prevScreen)) worst = Math.max(worst, Math.abs(screenInStartView - prevScreen));
    prevScreen = screenInStartView;
  }
  ok('the screen ease: a pinch fans the history in one continuous motion (< 0.6 % of the width a frame, no snap)', worst < 0.006, worst.toFixed(4));
  const [a, b] = easeAt(a0, b0, t0, dur, 2000);
  ok('…and comes to rest at exactly the identity (steady rows exactly as before)', a === 1 && b === 0);
}

console.log(`  waterfall pace: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);
