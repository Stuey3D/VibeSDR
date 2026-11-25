/**
 * ★★★ WHERE THE RECEIVER IS — one rule for the spots map, spot distances and the time-station guess.
 *
 * The aerial is what hears the spot, so distances and the map's home marker are measured from the
 * ANTENNA, not from the phone in someone's hand. A VibeServer publishes its own position at GET
 * /location (lat/lon, or only a locator); a KiwiSDR sends gps=(lat,lon). Either of those wins.
 * The phone's own position — or a town picked from SET LOCATION — is only for when the radio cannot
 * say: Local Hardware / OTG (the phone IS the receiver's site) or a server that publishes nothing.
 *
 * ✗ The bug this replaces (Stuart, 2026-11-25, 11.0.16 on the Duo simulator): connected to his RSP1A
 *   server, which shows "52.31, -0.88 · IO92nh" under its name, the Digital Spots map offered SET
 *   LOCATION and drew no receiver. SDRScreen read /location for the ITU region and kept only the
 *   LONGITUDE; the map was fed from the phone's GPS alone, which the simulator does not have.
 */
import { gridToLatLon } from './grid';

export type LatLon = { lat: number; lon: number };

/** A usable position: finite, on the globe, and not 0,0 (Null Island = "not set" everywhere in this app). */
export function validPos(p: { lat?: unknown; lon?: unknown } | null | undefined): p is LatLon {
  if (!p) return false;
  const { lat, lon } = p as { lat: unknown; lon: unknown };
  if (typeof lat !== 'number' || typeof lon !== 'number') return false;
  if (!Number.isFinite(lat) || !Number.isFinite(lon)) return false;
  if (Math.abs(lat) > 90 || Math.abs(lon) > 180) return false;
  return lat !== 0 || lon !== 0;
}

/** A VibeServer's GET /location reply → its position. lat/lon when both are given and sane, else the
 *  locator (`grid`, or `locator` from older servers) decoded to its square's centre, else null (the
 *  owner has not published a position — `{ name }` alone is a valid reply). */
export function serverPosFromLocation(j: unknown): LatLon | null {
  if (!j || typeof j !== 'object') return null;
  const o = j as Record<string, unknown>;
  const ll = { lat: o.lat, lon: o.lon };
  if (validPos(ll)) return { lat: ll.lat, lon: ll.lon };
  const g = typeof o.grid === 'string' ? o.grid : typeof o.locator === 'string' ? o.locator : '';
  const fromGrid = gridToLatLon(g);
  return validPos(fromGrid) ? fromGrid : null;
}

/** The receiver's position for maps and distances: the RADIO's own (server /location, Kiwi gps) first,
 *  then this phone's (GPS or a picked town), else null — which is the only case the map offers
 *  SET LOCATION. */
export function receiverPos(server: LatLon | null | undefined, local: LatLon | null | undefined): LatLon | null {
  if (validPos(server)) return server;
  if (validPos(local)) return local;
  return null;
}

/** Should this phone be asked for ITS location (an OS permission prompt)? Only when the radio is on
 *  this device, or the server has answered /location with no position. `serverPos === undefined`
 *  means the server has not answered yet — wait, or a phone prompt pops up for nothing. */
export function shouldAskPhoneForPlace(isLocal: boolean, isRemoteServer: boolean,
                                      serverPos: LatLon | null | undefined): boolean {
  if (!isLocal) return false;
  if (!isRemoteServer) return true;
  return serverPos === null;
}
