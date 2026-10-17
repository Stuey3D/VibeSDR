/**
 * ★★ THE LANGUAGES AROUND A RECEIVER — TRANSCRIBE's AUTO hint off the broadcast bands (Stuart, 2026-10-15: "try the
 * 4 most likely languages based on EiBi for broadcast stations and receiver location for all others, for instance a
 * receiver in the UK will maybe try French/Dutch/German/English").
 *
 * Offline and coarse on purpose: country centres with the languages heard on air there, nearest first. A receiver
 * hears its NEIGHBOURS too (a UK receiver hears French and Dutch ships, Belgian amateurs, German utility traffic), so
 * nearest-centre is the right shape — a border is not a wall. Only languages the phones can transcribe are listed;
 * the caller drops any this device lacks before taking four.
 */

// [ISO country, lat, lon, languages in the order they are likely on air]
const ANCHORS: [string, number, number, string[]][] = [
  ['GB', 54.0, -2.0, ['en']], ['IE', 53.2, -8.0, ['en']], ['FR', 46.6, 2.4, ['fr']], ['BE', 50.6, 4.6, ['nl', 'fr']],
  ['NL', 52.2, 5.3, ['nl']], ['LU', 49.8, 6.1, ['fr', 'de']], ['DE', 51.1, 10.4, ['de']],
  ['CH', 46.8, 8.2, ['de', 'fr', 'it']], ['AT', 47.6, 14.1, ['de']], ['IT', 42.8, 12.5, ['it']],
  ['ES', 40.2, -3.6, ['es', 'ca']], ['PT', 39.6, -8.0, ['pt']], ['DK', 56.0, 10.0, ['da']], ['NO', 61.0, 9.0, ['nb']],
  ['SE', 62.0, 15.0, ['sv']], ['FI', 64.0, 26.0, ['fi', 'sv']], ['PL', 52.0, 19.4, ['pl']], ['CZ', 49.8, 15.5, ['cs']],
  ['SK', 48.7, 19.7, ['sk']], ['HU', 47.2, 19.4, ['hu']], ['RO', 45.9, 25.0, ['ro']], ['GR', 39.1, 22.0, ['el']],
  ['HR', 45.1, 15.2, ['hr']], ['UA', 49.0, 31.4, ['uk', 'ru']], ['BY', 53.7, 28.0, ['ru']], ['RU', 56.0, 38.0, ['ru']],
  ['RU', 60.0, 100.0, ['ru']], ['TR', 39.0, 35.2, ['tr']], ['CY', 35.0, 33.0, ['el', 'tr']], ['IL', 31.4, 35.0, ['he', 'ar']],
  ['EG', 26.8, 30.8, ['ar']], ['SA', 24.0, 45.0, ['ar']], ['MA', 31.8, -7.0, ['ar', 'fr']], ['DZ', 28.0, 2.6, ['ar', 'fr']],
  ['TN', 34.0, 9.5, ['ar', 'fr']], ['IN', 21.0, 78.0, ['hi', 'en']], ['PK', 30.4, 69.3, ['ur', 'en']], ['BD', 23.7, 90.3, ['bn']],
  ['CN', 35.9, 104.2, ['zh']], ['HK', 22.3, 114.2, ['yue', 'zh', 'en']], ['TW', 23.7, 121.0, ['zh']], ['JP', 36.2, 138.3, ['ja']],
  ['KR', 36.5, 127.9, ['ko']], ['KP', 40.3, 127.5, ['ko']], ['VN', 14.0, 108.0, ['vi']], ['TH', 15.9, 101.0, ['th']],
  ['MY', 4.2, 101.9, ['ms', 'en', 'zh']], ['ID', -2.5, 118.0, ['id']], ['PH', 12.9, 121.8, ['en']], ['SG', 1.35, 103.8, ['en', 'zh', 'ms']],
  ['AU', -25.3, 133.8, ['en']], ['NZ', -41.0, 174.0, ['en']], ['US', 39.8, -98.6, ['en', 'es']], ['CA', 56.0, -106.0, ['en', 'fr']],
  ['CA', 52.0, -72.0, ['fr', 'en']], ['MX', 23.6, -102.5, ['es']], ['BR', -14.2, -51.9, ['pt']], ['AR', -38.4, -63.6, ['es']],
  ['CL', -35.7, -71.5, ['es']], ['CO', 4.6, -74.3, ['es']], ['PE', -9.2, -75.0, ['es']], ['VE', 6.4, -66.6, ['es']],
  ['CU', 21.5, -77.8, ['es']], ['ZA', -30.6, 22.9, ['en']], ['NG', 9.0, 8.7, ['en']], ['KE', 0.0, 37.9, ['en']],
];

function km(aLat: number, aLon: number, bLat: number, bLon: number): number {
  const r = Math.PI / 180;
  const dLat = (bLat - aLat) * r, dLon = (bLon - aLon) * r;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(aLat * r) * Math.cos(bLat * r) * Math.sin(dLon / 2) ** 2;
  return 12742 * Math.asin(Math.min(1, Math.sqrt(h)));
}

/** Up to `n` languages likely on air around a receiver, nearest country first, keeping only those `supported`.
 *  No receiver position (a local dongle that never said) → the phone's own country stands in for it. */
export function languagesNear(loc: { lat: number; lon: number } | null, supported: (code: string) => boolean, n = 4): string[] {
  const near = nearest(loc);
  const out: string[] = [];
  for (const { a } of near) {
    for (const c of a[3]) if (!out.includes(c) && supported(c)) out.push(c);
    if (out.length >= n) break;
  }
  return out.slice(0, n);
}

/** The anchors nearest first; no receiver position → the phone's own country stands in for it ([] if unknown). */
function nearest(loc: { lat: number; lon: number } | null): { a: [string, number, number, string[]]; d: number }[] {
  let at = loc;
  if (!at) {
    const region = ((Intl as any)?.DateTimeFormat?.().resolvedOptions?.().locale ?? '').split('-').pop()?.toUpperCase();
    const a = ANCHORS.find(x => x[0] === region);
    if (!a) return [];
    at = { lat: a[1], lon: a[2] };
  }
  return ANCHORS.map(a => ({ a, d: km(at!.lat, at!.lon, a[1], a[2]) })).sort((x, y) => x.d - y.d);
}

/** ★ The receiver's COUNTRY ('BR', 'GB') — the nearest anchor — so its main language can be tried in the variant
 *  spoken there (pt-BR for Kiko's receiver, en-GB for a UK one). Same stand-in as languagesNear; '' if unknown. */
export function countryNear(loc: { lat: number; lon: number } | null): string {
  return nearest(loc)[0]?.a[0] ?? '';
}
