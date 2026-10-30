/* wfRingGrow.ts — GROW THE APP'S WATERFALL RING WITHOUT LOSING A ROW (the JS path; the Android engine's twin is
 * cpp/spectrum/wf_ring_grow.h). How big it grows to: utils/wfRingSize.ts.
 *
 * ★★★ THE HARD RULE IT KEEPS (WaterfallView, RING_ROWS): "reallocating it on a size change wipes the ring … the waterfall
 *     persisting through a rotation/resize with NO redraw is a hero feature". So a growth is a REMAP, never a restart:
 *     frame f lives in slot f % rows, and every frame the old ring still holds (the newest `oldRows`) is copied from slot
 *     f % oldRows to slot f % newRows — the row's bytes AND everything kept per slot beside it:
 *       • the stored ring and the displayed ring (the zoom fan's, when a zoom has one), byte for byte;
 *       • MOTION: each slot's placement (startHz, hzPerCol) and its move to the next row (h, g) — the image's motion
 *         planes are re-encoded at the new layout (wfRowPlace.ts MOTION_ROWS / motionAt);
 *       • THE FAN: each slot's map (xfA, xfB) and span;
 *       • THE FULL-CAPTURE LAYER (wfWide.ts LayerRing): k, m, the floor row and the peaks.
 *     Everything ABSOLUTE is untouched: the frame counter, the motion range (lo/hi), the fan's validFrom, the stamps'
 *     anchors (wfStamps.ts — frames), tap-to-tune's lookup (displayMap of f % rows — the same row's geometry). The new
 *     slots that hold no frame yet are the floor, exactly what the shader shows for history that was never kept.
 *  ★ ONE STEP, between rows: the caller (WaterfallView) runs it on the JS thread between pushRow calls and publishes the
 *    new image with its row count in the same hand-over, so the shader never sees one ring's rows with another's height.
 *  ★ COST: one pass over the kept rows (n bytes + ~40 B each; the layer's ~0.8 KiB each when it exists) — ~1–5 MB of
 *    copies, once per growth step (RING_GROW_STEP rows), never per row.
 */
import { motionImageRows, newMotionState, setMotion, type FanState, type MotionState } from './wfRowPlace.ts';
import { newLayerRing, type LayerRing } from './wfWide.ts';   // ★ .ts: node runs this file directly (the tests)

/** Copy the newest min(frames, oldRows) frames' `stride`-element slots from `src` (oldRows slots) into `dst` (newRows). */
export function remapSlots<T extends ArrayLike<number> & { set(a: ArrayLike<number>, o?: number): void; subarray(a: number, b: number): T }>(
    src: T, dst: T, oldRows: number, newRows: number, frames: number, stride = 1): void {
  for (let f = Math.max(0, frames - oldRows); f < frames; f++) {
    const o = (f % oldRows) * stride, s = (f % newRows) * stride;
    dst.set(src.subarray(o, o + stride), s);
  }
}

/** A layer ring of `newRows` slots holding `L`'s rows for the newest `frames` frames. */
export function growLayer(L: LayerRing, oldRows: number, newRows: number, frames: number): LayerRing {
  const G = newLayerRing(newRows, L.F, L.C, L.P);
  G.fadeTx = L.fadeTx;
  remapSlots(L.k, G.k, oldRows, newRows, frames);
  remapSlots(L.m, G.m, oldRows, newRows, frames);
  remapSlots(L.floor, G.floor, oldRows, newRows, frames, L.F);
  remapSlots(L.pkN, G.pkN, oldRows, newRows, frames);
  remapSlots(L.pkCell, G.pkCell, oldRows, newRows, frames, L.P);
  remapSlots(L.pkIdx, G.pkIdx, oldRows, newRows, frames, L.P);
  remapSlots(L.pkShape, G.pkShape, oldRows, newRows, frames, L.P);
  return G;
}

/** Grow the ring from `oldRows` to `newRows` slots (n bytes a row) after `frames` rows were written. `ring` is the stored
 *  ring buffer (n × (oldRows + its motion rows)) or null (nothing written yet — only the per-slot state is resized).
 *  The MotionState and FanState are updated IN PLACE (the caller's refs stay valid); returns the new ring buffer
 *  (n × (newRows + motionImageRows(newRows, n))), or null when `ring` was null. No-op (returns `ring`) unless it grows. */
export function growRing(ring: Uint8Array | null, ms: MotionState, fan: FanState, n: number, oldRows: number, newRows: number,
                         frames: number): Uint8Array | null {
  if (!(newRows > oldRows)) return ring;
  const tail = motionImageRows(newRows, n);
  let out: Uint8Array | null = null;
  if (ring) {
    out = new Uint8Array(n * (newRows + tail));
    remapSlots(ring, out, oldRows, newRows, frames, n);
  }
  if (fan.disp) {
    const d = new Uint8Array(n * (newRows + tail));
    remapSlots(fan.disp, d, oldRows, newRows, frames, n);
    fan.disp = d;
  }
  // ── MOTION: the per-slot arrays, then the planes re-encoded at the new layout ──
  const m2 = newMotionState(newRows, n);
  remapSlots(ms.startHz, m2.startHz, oldRows, newRows, frames);
  remapSlots(ms.hzPerCol, m2.hzPerCol, oldRows, newRows, frames);
  remapSlots(ms.h, m2.h, oldRows, newRows, frames);
  remapSlots(ms.g, m2.g, oldRows, newRows, frames);
  for (let f = Math.max(0, frames - oldRows); f < frames; f++) {
    const s = f % newRows;
    setMotion(m2, s, m2.g[s], m2.h[s], n);
  }
  ms.startHz = m2.startHz; ms.hzPerCol = m2.hzPerCol; ms.h = m2.h; ms.g = m2.g; ms.bytes = m2.bytes; ms.rows = newRows;
  // (ms.lo / ms.hi are absolute frames — unchanged.)
  // ── THE FAN: maps + spans; the full-capture layer ──
  const xfA = new Float64Array(newRows).fill(1), xfB = new Float64Array(newRows), span = new Float64Array(newRows);
  remapSlots(fan.xfA, xfA, oldRows, newRows, frames);
  remapSlots(fan.xfB, xfB, oldRows, newRows, frames);
  remapSlots(fan.span, span, oldRows, newRows, frames);
  fan.xfA = xfA; fan.xfB = xfB; fan.span = span;
  if (fan.layer) fan.layer = growLayer(fan.layer, oldRows, newRows, frames);
  return out;
}
