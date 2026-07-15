/**
 * Public facade for the "stations near me" feature — the surface the UI (the
 * "Nearby" button + picker) calls. Offline-first: the list always comes from the
 * bundled FCC DB and NEVER blocks on a network call; enrichment (logo/genre) is
 * merged from cache and refreshed in the background (addendum §1, §7, §8).
 *
 * UI contract (see docs/backend/nearby-stations-api.md):
 *   getNearbyStations()  -> ranked NearbyStation[] + location + data date
 *   identifyByPi()       -> live station identity from RDS PI, before PS arrives
 *   enrichNow()          -> on-demand enrich one station (e.g. row on screen)
 */

import { getUserLocation } from './instancesApi';
import { nearbyStations as dbNearby, stationsForCallsignBase, snapshotDate } from './stationDb';
import { getCachedEnrichment, enrichStation } from './radioBrowser';
import { piToCallsign, callsignBase } from './piCallsign';
import type { NearbyStation, StationIdentity, StationRow } from './stationTypes';

export interface NearbyResult {
  /** GPS point used, or null if no fix (UI should say so). */
  location: { lat: number; lon: number } | null;
  radiusKm: number;
  stations: NearbyStation[];
  /** LMS snapshot date ("station data as of …"), or null if DB unbuilt. */
  snapshotDate: string | null;
}

export interface NearbyOptions {
  radiusKm?: number;          // default 100 km (addendum §5.4)
  limit?: number;
  /** Override GPS (e.g. a manually chosen city). */
  location?: { lat: number; lon: number };
  /** Kick off background online enrichment for the top rows (default true). */
  enrich?: boolean;
}

/**
 * Ranked nearby stations. Resolves entirely from the offline DB + enrichment
 * cache, so it works in airplane mode; if online and `enrich` is on, it also
 * fires background fetches for the top rows whose logo/genre aren't cached yet
 * (results appear on a later call — never awaited here).
 */
export async function getNearbyStations(opts: NearbyOptions = {}): Promise<NearbyResult> {
  const radiusKm = opts.radiusKm ?? 100;
  const location = opts.location ?? (await getUserLocation());
  const snap = await snapshotDate();
  if (!location) return { location: null, radiusKm, stations: [], snapshotDate: snap };

  const rows = await dbNearby(location.lat, location.lon, radiusKm, opts.limit ?? 100);

  // Merge cached enrichment (offline, per row). Absent fields stay null (§7).
  const stations: NearbyStation[] = await Promise.all(
    rows.map(async (r) => {
      const e = await getCachedEnrichment(r.callsignBase);
      return { ...r, genre: e?.genre ?? null, logoUri: e?.logoUri ?? null, homepage: e?.homepage ?? null };
    }),
  );

  // Background enrichment for the top rows lacking a cached logo (fire-and-forget,
  // rate-limited inside radioBrowser). Never blocks the returned list.
  if (opts.enrich !== false) {
    stations.slice(0, 20)
      .filter((s) => s.logoUri == null && s.service === 'FM')
      .forEach((s) => { void enrichStation(s.callsignBase); });
  }

  return { location, radiusKm, stations, snapshotDate: snap };
}

/** Pick the best DB row for a decoded callsign: prefer full-power FM. */
function bestStation(rows: StationRow[]): StationRow | null {
  if (rows.length === 0) return null;
  const order: Record<string, number> = { FM: 0, FL: 1, FX: 2 };
  return [...rows].sort((a, b) => (order[a.service] ?? 9) - (order[b.service] ?? 9))[0];
}

/**
 * Identify the live station from its RDS PI (addendum §6). PI is in block 1 of
 * every group, so this resolves an identity offline within a second of tuning —
 * before PS/RadioText assemble. The result is a HINT: `confident` is true only
 * for a clean formula decode that matches a full-power FM row in the DB.
 * Translators, defaults, and A-block PIs come back not-confident; the UI should
 * prefer decoded PS text when it disagrees.
 */
export async function identifyByPi(pi: number, psText?: string): Promise<StationIdentity> {
  const dec = piToCallsign(pi);
  if (!dec.callsign) {
    return { pi, callsign: null, confident: false, station: null, note: dec.note };
  }
  const rows = await stationsForCallsignBase(callsignBase(dec.callsign));
  const station = bestStation(rows);

  // Confident only when the decode is clean, a DB row exists, and it's a
  // full-power FM (translators are exactly where the formula misfires).
  let confident = dec.confident && station != null && station.service === 'FM';
  let note = dec.note;
  if (station == null) note = 'no DB match for computed callsign';
  else if (station.service !== 'FM') note = `matched a ${station.service} (formula unreliable for translators)`;

  // If PS text is present and clearly names a different callsign, stand down.
  if (psText) {
    const ps = psText.toUpperCase();
    const other = ps.match(/\b([KW][A-Z]{3})\b/);
    if (other && other[1] !== callsignBase(dec.callsign)) {
      confident = false;
      note = `PS text names ${other[1]}, not ${dec.callsign}`;
    }
  }
  return { pi, callsign: dec.callsign, confident, station, note };
}

/** On-demand enrichment for a single station (e.g. when its row is shown). */
export async function enrichNow(callsignBaseStr: string, nameHint?: string) {
  return enrichStation(callsignBaseStr, nameHint);
}

/** LMS snapshot date for the unobtrusive "data as of …" label (§8). */
export async function getStationDataDate(): Promise<string | null> {
  return snapshotDate();
}
