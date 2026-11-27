/**
 * adsb.ts — the web client's ADS-B map view: the full-screen map and its live list (V12, docs/v12/ADSB.md).
 *
 * ★★★ THE MAP IS THE MODE (Stuart, 2026-10-24: "when ADS-B is selected the waterfall drops and the full-screen
 *     map is on, an Exit ADS-B button is at the top of the screen, same as our HFDL map exit", with "a live list
 *     in the bottom corner"). It replaced the decoder box + MAP button + popup window: nothing at 1090 MHz is
 *     worth a waterfall (the server slows it to 1 fps), and a map in a second window was one more thing to find.
 * ★★ THE PAGE'S HALF ONLY. Entering and leaving the mode and the locked controls are main.ts's (beside DAB,
 *    whose rules they follow); the aircraft on the map are web/mapkit/adsblayer.js, the one layer the app draws
 *    with too. This file turns a report into the list's rows, and makes and unmakes the map itself.
 * ★ Everything in a report came off the air or over the network: every string is escaped where it meets
 *   HTML, and the callsign is cut to the characters a callsign has.
 */
import type { AdsbAircraft, AdsbReport } from './spectrum';
import { dataUrl } from './origin';


export type LatLon = { lat: number; lon: number };

const D2R = Math.PI / 180;
/** Great-circle distance in nautical miles and initial bearing — the same formulas as adsblayer.js. */
export function rangeBearing(rx: LatLon, lat: number, lon: number): { nm: number; brg: number } {
  const p1 = rx.lat * D2R, p2 = lat * D2R, dp = (lat - rx.lat) * D2R, dl = (lon - rx.lon) * D2R;
  const h = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  const nm = 2 * 6371008.8 * Math.asin(Math.min(1, Math.sqrt(h))) / 1852;
  const y = Math.sin(dl) * Math.cos(p2), x = Math.cos(p1) * Math.sin(p2) - Math.sin(p1) * Math.cos(p2) * Math.cos(dl);
  return { nm, brg: (Math.atan2(y, x) / D2R + 360) % 360 };
}

function esc(v: unknown): string {
  return String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}
function num(v: unknown): number | null { const n = Number(v); return v == null || v === '' || !isFinite(n) ? null : n; }
export function cleanFlight(v: unknown): string { return String(v ?? '').replace(/[^A-Za-z0-9 -]/g, '').trim().slice(0, 10); }
export function cleanHex(v: unknown): string { return String(v ?? '').replace(/[^~0-9A-Fa-f]/g, '').slice(0, 7).toLowerCase(); }

/** Flight levels from the transition altitude, feet below — AircraftPanel.tsx's rule (18 000 ft). */
/** ★ Feet, always — the map's labels say the same (adsblayer.js altText). */
export function altText(alt: AdsbAircraft['alt_baro']): string {
  if (alt === 'ground') return 'GND';
  const ft = num(alt);
  if (ft == null) return '—';
  return `${(Math.round(ft / 100) * 100).toLocaleString('en-GB')} ft`;
}
/** ★★ THE ALTITUDE COLOUR — the map's (adsblayer.js ALT_HUES / altColour, dark): orange-red at the ground fading through
 *  the spectrum to violet at 40 000 ft. ONE RULE, THREE READERS (the layer, this, src/services/adsbList.ts):
 *  scripts/test_adsb_layer.mjs fails if they part. */
export const ALT_HUES: [number, number][] = [[0, 15], [4000, 55], [10000, 120], [20000, 180], [30000, 235], [40000, 285]];
export function altColour(ft: number): string {
  let h = ALT_HUES[ALT_HUES.length - 1][1];
  const f = ft >= 0 ? ft : 0;
  for (let i = 1; i < ALT_HUES.length; i++) {
    const [a0, h0] = ALT_HUES[i - 1], [a1, h1] = ALT_HUES[i];
    if (f <= a1) { h = h0 + (h1 - h0) * (f - a0) / (a1 - a0); break; }
  }
  return `hsl(${h.toFixed(0)},90%,62%)`;
}

/* ★★ MILITARY — the map's rule (adsblayer.js MIL_BLOCKS / MIL_CALLS): an ICAO address in a block a state keeps for its
 *  military (UK 43C000–43CFFF, US AE0000–AFFFFF) or a military operator's callsign. ONE RULE, THREE READERS (the layer,
 *  web/client/src/adsb.ts, src/services/adsbList.ts): scripts/test_adsb_layer.mjs fails if they part. */
export const MIL_BLOCKS: [number, number][] = [[0x43C000, 0x43CFFF], [0xAE0000, 0xAFFFFF]];
export const MIL_CALLS = /^(RRR|ASCOT|TARTN|RCH|NATO|GAF|CTM|IAM|BAF)\d/;
export function isMilitaryHex(hex: string, flight: string | undefined): boolean {
  const h = String(hex || '');
  const n = parseInt(h.replace(/^~/, ''), 16);
  if (h[0] !== '~' && Number.isFinite(n)) for (const [lo, hi] of MIL_BLOCKS) if (n >= lo && n <= hi) return true;
  return MIL_CALLS.test(String(flight || '').trim().toUpperCase());
}
/** ★ What the emergency is, for its badge: the squawk first (7500 / 7600 / 7700), else the declared state; '' when none. */
export function emergencyBadge(squawk: unknown, state: unknown): string {
  const sq = String(squawk ?? '');
  if (sq === '7500') return '7500 HIJACK';
  if (sq === '7600') return '7600 RADIO FAIL';
  if (sq === '7700') return '7700 EMERGENCY';
  const st = String(state ?? '');
  return st && st !== 'none' && st !== '0' ? 'EMERGENCY' : '';
}

export function adsbStatus(r: AdsbReport | null): string {
  if (!r) return 'listening on 1090 MHz…';
  const rate = num(r.msgRate);
  const r1 = rate == null ? '' : rate >= 1000 ? ` · ${(rate / 1000).toFixed(1)}k msg/s` : ` · ${Math.round(rate)} msg/s`;
  /* ★ THE RATE AND THE CAPTURE, ALWAYS (Stuart, 2026-10-24: "there is no capture rate display for ADS-B to check the
   *  2.56MSPS setting") — the sample rate ADS-B runs at and the share of its samples the decoder received. Under
   *  ADSB_CAPTURE_WARN_PCT the red warning beside this line says it instead (adsbCaptureWarning), so not twice. */
  const sr = num(r.rate), cap = num(r.capture);
  const r2 = sr != null && sr > 0 ? ` · ${(sr / 1e6).toFixed(sr % 1e5 === 0 ? 1 : 2)} MS/s` : '';
  const r3 = cap != null && cap >= ADSB_CAPTURE_WARN_PCT ? ` · capture ${Math.round(cap)} %` : '';
  return `${r.aircraft.length} heard${r1}${r2}${r3}`;
}

/** ★ A chip names an aircraft as the list does: its callsign, else its registration (adsb_info), else its address. */
/** ★★ A chip's tap shows the aircraft ON THE MAP — so a chip names only one that is on it (Stuart, 2026-10-25: "I am
 *  clicking the longest tracked and it isnt selecting it"). The longest-followed aircraft is often far out and down to
 *  the odd message: heard, but its last position over 60 s old, so the report carries none and the map cannot select it. */

/** ★★ THE LIST HEADER'S THREE CHIPS (Stuart, 2026-10-25, the top bar crowded on a portrait iPhone 17 Pro Max: "move the
 *  furthest flight chip to just the header of the aircraft list, and with that we can add a newest flight detected,
 *  and furthest tracked chips too"). FURTHEST is adsbFurthest; these are the other two. Tap = show it on the map. */


/** ★★★ THE CAPTURE WARNING (Stuart, 2026-10-24) — the server counts the samples its ADS-B decoder RECEIVES against
 *  what the rate promises (vibe_adsb_capture.h CaptureMeter: the dongle/USB and the server's own IQ queue, not the
 *  decoder's own lag). Under 97 % the receiver is dropping samples, and replies are cut in half — the Pi 2 at 3.2 MS/s
 *  heard a third of what 2.4 did. `short` stays on a phone; `rest` says what it means. Null when nothing is wrong or unknown.
 *  ★★ ONE RULE, TWO READERS: src/services/adsbTypes.ts adsbCaptureWarning is the same — scripts/test_capture_and_rate_guard.ts. */
export const ADSB_CAPTURE_WARN_PCT = 97;
export function adsbCaptureWarning(r: AdsbReport | null): { short: string; rest: string } | null {
  const c = r ? num(r.capture) : null;
  if (c == null || c < 0 || c >= ADSB_CAPTURE_WARN_PCT) return null;
  return { short: `capture ${Math.round(c)} %`, rest: ' — the receiver is dropping samples (its USB bus or this server cannot keep up)' };
}



/** ★ The live list's rows. Each is a button to the highlight (role) and to Tab (tabindex): tapped, clicked
 *  or entered, it shows that aircraft on the map (AdsbMap.focus). */
/* ★★ beta96 (Stuart 11-26: "adsb should drop its 3 bar signal meter for the new colour coded snr"): the 3 bars are gone —
 *  the row shows the server's reply SNR over the 1090 MHz floor (smoothed over the last few replies), coloured by its margin
 *  over ADS-B's decode threshold: src/utils/snrColour.ts snrChipHtml('adsb'), the app's SnrChip the same. */

/** Where this server serves the owner's logos ("…/adsb/"), '' where the page has no http(s) origin. Machine-wide, like
 *  /mapgl/: the front door answers it too. */
export function adsbImageBase(): string {
  if (typeof location === 'undefined' || !/^https?:$/.test(location.protocol)) return '';
  /* ★★★ THIS RADIO'S PATH, NOT THE FRONT DOOR'S (Stuart, 2026-10-25: "when i select other aircraft their trails are not
   *  being drawn in … this radio is set for background adsb so should have built up a decent history but nope"). On a
   *  server with a front door the page is /r/<id>/…; "/adsb/trail/<hex>" asked of the door got the door's HTML, the JSON
   *  parse failed, the catch fell back to the map's own trail — so only what this map had seen since it opened, never the
   *  server's. The door forwards /r/<id>/adsb/… to the radio, which keeps the trails (and the owner's logos). */
  const m = /^\/r\/[^/?#]+/.exec(location.pathname);
  return dataUrl(location.origin + (m ? m[0] : '') + '/adsb/');   // ★ beta93: direct when the page was proxied (origin.ts)
}


/* ══ THE MAP ══════════════════════════════════════════════════════════════════════════════════════
 * ★★★ beta96 RETIRED: ADS-B mode's map is the AIR & SEA map's ADS-B-live state (docs/v12/ONE-MAP.md stage 4). The view
 *     (TrackView, tracks.ts) makes the GPU or Leaflet map and the shared aircraft layer (adsblayer.js), reads this mode's
 *     old toggles (vibesdr.adsbMapPrefs) until it has its own, and is made on entry and destroyed on exit, as AdsbMap was. */
