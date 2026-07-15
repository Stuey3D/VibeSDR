/**
 * On-device access to the bundled, read-only station database (addendum §3–§5).
 *
 * The DB (assets/db/stations.sqlite) is produced by tools/build_station_db/ from
 * the FCC LMS files and shipped in the APK. At runtime it's copied once into the
 * SQLite dir and opened read-only. This layer is OFFLINE-ONLY and never touches
 * the network — enrichment (logos/genre) lives in radioBrowser.ts. Everything
 * degrades to "empty" if the DB hasn't been built yet (placeholder ships with 0
 * rows), so the feature never crashes; it just returns nothing until the DB is
 * populated.
 */

import { Asset } from 'expo-asset';
import * as FileSystem from 'expo-file-system/legacy';
import * as SQLite from 'expo-sqlite';

import { boundingBox, haversineKm, receivabilityScore } from './stationGeo';
import type { StationRow } from './stationTypes';

const DB_NAME = 'stations.sqlite';
// Bump this whenever you rebuild + rebundle the DB (see tools/build_station_db):
// on change the stale copy in the SQLite dir is replaced with the new asset.
const DB_ASSET_VERSION = '1';

let dbPromise: Promise<SQLite.SQLiteDatabase | null> | null = null;

async function openDb(): Promise<SQLite.SQLiteDatabase | null> {
  try {
    const dir = `${FileSystem.documentDirectory}SQLite`;
    const dbPath = `${dir}/${DB_NAME}`;
    const verPath = `${dir}/${DB_NAME}.version`;

    const dbInfo = await FileSystem.getInfoAsync(dbPath);
    let haveCurrent = dbInfo.exists;
    if (haveCurrent) {
      try {
        const v = await FileSystem.readAsStringAsync(verPath);
        if (v.trim() !== DB_ASSET_VERSION) haveCurrent = false;
      } catch { haveCurrent = false; }
    }

    if (!haveCurrent) {
      await FileSystem.makeDirectoryAsync(dir, { intermediates: true }).catch(() => {});
      const asset = Asset.fromModule(require('../../assets/db/stations.sqlite'));
      await asset.downloadAsync();
      if (!asset.localUri) return null;
      if (dbInfo.exists) await FileSystem.deleteAsync(dbPath, { idempotent: true });
      await FileSystem.copyAsync({ from: asset.localUri, to: dbPath });
      await FileSystem.writeAsStringAsync(verPath, DB_ASSET_VERSION);
    }

    return await SQLite.openDatabaseAsync(DB_NAME);
  } catch (e) {
    console.warn('[stationDb] open failed', e);
    return null;
  }
}

function db(): Promise<SQLite.SQLiteDatabase | null> {
  return (dbPromise ??= openDb());
}

// Raw row shape as stored (snake_case columns) → mapped to StationRow.
interface RawRow {
  callsign: string; callsign_base: string; frequency_mhz: number; service: string;
  station_class: string | null; erp_kw: number | null; lat: number; lon: number;
  city: string | null; state: string | null; facility_id: number;
}
const mapRow = (r: RawRow): StationRow => ({
  callsign: r.callsign,
  callsignBase: r.callsign_base,
  frequencyMhz: r.frequency_mhz,
  service: r.service,
  stationClass: r.station_class,
  erpKw: r.erp_kw,
  lat: r.lat,
  lon: r.lon,
  city: r.city,
  state: r.state,
  facilityId: r.facility_id,
});

export interface NearbyDbResult extends StationRow {
  distanceKm: number;
  score: number;
}

/**
 * Stations within radiusKm of (lat,lon), ranked by receivability (best first).
 * Bounding-box prefilter on the indexed lat/lon columns, then haversine to trim
 * the box corners, then score. Pure-offline; returns [] if the DB is unbuilt.
 */
export async function nearbyStations(
  lat: number, lon: number, radiusKm = 100, limit = 100,
): Promise<NearbyDbResult[]> {
  const d = await db();
  if (!d) return [];
  const b = boundingBox(lat, lon, radiusKm);
  let raw: RawRow[];
  try {
    raw = await d.getAllAsync<RawRow>(
      `SELECT * FROM stations
        WHERE lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?`,
      [b.minLat, b.maxLat, b.minLon, b.maxLon],
    );
  } catch (e) {
    console.warn('[stationDb] nearby query failed', e);
    return [];
  }
  const out: NearbyDbResult[] = [];
  for (const r of raw) {
    const distanceKm = haversineKm(lat, lon, r.lat, r.lon);
    if (distanceKm > radiusKm) continue; // trim box → circle
    const row = mapRow(r);
    out.push({
      ...row,
      distanceKm,
      score: receivabilityScore({ erpKw: row.erpKw, stationClass: row.stationClass, distanceKm }),
    });
  }
  out.sort((a, b2) => b2.score - a.score);
  return out.slice(0, limit);
}

/** All station rows sharing a 4-letter callsign base (PI-decode lookup, §6). */
export async function stationsForCallsignBase(base: string): Promise<StationRow[]> {
  const d = await db();
  if (!d || !base) return [];
  try {
    const raw = await d.getAllAsync<RawRow>(
      `SELECT * FROM stations WHERE callsign_base = ?`, [base.toUpperCase()],
    );
    return raw.map(mapRow);
  } catch (e) {
    console.warn('[stationDb] callsign lookup failed', e);
    return [];
  }
}

/** LMS snapshot date to show unobtrusively ("station data as of …"), or null. */
export async function snapshotDate(): Promise<string | null> {
  const d = await db();
  if (!d) return null;
  try {
    const row = await d.getFirstAsync<{ value: string }>(
      `SELECT value FROM meta WHERE key = 'lms_snapshot_date'`,
    );
    const v = row?.value ?? null;
    return v && v !== 'unbuilt' ? v : null;
  } catch {
    return null;
  }
}
