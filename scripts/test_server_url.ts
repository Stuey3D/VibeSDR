// ★★★ GitHub #52 — the scheme and the port are a PAIR. An OpenWebRX+ with native TLS on 8073 was
// rebuilt as http://host:8073 → ws:// → close 1006. See src/utils/serverUrl.ts.
import { parseServerUrl, toWsUrl, toHttpUrl, withScheme, owrxWsUrl as OWRX_WS_URL, owrxHttpBase as OWRX_HTTP_BASE, owrxCloseReason } from '../src/utils/serverUrl.ts';

let bad = 0;
const eq = (what: string, got: unknown, want: unknown) => {
  if (JSON.stringify(got) !== JSON.stringify(want)) { bad++; console.log('FAIL', what, '→', got, 'want', want); }
};

type Want = { url: string; scheme: string; port: number; explicitScheme: boolean; host?: string; path?: string } | null;
const cases: Array<[string, { port?: number; defaultPort?: number } | undefined, Want]> = [
  // OWRX+ native TLS on its default port — the issue itself. Must stay https AND 8073.
  ['https://owrx.example.net:8073', undefined, { url: 'https://owrx.example.net:8073', scheme: 'https', port: 8073, explicitScheme: true }],
  ['https://owrx.example.net:8073/', undefined, { url: 'https://owrx.example.net:8073', scheme: 'https', port: 8073, explicitScheme: true }],
  ['wss://owrx.example.net:8073/ws/', undefined, { url: 'https://owrx.example.net:8073/ws', scheme: 'https', port: 8073, explicitScheme: true }],
  ['http://owrx.example.net:8073', undefined, { url: 'http://owrx.example.net:8073', scheme: 'http', port: 8073, explicitScheme: true }],
  // No scheme typed: http is only a GUESS (explicitScheme false → callers probe https first).
  ['owrx.example.net:8073', undefined, { url: 'http://owrx.example.net:8073', scheme: 'http', port: 8073, explicitScheme: false }],
  // Plain http on a port that "looks like" TLS stays plain.
  ['http://host.example:8443', undefined, { url: 'http://host.example:8443', scheme: 'http', port: 8443, explicitScheme: true }],
  ['https://host.example:443', undefined, { url: 'https://host.example', scheme: 'https', port: 443, explicitScheme: true }],
  ['https://host.example', undefined, { url: 'https://host.example', scheme: 'https', port: 443, explicitScheme: true }],
  // An explicit https with no port is 443 — a type's default port does NOT override it.
  ['https://host.example', { defaultPort: 8073 }, { url: 'https://host.example', scheme: 'https', port: 443, explicitScheme: true }],
  // A bare host takes the type's default port…
  ['host.example', { defaultPort: 8073 }, { url: 'http://host.example:8073', scheme: 'http', port: 8073, explicitScheme: false }],
  // …and a typed port field wins over a missing port, but not over a port in the text.
  ['https://host.example', { port: 8073 }, { url: 'https://host.example:8073', scheme: 'https', port: 8073, explicitScheme: true }],
  ['https://host.example:9000', { port: 8073 }, { url: 'https://host.example:9000', scheme: 'https', port: 9000, explicitScheme: true }],
  // Portless Kiwi proxy → 8073, scheme kept.
  ['http://x.proxy.kiwisdr.com', undefined, { url: 'http://x.proxy.kiwisdr.com:8073', scheme: 'http', port: 8073, explicitScheme: true }],
  ['https://x.proxy.kiwisdr.com', undefined, { url: 'https://x.proxy.kiwisdr.com:8073', scheme: 'https', port: 8073, explicitScheme: true }],
  ['http://x.proxy.kiwisdr.com:8074', undefined, { url: 'http://x.proxy.kiwisdr.com:8074', scheme: 'http', port: 8074, explicitScheme: true }],
  // IPv6.
  ['[::1]:8073', undefined, { url: 'http://[::1]:8073', scheme: 'http', port: 8073, explicitScheme: false, host: '::1' }],
  ['https://[2001:db8::5]:8073/owrx/', undefined, { url: 'https://[2001:db8::5]:8073/owrx', scheme: 'https', port: 8073, explicitScheme: true, host: '2001:db8::5', path: '/owrx' }],
  // Trailing path kept, query dropped.
  ['https://host.example:8073/owrx/', undefined, { url: 'https://host.example:8073/owrx', scheme: 'https', port: 8073, explicitScheme: true, path: '/owrx' }],
  ['https://host.example/OpenWebRX/?f=7', undefined, { url: 'https://host.example/OpenWebRX', scheme: 'https', port: 443, explicitScheme: true, path: '/OpenWebRX' }],
  // Not http receivers / not addresses.
  ['sdr://host:5555', undefined, null],
  ['', undefined, null],
  ['http://host:70000', undefined, null],
];
for (const [raw, opts, want] of cases) {
  const u = parseServerUrl(raw, opts);
  if (!want) { eq(`parse ${raw}`, u, null); continue; }
  if (!u) { bad++; console.log('FAIL parse', raw, '→ null'); continue; }
  const pick: Record<string, unknown> = { url: u.url, scheme: u.scheme, port: u.port, explicitScheme: u.explicitScheme };
  if (want.host != null) pick.host = u.host;
  if (want.path != null) pick.path = u.path;
  eq(`parse ${raw} ${JSON.stringify(opts ?? {})}`, pick, want);
}

// ws(s) follows the scheme, never the port.
eq('ws https:8073', toWsUrl('https://h:8073'), 'wss://h:8073');
eq('ws http:8443', toWsUrl('http://h:8443'), 'ws://h:8443');
eq('ws https:443 path', toWsUrl('https://h/owrx/'), 'wss://h/owrx');
eq('ws keeps wss', toWsUrl('wss://h:8073'), 'wss://h:8073');
eq('ws v6', toWsUrl('https://[::1]:8073'), 'wss://[::1]:8073');
eq('http from wss', toHttpUrl('wss://h:8073/'), 'https://h:8073');
eq('http from ws', toHttpUrl('ws://h:8443'), 'http://h:8443');
eq('http keeps https', toHttpUrl('https://h:8073'), 'https://h:8073');
eq('probe swap', withScheme(parseServerUrl('h:8073')!, 'https').url, 'https://h:8073');

// The OWRX adapter's own two builders (OwrxAdapter.toWsUrl / toHttpBase delegate to these).
eq('owrx ws tls 8073', OWRX_WS_URL('https://h:8073'), 'wss://h:8073/ws/');
eq('owrx ws plain', OWRX_WS_URL('http://h:8073'), 'ws://h:8073/ws/');
eq('owrx ws subfolder', OWRX_WS_URL('https://h/OpenWebRX/'), 'wss://h/OpenWebRX/ws/');
eq('owrx http from wss', OWRX_HTTP_BASE('wss://h:8073/ws/'), 'https://h:8073');

// The close reason: a TLS-only port dialled as ws:// says so; a dropped session does not claim it never connected.
{
  const r = owrxCloseReason('ws://h:8073/ws/', true, 1006, 'Socket is not connected');
  eq('close reason names url', r.includes('ws://h:8073/ws/'), true);
  eq('close reason carries error', r.includes('Socket is not connected (code 1006)'), true);
  eq('close reason https hint', r.includes('https://'), true);
  eq('no hint on wss', owrxCloseReason('wss://h:8073/ws/', true, 1006, '').includes('https://'), false);
  eq('dropped wording', owrxCloseReason('wss://h/ws/', false, 1001, '').startsWith('connection to'), true);
}

console.log(bad ? `${bad} FAIL` : `server URL: all ok`);
if (bad) process.exit(1);
