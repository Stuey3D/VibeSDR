/**
 * vibesdr.net — static site, plus one small API the demo card uses.
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

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

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
