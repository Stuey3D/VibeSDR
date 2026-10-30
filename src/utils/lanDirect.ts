/**
 * lanDirect.ts — SAME NETWORK → GO DIRECT (V12; Stuart, 2026-10-30). Shared by the app (InstancePickerScreen) and
 * the web client (web/client/src/lanHop.ts); no React Native, no DOM.
 *
 * ★★★ WHY. Stuart opens his own Lenovo from the directory "as its easier", so every byte went Lenovo → cloudflared →
 *     Cloudflare → back home: ~80 ms pings, the jitter buffer grown to 390 ms, 3-4 s drops whenever the tunnel
 *     stalled — while the same server across the room is ~6 ms with no loss. The directory now hands a listener on
 *     the SAME NETWORK as a server that server's LAN address (directory/src/index.js lanColumnsFor — never to
 *     anybody else), and the client goes there instead of through the tunnel.
 *
 * ★★★ BUT ONLY AFTER PROVING IT. "Same public address" is not "can reach it": guest Wi-Fi, a carrier NAT, phone data
 *     on the same carrier address — and a 192.168.1.20 on some other network may be a different box entirely. So the
 *     offer carries a nonce and the HMAC the real server answers it with (the shim's dirProof, the same challenge the
 *     directory verifies listings by). We ask http://<lan>/vibeserver.json?dirNonce=<nonce> with a short timeout and
 *     go direct ONLY when dirProof matches. Anything else — no answer, a wrong answer, a different box — and the
 *     caller uses the tunnel exactly as it always has.
 */

/** What the directory offers a same-network client: `lan` on /api/directory, or the page's vibe-lan meta. */
export interface LanOffer {
  /** "192.168.86.13:48001" — private IPv4 with a port; at most four. */
  hosts: string[];
  nonce: string;
  /** HMAC-SHA256(server key, nonce), lowercase hex. */
  proof: string;
}

/** ★ Only an RFC 1918 IPv4 address with a port — the same rule the directory applies (cleanLan). Anything else is not
 *  something we will aim a request at, whatever the listing says. */
export function isLanHost(h: unknown): h is string {
  if (typeof h !== 'string') return false;
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})$/.exec(h);
  if (!m) return false;
  const o = m.slice(1, 5).map(Number), port = Number(m[5]);
  if (o.some((n) => n > 255) || port < 1 || port > 65535) return false;
  return o[0] === 10 || (o[0] === 192 && o[1] === 168) || (o[0] === 172 && o[1] >= 16 && o[1] <= 31);
}

/** The listing's `lan` object, checked, or null. */
export function parseLanOffer(v: unknown): LanOffer | null {
  if (!v || typeof v !== 'object') return null;
  const o = v as { hosts?: unknown; nonce?: unknown; proof?: unknown };
  const hosts = Array.isArray(o.hosts) ? o.hosts.filter(isLanHost).slice(0, 4) : [];
  const nonce = typeof o.nonce === 'string' && /^[a-z0-9]{8,64}$/.test(o.nonce) ? o.nonce : '';
  const proof = typeof o.proof === 'string' && /^[0-9a-f]{64}$/.test(o.proof) ? o.proof : '';
  return hosts.length && nonce && proof ? { hosts, nonce, proof } : null;
}

/** The page's `<meta name="vibe-lan" content="h1,h2|nonce|proof">`, as the directory injects it. */
export function parseLanMeta(content: unknown): LanOffer | null {
  if (typeof content !== 'string' || content.length > 400) return null;
  const [h, nonce, proof] = content.split('|');
  return parseLanOffer({ hosts: (h || '').split(','), nonce, proof });
}

/** The probe URL for one host. */
export const lanProbeUrl = (host: string, nonce: string) =>
  `http://${host}/vibeserver.json?dirNonce=${encodeURIComponent(nonce)}`;

/** Fetch JSON from a URL, giving up after `timeoutMs` — supplied by the caller (RN fetch, or the browser's with its
 *  Local Network Access option). Resolves null (never throws) on any failure. */
export type LanFetch = (url: string, timeoutMs: number) => Promise<unknown>;

/**
 * ★★ PROVE ONE OF THE OFFERED ADDRESSES IS THIS SERVER. All are asked at once (a box can have Wi-Fi and Ethernet);
 * the first whose dirProof matches wins. Resolves the winning "host:port", or null when none proved within the
 * timeout — the caller then uses the tunnel. Never throws.
 * ★ ~1.5 s by default: a LAN answers in milliseconds, and an address that is not on this network (another site's
 *   192.168.x.x, a guest network) should cost the listener as little as possible before the tunnel takes over.
 */
export async function proveLan(offer: LanOffer | null, fetchJson: LanFetch, timeoutMs = 1500): Promise<string | null> {
  if (!offer || !offer.hosts.length) return null;
  const want = offer.proof.toLowerCase();
  return new Promise<string | null>((resolve) => {
    let left = offer.hosts.length, done = false;
    const finish = (v: string | null) => { if (!done) { done = true; clearTimeout(timer); resolve(v); } };
    const timer = setTimeout(() => finish(null), timeoutMs + 100);
    for (const host of offer.hosts) {
      Promise.resolve()
        .then(() => fetchJson(lanProbeUrl(host, offer.nonce), timeoutMs))
        .catch(() => null)
        .then((j: any) => {
          const got = j && typeof j.dirProof === 'string' ? j.dirProof.toLowerCase() : '';
          if (got && got === want) finish(host);
          else if (--left === 0) finish(null);
        });
    }
  });
}

/** ★ The ordinary fetch for proveLan (the app; and the web page's, with `extra` carrying Local Network Access's
 *  `targetAddressSpace`). An AbortController rather than AbortSignal.timeout so an older browser engine runs it too. */
export function makeLanFetch(extra: Record<string, unknown> = {}): LanFetch {
  return async (url, timeoutMs) => {
    const ctl = typeof AbortController !== 'undefined' ? new AbortController() : null;
    const t = setTimeout(() => { try { ctl?.abort(); } catch { /* gone */ } }, timeoutMs);
    try {
      const r = await fetch(url, { cache: 'no-store', ...(ctl ? { signal: ctl.signal } : {}), ...extra } as RequestInit);
      return r.ok ? await r.json() : null;
    } catch {
      return null;
    } finally {
      clearTimeout(t);
    }
  };
}
