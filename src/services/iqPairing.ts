/**
 * RAW IQ OUT through the tunnel — the pairing code's registration with the directory.
 *
 * ★★★ WHY THE CLIENT REGISTERS, NOT THE SERVER. The directory does not proxy streams, only the
 *     page, so the VibeIQ bridge on somebody's PC must reach the server's own tunnel hostname —
 *     and that rotates on every restart. The one stable name is the directory slug
 *     (<slug>.vibeserver.vibesdr.net), and a six-character code is what a person can type. Somebody
 *     has to tell the directory "code → slug + token". The server could, but it has TWO directory
 *     registrars already (directory.cpp on Linux, VibeTunnel.kt on Android) and a third copy of
 *     the key-holding logic is the "one rule, two readers" fault waiting to happen. The client is
 *     ON the slug host, holds the token the server just issued, and is one fetch away.
 *  ★ A bogus registration buys nothing: the token is checked by the real server on /ws/iq.
 *  ★ Refreshed every ten minutes while on; removed when off. The directory expires it anyway.
 */
const DIRECTORY = 'https://vibeserver.vibesdr.net';
const ZONE = '.vibeserver.vibesdr.net';

let timer: ReturnType<typeof setInterval> | null = null;
let current: { code: string; token: string; slug: string } | null = null;

/** The slug when `base` is a directory address, else null (LAN and raw tunnel hosts have none). */
export function slugOf(base: string): string | null {
  try {
    const h = new URL(base).hostname.toLowerCase();
    if (!h.endsWith(ZONE)) return null;
    const s = h.slice(0, -ZONE.length);
    return s && !s.includes('.') ? s : null;
  } catch { return null; }
}

async function post(path: string, body: unknown): Promise<void> {
  try {
    await fetch(DIRECTORY + path, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
  } catch { /* the directory being away is not the listener's problem — the LAN path is untouched */ }
}

/** Call with the server's `iqout` answer: a code + token registers (and keeps registering);
 *  undefined clears. `base` is the address this session connected to. */
export function registerIqCode(base: string, code?: string, token?: string): void {
  if (timer) { clearInterval(timer); timer = null; }
  if (current && (!code || current.code !== code)) { post('/api/iq/off', { code: current.code, token: current.token }); current = null; }
  const slug = slugOf(base);
  if (!code || !token || !slug) return;
  current = { code, token, slug };
  const send = () => post('/api/iq', { code, token, slug });
  send();
  timer = setInterval(send, 10 * 60 * 1000);
}
