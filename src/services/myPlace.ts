/**
 * ★★ beta94 MY LOCATION on the server list (Stuart 2026-11-25) — the pure half: which position the directory sorts and
 * filters by, what the row says, the words, and the distances worked out again when it changes. No React Native here
 * (scripts/test_sat_place.ts runs it).
 *
 * ONE stored value for the whole app (localPlace.ts, key vibe.localPlace): the SATELLITE box, the FT8 / spots map's city
 * picker and this row all read and write it, so a change in any of them shows in all of them.
 *
 *  - Which position: one the user ENTERED (a town or locator) wins; else the phone's location where it is allowed; else
 *    none — the list as it is with no location (no NEAR sort, no km). Same rule as Local Hardware (localPlaceChoose).
 *  - ★★★ NEVER PUBLISHED: distances are a haversine on this phone against the positions each listing already carries
 *    (no directory is told where the listener is — instancesApi.ts since 2026-09-15); the server owner's location
 *    (vibeServer.ts publishLocation / setLocationJson) is a separate thing and is never fed from here.
 */
import { haversineKm, latLonToGrid } from './grid';
import { localPlaceChoose, type LocalPlace } from './satPlace';

/** The directory's position: an entered one wins, else the phone's, else none. (Local Hardware's rule, on purpose.) */
export function directoryPlace(entered: { lat: number; lon: number; label?: string } | null | undefined,
                               gps: { lat: number; lon: number } | null | undefined): LocalPlace | null {
  return localPlaceChoose(entered, gps);
}

/** The row's value: "Northampton IO92 (entered)" / "This phone’s location" / "Not set — tap to set". */
export function myPlaceLine(p: LocalPlace | null | undefined): string {
  if (!p) return 'Not set — tap to set';
  if (p.src === 'gps') return 'This phone’s location';
  const sq = latLonToGrid(p.lat, p.lon).slice(0, 4);
  const label = (p.label ?? '').trim();
  // A typed locator is its own name ("IO92nh (entered)"), not said twice.
  if (!label || label.toUpperCase().startsWith(sq.toUpperCase())) return `${label || sq} (entered)`;
  return `${label} ${sq} (entered)`;
}

export const MY_PLACE_TITLE = 'MY LOCATION';
export const MY_PLACE_HELP =
  'Used for server distances on this list, and for satellite passes and the spots and maps in Local Hardware. ' +
  'It stays on this phone — it is never sent to a server or published. The nearest town is enough.';
export const MY_PLACE_ENTRY_HINT = 'Town or locator (e.g. Northampton or IO92nh)';
export const MY_PLACE_NOT_FOUND = 'That place could not be found. Try the nearest town, or a locator such as IO92nh (a locator works offline).';
/** Where to allow it — the phone's own Settings path, per platform (never naming the other one). */
export function myPlaceDenied(os: string): string {
  return os === 'android'
    ? 'VibeSDR is not allowed to use this phone’s location. Allow it in the phone’s Settings (Apps › VibeSDR › ' +
      'Permissions › Location), or type the nearest town or your locator instead.'
    : 'VibeSDR is not allowed to use this device’s location. Turn on Location for VibeSDR in Settings, or type the ' +
      'nearest town or your locator instead.';
}

type Located = { latitude?: number | null; longitude?: number | null; distance: number | null };

/** The distances again for a new position (or none) — from each row's own published position; no network. A row with
 *  no position keeps what it had (null with no position, as the fetch would have given). Located: nearest first, as
 *  fetchDirectory orders a list (the COUNTRY groups keep this order); none: the order is left as it is. */
export function relocate<T extends Located>(list: T[], loc: { lat: number; lon: number } | null): T[] {
  const out = list.map((i) => {
    if (i.latitude == null || i.longitude == null || !Number.isFinite(i.latitude) || !Number.isFinite(i.longitude)) {
      return loc ? i : (i.distance == null ? i : { ...i, distance: null });
    }
    const d = loc ? haversineKm(loc, { lat: i.latitude, lon: i.longitude }) : null;
    return d === i.distance ? i : { ...i, distance: d };
  });
  return loc ? out.sort((a, b) => (a.distance ?? 1e9) - (b.distance ?? 1e9)) : out;
}

/** Same for the favourites' harvested metrics (normUrl → {dist, snr, lat, lon}). */
export function relocateMeta<M extends { dist: number | null; lat?: number | null; lon?: number | null }>(
    meta: Record<string, M>, loc: { lat: number; lon: number } | null): Record<string, M> {
  const out: Record<string, M> = {};
  for (const [k, m] of Object.entries(meta)) {
    out[k] = m.lat != null && m.lon != null
      ? { ...m, dist: loc ? haversineKm(loc, { lat: m.lat, lon: m.lon }) : null }
      : (loc ? m : { ...m, dist: null });
  }
  return out;
}
