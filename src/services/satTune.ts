/* ★★★ beta97 TUNING INSIDE A SATELLITE PASS — one set of rules for the web page (main.ts) and the app (SDRScreen).
 *
 *  Stuart, beta96, RS-44's transponder in SATELLITE with AUTO DOPPLER on: "can't manually move the VFO in a linear
 *  transponder window; I wanted to see if a signal was RTTY or something." The Doppler lock was never the wall — the
 *  server's Engine::nudge (vibe_sat_engine.h) already re-bases the lock on any tune inside the passband. Three things in
 *  front of it were:
 *
 *   1. THE STEP. It was still 100 kHz from WFM, so every waterfall tap snapped to a 100 kHz grid point — past the
 *      signal, usually out of the 60 kHz passband, and that ENDED the lock. SatStepKeeper: the satellite view sets a
 *      500 Hz step for every satellite (Stuart: "may as well switch ALL satellites to 500 Hz"), puts the listener's own
 *      step back on the way out, and leaves a step they choose meanwhile alone for the rest of that visit.
 *   2. THE FRAME. While the lock is live the DIAL is the satellite-frame frequency (the server's nominal) and the channel
 *      sits at dial + the lock's shift; the waterfall is the real received spectrum (the marker is drawn at dial + shift).
 *      A tap sent the frequency under the finger as the DIAL, so the channel landed a whole Doppler shift away (up to
 *      ±10 kHz at 70 cm) from the signal tapped. satClickDialHz takes the shift off first.
 *   3. THE EDGE. A tap inside the passband never snaps out of it (the snap is clamped to the passband) — a tap outside
 *      it is the listener's own choice and goes where they asked (the server ends the lock: a tune outside the passband).
 *  (The server's half — a demodulator change KEEPS the lock — is in local_sdr_shim.cpp.) */
import { snapToStep } from '../utils/airband';

/** The step the satellite view tunes in (Hz). */
export const SAT_STEP_HZ = 500;

/** The listener's own step, held while the satellite view has the radio. Pure: the caller applies what it returns. */
export class SatStepKeeper {
  private saved: number | null = null;
  /** In the satellite view (or it is in already). Returns the step to set now, or null to leave the step alone. */
  enter(curStep: number): number | null {
    if (this.saved != null) return null;          // ★ already in: a step chosen since then stands
    this.saved = curStep;
    return curStep === SAT_STEP_HZ ? null : SAT_STEP_HZ;
  }
  /** Out of the satellite view. Returns the listener's own step to put back, or null when there is nothing to do. */
  leave(curStep: number): number | null {
    const s = this.saved;
    this.saved = null;
    return s != null && s > 0 && s !== curStep ? s : null;
  }
  /** The step before the satellite view took over, while it has the radio; null outside it. */
  get held(): number | null { return this.saved; }
}

export interface SatPassband { lo: number; hi: number }

/** The picked downlink's passband in the satellite frame (a linear transponder: hz…hiHz), else null. */
export function satPassbandOf(tx: { hz: number; hiHz?: number } | null | undefined): SatPassband | null {
  return tx && tx.hiHz != null && tx.hiHz > tx.hz ? { lo: tx.hz, hi: tx.hiHz } : null;
}

/**
 * Where a tap on the waterfall at `rawRfHz` puts the DIAL.
 *  `markerShiftHz` — the lock's shift less the finder's follow (satMarkerShiftHz: the marker is drawn at dial + this);
 *  0 with no live lock, and the answer is the ordinary snapped tap.
 *  `snap` — the listener's SNAP WATERFALL TUNING TO STEP switch.
 *  `passband` — the picked transponder's (satPassbandOf), or null.
 */
export function satClickDialHz(rawRfHz: number, markerShiftHz: number, step: number, snap: boolean,
                               passband: SatPassband | null = null): number {
  const raw = rawRfHz - (Number.isFinite(markerShiftHz) ? markerShiftHz : 0);
  let hz = snap ? snapToStep(raw, step) : Math.round(raw);
  if (passband && raw >= passband.lo && raw <= passband.hi)
    hz = Math.max(Math.ceil(passband.lo), Math.min(Math.floor(passband.hi), hz));   // ★ a tap inside never snaps out
  return hz;
}

/* ★★★ beta97 THE DIAL BESIDE THE SATELLITE BOX, NOT UNDER IT (Stuart, desktop, JO-97 on the XCover: the SATELLITE card
 *  covers the left ~half of the waterfall and the dial line and the passband sat under or at its edge). On a wide layout,
 *  while the box is open (not minimised), the view is centred so the channel's line sits in the MIDDLE OF THE UNCOVERED
 *  AREA to the box's right, and kept there as the Doppler moves the channel (the spectrum scrolls under it). Phone /
 *  narrow layouts (the box covers differently there) and a minimised box: the ordinary centring. */
export const SAT_ANCHOR_MIN_VIEW_PX = 900;     ///< narrower than this = a phone / narrow layout: leave the view alone
export const SAT_ANCHOR_MAX_COVER = 0.7;       ///< a box covering more than this share of the view: nothing sensible to do
export const SAT_ANCHOR_TOL_PX = 8;            ///< the line may wander this far before the view is moved again
/** The x (px from the view's left) the line belongs at: the middle of what the box leaves uncovered — null = no anchor. */
export function satAnchorX(viewW: number, coverRightPx: number): number | null {
  if (!(viewW >= SAT_ANCHOR_MIN_VIEW_PX) || !(coverRightPx > 0) || coverRightPx > viewW * SAT_ANCHOR_MAX_COVER) return null;
  return (coverRightPx + viewW) / 2;
}
/** The view centre (Hz) that puts `lineHz` at x = `anchorX` in a view `spanHz` wide and `viewW` px across. */
export function satAnchorCentreHz(lineHz: number, spanHz: number, viewW: number, anchorX: number): number {
  return Math.round(lineHz - (anchorX - viewW / 2) * (spanHz / viewW));
}
/** Whether the view must move: the line is more than SAT_ANCHOR_TOL_PX from where it belongs. */
export function satAnchorStale(curCentreHz: number, wantCentreHz: number, spanHz: number, viewW: number): boolean {
  return spanHz > 0 && viewW > 0 && Math.abs(curCentreHz - wantCentreHz) * (viewW / spanHz) > SAT_ANCHOR_TOL_PX;
}

/* ★★★ beta97 SPOTS VIA A SATELLITE (vibe_decoder_host.h setSatVia): a digital spot decoded on a channel with a live Doppler
 *  lock carries "via" (the bird's catalogue id, "JO-97") and "norad". The band column shows the BIRD instead of the
 *  downlink's band ("2m" would claim a terrestrial reception), and the detail line says "via JO-97". The server holds these
 *  spots back from PSK Reporter. */
/** The bird a spot was heard through, off the wire: printable, short, '' = none (an ordinary spot or an older server). */
export function parseSpotVia(v: unknown): string {
  return typeof v === 'string' ? v.replace(/[^0-9A-Za-z ()/+-]/g, '').trim().slice(0, 24) : '';
}
/** "via JO-97", or '' for an ordinary spot. */
export function spotViaText(via: string | undefined | null): string {
  return via ? `via ${via}` : '';
}
/** What a spot row's band column shows: the bird for a spot via a satellite, else the band. */
export function spotBandCell(band: string, via: string | undefined | null): string {
  return via ? via : band;
}

