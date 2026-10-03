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
      'x-content-type-options': 'nosniff',
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
  /* ★★★ AND NOT SOMETHING ONLY THE WORKER CAN REACH. The scheme was the only check, so a
   *  registration could name 127.0.0.1, a private range, or 169.254.169.254 — and because the
   *  worker faithfully proxies method, path, query and body to whatever it was given, that made
   *  it a general-purpose SSRF oracle wearing a vibesdr.net name. verifyAddress even reports the
   *  upstream status and the connection error back to the caller, which is a port scanner.
   *  A receiver on a private address is served by the LAN or by the operator's own tunnel; it has
   *  no business being reachable THROUGH us. (Audit, 2026-09-10.) */
  const h = parsed.hostname.toLowerCase().replace(/^\[|\]$/g, '').replace(/\.$/, '');
  if (h === 'localhost' || h.endsWith('.localhost') || h.endsWith('.local') ||
      h.endsWith('.internal') || h.endsWith('.home.arpa') || h === '0.0.0.0') return null;
  if (privateV4(h)) return null;
  /* ★★ IPv6 — ONLY WHEN IT IS ONE. The fc/fd test used to run on every hostname, so a real server at
   *  "fdx-radio.example" was refused as a ULA. Applied to literals only now (the URL parser has
   *  already put them in canonical form: `[::ffff:127.0.0.1]` arrives as `::ffff:7f00:1`).
   *  ★★★ AND THE FORMS THAT CARRY AN IPv4 ADDRESS INSIDE THEM (security audit, 2026-10-03):
   *      IPv4-mapped (::ffff:a.b.c.d), IPv4-compatible (::a.b.c.d), IPv4-translated (::ffff:0:…),
   *      NAT64 (64:ff9b::/96 and the local-use 64:ff9b:1::/48) and 6to4 (2002::/16) all reach
   *      an IPv4 address the earlier test never saw. Every ::/8-prefixed literal is refused outright
   *      (no public server lives there); NAT64 and 6to4 are judged by the IPv4 they embed. */
  if (h.includes(':')) {
    if (h === '::' || h === '::1' || h.startsWith('::')) return null;      // unspecified, loopback, mapped, compatible
    if (/^(fc|fd)[0-9a-f]{0,2}:/.test(h)) return null;                     // unique local fc00::/7
    if (/^fe[89ab][0-9a-f]:/.test(h)) return null;                         // link-local fe80::/10
    if (/^fe[c-f][0-9a-f]:/.test(h)) return null;                          // site-local fec0::/10
    if (/^ff[0-9a-f]{2}:/.test(h)) return null;                            // multicast
    if (/^0{0,4}:/.test(h)) return null;                                   // 0::/16 written longhand
    if (/^2001:0?db8:/.test(h)) return null;                               // documentation
    if (/^64:ff9b:1:/.test(h)) return null;                                // local-use NAT64
    const embedded = embeddedV4(h);
    if (embedded && privateV4(embedded)) return null;
  }
  return parsed.origin;
}

/** True for an IPv4 dotted quad in a range no public receiver can live at. */
function privateV4(h) {
  const v4 = h.match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/);
  if (!v4) return false;
  const [a, b, c] = [Number(v4[1]), Number(v4[2]), Number(v4[3])];
  return a === 127 || a === 10 || a === 0 ||
         (a === 192 && b === 168) || (a === 172 && b >= 16 && b <= 31) ||
         (a === 169 && b === 254) || (a === 100 && b >= 64 && b <= 127) ||
         (a === 192 && b === 0 && (c === 0 || c === 2)) ||                 // IETF protocol / TEST-NET-1
         (a === 198 && (b === 18 || b === 19)) ||                         // benchmarking
         (a === 198 && b === 51 && c === 100) || (a === 203 && b === 0 && c === 113) ||
         a >= 224;
}

/** The IPv4 address inside a NAT64 (64:ff9b::/96) or 6to4 (2002::/16) literal, else ''. */
function embeddedV4(h) {
  const full = expandV6(h);
  if (!full) return '';
  const g = full.split(':').map((x) => parseInt(x, 16));
  const quad = (hi, lo) => [hi >> 8, hi & 255, lo >> 8, lo & 255].join('.');
  if (g[0] === 0x64 && g[1] === 0xff9b && g[2] === 0 && g[3] === 0 && g[4] === 0 && g[5] === 0) return quad(g[6], g[7]);
  if (g[0] === 0x2002) return quad(g[1], g[2]);
  return '';
}

/** An IPv6 literal as eight 4-digit groups, or '' if it cannot be read. */
function expandV6(h) {
  let s = h;
  const dotted = s.match(/(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/);
  if (dotted) {
    const n = dotted.slice(1).map(Number);
    s = s.slice(0, dotted.index) + ((n[0] << 8) | n[1]).toString(16) + ':' + ((n[2] << 8) | n[3]).toString(16);
  }
  const halves = s.split('::');
  if (halves.length > 2) return '';
  const head = halves[0] ? halves[0].split(':') : [];
  const tail = halves.length === 2 && halves[1] ? halves[1].split(':') : [];
  const fill = halves.length === 2 ? 8 - head.length - tail.length : 0;
  if (fill < 0 || (halves.length === 1 && head.length !== 8)) return '';
  const groups = [...head, ...Array(fill).fill('0'), ...tail];
  if (groups.length !== 8 || groups.some((x) => !/^[0-9a-f]{1,4}$/.test(x))) return '';
  return groups.map((x) => x.padStart(4, '0')).join(':');
}

/* ★★★ THE STATUS BLOB IS BOUNDED (security audit, 2026-10-03). It is stored whole in D1 and handed
 *     to every directory visitor, and nothing limited it: one registration could park megabytes in
 *     the table and make /api/directory heavy for everybody. A real multi-radio server sends a few KB.
 *  ★ Too big is not an error the server sees — that would risk a ping loop dropping a working listing.
 *    The oversized status is simply not stored (a ping keeps the previous one) and logged by id. */
const STATUS_MAX_BYTES = 16 * 1024;
function statusJsonFor(status, fallback, who) {
  if (!status || typeof status !== 'object' || Array.isArray(status)) return fallback;
  let s;
  try { s = JSON.stringify(status); } catch { return fallback; }
  if (s.length > STATUS_MAX_BYTES) {
    console.warn('status too large, not stored', who, s.length);
    return fallback;
  }
  return s;
}

/* ★★ EVERY RADIO PASSED THROUGH list() IS SHAPE- AND SIZE-CAPPED. The radio objects are copied whole
 *    from the server's /vibeserver/radios (directory.cpp), and the page, the app and the watch each
 *    read different fields of them — so a key whitelist here would silently blank a feature the
 *    next time a field is added. Instead: plain identifier keys only, at most 48 of them; strings
 *    capped; arrays and nested objects bounded and one level deep at most. Text stays text. */
const RADIO_KEY = /^[A-Za-z][A-Za-z0-9_]{0,31}$/;
function capScalar(v, maxStr) {
  if (typeof v === 'string') return v.slice(0, maxStr);
  if (typeof v === 'number') return Number.isFinite(v) ? v : undefined;
  if (typeof v === 'boolean' || v === null) return v;
  return undefined;
}
function capFlat(o, maxKeys, maxStr) {
  if (!o || typeof o !== 'object' || Array.isArray(o)) return undefined;
  const out = {};
  let n = 0;
  for (const [k, v] of Object.entries(o)) {
    if (n >= maxKeys || !RADIO_KEY.test(k)) continue;
    const c = Array.isArray(v) ? v.slice(0, 16).map((x) => capScalar(x, 80)).filter((x) => x !== undefined)
                               : capScalar(v, maxStr);
    if (c !== undefined) { out[k] = c; n++; }
  }
  return out;
}
function capRadio(r) {
  if (!r || typeof r !== 'object' || Array.isArray(r)) return null;
  const out = {};
  let n = 0;
  for (const [k, v] of Object.entries(r)) {
    if (n >= 48 || !RADIO_KEY.test(k)) continue;
    let c;
    if (Array.isArray(v)) {
      // ★ ranges / allowed / coverage are arrays of [lo, hi] pairs — kept as small scalar tuples.
      c = v.slice(0, 64).map((x) => Array.isArray(x) ? x.slice(0, 8).map((y) => capScalar(y, 80)).filter((y) => y !== undefined)
                                  : (x && typeof x === 'object') ? capFlat(x, 16, 120)
                                  : capScalar(x, 120))
           .filter((x) => x !== undefined);
    } else if (v && typeof v === 'object') {
      c = capFlat(v, 32, 200);
    } else {
      c = capScalar(v, 300);
    }
    if (c !== undefined) { out[k] = c; n++; }
  }
  return out;
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

/* ★★★ FORKS ARE LABELLED, NOT REFUSED (Stuart, 2026-10-03: "I dont mind thats absolutely fine, I just need to
 *  be prepared for it ... like we do now with the server reporting do it for forks too. If the fork ends up low
 *  quality that may break the directory or the app then we have to restrict but for now just do the labelling").
 *  VibeServer is GPLv3; nothing can prove a build is official, so the directory shows what a fork says about
 *  itself (vibe_fork.h) next to the build it came from: "Lite+ 1.2 — community fork of VibeServer Lite 11.0.0".
 *  ★★ CHECKED LIKE `flavour`, because it is printed on the page: a closed character set and a length cap rather
 *     than free text, and an official build name is DROPPED as a fork name — a listener reads it as ours.
 *  ★ No name = not a fork; a version or link without a name means nothing and is dropped too. */
const OFFICIAL_BUILD_NAMES = ['vibeserver', 'vibeserver lite', 'vibeserver inside vibesdr', 'vibesdr'];
export function forkOf(status) {
  const name = typeof status?.forkName === 'string' ? status.forkName.trim().replace(/\s+/g, ' ') : '';
  // Letters, digits, combining marks (Indic, Tibetan), Tibetan tsheg/shad, ZWNJ/ZWJ, and . + - _ ( ) — no
  // markup characters, no controls, no bidi overrides.
  if (!/^[\p{L}\p{N}][\p{L}\p{M}\p{N} .+\-_()\u0f0b-\u0f14\u200c\u200d]{1,39}$/u.test(name)) return {};
  if (OFFICIAL_BUILD_NAMES.includes(name.toLowerCase())) return {};
  const ver = typeof status.forkVersion === 'string' ? status.forkVersion.trim() : '';
  const url = typeof status.forkUrl === 'string' ? status.forkUrl.trim() : '';
  return {
    forkName: name,
    forkVersion: /^[A-Za-z0-9][A-Za-z0-9.~+\-]{0,23}$/.test(ver) ? ver : '',
    forkUrl: url.length <= 200 && /^https?:\/\/[^\s"'<>\\]{3,}$/i.test(url) ? url : '',
  };
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
      'SELECT id, url, updated_at FROM servers WHERE slug = ?'
    ).bind(wanted).first();

    /* ★★★ THE URL IS NOT A SECRET, SO IT CANNOT BE THE PROOF. This released the slug to anyone
     *  serving "at the exact address the holder published" — and list() PUBLISHES that address to
     *  the whole world (see the `url` field it returns). Read the directory, re-register with a
     *  victim's url and their slug, and the slug was yours: victim.vibeserver.vibesdr.net then
     *  proxied YOUR box, which is attacker HTML on a vibesdr.net origin and every link the victim
     *  ever shared. Registration needs no credential, so nothing else stood in the way.
     *  ★ The url match stays — it is what lets a genuine server reclaim its own name after a
     *    crash — but only once the holder has LAPSED, which is exactly the rule releaseLapsedSlug
     *    already applies. A live holder keeps its address. (Audit, 2026-09-10.) */
    /* ★ "Lapsed" is the SAME expression releaseLapsedSlug and slugHeld use — a hold that grows
     *  with how long the server has been around, floored and capped — rather than a second
     *  definition that could drift from them. The UPDATE is its own test: it changes a row only
     *  if the holder is genuinely past its hold, so there is no gap between checking and acting. */
    let released = false;
    if (holder && holder.url === url) {
      const r = await env.DB.prepare(
        `UPDATE servers SET slug = NULL WHERE id = ? AND updated_at <= (? - ${HOLD_SQL})`
      ).bind(holder.id, now()).run();
      released = Number(r?.meta?.changes || 0) > 0;
    }
    if (released) {
      // the name is free again
    } else if (holder) {
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
           statusJsonFor(body.status, '{}', 'register ' + id), t, t, t + ttlSeconds(body), wanted,
           untilFrom(body, 0)),
    env.DB.prepare('INSERT INTO reg_log (ip, at) VALUES (?,?)').bind(ip, t),
    // ★ Housekeeping on the write path rather than a cron: free, and cron is one more thing to fail.
    env.DB.prepare('DELETE FROM reg_log WHERE at < ?').bind(since),
  ]);
  // ★ The address belongs to somebody again, so the note about who had it before goes — and old
  //   notes are pruned here too (see 0007-gone-slugs.sql). Kept OUT of the batch above: a failure
  //   here must never cost a server its registration. (A stale note is harmless anyway — a listed
  //   row always wins over a note in serveBySlug.)
  try {
    await env.DB.prepare('DELETE FROM gone_slugs WHERE slug = ? OR gone_at < ?').bind(wanted, t - GONE_KEEP_SEC).run();
  } catch (e) {
    console.error('register: gone_slugs cleanup failed', wanted, (e && e.stack) || String(e));
  }

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
  const status = statusJsonFor(body.status, row.status_json, 'ping ' + row.id);

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
  // ★★ A NOTE OF WHERE IT WAS AND WHAT IT COVERED, so a visitor holding the old link is offered
  //    receivers like it (see 0007-gone-slugs.sql). Not the name, the address or the status blob.
  //    Written in the same batch as the delete: either both happen or neither does.
  //  ★★★ THE DELETE MUST NOT DEPEND ON THE NOTE. Delist is the owner's privacy switch; a missing
  //      table (migration 0007 not applied) or any other failure writing the note is logged and
  //      the listing is removed regardless.
  const t = now();
  try {
    const note = goneNote(env, row, t);
    await env.DB.batch([
      ...(note ? [note] : []),
      env.DB.prepare('DELETE FROM gone_slugs WHERE gone_at < ?').bind(t - GONE_KEEP_SEC),
    ]);
  } catch (e) {
    console.error('delist: could not keep the gone_slugs note', row.slug, (e && e.stack) || String(e));
  }
  await env.DB.prepare('DELETE FROM servers WHERE id = ?').bind(row.id).run();
  // ★ Immediate, per the privacy rule: one-press delist, effective now, not at the next expiry.
  return json({ delisted: true });
}

/* ══ EiBi SHORTWAVE SCHEDULE, SERVED COMPACT ═══════════════════════════════════════════════
 * ★ The directory's "what's on" search and receiver ranking. eibispace.de publishes one CSV per
 *   season (sked-a26.csv from the last Sunday in March, sked-b26.csv from the last Sunday in
 *   October); it is fetched here, reduced to the fields the page uses, and cached for a day in
 *   the edge cache so the visitor never pulls the 1 MB file and eibispace sees one fetch a day.
 * ★ Fields, per row: [kHz, time "0000-2400", days, ITU home country, station, language, target
 *   area, transmitter-site code, persistence]. Persistence 8 (inactive) is dropped. Utility
 *   stations carry persistence 90+ and are kept — DDK and Northwood are what people hunt.
 * ★ ISO-8859-1 on the wire; decoded here so "Bécharé" survives. */
function eibiSeasonFile(d = new Date()) {
  const y = d.getUTCFullYear(), m = d.getUTCMonth() + 1;
  const lastSun = (yy, mm) => { const t = new Date(Date.UTC(yy, mm, 0)); return t.getUTCDate() - t.getUTCDay(); };
  const aStart = Date.UTC(y, 2, lastSun(y, 3)), bStart = Date.UTC(y, 9, lastSun(y, 10));
  const t = d.getTime();
  if (t >= bStart) return `sked-b${String(y % 100).padStart(2, '0')}.csv`;
  if (t >= aStart) return `sked-a${String(y % 100).padStart(2, '0')}.csv`;
  return `sked-b${String((y - 1) % 100).padStart(2, '0')}.csv`;
}
async function eibi(request) {
  const cache = caches.default;
  const key = new Request(new URL('/api/eibi', request.url).toString(), { method: 'GET' });
  const hit = await cache.match(key);
  if (hit) return hit;
  const file = eibiSeasonFile();
  const up = await fetch(`http://www.eibispace.de/dx/${file}`, { cf: { cacheTtl: 3600 } });
  if (!up.ok) return json({ error: `eibispace.de answered ${up.status}` }, 502);
  const text = new TextDecoder('iso-8859-1').decode(await up.arrayBuffer());
  const rows = [];
  for (const line of text.split(/\r?\n/)) {
    const f = line.split(';');
    if (f.length < 9) continue;
    const khz = parseFloat(f[0]);
    if (!(khz > 0)) continue;
    const persist = parseInt(f[8], 10) || 0;
    if (persist === 8) continue;
    const st = f[4].trim();
    if (!st) continue;
    rows.push([khz, f[1].trim(), f[2].trim(), f[3].trim(), st, f[5].trim(), f[6].trim(), f[7].trim(), persist]);
  }
  const body = JSON.stringify({ season: file.replace(/^sked-|\.csv$/g, ''), fetched: new Date().toISOString().slice(0, 16) + 'Z', rows });
  const res = new Response(body, { headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'public, max-age=3600, s-maxage=86400', 'access-control-allow-origin': '*' } });
  await cache.put(key, res.clone());
  return res;
}

/* ★★★ FILTERED BY THE REQUESTER, NOT ONCE FOR EVERYONE (BRIEF-v11 §7). Every client version
 *  reads this one endpoint. A requester that sends no `proto` is a legacy app: it cannot read
 *  `minProto`, so a radio it has no controls for is left OUT of what it is told. A requester
 *  that sends `proto=N` gets every radio and greys out the ones above N itself. */
async function list(env, url, request) {
  const reqProto = url && url.searchParams.has('proto') ? Number(url.searchParams.get('proto')) || 0 : null;
  const radiosFor = (rows0) => {
    if (!Array.isArray(rows0)) return [];
    const rows = rows0.slice(0, 32).map(capRadio).filter(Boolean);     // ★ see capRadio
    return reqProto === null ? rows.filter((r) => !(Number(r && r.minProto) > 0)) : rows;
  };
  // ★★★ EXPIRY EVALUATED AT READ TIME. Nothing sweeps; a server that stopped pinging is simply
  //     not selected. See schema.sql.
  const { results } = await env.DB.prepare(
    `SELECT id, name, url, kind, grid, lat, lon, country, status_json, updated_at, expires_at, slug,
            until
       FROM servers WHERE ${LIVE_WHERE} ORDER BY country, name`
  ).bind(now()).all();

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
      radios: radiosFor(status.radios),
      // ★ The host's battery, where it has one — level, charging, and whether it is in its low power state.
      battery: status.battery && typeof status.battery.level === 'number'
        ? { level: status.battery.level, charging: !!status.battery.charging, paused: !!status.battery.paused } : undefined,
      // ★ The contract each server speaks (BRIEF-v11 §4) — forwarded whole, never interpreted here.
      proto: typeof status.proto === 'number' ? status.proto : undefined,
      minProto: typeof status.minProto === 'number' ? status.minProto : undefined,
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
      /* ★★★ WHICH BUILD OF VibeServer, AND WHICH VERSION (Stuart, 2026-09-21: "i think in the
       *  directory we should list Server version so I can keep track of it").
       *  ★★ A CLOSED SET, not free text. `platform` above is printed as the server says it because
       *     it names an OS we may never have met; this one names OUR OWN builds, so there are
       *     exactly three legitimate answers and anything else is a server misreporting itself.
       *     Whitelisting them keeps a listing from carrying arbitrary text into the page.
       *  ★ The VERSION is bounded and character-restricted rather than enumerated — it moves every
       *    release and a table here would be one more thing to bump. Absent stays absent: a server
       *    too old to send either field shows nothing, and the page reads that as unknown rather
       *    than inventing a number. */
      flavour: ['VibeServer', 'VibeServer Lite', 'VibeServer inside VibeSDR']
                 .includes(status.flavour) ? status.flavour : '',
      // ★★ A COMMUNITY FORK, said by the fork itself (vibe_fork.h) — see forkOf.
      ...forkOf(status),
      /* ★ V11 betas carry a label — 11.0.0~b1 (dpkg's pre-release form). Digits and dots only threw
       *  away every B1 server's version the moment the estate moved (2026-09-28). */
      version: typeof status.version === 'string' && /^[0-9][0-9.]{0,15}(~[A-Za-z0-9]{1,8})?$/.test(status.version)
                 ? status.version : '',
      /* ★★ THE OWNER'S LANDING MESSAGE (Stuart, 2026-09-28): clicking a radio straight from here skips
       *  the server's landing page, and with it what the owner wrote there. Text stays TEXT (the page
       *  renders it with textContent), bounded; the link must be http(s) or it is dropped. */
      landingMessage:   typeof status.landingMessage === 'string' ? status.landingMessage.trim().slice(0, 600) : '',
      landingLinkUrl:   typeof status.landingLinkUrl === 'string' && /^https?:\/\/[^\s"'<>]{3,300}$/i.test(status.landingLinkUrl.trim())
                          ? status.landingLinkUrl.trim() : '',
      landingLinkLabel: typeof status.landingLinkLabel === 'string' ? status.landingLinkLabel.trim().slice(0, 60) : '',
      // ★ THE MACHINE, as the server measured it (vibe_hwinfo.h) — shown so a listener can see a DAB receiver
      //   running on a 900 MHz Pi 2 or a TV (Stuart, 2026-09-19). Each field checked and bounded; text stays text.
      hw: (status.hw && typeof status.hw === 'object') ? {
        cpu:   typeof status.hw.cpu === 'string' ? status.hw.cpu.slice(0, 60) : '',
        cores: Number.isInteger(status.hw.cores) && status.hw.cores > 0 && status.hw.cores <= 512 ? status.hw.cores : 0,
        mhz:   Number.isInteger(status.hw.mhz) && status.hw.mhz > 0 && status.hw.mhz < 10000 ? status.hw.mhz : 0,
        ramMb: Number.isInteger(status.hw.ramMb) && status.hw.ramMb > 0 && status.hw.ramMb < 4194304 ? status.hw.ramMb : 0,
        isa:   ['64-bit', '32-bit NEON', '32-bit'].includes(status.hw.isa) ? status.hw.isa : '',
      } : null,
      // ★ How long a listener gets, said BEFORE they click rather than when they are cut off.
      limitMin: Number(status.limitMin) > 0 ? Number(status.limitMin) : 0,
      listeners: Number(status.listeners || 0),
      maxListeners: Number(status.maxListeners || 0),
      freeInSec: Number.isFinite(Number(status.freeInSec)) ? Number(status.freeInSec) : -1,
      updatedAt: r.updated_at,
      expiresAt: r.expires_at,
    };
  });
  /* ★★★ WHERE THE LISTENER IS, so the page can put their own country at the top and open it.
   *  Alphabetical order meant Brazil opened for everybody, and Stuart — in the UK, usually after
   *  his own servers — found his at the bottom (2026-09-23).
   *  ★★ FROM CLOUDFLARE'S EDGE, not from an IP lookup we would have to ship, keep current and
   *     explain. `request.cf` is filled by the network that already terminated the connection, so
   *     it costs nothing and never leaves the edge.
   *  ★★ AND IT IS A HINT, NOT AN IDENTITY. A VPN or a corporate egress will say the wrong country
   *     and the page must simply open the wrong card — which is why this only reorders and opens
   *     something, and never filters anything out. Nothing is hidden on the strength of it.
   *  ★ Coarse by design: country, and the edge's own city-level lat/lon for "closest country when
   *    yours has no servers". No address, nothing stored, nothing logged. */
  const cf = (request && request.cf) || {};
  const you = {
    country: typeof cf.country === 'string' ? cf.country : null,
    lat: Number.isFinite(+cf.latitude)  ? +cf.latitude  : null,
    lon: Number.isFinite(+cf.longitude) ? +cf.longitude : null,
  };
  return json({ servers, count: servers.length, pingSec: PING_SEC, you });
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

/* ══ A DEAD ADDRESS STILL GETS VISITORS ═══════════════════════════════════════════════════════════
 * ★★★ Stuart, 2026-09-29: "anybody opening a dead link from history gets a sorry this server is no
 *     longer available on this address, here are some other servers that you may be interested in
 *     and bring up the servers closest to location of the old dead server and which covers the same
 *     ranges". Measured: clearing the Sony TV app's storage made it a NEW server (new key, new slug
 *     stuey3d-sony-bravia-tv); the old stuey3d-sonytv answered a plain-text 503 and still had 21
 *     visits in a week from bookmarks and Discord links. The two cannot be linked — the new one is
 *     a new identity — so we never redirect; we OFFER.
 *
 * ★★ WHICH ADDRESS GETS WHICH ANSWER (serveBySlug):
 *    · live                                  → proxied, unchanged.
 *    · live, but its tunnel does not answer  → 503 + Retry-After page: "isn't responding right now"
 *                                              + other servers like it (see UPSTREAM_DOWN).
 *    · offline under a day                   → today's plain 503, unchanged. A restart, a reboot, a
 *                                              flat phone overnight: nobody needs alternatives.
 *    · offline a day or more, still HELD     → 503 page: "not been online for N days, its address is
 *                                              kept for it" + other servers like it. Not "gone" —
 *                                              the owner may be on holiday.
 *    · offline past its hold (HOLD_SQL)      → 410 page: "no longer available at this address" +
 *                                              other servers like it. The row still exists (expiry
 *                                              never deletes), so its own grid and radios rank.
 *    · deleted by delist, note in gone_slugs → 410 page, same, ranked from the note.
 *    · never seen                            → 404 page: nearest servers to the VISITOR.
 *    ★ Only the ROOT DOCUMENT gets a page. Any other path under a dead address — an API read, an
 *      asset, a socket — gets the same status in plain text, never HTML inside a JSON reply.
 */
const AWAY_PAGE_SEC = 86400;              // offline this long before the 503 carries alternatives
const GONE_KEEP_SEC = 180 * 86400;        // how long a delisted address's note is kept
const SUGGEST_MAX = 6;
// Statuses that mean "the tunnel reached nobody" — Cloudflare's and cloudflared's, never VibeServer's.
const UPSTREAM_DOWN = new Set([502, 504, 520, 521, 522, 523, 524, 525, 526, 527, 530]);

/**
 * ★ The bands a listener would recognise, for "covers the same ranges". A COARSE table on purpose:
 *   it ranks suggestions, it filters nothing, and it never decides what a receiver may tune — the
 *   directory page learns band edges from the servers themselves (learnBands) and that stays so.
 */
const BANDS = [
  ['lw',   'Longwave',     148500,     283500],
  ['mw',   'Medium\u00a0wave', 526500,     1606500],
  ['hf',   'Shortwave',    3000000,    30000000],
  ['fm',   'FM',           87500000,   108000000],
  ['air',  'Airband',      118000000,  137000000],
  ['2m',   '2\u00a0m',       144000000,  148000000],
  ['dab',  'DAB',          174000000,  240000000],
  ['70cm', '70\u00a0cm',      430000000,  440000000],
];
const BAND_NAME = new Map(BANDS.map(([k, n]) => [k, n]));
// A phone publishes band NAMES in `coverage` (see index.html radioLine) — read the words too.
const BAND_WORDS = [
  ['lw', /\blong ?wave\b|\blw\b/i], ['mw', /\bmedium ?wave\b|\bmw\b|\bam broadcast/i],
  ['hf', /\bshort ?wave\b|\bhf\b|\bsw\b/i], ['fm', /\bfm\b/i], ['air', /\bair ?band\b|\baviation\b/i],
  ['2m', /\b2 ?m\b/i], ['dab', /\bdab\b/i], ['70cm', /\b70 ?cm\b/i],
];

const pairsOf = (v) => (Array.isArray(v) ? v : [])
  .filter((p) => Array.isArray(p) && p.length === 2 && Number.isFinite(+p[0]) && Number.isFinite(+p[1]))
  .map((p) => [+p[0], +p[1]]);

/** What one radio reaches, in Hz — the same precedence as the page's reachOf(): a locked radio is
 *  its window; then what a listener may tune; then the hardware. ★ A radio that published no numbers
 *  at all (an older phone — the old Sony row is exactly this) falls back to its driver's reach,
 *  because "covers nothing" would rank it below everything and that is not true. */
function reachOfRadio(r) {
  const centre = Number(r.centreHz) || 0, span = Number(r.spanHz) || 0;
  if (r.locked && centre > 0 && span > 0) return [[centre - span / 2, centre + span / 2]];
  for (const f of ['allowed', 'ranges', 'coverage']) {
    const p = pairsOf(r[f]);
    if (p.length) return p;
  }
  const d = String(r.driver || '').toLowerCase(), n = String(r.name || '').toLowerCase();
  if (d.includes('airspyhf')) return [[9000, 31000000], [60000000, 260000000]];
  if (d.includes('sdrplay') || /\brsp/.test(n)) return [[1000, 2000000000]];
  if (d.includes('rtl')) return /\bv4\b/.test(n) ? [[500000, 1766000000]] : [[24000000, 1766000000]];
  return [];
}

/** Band keys a server offers across its radios, from numbers and from words. */
function bandsFor(status) {
  const out = new Set();
  for (const r of (Array.isArray(status && status.radios) ? status.radios : [])) {
    if (!r || typeof r !== 'object') continue;
    const words = [r.allowedNames, r.coverage].flatMap((v) => Array.isArray(v) ? v : [])
      .filter((w) => typeof w === 'string');
    for (const w of words) for (const [k, re] of BAND_WORDS) if (re.test(w)) out.add(k);
    const reach = reachOfRadio(r);
    for (const [k, , lo, hi] of BANDS) if (reach.some(([a, b]) => a < hi && b > lo)) out.add(k);
  }
  return BANDS.map(([k]) => k).filter((k) => out.has(k));      // table order, stable
}

/** ★ The 4-character square's centre, even if the listing published six: ranking does not need a
 *  5 km square, and a note about a server that has gone should hold no more than it must. */
function coarsePos(grid, lat, lon) {
  const p = gridToLatLon(String(grid || '').slice(0, 4));
  return p || (Number.isFinite(+lat) && Number.isFinite(+lon) ? { lat: +lat, lon: +lon } : null);
}

function kmBetween(a, b) {
  const R = 6371, rad = Math.PI / 180;
  const dLat = (b.lat - a.lat) * rad, dLon = (b.lon - a.lon) * rad;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * rad) * Math.cos(b.lat * rad) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
}

const parseStatus = (s) => { try { return JSON.parse(s) || {}; } catch { return {}; } };

/** ★ THE SAME "LIVE" AS list() — one rule, two readers, so the expression is shared, not copied. */
const LIVE_WHERE = 'expires_at > ?1 AND verified = 1 AND (until = 0 OR until > ?1)';

/**
 * Up to SUGGEST_MAX live servers for a visitor at a dead address.
 * @param from  {lat, lon} to measure from (the gone server, or the visitor), or null
 * @param bands band keys to match (the gone server's), or [] to rank on distance alone
 * @param country fallback when there is no position: same country first
 * ★ Order: most shared bands, then a server a stranger can actually open (no server PIN), then
 *   nearest. A server-wide PIN is listed — the directory lists it — just not ahead of an open one.
 */
async function suggestServers(env, { from, bands = [], country = '', exclude = '', coarse = false }) {
  const { results } = await env.DB.prepare(
    `SELECT name, url, slug, country, grid, lat, lon, status_json FROM servers WHERE ${LIVE_WHERE}`
  ).bind(now()).all();
  const want = new Set(bands);
  const rows = (results || []).filter((r) => r.slug !== exclude).map((r) => {
    const status = parseStatus(r.status_json);
    const b = bandsFor(status);
    // ★ Measured at the SAME resolution as `from`: a gone server is held at its 4-character square,
    //   so candidates are too — otherwise a receiver in the very same place reads "23 km away".
    const pos = coarse ? coarsePos(r.grid, r.lat, r.lon)
      : Number.isFinite(+r.lat) && Number.isFinite(+r.lon) ? { lat: +r.lat, lon: +r.lon } : null;
    return {
      name: r.name, slug: r.slug, url: r.url, country: r.country || '',
      bands: b, shared: b.filter((k) => want.has(k)).length,
      km: from && pos ? kmBetween(from, pos) : null,
      pin: !!status.pin,
      listeners: Number(status.listeners) || 0, maxListeners: Number(status.maxListeners) || 0,
    };
  });
  rows.sort((a, b) =>
    (b.shared - a.shared) || (a.pin - b.pin)
    || (a.km !== null && b.km !== null ? a.km - b.km : 0)
    || ((b.country === country) - (a.country === country))
    || String(a.name).localeCompare(String(b.name)));
  return rows.slice(0, SUGGEST_MAX);
}

/** ★ THE DIRECTORY PAGE'S esc(), to the character — every string from the database goes through it,
 *  and no string from the database is ever placed anywhere but element text or a quoted attribute. */
const escHtml = (s) => String(s ?? '').replace(/[&<>"']/g,
  (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

function flagOf(cc) {
  if (!/^[A-Z]{2}$/.test(cc || '')) return '🌐';
  return String.fromCodePoint(...[...cc].map((c) => 0x1f1e6 + c.charCodeAt(0) - 65));
}

/** Where a suggestion links: its stable address when it has one, else its own validated URL. */
function hrefOf(s) {
  if (s.slug && /^[a-z0-9][a-z0-9-]{0,62}$/.test(s.slug)) return `https://${s.slug}.${PUBLIC_ZONE}/`;
  return validUrl(s.url) ? validUrl(s.url) + '/' : `https://${PUBLIC_ZONE}/`;
}

function cardHtml(s, sharedWith, distWord) {
  const shared = new Set(sharedWith);
  const covers = s.bands.length
    ? s.bands.map((k) => shared.has(k) ? `<b>${escHtml(BAND_NAME.get(k))}</b>` : escHtml(BAND_NAME.get(k))).join(' · ')
    : 'coverage not published';
  const full = s.maxListeners > 0 && s.listeners >= s.maxListeners;
  const state = full ? `<span class="st full">FULL RIGHT NOW</span>`
    : s.listeners > 0 ? `<span class="st">ONLINE · ${s.listeners} LISTENING</span>`
    : `<span class="st">ONLINE</span>`;
  const dist = s.km === null ? ''
    : s.km < 5 ? ` · ${distWord === 'you' ? 'near you' : 'same area'}`
    : ` · ${Math.round(s.km).toLocaleString('en-GB')} km ${distWord === 'you' ? 'from you' : 'away'}`;
  return `<li class="card"><a href="${escHtml(hrefOf(s))}">`
    + `<span class="top"><span class="nm">${flagOf(s.country)} ${escHtml(s.name)}</span>${state}</span>`
    + `<span class="sub">${covers}${escHtml(dist)}${s.pin ? ' · PIN needed' : ''}</span>`
    + `</a></li>`;
}

/**
 * The page itself. ★ The directory's own look — its palette tokens by value and its monospace — but
 * NO SCRIPT at all, and a CSP that says so: a page built from other people's names needs nothing
 * that runs. ★ noindex twice (meta and header): a dead address must not become a search result.
 */
function sorryPage({ status, title, lead, sub, heading, note = '', cards, empty, retry = false, extra = {} }) {
  const body = `<!doctype html>
<html lang="en-GB"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>${escHtml(title)} — VibeServer Directory</title>
<style>
  :root { --bg:#080601; --panel:#0d0a02; --line:rgba(255,176,0,.22); --text:#ffcc88;
          --dim:rgba(255,160,0,.5); --accent:#ffb833; --phosphor:#28A745; --busy:#e05050; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font-size:15px; line-height:1.5;
         font-family: ui-monospace, "SF Mono", Menlo, Consolas, monospace; }
  header { padding: max(26px, env(safe-area-inset-top)) 16px 18px; border-bottom:1px solid var(--line); text-align:center; }
  header a { color:var(--accent); text-decoration:none; font-size:20px; font-weight:600;
             letter-spacing:5px; text-indent:5px; display:inline-block; text-shadow:0 0 24px rgba(255,170,0,.35); }
  main { max-width:680px; margin:0 auto; padding:22px 16px 40px; }
  h1 { font-size:19px; color:var(--accent); font-weight:600; letter-spacing:.03em; margin:0 0 8px; }
  p { margin:0 0 10px; }
  .dim { color:var(--dim); font-size:13px; }
  h2 { font-size:12px; color:var(--dim); font-weight:400; letter-spacing:.14em; text-transform:uppercase;
       margin:26px 0 8px; }
  ul { list-style:none; margin:0; padding:0; }
  .card a { display:block; border:1px solid rgba(255,176,0,.35); border-radius:8px; padding:10px 12px;
            margin:0 0 10px; color:inherit; text-decoration:none; background:var(--panel); }
  .card a:hover, .card a:focus-visible { border-color:var(--accent); }
  .top { display:flex; justify-content:space-between; align-items:baseline; gap:12px; flex-wrap:wrap; }
  .nm { font-size:16px; font-weight:600; color:var(--accent); letter-spacing:.03em; overflow-wrap:anywhere; }
  .st { font-size:11px; color:var(--phosphor); letter-spacing:.06em; white-space:nowrap; }
  .st.full { color:var(--busy); }
  .sub { display:block; margin-top:3px; font-size:12px; color:var(--dim); letter-spacing:.03em; }
  .sub b { color:var(--text); font-weight:600; }
  .listen { display:inline-block; margin-top:18px; padding:9px 18px; border-radius:7px; background:var(--accent);
            color:#120a00; text-decoration:none; font-weight:600; font-size:12px; letter-spacing:.12em; text-transform:uppercase; }
  .listen:hover { background:#ffc75c; }
  .acts { display:flex; flex-wrap:wrap; gap:10px; margin:18px 0 0; }
  .acts .listen { margin-top:0; }
  /* ★ "Try again" is a plain link to this same address — a reload with no script on the page. */
  .listen.ghost { background:transparent; color:var(--accent); border:1px solid var(--line); }
  .listen.ghost:hover { border-color:var(--accent); background:transparent; }
</style></head>
<body><header><a href="https://${PUBLIC_ZONE}/">VIBESERVER</a></header>
<main>
<h1>${escHtml(lead)}</h1>
${sub.map((l) => `<p class="dim">${escHtml(l)}</p>`).join('\n')}
${cards.length
    ? `<h2>${escHtml(heading)}</h2>\n${note ? `<p class="dim">${escHtml(note)}</p>\n` : ''}<ul>${cards.join('\n')}</ul>`
    : `<p>${escHtml(empty)}</p>`}
<p class="acts">${retry ? '<a class="listen ghost" href="/">Try again</a>' : ''}<a class="listen" href="https://${PUBLIC_ZONE}/">${retry ? 'Back to the directory' : 'See every server in the directory'}</a></p>
</main></body></html>`;
  return new Response(body, {
    status,
    headers: {
      'content-type': 'text/html; charset=utf-8',
      'cache-control': 'no-store',
      'x-robots-tag': 'noindex',
      'content-security-policy': "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
      'referrer-policy': 'no-referrer',
      ...extra,
    },
  });
}

/** A root GET/HEAD for the document — the only request a dead address answers with a page. */
const wantsPage = (request, pathname) =>
  (request.method === 'GET' || request.method === 'HEAD') && (pathname === '/' || pathname === '/index.html')
  && (request.headers.get('upgrade') || '').toLowerCase() !== 'websocket';

const plain = (text, status) => new Response(text, {
  status, headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store', 'x-robots-tag': 'noindex' },
});

/**
 * The page for an address whose server we KNOW — three variants, distinct in wording and status:
 *   'down' — listed and pinging, but its tunnel did not answer (503 + Retry-After). NickB's server,
 *            2026-09-29: listed, shown unresponsive, and clicking it gave visitors Cloudflare's raw
 *            error page until he restarted it.
 *   'away' — has not pinged for a day or more but still holds its address (503).
 *   'gone' — past its hold, or delisted (410).
 * All three rank the same way: the same bands first, then nearest to THIS server.
 */
async function knownPage(env, slug, { variant, name, pos, bands, country, away, left }) {
  const picks = await suggestServers(env, { from: pos, bands, country, exclude: slug, coarse: true });
  const covered = bands.map((k) => BAND_NAME.get(k)).join(', ');
  const who = name || 'this server';
  let title, lead, sub, heading, status, extra = {}, retry = false;
  if (variant === 'down') {
    status = 503; retry = true; extra = { 'retry-after': '120' };
    title = "This server isn't responding right now";
    lead = title;
    sub = [`Apologies — ${who} appears to be experiencing technical difficulties and isn't responding at the moment.`
         + ' It may well be back shortly: check the VibeServer directory for its latest status.'];
    heading = 'In the meantime, here are some other servers you may enjoy';
  } else if (variant === 'away') {
    status = 503;
    title = 'Offline for now';
    // ★ Stuart, 2026-09-29: the day count yes, but factual rather than chatty — say it may return,
    //   and exactly when its address will be released if it does not.
    lead = `Sorry — ${who} has not been seen for ${away}.`;
    sub = ['This could be a temporary issue, and the server may return.'
         + ` However, if it does not check in within the next ${left}, it will be removed from the`
         + ' directory and this address will be made available again.'];
    heading = 'In the meantime, here are some other servers you may enjoy';
  } else {
    status = 410;
    title = 'No longer here';
    lead = 'Sorry — this server is no longer available at this address.';
    sub = name ? [`${name} used to be here, at ${slug}.${PUBLIC_ZONE}.`] : [];
    heading = 'Here are some other servers you may like';
  }
  // ★ Under the heading, not above it (Stuart, 2026-09-30: "it reads better") — it describes the list.
  const note = covered && picks.length ? `Listed nearest first, favouring servers that also cover ${covered}.` : '';
  return sorryPage({
    status, title, lead, sub, heading, note, retry, extra,
    cards: picks.map((s) => cardHtml(s, bands, 'away')),
    empty: 'No other servers are online right now.',
  });
}

/** The page for an address nobody ever held: nearest to the visitor, by Cloudflare's own edge. */
async function unknownPage(env, slug, request) {
  const cf = request.cf || {};
  // ★ The same coarse, edge-supplied position list() already uses — never the browser's location.
  const from = cf.latitude != null && cf.longitude != null && cf.latitude !== '' && cf.longitude !== ''
    && Number.isFinite(+cf.latitude) && Number.isFinite(+cf.longitude)
    ? { lat: +cf.latitude, lon: +cf.longitude } : null;
  const country = typeof cf.country === 'string' ? cf.country : '';
  const picks = await suggestServers(env, { from, country });
  return sorryPage({
    status: 404,
    title: 'No server here',
    lead: 'Sorry — there is no server at this address.',
    sub: [`No server is listed at ${slug}.${PUBLIC_ZONE}. Check the spelling, or pick one of these.`],
    heading: from ? 'Servers near you' : 'Servers online now',
    cards: picks.map((s) => cardHtml(s, [], 'you')),
    empty: 'No servers are online right now.',
  });
}

function awayWords(sec) {
  const d = Math.floor(sec / 86400);
  if (d >= 1) return d === 1 ? 'a day' : `${d} days`;
  const h = Math.max(1, Math.floor(sec / 3600));
  return h === 1 ? 'an hour' : `${h} hours`;
}

/** Time until the address hold runs out, rounded UP — "within the next 4 days" must never promise
 *  less time than the server really has. */
function leftWords(sec) {
  const d = Math.ceil(sec / 86400);
  if (d >= 2) return `${d} days`;
  const h = Math.max(1, Math.ceil(sec / 3600));
  return h >= 24 ? 'day' : (h === 1 ? 'hour' : `${h} hours`);
}

/** ★ Called by delist() BEFORE the row goes: the note gone_slugs keeps (see 0007-gone-slugs.sql). */
function goneNote(env, row, t) {
  const pos = coarsePos(row.grid, row.lat, row.lon);
  if (!row.slug || !pos) return null;
  return env.DB.prepare(
    'INSERT OR REPLACE INTO gone_slugs (slug, lat, lon, country, bands, gone_at) VALUES (?,?,?,?,?,?)'
  ).bind(row.slug, pos.lat, pos.lon, clean(row.country, 2).toUpperCase(),
         JSON.stringify(bandsFor(parseStatus(row.status_json))), t);
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

  /* ★★★ VERIFIED ONLY, as list() has always required. This did not check, so a registration that
   *  never passed verifyAddress could still be SERVED under its slug — the attacker in the note
   *  above never had to run a real receiver at all. (Audit, 2026-09-10.) */
  const row = await env.DB.prepare(
    `SELECT url, name, expires_at, updated_at, created_at, grid, lat, lon, country, status_json
       FROM servers WHERE slug = ? AND verified = 1`
  ).bind(slug).first();

  const upstream = new URL(request.url);
  const page = wantsPage(request, upstream.pathname);
  // Everything we know about this server that the "other servers" ranking needs.
  const known = (r, variant, extra = {}) => ({
    variant, name: r.name, pos: coarsePos(r.grid, r.lat, r.lon),
    bands: bandsFor(parseStatus(r.status_json)), country: r.country || '', ...extra,
  });

  if (!row) {
    /* ★ NOT LISTED: either a delisted server whose note we kept (410), or a name nobody holds
     *   (404). An UNVERIFIED row falls here too, as it always has — a claim is not a server. */
    let note = null;
    try {
      note = await env.DB.prepare(
        'SELECT lat, lon, country, bands FROM gone_slugs WHERE slug = ? AND gone_at > ?'
      ).bind(slug, now() - GONE_KEEP_SEC).first();
    } catch (e) {
      // ★ Logged, not swallowed: most likely migration 0007 is not applied. The visitor still gets
      //   the "no server here" page rather than a Worker exception.
      console.error('serveBySlug: gone_slugs lookup failed', slug, (e && e.stack) || String(e));
    }
    if (note) {
      if (!page) return plain(`No VibeServer is listed at ${slug}.${PUBLIC_ZONE} any more.`, 410);
      let bands = [];
      try { bands = JSON.parse(note.bands).filter((k) => BAND_NAME.has(k)); }
      catch (e) { console.warn('gone_slugs: unreadable bands for', slug, String(e)); }
      return knownPage(env, slug, { variant: 'gone', name: '', pos: { lat: +note.lat, lon: +note.lon },
                                    bands, country: note.country || '' });
    }
    if (!page) return plain(`No VibeServer is listed at ${slug}.${PUBLIC_ZONE}.`, 404);
    return unknownPage(env, slug, request);
  }
  if (Number(row.expires_at) <= now()) {
    // ★★ DO NOT PROMISE A RESERVATION THAT HAS LAPSED. Past the hold this name is up for grabs, so
    //    "it will work again when it returns" would be a claim we have stopped honouring.
    const lifetime = Number(row.updated_at) - Number(row.created_at);
    const hold = Math.min(Math.max(lifetime, ADDRESS_HOLD_MIN), ADDRESS_HOLD_MAX);
    const awaySec = now() - Number(row.updated_at);
    const stillHeld = awaySec < hold;
    // ★★★ PAST THE HOLD IT IS GONE — a 410 page with other servers like it. The row is still here
    //     (expiry never deletes), so its own square and radios do the ranking.
    if (!stillHeld) {
      if (!page) return plain(`${row.name} is no longer available at this address.`, 410);
      return knownPage(env, slug, known(row, 'gone'));
    }
    // ★★ AWAY A DAY OR MORE, STILL HELD: the address is kept, and the visitor is offered others.
    if (page && awaySec >= AWAY_PAGE_SEC) {
      return knownPage(env, slug, known(row, 'away', { away: awayWords(awaySec), left: leftWords(hold - awaySec) }));
    }
    // ★ Briefly offline (or not the document): exactly the answer it has always had.
    return new Response(
      `${row.name} is not online at the moment.\nIts address stays reserved, so this link will work again when it returns.`,
      { status: 503, headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' } }
    );
  }

  const origin = validUrl(row.url);
  if (!origin) return new Response('This server published an address we cannot use.', { status: 502 });

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

  // ★★★ SAY WHO THE VISITOR IS, PLAINLY. (★ Tried, and Cloudflare discards it — see below.) Proxying the page means the server sees CLOUDFLARE at the
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
  // ★★★ …AND IT DOES NOT ARRIVE. This fetch is a CROSS-ZONE subrequest, and for those Cloudflare
  //     "unconditionally replaces" X-Forwarded-For / X-Real-IP with its own Worker address
  //     (2a06:98c0:3600::103) to prevent spoofing (Cloudflare docs, Request Header Modification).
  //     So every server's landing-page list showed that one address "page left open for 41m" with a
  //     US flag, on every box (Stuart, 2026-09-29) — somebody's page, the owner's own admin page
  //     included, with the address hidden. The server cannot get the visitor back, but it CAN say
  //     honestly what the row is: we name ourselves, and the shim labels the row
  //     ("VibeServer directory visitor (via Cloudflare)") instead of drawing a lurker. It believes
  //     this header only from Cloudflare's Worker range, so nobody can relabel themselves with it.
  fwd.set('x-vibesdr-via', 'directory');
  /* ★★★ NO CREDENTIALS OF OURS GO TO SOMEBODY ELSE'S BOX (security audit, 2026-10-03). Every
   *     <slug>.vibeserver.vibesdr.net is one SITE with the directory, so a browser sends this
   *     proxied request any cookie scoped to .vibesdr.net / vibeserver.vibesdr.net — the portable
   *     store's included — and a receiver is a stranger's machine. VibeServer uses neither cookies nor
   *     HTTP auth (its credentials are the vs_* parameters), so nothing it needs is lost. */
  fwd.delete('cookie');
  fwd.delete('authorization');
  fwd.delete('proxy-authorization');

  /* ★★★ A LISTED SERVER THAT DOES NOT ANSWER GETS OUR PAGE, NOT CLOUDFLARE'S. A fetch that threw
   *     used to escape the Worker entirely (Cloudflare's 1101 page), and a tunnel with nobody behind
   *     it came back as Cloudflare's own 502/530 page, proxied faithfully to the visitor. Both now
   *     become the 'down' variant: 503 + Retry-After, with other servers like it.
   *  ★★ ONLY FAILURES THAT CANNOT BE THE SERVER'S OWN ANSWER. 502, 504 and 520–530 are what
   *     Cloudflare and cloudflared say when the origin is unreachable, and VibeServer emits none of
   *     them. 503 is deliberately NOT on the list: the front door answers 503 itself for a radio
   *     that is not up (main.cpp), and that is a real answer the client must see.
   *  ★ The document waits DOC_TIMEOUT_MS for headers, then gives up — the timer is cleared the
   *    moment headers arrive, so a slow body is never cut. Other paths keep Cloudflare's own
   *    100-second origin limit (a 524, which is then mapped like the rest). */
  const DOC_TIMEOUT_MS = 25000;
  const ctl = page ? new AbortController() : null;
  const timer = ctl ? setTimeout(() => ctl.abort(), DOC_TIMEOUT_MS) : null;
  const down = async (why) => {
    console.warn('slug upstream not responding', slug, String(why).replace(/[?#]\S*/g, '?…'));   // ★ never a query string in a log
    if (!page) {
      return new Response(`${row.name} is not responding at the moment. Please try again shortly.`, {
        status: 503,
        headers: { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store', 'retry-after': '120' },
      });
    }
    return knownPage(env, slug, known(row, 'down'));
  };
  let res;
  try {
    res = await fetch(target, {
      method: request.method,
      headers: fwd,
      body: (request.method === 'GET' || request.method === 'HEAD') ? undefined : request.body,
      redirect: 'manual',
      ...(ctl ? { signal: ctl.signal } : {}),
    });
  } catch (e) {
    return down(ctl && ctl.signal.aborted ? 'timeout' : String((e && e.message) || e).slice(0, 120));
  } finally {
    if (timer) clearTimeout(timer);
  }
  if (UPSTREAM_DOWN.has(res.status)) {
    // ★ Drain what Cloudflare sent so the connection is released; its content is not wanted.
    try { await res.body?.cancel(); } catch (e) { console.warn('slug upstream body cancel', String(e)); }
    return down(`http ${res.status}`);
  }

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

  /* ★★★ AND NO COOKIE COMES BACK FROM IT. A receiver's Set-Cookie would be stored for a vibesdr.net
   *     name — and with `Domain=vibesdr.net` for EVERY vibesdr.net page, the directory and the
   *     portable store included (cookie tossing). VibeServer sets none, so all are dropped. */
  const upHeaders = new Headers(res.headers);
  upHeaders.delete('set-cookie');

  if (!type.includes('text/html')) {
    // ★ Everything that is not the document streams through untouched (bar the cookie, above).
    return new Response(res.body, {
      status: res.status,
      headers: upHeaders,
    });
  }

  // ★★ TELL THE PAGE WHERE THE RECEIVER ACTUALLY IS. Injected rather than built into the client,
  //    because the tunnel hostname is not knowable at build time and changes under us.
  // ★★ AS A <meta> TOO (audit, 2026-10-03): an inline script needs 'unsafe-inline' in the receiver's
  //    CSP; the meta does not (web/client/src/origin.ts reads either). The script stays for clients
  //    older than the meta reader.
  const html = await res.text();
  const direct = new URL(origin).host;
  const inject = `<meta name="vibe-direct-host" content="${direct.replace(/[^A-Za-z0-9.:\[\]-]/g, '')}">`
               + `<script>window.__VIBE_DIRECT_HOST__=${JSON.stringify(direct).replace(/</g, '\\u003c')};</script>`;
  const out = html.includes('</head>')
    ? html.replace('</head>', inject + '</head>')
    : inject + html;

  const headers = upHeaders;
  // ★ The document must not be cached: the host it names changes when the tunnel restarts.
  headers.set('cache-control', 'no-store');
  headers.delete('content-length');
  return new Response(out, { status: res.status, headers });
}

// ── RAW IQ OUT pairing codes ──────────────────────────────────────────────────────────────────
// See migrations/0005-iq-codes.sql. The code is what a person types into the VibeIQ bridge; the
// answer is the receiver's slug (so the bridge can read /vibeserver.json through us and learn the
// live tunnel hostname) and the session token the server will check on /ws/iq.
const IQ_TTL = 20 * 60;            // seconds after the last refresh
const IQ_PER_HOUR = 30;            // registrations per source address
const IQ_CODE_RE = /^[a-z0-9]{6}$/;
const IQ_TOKEN_RE = /^[a-z0-9]{8,64}$/;
const IQ_SLUG_RE = /^[a-z0-9][a-z0-9-]{0,62}$/;
const IQ_PATH_RE = /^\/r\/[A-Za-z0-9_-]{1,40}\/?$/;   // a radio behind a front door, or '' for the root

async function iqRegister(request, env) {
  const body = await readBody(request);
  if (!body) return json({ error: 'bad json' }, 400);
  const code = String(body.code || '').toLowerCase(), token = String(body.token || ''), slug = String(body.slug || '').toLowerCase();
  if (!IQ_CODE_RE.test(code) || !IQ_TOKEN_RE.test(token) || !IQ_SLUG_RE.test(slug)) return json({ error: 'bad code, token or slug' }, 400);
  let path = typeof body.path === 'string' ? body.path : '';
  if (path && !IQ_PATH_RE.test(path)) return json({ error: 'bad path' }, 400);
  if (path && !path.endsWith('/')) path += '/';
  // ★ The slug must be a listed receiver — a code pointing nowhere is refused, not stored.
  const srv = await env.DB.prepare('SELECT id FROM servers WHERE slug = ?').bind(slug).first();
  if (!srv) return json({ error: 'no such receiver' }, 404);
  const ip = request.headers.get('cf-connecting-ip') || '';
  const t = now();
  // ★ Rate limit per source address, counting live rows from it — a refresh of the SAME code is
  //   an upsert and does not add to the count.
  const mine = await env.DB.prepare('SELECT COUNT(*) AS n FROM iq_codes WHERE ip = ? AND expires_at > ? AND code != ?').bind(ip, t, code).first();
  if (Number(mine?.n || 0) >= IQ_PER_HOUR) return json({ error: 'too many codes from this address' }, 429);
  // ★ A code already held by ANOTHER session (different token) is not overwritten — it is theirs.
  const held = await env.DB.prepare('SELECT token FROM iq_codes WHERE code = ? AND expires_at > ?').bind(code, t).first();
  if (held && held.token !== token) return json({ error: 'code in use' }, 409);
  await env.DB.batch([
    env.DB.prepare('INSERT OR REPLACE INTO iq_codes (code, slug, token, ip, expires_at, path) VALUES (?,?,?,?,?,?)').bind(code, slug, token, ip, t + IQ_TTL, path),
    env.DB.prepare('DELETE FROM iq_codes WHERE expires_at < ?').bind(t - 3600),
  ]);
  return json({ ok: true, expiresIn: IQ_TTL });
}

async function iqOff(request, env) {
  const body = await readBody(request);
  if (!body) return json({ error: 'bad json' }, 400);
  const code = String(body.code || '').toLowerCase(), token = String(body.token || '');
  if (!IQ_CODE_RE.test(code) || !IQ_TOKEN_RE.test(token)) return json({ error: 'bad code or token' }, 400);
  await env.DB.prepare('DELETE FROM iq_codes WHERE code = ? AND token = ?').bind(code, token).run();
  return json({ ok: true });
}

async function iqLookup(codeRaw, env) {
  const code = String(codeRaw || '').toLowerCase();
  if (!IQ_CODE_RE.test(code)) return json({ error: 'bad code' }, 400);
  const row = await env.DB.prepare('SELECT slug, token, expires_at, path FROM iq_codes WHERE code = ?').bind(code).first();
  if (!row || Number(row.expires_at) <= now()) return json({ error: 'unknown or expired code' }, 404);
  // ★ The bridge reads /vibeserver.json at this host to learn the live tunnel hostname (directUrl).
  return json({ slug: row.slug, host: `${row.slug}.${PUBLIC_ZONE}`, path: row.path || '', token: row.token, expiresAt: Number(row.expires_at) },
              200, { 'cache-control': 'no-store' });
}

/* ══ THE GPU MAP'S TILE PACKS, BY BYTE RANGE ══════════════════════════════════════════════════════
 * ★★ The directory map reads /mapgl/*.pmtiles the way a browser should: only the tiles in view, by
 *    HTTP Range (vibemapgl.js rangeBase) — never 19 MB per visit. Static assets reach us through this
 *    Worker (run_worker_first), and ASSETS.fetch answers a Range with the WHOLE file (measured: 200,
 *    full length). So each pack is put in the edge cache once per data centre, whole, and the Cache
 *    API answers every Range from it with a 206 by itself — no slicing code of ours on the hot path.
 * ★ The cache key carries the deployed file's ETag, so a redeploy is never answered from the old copy.
 * ★ If the cache will not hold it (wrangler dev has none), the bytes are sliced here instead: slower,
 *   never wrong. */
async function servePmtiles(request, env) {
  const url = new URL(request.url); url.search = '';
  const head = await env.ASSETS.fetch(new Request(url.toString(), { method: 'HEAD' }));
  if (!head.ok) return head;
  const etag = head.headers.get('etag') || '';
  const key = `${url.toString()}?v=${encodeURIComponent(etag)}`;
  const range = request.headers.get('range');
  const ask = () => new Request(key, { headers: range ? { range } : {} });
  const cache = caches.default;
  const hit = await cache.match(ask());
  if (hit) return hit;
  const full = await env.ASSETS.fetch(new Request(url.toString()));
  if (!full.ok) return full;
  const body = await full.arrayBuffer();
  const headers = { 'content-type': 'application/octet-stream', 'accept-ranges': 'bytes',
                    'cache-control': 'public, max-age=86400', ...(etag ? { etag } : {}) };
  try {
    await cache.put(key, new Response(body, { headers: { ...headers, 'content-length': String(body.byteLength) } }));
    const again = await cache.match(ask());
    if (again) return again;
  } catch { /* no cache here: slice below */ }
  if (!range) return new Response(body, { headers });
  const m = /^bytes=(\d*)-(\d*)$/.exec(range.trim());
  const size = body.byteLength;
  let a, b;
  if (m && m[1] !== '') { a = Number(m[1]); b = m[2] !== '' ? Math.min(Number(m[2]), size - 1) : size - 1; }
  else if (m && m[2] !== '') { a = Math.max(0, size - Number(m[2])); b = size - 1; }
  if (a === undefined || a >= size || b < a) return new Response(null, { status: 416, headers: { 'content-range': `bytes */${size}` } });
  return new Response(body.slice(a, b + 1), { status: 206, headers: { ...headers, 'content-range': `bytes ${a}-${b}/${size}` } });
}

const SPEEDTEST_MAX = 8 * 1024 * 1024;

/* ★★★ IQ CODE LOOKUPS ARE RATE-LIMITED PER ADDRESS (security audit, 2026-10-03). A lookup answers
 *     with the session TOKEN the receiver checks on /ws/iq, and a code is six characters — so an
 *     unlimited GET was a way to walk the code space and collect live tokens. Registration was
 *     already limited (IQ_PER_HOUR); lookup was not.
 *  ★ Cloudflare's Rate Limiting binding (IQ_LOOKUP_LIMIT in wrangler.jsonc: 30 a minute per address)
 *    when it is bound; a per-isolate counter otherwise (wrangler dev, or a deploy before the binding
 *    exists) — weaker, since isolates are many, but never nothing. No D1 write per GET. */
const IQ_LOOKUPS_PER_MIN = 30;
const iqLocal = new Map();     // ip -> { n, start }
async function iqLookupAllowed(request, env) {
  const ip = request.headers.get('cf-connecting-ip') || 'unknown';
  if (env && env.IQ_LOOKUP_LIMIT && typeof env.IQ_LOOKUP_LIMIT.limit === 'function') {
    try { return (await env.IQ_LOOKUP_LIMIT.limit({ key: 'iq:' + ip })).success; }
    catch (e) { console.error('iq lookup limiter failed', String((e && e.message) || e)); }
  }
  const t = Date.now();
  const rec = iqLocal.get(ip);
  if (!rec || t - rec.start > 60000) {
    if (iqLocal.size > 5000) iqLocal.clear();
    iqLocal.set(ip, { n: 1, start: t });
    return true;
  }
  rec.n++;
  return rec.n <= IQ_LOOKUPS_PER_MIN;
}

/* ★★★ SECURITY HEADERS ON EVERYTHING THE DIRECTORY SERVES ITSELF (security audit, 2026-10-03). Not
 *     on a proxied receiver's responses — those carry the receiver's own policy.
 *  ★★ store.html IS FRAMED ON PURPOSE, by every https://<slug>.vibeserver.vibesdr.net page
 *     (web/client/src/portable.ts) — so its frame-ancestors names exactly them, and it gets no
 *     X-Frame-Options (which cannot express a list). Every other page may not be framed at all.
 *  ★ The directory page's full CSP is REPORT-ONLY for now: it loads Leaflet, MapLibre (blob:
 *    workers), live counts from every receiver and the pmtiles, and a policy that broke the map would
 *    be worse than none. The enforced header carries only what cannot break it. Tighten once the
 *    report-only one has run clean in a browser. */
const SEC_BASE = {
  'x-content-type-options': 'nosniff',
  'referrer-policy': 'strict-origin-when-cross-origin',
};
const CSP_STORE = "default-src 'none'; script-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; "
                + "frame-ancestors https://*.vibeserver.vibesdr.net";
const CSP_PAGE_ENFORCED = "frame-ancestors 'none'; object-src 'none'; base-uri 'self'";
const CSP_PAGE_REPORT = "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
                      + "img-src 'self' data: blob: https:; font-src 'self' data:; "
                      + "connect-src 'self' https: wss: data: blob:; worker-src 'self' blob:; child-src 'self' blob:; "
                      + "frame-src 'self'; object-src 'none'; base-uri 'self'; form-action 'self'; frame-ancestors 'none'";
function withSecurityHeaders(res, pathname) {
  let out;
  try { out = new Response(res.body, res); } catch { return res; }   // ★ e.g. a 101 — leave it be
  const h = out.headers;
  for (const [k, v] of Object.entries(SEC_BASE)) if (!h.has(k)) h.set(k, v);
  const type = h.get('content-type') || '';
  if (type.includes('text/html')) {
    if (pathname === '/store' || pathname === '/store.html') {
      h.set('content-security-policy', CSP_STORE);
      h.delete('x-frame-options');
    } else {
      h.set('x-frame-options', 'DENY');
      h.set('content-security-policy', CSP_PAGE_ENFORCED);
      h.set('content-security-policy-report-only', CSP_PAGE_REPORT);
    }
  }
  return out;
}

export default {
  /* ★★★ THE REQUEST LOG CARRIES NO QUERY STRING (security audit, 2026-10-03). Proxied receiver
   *     requests carry credentials in their query (vs_nonce + vs_auth, a reusable hour-long session
   *     proof; vs_admin_nonce + vs_admin_auth; an older directory's vs_admin_ticket), and Workers
   *     Logs' automatic INVOCATION log records the full request URL. wrangler.jsonc turns invocation
   *     logs off; this line replaces them — host, path, method, status, country — so the traffic
   *     record ("traffic_drop" queries) survives without a single credential in it. */
  async fetch(request, env, ctx) {
    const t0 = Date.now();
    let res, err = null;
    try { res = await handle(request, env, ctx); }
    catch (e) { err = e; throw e; }
    finally {
      try {
        const u = new URL(request.url);
        console.log(JSON.stringify({
          req: request.method, host: u.hostname, path: u.pathname.slice(0, 200),
          status: res ? res.status : 500, ms: Date.now() - t0,
          country: (request.cf && request.cf.country) || '', ...(err ? { error: String(err).slice(0, 200) } : {}),
        }));
      } catch { /* logging must never fail a request */ }
    }
    return res;
  },
};

async function handle(request, env) {
  {
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
      if (p === '/api/directory' && request.method === 'GET') return await list(env, url, request);
      if (p === '/api/directory/name' && request.method === 'GET') return await checkName(url, env);
      if (p === '/api/directory/register' && request.method === 'POST') return await register(request, env);
      if (p === '/api/directory/ping' && request.method === 'POST') return await ping(request, env);
      if (p === '/api/directory/delist' && request.method === 'POST') return await delist(request, env);
      if (p === '/api/eibi' && request.method === 'GET') return await eibi(request);
      // ★ The server benchmark's UPLINK test (vibe_benchmark / Stuart, 2026-09-19): the server posts a few MB, we read
      //   and discard it, and it times the upload. Listeners it can carry = uplink / 100 kB/s (his worst case).
      //   Read and dropped, never stored; capped so it cannot be used to push arbitrary volumes through us.
      if (p === '/api/speedtest' && request.method === 'POST') {
        const len = Number(request.headers.get('content-length') || 0);
        if (len > SPEEDTEST_MAX) return json({ error: 'too large' }, 413);
        /* ★★ COUNTED AS IT ARRIVES (audit, 2026-10-03). A chunked upload carries no content-length,
         *    so the check above passed it and arrayBuffer() buffered whatever was sent. Now the
         *    bytes are read and dropped as they stream, and the upload is cut at the cap. */
        let bytes = 0;
        if (request.body) {
          const reader = request.body.getReader();
          for (;;) {
            const { done, value } = await reader.read();
            if (done) break;
            bytes += value.byteLength;
            if (bytes > SPEEDTEST_MAX) {
              try { await reader.cancel(); } catch { /* already gone */ }
              return json({ error: 'too large' }, 413);
            }
          }
        }
        return json({ bytes });
      }
      if (p === '/api/iq' && request.method === 'POST') return await iqRegister(request, env);
      if (p === '/api/iq/off' && request.method === 'POST') return await iqOff(request, env);
      if (p.startsWith('/api/iq/') && request.method === 'GET') {
        if (!await iqLookupAllowed(request, env)) {
          return json({ error: 'too many lookups from this address, try again in a minute' }, 429,
                      { 'retry-after': '60', 'cache-control': 'no-store' });
        }
        return await iqLookup(p.slice('/api/iq/'.length), env);
      }
    } catch (err) {
      // ★ Never leak a D1 error to a caller; it names tables.
      console.error('directory error', (err && err.stack) || String(err));
      return json({ error: 'server error' }, 500);
    }

    if (p.startsWith('/mapgl/') && p.endsWith('.pmtiles') && request.method === 'GET') return servePmtiles(request, env);
    return withSecurityHeaders(await env.ASSETS.fetch(request), p);
  }
}
