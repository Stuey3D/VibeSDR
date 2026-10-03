/**
 * framePool — the waterfall jitter buffer's frame copies, REUSED instead of allocated per frame.
 *
 * ★★★ WHY (iPhone profile, 2026-10-03, 61 s of heavy use): Hermes' garbage collector ("hades") was 14 % of
 *   VibeSDR's CPU — about a tenth of a core, all the time. The single biggest steady-state allocation was the
 *   jitter buffer copying EVERY frame before drawing it: `bins.slice()` (1024–4096 floats = 4–16 KB) plus a
 *   status spread plus a queue item, 8–20 times a second, all garbage a few hundred ms later.
 * ★★ THE COPY ITSELF STAYS — only the allocation goes. The parent reuses its own bins buffer (the clients fill
 *   one Float32Array in place), so a queued frame MUST hold its own copy; this keeps a small set of slots and
 *   copies into them. A slot is handed back only after handleFrame has finished with it (it reads the frame
 *   synchronously and keeps nothing — WaterfallView pushes each row into its own ring, and the watch reads the
 *   processed row synchronously), so a slot is never rewritten while anything still reads it.
 * ★ Bounded: at most `max` slots are kept for reuse; a burst beyond that falls back to fresh allocation.
 *
 * Pure (no React Native) so scripts/test_frame_pool.ts can hold the rules down.
 */
import type { SDRStatus } from './sdrProtocol';

export interface FrameSlot { bins: Float32Array; status: SDRStatus }

/** Copy every field of `src` into `dst` — and CLEAR trueCenterHz when `src` has none, so a reused slot can
 *  never carry the previous frame's true centre into a frame that did not state one (the watch crops by it). */
export function copyStatusInto(dst: SDRStatus, src: SDRStatus): SDRStatus {
  dst.frequency = src.frequency;
  dst.mode = src.mode;
  dst.bandwidthLow = src.bandwidthLow;
  dst.bandwidthHigh = src.bandwidthHigh;
  dst.binCount = src.binCount;
  dst.binBandwidth = src.binBandwidth;
  dst.centerHz = src.centerHz;
  dst.bwHz = src.bwHz;
  dst.trueCenterHz = src.trueCenterHz;
  return dst;
}

export class FramePool {
  private free: FrameSlot[] = [];
  private readonly max: number;
  constructor(max: number) { this.max = max; }

  /** A slot holding an independent copy of `bins` and `status`. */
  take(bins: Float32Array, status: SDRStatus): FrameSlot {
    let slot = this.free.pop();
    if (!slot) slot = { bins: new Float32Array(bins.length), status: { ...status } };
    else if (slot.bins.length !== bins.length) slot.bins = new Float32Array(bins.length);
    slot.bins.set(bins);
    copyStatusInto(slot.status, status);
    return slot;
  }

  /** Hand a slot back once NOTHING reads it any more. */
  release(slot: FrameSlot): void {
    if (this.free.length < this.max) this.free.push(slot);
  }

  /** Slots waiting for reuse (tests). */
  get size(): number { return this.free.length; }
}
