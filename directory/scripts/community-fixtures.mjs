// community-fixtures.mjs — fake contributing servers for the community map's tests, in the SERVER'S OWN v2 FORMAT.
//
// ★ The two bodies in testdata/community/ are REAL server output, not hand-written:
//   · real-server-stored.json — what a vibeserver built from b94-commap answered at /vibeserver/community.json from what it
//     STORES, with no decoder running (scripts/test-community-server.mjs): 61 real Lenovo NAVTEX messages (stations,
//     warnings with their areas, forecasts, messages), a DSC distress call, the WOD NDB, two MeshCore nodes, a remembered
//     APRS station and sonde flight;
//   · real-shim-aircraft.json — three aircraft rows as vibe_community.h builds them (vibeserver/test-community.cpp).
//   The other receivers here are built from those, with overlaps for the merge (the same aircraft, DSC call, NAVTEX warning
//   and message from two receivers; a lossier copy that must lose to the cleanest) and ships / HFDL / APRS of their own.
// ★ rebase(): every absolute time (a number between 2023 and 2036 in UTC seconds) moves to today.
import { readFileSync } from 'node:fs';

const td = (f) => JSON.parse(readFileSync(new URL(`testdata/community/${f}`, import.meta.url), 'utf8'));

export function rebase(j, nowSec) {
  const delta = nowSec - j.now;
  const walk = (o) => {
    if (Array.isArray(o)) { for (let i = 0; i < o.length; i++) { if (typeof o[i] === 'number' && o[i] > 1.67e9 && o[i] < 2.1e9) o[i] += delta; else walk(o[i]); } return; }
    if (!o || typeof o !== 'object') return;
    for (const k of Object.keys(o)) { if (typeof o[k] === 'number' && o[k] > 1.67e9 && o[k] < 2.1e9) o[k] += delta; else walk(o[k]); }
  };
  const c = JSON.parse(JSON.stringify(j));
  walk(c);
  return c;
}
const clone = (x) => JSON.parse(JSON.stringify(x));

/** The receivers' answers, keyed by id. With `n` > 3, more receivers (copies of the three, moved about) for a load test. */
export function answers(n = 3) {
  const lenovo = td('real-server-stored.json');
  const air = td('real-shim-aircraft.json');
  const now = lenovo.now;
  // pi500: the three real aircraft + the SAME DSC call (40 s later) + the Lenovo's newest NAVTEX message again, a LOSSIER copy
  const nx0 = lenovo.nx[0];
  const pi500 = { v: 2, on: true, now, layers: ['adsb', 'dsc', 'navtex'], a: clone(air.a),
    d: lenovo.d.map((r) => [r[0] + 40, r[1], r[2], r[3], r[4], 'pi500-dsc.txt']),
    nx: [[nx0[0], nx0[1], nx0[2], nx0[3], 1, 7, nx0[6], nx0[7], nx0[8], 'pi500-copy.txt', nx0[10]]],
    nw: { now, warnings: [clone(lenovo.nw.warnings[0])] } };
  pi500.nw.warnings[0].f = 'pi500-warn.txt';
  pi500.a[0][3] = 5;   // ★ BAW696 heard here 5 s before the XCover heard it
  // xcover: BAW696 again (4CA000, newer) + an HFDL aircraft over the Atlantic + two ships off the Solent + an APRS car
  const xcover = { v: 2, on: true, now, layers: ['adsb', 'hfdl', 'ais', 'aprs'],
    a: [['4CA000', 52.02, 0.05, 0, 35100, 92, 441, 1, 'BAW696', '', ''], ['A1B2C3', 54.5, -30.2, 240, 37000, null, null, 8 | 16, 'UAL917', 'N12345', 'B77W']],
    s: [[235000001, 50.79, -1.31, now - 25, 12.4, 181, 'passenger', 'A', 'RED FALCON', 'MXRF'], [232001234, 50.7, -1.1, now - 600, 5.1, 90, 'leisure', 'B', 'SEA BREEZE', '']],
    p: [['M0XYZ-7', 'M0XYZ-7', 50.9, -1.4, now - 120, '/>', 'position', 'M0XYZ-7: car, 48 km/h'], ['G4ABC-9', 'G4ABC-9', 52.21, -1.31, now - 60, '/>', 'position', 'car 50 km/h']] };
  const out = { lenovo, pi500, xcover };
  for (let i = 3; i < n; i++) {   // ★ a load test: more receivers, each a copy moved east, with its own keys
    const src = clone([lenovo, pi500, xcover][i % 3]), dx = 2 + i;
    const mv = (lat, lon) => [lat + (i % 5) * 0.3, lon + dx];
    for (const r of src.a || []) { [r[1], r[2]] = mv(r[1], r[2]); r[0] = (parseInt(r[0], 16) + i * 4096).toString(16).toUpperCase().padStart(6, '0').slice(-6); }
    for (const r of src.s || []) { [r[1], r[2]] = mv(r[1], r[2]); r[0] += i; }
    for (const r of src.p || []) { [r[2], r[3]] = mv(r[2], r[3]); r[0] += '-' + i; }
    for (const r of src.m || []) { [r[3], r[4]] = mv(r[3], r[4]); r[0] += i; }
    out['rx' + i] = src;
  }
  return out;
}

/** The contributors list as /api/community answers it (contributorOf's shape), aimed at `urlOf(id)`. */
export function contributors(urlOf, n = 3) {
  const one = (id, name, grid, lat, lon, country, layers, extra = {}) => ({ id, name, country, slug: id, address: `${id}.vibeserver.vibesdr.net`, url: urlOf(id),
    grid, lat, lon, layers, listeners: 0, maxListeners: 4, pin: false, temporary: false, updatedAt: 0, ...extra });
  const servers = [
    one('lenovo', 'Stuart’s Lenovo (NAVTEX + NDB)', 'IO92', 52.5, -1, 'GB', ['ndb', 'dsc', 'navtex', 'sondes', 'aprs', 'mesh'], { listeners: 1 }),
    one('pi500', 'Pi 500 — RSP1B + V4', 'IO91', 51.5, -1, 'GB', ['adsb', 'dsc', 'navtex'], { listeners: 4 }),
    one('xcover', 'XCover portable', 'IO90', 50.5, -1, 'GB', ['adsb', 'hfdl', 'ais', 'aprs']),
  ];
  for (let i = 3; i < n; i++) { const b = servers[i % 3]; servers.push({ ...b, id: 'rx' + i, slug: 'rx' + i, address: `rx${i}.vibeserver.vibesdr.net`, url: urlOf('rx' + i), name: `Receiver ${i}`, lon: b.lon + 2 + i, lat: b.lat + (i % 5) * 0.3 }); }
  return { servers, count: servers.length };
}

/** ★ What a server answers for ?s=<layers>: only those layers' sections (vibe_community.h sectionsOfLayer). */
const SEC = { adsb: ['a'], acars: ['a'], vdl2: ['a'], hfdl: ['a'], ais: ['s'], dsc: ['d'], navtex: ['ns', 'nw', 'nm', 'nx'], ndb: ['b'], vor: ['o'], sondes: ['z'], aprs: ['p'], mesh: ['m'] };
export function answerFor(body, layersCsv) {
  if (!layersCsv) return body;
  const keep = new Set(layersCsv.split(',').flatMap((l) => SEC[l] || []));
  const out = {};
  for (const [k, v] of Object.entries(body)) if (['v', 'on', 'now', 'layers'].includes(k) || keep.has(k)) out[k] = v;
  return out;
}
