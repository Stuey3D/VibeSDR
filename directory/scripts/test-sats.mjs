// Test the sats.json MIRROR (src/sats.js) — the only CelesTrak client in VibeSDR — against a REAL SQLite database
// (schema + every migration) behind a minimal D1 shim, and a FAKE CelesTrak / SatNOGS (nothing touches the network).
//   node directory/scripts/test-sats.mjs
// Checks CelesTrak's usage policy (https://celestrak.org/usage-policy.php) path by path:
//   · FORMAT=csv, never tle; gzip/br asked; a clear User-Agent; the CSV parsed by header name, any column order, six-digit
//     (and nine-digit) catalogue numbers, quoted names with commas
//   · once per update: no round within 5 h of the last, whatever calls it
//   · 500 / 503 / a network failure: the round STOPS (no other group, no retry), hold ≥ 15 min (6 h here), the hold is
//     on disk before anything else, and a provisional 15-min hold precedes the first request (a Worker killed mid-round)
//   · 403: a long hold (24 h → 48 h); 404: that URL never again until MIRROR_VERSION changes; 301: the old URL never
//     again, the new CelesTrak location used next round, a move off CelesTrak / to TLE = dead
//   · a 200 that is not GP CSV never replaces the good copy; sats.json keeps serving the last good elements
//   · the words a person reads; ETag / 304; SatNOGS marks a downlink inactive and moves a "follow":"mode" one
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync } from 'node:fs';
import * as M from '../src/sats.js';

const dir = new URL('../', import.meta.url);
const root = new URL('../../', import.meta.url);
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const db = new DatabaseSync(':memory:');
db.exec(readFileSync(new URL('schema.sql', dir), 'utf8'));
for (const f of readdirSync(new URL('migrations/', dir)).filter((f) => f.endsWith('.sql')).sort()) {
  try { db.exec(readFileSync(new URL('migrations/' + f, dir), 'utf8')); } catch { /* an older migration that needs a by-hand column */ }
}
function stmt(sql, args = []) {
  return {
    bind: (...a) => stmt(sql, a),
    first: async () => db.prepare(sql).get(...args) ?? null,
    all: async () => ({ results: db.prepare(sql).all(...args) }),
    run: async () => { const r = db.prepare(sql).run(...args); return { meta: { changes: r.changes } }; },
  };
}
const env = { DB: { prepare: (sql) => stmt(sql) } };
const curated = JSON.parse(readFileSync(new URL('src/sats-curated.json', dir), 'utf8'));
const csvAmateur = readFileSync(new URL('vibeserver/testdata-sat/celestrak-amateur-2026-11-19.csv', root), 'utf8');
// weather: built from the JSON fixture, columns in a DIFFERENT order (header-named parsing)
const wj = JSON.parse(readFileSync(new URL('vibeserver/testdata-sat/celestrak-weather-2026-11-19.json', root), 'utf8'));
const wcols = ['NORAD_CAT_ID', 'OBJECT_NAME', 'EPOCH', 'MEAN_MOTION', 'ECCENTRICITY', 'INCLINATION', 'RA_OF_ASC_NODE', 'ARG_OF_PERICENTER', 'MEAN_ANOMALY', 'BSTAR', 'MEAN_MOTION_DOT', 'MEAN_MOTION_DDOT', 'EPHEMERIS_TYPE', 'OBJECT_ID'];
const csvWeather = [wcols.join(','), ...wj.map((o) => wcols.map((c) => { const v = String(o[c] ?? ''); return v.includes(',') ? `"${v}"` : v; }).join(','))].join('\r\n') + '\r\n';

// ── the fake network ────────────────────────────────────────────────────────────────────────────────────────────
let log = [];
let plan = {};       // url substring → { status, body, location, throw }
const satnogsBody = JSON.stringify([
  { norad_cat_id: 57166, description: 'LRPT', alive: true, status: 'active', mode: 'LRPT', baud: 72000, downlink_low: 137100000, type: 'Transmitter' },
  { norad_cat_id: 27607, description: 'FM', alive: false, status: 'inactive', mode: 'FM', downlink_low: 436795000, type: 'Transceiver' },
  { norad_cat_id: 25544, description: 'FM', alive: true, status: 'active', mode: 'FM', downlink_low: 437800000, type: 'Transceiver' },
  { norad_cat_id: 99999, description: 'other', alive: true, status: 'active', mode: 'FM', downlink_low: 145000000 },
]);
async function fakeFetch(url, opt) {
  log.push({ url, opt });
  for (const [k, p] of Object.entries(plan)) if (url.includes(k)) {
    if (p.throw) throw Object.assign(new Error(p.throw), { name: p.throw === 'timeout' ? 'TimeoutError' : 'TypeError' });
    return new Response(p.status === 200 ? p.body : null, { status: p.status, headers: p.location ? { location: p.location } : {} });
  }
  if (url.includes('GROUP=amateur')) return new Response(csvAmateur, { status: 200 });
  if (url.includes('GROUP=weather')) return new Response(csvWeather, { status: 200 });
  if (url.includes('satnogs')) return new Response(satnogsBody, { status: 200 });
  return new Response('not found', { status: 404 });
}
let now = 1795600000;
const run = (extra = {}) => M.runMirror(env, { fetch: fakeFetch, now, curated, ...extra });
const state = async () => M.loadState(env);
const sats = async () => JSON.parse((await env.DB.prepare("SELECT v FROM sat_mirror WHERE k='sats'").first()).v.replace(/^/, '')).body;
const satsDoc = async () => JSON.parse(await sats());

console.log('§1 CSV parsing');
{
  const a = M.parseGpCsv(csvAmateur);
  ok(a && a.length >= 90, `the real CelesTrak amateur CSV parses (${a && a.length} objects)`);
  const erm = a.find((o) => o.NORAD_CAT_ID === 101095);
  ok(erm && erm.OBJECT_NAME === 'ERMINAZ-2V (AO-130)' && erm.MEAN_MOTION > 15, 'six-digit 101095 ERMINAZ-2V, numbers as numbers');
  ok(a.find((o) => o.NORAD_CAT_ID === 7530).BSTAR === 0.91182313e-4, 'CelesTrak\'s ".91182313E-4" form read exactly');
  const w = M.parseGpCsv(csvWeather);
  ok(w && w.length === wj.length && w.find((o) => o.NORAD_CAT_ID === 57166).EPOCH === wj.find((o) => o.NORAD_CAT_ID === 57166).EPOCH, 'columns in another order: found by header name');
  const q = M.parseGpCsv('OBJECT_NAME,NORAD_CAT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,RA_OF_ASC_NODE,ARG_OF_PERICENTER,MEAN_ANOMALY\n"A, ""B""",123456789,2026-11-19T00:00:00,15,0.001,51,1,2,3\n');
  ok(q && q[0].OBJECT_NAME === 'A, "B"' && q[0].NORAD_CAT_ID === 123456789, 'a quoted name with a comma; a nine-digit number');
  ok(M.parseGpCsv('No GP data found') === null && M.parseGpCsv('<html>sign in</html>') === null && M.parseGpCsv('') === null, 'an error text / portal page / empty is not GP CSV');
  ok(M.parseGpCsv('OBJECT_NAME,NORAD_CAT_ID,EPOCH,MEAN_MOTION\nX,1,2026-11-19T00:00:00,15\n') === null, 'a CSV missing element columns is refused');
  ok(M.noradOf('1234567890') === 0 && M.noradOf('-5') === 0 && M.noradOf('A0000') === 0 && M.noradOf(101095) === 101095, 'catalogue numbers: 1–9 digits only');
}

console.log('§2 the first round — CSV, compression, User-Agent, once per update');
{
  log = [];
  const r = await run();
  ok(r.updated.join() === 'amateur,weather' && r.built, `both groups fetched, sats.json built (${JSON.stringify(r)})`);
  ok(log.filter((x) => x.url.includes('celestrak')).every((x) => /FORMAT=csv$/.test(x.url)) && !log.some((x) => /FORMAT=tle/i.test(x.url)), 'FORMAT=csv, never tle');
  ok(log[0].opt.headers['accept-encoding'].includes('gzip') && log[0].opt.headers['accept-encoding'].includes('br'), 'gzip + brotli asked');
  ok(/^VibeSDR-sats-mirror\/\d+ \(\+https:\/\/vibesdr\.net\)$/.test(log[0].opt.headers['user-agent']), 'a clear User-Agent, no personal data');
  ok(log[0].opt.redirect === 'manual', 'redirects are not followed silently');
  ok(log.some((x) => x.url.includes('satnogs')), 'SatNOGS asked on the first round');
  const d = await satsDoc();
  ok(d.format === 1 && d.satellites.length === curated.satellites.length && /CelesTrak/.test(d.attribution.orbits) && /CC BY-SA/.test(d.attribution.transmitters), 'sats.json: every curated bird, attribution for CelesTrak + SatNOGS');
  ok(d.satellites.filter((s) => s.omm).length === curated.satellites.length, 'every curated bird has elements');
  const m3 = d.satellites.find((s) => s.id === 'METEOR-M2-3'), so50 = d.satellites.find((s) => s.id === 'SO-50');
  ok(m3.transmitters[0].downlink === 137100000 && so50.transmitters[0].status === 'inactive', 'SatNOGS moved Meteor-M2-3 (follow: mode) and marked SO-50 inactive');
  ok(d.satellites.find((s) => s.id === 'ISS').transmitters[0].status === 'active' && !('status' in d.satellites.find((s) => s.id === 'RS-44').transmitters[0]), 'active where SatNOGS agrees; nothing claimed where it says nothing');
  log = []; now += 2 * 3600;
  const r2 = await run();
  ok(r2.stopped === 'recent' && log.length === 0, '2 h later: no request at all (once per update; the cron runs every 6 h)');
}

console.log('§3 503 — stop everything, ≥ 15 min, the hold on disk first');
{
  now += 4 * 3600; log = [];
  plan = { 'GROUP=amateur': { status: 503 } };
  const good = await sats();
  const r = await run();
  const s = await state();
  ok(r.stopped === '503 overloaded' && log.length === 1, `the round STOPPED at the 503: weather and SatNOGS not asked (${log.length} request)`);
  ok(s.holdUntil - now >= M.HOLD_5XX_MIN_SEC && s.holdUntil - now === 6 * 3600, `hold ≥ 15 min (${(s.holdUntil - now) / 3600} h)`);
  ok(/CelesTrak is overloaded \(HTTP 503\) for the amateur group — the mirror has stopped asking and will try again after \d{4}-\d\d-\d\d \d\d:\d\d UTC/.test(s.lastError), `the words: ${s.lastError}`);
  ok(await sats() === good, 'sats.json unchanged — the good copy kept');
  log = []; now += 20 * 60;
  ok((await run()).stopped === 'hold' && log.length === 0, '20 min later: still holding, no request');
  ok(M.classify(500, { fails: 0 }).hold >= M.HOLD_5XX_MIN_SEC && M.classify(503, { fails: 9 }).hold <= 86400, '500 the same; doubling caps at a day');
  ok(M.HOLD_5XX_MIN_SEC === 900 && Object.values({ a: M.classify(0, { fails: 0 }), b: M.classify(429, { fails: 0 }), c: M.classify(404, {}), d: M.classify(301, {}) }).every((c) => c.hold >= 900), 'no path holds less than 15 minutes');
  // the provisional hold: a Worker that dies after the first request
  now += 7 * 3600; plan = { 'GROUP=amateur': { throw: 'killed' } };
  let died = false;
  const dying = async (url, opt) => { if (url.includes('GROUP=weather')) { died = true; throw new Error('never reached'); } return fakeFetch(url, opt); };
  plan = {};
  const crashEnv = { DB: { prepare: (sql) => { const st = stmt(sql); if (/INSERT INTO sat_mirror/.test(sql)) return { bind: (...a) => (a[0] === 'omm:amateur' ? { run: async () => { throw new Error('worker killed'); } } : st.bind(...a)) }; return st; } } };
  try { await M.runMirror(crashEnv, { fetch: dying, now, curated }); } catch { /* the Worker died mid-round */ }
  const sc = await state();
  ok(!died && sc.holdUntil >= now + M.HOLD_5XX_MIN_SEC, 'a round killed after its first request left a ≥ 15-min hold on disk');
  now += 16 * 60;
}

console.log('§4 a network failure / timeout = overload');
{
  plan = { 'GROUP=amateur': { throw: 'timeout' } }; log = [];
  const r = await run({ force: true });
  const s = await state();
  ok(r.stopped === '0 network' && log.length === 1 && s.holdUntil - now >= M.HOLD_5XX_MIN_SEC, `a timeout stops the round and holds (${(s.holdUntil - now) / 3600} h)`);
  ok(/could not be reached for the amateur group \(timed out\) — treated as overload/.test(s.lastError), s.lastError);
  now = s.holdUntil + 1; plan = {}; log = [];
  const r2 = await run();
  ok(r2.updated.length === 2 && (await state()).fails === 0 && (await state()).lastError === '', 'recovers; the failures and the message clear');
}

console.log('§5 403 — a long hold');
{
  now += 6 * 3600; plan = { 'GROUP=weather': { status: 403 } }; log = [];
  const r = await run();
  let s = await state();
  ok(r.updated.join() === 'amateur' && r.stopped === '403 blocked', 'amateur refreshed, then the 403 stopped the round');
  ok(s.holdUntil - now === 24 * 3600 && /answered 403 .* will not ask again until/.test(s.lastError), `24 h: ${s.lastError}`);
  now = s.holdUntil + 1; log = [];
  await run();
  s = await state();
  ok(s.holdUntil - now === 48 * 3600, `a second 403: 48 h (${(s.holdUntil - now) / 3600} h)`);
  now = s.holdUntil + 1; plan = {};
  await run();
  ok((await state()).f403 === 0, 'a good round clears it');
}

console.log('§6 404 — dead until a software update');
{
  now += 6 * 3600; plan = { 'GROUP=weather': { status: 404 } }; log = [];
  await run();
  let s = await state();
  ok(s.dead[M.gpUrl('weather')] === M.MIRROR_VERSION && /answered 404 for the weather group — that address is no longer served/.test(s.lastError), s.lastError);
  now = s.holdUntil + 1; log = []; plan = {};
  const r = await run();
  ok(!log.some((x) => x.url.includes('GROUP=weather')) && r.updated.join() === 'amateur', 'the dead URL is never asked again (amateur still is)');
  const d = await satsDoc();
  ok(d.satellites.find((x) => x.id === 'METEOR-M2-4').omm, 'the weather birds keep their last good elements');
  // a software update (MIRROR_VERSION) clears it
  const raw = JSON.parse((await env.DB.prepare("SELECT v FROM sat_mirror WHERE k='state'").first()).v);
  for (const k of Object.keys(raw.dead)) raw.dead[k] = M.MIRROR_VERSION; raw.version = M.MIRROR_VERSION - 1;
  db.prepare("UPDATE sat_mirror SET v=? WHERE k='state'").run(JSON.stringify(raw));
  ok(Object.keys((await state()).dead).length === 0, 'a new MIRROR_VERSION clears the dead list');
}

console.log('§7 301 — never the old URL again');
{
  now += 6 * 3600; log = [];
  const moved = 'https://celestrak.org/NORAD/elements/gp-new.php?GROUP=amateur&FORMAT=csv';
  plan = { 'gp.php?GROUP=amateur': { status: 301, location: moved } };
  const r = await run();
  let s = await state();
  ok(r.stopped === '301 moved' && log.length === 1 && s.moved[M.gpUrl('amateur')] === moved, 'the move recorded, the round stopped');
  ok(s.lastError.includes(moved) && /never ask the old one again/.test(s.lastError), s.lastError);
  now = s.holdUntil + 1; log = []; plan = { 'gp-new.php': { status: 200, body: csvAmateur } };
  await run();
  ok(log[0].url === moved && !log.some((x) => x.url === M.gpUrl('amateur')), 'the next round asks the NEW address, never the old');
  // a move off CelesTrak, or to TLE, is not followed: dead
  now += 6 * 3600; log = []; plan = { 'GROUP=weather': { status: 301, location: 'https://celestrak.org/NORAD/elements/gp.php?GROUP=weather&FORMAT=tle' } };
  await run();
  s = await state();
  ok(s.dead[M.gpUrl('weather')] && /will not follow/.test(s.lastError), 'a move to a TLE query is not followed');
  ok(M.acceptableMove('http://celestrak.org/x') === null && M.acceptableMove('https://evil.example/celestrak.org') === null && M.acceptableMove('/NORAD/elements/gp.php?GROUP=a&FORMAT=csv').startsWith('https://celestrak.org/'), 'only https CelesTrak');
}

console.log('§8 a 200 that is not the data');
{
  // fresh database state for this section
  db.exec('DELETE FROM sat_mirror');
  now += 86400; plan = {}; await run();
  const good = await sats();
  now += 6 * 3600; plan = { 'GROUP=amateur': { status: 200, body: '<html>Please sign in</html>' } }; log = [];
  const r = await run();
  const s = await state();
  ok(r.stopped === '200 notdata' && log.length === 1 && s.holdUntil - now >= M.HOLD_5XX_MIN_SEC, 'refused, the round stopped, a hold');
  ok(await sats() === good && /was not the data/.test(s.lastError), 'the good copy is still served');
}

console.log('§9 serving — ETag, Last-Modified, 304');
{
  const req = (h = {}) => new Request('https://vibeserver.vibesdr.net/api/sats', { headers: h });
  const a = await M.serveSats(req(), env);
  const et = a.headers.get('etag');
  ok(a.status === 200 && /^"[0-9a-f]{24}"$/.test(et) && a.headers.get('last-modified') && /max-age=3600/.test(a.headers.get('cache-control')), 'ETag + Last-Modified + Cache-Control');
  const body = JSON.parse(await a.text());
  ok(body.satellites.every((x) => Number.isInteger(x.norad) && x.norad > 0), 'norad as integers');
  ok((await M.serveSats(req({ 'if-none-match': et }), env)).status === 304, 'If-None-Match → 304');
  ok((await M.serveSats(req({ 'if-modified-since': a.headers.get('last-modified') }), env)).status === 304, 'If-Modified-Since → 304');
  ok((await M.serveSats(req({ 'if-none-match': '"old"' }), env)).status === 200, 'another ETag → 200');
  const st = JSON.parse(await (await M.serveSatsStatus(env)).text());
  ok(typeof st.lastError === 'string' && st.groups.amateur && st.groups.amateur.count > 0, 'the status page for a person');
  const empty = { DB: { prepare: (sql) => ({ bind: () => ({ first: async () => null }) }) } };
  ok((await M.serveSats(req(), empty)).status === 503, 'before the first round: 503 + Retry-After');
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
