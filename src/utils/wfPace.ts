/* wfPace.ts — WHICH ROWS THE WATERFALL SHOWS, on the display's clock (moved out of WaterfallView, the settled pacer
 * unchanged, so the pace can be tested in node: scripts/test_wf_pace.ts). Worklets: called from the UI-thread frame
 * callbacks (the JS path's pacer and the Android engine's poll).
 *
 * ★★★ A STEADY PACE (Stuart, 2026-10-28): "I would rather keep a STEADY PACE but have everything feel smooth rather than
 *     awkwardly ramp up and slow down like some of the v2 builds did." The scroll speed (rows per second down the screen)
 *     is the SAME before, during and after a tune or zoom. Only how it is DRAWN changes:
 *       settled     — whole rows, released on the display's beat from a ~40 ms playout buffer (rowPace, 2026-10-13);
 *       interacting — the same rows at the same rate, but the position is CONTINUOUS (contPace) and drawn every display
 *                     frame (120 Hz on ProMotion), so the shader's motion-compensated blend shows the trail moving
 *                     between data rows. The display rate rises while interacting and drops back after; the pace not.
 *     The two hand over in place (pos ↔ shown), keeping the buffer: before, a gesture SNAPPED to the newest row (a lurch
 *     forward of the whole buffer), a per-arrival glide restarted on every data row while the repeat rows landed on top of
 *     it (two motions), and the release sat still while the buffer refilled (a pause) — the ramp Stuart describes.
 *  ★ The rate is NOT re-measured while interacting: a tune can pause the feed (a re-subscribe), and folding that pause
 *    into the rate slowed the scroll for a beat afterwards. (avgFrameMs is frozen the same way in handleFrame.)
 *  ★ Nothing here asks a server for anything: all of it is the rows that already arrived.
 */

/** Rows held in hand, in ms of rows — the playout buffer (2026-10-13; Stuart: "30-50ms is imperceptible to most"). */
export const PLAYOUT_MS = 40;

/** ★★ V12 2026-10-29 (v12-wf-no-jb): how long after a frame's PRESENTATION time on the fixed clock its row is on screen —
 *  the depth clockPace holds in hand (PLAYOUT_MS, at least one row) — so the SPECTRUM TRACE, retargeted on the same
 *  schedule, can wait for it. Stuart: "if the buffer isnt needed for the waterfall improve the latency there too
 *  please". The waterfall's rows now skip the jitter buffer and go to the clock on arrival; the trace, through a jitter
 *  buffer of its own, would then trail the newest row by ~85 ms (scripts/test_wf_fixed_clock.ts — the trace "via its own
 *  jitter buffer" line). Retargeted at presentation + this, the trace and the row it goes with land together.
 *  `rate` rows/s (0 = off the clock: no wait). Pure — the Android engine has the same line (spectrum_engine.cpp). */
export function traceLagMs(rate: number): number {
  'worklet';
  return rate > 0 ? Math.max(1, rate * PLAYOUT_MS / 1000) * 1000 / rate : 0;
}

type Sv = { value: number };

/** The row rate (rows per ms) for the pacer, over ½ s windows. ★ The old per-arrival average swung ±15 % on jittery
 *  arrivals — enough to flip the pacer's mode (one row every 2nd frame ↔ a free count) and lose the rhythm. A change
 *  of more than 25 % (Default ↔ Smooth, a slower feed) is taken at once; anything less is smoothed.
 *  `frozen` (interacting): no measurement — the window restarts, so the first one after begins clean. */
export function rowRate(avail: number, t: number, rateSv: Sv, nSv: Sv, tSv: Sv, frozen = false): void {
  'worklet';
  const dt = t - tSv.value;
  if (frozen || tSv.value <= 0 || dt > 2000 || avail < nSv.value) { nSv.value = avail; tSv.value = t; return; }
  if (dt < 500) return;
  const inst = (avail - nSv.value) / dt;
  if (inst > 0) rateSv.value = Math.abs(inst / rateSv.value - 1) > 0.25 ? inst : rateSv.value * 0.6 + inst * 0.4;
  nSv.value = avail; tSv.value = t;
}

/* ★★★ THE RELEASE FOR CLOCKED ROWS (V12 2026-10-29 — the fixed output clock, src/assets/wfClock.ts; every mode but NATIVE,
 *  which keeps rowPace's own rules below, unchanged). Stuart: "over interpolate and discard … leaving headroom to account
 *  for link rate judder". The supply is now EXACTLY `rate` rows/s (repeats absorb the feed's drift upstream), so this only
 *  has to release them at that rate on the display's frames — evenly, and without running the buffer dry:
 *   • A PHASE ACCUMULATOR, no mode. acc += rate × gap each frame; a row goes when it passes a whole row. The gaps sum to
 *     real time, so the release runs at exactly the rate on ANY panel — every 2nd frame at 60 Hz, every 4th at 120, 3s and
 *     4s on the Mac's 90–100 Hz (rowPace locked Default 30 to every 3rd frame there: 31.7–33.3 rows/s out of a 30/s
 *     supply, and the buffer ran dry between slips — scripts/test_wf_fixed_clock.ts).
 *   • THE PHASE IS CENTRED half a frame's share away from the edge (acc starts at rate × gap / 2), so on a panel that is
 *     a whole multiple of the rate the frame jitter can never flip a row between two frames.
 *   • THE BUFFER IS HELD BY WHOLE-FRAME SLIPS, which keep the phase centred (a slip moves it by exactly one frame's share;
 *     a rate nudge would walk it to the edge): after a start or a stall it FILLS — one frame's wait every 6 frames until
 *     the depth is within 0.1 row of the target (≈1.2 rows at 30/s) — and is then HELD, corrected only past 0.7 row either
 *     way (one slip per 30 frames at most). With supply and release at one rate nothing else moves the depth; rowPace's
 *     ±0.6 deadband let it settle at ~0.7 row, where a row a millisecond late was a stall.
 *   • Never more rows in a frame than the rate's share rounded (Smooth 60 at 60 Hz: 1), never more than one row owed. */
export function clockPace(avail: number, shownSv: Sv, accSv: Sv, leadSv: Sv, coolSv: Sv, modeSv: Sv, rate: number,
                          gap: number, settled: boolean): number {
  'worklet';
  let shown = shownSv.value;
  const target = Math.max(1, rate * PLAYOUT_MS);
  const expect = rate * gap;
  if (!settled || shown <= 0 || shown > avail || avail - shown > target + 6) {
    shown = avail; accSv.value = Math.min(0.5, expect / 2); leadSv.value = 0; coolSv.value = 0; modeSv.value = 0;
  } else {
    const lead0 = avail - shown;
    let lead = leadSv.value;
    const cool = coolSv.value + 1;
    const far = Math.abs(lead - target) > 1.5;
    const slipOk = cool >= (far ? 6 : 30);
    // modeSv: 0 = FILLING (after a start or a stall: wait frames until the depth is within 0.1 row of the target), 1 =
    // HELD (correct only past 0.7 row either way — the depth then sits still, since supply and release share one rate).
    const filling = modeSv.value === 0;
    if (filling && lead >= target - 0.1) modeSv.value = 1;
    const shortBy = filling ? 0.1 : 0.7;
    if (!filling && lead < target - shortBy) modeSv.value = 0;
    let inc = expect;
    if ((filling ? cool >= 6 : slipOk) && lead < target - shortBy) { inc = 0; coolSv.value = 0; lead += expect; }   // short: wait a frame
    else if (slipOk && lead > target + 0.7 && lead0 >= 2) { inc = 2 * expect; coolSv.value = 0; lead -= expect; }   // ahead: a frame early
    else coolSv.value = cool;
    let acc = accSv.value + inc;
    const maxPer = Math.max(1, Math.round(expect));
    let n = 0;
    while (acc >= 1 && shown < avail && n < maxPer) { shown++; acc -= 1; n++; }
    if (acc >= 1) acc = Math.min(acc, 1 + expect / 2);     // a row owed (none in hand): at most one, released next
    accSv.value = acc;
    const a = Math.min(1, gap / 500);
    leadSv.value = lead + (avail - shown - lead) * a;
  }
  shownSv.value = shown;
  return shown;
}

/* ★★★ A PLAYOUT BUFFER (2026-10-13, Tab A9 at 60 with Smooth: ~1–2 % of drawn frames showed NO row — measured in-app,
 *  "a semi regular stutter"). The pacer before this held almost nothing in hand: after each release 0–1 rows were
 *  left, so a row landing a few ms after the vsync left that frame empty and the next took two. Rows arrive at
 *  59–68/s (network bursts, FFT batching) around a true 60 — the jitter is the problem, not the rate.
 *  Now the pacer keeps PLAYOUT_MS of rows in hand (≈2.4 rows at 60/s, 1 row minimum) and releases them on the
 *  frame beat, like the audio jitter buffer: a late row is still inside the buffer, so the beat never misses.
 *  The buffer depth is a SMOOTHED lead (≈½ s), and it is corrected by SLIPS — one extra row, or one frame's wait —
 *  no more than one per ½ s once settled. The feed and the display share no clock, so a slip now and then is the
 *  honest cost of a whole-row display; at a real 60 against 60 Hz it is ppm-rare. (Instant-backlog reserve rules,
 *  tried the night before, oscillated 0/4 at 30 — the smoothing and the cool-down are what stop that.) */
/** One frame of the settled row pacer: returns the row to SHOW. `settled` false resets it to the newest (a stall, a
 *  resume — and, before 2026-10-28, every gesture; gestures now hand over through contPace instead). */
export function rowPace(avail: number, shownSv: Sv, accSv: Sv, leadSv: Sv, coolSv: Sv, modeSv: Sv, rate: number,
                        gap: number, settled: boolean, clocked = false): number {
  'worklet';
  if (clocked) return clockPace(avail, shownSv, accSv, leadSv, coolSv, modeSv, rate, gap, settled);
  let shown = shownSv.value;
  const target = Math.max(1, rate * PLAYOUT_MS);     // rows to hold in hand
  if (!settled || shown <= 0 || shown > avail || avail - shown > target + 6) {
    // The first rows, a stall or a resume: show the newest; the buffer refills by waiting.
    shown = avail; accSv.value = 0; leadSv.value = 0; coolSv.value = 0; modeSv.value = 0;
  } else {
    const lead0 = avail - shown;            // rows in hand before this frame
    let lead = leadSv.value;                // …smoothed
    const expect = rate * gap;              // rows this frame at the measured rate
    const whole = Math.round(expect);
    const m = expect > 0 ? Math.round(1 / expect) : 0;
    const cool = coolSv.value + 1;
    /* ★ THE MODE, WITH HYSTERESIS: w (> 0) = w rows every frame, −m = one row every m frames, 0 = neither. The measured
     *  rate is noisy enough (±10 %) to flip a bare test frame to frame — and a flip mid-count misread the counter as
     *  rows (two at once, in simulation). Enter within 0.15 of the ratio, leave only past 0.3; a change zeroes it. */
    const prevMode = modeSv.value;
    let mode = 0;
    // ★ 0.75, not `whole >= 1`: Math.round(0.5) is 1, so Default on 60 Hz (0.5 rows a frame) fell between the two tests
    //   whenever the rate read a hair over 30/s — measured in simulation as the rhythm lost and a 5-row catch-up.
    if (expect >= 0.75) { const e = Math.abs(expect - whole); if (e < 0.15 || (prevMode === whole && e < 0.3)) mode = whole; }
    else if (m >= 2) { const e = Math.abs(expect * m - 1); if (e < 0.15 || (prevMode === -m && e < 0.3)) mode = -m; }
    if (mode !== prevMode) { modeSv.value = mode; accSv.value = 0; }
    // A slip may come every 6 frames while the buffer is well off target (filling after a gesture), else every 30.
    const far = Math.abs(lead - target) > 1.5;
    const slipOk = cool >= (far ? 6 : 30);
    if (mode > 0) {
      /* ★★ LOCKED: A WHOLE NUMBER OF ROWS PER FRAME (Stuart, 2026-10-13: 30 fps + Default "juddery still", 60 fine).
       *  At 30 drawn, Default is ONE row a frame; at 60, Smooth is one; at 30, Smooth is two. Each frame releases
       *  exactly that step — never a partial one: a frame that finds less waits (an underrun, now rare). */
      // ★ A slip's effect on the depth is known at once, so it is booked into the smoothed lead now — waiting for the
      //   ½ s average to see it would let a filling buffer slip again and again and overshoot.
      let step = mode;
      if (slipOk && lead > target + 0.6 && lead0 >= mode + 1) { step = mode + 1; coolSv.value = 0; lead -= 1; } // ahead
      else if (slipOk && lead < target - 0.6) { step = 0; coolSv.value = 0; lead += mode; }                     // short
      else coolSv.value = cool;
      if (lead0 >= step) shown += step;
      accSv.value = 0;
    } else if (mode < 0) {
      /* ★★ LOCKED THE OTHER WAY: ONE ROW EVERY m FRAMES (Default at 60 = every 2nd, at 120 = every 4th; Smooth at 120 =
       *  every 2nd). The fractional counter below broke the 1-0-1-0 rhythm on ~4 % of frames in simulation (23 % before
       *  the buffer); a frame count keeps it exact, and a slip is one frame early or late. accSv counts frames here. */
      const mm = -mode;
      const n = accSv.value + 1;              // frames since the last row
      let due = mm, slip = 0;
      if (slipOk && lead > target + 0.6 && lead0 >= 2) { due = mm - 1; slip = -1; }   // ahead: one frame early
      else if (slipOk && lead < target - 0.6) { due = mm + 1; slip = 1; }            // short: one frame late
      if (n >= due && lead0 >= 1) {
        shown++; accSv.value = 0;
        if (slip !== 0) { coolSv.value = 0; lead += slip / mm; } else coolSv.value = cool;
      } else { accSv.value = Math.min(n, 2 * mm); coolSv.value = cool; }
    } else {
      // Rows and frames at no whole ratio (40 rows on 60 Hz, a wandering rate): a fractional counter, nudged toward the target.
      const k = Math.max(0.85, Math.min(1.25, 1 + 0.15 * (lead - target)));
      let acc = accSv.value + expect * k;
      while (acc >= 1 && shown < avail) { shown++; acc -= 1; }
      accSv.value = Math.min(acc, shown === avail ? 1 : 2);
      coolSv.value = cool;
    }
    const a = Math.min(1, gap / 500);       // ≈½ s smoothing
    leadSv.value = lead + (avail - shown - lead) * a;
  }
  shownSv.value = shown;
  return shown;
}

/** The continuous pacer (interacting, or a slow Sharp feed that glides): the position in rows, fractional.
 *  ★ `rate` is the pace the SETTLED picture was actually scrolling at (rowRate over the SHOWN row, which rowPace locks to
 *    a whole number of display frames — exactly 20/s on a 20/s feed), not the arrival estimate (±5 % on a jittery feed:
 *    the first version ran 5–10 % fast for the whole gesture and corrected in lurches). Frozen while it runs.
 *  ★ `target` is the buffer it was handed (the settled lead at entry): it keeps that, nudging by at most ±3 % — a
 *    continuous scroll shows a speed change that whole-row steps hide, so the correction is slow and small.
 *  It never passes the newest row (a feed that pauses holds the picture still — it does not run on into nothing) and
 *  snaps only on a real stall/resume, as rowPace does. The shader shows position P as uHeadF = floor(P) + 1,
 *  uFrac = frac(P): the top line is frame floor(P) − 1 blended into floor(P) by frac(P), which at a whole P is exactly
 *  rowPace's picture of shown = P. */
export function contPace(avail: number, posSv: Sv, leadSv: Sv, rate: number, gap: number, target: number): number {
  'worklet';
  const tgt = Math.max(0.75, target);
  let pos = posSv.value;
  if (!(pos > 0) || pos > avail || avail - pos > tgt + 6) {
    pos = Math.max(avail > 0 ? 1 : 0, avail - tgt);
    leadSv.value = avail - pos;
  } else {
    const lead = leadSv.value;
    const k = Math.max(0.97, Math.min(1.03, 1 + 0.02 * (lead - tgt)));
    pos = Math.min(avail, pos + rate * gap * k);
    const a = Math.min(1, gap / 500);
    leadSv.value = lead + (avail - pos - lead) * a;
  }
  posSv.value = pos;
  return pos;
}

/** Settled → continuous: start where the whole-row picture is (no jump), with its buffer as the target. */
export function paceToCont(shownSv: Sv, posSv: Sv, leadSv: Sv, contLeadSv: Sv, targetSv: Sv): void {
  'worklet';
  posSv.value = shownSv.value;
  contLeadSv.value = leadSv.value;
  targetSv.value = leadSv.value;
}

/** The pace the SETTLED picture actually runs at, rows per ms: the time between its whole-row steps, averaged over
 *  ~10 steps (rowPace locks them to whole display frames, so this is exact where the arrival estimate is ±5 %, and it
 *  works at 5 rows/s where a ½ s window counts 2 or 3 rows). Call every settled frame; not while continuous. */
export function shownRate(shown: number, t: number, rateSv: Sv, lastNSv: Sv, lastTSv: Sv): void {
  'worklet';
  if (lastTSv.value <= 0 || shown < lastNSv.value || t - lastTSv.value > 3000) { lastNSv.value = shown; lastTSv.value = t; return; }
  const d = shown - lastNSv.value;
  if (d <= 0) return;
  const per = (t - lastTSv.value) / d;
  const r = 1 / per;
  rateSv.value = rateSv.value > 0 && Math.abs(r / rateSv.value - 1) < 0.5 ? rateSv.value * 0.9 + r * 0.1 : r;
  lastNSv.value = shown; lastTSv.value = t;
}
/** Continuous → settled waits for the position to reach a whole row (at most one row interval): this frame's advance
 *  would carry it there or past. Switching mid-row would step the picture forward by the fraction at once — a small
 *  lurch exactly when the hand finishes. */
export function contAtWholeRow(pos: number, rate: number, gap: number): boolean {
  'worklet';
  const frac = pos - Math.floor(pos);
  return frac < 1e-6 || 1 - frac <= rate * gap * 1.05;
}
/** Continuous → settled, on the frame the position reaches a whole row (contAtWholeRow): that row, the buffer carried
 *  over, the beat restarted. */
export function paceToSettled(avail: number, posSv: Sv, shownSv: Sv, accSv: Sv, leadSv: Sv, contLeadSv: Sv, modeSv: Sv): void {
  'worklet';
  shownSv.value = Math.min(avail, Math.round(posSv.value));   // contAtWholeRow: this frame landed on (or just past) it
  accSv.value = 0; modeSv.value = 0;
  leadSv.value = contLeadSv.value;
}

/* ── THE SCREEN EASE: a fan (zoom, or a jump re-mapped) drawn as a motion at display rate, not a snap per data row ──
 *  A fan re-makes the image at the new view (utils/wfRowPlace.ts); the screen move it made is x_old = c + k·x_new
 *  (takeFanStep). At the swap the shader is set to show the NEW image in the OLD geometry — image column u = A·x + B of
 *  screen x, A = A_now / k, B = (B_now − c) / k — and A, B ease to (1, 0) over one row interval: a zoom fans out/in, a
 *  jump slides in from the side, at 120 Hz. Composes with an ease still running (a pinch is a chain of steps). Outside
 *  the image (u < 0 or u > 1) the shader draws the floor: the edge the radio has not captured yet. */
export function easeAt(a0: number, b0: number, t0: number, dur: number, ts: number): [number, number] {
  'worklet';
  const p = dur > 0 ? Math.min(1, Math.max(0, (ts - t0) / dur)) : 1;
  return [1 + (a0 - 1) * (1 - p), b0 * (1 - p)];
}
export function easeCompose(a: number, b: number, k: number, c: number): [number, number] {
  'worklet';
  if (!(k > 0) || !isFinite(c)) return [1, 0];
  return [a / k, (b - c) / k];
}
