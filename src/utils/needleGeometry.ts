/* needleGeometry.ts — where WaterfallView draws the VFO needle and its passband (the acrylic sidebands, the edge strips,
 * the glow, the LISTEN label). ★ Every renderer path reads THIS one result — the iOS needle canvas, the Android in-waterfall
 * group, the label — so `hidden` removes all of them at once.
 * ★★ V12 ISM HIDES IT (the web's wf.vfoHidden in ismUiOn): on a band capture there is nothing to listen to and no dial to
 *    tune, so a LISTEN needle in the middle of the band only says something untrue.
 * PURE — scripts/test_ism_app.ts runs it in node. */
export interface NeedleGeometry { nX: number; loXc: number; hiXc: number; loX: number; hiX: number; scale: number; scaleQ: number }

export function needleGeometry(o: {
  hidden?: boolean; bwHz: number; tuneHz: number; filterLow: number; filterHigh: number;
  width: number; pxPerHz: number; hzToX: (hz: number) => number;
}): NeedleGeometry | null {
  if (o.hidden || !(o.bwHz > 0) || !(o.tuneHz > 0)) return null;
  const nX = o.hzToX(o.tuneHz);
  let loX = o.hzToX(o.tuneHz + o.filterLow);
  let hiX = o.hzToX(o.tuneHz + o.filterHigh);
  const minSbPx = o.filterLow === 0 && o.filterHigh === 0 ? 20 : 4;
  if (nX - loX < minSbPx) loX = nX - minSbPx;
  if (hiX - nX < minSbPx) hiX = nX + minSbPx;
  const scale = Math.max(0.25, Math.min(1.0, o.pxPerHz * 4000));
  // Quantised scale (0.05 steps) so the pre-rendered glow strips are not re-blurred on every zoom tick.
  const scaleQ = Math.round(scale * 20) / 20;
  return { nX, loXc: Math.max(0, loX), hiXc: Math.min(o.width, hiX), loX, hiX, scale, scaleQ };
}
