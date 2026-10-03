/**
 * The waterfall jitter buffer's pooled frame copies — src/services/framePool.ts (2026-10-03, GC churn).
 *
 * Proves:
 *   • a queued frame is an INDEPENDENT copy: the parent refilling its (reused) bins buffer and status object
 *     afterwards changes nothing in the queued frame — the property the waterfall history and the watch (Buddy)
 *     rows depend on;
 *   • a released slot is reused (no new buffer), and resized when the bin count changes;
 *   • a reused slot never carries the previous frame's trueCenterHz into a frame that did not state one;
 *   • the pool is bounded.
 * Run: node --no-warnings scripts/test_frame_pool.ts   (run-tests.sh does)
 */
import { FramePool, copyStatusInto } from '../src/services/framePool.ts';

let passes = 0, fails = 0;
const ok = (what: string, cond: boolean, detail = '') => {
  if (cond) { passes++; return; }
  fails++; console.error(`FAIL ${what}${detail ? `\n   ${detail}` : ''}`);
};
const st = (o: Partial<Record<string, unknown>> = {}) => ({
  frequency: 96_600_000, mode: 'wfm', bandwidthLow: -100_000, bandwidthHigh: 100_000, binCount: 4,
  binBandwidth: 100, centerHz: 96_600_000, bwHz: 400, ...o,
}) as any;

{
  const pool = new FramePool(5);
  const parentBins = new Float32Array([1, 2, 3, 4]);
  const parentStatus = st({ trueCenterHz: 96_601_000 });
  const a = pool.take(parentBins, parentStatus);
  // The parent reuses its buffers for the next frame:
  parentBins.set([9, 9, 9, 9]); parentStatus.centerHz = 1; parentStatus.trueCenterHz = 2;
  ok('queued bins are an independent copy', Array.from(a.bins).join() === '1,2,3,4', Array.from(a.bins).join());
  ok('queued status is an independent copy', a.status.centerHz === 96_600_000 && a.status.trueCenterHz === 96_601_000);
  ok('queued bins are not the parent buffer', a.bins !== parentBins && a.status !== parentStatus);

  pool.release(a);
  const b = pool.take(new Float32Array([5, 6, 7, 8]), st());
  ok('a released slot is reused (same buffer)', b === a && b.bins === a.bins);
  ok('reused slot has the new data', Array.from(b.bins).join() === '5,6,7,8');
  ok('reused slot drops a stale trueCenterHz', b.status.trueCenterHz === undefined);

  pool.release(b);
  const c = pool.take(new Float32Array(8).fill(3), st({ binCount: 8 }));
  ok('a slot is resized when the bin count changes', c.bins.length === 8 && c.bins[7] === 3);

  // Two frames queued at once must not share storage.
  const d = pool.take(new Float32Array([1, 1, 1, 1]), st());
  const e = pool.take(new Float32Array([2, 2, 2, 2]), st());
  ok('two live slots never share a buffer', d.bins !== e.bins && d.bins[0] === 1 && e.bins[0] === 2);
}
{
  const pool = new FramePool(2);
  const slots = [0, 1, 2, 3].map(() => pool.take(new Float32Array(4), st()));
  for (const s of slots) pool.release(s);
  ok('the pool keeps at most `max` slots', pool.size === 2, String(pool.size));
}
{
  const dst = st({ trueCenterHz: 5 }); copyStatusInto(dst, st({ centerHz: 7 }));
  ok('copyStatusInto copies fields and clears trueCenterHz', dst.centerHz === 7 && dst.trueCenterHz === undefined);
}

console.log(`frame pool: ${passes} passed, ${fails} failed`);
if (fails) process.exit(1);
