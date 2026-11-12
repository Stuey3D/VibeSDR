/**
 * wideVfo — THE DRAWN WIDTH OF A WHOLE-CHANNEL MODE (app + web; handheld-ui/src/wide_vfo.h is its port for the R36S).
 *
 * ★★★ WHY (Stuart, 2026-11-12, a USRP B210 on a ~7 MHz span with a DAB multiplex on 7D, 194.064 MHz: "this is where we
 *     need an extra wide VFO for things like DAB"). The multiplex filled ~1.5 MHz of the spectrum and the VFO's acrylic
 *     passband sat ~200 kHz wide in the middle of it — the AUDIO filter's width, which says nothing about what DAB is
 *     receiving. A DAB ensemble is one 1.536 MHz block (ETSI EN 300 401, transmission mode I) and the receiver takes all
 *     of it; so the passband drawn is the block.
 * ★★★ DRAWN ONLY, NEVER SENT. Nothing here goes to the server: the DAB pipeline, the IF filter and the listener's own
 *     passband are untouched. The CW skimmer's segment and Band Spots' span are drawn the same way (main.ts renderFrame,
 *     SDRScreen cwDrawnPassband / bandDrawn).
 *
 * THE TABLE — only the modes whose real channel is far wider than an audio filter AND which leave a needle on screen:
 *   dab   ±768 kHz about the block centre — the ensemble (1.536 MHz)
 *   adsb  ±1 MHz about 1090 MHz — the Mode S pulses' main lobe (1 µs PPM at 1 Mbit/s, ~2 MHz); the app can close the
 *         ADS-B window and keep the mode, which leaves the waterfall and this needle on screen
 *   ais   ±37.5 kHz about 162.000 MHz — both channels (161.975 + 162.025 MHz, 25 kHz each), which the server decodes
 *         together from the one dial (vibe_ais.h kCentreHz)
 * And the ones checked and LEFT ALONE, so nobody adds them by reflex:
 *   wfm   ±100 kHz already (spectrum.ts MODE_BANDWIDTHS) — the audio filter IS the channel
 *   drm   ±5 kHz already — the server cuts its own 10 kHz channel (spectrum occupancy 3); a rarer 20 kHz signal is still
 *         decoded through that 10 kHz, so 10 kHz is what is received
 *   ism   no needle at all (needleHidden / wf.vfoHidden) — a band capture, nothing to listen to; LoRa lives inside ISM
 *         (its 125 / 250 / 500 kHz channels are found across the band, not tuned), so it inherits that
 *   ndb   the listener's own dial and filter inside the beacon band — an ordinary passband
 *   hd    none: HD Radio is declined (licensed), there is no digital broadcast mode beside DAB and DRM
 *
 * ★★ A BLOCK THAT FILLS THE VIEW IS NOT WASHED OVER IT. On an RTL the DAB view is the 2.048 MS/s capture, so the block
 *    is ~¾ of the screen; on a view narrower than the block it would be all of it. Past WIDE_VFO_FILL_MAX of the view the
 *    acrylic panels are dropped and the passband is its edges only (any that are on screen) — the waterfall under it is
 *    the thing being looked at. And no frosted backing on a wide passband: the frost dims the waterfall for a narrow
 *    needle's contrast, and across a whole multiplex it would only hide the multiplex.
 *
 * PURE — scripts/test_wide_vfo.ts; handheld-ui/test/gen_wide_vfo_golden.ts holds the C++ port to the same answers.
 */

export type WideVfoMode = 'dab' | 'adsb' | 'ais';

export interface WideVfoEntry {
  /** Half the channel's width, Hz — the drawn passband is centre ± half. */
  half: number;
  /** What the needle's label calls it. */
  name: string;
}

export const WIDE_VFO: Readonly<Record<WideVfoMode, WideVfoEntry>> = {
  dab:  { half: 768_000,   name: 'DAB' },
  adsb: { half: 1_000_000, name: 'ADS-B' },
  ais:  { half: 37_500,    name: 'AIS' },
};

/** Past this fraction of the view the passband is drawn as its edges only (see the header). */
export const WIDE_VFO_FILL_MAX = 0.9;

/** The mode's drawn passband as [low, high] OFFSETS FROM THE DIAL (the shape filterLow / filterHigh take), centred on the
 *  channel's own centre — the block's, for DAB — which is normally the dial, but need not be for a frame or two while the
 *  readout catches up. */
export function wideVfoPassband(mode: WideVfoMode, dialHz: number, centreHz: number = dialHz): [number, number] {
  const h = WIDE_VFO[mode].half;
  const c = centreHz > 0 ? centreHz : dialHz;
  return [c - h - dialHz, c + h - dialHz];
}

/** The needle's label in the mode: "DAB 7D · 194.064M", "ADS-B · 1090.000M", "AIS · 162.000M". */
export function wideVfoLabel(mode: WideVfoMode, centreHz: number, block = ''): string {
  return `${WIDE_VFO[mode].name}${block ? ' ' + block : ''} · ${(centreHz / 1e6).toFixed(3)}M`;
}

/** Whether the acrylic panels are drawn for a passband from loX to hiX on a view `width` wide — false when the visible
 *  part of it covers more than WIDE_VFO_FILL_MAX of the view (then the edges alone are drawn). */
export function wideVfoFill(loX: number, hiX: number, width: number): boolean {
  if (!(width > 0)) return false;
  const vis = Math.min(width, hiX) - Math.max(0, loX);
  return vis < WIDE_VFO_FILL_MAX * width;
}
