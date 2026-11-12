/* needleGeometry.ts — where WaterfallView draws the VFO needle and its passband (the acrylic sidebands, the edge strips,
 * the glow, the LISTEN label). ★ Every renderer path reads THIS one result — the iOS needle canvas, the Android in-waterfall
 * group, the label — so `hidden` removes all of them at once.
 * ★★ V12 ISM HIDES IT (the web's wf.vfoHidden in ismUiOn): on a band capture there is nothing to listen to and no dial to
 *    tune, so a LISTEN needle in the middle of the band only says something untrue.
 * ★★ V12 A WIDE PASSBAND (`wide` — DAB's 1.536 MHz block, wideVfo.ts): `fill` false once it covers most of the view (the
 *    acrylic panels dropped, its edges alone drawn) and `frost` false always (the frosted backing would only dim the
 *    multiplex). Both true for every ordinary passband — exactly as before.
 * PURE — scripts/test_ism_app.ts and scripts/test_wide_vfo.ts run it in node. */
import { wideVfoFill } from './wideVfo.ts';

export interface NeedleGeometry {
  nX: number; loXc: number; hiXc: number; loX: number; hiX: number; scale: number; scaleQ: number;
  /** Draw the acrylic panels (false: a wide passband covering most of the view — edges only). */
  fill: boolean;
  /** Draw the frosted backing (false on a wide passband). */
  frost: boolean;
}

export function needleGeometry(o: {
  hidden?: boolean; bwHz: number; tuneHz: number; filterLow: number; filterHigh: number;
  width: number; pxPerHz: number; hzToX: (hz: number) => number;
  /** The passband is a whole channel (wideVfo.ts), not an audio filter. */
  wide?: boolean;
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
  const fill = !o.wide || wideVfoFill(loX, hiX, o.width);
  return { nX, loXc: Math.max(0, loX), hiXc: Math.min(o.width, hiX), loX, hiX, scale, scaleQ, fill, frost: !o.wide };
}
