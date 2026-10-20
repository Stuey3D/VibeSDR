/**
 * ★★★ WATERFALL ROW PACING — pure functions, so the cadence can be proved without a browser
 *     (scripts/test_wf_pacing.ts). waterfall.ts and main.ts's render loop both use these.
 *
 * The rule they keep: ONE whole row every N display frames, N a whole number, every row a REAL
 * measurement (repeated, never blended — Stuart's rule 7bce170d). What went wrong (2026-10-20,
 * Stuart on the Pi 500 in Safari: "a bit sticky, seems to judder a little"):
 *
 *  1. N SAT ON A ROUNDING KNIFE-EDGE. DEFAULT is 20 screen rows/s = 40 device rows/s on a Retina
 *     canvas, and a 60 Hz display divided by 40 is EXACTLY 1.5. N was Math.round(measuredHz / 40)
 *     with measuredHz a raw EMA of frame intervals, so 59.99 Hz gave N = 1 (60 rows/s) and 60.01 gave
 *     N = 2 (30 rows/s) — and it was re-decided at every arrival. MEASURED in headless Edge at dpr 2:
 *     43-60 rows/s second to second, 24 hitches/s, a quarter of rows landing two-in-one-frame.
 *     Fix: snap the measurement to the panel's NOMINAL rate (60 is 60), measure it from the MEDIAN
 *     interval (a skipped frame or a GC pause cannot move it), and give N hysteresis.
 *  2. THE RENDER THROTTLE DROPPED REAL FRAMES. main.ts skipped any frame closer than
 *     1000/renderHz − 1 ms to the last; at renderHz 60 on a 60 Hz panel that is 15.7 ms, so a rAF
 *     callback running ~1 ms early (performance.now() inside the callback, which jitters on Safari)
 *     was thrown away whole — a 33 ms hole in the scroll, which also inflated the frame EMA and
 *     pushed (1) across its edge. And a render rate that is not a divisor of the display (40 Hz on a
 *     slow feed, 60 Hz panel) drew on frames 1, 2, 1, 2 apart. Fix: count FRAMES (renderEvery).
 *  3. A MISSED SLOT WAS REPAID AS A LURCH. At N = 1 the next frame emitted two rows; at N = 2 the
 *     next row came one frame early. Now at most one row per display frame, no banked credit
 *     (rowsDueThisFrame).
 *  4. THE PAIR WAS SIZED WITH round(), so a gap 0.4 rows longer than the worst seen ran it dry a
 *     frame early (a stall, then fix 3's lurch). Now ceil() + 1 — the surplus repeats are discarded
 *     at the next arrival, and discarding a repeat costs nothing real.
 */

/** Display rates panels actually run at. A measured rate within 5 % of one IS that one. */
const NOMINAL_HZ = [24, 30, 48, 50, 60, 72, 75, 90, 100, 120, 144, 165, 240];

export function nominalRefreshHz(measuredHz: number): number {
  if (!(measuredHz > 0)) return 60;
  let best = measuredHz, bestErr = Infinity;
  for (const hz of NOMINAL_HZ) {
    const err = Math.abs(measuredHz - hz) / hz;
    if (err < bestErr) { bestErr = err; best = hz; }
  }
  return bestErr <= 0.05 ? best : measuredHz;
}

/** Robust display-interval estimate: the MEDIAN of the last few rAF intervals. An EMA let one long
 *  frame (a skipped render, a GC pause, a tab switch) drag the rate estimate for a second or more. */
export class FrameClock {
  private dts: number[] = [];
  private last = 0;
  private static readonly N = 31;
  /** Feed a frame timestamp (prefer the rAF timestamp: vsync-aligned, unlike performance.now()). */
  tick(ts: number) {
    if (this.last > 0) {
      const dt = ts - this.last;
      if (dt > 0 && dt < 250) {
        this.dts.push(dt);
        if (this.dts.length > FrameClock.N) this.dts.shift();
      }
    }
    this.last = ts;
  }
  /** Median interval, ms (16.67 until measured). */
  frameMs(): number {
    if (!this.dts.length) return 1000 / 60;
    const s = this.dts.slice().sort((a, b) => a - b);
    return s[s.length >> 1];
  }
  /** The tick rate snapped to a nominal panel rate. */
  hz(): number { return nominalRefreshHz(1000 / this.frameMs()); }
}

/** Display frames per waterfall row. Ties round UP to the slower cadence (60 Hz / 40 rows/s → 2 →
 *  30 rows/s, as documented since 2026-08-03), and an existing N is kept unless the ideal has moved
 *  clearly past the half-way line, so measurement wobble can never toggle it. */
export function framesPerRowFor(refreshHz: number, rowsPerSec: number, prev: number): number {
  const ideal = refreshHz / Math.max(1, rowsPerSec);
  const cand = Math.max(1, Math.floor(ideal + 0.5 + 1e-9));
  if (prev >= 1 && cand !== prev && Math.abs(ideal - prev) < 0.5 + 0.15) return prev;
  return cand;
}

/** How to draw `deviceRowsPerSec` on a `refreshHz` panel evenly: `per` device rows (all one real row)
 *  every `fpr` frames. Faster than the display → several rows a frame. Slower → one device row every
 *  N frames if that is a whole number, else one SCREEN row (rowPx device rows) every N frames if THAT
 *  is (Default 20/s at dpr 2 on 60 Hz: 2 rows every 3 frames), else one device row, N rounded. */
export function emitShape(refreshHz: number, deviceRowsPerSec: number, rowPx: number): { per: number; fpr: number } {
  const rate = Math.max(1, deviceRowsPerSec);
  if (rate >= refreshHz * 0.95) return { per: Math.max(1, Math.floor(rate / refreshHz + 0.5)), fpr: 1 };
  for (const per of [1, Math.max(1, Math.round(rowPx))]) {
    const x = refreshHz * per / rate;
    if (Math.abs(x - Math.round(x)) < 0.05) return { per, fpr: Math.max(1, Math.round(x)) };
  }
  return { per: 1, fpr: Math.max(1, Math.floor(refreshHz / rate + 0.5 + 1e-9)) };
}

/** Rows to hold for one arrival-to-arrival pair: enough for the WORST recent gap, rounded UP, plus
 *  ONE for the phase of the first slot (it can fall anywhere in the first row interval). The surplus
 *  is repeats of a real row, discarded at the next arrival; on a dead link it is one row more of the
 *  last picture before the scroll stops. */
export function rowsForPair(gapMs: number, slowGapMs: number, rowMs: number): number {
  const clamped = Math.max(20, Math.min(1000, gapMs));
  return Math.max(1, Math.ceil(Math.max(clamped, slowGapMs) / rowMs - 1e-6) + 1);
}

/** Render-loop throttle as a WHOLE NUMBER of display frames: draw every Nth rAF frame. Floor, so the
 *  render rate never falls below what was asked (40 Hz wanted on 60 Hz → every frame; 20 → every
 *  3rd; 60 on 120 Hz → every 2nd). A clock-based gap test either drops real frames to callback
 *  jitter (at renderHz = display rate) or beats against the display (40 Hz on 60 Hz = 1, 2, 1, 2). */
export function renderEvery(displayHz: number, renderHz: number): number {
  return Math.max(1, Math.floor(displayHz / Math.max(1, renderHz) + 1e-9));
}

/** Per display frame: advance the cadence counter and say whether a row is due. Returns the new
 *  counter and 0 or 1 row — NEVER two in one frame (a missed slot is late by a frame, not a lurch).
 *  `available` = rows still unemitted in the current pair. */
export function rowsDueThisFrame(frameTick: number, framesPerRow: number, available: number):
    { frameTick: number; rows: number } {
  let t = frameTick + 1;
  let rows = 0;
  if (available > 0 && t >= framesPerRow) { t -= framesPerRow; rows = 1; }
  // ★ NO BANKED CREDIT. A slot that found no row is simply LATE: the row goes out on the next frame
  //   that has one and the cadence restarts from there (gaps 3, 2, 2 at N = 2). Banking the missed
  //   slot repaid it straight after (3, 1, 2) — a stall AND a lurch, two hitches for one.
  if (t >= framesPerRow) t = framesPerRow - 1;
  return { frameTick: t, rows };
}

/** ★★★ THE WATERFALL'S ROW PACER — the arrival/display-frame state machine waterfall.ts drives
 *  (push → onArrival, tick → onFrame). Pure: no DOM, no clock of its own, so the test can feed it a
 *  simulated display and feed and measure exactly what the screen would show. */
export class RowPacer {
  rowsPerSec = 40;     // DEVICE rows/sec the setting asks for (screen speed × dpr)
  sharp = false;       // SHARP: one row per received frame, no repeats
  private clock = new FrameClock();
  private lastArrival = 0;
  private slowGap = 0;        // worst recent arrival gap (jumps up, decays ×0.98 per arrival)
  private avgGap = 0;         // smoothed arrival gap — the trace tween's span
  private frameTick = 0;
  framesPerRow = 0;           // display frames per EMISSION (0 = re-decide at the next arrival)
  /** Device rows drawn per emission — all the SAME real row. 1, except: SMOOTH on a Retina canvas
   *  (60 screen rows/s = 120 device rows/s, two per frame at 60 Hz), and SHARP, whose one row per
   *  received frame is one SCREEN row (rowPx device rows) — the app's look, one point per frame. */
  rowsPerEmit = 1;
  /** Device pixels per screen row (round(dpr), ≥ 1). */
  rowPx = 1;
  /** Device rows the last onFrame() asked for (0 or rowsPerEmit). */
  rowsNow = 0;
  emitStart = 0;
  emitInterval = 0;
  emitTotal = 0;
  emitted = 0;

  /** The setting changed: re-decide the cadence from scratch (the hysteresis is for wobble only). */
  setRate(rowsPerSec: number, sharp: boolean, rowPx = 1) {
    const px = Math.max(1, Math.round(rowPx));
    if (rowsPerSec !== this.rowsPerSec || sharp !== this.sharp || px !== this.rowPx) this.framesPerRow = 0;
    this.rowsPerSec = rowsPerSec; this.sharp = sharp; this.rowPx = px;
  }

  /** A real row arrived at `now` (ms). Unemitted repeats of the last pair are DROPPED, never flushed. */
  onArrival(now: number) {
    this.emitted = this.emitTotal;
    const gap = this.lastArrival ? now - this.lastArrival : 1000 / this.rowsPerSec;
    this.lastArrival = now;
    this.slowGap = Math.max(gap, this.slowGap * 0.98);
    this.avgGap = this.avgGap ? this.avgGap * 0.85 + gap * 0.15 : gap;
    const refreshHz = this.clock.hz();
    // ★ SHARP paces at the FEED's rate (one real row per arrival, nothing repeated) — but still on a
    //   whole-frame cadence. Drawing each row on the first frame after it lands put every arrival
    //   that jittered across a vsync boundary on screen as 2, 4, 2, 4 frames apart (measured up to
    //   9 hitches/s at 20 fps). On a cadence, a row that just misses its slot goes out one frame late
    //   and the cadence restarts from THERE, so the phase settles where arrivals precede slots.
    // ★ FASTER THAN THE DISPLAY → several device rows per frame. Smooth at dpr 2 is 120 device
    //   rows/s; one row a frame would cap it at 60 (half the app's speed). Rounded, from the nominal
    //   refresh, so it cannot wobble: 120/60 = 2, 60/60 = 1, 120/120 = 1.
    // ★ And a rate that is not a whole number of frames per DEVICE row is drawn a SCREEN row at a time:
    //   Default's 20 rows/s is 40 device rows/s on Retina — 1.5 frames each at 60 Hz, the knife-edge —
    //   so it goes as 2 device rows every 3 frames instead (the app's own one-point step).
    const per = this.sharp ? this.rowPx : emitShape(refreshHz, this.rowsPerSec, this.rowPx).per;
    if (per !== this.rowsPerEmit) { this.rowsPerEmit = per; this.framesPerRow = 0; }
    const target = this.sharp ? 1000 / Math.max(1, this.avgGap) : this.rowsPerSec / per;
    this.framesPerRow = framesPerRowFor(refreshHz, target, this.framesPerRow);
    const rowMs = 1000 / (refreshHz / this.framesPerRow);
    this.emitTotal = this.sharp ? 1 : rowsForPair(gap, this.slowGap, rowMs);
    this.emitInterval = (this.avgGap || Math.max(20, Math.min(1000, gap))) / this.emitTotal;
    this.emitStart = now;
    this.emitted = 0;
  }

  /** One display frame at `frameTs`. Returns 0, or t ∈ (0, 1] for the row to draw now (`rowsNow`
   *  device rows of it): t < 0.5 shows the PREVIOUS real row, otherwise the newest — whole rows,
   *  never a blend. `hasRow` = a row exists. */
  onFrame(frameTs: number, hasRow: boolean): number {
    this.clock.tick(frameTs);
    this.rowsNow = 0;
    if (!hasRow) return 0;
    const fpr = Math.max(1, this.framesPerRow);
    const due = rowsDueThisFrame(this.frameTick, fpr, this.emitTotal - this.emitted);
    this.frameTick = due.frameTick;
    if (!due.rows) return 0;
    this.emitted++;
    this.rowsNow = this.rowsPerEmit;
    // ★ t against the pair WITHOUT rowsForPair's +1 headroom row, so the headroom never changes which
    //   real row is shown (prev/cur) — only how long the pair can last. Surplus rows show the newest.
    const nominal = this.sharp ? 1 : Math.max(1, this.emitTotal - 1);
    return Math.min(1, this.emitted / nominal);
  }
}
