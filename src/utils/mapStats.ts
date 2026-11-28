/**
 * mapStats.ts — ★★★ beta97 THE STATS CARD in the app (Stuart 11-27, the web's main.ts asStatsSync — the same rule, pure):
 * per MODE present (ADS-B, HFDL, VDL2, ACARS, ADS-C) — NEWEST (first heard since the map opened), FURTHEST (from the
 * receiver), FURTHEST TRACKED (distance flown while that mode heard it) and LONGEST TRACKED (time heard). Modes never
 * compete; a row selects that aircraft. Bounded (MAP-TEMPLATE rule 5): ≤ 3 000 a mode, forgotten 10 min after the last
 * sighting; a row counts only while heard in the last 2 min. A jump of 300 NM is a bad fix, never distance flown.
 */
export interface StatsEntry { first: number; at: number; lat: number; lon: number; nm: number; name: string }
export type StatsStore = Map<string, Map<string, StatsEntry>>;
export interface StatsObs { mode: string; key: string; lat: number; lon: number; name: string; trackedNm?: number }
export interface StatsSection { mode: string; rows: Array<{ key: string; label: string; value: string; name: string; lat: number; lon: number }> }

export const STATS_CAP = 3000, STATS_FORGET_MS = 600_000, STATS_LIVE_MS = 120_000;
export const STATS_ORDER = ['ADS-B', 'HFDL', 'VDL2', 'ACARS', 'ADS-C'];

const D2R = Math.PI / 180;
export function nmBetween(a: { lat: number; lon: number }, b: { lat: number; lon: number }): number {
  const p1 = a.lat * D2R, p2 = b.lat * D2R, dp = (b.lat - a.lat) * D2R, dl = (b.lon - a.lon) * D2R;
  const h = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  return (2 * 6371.0088 * Math.asin(Math.min(1, Math.sqrt(h)))) / 1.852;
}

/** ★★ 11-28 EACH MODE ITS OWN FIXES (Stuart, the community map: "ADS-B furthest 2540 km" — an aircraft first heard on
 *  ADS-B that later moved to HFDL kept counting for ADS-B at its HFDL position). A shared aircraft's `src` is every source
 *  that has heard it (the server's srcMask), its position came from ONE of them (`posSrc`, vibe_aircraft_share.h): the
 *  position is a sighting for THAT mode only — never ADS-B's furthest from an HFDL / VDL2 / ACARS / ADS-C fix, nor the
 *  other way round. No posSrc (an older server's row) = nobody's fix. */
export function statsObsOfShared(aircraft: ReadonlyArray<{ key: string; lat?: number; lon?: number; posSrc?: string; flight?: string; reg?: string; icao?: string }> | null | undefined): StatsObs[] {
  const out: StatsObs[] = [];
  for (const pl of aircraft ?? []) {
    if (pl.lat == null || pl.lon == null || !pl.posSrc) continue;
    out.push({ mode: pl.posSrc, key: `a:${pl.key}`, lat: pl.lat, lon: pl.lon, name: pl.flight || pl.reg || pl.icao || '' });
  }
  return out;
}

/** Fold one batch of sightings in. `firstPass` (the store was empty — the map just opened): nothing counts as NEWEST. */
export function statsSeen(store: StatsStore, obs: StatsObs[], now: number): void {
  const firstPass = store.size === 0;
  for (const o of obs) {
    if (!Number.isFinite(o.lat) || !Number.isFinite(o.lon) || !o.key) continue;
    let m = store.get(o.mode); if (!m) { m = new Map(); store.set(o.mode, m); }
    let e = m.get(o.key);
    if (!e) { if (m.size >= STATS_CAP) continue; e = { first: firstPass ? 0 : now, at: now, lat: o.lat, lon: o.lon, nm: 0, name: o.name }; m.set(o.key, e); }
    const d = nmBetween(e, o);
    if (d > 0.05 && d < 300) e.nm += d;
    e.lat = o.lat; e.lon = o.lon; e.at = now; if (o.name) e.name = o.name;
    if (o.trackedNm != null && o.trackedNm > e.nm) e.nm = o.trackedNm;
  }
}

/** The card's sections, the stale forgotten as it goes. `opened` = when the map opened (LONGEST TRACKED's floor). */
export function statsSections(store: StatsStore, rx: { lat: number; lon: number } | null, now: number, opened: number): StatsSection[] {
  const out: StatsSection[] = [];
  const rank = (m: string) => { const i = STATS_ORDER.indexOf(m); return i < 0 ? 99 : i; };
  for (const mode of [...store.keys()].sort((a, b) => rank(a) - rank(b) || a.localeCompare(b))) {
    const m = store.get(mode)!;
    for (const [k, e] of m) if (now - e.at > STATS_FORGET_MS) m.delete(k);
    const live = [...m.entries()].filter(([, e]) => now - e.at < STATS_LIVE_MS);
    if (!live.length) continue;
    const rows: StatsSection['rows'] = [];
    const push = (label: string, b: [string, StatsEntry] | undefined, value: string) => { if (b) rows.push({ key: b[0], label, value, name: b[1].name, lat: b[1].lat, lon: b[1].lon }); };
    const newest = live.filter(([, e]) => e.first > 0 && e.name).sort((a, b) => b[1].first - a[1].first)[0];
    push('NEWEST', newest, newest ? `${Math.max(1, Math.round((now - newest[1].first) / 60000))} min ago` : '');
    if (rx) {
      const fur = live.map(([k, e]) => [k, e, nmBetween(rx, e)] as const).sort((a, b) => b[2] - a[2])[0];
      if (fur) rows.push({ key: fur[0], label: 'FURTHEST', value: `${Math.round(fur[2])} NM`, name: fur[1].name, lat: fur[1].lat, lon: fur[1].lon });
    }
    const lng = live.filter(([, e]) => e.nm >= 1).sort((a, b) => b[1].nm - a[1].nm)[0];
    push('FURTHEST TRACKED', lng, lng ? `${Math.round(lng[1].nm)} NM` : '');
    const old = live.slice().sort((a, b) => (a[1].first || opened) - (b[1].first || opened))[0];
    if (old) { const mins = Math.round((now - (old[1].first || opened)) / 60000); if (mins >= 1) push('LONGEST TRACKED', old, `${mins} min${old[1].first ? '' : '+'}`); }
    if (rows.length) out.push({ mode, rows });
  }
  return out;
}
