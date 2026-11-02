#!/usr/bin/env node
// gen-sea-areas.mjs — THE SHIPPING MAP's SEA AREAS (V12, 2026-11-02): the forecast areas' OUTLINES, a label point inside
// each, and the names a forecast heading uses — ONE dataset, written for the three readers:
//   src/utils/seaAreas.generated.ts                       — the web client (web/client imports src/utils) AND the app
//   android/app/src/main/cpp/vibe_sea_areas.generated.h   — the server's met parser (vibe_navtex_met.h areas() appends
//                                                           these to its own hand-made table: ONE table, extended)
//
// ★★ Stuart (2026-11-02): "can we add the shipping lanes and shipping zones to the map too?" … "worldwide if possible" …
//    "the things like Dogger, Wight Plymouth etc".
//
// ★★★ SOURCES — EACH ONE'S LICENCE, AND WHAT WE TOOK (the map's attribution line reads SEA_AREA_SOURCES below):
//   · UK   — Met Office shipping forecast areas (Viking … Southeast Iceland, Trafalgar). The CORNER POINTS of each area,
//            as the Met Office defines them (2002 revision): facts, typed into scripts/mapdata/sea-areas/uk-shipping.json
//            from Martin Stubbs' tabulation of them (see that file's "about"); Wikipedia used only to corroborate single
//            entries. NO drawn chart was copied. Between two corners on the same coast the boundary IS the coast: we
//            follow Natural Earth's 1:10m land outline (public domain) between them.
//   · US   — NOAA National Weather Service marine zones: coastal (mz16ap26) and offshore (oz16ap26) shapefiles,
//            weather.gov/gis/MarineZones. US Government work: PUBLIC DOMAIN. The Great Lakes zones are left out (no
//            NAVTEX, and their weight); the high-seas basins (hz) too — five ocean-sized polygons with no forecast
//            area inside them that a heading names.
//   · CA   — Environment and Climate Change Canada, MSC Geography Package 6.15.0, marine standard forecast areas
//            (water_MarStdZone_coarse), dd.weather.gc.ca/today/meteocode/geodata/. OPEN GOVERNMENT LICENCE – CANADA
//            (the MSC data servers' end-use licence): "Contains information licensed under the Open Government Licence
//            – Canada." Atlantic, Pacific and Arctic areas; the Great Lakes and inland waters left out.
//   · DE   — Deutscher Wetterdienst coastal warning areas (dwd:Warngebiete_Kueste, maps.dwd.de GeoServer WFS).
//            DWD open data under the GeoNutzV (Datenlizenz Deutschland – Namensnennung 2.0); geometry
//            "© GeoBasis-DE / BKG 2021 (Daten modifiziert)". The Elbe river stretch is left out.
//   ✗ NOT INCLUDED, and why (docs/v12/SHIPPING-MAP.md keeps this list):
//     · UK inshore waters stretches, Irish coastal areas (Met Éireann) — defined as headland-to-headland strips out to
//       12 / 10 NM; we have the headlands but not a buffer we trust yet. Their LABEL POINTS stay in vibe_navtex_met.h.
//     · Australia (BoM IDM00001/IDM00003) — the files carry no Creative Commons statement, and the Bureau's default
//       terms forbid supplying them to others or commercial use. Not used.
//     · Norway (MET Norway), the Netherlands (KNMI), France (Météo-France), DWD's offshore Seegebiete (Deutsche Bucht,
//       Westliche Ostsee …) — no open boundary set or published corner list found (2026-11-02).
//
// ★ SIZE: polygons simplified (Douglas–Peucker, per kind) and stored as delta-encoded polylines at 0.001°; the
//   generator prints the sizes. Holes and specks are dropped (an outline, not a land mask).
//
// usage: node scripts/gen-sea-areas.mjs [--src <dir with the downloads>]        write the generated files
//        node scripts/gen-sea-areas.mjs --check                                 exit 1 if they are stale (no downloads
//                                                                               needed: it re-reads the committed cache)
//   The downloads (scripts/mapdata/sea-areas/fetch.sh lists them) are NOT committed; the generator writes
//   scripts/mapdata/sea-areas/sea-areas.cache.json — the processed polygons, committed — so --check and a rebuild of the
//   readers never need the network.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { readShapefile } from './mapdata/sea-areas/shp.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const DIR = path.join(root, 'scripts/mapdata/sea-areas');
const CACHE = path.join(DIR, 'sea-areas.cache.json');
const OUT_TS = path.join(root, 'src/utils/seaAreas.generated.ts');
const OUT_H = path.join(root, 'android/app/src/main/cpp/vibe_sea_areas.generated.h');
const arg = (k) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : null; };
const CHECK = process.argv.includes('--check');

export const SOURCES = {
  uk: { name: 'Met Office', what: 'UK shipping forecast areas (boundary points; coast from Natural Earth)', licence: 'facts (corner points); Natural Earth public domain', attribution: 'Sea areas: Met Office definitions' },
  us: { name: 'NOAA NWS', what: 'US coastal and offshore marine zones', licence: 'Public domain (US Government)', attribution: 'NOAA NWS marine zones' },
  ca: { name: 'ECCC', what: 'Canadian marine forecast areas', licence: 'Open Government Licence – Canada', attribution: 'Contains information licensed under the Open Government Licence – Canada (ECCC)' },
  de: { name: 'DWD', what: 'German coastal warning areas', licence: 'GeoNutzV / Datenlizenz Deutschland – Namensnennung 2.0', attribution: 'Deutscher Wetterdienst, © GeoBasis-DE / BKG 2021 (Daten modifiziert)' },
};

// ── geometry ──────────────────────────────────────────────────────────────────────────────────────────────────────
const R = 6371;
const toRad = Math.PI / 180;
function km(a, b) { // a, b: [lon, lat]
  const dLat = (b[1] - a[1]) * toRad, dLon = (b[0] - a[0]) * toRad;
  const s = Math.sin(dLat / 2) ** 2 + Math.cos(a[1] * toRad) * Math.cos(b[1] * toRad) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(s));
}
function signedArea(r) { let s = 0; for (let i = 0, j = r.length - 1; i < r.length; j = i++) s += (r[j][0] - r[i][0]) * (r[j][1] + r[i][1]); return s / 2; }
function bbox(r) { let a = 180, b = 90, c = -180, d = -90; for (const p of r) { if (p[0] < a) a = p[0]; if (p[1] < b) b = p[1]; if (p[0] > c) c = p[0]; if (p[1] > d) d = p[1]; } return [a, b, c, d]; }
/** Douglas–Peucker, iterative (rings of 100 000 points). */
function simplify(pts, tol) {
  if (pts.length < 4) return pts.slice();
  const keep = new Uint8Array(pts.length); keep[0] = keep[pts.length - 1] = 1;
  const st = [[0, pts.length - 1]];
  const t2 = tol * tol;
  while (st.length) {
    const [a, b] = st.pop();
    let md = -1, mi = -1;
    const [ax, ay] = pts[a], [bx, by] = pts[b];
    const dx = bx - ax, dy = by - ay, L = dx * dx + dy * dy;
    for (let i = a + 1; i < b; i++) {
      const [px, py] = pts[i];
      let d;
      if (L === 0) d = (px - ax) ** 2 + (py - ay) ** 2;
      else { let t = ((px - ax) * dx + (py - ay) * dy) / L; t = Math.max(0, Math.min(1, t)); d = (px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2; }
      if (d > md) { md = d; mi = i; }
    }
    if (md > t2) { keep[mi] = 1; st.push([a, mi], [mi, b]); }
  }
  return pts.filter((_, i) => keep[i]);
}
function simplifyRing(r, tol) {
  // split at the point farthest from the first so a closed ring keeps its shape
  let far = 0, fd = -1; for (let i = 1; i < r.length; i++) { const d = (r[i][0] - r[0][0]) ** 2 + (r[i][1] - r[0][1]) ** 2; if (d > fd) { fd = d; far = i; } }
  const a = simplify(r.slice(0, far + 1), tol), b = simplify(r.slice(far).concat([r[0]]), tol);
  const out = a.concat(b.slice(1, -1));
  return out.length >= 3 ? out : null;
}
export function pointInRing(p, r) {
  let inside = false;
  for (let i = 0, j = r.length - 1; i < r.length; j = i++) {
    const xi = r[i][0], yi = r[i][1], xj = r[j][0], yj = r[j][1];
    if (((yi > p[1]) !== (yj > p[1])) && p[0] < ((xj - xi) * (p[1] - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}
function segDist2(p, a, b) {
  const dx = b[0] - a[0], dy = b[1] - a[1], L = dx * dx + dy * dy;
  let t = L ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L : 0; t = Math.max(0, Math.min(1, t));
  return (p[0] - a[0] - t * dx) ** 2 + (p[1] - a[1] - t * dy) ** 2;
}
/** ★ A label point INSIDE the polygon, as far from its edges as a coarse search finds (the pole of inaccessibility,
 *  near enough) — in the largest ring. Longitude is scaled by cos(lat) so "far" means far on the map. */
function labelPoint(rings) {
  let best = rings[0], ba = 0;
  for (const r of rings) { const a = Math.abs(signedArea(r)); if (a > ba) { ba = a; best = r; } }
  const [x0, y0, x1, y1] = bbox(best);
  const k = Math.cos(((y0 + y1) / 2) * toRad);
  let bp = null, bd = -1;
  const N = 40;
  for (let i = 0; i <= N; i++) for (let j = 0; j <= N; j++) {
    const p = [x0 + ((x1 - x0) * (i + 0.5)) / (N + 1), y0 + ((y1 - y0) * (j + 0.5)) / (N + 1)];
    if (!pointInRing(p, best)) continue;
    let d = Infinity;
    for (let m = 0, n = best.length - 1; m < best.length; n = m++) {
      const a = [best[n][0] * k, best[n][1]], b = [best[m][0] * k, best[m][1]];
      d = Math.min(d, segDist2([p[0] * k, p[1]], a, b));
      if (d < bd) break;
    }
    if (d > bd) { bd = d; bp = p; }
  }
  if (!bp) { const c = best.reduce((a, p) => [a[0] + p[0] / best.length, a[1] + p[1] / best.length], [0, 0]); bp = c; }
  return [Math.round(bp[0] * 1000) / 1000, Math.round(bp[1] * 1000) / 1000];
}
// ── the polyline encoding (Google's algorithm, at 0.001°) ─────────────────────────────────────────────────────────
function encNum(v, out) { v = v < 0 ? ~(v << 1) : v << 1; while (v >= 0x20) { out.push(String.fromCharCode((0x20 | (v & 0x1f)) + 63)); v >>= 5; } out.push(String.fromCharCode(v + 63)); }
export function encodeRing(r) {
  const out = []; let px = 0, py = 0;
  for (const [x, y] of r) { const X = Math.round(x * 1000), Y = Math.round(y * 1000); if (X === px && Y === py && out.length) continue; encNum(Y - py, out); encNum(X - px, out); px = X; py = Y; }
  return out.join('');
}
export function decodeRing(s) {
  const out = []; let i = 0, lat = 0, lon = 0;
  const one = () => { let r = 0, sh = 0, b; do { b = s.charCodeAt(i++) - 63; r |= (b & 0x1f) << sh; sh += 5; } while (b >= 0x20); return r & 1 ? ~(r >> 1) : r >> 1; };
  while (i < s.length) { lat += one(); lon += one(); out.push([lon / 1000, lat / 1000]); }
  return out;
}

// ── the sources ───────────────────────────────────────────────────────────────────────────────────────────────────
function dms(s) {
  const m = /^(\d+)°(\d+)'([NS])\s+(\d+)°(\d+)'([EW]?)$/.exec(s.trim());
  if (!m) throw new Error('bad corner: ' + s);
  const lat = (+m[1] + +m[2] / 60) * (m[3] === 'S' ? -1 : 1), lon = (+m[4] + +m[5] / 60) * (m[6] === 'W' ? -1 : 1);
  return [lon, lat];
}
function loadLand(src) {
  const f = readShapefile(path.join(src, 'ne/land/ne_10m_land'));
  const rings = [];
  for (const x of f) for (const r of x.rings) if (r.length >= 8) rings.push({ r, bb: bbox(r) });
  return rings;
}
/** ★ The UK areas: the corners, the coast between two corners on the same land. */
function buildUk(land) {
  const def = JSON.parse(fs.readFileSync(path.join(DIR, 'uk-shipping.json'), 'utf8'));
  const SNAP_KM = 25;
  const snap = (p) => {
    // ★ the biggest land within reach wins (the mainland over a skerry beside the corner)
    let best = null;
    for (const L of land) {
      const [a, b, c, d] = L.bb;
      if (p[0] < a - 0.6 || p[0] > c + 0.6 || p[1] < b - 0.4 || p[1] > d + 0.4) continue;
      let bi = -1, bd = Infinity;
      for (let i = 0; i < L.r.length; i++) { const q = L.r[i]; if (Math.abs(q[1] - p[1]) > 0.4 || Math.abs(q[0] - p[0]) > 0.8) continue; const dk = km(p, q); if (dk < bd) { bd = dk; bi = i; } }
      if (bi >= 0 && bd <= SNAP_KM && (!best || L.r.length > best.L.r.length)) best = { L, i: bi, d: bd };
    }
    return best;
  };
  const out = [];
  for (const a of def.areas) {
    // ★ a 'via' crossing is two more corners (each snaps to its own coast, so the coast is followed on both sides)
    const pts = a.via ? [...a.pts.slice(0, a.via.after + 1), ...a.via.pts, ...a.pts.slice(a.via.after + 1)] : a.pts;
    const corners = pts.map(dms);
    const snaps = corners.map(snap);
    const ring = [];
    const n = corners.length;
    for (let k = 0; k < n; k++) {
      const p = corners[k], q = corners[(k + 1) % n], sp = snaps[k], sq = snaps[(k + 1) % n];
      ring.push(p);
      // ★ the crossing itself is a straight line (Africa and Eurasia are ONE ring in Natural Earth — "the coast between"
      //   Cape Spartel and Tarifa would be the whole Mediterranean)
      const crossing = a.via && k === a.via.after + 1;
      if (!crossing && sp && sq && sp.L === sq.L) {
        // ★ the coast between them: the shorter way round that land
        const N = sp.L.r.length, i = sp.i, j = sq.i;
        const fwd = (j - i + N) % N, back = (i - j + N) % N;
        if (fwd <= back) for (let s = 1; s < fwd; s++) ring.push(sp.L.r[(i + s) % N]);
        else for (let s = 1; s < back; s++) ring.push(sp.L.r[(i - s + N) % N]);
      }
    }
    out.push({ id: a.id, src: 'uk', kind: 's', rings: [ring] });
  }
  return out;
}
const TITLE = (s) => s.replace(/\s+/g, ' ').trim().replace(/[.,;:]+$/, '');
function outerRings(rings) {
  // shapefile outer rings run clockwise (negative signed area here); holes are dropped
  const o = rings.filter((r) => signedArea(r) < 0);
  return o.length ? o : rings;
}
function buildUs(src) {
  const out = [];
  const GL = /^(LSZ|LMZ|LHZ|LEZ|LOZ|LCZ|SLZ)/;
  for (const [file, kind] of [['nws/mz/mz16ap26', 'c'], ['nws/oz/oz16ap26', 'o']]) {
    for (const x of readShapefile(path.join(src, file))) {
      const id = String(x.props.ID || '').toUpperCase();
      if (!/^[A-Z]{3}\d{3}$/.test(id) || GL.test(id)) continue;
      out.push({ id: 'us-' + id.toLowerCase(), ugc: id, src: 'us', kind, name: TITLE(x.props.NAME || id), rings: outerRings(x.rings) });
    }
  }
  return out;
}
function buildCa(src) {
  const out = [];
  for (const x of readShapefile(path.join(src, 'eccc/w/water_MarStdZone_coarse_unproj'))) {
    if (!['AT', 'PC', 'AC'].includes(x.props.WATRBODY_C)) continue;
    const name = TITLE(x.props.NAME || '');
    if (!name || /^C[A-Z]\d$/.test(name)) continue;   // ★ CX4, CW5: grid codes, not names a forecast says
    out.push({ id: 'ca-' + x.props.CLC, src: 'ca', kind: 'o', name, fr: TITLE(x.props.NOM || ''), rings: outerRings(x.rings) });
  }
  return out;
}
function buildDe(src) {
  const j = JSON.parse(fs.readFileSync(path.join(src, 'dwd/kueste.json'), 'utf8'));
  const out = [];
  for (const f of j.features) {
    const p = f.properties; if (!p || /^Elbe von/.test(p.NAME)) continue;
    const g = f.geometry; const polys = g.type === 'MultiPolygon' ? g.coordinates : [g.coordinates];
    // GeoJSON: outer ring first, counter-clockwise — keep the outer rings
    out.push({ id: 'de-' + String(p.CCC).toLowerCase(), src: 'de', kind: 'c', name: TITLE(p.NAME), rings: polys.map((pp) => pp[0]) });
  }
  return out;
}

// ── the names a heading uses (the met parser's "match") ──────────────────────────────────────────────────────────
const ASCII = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/ß/g, 'SS').toUpperCase().replace(/['`’]/g, '').replace(/\s+/g, ' ').trim();
// ★ too generic to be a heading on its own anywhere in the world (a line "CENTRAL" or "EAST COAST" elsewhere must not
//   land in Canada)
const GENERIC = new Set(['CENTRAL', 'EAST COAST', 'SOUTH COAST', 'NORTH COAST', 'WEST COAST', 'NORTHEAST COAST', 'SOUTHWEST COAST', 'COATS', 'BAKER', 'SABLE']);
function matchOf(a) {
  const m = new Set();
  const add = (s) => { const u = ASCII(s); if (u.length >= 4 && !GENERIC.has(u) && !/^\d/.test(u)) m.add(u); };
  if (a.src === 'ca') { add(a.name); add(a.name.replace(/\s+-\s+/g, ' ')); }
  if (a.src === 'us') {
    // ★ the 2025 rename: a bulletin (or a saved message) from before it says GULF OF MEXICO
    for (const n of [a.name, a.name.replace(/Gulf of America/gi, 'Gulf of Mexico')]) { add(n); add(n.replace(/\s+out\s+\d+\s+NM$/i, '')); }
  }
  if (a.src === 'de') {
    add(a.name); add(a.name.replace(/Küste/, 'Kueste'));
    const EN = { nof: 'NORTH FRISIAN COAST', osf: 'EAST FRISIAN COAST', hel: 'HELGOLAND|HELIGOLAND', elm: 'ELBE AND WESER ESTUARIES|ELBE-/WESERMUENDUNG|ELBE- UND WESERMUENDUNG', flf: 'FLENSBURG TO FEHMARN', fer: 'EAST OF FEHMARN TO RUEGEN|EAST OF FEHMARN TO RUGEN', osr: 'EAST OF RUEGEN|EAST OF RUGEN' };
    const e = EN[a.id.slice(3)]; if (e) for (const s of e.split('|')) add(s);
  }
  return [...m];
}

// ── build ────────────────────────────────────────────────────────────────────────────────────────────────────────
const TOL = { s: 0.02, o: 0.02, c: 0.012, i: 0.01, n: 0.02 };
const MIN_AREA = { s: 0, o: 0.004, c: 0.002, i: 0, n: 0 };   // ★ specks (square degrees) dropped — never a whole area
function build(src) {
  const land = loadLand(src);
  const raw = [...buildUk(land), ...buildUs(src), ...buildCa(src), ...buildDe(src)];
  const areas = [];
  for (const a of raw) {
    let rings = a.rings.map((r) => simplifyRing(r, TOL[a.kind])).filter(Boolean);
    const big = rings.filter((r) => Math.abs(signedArea(r)) >= MIN_AREA[a.kind]);
    if (big.length) rings = big;
    if (!rings.length) continue;
    const [lon, lat] = labelPoint(rings);
    areas.push({ id: a.id, src: a.src, kind: a.kind, name: a.name || null, ugc: a.ugc || null, lat, lon, rings: rings.map(encodeRing), match: a.src === 'uk' ? [] : matchOf(a) });
  }
  return areas;
}
function stats(areas) {
  const by = {};
  for (const a of areas) { const k = a.src; by[k] = by[k] || { n: 0, bytes: 0 }; by[k].n++; by[k].bytes += a.rings.reduce((s, r) => s + r.length, 0); }
  return by;
}

// ── the readers ──────────────────────────────────────────────────────────────────────────────────────────────────
function writeTs(areas) {
  const rows = areas.map((a) => JSON.stringify([a.id, a.kind, a.src, a.name || '', a.lat, a.lon, a.rings.join(' ')]));
  return `// seaAreas.generated.ts — GENERATED by scripts/gen-sea-areas.mjs from scripts/mapdata/sea-areas/. Do not edit.
// The SHIPPING MAP's SEA AREAS: [id, kind, source, name, labelLat, labelLon, rings] — kind 's' UK shipping forecast area,
// 'o' offshore / marine forecast area, 'c' coastal zone; rings: polylines at 0.001° (Google's encoding), space-separated.
// UK areas carry no name here: the name is the met table's (vibe_navtex_met.h), joined by id in services/seaAreas.ts.
/* eslint-disable */
export const SEA_AREA_SOURCES: Record<string, { name: string; what: string; licence: string; attribution: string }> = ${JSON.stringify(SOURCES)};
export const SEA_AREA_ROWS: ReadonlyArray<[string, string, string, string, number, number, string]> = [
${rows.join(',\n')}
];
`;
}
function cstr(s) { return '"' + s.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"'; }
function writeH(areas) {
  const rows = areas.filter((a) => a.src !== 'uk' && (a.match.length || a.ugc)).map((a) => {
    const match = a.match.join('|');
    return `        {${cstr(a.id)}, ${cstr(ASCII(a.name).length ? a.name.normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/ß/g, 'ss') : a.id)}, ${a.lat.toFixed(3)}, ${a.lon.toFixed(3)}, '${a.kind === 'c' ? 'c' : 'o'}', ${cstr(match)}, ${cstr(a.ugc || '')}},`;
  });
  return `// vibe_sea_areas.generated.h — GENERATED by scripts/gen-sea-areas.mjs from scripts/mapdata/sea-areas/. Do not edit.
// The SEA AREAS beyond the UK that a forecast heading can name — appended to vibe_navtex_met.h areas() (ONE table).
// {id, name, label lat, label lon, kind ('o' offshore / marine area, 'c' coastal zone), match ('|'-separated uppercase
// forms), UGC code (US: the "ANZ800-" line of an NWS bulletin names the zone)}. Sources and licences: the generator's
// header (NOAA NWS public domain · ECCC Open Government Licence – Canada · DWD GeoNutzV).
#pragma once
namespace vibe {
namespace navmet {
struct GenArea { const char* id; const char* name; double lat, lon; char kind; const char* match; const char* ugc; };
inline const GenArea* generatedAreas(size_t& n) {
    static const GenArea a[] = {
${rows.join('\n')}
    };
    n = sizeof a / sizeof a[0];
    return a;
}
} // namespace navmet
} // namespace vibe
`;
}

function main() {
  let areas;
  const src = arg('--src');
  if (src) {
    areas = build(src);
    fs.writeFileSync(CACHE, JSON.stringify({ generated: new Date().toISOString().slice(0, 10), areas }, null, 0).replace(/\},\{"id"/g, '},\n{"id"') + '\n');
  } else {
    if (!fs.existsSync(CACHE)) { console.error('no cache: run with --src <downloads> first (scripts/mapdata/sea-areas/fetch.sh)'); process.exit(1); }
    areas = JSON.parse(fs.readFileSync(CACHE, 'utf8')).areas;
  }
  // ★ the same heading form for two areas: the first source keeps it (UK > CA > US > DE), the other drops it
  const seen = new Map();
  for (const a of areas) a.match = a.match.filter((m) => { if (seen.has(m)) { console.warn(`  name clash "${m}": ${seen.get(m)} keeps it, ${a.id} drops it`); return false; } seen.set(m, a.id); return true; });
  const ts = writeTs(areas), h = writeH(areas);
  if (CHECK) {
    const stale = [[OUT_TS, ts], [OUT_H, h]].filter(([f, c]) => !fs.existsSync(f) || fs.readFileSync(f, 'utf8') !== c);
    if (stale.length) { console.error('stale: ' + stale.map(([f]) => path.relative(root, f)).join(', ') + ' — run node scripts/gen-sea-areas.mjs'); process.exit(1); }
    console.log('sea areas: generated files are current');
    return;
  }
  fs.writeFileSync(OUT_TS, ts); fs.writeFileSync(OUT_H, h);
  const st = stats(areas);
  for (const k of Object.keys(st)) console.log(`  ${k}: ${st[k].n} areas, ${(st[k].bytes / 1024).toFixed(1)} KB of outline`);
  console.log(`  ${path.relative(root, OUT_TS)} ${(ts.length / 1024).toFixed(1)} KB · ${path.relative(root, OUT_H)} ${(h.length / 1024).toFixed(1)} KB`);
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
