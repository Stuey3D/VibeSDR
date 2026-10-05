// Maidenhead grid-locator → lat/lon, and great-circle distance.
//
// FT8/FT4 messages carry the transmitter's Maidenhead locator (usually 4 chars,
// sometimes 6). The on-device decoder (local USB hardware + Kiwi via the native
// sidecar) emits that grid in each digital_spot, so we can show distance from the
// receiver to each spot without any server-side enrichment.

/**
 * Encode lat/lon as a 6-char Maidenhead locator ("IO92nh").
 *
 * The inverse of gridToLatLon. Used to describe the RECEIVER's own position the way
 * radio people expect to read it — a VibeServer announces itself as e.g.
 * "Moto G35 / Northampton IO92nh", the same shape UberSDR uses.
 *
 * Field (18 zones of 20°/10°) → square (10 of 2°/1°) → subsquare (24 of 5'/2.5').
 * Convention: fields and squares upper-case, subsquare lower-case.
 */
export function latLonToGrid(lat: number, lon: number): string {
  const A = 'ABCDEFGHIJKLMNOPQR';
  const a = 'abcdefghijklmnopqrstuvwx';
  // Shift to 0..360 / 0..180 so all the divisions are positive.
  const x = Math.min(359.99999, Math.max(0, lon + 180));
  const y = Math.min(179.99999, Math.max(0, lat + 90));
  const f1 = Math.floor(x / 20), f2 = Math.floor(y / 10);
  const s1 = Math.floor((x % 20) / 2), s2 = Math.floor(y % 10);
  const u1 = Math.floor(((x % 2) / 2) * 24), u2 = Math.floor((y % 1) * 24);
  return `${A[f1]}${A[f2]}${s1}${s2}${a[u1]}${a[u2]}`;
}

/**
 * The receiver badge's second line — "Northampton · 52.31, -0.88 · IO92nh" — one format for every server type
 * (2026-10-05). VibeServer's /location, a Kiwi's /status (loc= + gps=) and OpenWebRX's /status.json
 * (receiver.location + receiver.gps) all carry a place and a position; the badge used to show the position only for
 * VibeServer. The grid is derived from the position when the server does not state one.
 * ★ (0, 0) is "not set", not the Gulf of Guinea — a Kiwi with no GPS fix and no admin position reports exactly that.
 * ★ The place is shown once: a Kiwi admin often types the locator or the coordinates into loc= themselves.
 */
export function receiverPlaceLine(place?: string | null, lat?: number | null, lon?: number | null,
                                  grid?: string | null): string {
  const p = (place ?? '').trim();
  const hasPos = typeof lat === 'number' && typeof lon === 'number' && Number.isFinite(lat) && Number.isFinite(lon)
    && Math.abs(lat) <= 90 && Math.abs(lon) <= 180 && !(lat === 0 && lon === 0);
  const coords = hasPos ? `${lat!.toFixed(2)}, ${lon!.toFixed(2)}` : '';
  // ★ A place that IS a locator already says it — a second, derived one would only disagree in the subsquare.
  const placeIsGrid = /^[A-R]{2}[0-9]{2}([A-X]{2})?$/i.test(p);
  const g = placeIsGrid ? '' : (grid ?? '').trim() || (hasPos ? latLonToGrid(lat!, lon!) : '');
  const inPlace = (s: string) => !!s && p.toUpperCase().includes(s.toUpperCase());
  return [p || coords, p && coords && !inPlace(coords) ? coords : '', g && !inPlace(g) && g !== (p || coords) ? g : '']
    .filter(Boolean).join(' · ');
}

/** Decode a 4- or 6-char Maidenhead locator to the centre of its square.
 *  Returns null for anything that isn't a valid locator. */
export function gridToLatLon(grid?: string | null): { lat: number; lon: number } | null {
  if (!grid) return null;
  const g = grid.trim().toUpperCase();
  if (!/^[A-R]{2}[0-9]{2}([A-X]{2})?$/.test(g)) return null;
  // ★★ "RR73" is FT8's sign-off, never a locator here — it LOOKS like a square near the North Pole, so
  //    every station signing off was plotted in the Arctic (older servers still send it as the grid).
  if (g === 'RR73') return null;

  // Field (20° lon / 10° lat) + square (2° lon / 1° lat).
  let lon = (g.charCodeAt(0) - 65) * 20 - 180;
  let lat = (g.charCodeAt(1) - 65) * 10 - 90;
  lon += (g.charCodeAt(2) - 48) * 2;
  lat += (g.charCodeAt(3) - 48) * 1;

  if (g.length === 6) {
    // Sub-square (5′ lon / 2.5′ lat); centre within it.
    lon += (g.charCodeAt(4) - 65) * (2 / 24) + (1 / 24);
    lat += (g.charCodeAt(5) - 65) * (1 / 24) + (0.5 / 24);
  } else {
    // Centre of the 2°×1° square.
    lon += 1;
    lat += 0.5;
  }
  return { lat, lon };
}

const R_KM = 6371.0088;
const toRad = (d: number) => (d * Math.PI) / 180;

/** Great-circle distance in km between two lat/lon points. */
export function haversineKm(
  a: { lat: number; lon: number },
  b: { lat: number; lon: number },
): number {
  const dLat = toRad(b.lat - a.lat);
  const dLon = toRad(b.lon - a.lon);
  const s =
    Math.sin(dLat / 2) ** 2 +
    Math.cos(toRad(a.lat)) * Math.cos(toRad(b.lat)) * Math.sin(dLon / 2) ** 2;
  return 2 * R_KM * Math.asin(Math.min(1, Math.sqrt(s)));
}

/** Distance (km) from a receiver to a spot's grid, or undefined if either is
 *  missing/invalid. */
export function distanceKmToGrid(
  rx: { lat: number; lon: number } | null,
  grid?: string | null,
): number | undefined {
  if (!rx) return undefined;
  const tx = gridToLatLon(grid);
  if (!tx) return undefined;
  return Math.round(haversineKm(rx, tx));
}
