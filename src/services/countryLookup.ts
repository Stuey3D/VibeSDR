// Offline coordinate → ISO-3166 country code. Kiwi/Receiverbook directory entries carry
// lat/long but no country code, so we derive it here to group them by country — no network,
// no permission, works on the standalone watch too. Data: countryBounds.ts (Natural Earth
// 110m, public domain, simplified to ~1km). Accuracy is country-level; borders may be a hair
// off, which is fine for grouping.

import { COUNTRY_BOUNDS } from './countryBounds';

// ~0.05° buckets — nearby servers reuse a result, so 2000 lookups don't each scan the world.
const cache = new Map<string, string>();

// Ray-casting, even-odd across ALL rings of a country. A point inside a hole (e.g. Lesotho
// inside South Africa's polygon) crosses both the outer ring and the hole → even → outside,
// so holes are handled correctly regardless of country test order.
function inRings(lon: number, lat: number, rings: number[][][]): boolean {
  let inside = false;
  for (const ring of rings) {
    for (let i = 0, j = ring.length - 1; i < ring.length; j = i++) {
      const xi = ring[i][0], yi = ring[i][1];
      const xj = ring[j][0], yj = ring[j][1];
      if (((yi > lat) !== (yj > lat)) &&
          (lon < ((xj - xi) * (lat - yi)) / (yj - yi) + xi)) {
        inside = !inside;
      }
    }
  }
  return inside;
}

/** ISO alpha-2 for a coordinate, or '' if not resolvable. */
export function countryForCoord(lat?: number | null, lon?: number | null): string {
  if (lat == null || lon == null || !Number.isFinite(lat) || !Number.isFinite(lon)) return '';
  const key = `${Math.round(lat * 20)},${Math.round(lon * 20)}`;
  const hit = cache.get(key);
  if (hit !== undefined) return hit;

  let result = '';
  for (const iso in COUNTRY_BOUNDS) {
    const c = COUNTRY_BOUNDS[iso];
    const b = c.b;                                    // [minLon, minLat, maxLon, maxLat]
    if (lon < b[0] || lon > b[2] || lat < b[1] || lat > b[3]) continue;   // fast bbox reject
    if (inRings(lon, lat, c.r)) { result = iso; break; }
  }

  // NEAREST-COAST FALLBACK. The 110m borders are coarse, so a COASTAL receiver often lands
  // just OUTSIDE its country's simplified coastline (and a few tiny countries are absent at
  // this resolution) — point-in-polygon then finds nothing. Snap to the nearest country
  // vertex within a sane radius so those don't fall into "Unknown". Coastal points are a
  // fraction of a degree out; the radius stops mid-ocean coordinates snapping to something far.
  if (!result) {
    let bestD = 6.25;   // (2.5°)² — squared-degree threshold, ~275km
    for (const iso in COUNTRY_BOUNDS) {
      const c = COUNTRY_BOUNDS[iso];
      const b = c.b;
      if (lon < b[0] - 3 || lon > b[2] + 3 || lat < b[1] - 3 || lat > b[3] + 3) continue;
      for (const ring of c.r) {
        for (let i = 0; i < ring.length; i++) {
          const dx = ring[i][0] - lon, dy = ring[i][1] - lat;
          const d = dx * dx + dy * dy;
          if (d < bestD) { bestD = d; result = iso; }
        }
      }
    }
  }
  cache.set(key, result);
  return result;
}
