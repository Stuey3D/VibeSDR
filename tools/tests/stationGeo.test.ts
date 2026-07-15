// Runnable with: node tools/tests/stationGeo.test.ts   (Node >= 22 strips types)
// Verifies the geo + receivability-ranking logic (addendum §5).
import { haversineKm, boundingBox, receivabilityScore } from '../../src/services/stationGeo.ts';

let fail = 0;
const near = (name: string, got: number, want: number, tol: number) => {
  const ok = Math.abs(got - want) <= tol;
  console.log((ok ? 'ok   ' : 'FAIL ') + `${name}: ${got.toFixed(2)}` + (ok ? '' : ` want ~${want}±${tol}`));
  if (!ok) fail++;
};
const t = (name: string, cond: boolean) => { console.log((cond ? 'ok   ' : 'FAIL ') + name); if (!cond) fail++; };

// Known great-circle distances.
near('1° latitude ≈ 111 km', haversineKm(0, 0, 1, 0), 111.19, 1);
near('NYC→LA ≈ 3936 km', haversineKm(40.7128, -74.006, 34.0522, -118.2437), 3936, 30);
near('same point = 0', haversineKm(40, -74, 40, -74), 0, 0.001);

// Bounding box is a superset of the radius circle; haversine trims the corners.
const b = boundingBox(40, -74, 100);
t('bbox lon span wider than lat (cos φ)', (b.maxLon - b.minLon) > (b.maxLat - b.minLat));
t('box corner is outside the radius', haversineKm(40, -74, b.maxLat, b.maxLon) > 100);

// Ranking: a big class-C at distance beats a nearby low-power translator (§5).
const MI = 1.60934;
const bigC = receivabilityScore({ erpKw: 100, stationClass: 'C', distanceKm: 60 * MI });
const translator = receivabilityScore({ erpKw: 0.25, stationClass: null, distanceKm: 15 * MI });
t('100 kW C @60 mi beats 250 W translator @15 mi', bigC > translator);
t('closer + stronger always wins', receivabilityScore({ erpKw: 100, stationClass: 'C', distanceKm: 20 }) >
                                   receivabilityScore({ erpKw: 1, stationClass: 'A', distanceKm: 80 }));

console.log(fail === 0 ? '\nGEO: ALL PASS' : `\nGEO: ${fail} FAILURES`);
if (fail) process.exit(1);
