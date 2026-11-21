/**
 * V12 beta90 — RECEIVER RANKINGS for the gallery card (docs/v12/PSK-REPORTER.md). The directory asks the two public
 * sources ONCE AN HOUR and publishes a compact {callsign → places} lookup; each VibeServer asks only for its OWN
 * receiver callsign (GET /api/ranks?call=G4ABC), so PSK Reporter and wspr.live see one polite client, not every server.
 *
 * ★ The idea is M9PSY's: he built PSK Reporter / WSPR rank fetching into UberSDR. This is our own code, from the two
 *   sources' public pages — nothing of his was copied.
 *
 * ★★★ THE SOURCES, AND THE MANNERS:
 *   · PSK Reporter's statistics page, https://www.pskreporter.info/cgi-bin/pskstats.pl — its "Top Monitors by reports
 *     over last 24 hours" (var reportResult) and "Top Monitors by number of different countries reported"
 *     (var countryResult) tables: per band, the top 50 receivers, each {"callsign","day","week"}. The page is itself a
 *     cached page ("Cached at …"); one GET an hour.
 *   · wspr.live's public ClickHouse (https://wspr.live: "You are allowed to use the services … as long as the results are
 *     accessible free of charge for everyone"; "every query you take should limit the data … by time and band"): one
 *     query an hour, the last 24 hours, receivers by UNIQUE transmitters heard per band, the top 100 a band. GET with
 *     ?query= only (the service drops everything else).
 *   · A clear User-Agent naming the project (no person's address); compression asked for; 60 s timeouts.
 *   · ANY failure STOPS that source for the round and holds it: 1 h doubling to a day; the last good copy keeps serving.
 *     A round never runs within 50 minutes of the last, whatever triggers it.
 * ★★ BOUNDED: answers over 4 MB are refused; at most 60 bands × 100 places; the lookup is stored in 37 buckets by the
 *    callsign's first character (D1, table rank_mirror), so a lookup reads one small row.
 */

export const RANKS_VERSION = 1;
export const PSK_STATS_URL = 'https://www.pskreporter.info/cgi-bin/pskstats.pl';
export const WSPR_LIVE_URL = 'https://db1.wspr.live/';
export const UA = `VibeSDR-directory-ranks/${RANKS_VERSION} (+https://vibesdr.net)`;
export const ROUND_MIN_SEC = 50 * 60;
export const HOLD_FAIL_SEC = 3600;
export const HOLD_MAX_SEC = 24 * 3600;
export const MAX_BYTES = 4 * 1024 * 1024;
export const WSPR_TOP = 100;
const TIMEOUT_MS = 60000;

/** wspr.live's band codes (the WSPR convention: MHz, with -1 = LF and 0 = MF) → band names as PSK Reporter writes them. */
export const WSPR_BANDS = { '-1': '2200m', 0: '630m', 1: '160m', 3: '80m', 5: '60m', 7: '40m', 10: '30m', 13: '22m', 14: '20m',
  18: '17m', 21: '15m', 24: '12m', 28: '10m', 40: '8m', 50: '6m', 70: '4m', 144: '2m', 432: '70cm', 1296: '23cm' };
/** The query: the last 24 hours, receivers by unique transmitters, per band, the top 100 each. */
export const WSPR_QUERY = `SELECT band, rx_sign, uniq(tx_sign) AS n FROM wspr.rx WHERE time > subtractHours(now(), 24) ` +
  `GROUP BY band, rx_sign ORDER BY band, n DESC LIMIT ${WSPR_TOP} BY band FORMAT JSONCompact`;

/** A callsign as a key: upper case, A–Z 0–9 / - only, ≤ 20. "" = not one. */
export function callKey(s) {
  const c = String(s || '').trim().toUpperCase();
  return /^[A-Z0-9][A-Z0-9/-]{1,19}$/.test(c) ? c : '';
}
/** A band name we keep ("40m", "70cm", "2.4Ghz" no — amateur HF/VHF/UHF names only). */
export function bandKey(b) {
  const s = String(b || '').trim();
  return /^\d{1,4}(\.\d+)?c?m$/.test(s) ? s : '';
}
export function bucketOf(call) {
  const c = call[0];
  return /[A-Z0-9]/.test(c) ? c : '_';
}

/** The text after `var <name> =` on the stats page → its object (null if absent / not JSON). Braces matched, strings
 *  skipped, so a callsign can never end it early. */
export function extractVar(html, name) {
  const at = html.indexOf(`var ${name}`);
  if (at < 0) return null;
  const s = html.indexOf('{', at);
  if (s < 0 || s - at > 40) return null;
  let depth = 0, q = false;
  for (let i = s; i < html.length && i - s < MAX_BYTES; i++) {
    const c = html[i];
    if (q) { if (c === '\\') i++; else if (c === '"') q = false; continue; }
    if (c === '"') q = true;
    else if (c === '{') depth++;
    else if (c === '}' && --depth === 0) {
      try { return JSON.parse(html.slice(s, i + 1)); } catch { return null; }
    }
  }
  return null;
}

/** Competition ranking ("1224"): equal counts share a place. rows = [[call, n], …] in any order → Map call → {rank, n}. */
export function rankRows(rows) {
  const sorted = rows.filter(([c, n]) => c && Number.isFinite(n) && n >= 0).sort((a, b) => b[1] - a[1]);
  const out = new Map();
  let prevN = null, prevRank = 0;
  sorted.forEach(([c, n], i) => {
    const rank = n === prevN ? prevRank : i + 1;
    prevN = n; prevRank = rank;
    if (!out.has(c)) out.set(c, { rank, n });
  });
  return out;
}

/** PSK Reporter's stats page → { bands: { "40m": { reports: Map, countries: Map } } } or null when it is not that page. */
export function parsePskStats(html) {
  if (typeof html !== 'string' || html.length > MAX_BYTES || !html.includes('PSKReporter')) return null;
  const rep = extractVar(html, 'reportResult'), cty = extractVar(html, 'countryResult');
  if (!rep || !cty || typeof rep !== 'object' || typeof cty !== 'object') return null;
  const bands = {};
  let n = 0;
  for (const [table, key] of [[rep, 'reports'], [cty, 'countries']]) {
    for (const [b, list] of Object.entries(table)) {
      const band = bandKey(b);
      if (!band || !Array.isArray(list) || n > 60 * 2) continue;
      const rows = list.slice(0, 100).map((x) => [callKey(x && x.callsign), Number(x && x.day)]);
      (bands[band] ||= {})[key] = rankRows(rows);
      n++;
    }
  }
  return Object.keys(bands).length ? { bands } : null;
}

/** wspr.live's JSONCompact answer → { bands: { "40m": Map } } or null. */
export function parseWsprLive(text) {
  if (typeof text !== 'string' || text.length > MAX_BYTES) return null;
  let j; try { j = JSON.parse(text); } catch { return null; }
  if (!j || !Array.isArray(j.data) || !Array.isArray(j.meta)) return null;
  const by = {};
  for (const r of j.data) {
    if (!Array.isArray(r) || r.length < 3) continue;
    const band = WSPR_BANDS[String(r[0])];
    const c = callKey(r[1]);
    if (!band || !c) continue;
    (by[band] ||= []).push([c, Number(r[2])]);
  }
  const bands = {};
  for (const [b, rows] of Object.entries(by)) bands[b] = rankRows(rows.slice(0, WSPR_TOP));
  return Object.keys(bands).length ? { bands } : null;
}

/** The two parses → bucket → { call → { psk?: { band: {reports?, reportsN?, countries?, countriesN?} }, wspr?: { band: {rank, n} } } }. */
export function buildLookup(psk, wspr) {
  const buckets = {};
  const put = (call) => { const b = bucketOf(call); const bk = (buckets[b] ||= {}); return (bk[call] ||= {}); };
  if (psk) for (const [band, t] of Object.entries(psk.bands)) {
    for (const key of ['reports', 'countries']) {
      if (!t[key]) continue;
      for (const [call, { rank, n }] of t[key]) {
        const e = put(call); const pb = ((e.psk ||= {})[band] ||= {});
        pb[key] = rank; pb[key + 'N'] = n;
      }
    }
  }
  if (wspr) for (const [band, m] of Object.entries(wspr.bands)) {
    for (const [call, { rank, n }] of m) { const e = put(call); (e.wspr ||= {})[band] = { rank, n }; }
  }
  return buckets;
}

// ── State (D1 table rank_mirror, migrations/0011-ranks.sql) ──────────────────────────────────────────────────────
async function getKv(env, k) {
  const r = await env.DB.prepare('SELECT v FROM rank_mirror WHERE k = ?').bind(k).first();
  if (!r) return null;
  try { return JSON.parse(r.v); } catch { return null; }
}
async function putKv(env, k, v, now) {
  await env.DB.prepare('INSERT INTO rank_mirror (k, v, updated_at) VALUES (?, ?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v, updated_at = excluded.updated_at')
    .bind(k, JSON.stringify(v), now).run();
}
function freshState() {
  return { lastRound: 0, psk: { at: 0, holdUntil: 0, fails: 0, lastCode: 0, lastError: '' }, wspr: { at: 0, holdUntil: 0, fails: 0, lastCode: 0, lastError: '' } };
}
export async function loadState(env) {
  const s = Object.assign(freshState(), (await getKv(env, 'state')) || {});
  s.psk = Object.assign(freshState().psk, s.psk || {}); s.wspr = Object.assign(freshState().wspr, s.wspr || {});
  return s;
}

async function get(fetchFn, url) {
  try {
    const r = await fetchFn(url, { headers: { 'user-agent': UA, 'accept-encoding': 'gzip, br' }, redirect: 'manual', signal: AbortSignal.timeout(TIMEOUT_MS) });
    if (r.status !== 200) return { code: r.status, why: `answered ${r.status}` };
    const len = Number(r.headers.get('content-length') || 0);
    if (len > MAX_BYTES) return { code: 200, why: 'too large' };
    const body = await r.text();
    if (body.length > MAX_BYTES) return { code: 200, why: 'too large' };
    return { code: 200, body };
  } catch (e) {
    return { code: 0, why: e && e.name === 'TimeoutError' ? 'timed out' : 'no answer' };
  }
}
function fail(src, now, code, why) {
  src.fails = (src.fails || 0) + 1;
  src.holdUntil = now + Math.min(HOLD_MAX_SEC, HOLD_FAIL_SEC * 2 ** (src.fails - 1));
  src.lastCode = code; src.lastError = why;
}

/** One round (the cron, hourly). Each source on its own: a failure holds that one and keeps its last good copy. */
export async function runRanks(env, deps = {}) {
  const fetchFn = deps.fetch || fetch;
  const now = Math.floor(deps.now != null ? deps.now : Date.now() / 1000);
  const s = await loadState(env);
  const summary = { asked: [], stopped: '', built: false };
  if (s.lastRound && now - s.lastRound < ROUND_MIN_SEC && !deps.force) { summary.stopped = 'recent'; return summary; }
  s.lastRound = now;
  await putKv(env, 'state', s, now);      // ★ written first: a Worker killed mid-round still waits for the next
  let psk = null, wspr = null;
  if (now >= s.psk.holdUntil) {
    summary.asked.push(PSK_STATS_URL);
    const r = await get(fetchFn, PSK_STATS_URL);
    const parsed = r.code === 200 && !r.why ? parsePskStats(r.body) : null;
    if (parsed) { psk = parsed; s.psk.at = now; s.psk.fails = 0; s.psk.lastCode = 200; s.psk.lastError = ''; }
    else fail(s.psk, now, r.code, r.why || 'not the statistics page');
  }
  if (now >= s.wspr.holdUntil) {
    const url = `${WSPR_LIVE_URL}?query=${encodeURIComponent(WSPR_QUERY)}`;
    summary.asked.push(url);
    const r = await get(fetchFn, url);
    const parsed = r.code === 200 && !r.why ? parseWsprLive(r.body) : null;
    if (parsed) { wspr = parsed; s.wspr.at = now; s.wspr.fails = 0; s.wspr.lastCode = 200; s.wspr.lastError = ''; }
    else fail(s.wspr, now, r.code, r.why || 'not a wspr.live answer');
  }
  await putKv(env, 'state', s, now);
  if (!psk && !wspr) return summary;
  // ★ NEVER A GOOD COPY REPLACED BY NOTHING: a source that failed keeps its last lookup half.
  const prev = (await getKv(env, 'index')) || { buckets: [], psk: 0, wspr: 0 };
  const built = buildLookup(psk, wspr);
  const names = new Set([...prev.buckets, ...Object.keys(built)]);
  for (const b of names) {
    const old = (await getKv(env, `b:${b}`)) || {};
    const neu = built[b] || {};
    const merged = {};
    for (const [call, e] of Object.entries(old)) {
      const keep = {};
      if (!psk && e.psk) keep.psk = e.psk;
      if (!wspr && e.wspr) keep.wspr = e.wspr;
      if (keep.psk || keep.wspr) merged[call] = keep;
    }
    for (const [call, e] of Object.entries(neu)) merged[call] = Object.assign(merged[call] || {}, e);
    await putKv(env, `b:${b}`, merged, now);
  }
  await putKv(env, 'index', { buckets: [...names], psk: psk ? now : prev.psk, wspr: wspr ? now : prev.wspr }, now);
  summary.built = true;
  return summary;
}

const SRC = {
  psk: { source: 'PSK Reporter', url: 'https://pskreporter.info/', window: 'last 24 hours', metrics: { reports: 'reports', countries: 'countries' } },
  wspr: { source: 'wspr.live (WSPRnet spots)', url: 'https://wspr.live/', window: 'last 24 hours', metrics: { rank: 'unique stations' } },
};

/** GET /api/ranks?call=G4ABC → {call, psk:{fetched, window, source, bands:{band:{reports,reportsN,countries,countriesN}}},
 *  wspr:{fetched, window, source, bands:{band:{rank,n}}}} — only this callsign's places; {call, psk:null, wspr:null}
 *  when it is not ranked. 400 for a missing / malformed callsign; 503 before the first round. */
export async function serveRanks(request, env) {
  const url = new URL(request.url);
  const call = callKey(url.searchParams.get('call'));
  const h = { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'public, max-age=900', 'access-control-allow-origin': '*' };
  if (!call) return new Response(JSON.stringify({ error: 'call= a callsign' }), { status: 400, headers: h });
  const idx = await getKv(env, 'index');
  if (!idx) return new Response(JSON.stringify({ error: 'the rankings have not been fetched yet' }), { status: 503, headers: { ...h, 'retry-after': '3600', 'cache-control': 'no-store' } });
  const e = ((await getKv(env, `b:${bucketOf(call)}`)) || {})[call] || {};
  const out = { call, psk: null, wspr: null };
  if (e.psk && idx.psk) out.psk = { fetched: idx.psk, window: SRC.psk.window, source: SRC.psk.source, bands: e.psk };
  if (e.wspr && idx.wspr) out.wspr = { fetched: idx.wspr, window: SRC.wspr.window, source: SRC.wspr.source, bands: e.wspr };
  return new Response(JSON.stringify(out), { status: 200, headers: h });
}

/** GET /api/ranks/status — the round's state for a person (holds, last error in words, ages). */
export async function serveRanksStatus(env) {
  const s = await loadState(env);
  const idx = await getKv(env, 'index');
  return new Response(JSON.stringify({ lastRound: s.lastRound, psk: s.psk, wspr: s.wspr, built: idx ? { psk: idx.psk, wspr: idx.wspr, buckets: idx.buckets.length } : null }),
    { headers: { 'content-type': 'application/json', 'cache-control': 'no-store' } });
}
