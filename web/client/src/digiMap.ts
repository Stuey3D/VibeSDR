/**
 * digiMap.ts — V12 beta90 THE DIGI MODES MAP's door (docs/v12/DIGI-MODES-MAP.md).
 *
 * ★★★ ONE OPENER (Stuart, 2026-11-21): the spots map became the DIGI MODES MAP — the merged digital spots (this server's
 *     station store: background decoding and every listener, plus this page's own live spots) and APRS ("aprs map gets
 *     combined with the rest of the digispots"), each a LAYER. Everything that opens it calls openDigiModesMap():
 *     VIEWS' DIGI MODES key, the spots box's MAP, the APRS box's MAP and a station row (layer 'aprs'), the gallery's
 *     spots rows — and the SATELLITE client (stations heard through ISS / SONATE-2, with the pass's orbit and footprint
 *     as an `overlay`).
 * ★ This module holds only the door and the types, so satellites.ts (or anything else) can import it without importing
 *   main.ts; main.ts registers the map itself (setDigiModesMapOpener) at boot.
 */

import type { DigiMapOverlay } from '../../../src/utils/digiMapShared';

/** ★★★ beta96 stage 7 THE AMATEUR MAP's layers (src/utils/amateurMap.ts): + MESH (MeshCore / Meshtastic) and BEACONS (NCDXF). */
export type DigiMapLayer = 'spots' | 'aprs' | 'mesh' | 'beacons';

export interface DigiMapOpts {
  /** The layer it opens on (shown, fitted, first). Default 'spots'. */
  layer?: DigiMapLayer;
  /** A callsign / APRS station key to centre on and open. */
  focus?: string;
  /** The time window it opens on ('1h' | '24h' | '7d' | '30d' | 'all'); default the last one used, else 24 h. */
  window?: string;
  overlay?: DigiMapOverlay | null;
}

let opener: ((o: DigiMapOpts) => void) | null = null;
/** main.ts: the map itself. */
export function setDigiModesMapOpener(fn: (o: DigiMapOpts) => void): void { opener = fn; }
/** ★ THE ONE OPENER. Must be called inside a click (it opens a window — a browser blocks one opened later). */
export function openDigiModesMap(o: DigiMapOpts = {}): void {
  if (opener) opener(o);
  else console.error('[digimap] openDigiModesMap called before the page registered the map');
}

export { DIGI_WINDOWS, digiWindowOf, digiOverlayClean, digiSvgOk, digiAprsPoints, type DigiAprsPoint, type DigiMapOverlay } from '../../../src/utils/digiMapShared';
