// digiModesMap.ts — V12 beta90 THE DIGI MODES MAP's door in the app (docs/v12/DIGI-MODES-MAP.md) — the web's
// web/client/src/digiMap.ts openDigiModesMap, the same options.
//
// ★★★ ONE OPENER (Stuart, 2026-11-21): VIEWS' DIGI MODES, the APRS box's MAP and the SATELLITE client (stations heard through
//     ISS / SONATE-2, with the pass's orbit track and footprint as an `overlay`) all open the same map. SDRScreen registers
//     the map (setDigiModesMapOpener); anything else imports this module and calls openDigiModesMap — no SDRScreen import.
import type { DigiMapOverlay } from '../utils/digiMapShared';

/** ★ beta96 the AMATEUR map's layers (amateurMap.ts AmLayer) — the tab it opens on; 'aprs' also switches the APRS stations on. */
export type DigiMapLayer = 'spots' | 'aprs' | 'mesh' | 'beacons';
export interface DigiMapOpts {
  /** The layer it opens on — 'aprs' shows the APRS box's stations (and centres on `focus`). Default 'spots'. */
  layer?: DigiMapLayer;
  /** An APRS station key or callsign to centre on. */
  focus?: string;
  /** The window ('1h' | '24h' | '7d' | '30d' | 'all') — the map's age filter starts on it where the server keeps stations. */
  window?: string;
  /** A caller's extra layer (digiMapShared.ts DigiMapOverlay: lines, circles, points) — null clears it. */
  overlay?: DigiMapOverlay | null;
}
let opener: ((o: DigiMapOpts) => void) | null = null;
export function setDigiModesMapOpener(fn: ((o: DigiMapOpts) => void) | null): void { opener = fn; }
export function openDigiModesMap(o: DigiMapOpts = {}): void {
  if (opener) opener(o);
  else console.error('[digimap] openDigiModesMap called with no map registered (not on a receiver screen)');
}
