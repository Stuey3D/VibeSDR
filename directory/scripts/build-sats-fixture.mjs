// Builds the sats.json test fixtures from CelesTrak's 2026-11-19 GP data in vibeserver/testdata-sat, through the REAL
// mirror code (src/sats.js) with a fake network and an in-memory D1 — so the server tests read exactly what the mirror
// would serve. No network.
//   node directory/scripts/build-sats-fixture.mjs
//   → vibeserver/testdata-sat/sats-2026-11-19.json          (the curated list)
//   → vibeserver/testdata-sat/sats-6digit-2026-11-19.json   (+ TEST-101095: ERMINAZ-2V's real elements under a TEST-ONLY
//                                                              downlink — six-digit catalogue numbers end to end)
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, writeFileSync } from 'node:fs';
import * as M from '../src/sats.js';

const dir = new URL('../', import.meta.url);
const root = new URL('../../', import.meta.url);
const csvA = readFileSync(new URL('vibeserver/testdata-sat/celestrak-amateur-2026-11-19.csv', root), 'utf8');
const wj = JSON.parse(readFileSync(new URL('vibeserver/testdata-sat/celestrak-weather-2026-11-19.json', root), 'utf8'));
const cols = Object.keys(wj[0]);
const csvW = [cols.join(','), ...wj.map((o) => cols.map((c) => String(o[c] ?? '')).join(','))].join('\n') + '\n';

async function build(curated) {
  const db = new DatabaseSync(':memory:');
  db.exec(readFileSync(new URL('migrations/0010-sats.sql', dir), 'utf8'));
  const stmt = (sql, args = []) => ({ bind: (...a) => stmt(sql, a), first: async () => db.prepare(sql).get(...args) ?? null,
    run: async () => { db.prepare(sql).run(...args); return {}; } });
  const env = { DB: { prepare: (sql) => stmt(sql) } };
  const fetch = async (url) => url.includes('GROUP=amateur') ? new Response(csvA) : url.includes('GROUP=weather') ? new Response(csvW) : new Response('[]');
  await M.runMirror(env, { fetch, now: Date.parse('2026-11-19T18:00:00Z') / 1000, curated });
  return JSON.parse(db.prepare("SELECT v FROM sat_mirror WHERE k='sats'").get().v).body;
}
const curated = JSON.parse(readFileSync(new URL('src/sats-curated.json', dir), 'utf8'));
writeFileSync(new URL('vibeserver/testdata-sat/sats-2026-11-19.json', root), await build(curated) + '\n');
const six = structuredClone(curated);
six.satellites.push({ id: 'TEST-101095', name: 'TEST six-digit (ERMINAZ-2V elements)', norad: 101095, group: 'amateur',
  note: 'TEST ONLY — not a real downlink', transmitters: [{ label: 'Test FM', downlink: 435500000, mode: 'nfm', loop: 'fll' }] });
writeFileSync(new URL('vibeserver/testdata-sat/sats-6digit-2026-11-19.json', root), await build(six) + '\n');
console.log('written');
