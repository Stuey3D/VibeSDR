/**
 * lanHop.ts — V12 SAME NETWORK → GO DIRECT, in the browser (Stuart, 2026-10-30: his own Lenovo, opened from the
 * directory, went Lenovo → cloudflared → Cloudflare → back home: ~80 ms pings, a 390 ms jitter buffer, 3-4 s drops).
 *
 * ★★★ HOW THE PAGE KNOWS. Opened through <slug>.vibeserver.vibesdr.net by a visitor on the server's OWN network, the
 *     directory injects <meta name="vibe-lan" content="192.168.86.13:48001|nonce|proof"> — and only for that visitor
 *     (directory/src/index.js lanColumnsFor). Anybody else's page carries nothing, and this file does nothing.
 *
 * ★★★ AND IT PROVES BEFORE IT MOVES (src/utils/lanDirect.ts): http://<lan>/vibeserver.json?dirNonce=<nonce> must
 *     come back with the server's own HMAC. Then the page goes to http://<lan><same path, query and #fragment> — the
 *     same door, so a radio's /r/<id>/ and a directory PIN signature (#vs_nonce=…, minted by this very process)
 *     carry straight over.
 *
 * ★★★ THE BROWSER DECIDES WHETHER IT CAN EVEN ASK. This page is https and the LAN is plain http, so the probe is
 *     mixed content. Chromium (Chrome / Edge 142+) has Local Network Access: a fetch to a private address with
 *     `targetAddressSpace: 'local'` is allowed once the visitor grants "look for devices on your local network".
 *       · GRANTED → proved at boot, before the landing page draws (≤ ~1.5 s), and the page moves on its own.
 *       · NOT YET ASKED → the landing page offers "CONNECT DIRECTLY"; the press asks the browser (its own prompt),
 *         proves, and moves — or says it could not and stays on the tunnel. A prompt is never raised unasked.
 *       · Safari, Firefox and older Chromium cannot probe http from https at all. They get a plain link to the LAN
 *         address — the visitor's own choice, which works everywhere as a top-level navigation; if they are not
 *         really on that network the browser says it cannot reach it and Back returns here.
 *       · DENIED → nothing is offered; the visitor said no.
 *  ★ On the LAN page itself (plain http) there is no meta and nothing happens. Settings there are that origin's own:
 *    a browser keeps localStorage per origin, so the LAN page has its own view settings, separate from the tunnel's.
 */
import { parseLanMeta, proveLan, makeLanFetch, type LanOffer } from '../../../src/utils/lanDirect';

/** The directory's offer on this page, or null. Only on an https page — a plain http one is already direct. */
export function lanOfferFromPage(): LanOffer | null {
  if (typeof document === 'undefined' || location.protocol !== 'https:') return null;
  return parseLanMeta(document.querySelector('meta[name="vibe-lan"]')?.getAttribute('content'));
}

/** Where the page goes: the same path, query and fragment on the LAN address. */
export function hopUrl(host: string, loc: { pathname: string; search: string; hash: string }): string {
  return `http://${host}${loc.pathname}${loc.search}${loc.hash}`;
}

export type LnaState = 'granted' | 'prompt' | 'denied' | 'unsupported';
/** ★ Local Network Access, as the Permissions API names it — 'local-network-access' (Chrome 142), and the later
 *  split name 'local-network'. A browser that knows neither cannot probe the LAN from https at all. */
export async function lnaState(): Promise<LnaState> {
  const perms: any = (typeof navigator !== 'undefined' && (navigator as any).permissions) || null;
  if (!perms?.query) return 'unsupported';
  for (const name of ['local-network-access', 'local-network']) {
    try {
      const st = await perms.query({ name });
      if (st && (st.state === 'granted' || st.state === 'prompt' || st.state === 'denied')) return st.state;
    } catch { /* this browser does not know the name */ }
  }
  return 'unsupported';
}

// ★ The LNA hint: lets Chromium treat the http fetch as a local-network request instead of blocking it as mixed
//   content. Other browsers ignore an option they do not know (and block the fetch, which proveLan reads as "no").
const lnaFetch = makeLanFetch({ targetAddressSpace: 'local' });

/** Prove and move. True when the page is navigating away. */
async function proveAndGo(offer: LanOffer, timeoutMs: number): Promise<boolean> {
  const host = await proveLan(offer, lnaFetch, timeoutMs);
  if (!host) return false;
  location.replace(hopUrl(host, location));
  return true;
}

/**
 * ★ At boot, BEFORE the landing page starts asking the server things over the tunnel. Resolves true when the page is
 * moving to the LAN (the caller then draws nothing); false to carry on as ever. Never asks the visitor anything.
 */
export async function lanHopAtBoot(offer: LanOffer): Promise<boolean> {
  if (await lnaState() !== 'granted') return false;
  return proveAndGo(offer, 1500);
}

/**
 * The landing page's offer, when the boot hop did not happen: a button that asks (Chromium, not yet granted) or a
 * plain link (a browser that cannot probe). Draws into #splashLan; nothing when denied or when there is no offer.
 */
export async function offerLanOnSplash(offer: LanOffer | null): Promise<void> {
  const el = document.getElementById('splashLan');
  if (!el || !offer) return;
  const st = await lnaState();
  if (st === 'denied' || st === 'granted') return;   // granted and still here = it did not prove; the tunnel stays
  el.textContent = '';
  const lead = document.createElement('span');
  lead.textContent = 'ON YOUR NETWORK · ';
  el.appendChild(lead);
  if (st === 'prompt') {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'lanGo';
    b.textContent = 'CONNECT DIRECTLY';
    b.title = 'This receiver is on your own network. Connecting directly skips the internet tunnel — '
            + 'your browser will ask to look for devices on your local network.';
    b.addEventListener('click', async () => {
      b.disabled = true;
      b.textContent = 'CHECKING…';
      // ★ Generous: the browser's own prompt is on screen and the visitor is reading it.
      if (await proveAndGo(offer, 20000)) return;
      lead.textContent = '';
      b.remove();
      el.appendChild(document.createTextNode('COULD NOT REACH IT DIRECTLY — STAYING ON THE INTERNET ADDRESS'));
    });
    el.appendChild(b);
  } else {
    // ★ No probe is possible here, so the visitor chooses. The first offered address; the link names it.
    const a = document.createElement('a');
    a.className = 'lanGo';
    a.href = hopUrl(offer.hosts[0], location);
    a.textContent = `OPEN DIRECTLY (${offer.hosts[0]})`;
    a.title = 'This receiver is on your own network. Opening it directly skips the internet tunnel.';
    el.appendChild(a);
  }
  el.hidden = false;
}
