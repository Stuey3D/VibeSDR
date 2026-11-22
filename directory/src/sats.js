/**
 * V12 SATELLITE ENGINE — the sats.json MIRROR (docs/v12/SATELLITE-ENGINE.md §6.2). The ONLY CelesTrak client in the
 * whole VibeSDR system: VibeServers never ask CelesTrak themselves, they sync sats.json from here (vibeserver/satcat.cpp),
 * like the EiBi schedule. Run by the directory Worker's Cron Trigger (wrangler.jsonc "triggers"), served at GET /api/sats.
 *
 * ★★★ CELESTRAK'S USAGE POLICY IS THE DESIGN (https://celestrak.org/usage-policy.php, read 2026-11-19):
 *   · GP data updates every 2 hours: download only when needed and only once per update — with a fresh good copy a round
 *     runs every 6 h (REFRESH_SEC), whatever triggered it; only FAILED rounds retry sooner (THE SCHEDULE below).
 *   · FORMAT=csv (JSON is ~3× larger; TLE answers errors now and cannot carry six-digit catalogue numbers) — never tle.
 *   · Compression: Workers' fetch() sends Accept-Encoding gzip/br and decodes transparently (we say so explicitly too).
 *   · ANY non-200 STOPS THE ROUND (no other group is asked, nothing is retried) and is REPORTED TO A PERSON: one log line
 *     and the persisted state at GET /api/sats/status (Workers logs are sampled at 10 %, the status never is).
 *   · 301 → the old URL is never asked again; the new location is recorded and used next round (the policy: "your
 *     software can update the URL") — only an https celestrak.org address that is not a TLE query, else the group is dead.
 *   · 404 → that URL is dead: never asked again until a software update (MIRROR_VERSION changes).
 *   · 403 → a block: a long hold, 24 h doubling to 7 days.
 * ★★★ 500 / 503 (and a network failure, a timeout, a reset — the server may be overloaded): STOP EVERYTHING at once and
 *     hold for at least 15 minutes (HOLD_5XX_MIN_SEC — no setting can lower it). THE SCHEDULE (Stuart, 2026-11-20):
 *       · NO GOOD COPY, or the copy is OLD (no good round for 24 h, STALE_SEC): retry 15 min → 30 min → 1 h → then
 *         every hour (FAST_LADDER) — servers have no satellites until this succeeds, so the first pass comes first.
 *       · A FRESH GOOD COPY: a round every 6 h (REFRESH_SEC); a failed one waits for the next 6-h slot. Four failed
 *         6-hourly rounds = 24 h without a good one = OLD → back to the start of the fast ladder.
 *       · 403 is a block, never the ladder: 24 h doubling to 7 days, whatever the copy's age.
 *     The cron ticks every 15 min; a tick inside a hold only reads one row. THE HOLD
 *     IS WRITTEN BEFORE ANYTHING ELSE HAPPENS, and a provisional 15-minute hold is written BEFORE the first request, so a
 *     Worker that dies mid-round still honours it.
 * ★★ NEVER REPLACE A GOOD COPY WITH A BAD ONE: each group's last good element sets are kept (D1) and sats.json is rebuilt
 *    from the good copies; an answer that is not GP CSV (an error page, an empty file, > 2 MB) is a failure.
 * ★★ SIX-DIGIT CATALOGUE NUMBERS: plain integers up to 9 digits (the OMM field), never a 5-character TLE field.
 * ★ SatNOGS DB (CC BY-SA 4.0) once a day, the same stop-on-non-200 rules: it may mark a curated downlink inactive, and
 *   moves a downlink marked "follow":"mode" (Meteor — frequency and mode change on orbit, brief §6.2 TRAP). The curated
 *   list (sats-curated.json) stays the authority on WHICH birds and transmitters are offered.
 */

export const MIRROR_VERSION = 1;                 // ★ bump with a change of URL rules: it clears the 404 dead list
export const CT_BASE = 'https://celestrak.org/NORAD/elements/gp.php';
export const SATNOGS_URL = 'https://db.satnogs.org/api/transmitters/?format=json';
export const REFRESH_SEC = 6 * 3600;             // a fresh good copy is refreshed every 6 h (CelesTrak updates every 2 h)
export const REFRESH_SLACK_SEC = 5 * 60;         // the 15-min cron's tick lands a little either side of the 6 h
/* ★★ 12 h, not a day (Stuart 11-22: "we want the data to be as current as we can"). At 24 h one failed round on
 *  11-21 ~21:40 UTC meant three more 6-hour waits — 25 h with no fresh copy, and servers synced mid-stall kept it for
 *  their day. Now: one failed 6-hourly round, then the fast ladder (15 min → 30 min → hourly, the policy's floor kept).
 *  The same 12 h a server uses to ask again sooner (vibeserver/satcat.h kStaleDataSec). */
export const STALE_SEC = 12 * 3600;              // no good round for 12 h: the copy is OLD → the fast ladder
export const HISTORY_MAX = 30;                   // ★ the last rounds' outcomes kept for /api/sats/status (a stall explained)
export const FAST_LADDER = [15 * 60, 30 * 60, 3600];   // no / old copy: after a failure, then hourly
export const SATNOGS_EVERY_SEC = 20 * 3600;      // "daily"
export const HOLD_5XX_MIN_SEC = 15 * 60;         // ★★★ the policy's floor — nothing may hold less after a 5xx
export const HOLD_FAIL_SEC = 6 * 3600;           // a failure, then doubling to a day
export const HOLD_403_SEC = 24 * 3600;           // a block, then doubling to a week
export const MAX_GROUP_BYTES = 2 * 1024 * 1024;
export const UA = `VibeSDR-sats-mirror/${MIRROR_VERSION} (+https://vibesdr.net)`;
const TIMEOUT_MS = 30000;
/* ★★ beta91 SATELLITE LABELS (docs/v12/SAT-LABELS.md — Stuart 2026-11-22): curated.labelGroups are CelesTrak groups whose
 *  birds are LABEL-ONLY (ORBCOMM — commercial messaging: named on the waterfall, never offered in SATELLITE mode, never
 *  decoded). They are asked AFTER the main groups in the same round — one more URL, the same rules (a non-200 stops the
 *  round there; the main groups already fetched this round are kept). Their birds come from curated.constellations. */
export const LABEL_SAT_MAX = 60;                 // label-only birds a constellation may add
export const LABEL_GROUP_ROWS = 200;             // rows of a label group's copy kept (CelesTrak's orbcomm group is ~70)
export const SATS_JSON_MAX = 200;                // ★★ every server's parseSats refuses a sats.json with more birds

// ── CSV (RFC 4180: quoted fields, "" inside quotes) ────────────────────────────────────────────────────────────────
export function parseCsv(text, maxRows = 20000) {
  const rows = [];
  let row = [], f = '', q = false, i = 0;
  const n = text.length;
  while (i < n) {
    const c = text[i];
    if (q) {
      if (c === '"') { if (text[i + 1] === '"') { f += '"'; i += 2; continue; } q = false; i++; continue; }
      f += c; i++; continue;
    }
    if (c === '"' && f === '') { q = true; i++; continue; }
    if (c === ',') { row.push(f); f = ''; i++; continue; }
    if (c === '\r') { i++; continue; }
    if (c === '\n') { row.push(f); f = ''; rows.push(row); row = []; if (rows.length > maxRows) break; i++; continue; }
    f += c; i++;
  }
  if (f !== '' || row.length) { row.push(f); rows.push(row); }
  return rows;
}

const OMM_NUM = ['MEAN_MOTION', 'ECCENTRICITY', 'INCLINATION', 'RA_OF_ASC_NODE', 'ARG_OF_PERICENTER', 'MEAN_ANOMALY',
  'EPHEMERIS_TYPE', 'NORAD_CAT_ID', 'ELEMENT_SET_NO', 'REV_AT_EPOCH', 'BSTAR', 'MEAN_MOTION_DOT', 'MEAN_MOTION_DDOT'];
const OMM_STR = ['OBJECT_NAME', 'OBJECT_ID', 'EPOCH', 'CLASSIFICATION_TYPE'];
const REQUIRED = ['NORAD_CAT_ID', 'EPOCH', 'MEAN_MOTION', 'ECCENTRICITY', 'INCLINATION', 'RA_OF_ASC_NODE', 'ARG_OF_PERICENTER', 'MEAN_ANOMALY'];

/** A catalogue number: an integer 1 … 999 999 999 (OMM's NORAD_CAT_ID). ★ Six digits and more are normal now. */
export function noradOf(v) {
  const s = String(v).trim();
  if (!/^\d{1,9}$/.test(s)) return 0;
  const n = Number(s);
  return n > 0 ? n : 0;
}

/** CelesTrak GP CSV → OMM objects (JSON numbers for the numeric fields). Columns found BY HEADER NAME, any order.
 *  Returns null when it is not GP CSV. */
export function parseGpCsv(text) {
  if (typeof text !== 'string' || text.length === 0 || text.length > MAX_GROUP_BYTES) return null;
  const rows = parseCsv(text.replace(/^\uFEFF/, ''));
  if (rows.length < 2) return null;
  const head = rows[0].map((h) => h.trim().toUpperCase());
  const col = {};
  head.forEach((h, k) => { col[h] = k; });
  for (const r of REQUIRED) if (!(r in col)) return null;
  const out = [];
  for (let k = 1; k < rows.length; k++) {
    const r = rows[k];
    if (r.length === 1 && r[0] === '') continue;
    const o = {};
    for (const key of OMM_STR) if (key in col && r[col[key]] !== undefined) o[key] = r[col[key]].trim();
    let bad = false;
    for (const key of OMM_NUM) {
      if (!(key in col)) continue;
      const s = (r[col[key]] ?? '').trim();
      if (s === '') continue;
      const v = Number(s);
      if (!Number.isFinite(v)) { bad = true; break; }
      o[key] = v;
    }
    if (bad) continue;
    const id = noradOf((r[col.NORAD_CAT_ID] ?? '').trim());
    if (!id || !/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d/.test(o.EPOCH || '')) continue;
    o.NORAD_CAT_ID = id;
    out.push(o);
  }
  return out.length ? out : null;
}

export function gpUrl(group) {
  if (!/^[a-z0-9-]{1,40}$/.test(group)) throw new Error('bad group');
  return `${CT_BASE}?GROUP=${group}&FORMAT=csv`;
}
/** ★ A 301's new location is followed only to https CelesTrak, and never to a TLE query. */
export function acceptableMove(loc) {
  try {
    const u = new URL(loc, CT_BASE);
    if (u.protocol !== 'https:') return null;
    if (u.hostname !== 'celestrak.org' && !u.hostname.endsWith('.celestrak.org')) return null;
    if (/FORMAT=(tle|3le|2le)/i.test(u.search)) return null;
    return u.toString();
  } catch { return null; }
}

// ── State (D1 table sat_mirror, migrations/0010-sats.sql) ─────────────────────────────────────────────────────────
async function getKv(env, k) {
  const r = await env.DB.prepare('SELECT v FROM sat_mirror WHERE k = ?').bind(k).first();
  if (!r) return null;
  try { return JSON.parse(r.v); } catch { return null; }
}
async function putKv(env, k, v, now) {
  await env.DB.prepare('INSERT INTO sat_mirror (k, v, updated_at) VALUES (?, ?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v, updated_at = excluded.updated_at')
    .bind(k, JSON.stringify(v), now).run();
}
function freshState() {
  return { version: MIRROR_VERSION, holdUntil: 0, fails: 0, f403: 0, lastRound: 0, lastCode: 200, lastError: '', lastErrorAt: 0,
           lastGoodAt: 0, fastFails: 0, history: [],
           dead: {}, moved: {}, satnogs: { holdUntil: 0, fails: 0, at: 0, lastCode: 200, lastError: '' } };
}
export async function loadState(env) {
  const s = Object.assign(freshState(), (await getKv(env, 'state')) || {});
  if (s.version !== MIRROR_VERSION) { s.dead = {}; s.version = MIRROR_VERSION; }   // ★ "until a software update"
  return s;
}

/** What a non-200 (or a failure) means: { kind, hold } — hold in seconds from now. */
export function classify(code, s) {
  if (code === 0 || (code >= 500 && code <= 599)) {
    const hold = Math.min(86400, HOLD_FAIL_SEC * 2 ** Math.min(s.fails, 2));
    return { kind: code === 0 ? 'network' : 'overloaded', hold: Math.max(HOLD_5XX_MIN_SEC, hold) };
  }
  if (code === 403) return { kind: 'blocked', hold: Math.max(HOLD_5XX_MIN_SEC, Math.min(7 * 86400, HOLD_403_SEC * 2 ** Math.min(s.f403, 3))) };
  if (code === 404) return { kind: 'dead', hold: Math.max(HOLD_5XX_MIN_SEC, HOLD_FAIL_SEC) };
  if (code === 301 || code === 308) return { kind: 'moved', hold: Math.max(HOLD_5XX_MIN_SEC, HOLD_FAIL_SEC) };
  return { kind: 'refused', hold: Math.max(HOLD_5XX_MIN_SEC, Math.min(86400, HOLD_FAIL_SEC * 2 ** Math.min(s.fails, 2))) };
}

/** A good copy that is less than STALE_SEC old. */
export function freshCopy(s, now) { return !!s.lastGoodAt && now - s.lastGoodAt < STALE_SEC; }

/** CelesTrak's hold after a failure of `kind`, by THE SCHEDULE (header). Call BEFORE fastFails is counted. */
export function celestrakHold(kind, s, now) {
  if (kind === 'blocked') return classify(403, s).hold;
  if (freshCopy(s, now)) return REFRESH_SEC;
  return Math.max(HOLD_5XX_MIN_SEC, FAST_LADDER[Math.min(s.fastFails || 0, FAST_LADDER.length - 1)]);
}

/** The hold actually in force. With no / an old copy, a hold written by an older, slower schedule gives way to the ladder
 *  (the 12:23 UTC 522 of 2026-11-20 had been held to 18:23) — never below the policy's 15 min from the last round, and
 *  never for a 403. */
export function holdInForce(s, now) {
  if (freshCopy(s, now) || s.lastCode === 403 || !s.lastErrorAt) return s.holdUntil;
  const ladder = s.lastErrorAt + FAST_LADDER[Math.min(Math.max((s.fastFails || 0) - 1, 0), FAST_LADDER.length - 1)];
  return Math.max(Math.min(s.holdUntil, ladder), (s.lastRound || 0) + HOLD_5XX_MIN_SEC);
}

/** The words a person reads (the log line and /api/sats/status). */
export function describe(source, code, kind, group, untilSec, extra = '') {
  const t = new Date(untilSec * 1000).toISOString().replace('T', ' ').slice(0, 16) + ' UTC';
  const who = source === 'satnogs' ? 'SatNOGS DB' : 'CelesTrak';
  const what = group ? ` for the ${group} group` : '';
  switch (kind) {
    case 'overloaded': return `${who} is overloaded (HTTP ${code})${what} — the mirror has stopped asking and will try again after ${t}`;
    case 'network': return `${who} could not be reached${what} (${extra || 'no answer'}) — treated as overload: the mirror has stopped asking and will try again after ${t}`;
    case 'blocked': return `${who} answered 403 (access refused, usually a temporary block)${what} — the mirror will not ask again until ${t}`;
    case 'dead': return `${who} answered 404${what} — that address is no longer served; the mirror will not ask for it again until its software is updated`;
    case 'moved': return extra ? `${who} answered 301 (moved)${what} — new address ${extra}; the mirror will use it from ${t} and never ask the old one again`
                               : `${who} answered 301 (moved)${what} to an address the mirror will not follow; it will not ask for this group again until its software is updated`;
    case 'notdata': return `${who}'s answer${what} was not the data (${extra}) — the good copy is kept; the mirror will try again after ${t}`;
    default: return `${who} answered HTTP ${code}${what} — the mirror has stopped asking and will try again after ${t}`;
  }
}

async function get(fetchFn, url) {
  try {
    const r = await fetchFn(url, {
      redirect: 'manual',
      headers: { 'user-agent': UA, 'accept-encoding': 'gzip, br', accept: 'text/csv, application/json;q=0.9, */*;q=0.1' },
      signal: AbortSignal.timeout(TIMEOUT_MS),
    });
    if (r.status !== 200) return { code: r.status, location: r.headers.get('location') || '' };
    const len = Number(r.headers.get('content-length') || 0);
    if (len > MAX_GROUP_BYTES * 4) return { code: 200, text: null, why: 'larger than any group' };
    const text = await r.text();
    return { code: 200, text };
  } catch (e) {
    return { code: 0, why: String((e && e.name === 'TimeoutError') ? 'timed out' : (e && e.message) || e).slice(0, 120) };
  }
}

/** Records a failure. ★ Called FIRST after a non-200 — before any other work — so the hold is on disk. */
/** ★ One line in the round history (newest first, HISTORY_MAX kept): { at, source, outcome, code, group, message }. */
export function noteHistory(s, entry) {
  s.history = [entry, ...(Array.isArray(s.history) ? s.history : [])].slice(0, HISTORY_MAX);
}

async function fail(env, s, now, source, code, kind, hold, group, extra) {
  const tgt = source === 'satnogs' ? s.satnogs : s;
  tgt.holdUntil = now + hold;
  if (source !== 'satnogs' && (kind === 'overloaded' || kind === 'network')) s.satnogs.holdUntil = Math.max(s.satnogs.holdUntil, now + HOLD_5XX_MIN_SEC);
  if (kind === 'blocked') tgt.f403 = (tgt.f403 || 0) + 1; else tgt.fails = (tgt.fails || 0) + 1;
  tgt.lastCode = code;
  tgt.lastError = describe(source, code, kind, group, tgt.holdUntil, extra);
  tgt.lastErrorAt = now;
  noteHistory(s, { at: now, source, outcome: kind, code, group: group || '', message: tgt.lastError });
  await putKv(env, 'state', s, now);
  console.error(JSON.stringify({ sats_mirror: source, code, kind, group: group || '', message: tgt.lastError }));
}

/**
 * One round. `deps` = { fetch, now (seconds), curated } — seams for the test. Returns a summary.
 * ★ No retry anywhere: every URL is asked at most once a round, and the round ends at the first non-200.
 */
export async function runMirror(env, deps) {
  const fetchFn = deps.fetch || fetch;
  const now = Math.floor(deps.now != null ? deps.now : Date.now() / 1000);
  const curated = deps.curated;
  const s = await loadState(env);
  const summary = { asked: [], updated: [], stopped: '', built: false };
  if (now < holdInForce(s, now)) { summary.stopped = 'hold'; return summary; }
  if (freshCopy(s, now) && now - s.lastGoodAt < REFRESH_SEC - REFRESH_SLACK_SEC && !deps.force) { summary.stopped = 'recent'; return summary; }
  const ladder = !freshCopy(s, now);              // ★ this round's failures climb the fast ladder
  // ★★ the provisional hold, BEFORE the first request: a Worker killed mid-round still waits ≥ 15 minutes
  s.lastRound = now;
  s.holdUntil = now + HOLD_5XX_MIN_SEC;
  await putKv(env, 'state', s, now);

  const want = new Set(curated.satellites.map((x) => noradOf(x.norad)));
  const labelGroups = new Set(curated.labelGroups || []);
  let changed = false, stopped = false;
  for (const g of roundGroups(curated)) {
    let url = gpUrl(g);
    for (let hop = 0; s.moved[url] && hop < 4; hop++) url = s.moved[url];
    if (s.dead[url]) continue;                     // ★ a 404'd / unfollowable URL: not until a software update
    summary.asked.push(url);
    const r = await get(fetchFn, url);
    if (r.code !== 200) {
      const c = classify(r.code, s);
      let extra = r.why || '';
      if (c.kind === 'moved') {
        const to = acceptableMove(r.location);
        if (to && to !== url) { s.moved[url] = to; extra = to; } else { s.dead[url] = MIRROR_VERSION; extra = ''; }
      }
      if (c.kind === 'dead') s.dead[url] = MIRROR_VERSION;
      const hold = celestrakHold(c.kind, s, now);
      if (ladder && c.kind !== 'blocked') s.fastFails = (s.fastFails || 0) + 1;
      await fail(env, s, now, 'celestrak', r.code, c.kind, hold, g, extra);
      summary.stopped = `${r.code} ${c.kind}`;
      stopped = true;
      break;                                         // ★★★ STOP: nothing more is asked this round
    }
    const omm = r.text == null ? null : parseGpCsv(r.text);
    if (!omm) {
      const hold = celestrakHold('notdata', s, now);
      if (ladder) s.fastFails = (s.fastFails || 0) + 1;
      await fail(env, s, now, 'celestrak', 200, 'notdata', hold, g, r.why || 'not GP CSV');
      summary.stopped = '200 notdata';
      stopped = true;
      break;
    }
    // ★ a label group keeps its constellation's birds (by name prefix), bounded; a main group only the curated birds
    const mine = labelGroups.has(g) ? constellationRows(curated, g, omm) : omm.filter((o) => want.has(o.NORAD_CAT_ID));
    await putKv(env, `omm:${g}`, { fetched: now, url, count: omm.length, omm: mine }, now);
    summary.updated.push(g);
    changed = true;
  }

  // ★ beta91: SatNOGS is asked about the label-only birds too — whether each is alive, and its channel (satnogsChannel)
  for (const g of labelGroups) { const c = await getKv(env, `omm:${g}`); if (c) for (const o of c.omm) want.add(o.NORAD_CAT_ID); }
  // SatNOGS, daily, the same rules (its own hold; a CelesTrak stop this round skips it too)
  if (!stopped && now >= s.satnogs.holdUntil && now - (s.satnogs.at || 0) >= SATNOGS_EVERY_SEC && !s.dead[SATNOGS_URL]) {
    summary.asked.push(SATNOGS_URL);
    const r = await get(fetchFn, SATNOGS_URL);
    let list = null;
    if (r.code === 200 && r.text != null) { try { const j = JSON.parse(r.text); if (Array.isArray(j)) list = j; } catch { list = null; } }
    if (r.code !== 200 || !list) {
      const c = r.code === 200 ? { kind: 'notdata', hold: Math.max(HOLD_5XX_MIN_SEC, HOLD_FAIL_SEC) } : classify(r.code, s.satnogs);
      if (c.kind === 'dead' || (c.kind === 'moved')) s.dead[SATNOGS_URL] = MIRROR_VERSION;
      await fail(env, s, now, 'satnogs', r.code, c.kind, c.hold, '', r.why || (r.code === 200 ? 'not a transmitter list' : ''));
    } else {
      const mine = list.filter((t) => t && want.has(noradOf(t.norad_cat_id))).slice(0, 2000).map((t) => ({
        norad: noradOf(t.norad_cat_id), description: String(t.description || '').slice(0, 80), alive: t.alive !== false,
        status: String(t.status || ''), mode: String(t.mode || ''), baud: Number(t.baud) || 0,
        low: Number(t.downlink_low) || 0, high: Number(t.downlink_high) || 0, type: String(t.type || '') }));
      await putKv(env, 'satnogs', { fetched: now, tx: mine }, now);
      s.satnogs.at = now; s.satnogs.fails = 0; s.satnogs.lastCode = 200; s.satnogs.lastError = '';
      changed = true;
    }
  }

  if (!stopped) {
    s.holdUntil = 0; s.fails = 0; s.f403 = 0; s.lastCode = 200; s.lastError = ''; s.lastGoodAt = now; s.fastFails = 0;
    noteHistory(s, { at: now, source: 'celestrak', outcome: 'good', code: 200, group: summary.updated.join(' '), message: '' });
  }
  await putKv(env, 'state', s, now);
  if (changed || !(await getKv(env, 'sats'))) summary.built = await rebuild(env, curated, now);
  return summary;
}

/** SatNOGS's word on one curated downlink: a transmitter of the same bird within ±15 kHz (or, for "follow":"mode", the
 *  alive transmitter of the same mode). Returns { status, downlink? }. */
export function satnogsFor(tx, norad, list) {
  const mine = (list || []).filter((t) => t.norad === norad && t.low > 0);
  if (tx.follow === 'mode') {
    const m = mine.filter((t) => t.alive && t.status === 'active' && t.mode.toLowerCase().includes(tx.mode.toLowerCase()));
    if (m.length === 1 && m[0].low >= 136e6 && m[0].low <= 138e6) return { status: 'active', downlink: m[0].low };
  }
  const near = mine.filter((t) => Math.abs(t.low - tx.downlink) <= 15000);
  if (!near.length) return { status: '' };
  if (near.some((t) => t.alive && t.status === 'active')) return { status: 'active' };
  return { status: 'inactive' };
}

/** ★ beta91: every group a round asks — the main groups first, the label-only ones after (a failure there cannot cost
 *  the main groups their round's fetch). */
export function roundGroups(curated) { return [...curated.groups, ...(curated.labelGroups || []).filter((g) => !curated.groups.includes(g))]; }

/** ★ beta91: a label group's rows worth keeping — its constellation's birds (OBJECT_NAME starts with the prefix), at most
 *  LABEL_GROUP_ROWS, by catalogue number. */
export function constellationRows(curated, group, omm) {
  const c = (curated.constellations || []).find((x) => x.group === group);
  const pre = c && c.prefix ? String(c.prefix).toUpperCase() : '';
  return omm.filter((o) => !pre || String(o.OBJECT_NAME || '').toUpperCase().startsWith(pre))
            .sort((a, b) => a.NORAD_CAT_ID - b.NORAD_CAT_ID).slice(0, LABEL_GROUP_ROWS);
}

/** ★ beta91: a sats.json id from a CelesTrak name — "ORBCOMM FM108" → "ORBCOMM-FM108" (letters, digits, '-'; ≤ 32, the
 *  servers' rule), else the prefix and the catalogue number. */
export function constellationId(prefix, name, norad) {
  const id = String(name || '').toUpperCase().replace(/[^A-Z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 32).replace(/-+$/, '');
  return id || `${String(prefix || 'SAT').toUpperCase().replace(/[^A-Z0-9]+/g, '').slice(0, 16)}-${norad}`;
}

/** ★★ beta91: what SatNOGS DB says about a label-only bird's 137–138 MHz downlink. ORBCOMM satellites change channel,
 *  so a bird's own frequency is used only when SatNOGS lists exactly ONE alive, active one for it ('channel'); every
 *  listing marked not alive = 'dead' (the bird is left out — no label is better than a wrong one); anything else, or no
 *  listing at all = 'band' (the bird is named over the whole band). */
export function satnogsChannel(norad, list, lo = 137e6, hi = 138e6) {
  const mine = (list || []).filter((t) => t.norad === norad && t.low >= lo && t.low <= hi);
  if (!mine.length) return { state: 'band' };
  if (mine.every((t) => t.alive === false)) return { state: 'dead' };
  const f = [...new Set(mine.filter((t) => t.alive && t.status === 'active').map((t) => t.low))];
  return f.length === 1 ? { state: 'channel', downlink: f[0] } : { state: 'band' };
}

/** sats.json from the curated list and the good copies. */
export async function rebuild(env, curated, now) {
  const groups = {};
  const byNorad = new Map();
  for (const g of curated.groups) {
    const c = await getKv(env, `omm:${g}`);
    if (!c) continue;
    groups[g] = { fetched: new Date(c.fetched * 1000).toISOString() };
    for (const o of c.omm) byNorad.set(o.NORAD_CAT_ID, o);
  }
  /* ★★ NEVER A sats.json WITHOUT ORBITS (2026-11-20, the first live round: CelesTrak answered 522 for the amateur group,
   *  the round stopped as it must, and the mirror then published the 13 curated birds with `orbits: {}` — a list no
   *  server can place or predict a pass for, which a server would have kept for a day). With no group's good copy yet,
   *  nothing is published: GET /api/sats stays 503 + Retry-After and servers retry soon. Once a good copy exists, a
   *  later failure keeps serving it (the `omm:<group>` rows are only ever replaced by a good round). */
  if (!Object.keys(groups).length) return false;
  const sn = await getKv(env, 'satnogs');
  const sats = curated.satellites.map((x) => {
    const norad = noradOf(x.norad);
    const tx = x.transmitters.map((t) => {
      const o = { ...t };
      if (sn) {
        const w = satnogsFor(t, norad, sn.tx);
        if (w.status) o.status = w.status;
        if (w.downlink && w.downlink !== t.downlink) o.downlink = w.downlink;
      }
      return o;
    });
    const out = { id: x.id, name: x.name, norad, group: x.group, note: x.note, transmitters: tx };
    if (byNorad.has(norad)) out.omm = byNorad.get(norad);
    return out;
  });
  // ★★ beta91 the label-only birds (constellations), after the curated ones, never past SATS_JSON_MAX in all
  const labelOrbits = {};
  const ids = new Set(sats.map((x) => x.id));
  for (const c of curated.constellations || []) {
    const cp = await getKv(env, `omm:${c.group}`);
    if (!cp) continue;
    labelOrbits[c.group] = { fetched: new Date(cp.fetched * 1000).toISOString() };
    let n = 0;
    for (const o of cp.omm) {
      if (n >= Math.min(c.max || LABEL_SAT_MAX, LABEL_SAT_MAX) || sats.length >= SATS_JSON_MAX) break;
      const norad = noradOf(o.NORAD_CAT_ID);
      if (!norad || sats.some((x) => x.norad === norad)) continue;
      const w = sn ? satnogsChannel(norad, sn.tx) : { state: 'band' };
      if (w.state === 'dead') continue;
      const tx = { ...c.transmitter };
      if (w.state === 'channel') { tx.downlink = w.downlink; delete tx.downlinkHigh; }
      let id = constellationId(c.prefix, o.OBJECT_NAME, norad);
      if (ids.has(id)) id = `${id.slice(0, 22)}-${norad}`;
      ids.add(id);
      sats.push({ id, name: String(o.OBJECT_NAME || id).slice(0, 64), norad, group: c.group, note: c.note || '', labelOnly: true,
                  transmitters: [tx], omm: o });
      n++;
    }
  }
  const doc = {
    format: 1,
    generated: new Date(now * 1000).toISOString(),
    attribution: {
      orbits: 'Orbits: CelesTrak (celestrak.org) GP data',
      transmitters: sn ? 'Transmitters: curated by VibeSDR from AMSAT and SatNOGS DB (db.satnogs.org, CC BY-SA 4.0)' : 'Transmitters: curated by VibeSDR from AMSAT and SatNOGS',
    },
    licence: sn ? 'Transmitter data CC BY-SA 4.0 (SatNOGS DB)' : '',
    orbits: groups,
    ...(Object.keys(labelOrbits).length ? { labelOrbits } : {}),
    satellites: sats,
  };
  const body = JSON.stringify(doc);
  const hash = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(body));
  const etag = '"' + [...new Uint8Array(hash)].slice(0, 12).map((b) => b.toString(16).padStart(2, '0')).join('') + '"';
  const prev = await getKv(env, 'sats');
  if (prev && prev.etag === etag) return false;
  await putKv(env, 'sats', { body, etag, modified: new Date(now * 1000).toUTCString() }, now);
  return true;
}

/** GET /api/sats — sats.json with an ETag / Last-Modified; 304 for a server that already has it. */
export async function serveSats(request, env) {
  const s = await getKv(env, 'sats');
  if (!s) return new Response(JSON.stringify({ error: 'the satellite mirror has not run yet' }), {
    status: 503, headers: { 'content-type': 'application/json', 'retry-after': '900', 'cache-control': 'no-store' } });   // ★ the ladder's first rung
  const h = { 'content-type': 'application/json; charset=utf-8', etag: s.etag, 'last-modified': s.modified,
              'cache-control': 'public, max-age=3600', 'access-control-allow-origin': '*' };
  const inm = request.headers.get('if-none-match');
  const ims = request.headers.get('if-modified-since');
  if ((inm && inm.split(',').map((x) => x.trim()).includes(s.etag)) || (!inm && ims && Date.parse(ims) >= Date.parse(s.modified)))
    return new Response(null, { status: 304, headers: h });
  return new Response(s.body, { status: 200, headers: h });
}

/** GET /api/sats/status — the mirror's state for a person: the last error in words, the holds, the copies' ages. */
export async function serveSatsStatus(env) {
  const s = await loadState(env);
  const now = Math.floor(Date.now() / 1000);
  const out = { holdUntil: holdInForce(s, now), mode: freshCopy(s, now) ? 'fresh: every 6 h' : 'no / old copy: 15 min → 30 min → 1 h → hourly',
                lastGoodAt: s.lastGoodAt, fastFails: s.fastFails, lastRound: s.lastRound, lastCode: s.lastCode, lastError: s.lastError, lastErrorAt: s.lastErrorAt,
                dead: Object.keys(s.dead), moved: s.moved, satnogs: s.satnogs, groups: {},
                // ★ the last rounds, newest first, times as UTC text for a person (and `at` for a script)
                history: (s.history || []).map((h) => ({ ...h, when: new Date(h.at * 1000).toISOString().replace('T', ' ').slice(0, 16) + ' UTC' })) };
  for (const g of ['amateur', 'weather', 'orbcomm']) { const c = await getKv(env, `omm:${g}`); if (c) out.groups[g] = { fetched: c.fetched, count: c.count, url: c.url }; }
  return new Response(JSON.stringify(out), { headers: { 'content-type': 'application/json', 'cache-control': 'no-store' } });
}
