// Shared types for the offline "stations near me" feature (addendum §4, §5, §7).

/** A row of the bundled, read-only FCC-derived station table. */
export interface StationRow {
  callsign: string;
  callsignBase: string;      // 4-letter base, PI join key
  frequencyMhz: number;      // 88.1 .. 107.9
  service: 'FM' | 'FX' | 'FL' | string;  // full-power / translator / LPFM
  stationClass: string | null;
  erpKw: number | null;
  lat: number;
  lon: number;
  city: string | null;
  state: string | null;
  facilityId: number;
}

/** Online enrichment (Radio-Browser). Every field is optional/absent offline. */
export interface Enrichment {
  genre: string | null;
  logoUri: string | null;    // local cached file:// path, or null
  homepage: string | null;
  fetchedAt: number | null;  // epoch ms
}

/** A ranked nearby station = DB row + distance/score + (optional) enrichment. */
export interface NearbyStation extends StationRow {
  distanceKm: number;
  score: number;             // receivability rank, higher = more likely audible
  genre: string | null;
  logoUri: string | null;
  homepage: string | null;
}

/** Result of identifying a live station from its RDS PI (addendum §6). */
export interface StationIdentity {
  pi: number;
  callsign: string | null;   // PI-derived hint (may be wrong for translators)
  confident: boolean;        // PI decode clean AND a DB match agrees
  station: StationRow | null;
  note?: string;
}
