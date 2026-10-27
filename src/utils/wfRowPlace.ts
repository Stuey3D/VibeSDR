/* wfRowPlace.ts — where WaterfallView puts one spectrum row in its history ring (moved out of the component, unchanged, so
 * it can be tested in node: scripts/test_wf_row_place.ts — GitHub #57).
 *
 * ★★★ THE HARD RULE (Stuart, 2026-10-27, verbatim): "the waterfall is what I always wanted to make sure was perfect, it was
 *     a fully buffered waterfall which when rotated or resized never lost any content it expanded or shrunk as needed.
 *     Zooming and tuning also never lost the history and it was seemless to the history too so signals always glided in
 *     from the sides and settled and when zooming in and out you had a cool fanning in and out of the signals. Those 2
 *     things were the major things that we nailed in our first native build and I made a hard rule that these must be
 *     preserved at all costs".
 *  ★★ HOW THAT LOOK IS MADE: every row is written ONCE, placed against the view at the moment it lands, and the stored
 *     history is never rewritten — so a station's trail bends while the dial moves ("diagonal when tuning then straight
 *     again with the signals in the new spot") and fans on a zoom. Tearing comes from a row whose bins do not belong to
 *     the centre it is placed by; that was the server (#57, local_sdr_shim.cpp onSpectrum), and this placement is what
 *     turns a correct row into a continuous trail. Do not "re-align" the history on a tune: that is a different picture
 *     (every trail vertical), and absolute placement was built and reverted once already (BRIEF-waterfall-frame-alignment
 *     §9 — it restarted the ring on every zoom).
 * PURE. */

/** Bins to slide a row by so it lands under the CURRENT view: the gap between the row's own centre and the view's, in the
 *  row's bins. 0 when it cannot be placed honestly — unknown centres, a different bin scale (a frame from another span
 *  would draw the right signal at the wrong width), or no overlap at all. */
export function rowViewOffset(rowCenterHz: number, rowHzPerBin: number, viewCenterHz: number, viewBwHz: number, n: number): number {
  if (!(rowHzPerBin > 0 && rowCenterHz > 0 && viewCenterHz > 0 && viewBwHz > 0 && n > 0)) return 0;
  const viewHzPerBin = viewBwHz / n;
  if (Math.abs(rowHzPerBin - viewHzPerBin) > viewHzPerBin * 1e-6) return 0;
  const off = Math.round((rowCenterHz - viewCenterHz) / rowHzPerBin);
  return Math.abs(off) >= n ? 0 : off;
}

/** Write `row` (n bins) into `ring` at `slot`, slid by `off` bins; the vacated edge is the floor (0) — the "slight gap"
 *  at a newly exposed edge, never a smear of the edge bin. */
export function writeRow(ring: Uint8Array, slot: number, row: Uint8Array, n: number, off: number): void {
  if (off === 0) { ring.set(row.subarray(0, n), slot); return; }
  ring.fill(0, slot, slot + n);
  if (off > 0) ring.set(row.subarray(0, n - off), slot + off);
  else         ring.set(row.subarray(-off, n), slot);
}
