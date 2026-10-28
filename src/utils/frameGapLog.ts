/**
 * frameGapLog.ts — a DEV-ONLY late-waterfall-row log, OFF by default (Stuart, 2026-10-28, Mac build 501 over the public
 * tunnel at 20 fps: "it scrolls smoothly mostly but every ~15 s or so there is a late frame"; the server measured clean on
 * the LAN — 1200 frames in 60 s, max gap 105 ms).
 *
 * It answers ONE question about each late row: did the JS timer that draws rows fire late (the JS THREAD was blocked —
 * something in the app), or fire on time with nothing to draw (the row had not ARRIVED — the network / tunnel)? And when it is the JS thread, WHAT ran just
 * before: periodic app work calls jsMark(label, ms) around itself, and the log prints the marks in the window before
 * the late row.
 *
 * ▶ Turn on: set WF_GAP_LOG = true (or `globalThis.__vibeWfGapLog = true` from a debugger). Output: console.log lines
 *   starting "[wf-gap]", which show in Metro / Xcode's console / `log stream --process VibeSDR`.
 * ★ Off, every call is one boolean test. On, it is a few number writes per row and a short string per LATE row only.
 * PURE apart from the console line, so scripts/test_frame_gap_log.ts can drive it with a fake clock.
 */

export const WF_GAP_LOG = false;

const on = (): boolean => WF_GAP_LOG || (globalThis as { __vibeWfGapLog?: boolean }).__vibeWfGapLog === true;

/** A row is LATE when it comes this much after the previous one beyond the expected cadence. */
export const GAP_LATE_MS = 40;
/** How far back the marks are listed for a late row. */
export const GAP_LOOKBACK_MS = 400;

const MARKS = 32;
const markT = new Float64Array(MARKS);
const markMs = new Float64Array(MARKS);
const markL: string[] = new Array(MARKS).fill('');
let markN = 0;

let clock: () => number = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());
let sink: (line: string) => void = (l) => console.log(l);
/** Tests only: a fake clock and a captured sink. */
export function _setGapLogIo(c: () => number, s: (line: string) => void): void { clock = c; sink = s; }

/** Note that `label` ran (and took `ms`, if known) — call it at the END of a periodic job. */
export function jsMark(label: string, ms = 0): void {
  if (!on()) return;
  const i = markN++ % MARKS;
  markT[i] = clock(); markMs[i] = ms; markL[i] = label;
}

/** Time `fn` and mark it. Off: just calls fn. */
export function jsTimed<T>(label: string, fn: () => T): T {
  if (!on()) return fn();
  const t0 = clock();
  try { return fn(); } finally { jsMark(label, clock() - t0); }
}

/** The marks within GAP_LOOKBACK_MS before `now`, oldest first, "label 12ms @-80". */
export function marksBefore(now: number): string[] {
  const out: string[] = [];
  const n = Math.min(markN, MARKS);
  for (let k = n; k >= 1; k--) {
    const i = (markN - k) % MARKS;
    const age = now - markT[i];
    if (age >= 0 && age <= GAP_LOOKBACK_MS) out.push(`${markL[i]}${markMs[i] >= 1 ? ` ${markMs[i].toFixed(0)}ms` : ''} @-${age.toFixed(0)}`);
  }
  return out;
}

let lastArr = 0, lastDraw = 0, arrGap = 0, rows = 0, late = 0, startedAt = 0, lastGcN = -1, lastGcMs = 0;

/** Hermes's own GC counters (count, total ms), read per row ONLY while the log is on — a GC between two rows is then
 *  named on the late row rather than guessed at. null when not Hermes (node tests, JSC). */
let gcStats: () => { n: number; ms: number } | null = () => {
  const h = (globalThis as { HermesInternal?: { getInstrumentedStats?: () => Record<string, number> } }).HermesInternal;
  const st = h?.getInstrumentedStats?.();
  return st && typeof st.js_numGCs === 'number' ? { n: st.js_numGCs, ms: st.js_gcTime ?? 0 } : null;
};
/** Tests only. */
export function _setGcStats(f: () => { n: number; ms: number } | null): void { gcStats = f; }

/** Is the log on? (Callers skip their own bookkeeping when not.) */
export const gapLogOn = on;

/** A frame ARRIVED from the socket (WaterfallView enqueueFrame). */
export function gapArrival(): void {
  if (!on()) return;
  const t = clock();
  arrGap = lastArr > 0 ? t - lastArr : 0;
  lastArr = t;
}

/** A frame was DRAWN (a row pushed). `expectMs` = the cadence it should have come at; `timerLateMs` = how late the
 *  JS timer that drew it fired against its due time (0 for a draw made straight from an arrival). Returns the verdict
 *  for tests: '' on time, 'js' (the timer itself fired late: the JS thread was busy), 'net' (the timer was on time but
 *  had nothing to draw: the row had not arrived). */
export function gapDraw(expectMs: number, timerLateMs: number): '' | 'net' | 'js' {
  if (!on()) return '';
  const t = clock();
  if (!startedAt) startedAt = t;
  rows++;
  const gap = lastDraw > 0 ? t - lastDraw : 0;
  lastDraw = t;
  const gc = gcStats();
  const gcN = gc ? gc.n - lastGcN : 0, gcMs = gc ? gc.ms - lastGcMs : 0;
  const gcKnown = !!gc && lastGcN >= 0;
  if (gc) { lastGcN = gc.n; lastGcMs = gc.ms; }
  if (!(gap > expectMs + GAP_LATE_MS)) return '';
  late++;
  const verdict: 'net' | 'js' = timerLateMs > GAP_LATE_MS / 2 ? 'js' : 'net';
  const marks = marksBefore(t);
  sink(`[wf-gap] row late by ${(gap - expectMs).toFixed(0)} ms (gap ${gap.toFixed(0)} vs ${expectMs.toFixed(0)}) — ` +
       (verdict === 'js' ? `JS THREAD: the draw timer fired ${timerLateMs.toFixed(0)} ms late`
                         : `NETWORK: draw timer on time, row not there (last arrival gap ${arrGap.toFixed(0)} ms)`) +
       (gcKnown ? (gcN > 0 ? ` · GC ×${gcN} (${gcMs.toFixed(0)} ms) since the last row` : ' · no GC') : '') +
       ` · ${late}/${rows} late in ${((t - startedAt) / 1000).toFixed(0)} s` +
       (marks.length ? ` · before it: ${marks.join(', ')}` : ' · no marks'));
  return verdict;
}

/** Tests only. */
export function _resetGapLog(): void { lastArr = lastDraw = arrGap = rows = late = startedAt = 0; lastGcN = -1; lastGcMs = 0; markN = 0; }

/** The UI-thread row pacer slipped (WaterfallView rowPace, JS path): a row shown a frame early/late on purpose. */
export function gapPacerSlip(lead: number, target: number, rate: number): void {
  if (!on()) return;
  sink(`[wf-gap] PACER SLIP (UI thread): lead ${lead.toFixed(2)} rows vs target ${target.toFixed(2)} at ${(rate * 1000).toFixed(1)} rows/s — a row shown a frame early/late to hold the playout buffer`);
}
