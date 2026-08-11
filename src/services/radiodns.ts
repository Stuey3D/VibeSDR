/**
 * RadioDNS station logos, resolved IN THE APP.
 *
 * ★★★ WHY THE APP CAN DO THIS AND A BROWSER CANNOT. The blocker was never DNS — DoH is an
 *     ordinary HTTPS GET. It is the SECOND half: the broadcaster's SPI document is served with no
 *     CORS headers, so a page may issue the request and is then forbidden to READ the reply.
 *     React Native enforces no such rule, so the whole chain runs here in TypeScript.
 *     ★★ Which matters because it reaches the servers the VibeServer route cannot: Kiwi, OWRX,
 *        FM-DX and other people's UberSDRs have no /vibeserver/stationlogo to ask. Against a
 *        VibeServer, PREFER THE SERVER — it caches per machine, so one lookup serves every
 *        listener instead of every listener repeating it.
 *
 * ★★★ IDENTITY, NOT NAME. This is keyed on the PI, which the transmitter error-protects and
 *     repeats ~11 times a second, and it returns the BROADCASTER'S OWN artwork. The name search
 *     it backs up (stationLogo.ts) can be fooled by one bad PS decode — and because the logo is
 *     invalidated only by a PI change, a wrong answer LOCKS: a weak BBC Radio 3 wore the Radio 1
 *     roundel and kept it (Stuart, 2026-08-11).
 *
 * Three traps, all paid for already in vibeserver/radiodns.cpp — this is a faithful port:
 *   1. THE GCC IS NOT THE ECC. It is the PI's country nibble followed by the ECC: PI C202 + ECC
 *      E1 gives "ce1". The ECC alone resolves nothing, and it looks like "not in RadioDNS".
 *   2. THE CNAME TARGET IS NOT A WEB SERVER. 08810.c202.ce1.fm.radiodns.org resolves to
 *      radiodns.api.bbci.co.uk, which has NO ADDRESS RECORD AT ALL. The service is found with an
 *      SRV lookup under that name — and the SRV's PORT is what says http or https.
 *   3. FREQUENCY IS MHz x 100, five digits, zero padded: 88.1 -> "08810" (units of 10 kHz).
 */

const DOH = 'https://cloudflare-dns.com/dns-query';

/** ★ A day for a hit, an hour for a miss — a station JOINING RadioDNS is worth noticing sooner
 *  than a logo changing, but not at the cost of a lookup every time somebody tunes past. */
const HIT_TTL = 24 * 3600 * 1000;
const MISS_TTL = 3600 * 1000;

const cache = new Map<string, { url: string; at: number }>();
const inflight = new Map<string, Promise<string>>();

async function dns(name: string, type: 'CNAME' | 'SRV'): Promise<string> {
  const r = await fetch(`${DOH}?name=${encodeURIComponent(name)}&type=${type}`,
                        { headers: { accept: 'application/dns-json' } });
  if (!r.ok) return '';
  const j: any = await r.json();
  const ans = Array.isArray(j?.Answer) ? j.Answer : [];
  // ★ Take the record of the type we ASKED for. A CNAME query often answers with the CNAME chain
  //   plus other types, and picking Answer[0] blindly picks whichever the resolver put first.
  const want = type === 'CNAME' ? 5 : 33;
  const rec = ans.find((a: any) => a?.type === want) ?? ans[0];
  let data = String(rec?.data ?? '').trim();
  if (type === 'CNAME' && data.endsWith('.')) data = data.slice(0, -1);
  return data;
}

/** "prio weight port target" -> "host:port". */
async function srv(name: string): Promise<string> {
  const data = await dns(name, 'SRV');
  const m = data.match(/^(\d+)\s+(\d+)\s+(\d+)\s+(\S+)$/);
  if (!m) return '';
  const port = Number(m[3]);
  let host = m[4];
  if (host.endsWith('.')) host = host.slice(0, -1);
  if (!host || !port) return '';
  return `${host}:${port}`;
}

/** The FM bearer's own id, and the label under which the SPI lists it. */
export function fmBearerId(piHex: string, ecc: string, freqHz: number): string {
  const pi = piHex.toLowerCase();
  const gcc = pi[0] + ecc.toLowerCase().slice(-2);
  const f = String(Math.round(freqHz / 10000)).padStart(5, '0');
  return `fm:${gcc}.${pi}.${f}`;
}

function fqdnFor(piHex: string, ecc: string, freqHz: number): string {
  if (piHex.length !== 4 || ecc.length < 2 || !(freqHz > 0)) return '';
  const pi = piHex.toLowerCase();
  const gcc = pi[0] + ecc.toLowerCase().slice(-2);
  const f = String(Math.round(freqHz / 10000)).padStart(5, '0');
  return `${f}.${pi}.${gcc}.fm.radiodns.org`;
}

/**
 * Pull the logo for OUR bearer out of an SPI document.
 *
 * ★★ The document lists every service the broadcaster runs, so the first <multimedia> in the file
 *    is very often a SIBLING STATION'S logo — Radio 1's artwork on Radio 4. The bearer id says
 *    which <service> block is ours, so the search is scoped to it.
 * ★ Prefer the widest offered: these come in several sizes and a bar scales down far more
 *   gracefully than it scales up.
 */
export function logoFromSpi(xml: string, bearerId: string): string {
  const b = xml.indexOf(bearerId);
  if (b < 0) return '';
  const open = xml.lastIndexOf('<service', b);
  if (open < 0) return '';
  let close = xml.indexOf('</service>', b);
  if (close < 0) close = xml.length;
  const block = xml.slice(open, close);

  let best = '', bestW = -1;
  const tag = /<multimedia\b[^>]*>/g;
  let m: RegExpExecArray | null;
  while ((m = tag.exec(block))) {
    const url = m[0].match(/url="([^"]+)"/)?.[1] ?? '';
    if (!url.startsWith('http')) continue;
    const w = Number(m[0].match(/width="(\d+)"/)?.[1] ?? 0);
    if (w > bestW) { bestW = w; best = url; }
  }
  // ★★★ https OR IT MAY NOT LOAD AT ALL. SPI documents still carry plain http urls (Global's
  //     Classic FM entry does); an https page blocks those as mixed content before the request is
  //     made, and iOS ATS refuses them outright. Both schemes answered 200 everywhere tested.
  return best.startsWith('http://') ? 'https://' + best.slice(7) : best;
}

/**
 * The broadcaster's own logo for an FM station, or '' if it publishes none.
 *
 * @param piHex four hex digits, e.g. "C203"
 * @param ecc   the Extended Country Code from RDS group 1A, e.g. "E1"
 * @param freqHz the tuned carrier
 */
export async function radioDnsLogo(piHex: string, ecc: string, freqHz: number): Promise<string> {
  const fqdn = fqdnFor(piHex, ecc, freqHz);
  if (!fqdn) return '';

  const hit = cache.get(fqdn);
  if (hit && Date.now() - hit.at < (hit.url ? HIT_TTL : MISS_TTL)) return hit.url;
  const busy = inflight.get(fqdn);
  if (busy) return busy;                    // ★ one lookup per station, however many callers ask

  const p = (async (): Promise<string> => {
    let url = '';
    try {
      const anchor = await dns(fqdn, 'CNAME');
      if (anchor) {
        // ★ _radiospi is the modern name; plenty of broadcasters still publish only _radioepg.
        const hostPort = (await srv(`_radiospi._tcp.${anchor}`))
                      || (await srv(`_radioepg._tcp.${anchor}`));
        if (hostPort) {
          const tls = hostPort.endsWith(':443');
          const base = tls ? `https://${hostPort.slice(0, -4)}` : `http://${hostPort}`;
          const r = await fetch(`${base}/radiodns/spi/3.1/SI.xml`);
          if (r.ok) url = logoFromSpi(await r.text(), fmBearerId(piHex, ecc, freqHz));
        }
      }
    } catch {
      url = '';                             // offline, or the broadcaster's host is down
    }
    cache.set(fqdn, { url, at: Date.now() });
    inflight.delete(fqdn);
    return url;
  })();
  inflight.set(fqdn, p);
  return p;
}
