// adsbList.ts — the ADS-B map's LIVE LIST (V12, docs/v12/ADSB.md "Client behaviour"): what each row says, how many
// rows fit, how wide the list may be. The component (src/components/AdsbLiveList.tsx) only draws what this decides.
//
// ★★★ WHY A LIST ON THE MAP AT ALL. Stuart, 2026-10-24: "when ADS-B is selected the waterfall drops and the full-screen map is on … a live list in the
// bottom corner". The box-and-MAP-button version made the table the screen and the map an extra; now the map IS the
// screen, and the list is the bit of the table you glance at — nearest first, a few rows, tap one to find it.
// ★★ PURE: no React Native — scripts/test_adsb_list.ts runs it in node.
import type { Aircraft } from './SDRBackend';
import { flagEmoji } from './adsbInfo.ts';

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
  /** The emergency's badge ("7700 EMERGENCY", "7600 RADIO FAIL", "7500 HIJACK", "EMERGENCY"), '' when none. */
  badge: string;
  military: boolean;
  /** ★ beta96 the reply SNR over the 1090 MHz floor, dB (the server's, smoothed over the last few replies); undefined from
   *  an older server or another source — the row then shows no chip (Stuart 11-26: the 3-bar meter gave way to it). */
  snr?: number;
  /** ▲ ▼ ● beside the altitude (trendGlyph), and the altitude's colour (altColour) — '' when none. */
  trend: string;
  altCol: string;
  /* ★ V12 aircraft data — the row's second line (adsbInfoLine), the flag of the state of registry and the operator's
   *   badge / logo key; all '' from a server without the data. */
  info: string;
  flag: string;
  op: string;
  logo: string;
}

/* ★★ beta96 (Stuart 11-26: "adsb should drop its 3 bar signal meter for the new colour coded snr"): the bars (3 from
 *  −12 dBFS, 2 from −22 — a level relative to the radio's gain) are gone; the row shows the server's SNR in dB over the
 *  noise floor, coloured by its margin over ADS-B's decode threshold (src/utils/snrColour.ts 'adsb'). */

export function altText(a: Aircraft): string {
  if (a.onGround) return 'GND';
  const ft = a.altitude;
  if (ft == null || !Number.isFinite(ft)) return '';
  return `${(Math.round(ft / 100) * 100).toLocaleString('en-GB')} ft`;   // ★ feet, always — as the map's labels
}

/** ★★ THE ALTITUDE COLOUR — the map's (web/mapkit/adsblayer.js ALT_HUES / altColour, dark), fading through the spectrum.
 *  ONE RULE, THREE READERS (the layer, web/client/src/adsb.ts, this): scripts/test_adsb_layer.mjs fails if they part. */
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
/** ▲ climbing / ▼ descending / ● level (within ±300 ft/min) — the map's trendOf; '' on the ground or with no altitude. */
export function trendGlyph(a: Aircraft): string {
  if (a.onGround || a.altitude == null || !Number.isFinite(a.altitude)) return '';
  const vr = a.vspeed;
  return vr != null && vr > 300 ? '▲' : vr != null && vr < -300 ? '▼' : '●';
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

/** ★★ THE ORDER (Stuart, 2026-10-24): emergencies first, then military, then the nearest to the receiver. */
export function adsbListRows(list: Aircraft[]): AdsbListRow[] {
  // ★★ military: the SERVER SAYS (a.mil — its Annex 10 + VRS block table, the Mictronics flag), else the old rule
  const mil = (a: Aircraft) => !!a.mil || isMilitaryHex(a.icao, a.flight);
  const rank = (a: Aircraft) => (emergencyBadge(a.squawk, a.emergency) ? 0 : mil(a) ? 1 : 2);
  const sorted = [...list].sort((x, y) => {
    const rx = rank(x), ry = rank(y);
    if (rx !== ry) return rx - ry;
    if (x.distKm != null && y.distKm != null && x.distKm !== y.distKm) return x.distKm - y.distKm;
    if (x.distKm != null && y.distKm == null) return -1;
    if (y.distKm != null && x.distKm == null) return 1;
    if (x.distKm == null && (x.rssi ?? -99) !== (y.rssi ?? -99)) return (y.rssi ?? -99) - (x.rssi ?? -99);
    return x.icao < y.icao ? -1 : x.icao > y.icao ? 1 : 0;
  });
  return sorted.map((a) => ({
    hex: a.icao,
    label: a.flight?.trim() || a.reg || a.icao.toUpperCase(),
    alt: altText(a),
    kt: a.speed != null && Number.isFinite(a.speed) ? `${Math.round(a.speed)} kt` : '',
    range: rangeText(a),
    placed: a.lat != null && a.lon != null,
    emergency: !!emergencyBadge(a.squawk, a.emergency),
    badge: emergencyBadge(a.squawk, a.emergency),
    military: mil(a),
    snr: a.snr != null && Number.isFinite(a.snr) ? a.snr : undefined,
    trend: trendGlyph(a),
    altCol: !a.onGround && a.altitude != null && Number.isFinite(a.altitude) ? altColour(a.altitude) : '',
    info: a.info ?? '',
    flag: flagEmoji(a.ccode),
    op: a.op ?? '',
    logo: a.logo ?? '',
  }));
}

/* ── geometry ──────────────────────────────────────────────────────────────────────────────────────────────── */
/** One row's height and the header's, pt — the list sizes from these (the component uses the same numbers). */
// ★ 32 (was 24): a second line under the callsign — operator · type · registration · route (V12 aircraft data)
export const LIST_ROW_H = 32;
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

/* ── ★★★ beta96 THE ROW BY THE LIST'S OWN WIDTH (Stuart 11-26: "the signal hides the aircraft details") ───────────────
 * The web list at ~232 px showed the SNR chip and no callsign: fixed altitude / speed / range columns took the row and the
 * chip beside the name never shrinks. The same arithmetic here: at 240 pt (an SE in Display Zoom) 8 + 4·3 + 50 + 40 + 60 + 8
 * left 62 pt, and the chip (~41 pt with its margin) left the callsign ~3 characters. So the row is laid out from the LIST's
 * width (adsbListWidth — the window's, never the device's), giving way in the web's order and never with the callsign:
 * speed first, then the chip moves under the callsign (the head of the second line), then altitude goes over range in one
 * column at the end. The web's is a container query on #adsbList (web/client/index.html) — the same order. */
export const ROW_PAD = 8, ROW_GAP = 4;
export const COL_ALT = 58, COL_KT = 40, COL_RNG = 60;
/** ★ What the callsign needs: a flag (~15 pt) and 7 characters of the 12 pt decoder font (~53 pt); the chip beside it. */
export const CALL_MIN = 68, CHIP_ROOM = 41;
export interface AdsbRowLayout { kt: boolean; chipUnder: boolean; stacked: boolean }
/** The callsign column's width (pt) for a row `listW` wide laid out as `l`. */
export function adsbCallRoom(listW: number, l: AdsbRowLayout): number {
  const cols = l.stacked ? [Math.max(COL_ALT, COL_RNG)] : [COL_ALT, ...(l.kt ? [COL_KT] : []), COL_RNG];
  return listW - 2 - ROW_PAD * 2 - cols.length * ROW_GAP - cols.reduce((a, b) => a + b, 0);   // 2: the list's border
}
/** ★ The fullest layout whose callsign column still holds the callsign (and the chip, when it sits beside it). */
export function adsbRowLayout(listW: number): AdsbRowLayout {
  const tries: AdsbRowLayout[] = [{ kt: true, chipUnder: false, stacked: false }, { kt: false, chipUnder: false, stacked: false },
                                  { kt: false, chipUnder: true, stacked: false }];
  for (const l of tries) if (adsbCallRoom(listW, l) >= CALL_MIN + (l.chipUnder ? 0 : CHIP_ROOM)) return l;
  return { kt: false, chipUnder: true, stacked: true };
}

/** ★ The collapse state, as it is kept per device ('1' open, '0' a pill). Open unless somebody shut it. */
export const parseListOpen = (v: string | null | undefined): boolean => v !== '0';

/* ── ★★ THE LIST HEADER'S CHIPS (V12, Stuart 2026-10-25): NEWEST · FURTHEST · LONGEST TRACKED ─────────────────────────
 * The web client's list header (web/client/src/adsb.ts adsbNewest / adsbFurthest / adsbLongestTracked) — ONE RULE, TWO
 * READERS, held to each other by scripts/test_adsb_furthest.ts. Tap = show it on the map, so a chip names only an aircraft
 * that is ON the map (a position in the report). FURTHEST moved here from the map's top bar, as on the web (d48dfe43: the
 * top bar crowded on a portrait iPhone). Read from the RAW report (the server's fields — tracked_nm, r), not `Aircraft`. */

export interface AdsbChipSrc { hex?: unknown; flight?: unknown; r?: unknown; lat?: unknown; lon?: unknown; tracked_nm?: unknown }

const chipNum = (v: unknown): number | null => { const n = Number(v); return v == null || v === '' || !isFinite(n) ? null : n; };
/** The web's cleanHex: the ~ of a non-ICAO address kept, lower case, 7 at most. */
export const chipHex = (v: unknown): string => String(v ?? '').replace(/[^~0-9A-Fa-f]/g, '').slice(0, 7).toLowerCase();
const onMap = (a: AdsbChipSrc) => chipNum(a.lat) != null && chipNum(a.lon) != null;
/** As the list names it: its callsign, else its registration, else its address. */
const chipName = (a: AdsbChipSrc, hex: string) => String(a.flight || '').trim() || String(a.r || '').trim() || hex.toUpperCase();
const hasName = (a: AdsbChipSrc) => !!(String(a.flight || '').trim() || String(a.r || '').trim());

/** Great-circle distance, NM — the web's rangeBearing (R = 6371008.8 m). */
function chipNm(rx: { lat: number; lon: number }, lat: number, lon: number): number {
  const D = Math.PI / 180, dp = (lat - rx.lat) * D, dl = (lon - rx.lon) * D;
  const h = Math.sin(dp / 2) ** 2 + Math.cos(rx.lat * D) * Math.cos(lat * D) * Math.sin(dl / 2) ** 2;
  return 2 * 6371008.8 * Math.asin(Math.min(1, Math.sqrt(h))) / 1852;
}

/** FURTHEST: the located aircraft farthest from the receiver; null without a receiver position. ★ Named by callsign, else
 *  its address — the web's adsbFurthest exactly (it does not use the registration). */
export function adsbFurthestOf(list: readonly AdsbChipSrc[] | null, rx: { lat: number; lon: number } | null):
    { hex: string; nm: number; name: string } | null {
  if (!list || !rx) return null;
  let best: { hex: string; nm: number; name: string } | null = null;
  for (const a of list) {
    const lat = chipNum(a.lat), lon = chipNum(a.lon), hex = chipHex(a.hex);
    if (lat == null || lon == null || !hex) continue;
    const nm = chipNm(rx, lat, lon);
    if (!best || nm > best.nm) best = { hex, nm, name: String(a.flight || '').trim() || hex.toUpperCase() };
  }
  return best;
}

/** LONGEST TRACKED: the aircraft the server has followed the furthest (tracked_nm ≥ 1), on the map. */
export function adsbLongestTrackedOf(list: readonly AdsbChipSrc[] | null): { hex: string; nm: number; name: string } | null {
  if (!list) return null;
  let best: { hex: string; nm: number; name: string } | null = null;
  for (const a of list) {
    const nm = chipNum(a.tracked_nm), hex = chipHex(a.hex);
    if (nm == null || nm < 1 || !hex || !onMap(a)) continue;
    if (!best || nm > best.nm) best = { hex, nm, name: chipName(a, hex) };
  }
  return best;
}

/** NEWEST: the aircraft first heard most recently, with a name and a position. The report carries no "first heard", so
 *  `firstSeen` notes when each hex FIRST appears (call once per report). The first report's aircraft were there before the
 *  listener arrived — stamped 0, never chosen. One that leaves is forgotten (it is new again if it comes back). null clears. */
export function adsbNewestOf(list: readonly AdsbChipSrc[] | null, firstSeen: Map<string, number>, nowMs: number):
    { hex: string; name: string; ageS: number } | null {
  if (!list) { firstSeen.clear(); return null; }
  const fresh = firstSeen.size === 0;
  const byHex = new Map<string, AdsbChipSrc>();
  for (const a of list) {
    const hex = chipHex(a.hex);
    if (!hex) continue;
    byHex.set(hex, a);
    if (!firstSeen.has(hex)) firstSeen.set(hex, fresh ? 0 : nowMs);
  }
  for (const h of [...firstSeen.keys()]) if (!byHex.has(h)) firstSeen.delete(h);
  let best = '', at = 0;
  for (const [h, t] of firstSeen) { const a = byHex.get(h); if (t > at && a && hasName(a) && onMap(a)) { at = t; best = h; } }
  if (!best) return null;
  return { hex: best, name: chipName(byHex.get(best)!, best), ageS: Math.max(0, Math.round((nowMs - at) / 1000)) };
}

