/**
 * vibesdr.net — static site, plus two small APIs: /api/demo (the old status card) and
 * /api/spectrogram (the live HF record behind the hero, cached at the edge).
 *
 * ★★★ WHY THE WORKER FETCHES THE RECEIVER AND THE BROWSER DOES NOT. The site is HTTPS; the demo Pi
 *     is plain HTTP on a home connection. A browser on an HTTPS page is FORBIDDEN from fetching
 *     http:// — it is mixed content and is blocked outright, so a status card written the obvious
 *     way would show nothing and log a console error. Fetching from here happens server-side,
 *     where that rule does not apply.
 *
 * ★★★ AND IT KEEPS STUART'S HOME ADDRESS OFF THE PAGE. The receiver lives on a residential line;
 *     the card would otherwise publish its hostname to every visitor, including the ones who read
 *     the source. Only this Worker knows where it is.
 *
 * ★★ NEVER LET A DEAD DEMO SLOW THE SITE DOWN. Every request to the receiver has a short deadline
 *    and the answer is cached, so a Pi that is switched off costs a visitor a second at most and
 *    the card simply does not appear. A demo link that hangs is worse than no demo link.
 */

/** Where the demo receiver actually is. The one place that knows. */
// ★★★ THROUGH THE TUNNEL, NOT THE FRONT DOOR. This was a DDNS name and a forwarded port, which
//     works but publishes a home address and gives every visitor a "Not Secure" warning on a link
//     from an HTTPS page. cloudflared runs on the Pi and dials OUT, so there is no port to forward,
//     nothing to re-point when the ISP changes the address, and the receiver gets real TLS.
const DEMO_ORIGIN = 'https://demo.vibesdr.net';

/** ★ Short: the point is a LIVE count. Long enough that a busy minute is a handful of requests to
 *  a Raspberry Pi rather than one per visitor. */
const CACHE_SECONDS = 15;
/** ★ A receiver that is off must not hold the page up. */
const TIMEOUT_MS = 2500;

async function askReceiver(path) {
  const res = await fetch(DEMO_ORIGIN + path, {
    signal: AbortSignal.timeout(TIMEOUT_MS),
    cf: { cacheTtl: CACHE_SECONDS, cacheEverything: true },
    headers: { 'user-agent': 'vibesdr.net status card' },
  });
  if (!res.ok) throw new Error(`${path} -> ${res.status}`);
  return res.json();
}

/**
 * What the site is allowed to know. Deliberately a SUBSET of what the receiver publishes:
 * ★★★ NO SERIAL NUMBERS. The directory carries them because that is how a client routes to a
 *     radio, but they are hardware identity and have no business on a public web page — the same
 *     rule the receiver's own landing page follows. They are used here and dropped here.
 */
async function demoStatus() {
  const dir = await askReceiver('/vibeserver/radios');
  const radios = (dir.radios || []).filter((r) => r && r.serial);

  // ★ In parallel, and a radio that does not answer is reported as down rather than failing the
  //   whole card — one wedged receiver should not hide the two that are working.
  const states = await Promise.all(radios.map(async (r) => {
    try {
      // ★ By the opaque id where the server offers one — the same reason the links use it.
      const j = await askReceiver(`/r/${encodeURIComponent(r.id || r.serial)}/vibeserver.json`);
      return { ok: true, j };
    } catch { return { ok: false, j: {} }; }
  }));

  const out = radios.map((r, i) => {
    const j = states[i].j;
    const max = Number(j.maxUsers || r.users || 1);
    return {
      name: r.label,                       // already stripped of any serial by the server
      driver: r.driver,
      // ★ "shared" is the honest word for what a listener gets: their own tuning inside the
      //   owner's window. `locked` is the server's internal name for the same thing.
      shared: !!r.locked,
      mode: r.mode || '',
      centreHz: r.centreHz || 0,
      spanHz: r.spanHz || 0,
      listeners: Number(j.listeners || 0),
      maxListeners: max,
      queue: Number(j.waiting || 0),
      // ★ A single-listener radio with somebody on it is FULL, not merely busy — that is the
      //   distinction a visitor cares about before they click.
      full: Number(j.listeners || 0) >= max,
      // ★ How long until the current occupant's turn ends. -1 means no limit, or nobody on it.
      //   The receiver already tracks this for its own countdown; the card just repeats it.
      freeInSec: Number.isFinite(Number(j.freeInSec)) ? Number(j.freeInSec) : -1,
      // ★ Coverage is the hardware's reach; restricted says whether the owner has narrowed it.
      //   Both travel so the card can say WHICH wall a listener would hit.
      coverage: Array.isArray(r.coverage) ? r.coverage : [],
      restricted: !!r.restricted,
      allowList: r.allowList || '',
      blockList: r.blockList || '',
      up: states[i].ok,
    };
  });

  return {
    onAir: out.some((r) => r.up),
    url: DEMO_ORIGIN,
    radios: out,
    listeners: out.reduce((n, r) => n + r.listeners, 0),
    queue: out.reduce((n, r) => n + r.queue, 0),
  };
}


/* ══ THE LIVE SPECTROGRAM BEHIND THE HERO ═══════════════════════════════════════════════════════
 * ★★★ VISITORS NEVER FETCH IT FROM THE PI (briefs/BRIEF-website-redesign.md, "push-and-cache").
 *     Stuart's own diagnosis: "the fanout would hammer my bandwidth or cost me my cloudflare
 *     account". So the Worker fetches the receiver's 24-hour record ONCE per SPG_REFRESH_S per
 *     edge location, shrinks it here, and keeps the result in the edge cache for a week. A visitor
 *     is served from that cache; the Pi sees a handful of requests an hour however many people
 *     are on the page.
 * ★★★ A POWER CUT DOES NOT BLANK THE PAGE. The cached copy outlives the receiver: when the Pi is
 *     off, whatever was last fetched is served, marked stale in a header. The page shows the real
 *     time span of what it has, never a fixed "24 hours".
 * ★★ SHRUNK HERE, NOT IN THE BROWSER. The full record is 1440 rows x 2048 bins = 2.9 MB; nobody
 *    should download that for a backdrop. 512 x 360 (max-hold over 4 bins and 4 minutes, so a
 *    brief carrier is kept, not averaged away) is 184 KB and still shows every station.
 * ★ It is the RSP's WINDOW (2.8–10.8 MHz), not the network's — a readable spectrogram needs every
 *   row to share a profile, which only a locked radio gives. The caption says so.
 *
 * Wire in (GET /vibeserver/spectrogram): "VSPG" u8 ver u16 bins u16 rows f64 centreHz f64 spanHz,
 *   then per row i64 epoch-ms + `bins` bytes of dB.
 * Wire out (/api/spectrogram): "VSPW" u8 ver=1 u16 bins u16 rows f64 centreHz f64 spanHz
 *   i64 firstMs i64 lastMs, then rows x bins bytes, oldest row first. Little-endian throughout.
 */
const SPG_REFRESH_S = 600;          // how often ONE edge location asks the Pi
const SPG_KEEP_S = 7 * 24 * 3600;   // how long the last good picture survives a dead receiver
const SPG_BINS = 512, SPG_ROWS = 360;
const SPG_TIMEOUT_MS = 12000;       // 3 MB through a home uplink is not a 2.5 s job

function shrinkSpectrogram(buf) {
  const dv = new DataView(buf), u8 = new Uint8Array(buf);
  if (u8.length < 25 || String.fromCharCode(u8[0], u8[1], u8[2], u8[3]) !== 'VSPG') throw new Error('not VSPG');
  const bins = dv.getUint16(5, true), rows = dv.getUint16(7, true);
  const centre = dv.getFloat64(9, true), span = dv.getFloat64(17, true);
  const stride = 8 + bins;
  if (rows < 1 || bins < 1 || u8.length < 25 + rows * stride) throw new Error('short VSPG');
  const outRows = Math.min(SPG_ROWS, rows), outBins = Math.min(SPG_BINS, bins);
  const rStep = rows / outRows, bStep = bins / outBins;
  const out = new Uint8Array(41 + outRows * outBins);
  const odv = new DataView(out.buffer);
  out.set([0x56, 0x53, 0x50, 0x57, 1]);                       // "VSPW", version 1
  odv.setUint16(5, outBins, true); odv.setUint16(7, outRows, true);
  odv.setFloat64(9, centre, true); odv.setFloat64(17, span, true);
  odv.setBigInt64(25, dv.getBigInt64(25, true), true);
  odv.setBigInt64(33, dv.getBigInt64(25 + (rows - 1) * stride, true), true);
  let o = 41;
  for (let r = 0; r < outRows; r++) {
    const r0 = Math.floor(r * rStep), r1 = Math.max(r0 + 1, Math.floor((r + 1) * rStep));
    for (let b = 0; b < outBins; b++) {
      const b0 = Math.floor(b * bStep), b1 = Math.max(b0 + 1, Math.floor((b + 1) * bStep));
      let best = 0;
      for (let rr = r0; rr < r1; rr++) {
        const base = 25 + rr * stride + 8;
        for (let bb = b0; bb < b1; bb++) { const v = u8[base + bb]; if (v > best) best = v; }
      }
      out[o++] = best;
    }
  }
  return out;
}

async function spectrogram(request, ctx) {
  const cache = caches.default;
  const key = new Request(new URL('/api/spectrogram', request.url).toString(), { method: 'GET' });
  const hit = await cache.match(key);
  const fetchedAt = hit ? Number(hit.headers.get('x-fetched-at')) || 0 : 0;
  const ageS = (Date.now() - fetchedAt) / 1000;

  const reply = (body, fetched, stale) => new Response(body, {
    headers: {
      'content-type': 'application/octet-stream',
      'cache-control': 'public, max-age=300',
      'access-control-allow-origin': '*',
      'access-control-expose-headers': 'x-fetched-at, x-stale',
      'x-fetched-at': String(fetched),
      'x-stale': stale ? '1' : '0',
    },
  });

  if (hit && ageS < SPG_REFRESH_S) return reply(hit.body, fetchedAt, false);

  try {
    const res = await fetch(`${DEMO_ORIGIN}/vibeserver/spectrogram?rows=1440`, {
      signal: AbortSignal.timeout(SPG_TIMEOUT_MS),
      headers: { 'user-agent': 'vibesdr.net hero spectrogram' },
    });
    if (!res.ok) throw new Error(`spectrogram -> ${res.status}`);
    const small = shrinkSpectrogram(await res.arrayBuffer());
    const now = Date.now();
    // ★ Stored under a LONG max-age so a dead receiver still has a picture to show; freshness is
    //   judged by x-fetched-at above, not by the cache expiring.
    const stored = new Response(small, { headers: { 'cache-control': `public, max-age=${SPG_KEEP_S}`, 'x-fetched-at': String(now) } });
    ctx.waitUntil(cache.put(key, stored.clone()));
    return reply(small, now, false);
  } catch {
    if (hit) return reply(hit.body, fetchedAt, true);
    // ★ Nothing cached and nothing reachable: the page draws no background and prints no claim.
    return new Response(null, { status: 204, headers: { 'access-control-allow-origin': '*' } });
  }
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (url.pathname === '/api/spectrogram') return spectrogram(request, ctx);

    if (url.pathname === '/api/demo') {
      let body;
      try {
        body = await demoStatus();
      } catch {
        // ★ Off, unreachable, or mid-restart. Not an error worth a 500: the card asks, hears "no",
        //   and stays hidden. 200 keeps it out of the browser console on a page that is fine.
        body = { onAir: false, radios: [], listeners: 0, queue: 0 };
      }
      return new Response(JSON.stringify(body), {
        headers: {
          'content-type': 'application/json; charset=utf-8',
          'cache-control': `public, max-age=${CACHE_SECONDS}`,
          'access-control-allow-origin': '*',
        },
      });
    }

    return env.ASSETS.fetch(request);
  },
};
