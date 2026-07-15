/**
 * Pure geo + receivability helpers for the "stations near me" query (addendum §5).
 * No I/O — unit-testable in isolation. The DB layer uses boundingBox() for a
 * cheap indexed prefilter, then haversineKm() for true distance, then
 * receivabilityScore() to rank by "can I actually hear it", not raw distance.
 */

const EARTH_R_KM = 6371.0088;
const KM_PER_DEG_LAT = 111.32;
const D2R = Math.PI / 180;

/** Great-circle distance in km. */
export function haversineKm(
  lat1: number, lon1: number, lat2: number, lon2: number,
): number {
  const dLat = (lat2 - lat1) * D2R;
  const dLon = (lon2 - lon1) * D2R;
  const a =
    Math.sin(dLat / 2) ** 2 +
    Math.cos(lat1 * D2R) * Math.cos(lat2 * D2R) * Math.sin(dLon / 2) ** 2;
  return 2 * EARTH_R_KM * Math.asin(Math.min(1, Math.sqrt(a)));
}

export interface BBox { minLat: number; maxLat: number; minLon: number; maxLon: number; }

/**
 * Lat/lon box enclosing a radius (km) around a point — for the SQL prefilter
 * `lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?` on the indexed columns. A box is
 * a superset of the circle; haversine then trims the corners.
 */
export function boundingBox(lat: number, lon: number, radiusKm: number): BBox {
  const dLat = radiusKm / KM_PER_DEG_LAT;
  // Guard the cosine near the poles (irrelevant for the US, but keep it safe).
  const cos = Math.max(0.01, Math.cos(lat * D2R));
  const dLon = radiusKm / (KM_PER_DEG_LAT * cos);
  return {
    minLat: lat - dLat, maxLat: lat + dLat,
    minLon: lon - dLon, maxLon: lon + dLon,
  };
}

/** Small additive bonus by FM station class (higher class → more reach). */
export function classBonus(stationClass?: string | null): number {
  switch ((stationClass ?? '').toUpperCase()) {
    case 'C': case 'C0': return 6;
    case 'C1': return 4.5;
    case 'C2': case 'B': return 3;
    case 'C3': case 'B1': return 1.5;
    case 'A': return 0;
    default: return 0; // LPFM / translators / unknown carry no bonus (ERP already low)
  }
}

export interface ReceivabilityInput {
  erpKw?: number | null;
  stationClass?: string | null;
  distanceKm: number;
}

/**
 * Relative "likely receivability" score — HIGHER is better. Not a real field
 * prediction: a free-space proxy in dB, 10·log10(ERP) − 20·log10(distance),
 * plus a small class bonus. That ordering makes a 100 kW class C at 60 mi beat a
 * 250 W translator at 15 mi (addendum §5). Tune empirically; do not chase
 * perfection. A v2 could use the FCC FM service-contour points instead.
 */
export function receivabilityScore(inp: ReceivabilityInput): number {
  const erpKw = Math.max(0.0001, inp.erpKw ?? 0.05); // floor; assume tiny if unknown
  const distKm = Math.max(1, inp.distanceKm); // avoid log(0) right on top of a TX
  return 10 * Math.log10(erpKw) - 20 * Math.log10(distKm) + classBonus(inp.stationClass);
}
