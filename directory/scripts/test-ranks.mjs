// Test the RECEIVER RANKINGS round (src/ranks.js) against a REAL SQLite database (schema + every migration) behind a
// minimal D1 shim, and a FAKE PSK Reporter / wspr.live (nothing touches the network — the answers are trimmed copies of
// the real ones fetched 2026-11-21, in scripts/testdata/).
//   node directory/scripts/test-ranks.mjs
// Checks: the stats page's two tables parsed by name (braces matched, strings skipped); places by competition ranking
// (ties share); wspr.live's band codes; the per-callsign lookup (one bucket read); a clear User-Agent, compression,
// ?query= only; one round in 50 minutes whatever calls it; a failure holds THAT source (1 h doubling) and its last good
// copy keeps serving; a 200 that is not the page never replaces a good copy; 400 / 503 / the status words.
import { DatabaseSync } from 'node:sqlite';
import { readFileSync, readdirSync } from 'node:fs';
import * as R from '../src/ranks.js';

const dir = new URL('../', import.meta.url);
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
const pskHtml = readFileSync(new URL('scripts/testdata/pskstats-2026-11-21.html', dir), 'utf8');
const wsprJson = readFileSync(new URL('scripts/testdata/wsprlive-2026-11-21.json', dir), 'utf8');

let log = [];
let plan = {};   // 'psk' | 'wspr' → { status, body, throw }
async function fakeFetch(url, opt) {
  log.push({ url, opt });
  const which = url.startsWith(R.PSK_STATS_URL) ? 'psk' : url.startsWith(R.WSPR_LIVE_URL) ? 'wspr' : 'other';
  const p = plan[which];
  if (p && p.throw) throw Object.assign(new Error(p.throw), { name: p.throw === 'timeout' ? 'TimeoutError' : 'TypeError' });
  if (p) return new Response(p.body ?? null, { status: p.status });
  if (which === 'psk') return new Response(pskHtml, { status: 200 });
  if (which === 'wspr') return new Response(wsprJson, { status: 200 });
  return new Response(null, { status: 404 });
}
const ask = async (call) => { const r = await R.serveRanks(new Request(`https://vibeserver.vibesdr.net/api/ranks?call=${encodeURIComponent(call)}`), env); return { status: r.status, j: await r.json(), h: r.headers }; };

console.log('── the parsers ──');
const psk = R.parsePskStats(pskHtml);
ok(psk && psk.bands['40m'] && psk.bands['40m'].reports && psk.bands['40m'].countries, 'the stats page: both tables, by band');
ok(!psk.bands.All && !psk.bands.invalid, '"All" and "invalid" are not bands');
ok(psk.bands['40m'].reports.get('EA8BFK').rank === 1 && psk.bands['40m'].reports.get('WESSEX').rank === 3, '40 m by reports: EA8BFK 1st, WESSEX 3rd (as the page lists them)');
ok(psk.bands['60m'].countries.get('MM9PSY').rank === 28, "60 m by countries: MM9PSY 28th");
ok(R.extractVar('var x = {"a":"}{","b":[1,2]};', 'x').b[1] === 2, 'a brace inside a string never ends the object');
ok(R.parsePskStats('<html>Service unavailable</html>') === null && R.parsePskStats('PSKReporter var reportResult = {broken') === null, 'not the page: null');
const ws = R.parseWsprLive(wsprJson);
ok(ws && ws.bands['40m'].get('ON5KQ').rank === 1 && ws.bands['20m'], "wspr.live: band 7 → 40 m, ON5KQ 1st by unique stations");
ok(ws.bands['60m'].get('GW2HFR').rank === 1 && ws.bands['60m'].get('WESSEX').rank === 1 && ws.bands['60m'].get('ON5KQ').rank === 3, 'ties share a place (44, 44, then 3rd)');
ok(R.parseWsprLive('{"error":"x"}') === null && R.parseWsprLive('Code: 241. DB::Exception') === null, 'an error answer: null');
ok(R.callKey(' g4abc ') === 'G4ABC' && R.callKey('<x>') === '' && R.callKey('EA8/DF4UE') === 'EA8/DF4UE', 'callsign keys');

console.log('── a round ──');
let now = 1_800_000_000;
let s = await R.runRanks(env, { fetch: fakeFetch, now });
ok(s.built && log.length === 2, 'one round: two requests, the lookup built');
ok(log.every((x) => x.opt.headers['user-agent'] === R.UA && /VibeSDR/.test(R.UA) && !/@/.test(R.UA)), 'a clear User-Agent naming the project, no e-mail address');
ok(log.every((x) => /gzip/.test(x.opt.headers['accept-encoding'])), 'compression asked for');
const wurl = new URL(log[1].url);
ok([...wurl.searchParams.keys()].join() === 'query' && /subtractHours\(now\(\), 24\)/.test(wurl.searchParams.get('query')) && /LIMIT 100 BY band/.test(wurl.searchParams.get('query')),
   'wspr.live: ?query= only, the last 24 hours, the top 100 a band');
let a = await ask('ea8bfk');
ok(a.status === 200 && a.j.call === 'EA8BFK' && a.j.psk.bands['40m'].reports === 1 && a.j.psk.bands['40m'].countries === 3, 'EA8BFK: 1st on 40 m for reports, joint 3rd for countries (148 = WESSEX\'s 148)');
ok(a.j.psk.window === 'last 24 hours' && a.j.psk.source === 'PSK Reporter' && a.j.psk.fetched === now, 'the answer names its source, window and age');
ok(/max-age=900/.test(a.h.get('cache-control')), 'cacheable for a quarter of an hour');
a = await ask('WESSEX');
ok(a.j.wspr && a.j.wspr.bands['40m'].rank === 2 && a.j.wspr.bands['40m'].n === 434 && a.j.psk.bands['60m'].reports === 1, 'WESSEX: WSPR 2nd on 40 m (434 stations), PSK 1st on 60 m');
a = await ask('M0NOBODY');
ok(a.status === 200 && a.j.psk === null && a.j.wspr === null, 'a callsign not ranked: nulls, not an error');
ok((await ask('')).status === 400 && (await ask('<script>')).status === 400, 'no / a malformed callsign: 400');
const bucketRows = db.prepare("SELECT k, length(v) AS n FROM rank_mirror WHERE k LIKE 'b:%'").all();
ok(bucketRows.length > 5 && bucketRows.every((r) => r.n < 200000), `the lookup in ${bucketRows.length} buckets, each small (one row read a lookup)`);

console.log('── manners ──');
log = [];
s = await R.runRanks(env, { fetch: fakeFetch, now: now + 1200 });
ok(s.stopped === 'recent' && log.length === 0, 'no second round within 50 minutes');
now += 3600;
plan = { psk: { status: 503 } };
s = await R.runRanks(env, { fetch: fakeFetch, now });
let st = await (await R.serveRanksStatus(env)).json();
ok(st.psk.lastCode === 503 && st.psk.holdUntil === now + 3600 && /503/.test(st.psk.lastError), 'PSK Reporter 503: that source held an hour, said in words');
a = await ask('EA8BFK');
ok(a.j.psk && a.j.psk.bands['40m'].reports === 1 && a.j.psk.fetched === now - 3600, 'its last good places keep serving (with their age)');
ok(a.j.wspr === null || a.j.wspr.fetched === now, 'wspr.live answered this round: its half is fresh');
log = []; now += 3600;
s = await R.runRanks(env, { fetch: fakeFetch, now });
ok(log.length === 2, 'after the hold: asked again');
st = await (await R.serveRanksStatus(env)).json();
ok(st.psk.holdUntil === now + 7200, '…and a second failure holds two hours (doubling)');
plan = { psk: { status: 200, body: '<html>Maintenance</html>' } };
now += 3 * 3600; log = [];
await R.runRanks(env, { fetch: fakeFetch, now });
a = await ask('EA8BFK');
ok(a.j.psk && a.j.psk.bands['40m'].reports === 1, 'a 200 that is not the statistics page never replaces the good copy');
plan = { wspr: { throw: 'timeout' } };
now += 6 * 3600;
await R.runRanks(env, { fetch: fakeFetch, now });
st = await (await R.serveRanksStatus(env)).json();
ok(st.wspr.lastError === 'timed out' && st.wspr.holdUntil > now && st.psk.lastError === '', 'a timeout: in words, that source held; the other recovered');
plan = {};
const fresh = new DatabaseSync(':memory:');
fresh.exec(readFileSync(new URL('migrations/0011-ranks.sql', dir), 'utf8'));
const env2 = { DB: { prepare: (sql) => ({ bind: (...x) => ({ first: async () => fresh.prepare(sql).get(...x) ?? null }), first: async () => fresh.prepare(sql).get() ?? null }) } };
const r503 = await R.serveRanks(new Request('https://x/api/ranks?call=G4ABC'), env2);
ok(r503.status === 503 && r503.headers.get('retry-after') === '3600', 'before the first round: 503 with Retry-After');
const wr = readFileSync(new URL('wrangler.jsonc', dir), 'utf8');
ok(/"41 \* \* \* \*"/.test(wr) && /"(23 \*\/6 \* \* \*|8,23,38,53 \* \* \* \*)"/.test(wr), 'wrangler: the hourly rankings cron beside the satellite one');
const idx = readFileSync(new URL('src/index.js', dir), 'utf8');
ok(/controller\.cron === '41 \* \* \* \*'/.test(idx) && /'\/api\/ranks'/.test(idx), 'index.js dispatches the cron and serves /api/ranks');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
