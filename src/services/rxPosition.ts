/* ★★ b96-rangefeed (Stuart 11-27, the Lenovo's AIR & SEA viewer: "actually i cant activate the range rings either") —
 * WHERE A MAP'S RECEIVER POSITION COMES FROM, for the web client's maps (tracks.ts host: the AIR & SEA map, the datalink
 * map) and the app's. The ADS-B layer itself (web/mapkit/adsblayer.js) falls back to the range outline's centre last.
 *
 * The order: the server's lat/lon → else the centre of its LOCATOR square (approximate: to the square, ~5 × 5 km for six
 * characters, 2° × 1° for four) → else (the layer) the /adsb/range centre. A server set up with only a locator (IO92nh)
 * answers /location with the square's centre as lat/lon — so a position that IS its locator's centre is approximate too.
 *
 * And on a FRONT DOOR page (the splash's AIR & SEA viewer, before any radio is started) /location and /adsb/range are the
 * radios' — the door answers its own HTML page — so the viewer asks the door's primary radio (/r/<id>/…). Pure. */
import { gridToLatLon } from './grid';

export type RxFrom = 'set' | 'locator' | 'range';
export interface RxPos { lat: number; lon: number; from: RxFrom }

const ok = (lat: unknown, lon: unknown): boolean =>
  typeof lat === 'number' && typeof lon === 'number' && Number.isFinite(lat) && Number.isFinite(lon)
  && Math.abs(lat) <= 90 && Math.abs(lon) <= 180 && !(lat === 0 && lon === 0);

/** GET /location's answer → the receiver's position and how exact it is (null = none). */
export function rxFromLocation(j: unknown): RxPos | null {
  if (!j || typeof j !== 'object') return null;
  const o = j as Record<string, unknown>;
  const g = typeof o.grid === 'string' ? o.grid : typeof o.locator === 'string' ? o.locator : '';
  const sq = gridToLatLon(g);
  if (ok(o.lat, o.lon)) {
    const lat = o.lat as number, lon = o.lon as number;
    // ★ the server put the locator's centre in lat/lon (no lat/lon configured): say it is approximate
    const isCentre = !!sq && Math.abs(sq.lat - lat) < 1e-4 && Math.abs(sq.lon - lon) < 1e-4;
    return { lat, lon, from: isCentre ? 'locator' : 'set' };
  }
  return sq && ok(sq.lat, sq.lon) ? { lat: sq.lat, lon: sq.lon, from: 'locator' } : null;
}

/** The words a map adds where its receiver position is not exact ('' = exact). */
export function rxApproxWords(from: RxFrom | null | undefined, grid?: string): string {
  if (from === 'locator') return `approximate — the centre of ${grid ? `locator square ${grid.toUpperCase()}` : 'the server\'s locator square'}`;
  if (from === 'range') return 'approximate — the centre of the server\'s ADS-B range outline';
  return '';
}

/** The front door's radio a viewer on the door asks for /location and /adsb/range: the primary, else the first. */
export function doorRadioId(dir: unknown): string {
  const radios = dir && typeof dir === 'object' && Array.isArray((dir as any).radios) ? (dir as any).radios as any[] : [];
  const ids = radios.filter((r) => r && typeof r.id === 'string' && /^[0-9a-z]{1,16}$/.test(r.id));
  const p = ids.find((r) => r.primary === true) ?? ids[0];
  return p ? p.id : '';
}
