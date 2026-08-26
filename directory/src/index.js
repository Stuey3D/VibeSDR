/**
 * vibeserver.vibesdr.net — the public VibeServer directory.
 *
 * ★★★ THE ADDRESS IS A FIELD, NOT AN IDENTITY. A Quick Tunnel gets a NEW hostname every time it
 *     restarts, so nothing here may key on the URL. The listing is keyed on a server-issued id and
 *     the server re-registers; the human sees the operator's chosen NAME and their locator.
 *     Stuart, 2026-08-22: "The address doesn't matter at that point as you won't be typing it
 *     directly as the directory does the hard work."
 *
 * ★★★ THE KEY IS THE IDENTITY, SO IT IS STORED HASHED AND NEVER ECHOED. Anyone holding it can
 *     take over or delist a listing. We issue it once, at registration, and thereafter only ever
 *     compare hashes.
 */

const PING_SEC = 900;              // 15 minutes — the sdr.hu interval, arrived at independently.
const DEFAULT_TTL_MIN = 30;        // ★ Two missed pings, not one: a single lost request is normal.
const MAX_TTL_MIN = 60 * 24;
const REG_PER_HOUR = 10;           // per source address

// ★★★ WHERE SHAREABLE ADDRESSES LIVE. Chosen over the apex (dave.vibesdr.net, also free) so that
//     user-chosen names are identifiable as VibeServer addresses and stay out of the namespace
//     that holds demo/www/api. Cloudflare's cert for the custom domain already covers one wildcard
//     level beneath it, so this costs nothing.
const PUBLIC_ZONE = 'vibeserver.vibesdr.net';

/**
 * ★★★ HOW LONG AN ADDRESS IS HELD AFTER THE SERVER LAST CHECKED IN. Stuart, 2026-08-22: "we tag
 *     the address to the ID for a week or so and then if the server hasn't checked in in a week we
 *     release the address."
 *
 * ★★ THIS IS THE MIDDLE GROUND AND BOTH EXTREMES ARE WRONG. Releasing on EXPIRY (30 min) would
 *    mean a receiver switched off overnight loses the address its owner has already shared.
 *    Holding FOR EVER would mean a name tried once and abandoned is gone permanently.
 *
 * ★★★ AND THE DIRECTION OF FAILURE MATTERS. Releasing a name means somebody else can take it, and
 *     then links already shared land on A STRANGER'S RECEIVER rather than breaking honestly. That
 *     is the real cost of reuse, and the week IS the mitigation — long enough that a holiday, a
 *     house move or a dead SD card does not cost you your address.
 *
 * ★ Refreshes itself: every ping sets updated_at, so a live server never approaches this.
 */
const ADDRESS_HOLD_MAX = 7 * 86400;   // an established server keeps its name for a week
const ADDRESS_HOLD_MIN = 3600;        // an experiment keeps it for an hour

/**
 * ★★★ THE HOLD IS AS LONG AS THE LISTING WAS ACTUALLY USED, CAPPED AT A WEEK.
 *
 * A flat week has a nasty failure that Stuart spotted immediately: someone who reinstalls loses
 * the id and key stored with their config, re-registers, and is BLOCKED FROM THEIR OWN NAME BY
 * THEIR OWN DEAD ENTRY — offered "dave1" because of a ghost they cannot delete. Punishing a user
 * for their own abandoned listing is the worst version of this.
 *
 * ★★ So: you hold your address for as long as you have been using it. A server that registered,
 *    pinged twice and vanished — an experiment, or the install being replaced — gives its name up
 *    within the hour. A receiver that has been listed for weeks keeps it for the full week, which
 *    is what the hold was FOR: a holiday, a house move, a dead SD card.
 *
 * ★ Turning the switch OFF still frees the name immediately, via delist. This only governs the
 *   case where a server simply stopped talking to us.
 *
 * Expressed in SQL so it is evaluated per row: a row holds its slug while
 *   updated_at > now - clamp(updated_at - created_at, MIN, MAX)
 */
const HOLD_SQL = `min(max(updated_at - created_at, ${ADDRESS_HOLD_MIN}), ${ADDRESS_HOLD_MAX})`;

const json = (body, status = 200, extra = {}) =>
  new Response(JSON.stringify(body), {
    status,
    headers: {
      'content-type': 'application/json; charset=utf-8',
      // ★ The apps fetch this cross-origin. Read-only data, deliberately public.
      'access-control-allow-origin': '*',
      ...extra,
    },
  });

const now = () => Math.floor(Date.now() / 1000);

async function sha256Hex(s) {
  const buf = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

/** ★ Constant time: a length-independent compare so a wrong key cannot be found a character at a
 *  time. Both sides are fixed-length hex hashes, so length equality is expected, not secret. */
function timingSafeEqual(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

/**
 * Maidenhead locator to the CENTRE of the square.
 *
 * ★★ The centre, not the corner. A 4-character square is ~70 x 110 km; pinning its south-west
 *    corner would put every marker consistently down-left of where the operator actually is, and
 *    for a coastal square that can be in the sea.
 */
function gridToLatLon(grid) {
  const g = String(grid || '').trim().toUpperCase();
  if (!/^[A-R]{2}[0-9]{2}([A-X]{2})?$/.test(g)) return null;
  let lon = (g.charCodeAt(0) - 65) * 20 - 180;
  let lat = (g.charCodeAt(1) - 65) * 10 - 90;
  lon += Number(g[2]) * 2;
  lat += Number(g[3]) * 1;
  if (g.length === 6) {
    lon += (g.charCodeAt(4) - 65) * (2 / 24) + (2 / 24) / 2;
    lat += (g.charCodeAt(5) - 65) * (1 / 24) + (1 / 24) / 2;
  } else {
    lon += 1;        // half of 2 degrees
    lat += 0.5;      // half of 1 degree
  }
  return { lat, lon };
}

/** ★ Strip control characters and cap the length. Operator-supplied text reaches the page. */
const clean = (s, max) => String(s ?? '').replace(/[\x00-\x1f\x7f]/g, '').trim().slice(0, max);

/**
 * ★★★ THE SAME SLUG RULE AS THE SERVER'S mdnsLabel(). Do not "improve" it here.
 *     vibeserver_config.cpp:156 is the original and vibe_setup_page.h:859 already mirrors it in JS
 *     so the setup page can show the address live as the owner types. This is the THIRD copy of
 *     one rule; if they ever disagree, the address a user is shown is not the address they get.
 *     Lowercase, non-alphanumerics collapsed to a single dash, trimmed, 63 max = one DNS label.
 */
function slugify(s) {
  let out = '', dash = false;
  for (const ch of String(s || '')) {
    if (/[a-z0-9]/i.test(ch)) { out += ch.toLowerCase(); dash = false; }
    else if (out && !dash) { out += '-'; dash = true; }
  }
  out = out.replace(/-+$/, '');
  return out.slice(0, 63).replace(/-+$/, '');
}

/**
 * ★★★ NAMES WE WILL NOT ISSUE.
 *
 * "vibeserver" is the important one and it is a SCAR, not a precaution: mdnsLabel("") falls back
 * to it, and main.cpp:1290 records an unnamed laptop taking `vibeserver.local` away from the Pi
 * until SSH to it started failing. Publicly it is worse — the first unnamed server would claim
 * vibeserver.vibeserver.vibesdr.net and every other unnamed one would collide with it.
 * ★ So an empty or fallback name is REFUSED, never auto-issued: "no name yet" and "not ready to
 *   advertise" are one state, exactly as mDNS already treats them.
 */
const RESERVED = new Set([
  'vibeserver', 'vibesdr', 'www', 'api', 'demo', 'mail', 'admin', 'root', 'static',
  'cdn', 'assets', 'status', 'help', 'support', 'app', 'web', 'test', 'dev', 'staging',
]);

/**
 * Is this slug free? Reserved names fail; taken names fail only while their holder still HOLDS it.
 * ★ A slug on a server that has not checked in for ADDRESS_HOLD_DAYS is available again.
 */
async function slugFree(env, slug) {
  if (!slug || slug.length < 2 || RESERVED.has(slug)) return false;
  const row = await env.DB.prepare(
    `SELECT 1 AS x FROM servers WHERE slug = ? AND updated_at > (? - ${HOLD_SQL})`
  ).bind(slug, now()).first();
  return !row;
}

/**
 * ★★ Release a lapsed hold so the new owner can take the name.
 *
 * The unique index means the stale row must give the slug up before anyone else can hold it. We
 * clear it rather than deleting the row: the old server keeps its id and key, so if it ever
 * returns it can still ping, still be listed, and simply be told its address has gone.
 */
async function releaseLapsedSlug(env, slug) {
  await env.DB.prepare(
    `UPDATE servers SET slug = NULL WHERE slug = ? AND updated_at <= (? - ${HOLD_SQL})`
  ).bind(slug, now()).run();
}

/**
 * What to offer when the name is taken. Stuart, 2026-08-22: the second Dave "could be given the
 * choice of dave1.vibeserver.vibesdr.net or daveio92nh.vibeserver.vibesdr.net".
 * ★ The locator suffix is the more useful of the two — it says WHERE, which is what actually
 *   distinguishes two Daves — so it is offered first when we have one.
 */
async function suggestions(env, base, locator) {
  const out = [];
  const grid = slugify(locator || '');
  if (grid && await slugFree(env, `${base}${grid}`)) out.push(`${base}${grid}`);
  for (let i = 1; i <= 9 && out.length < 4; i++) {
    const s = `${base}${i}`;
    if (await slugFree(env, s)) out.push(s);
  }
  return out;
}

/**
 * ★★★ WHAT WE ACCEPT AS AN ADDRESS. It is published and clicked, so it must be a plain http(s)
 *     URL and nothing else — a `javascript:` or `data:` URL here would be a stored XSS delivered
 *     to every visitor of the directory.
 */
function validUrl(u) {
  let parsed;
  try { parsed = new URL(String(u)); } catch { return null; }
  if (parsed.protocol !== 'https:' && parsed.protocol !== 'http:') return null;
  if (!parsed.hostname) return null;
  return parsed.origin;
}

async function readBody(request) {
  try { return await request.json(); } catch { return null; }
}

async function findServer(env, id) {
  return env.DB.prepare('SELECT * FROM servers WHERE id = ?').bind(String(id || '')).first();
}

/** Shared by ping and delist: the caller must hold the key. */
async function authed(env, body) {
  const row = await findServer(env, body?.id);
  if (!row) return { error: json({ error: 'unknown server' }, 404) };
  const given = await sha256Hex(String(body?.key || ''));
  if (!timingSafeEqual(given, row.key_hash)) {
    // ★ A distinct 403 is what lets an operator whose config was restored from a backup
    //   understand why they are not listed.
    return { error: json({ error: 'bad key' }, 403) };
  }
  return { row };
}

/**
 * ★★★ HOW LONG THIS SHARE IS OFFERED FOR — an ABSOLUTE end, not a lifetime, so a ping cannot keep
 *     nudging it into the future. "A week" means a week from when it was set, not a week from
 *     whenever the server last spoke.
 * ★ 0 / absent = permanent, which is every server that has not asked for anything else. Capped at
 *   a year: past that it is a permanent server with extra steps.
 */
function untilFrom(body, prev) {
  if (body.shareForSec === 0 || body.shareForSec === null) return 0;   // explicitly permanent
  const n = Number(body.shareForSec);
  if (!Number.isFinite(n) || n <= 0) return Number(prev) || 0;         // said nothing: keep
  return now() + Math.min(Math.floor(n), 365 * 86400);
}

function ttlSeconds(body) {
  const n = Number(body?.ttlMin);
  if (!Number.isFinite(n) || n <= 0) return DEFAULT_TTL_MIN * 60;
  return Math.min(Math.max(Math.floor(n), 1), MAX_TTL_MIN) * 60;
}

/**
 * ★★★ PROVE THE ADDRESS IS THIS RECEIVER, NOT JUST A RECEIVER.
 *
 * Registering is a CLAIM: "listen to me at <url>". Nothing in it was checked, so anybody could
 * list somebody else's server under their own name, or point an entry at a site that has never
 * heard of us — and the directory's whole job is telling strangers where to go.
 *
 * ★★★ THE KEY NEVER CROSSES THE WIRE. The probe may run over plain HTTP to somebody's own port —
 *     their machine, their choice — so echoing the key would put the identity of the listing in
 *     the clear on every check, and anyone on the path could take the listing over. A nonce out,
 *     an HMAC back: what a listener sees is single-use and worth nothing.
 *
 * ★★ VERIFIED ON PING, NOT AT REGISTRATION, and that is forced by the order of things: the server
 *    cannot answer a challenge with a key it has not been given yet, and we issue the key in the
 *    registration RESPONSE. So a new listing exists immediately and is SHOWN once it answers.
 * ★ Failure is not an error to the caller. A server that cannot answer yet keeps its entry and
 *   simply is not listed — it may be mid-restart, and dropping it would punish a blip.
 */
/** ★ `why` is filled in on failure so the PING can tell the owner what went wrong — an address
 *  that silently refuses to list is the worst possible answer. It never carries anything about the
 *  key itself. */
async function verifyAddress(url, key, why) {
  const nonce = crypto.randomUUID().replace(/-/g, '');
  const target = `${url}/vibeserver.json?dirNonce=${nonce}`;
  try {
    const res = await fetch(target, {
      // ★★★ GENEROUS ON PURPOSE — A BUSY SERVER IS NOT AN ABSENT ONE. Eight seconds is fine for a
      //     Pi with nobody on it and tight for a low-end phone serving listeners: the Xcover 4S
      //     answered this challenge in 0.65 s idle and could not answer it at all while streaming,
      //     so every app restart (which rotates the tunnel hostname and forces a re-proof) dropped
      //     a working, reachable receiver out of the directory until the next retry (2026-08-23).
      //  ★★ The failure is silent to the owner — the switch still reads ON and the server still
      //     works — which makes it the worst kind of wrong. The retry ladder does recover it, but
      //     punishing the exact machines this feature is meant to show off is a poor trade for
      //     seven seconds of a Worker's time.
      //  ★ It does NOT weaken the check: a wrong or missing HMAC still fails, however long it
      //    takes to arrive. Only patience changed.
      signal: AbortSignal.timeout(15000),
      cache: 'no-store',
      // ★ Some receivers refuse a request with no user agent — ours does.
      headers: { 'user-agent': 'vibesdr.net directory verifier' },
    });
    if (why) why.status = res.status;
    if (!res.ok) { if (why) why.reason = 'http'; return false; }
    const j = await res.json();
    const given = typeof j?.dirProof === 'string' ? j.dirProof.toLowerCase() : '';
    if (given.length !== 64) { if (why) why.reason = 'no-proof'; return false; }

    const mac = await crypto.subtle.importKey(
      'raw', new TextEncoder().encode(key), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const sig = await crypto.subtle.sign('HMAC', mac, new TextEncoder().encode(nonce));
    const want = [...new Uint8Array(sig)].map((b) => b.toString(16).padStart(2, '0')).join('');
    const ok = timingSafeEqual(given, want);
    if (why && !ok) why.reason = 'mismatch';
    return ok;
  } catch (e) {
    if (why) why.reason = 'threw: ' + String((e && e.message) || e).slice(0, 80);
    return false;                      // unreachable, too slow, or not a VibeServer
  }
}

async function register(request, env) {
  const body = await readBody(request);
  if (!body) return json({ error: 'bad json' }, 400);

  const name = clean(body.name, 60);
  if (name.length < 2) return json({ error: 'a public server name is required' }, 400);

  const grid = clean(body.grid, 6).toUpperCase();
  const pos = gridToLatLon(grid);
  if (!pos) return json({ error: 'a valid Maidenhead locator is required (e.g. IO83 or IO83xk)' }, 400);

  const url = validUrl(body.url);
  if (!url) return json({ error: 'url must be a plain http(s) address' }, 400);

  const kind = body.kind === 'tunnel' ? 'tunnel' : 'direct';

  // ★ Rate limit per source address. Registration is the only write a stranger can drive.
  const ip = request.headers.get('cf-connecting-ip') || '';
  const since = now() - 3600;
  const recent = await env.DB.prepare('SELECT COUNT(*) AS n FROM reg_log WHERE ip = ? AND at > ?')
    .bind(ip, since).first();
  if (Number(recent?.n || 0) >= REG_PER_HOUR) {
    return json({ error: 'too many registrations from this address, try later' }, 429);
  }

  // ★★★ THE SHAREABLE ADDRESS. Derived from the operator's friendly name the same way the .local
  //     label is, and then FROZEN — see migrations/0002-slugs.sql. A caller may name the slug it
  //     wants (having been offered a choice when its first pick was taken); otherwise we derive.
  const wanted = slugify(body.slug || name);
  if (!wanted || wanted.length < 2) {
    return json({ error: 'that name cannot be turned into an address' }, 400);
  }
  // ★★★ A RESERVED NAME IS A DEAD END, WITH NO NEAR-MISS OFFERED — see checkName(). Checked
  //     BEFORE the taken-check so it can never be reported as merely "taken", which would invite
  //     the caller to retry with vibeserver1.
  if (RESERVED.has(wanted)) {
    return json({ error: 'that name is reserved — please choose a different one', slug: wanted }, 409);
  }
  if (!await slugFree(env, wanted)) {
    /**
     * ★★★ A SERVER MUST BE ABLE TO RECLAIM ITS OWN NAME. AN OUTAGE MUST NOT COST IT.
     *
     * Stuart's Pi, 2026-08-26: a dropped internet connection made its client mistake "cannot
     * reach the directory" for "your row is gone", and it deleted the id and key it is issued
     * ONCE. It then re-registered and was refused BY ITS OWN ENTRY — "that address is taken" —
     * and the hold is proportional to how long it had been listed, so a working, reachable
     * receiver was locked out of its own address for the better part of a week, with nothing
     * said to its owner. The client half of that is fixed; this is the half that matters even
     * when a client loses its key for some other reason entirely (a wiped SD card, a reinstall,
     * a restore from backup).
     *
     * ★★ SAME ADDRESS ⇒ SAME SERVER. The hold exists to stop a STRANGER taking a name while its
     *    owner is away — it was never meant to stop the owner coming back. Matching on `url` is
     *    what separates those two cases, and it cannot be abused: to claim a held name this way
     *    you must already be serving at the exact address the holder published, and control of
     *    that address is precisely what the listing asserts. The verification challenge still
     *    has to pass afterwards, so a wrong claim is listed by nobody.
     *
     * ★ The old row keeps its id and key and only gives up the slug — the same choice
     *   releaseLapsedSlug makes, and for the same reason: if it ever returns it can still ping
     *   and simply be told its address has gone.
     */
    const holder = await env.DB.prepare(
      'SELECT id, url FROM servers WHERE slug = ?'
    ).bind(wanted).first();

    if (holder && holder.url === url) {
      await env.DB.prepare('UPDATE servers SET slug = NULL WHERE id = ?').bind(holder.id).run();
    } else {
      // ★ 409 with alternatives, so the app can put them straight into its dropdown rather than
      //   making the owner guess what is free.
      return json({
        error: 'that address is taken',
        slug: wanted,
        suggestions: await suggestions(env, wanted, body.locator || grid),
      }, 409);
    }
  }

  // ★ The name is free, but a LAPSED holder may still be sitting on the unique index.
  await releaseLapsedSlug(env, wanted);

  const id = crypto.randomUUID();
  const key = crypto.randomUUID().replace(/-/g, '') + crypto.randomUUID().replace(/-/g, '');
  const t = now();

  // ★ cf.country is the country the SERVER dialled us from — it POSTs to us directly, not through
  //   its own tunnel — so it is the operator's country, not a Cloudflare edge. Two letters only.
  const country = clean(request.cf?.country || '', 2).toUpperCase();

  await env.DB.batch([
    env.DB.prepare(
      `INSERT INTO servers (id, key_hash, name, url, kind, grid, lat, lon, country,
                            status_json, created_at, updated_at, expires_at, slug, until)
       VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`
    ).bind(id, await sha256Hex(key), name, url, kind, grid, pos.lat, pos.lon, country,
           JSON.stringify(body.status || {}), t, t, t + ttlSeconds(body), wanted,
           untilFrom(body, 0)),
    env.DB.prepare('INSERT INTO reg_log (ip, at) VALUES (?,?)').bind(ip, t),
    // ★ Housekeeping on the write path rather than a cron: free, and cron is one more thing to fail.
    env.DB.prepare('DELETE FROM reg_log WHERE at < ?').bind(since),
  ]);

  // ★★★ THE ONLY TIME THE KEY IS EVER SENT. It cannot be recovered; a lost key means re-register.
  //     `address` is what the switch shows underneath itself — the thing the owner shares.
  return json({ id, key, pingSec: PING_SEC, slug: wanted, address: `${wanted}.${PUBLIC_ZONE}` });
}

async function ping(request, env) {
  const body = await readBody(request);
  if (!body) return json({ error: 'bad json' }, 400);
  const { row, error } = await authed(env, body);
  if (error) return error;

  const t = now();
  // ★★★ A server may move address between pings — a Quick Tunnel does exactly that on every
  //     restart — so the URL is REFRESHED here, not frozen at registration.
  const url = body.url ? validUrl(body.url) : row.url;
  if (body.url && !url) return json({ error: 'url must be a plain http(s) address' }, 400);

  // ★★ A PING THAT OMITS status MUST NOT WIPE THE RADIO LIST. Status is replaced wholesale when it
  //    is sent — that is what makes a radio disappearing from the server disappear here too — but
  //    "said nothing" and "said I have no radios" are different statements, and only the second
  //    should empty the entry.
  const status = (body.status && typeof body.status === 'object')
    ? JSON.stringify(body.status)
    : row.status_json;

  // ★★★ PROVE THE ADDRESS WHILE IT IS UNPROVEN, AND AGAIN WHENEVER IT CHANGES. A Quick Tunnel
  //     hostname rotates on every restart, so "verified once" would leave a proven server quietly
  //     carrying an unproven address for the rest of its life — which is exactly the claim the
  //     challenge exists to check. The key does not change across the move, so re-proving costs
  //     one request and settles it.
  // ★★★ A RECEIVER CAN MOVE. The locator was only ever read at registration, so a server that
  //     was carried somewhere else kept its original pin — and the map is the directory's front
  //     page. Accepted on every ping, validated exactly as at registration, and IGNORED when it is
  //     absent or malformed rather than blanking a good position with a bad one.
  //  ★ The country follows the position, not the request: cf.country is where the server DIALLED
  //    FROM, which is the same thing while it is at home and wrong the moment it travels — a phone
  //    on a foreign SIM would file itself under the wrong flag. The grid the owner's own device
  //    resolved is the better answer, and the one the map already uses.
  let gridPos = null;
  if (typeof body.grid === 'string' && body.grid.trim()) {
    const g = clean(body.grid, 6).toUpperCase();
    const p2 = gridToLatLon(g);
    if (p2) gridPos = { grid: g, lat: p2.lat, lon: p2.lon };
  }

  const moved = url !== row.url;
  let verified = Number(row.verified) === 1 && !moved;
  const why = {};
  if (!verified) verified = await verifyAddress(url, String(body.key || ''), why);

  await env.DB.prepare(
    `UPDATE servers SET url = ?, name = ?, status_json = ?, updated_at = ?, expires_at = ?,
                        verified = ?, grid = ?, lat = ?, lon = ?, until = ?,
                        verify_note = ?
     WHERE id = ?`
  ).bind(url, body.name ? clean(body.name, 60) : row.name,
         status, t, t + ttlSeconds(body), verified ? 1 : 0,
         gridPos ? gridPos.grid : row.grid,
         gridPos ? gridPos.lat : row.lat,
         gridPos ? gridPos.lon : row.lon,
         untilFrom(body, row.until),
         // ★ Why it failed, kept on the row so an owner (and whoever is debugging) can see it
         //   without a log pipeline. Cleared the moment it succeeds.
         verified ? '' : JSON.stringify(why).slice(0, 200),
         row.id).run();

  // ★★ TELL A RETURNING SERVER THE TRUTH ABOUT ITS ADDRESS. If it was away longer than the hold
  //    and somebody else took the name, `slug` is now NULL — the switch must be able to say so
  //    rather than keep showing an address that belongs to a stranger.
  return json({
    listed: true,
    // ★ Say whether the address proved itself, rather than leaving an owner to wonder why a
    //   perfectly live server is not on the map.
    verified,
    verifyWhy: verified ? undefined : why,
    pingSec: PING_SEC,
    slug: row.slug || null,
    address: row.slug ? `${row.slug}.${PUBLIC_ZONE}` : null,
  });
}

async function delist(request, env) {
  const body = await readBody(request);
  if (!body) return json({ error: 'bad json' }, 400);
  const { row, error } = await authed(env, body);
  if (error) return error;
  await env.DB.prepare('DELETE FROM servers WHERE id = ?').bind(row.id).run();
  // ★ Immediate, per the privacy rule: one-press delist, effective now, not at the next expiry.
  return json({ delisted: true });
}

async function list(env) {
  // ★★★ EXPIRY EVALUATED AT READ TIME. Nothing sweeps; a server that stopped pinging is simply
  //     not selected. See schema.sql.
  const { results } = await env.DB.prepare(
    `SELECT id, name, url, kind, grid, lat, lon, country, status_json, updated_at, expires_at, slug,
            until
       FROM servers WHERE expires_at > ? AND verified = 1
                      AND (until = 0 OR until > ?) ORDER BY country, name`
  ).bind(now(), now()).all();

  const servers = (results || []).map((r) => {
    let status = {};
    try { status = JSON.parse(r.status_json) || {}; } catch { status = {}; }
    return {
      id: r.id, name: r.name, url: r.url, kind: r.kind,
      slug: r.slug || null,
      // ★ The address a listener should be given and a friend should be sent. The tunnel hostname
      //   rotates; this does not.
      address: r.slug ? `${r.slug}.${PUBLIC_ZONE}` : null,
      grid: r.grid, lat: r.lat, lon: r.lon, country: r.country,
      // ★ Passed through whole, `id` included — the page addresses each radio's own
      //   /r/<id>/vibeserver.json to refresh a count the ping cannot keep current.
      radios: Array.isArray(status.radios) ? status.radios : [],
      // ★★★ SAID, NOT INFERRED. The page guessed "temporary share" from how far the expiry sat
      //     from the last ping, so an ordinary listing with a 30-minute TTL was drawn as a yellow
      //     diamond — a product concept invented out of a timing value. A server says whether it
      //     is a temporary share; if it says nothing, it is not one.
      // ★★ TEMPORARY IS A FACT ABOUT THE OFFER, read from the clock that governs it rather than
      //    from a flag anybody could forget to clear.
      temporary: Number(r.until) > 0,
      until: Number(r.until) || 0,
      // ★★ WHAT THE RECEIVER IS, not just where it is. A listener choosing between servers wants
      //    the aerial and the machine — "YouLoop into an LNA, on a phone" tells them far more than
      //    a hostname does. Absent stays absent: a server that has not said is not guessed at.
      // ★ A locked receiver is still worth listing — a club's members need to find it — but a
      //   stranger must be able to see it is not for them before they click.
      pin: !!status.pin,
      antenna: typeof status.antenna === 'string' ? status.antenna.slice(0, 120) : '',
      host: typeof status.host === 'string' ? status.host.slice(0, 80) : '',
      // ★ How the server names its own OS — "Android 16", "Debian 13". Text only, never a logo
      //   or an icon URL: what a server says about itself must stay text, or one day a listing
      //   carries an image. Not lowercased — it is a NAME, and "macOS" is not "macos".
      platform: typeof status.platform === 'string' ? status.platform.slice(0, 40) : '',
      // ★ How long a listener gets, said BEFORE they click rather than when they are cut off.
      limitMin: Number(status.limitMin) > 0 ? Number(status.limitMin) : 0,
      listeners: Number(status.listeners || 0),
      maxListeners: Number(status.maxListeners || 0),
      freeInSec: Number.isFinite(Number(status.freeInSec)) ? Number(status.freeInSec) : -1,
      updatedAt: r.updated_at,
      expiresAt: r.expires_at,
    };
  });
  return json({ servers, count: servers.length, pingSec: PING_SEC });
}

/**
 * Is this public name available, and if not what else could they have?
 *
 * ★★ THIS IS WHAT MAKES THE SWITCH HONEST. The setup page already previews the `.local` label live
 *    as the owner types (vibe_setup_page.h:2075) — this is the same idea for the public address,
 *    so nobody flicks the switch and is then told their name was taken.
 */
async function checkName(url, env) {
  const wanted = slugify(url.searchParams.get('name') || '');
  const locator = url.searchParams.get('locator') || '';
  if (!wanted || wanted.length < 2) {
    return json({ ok: false, reason: 'that name cannot be turned into an address' });
  }
  if (RESERVED.has(wanted)) {
    // ★★★ NO SUGGESTIONS FOR A RESERVED NAME — and that is not tidiness, it is the point of
    //     reserving it. Offering "vibeserver1" to somebody who asked for "vibeserver" hands them
    //     an address that READS AS OFFICIAL: vibeserver1.vibeserver.vibesdr.net is exactly what a
    //     visitor would believe is ours. A reserved word must be a dead end, not a nudge toward a
    //     near-miss of itself.
    // ★ Say WHY. "vibeserver" is the one people will hit by leaving the name blank.
    return json({
      ok: false, slug: wanted,
      reason: 'that name is reserved — please choose a different one',
    });
  }
  if (await slugFree(env, wanted)) {
    return json({ ok: true, slug: wanted, address: `${wanted}.${PUBLIC_ZONE}` });
  }
  return json({
    ok: false, slug: wanted, reason: 'that address is already taken',
    suggestions: (await suggestions(env, wanted, locator)).map((s) => ({ slug: s, address: `${s}.${PUBLIC_ZONE}` })),
  });
}

/**
 * ★★★ <slug>.vibeserver.vibesdr.net — A REDIRECT, NEVER A PROXY.
 *
 * Proxying would put every listener's audio through this Worker: straight into the free tier's
 * 100k requests/day and, worse, it would make us the transit provider for other people's streams —
 * which this whole design has refused twice over. A 302 costs ONE request per click, and the
 * listening itself goes directly to the tunnel edge.
 *
 * ★★ And this is the point of the friendly name: the Quick Tunnel hostname rotates on every
 *    restart, so a link shared with a friend would rot. This does not — it resolves to whatever
 *    the server's latest ping said.
 */
async function serveBySlug(host, request, env) {
  const slug = host.slice(0, -(PUBLIC_ZONE.length + 1)).toLowerCase();
  if (!slug || slug.includes('.')) return null;      // only one label deep

  const row = await env.DB.prepare(
    'SELECT url, name, expires_at, updated_at, created_at FROM servers WHERE slug = ?'
  ).bind(slug).first();

  if (!row) {
    return new Response(
      `No VibeServer is listed at ${slug}.${PUBLIC_ZONE}.`,
      { status: 404, headers: { 'content-type': 'text/plain; charset=utf-8' } }
    );
  }
  if (Number(row.expires_at) <= now()) {
    // ★★ DO NOT PROMISE A RESERVATION THAT HAS LAPSED. Past the hold this name is up for grabs, so
    //    "it will work again when it returns" would be a claim we have stopped honouring.
    const lifetime = Number(row.updated_at) - Number(row.created_at);
    const hold = Math.min(Math.max(lifetime, ADDRESS_HOLD_MIN), ADDRESS_HOLD_MAX);
    const stillHeld = Number(row.updated_at) > now() - hold;
    const tail = stillHeld
      ? 'Its address stays reserved, so this link will work again when it returns.'
      : 'It has been away a long time and this address may be reassigned.';
    return new Response(
      `${row.name} is not online at the moment.\n${tail}`,
      { status: 503, headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' } }
    );
  }

  const origin = validUrl(row.url);
  if (!origin) return new Response('This server published an address we cannot use.', { status: 502 });

  const upstream = new URL(request.url);
  const target = origin + upstream.pathname + upstream.search;

  // ★★★ WE PROXY THE PAGE, NOT THE STREAM — and that distinction is the whole design.
  //
  //     This used to be a 302 to the tunnel, which is cheaper still: one request per click and not
  //     a byte through us. But a Quick Tunnel's hostname ROTATES on every restart, and a browser
  //     keys localStorage by ORIGIN — so a redirect landed every listener on a brand-new origin
  //     and their view settings were gone (Stuart, 2026-08-22: "saving the view settings wont be
  //     remembered"). ★★ The shared-storage cure does not exist: `trycloudflare.com` is on the
  //     Public Suffix List, so each tunnel hostname is its own SITE and browsers partition
  //     third-party storage per site — a vibesdr.net iframe would get a different bucket per
  //     tunnel.
  //
  // ★★★ SO THE HTML AND ITS ASSETS COME THROUGH HERE, ON A STABLE ORIGIN WHOSE STORAGE PERSISTS,
  //     AND THE WEBSOCKET GOES DIRECT. The socket is where the audio and the spectrum live, so the
  //     expensive bytes still never cross this Worker and we are still not anybody's transit
  //     provider — the thing this design has refused from the start. What crosses is a page, some
  //     script, and a few small JSON reads.
  //
  // ★★ A WebSocket upgrade must never be proxied here: the page is told to dial the tunnel itself
  //    (see __VIBE_DIRECT_HOST__ below), so an upgrade arriving at this Worker means something has
  //    gone wrong upstream. Refuse it loudly rather than quietly becoming the stream's relay.
  if ((request.headers.get('upgrade') || '').toLowerCase() === 'websocket') {
    return new Response('This address does not carry the audio stream.', { status: 426 });
  }

  // ★★★ SAY WHO THE VISITOR IS, PLAINLY. Proxying the page means the server sees CLOUDFLARE at the
  //     other end of every HTTP request, not the person — so the landing-page visitor list, the
  //     country breakdown and the ban list all described us instead of them. Stuart spotted it on
  //     his own admin screen: "ON THE LANDING PAGE  2a06:98c0:3600::103" — a Cloudflare address,
  //     sitting there because the directory page polls for live counts (2026-08-22).
  //
  // ★★ FORWARDING THE HEADERS WAS NOT ENOUGH. The chain reaching the receiver is
  //    [browser, cloudflare-edge] -> cloudflared -> loopback, and the shim walks X-Forwarded-For
  //    from the RIGHT taking the first address it does not trust — which is Cloudflare's edge,
  //    because the only trusted entry is loopback. It cannot know our edge is also "us".
  //
  // ★★★ So the header is REPLACED, not appended: exactly one address, the browser's, which is the
  //     only one the receiver has any use for. Right-to-left then lands on the visitor whether the
  //     server trusts one hop or two.
  //  ★ X-Real-IP too, for the same value — the shim reads it when there is no X-Forwarded-For.
  const fwd = new Headers(request.headers);
  const visitor = request.headers.get('cf-connecting-ip');
  if (visitor) { fwd.set('x-forwarded-for', visitor); fwd.set('x-real-ip', visitor); }
  else { fwd.delete('x-forwarded-for'); fwd.delete('x-real-ip'); }

  const res = await fetch(target, {
    method: request.method,
    headers: fwd,
    body: (request.method === 'GET' || request.method === 'HEAD') ? undefined : request.body,
    redirect: 'manual',
  });

  const type = res.headers.get('content-type') || '';

  // ★★★ TELL A NON-BROWSER CLIENT WHERE THE RECEIVER REALLY IS.
  //
  //     The stable address exists to solve a BROWSER problem — localStorage is keyed by origin, and
  //     a rotating tunnel takes it with it. The apps have no such problem: they key their settings
  //     on the server's own `instance` id, and they cannot see the __VIBE_DIRECT_HOST__ we inject
  //     into the HTML because they never load the page. Point one at this address and it proxies
  //     its HTTP happily and then opens a WebSocket against THIS WORKER, which refuses upgrades —
  //     so it fails at the last step with everything before it working (Stuart, 2026-08-22).
  //
  // ★★ /vibeserver.json is what every client reads before it connects, so the answer rides along
  //    with a request that already happens: no new endpoint, no extra round trip, and a client that
  //    does not know the field simply ignores it.
  if (type.includes('application/json') && upstream.pathname.endsWith('/vibeserver.json')) {
    try {
      const j = await res.json();
      j.directHost = new URL(origin).host;
      j.directUrl = origin;
      return new Response(JSON.stringify(j), {
        status: res.status,
        headers: {
          'content-type': 'application/json; charset=utf-8',
          'cache-control': 'no-store',
          // ★★ SO THE DIRECTORY PAGE CAN ASK A SERVER HOW BUSY IT IS, RIGHT NOW. The ping is
          //    liveness on a 15-minute interval — far too slow to answer "is anyone listening",
          //    and pinging fast enough to be live would burn the D1 write budget for the sake of a
          //    number that changes nothing. A tunnelled server is real HTTPS, so the page can read
          //    it straight from the source; this header is the only thing that was in the way.
          'access-control-allow-origin': '*',
        },
      });
    } catch {
      // ★ Not the JSON we expected — pass it through rather than swallow the server's own answer.
    }
  }

  if (!type.includes('text/html')) {
    // ★ Everything that is not the document streams through untouched.
    return new Response(res.body, {
      status: res.status,
      headers: res.headers,
    });
  }

  // ★★ TELL THE PAGE WHERE THE RECEIVER ACTUALLY IS. Injected rather than built into the client,
  //    because the tunnel hostname is not knowable at build time and changes under us.
  const html = await res.text();
  const inject = `<script>window.__VIBE_DIRECT_HOST__=${JSON.stringify(new URL(origin).host)};</script>`;
  const out = html.includes('</head>')
    ? html.replace('</head>', inject + '</head>')
    : inject + html;

  const headers = new Headers(res.headers);
  // ★ The document must not be cached: the host it names changes when the tunnel restarts.
  headers.set('cache-control', 'no-store');
  headers.delete('content-length');
  return new Response(out, { status: res.status, headers });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const p = url.pathname;

    // ★ Anything under the public zone that is not the directory itself is a shareable address.
    const host = url.hostname.toLowerCase();
    if (host.endsWith('.' + PUBLIC_ZONE)) {
      const res = await serveBySlug(host, request, env);
      if (res) return res;
    }

    if (request.method === 'OPTIONS') {
      return new Response(null, {
        headers: {
          'access-control-allow-origin': '*',
          'access-control-allow-methods': 'GET,POST,OPTIONS',
          'access-control-allow-headers': 'content-type',
        },
      });
    }

    try {
      if (p === '/api/directory' && request.method === 'GET') return await list(env);
      if (p === '/api/directory/name' && request.method === 'GET') return await checkName(url, env);
      if (p === '/api/directory/register' && request.method === 'POST') return await register(request, env);
      if (p === '/api/directory/ping' && request.method === 'POST') return await ping(request, env);
      if (p === '/api/directory/delist' && request.method === 'POST') return await delist(request, env);
    } catch (err) {
      // ★ Never leak a D1 error to a caller; it names tables.
      console.error('directory error', (err && err.stack) || String(err));
      return json({ error: 'server error' }, 500);
    }

    return env.ASSETS.fetch(request);
  },
};
