// community-fixtures.mjs — three fake contributing servers for the community map's tests, in the SERVER'S OWN FORMATS.
//
// ★ Two of the four bodies are REAL server output, not hand-written: testdata/community/real-server-dsc-navtex-ndb.json is
//   what a vibeserver built from b94-commap answered at /vibeserver/community.json (scripts/test-community-server.mjs —
//   a DSC distress call, Cullercoats NAVTEX, the WOD NDB), and real-shim-aircraft.json is what vibe_community.h built from
//   three aircraft-table rows (vibeserver/test-community.cpp, COMMUNITY_FIXTURE). The rest (an overlapping aircraft, an HFDL
//   aircraft, two ships in ais-ships.json's vessel shape) is written here in those same shapes.
// ★ rebase(): every absolute time ("now", "t", "first", "last") is moved to today, so the page's age filter sees them as fresh.
import { readFileSync } from 'node:fs';

const td = (f) => JSON.parse(readFileSync(new URL(`testdata/community/${f}`, import.meta.url), 'utf8'));

export function rebase(j, nowSec) {
  const delta = nowSec - j.now;
  const walk = (o) => {
    if (Array.isArray(o)) { o.forEach(walk); return; }
    if (!o || typeof o !== 'object') return;
    for (const k of Object.keys(o)) {
      if (['now', 't', 'first', 'last'].includes(k) && typeof o[k] === 'number' && o[k] > 1e9) o[k] += delta;
      else walk(o[k]);
    }
  };
  const c = JSON.parse(JSON.stringify(j));
  walk(c);
  return c;
}

/** The three servers' answers, keyed by id. */
export function answers() {
  const lenovo = td('real-server-dsc-navtex-ndb.json');
  const air = td('real-shim-aircraft.json');
  // pi500: the shim's three ADS-B aircraft + the SAME DSC distress the Lenovo heard (two receivers, one call)
  const pi500 = JSON.parse(JSON.stringify(air));
  pi500.now = lenovo.now;
  pi500.aircraft.now = lenovo.now;
  pi500.layers = ['adsb', 'dsc'];
  pi500.dsc = { items: lenovo.dsc.items.filter((i) => i.plat != null).map((i) => ({ ...i, r: 'RSP1B', t: i.t + 40 })), count: 1 };
  // xcover: BAW696 again (4CA000, a newer position) + an HFDL aircraft over the Atlantic + two ships off the Solent
  const now = lenovo.now;
  const xcover = {
    v: 1, on: true, now, layers: ['adsb', 'hfdl', 'ais'],
    aircraft: { now, count: 2, aircraft: [
      { key: '4CA000', icao: '4CA000', flight: 'BAW696', lat: 52.02, lon: 0.05, posAge: 0, posSrc: 'ADS-B', alt: 35100, trk: 92, gs: 441, src: ['ADS-B'], srcAge: { 'ADS-B': 0 }, age: 0, n: 40 },
      { key: 'A1B2C3', icao: 'A1B2C3', flight: 'UAL917', reg: 'N12345', lat: 54.5, lon: -30.2, posAge: 420, posSrc: 'HFDL', alt: 37000, src: ['HFDL', 'ADS-C'], srcAge: { HFDL: 420, 'ADS-C': 420 }, age: 420, n: 3 },
    ], stations: [] },
    ships: { now, count: 2, total: 2, vessels: [
      { mmsi: 235000001, kind: 'A', name: 'RED FALCON', call: 'MXRF', flag: 'GB', country: 'United Kingdom', stype: 60, stypeName: 'Passenger', family: 'passenger',
        lat: 50.79, lon: -1.31, sog: 12.4, cog: 181.0, hdg: 180, line: 'Passenger · 12.4 kn', ch: 'A', level: -42.0, snr: 21.0, seen: 25, age: 3600, posAge: 25, count: 320, trail: 0, stored: 1 },
      { mmsi: 232001234, kind: 'B', name: 'SEA BREEZE', flag: 'GB', country: 'United Kingdom', lat: 50.70, lon: -1.10, sog: 5.1, cog: 90.0, line: 'Sailing · 5.1 kn',
        ch: 'B', level: -60.0, snr: 9.0, seen: 600, age: 5000, posAge: 600, count: 12, trail: 0, stored: 1 },
    ], radios: [] },
  };
  return { lenovo, pi500, xcover };
}

/** The contributors list as /api/community answers it (contributorOf's shape), aimed at `urlOf(id)`. */
export function contributors(urlOf) {
  const one = (id, name, grid, lat, lon, country, layers, extra = {}) => ({ id, name, country, slug: id, address: `${id}.vibeserver.vibesdr.net`, url: urlOf(id),
    grid, lat, lon, layers, listeners: 0, maxListeners: 4, pin: false, temporary: false, updatedAt: 0, ...extra });
  return { servers: [
    one('lenovo', 'Stuart’s Lenovo (NAVTEX + NDB)', 'IO92', 52.5, -1, 'GB', ['dsc', 'navtex', 'ndb'], { listeners: 1, maxListeners: 4 }),
    one('pi500', 'Pi 500 — RSP1B + V4', 'IO91', 51.5, -1, 'GB', ['adsb', 'dsc'], { listeners: 4, maxListeners: 4 }),
    one('xcover', 'XCover portable', 'IO90', 50.5, -1, 'GB', ['adsb', 'hfdl', 'ais']),
  ], count: 3 };
}
