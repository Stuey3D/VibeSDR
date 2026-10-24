// adsbList.ts — the ADS-B map's LIVE LIST (V12, docs/v12/ADSB.md "Client behaviour"): what each row says, how many
// rows fit, how wide the list may be. The component (src/components/AdsbLiveList.tsx) only draws what this decides.
//
// ★★★ WHY A LIST ON THE MAP AT ALL. Stuart, 2026-10-24: "when ADS-B is selected the waterfall drops and the full-screen map is on … a live list in the
// bottom corner". The box-and-MAP-button version made the table the screen and the map an extra; now the map IS the
// screen, and the list is the bit of the table you glance at — nearest first, a few rows, tap one to find it.
// ★★ PURE: no React Native — scripts/test_adsb_list.ts runs it in node.
import type { Aircraft } from './SDRBackend';

const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
/** ★ km → nautical miles: aviation's unit, and the range rings on the same map are drawn in NM. */
export const KM_PER_NM = 1.852;

export interface AdsbListRow {
  hex: string;
  /** Callsign, else the ICAO address in capitals — a row is never nameless (AircraftPanel's rule). */
  label: string;
  /** FL350 from 18 000 ft (the transition altitude AircraftPanel and the layer's labels use), else "4,300 ft"; GND. */
  alt: string;
  /** "450 kt", or "" when unknown. */
  kt: string;
  /** "12 NM NE", or "" without a position (or without the receiver's). */
  range: string;
  /** Has a position — only then can a tap find it on the map (the layer's focus() needs one). */
  placed: boolean;
  emergency: boolean;
}

export function altText(a: Aircraft): string {
  if (a.onGround) return 'GND';
  const ft = a.altitude;
  if (ft == null || !Number.isFinite(ft)) return '';
  return ft >= 18_000
    ? `FL${String(Math.round(ft / 100)).padStart(3, '0')}`
    : `${(Math.round(ft / 100) * 100).toLocaleString('en-GB')} ft`;
}

export function rangeText(a: Aircraft): string {
  if (a.distKm == null || !Number.isFinite(a.distKm)) return '';
  const nm = a.distKm / KM_PER_NM;
  const n = nm < 10 ? nm.toFixed(1) : String(Math.round(nm));
  const dir = a.bearing != null && Number.isFinite(a.bearing) ? ' ' + COMPASS[Math.round(((a.bearing % 360) + 360) % 360 / 45) % 8] : '';
  return `${n} NM${dir}`;
}

/** ★ NEAREST FIRST — the same order as AircraftPanel and the watch (adsbToAircraft already sorts; sorted again here
 *  so the list never depends on its caller): placed by distance, then the unplaced by signal, then by address so two
 *  equal rows never swap places between reports (a list that shuffles under the finger is a list you cannot tap). */
export function adsbListRows(list: Aircraft[]): AdsbListRow[] {
  const sorted = [...list].sort((x, y) => {
    if (x.distKm != null && y.distKm != null && x.distKm !== y.distKm) return x.distKm - y.distKm;
    if (x.distKm != null && y.distKm == null) return -1;
    if (y.distKm != null && x.distKm == null) return 1;
    if (x.distKm == null && (x.rssi ?? -99) !== (y.rssi ?? -99)) return (y.rssi ?? -99) - (x.rssi ?? -99);
    return x.icao < y.icao ? -1 : x.icao > y.icao ? 1 : 0;
  });
  return sorted.map((a) => ({
    hex: a.icao,
    label: a.flight?.trim() || a.icao.toUpperCase(),
    alt: altText(a),
    kt: a.speed != null && Number.isFinite(a.speed) ? `${Math.round(a.speed)} kt` : '',
    range: rangeText(a),
    placed: a.lat != null && a.lon != null,
    emergency: !!a.emergency,
  }));
}

/* ── geometry ──────────────────────────────────────────────────────────────────────────────────────────────── */
/** One row's height and the header's, pt — the list sizes from these (the component uses the same numbers). */
export const LIST_ROW_H = 24;
export const LIST_HEAD_H = 30;
/** ★ The map page's own top bar (#topbar in MapOverlay's page: 8 + 32 + 6, below the safe area). */
export const MAP_TOPBAR_H = 46;
/** ★★ What the list must leave free on the RIGHT of the bottom edge: the map's +/− stack (MapLibre 29 px, Leaflet 34)
 *  and the attribution ⓘ under it, with their 10 px margin and a gap. */
export const RIGHT_KEEP = 60;
export const EDGE = 10;

/** ★ How many rows: ~6–8 where there is room, never more than HALF the map's height (the map is the screen — the
 *  list is a glance), never fewer than 3 (a phone held sideways still gets a useful list; it scrolls). */
export function adsbListRowsFit(areaH: number): number {
  const mapH = Math.max(0, areaH - MAP_TOPBAR_H);
  const n = Math.floor((mapH * 0.5 - LIST_HEAD_H) / LIST_ROW_H);
  return Math.max(3, Math.min(8, n));
}

/** ★ How wide: up to 300 pt, and always short of the zoom stack on the right — at the narrowest layout the app has
 *  (an SE in Display Zoom, 320 pt) that is 250 pt, which still holds a callsign, FL, kt and NM + bearing. */
export function adsbListWidth(areaW: number): number {
  return Math.max(160, Math.min(300, Math.round(areaW - EDGE * 2 - RIGHT_KEEP)));
}

/** ★ The collapse state, as it is kept per device ('1' open, '0' a pill). Open unless somebody shut it. */
export const parseListOpen = (v: string | null | undefined): boolean => v !== '0';
