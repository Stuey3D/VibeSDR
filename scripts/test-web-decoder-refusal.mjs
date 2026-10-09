/**
 * test-web-decoder-refusal.mjs — the web client's decoder socket says WHO it is, and takes "no"
 * for an answer (B6, 2026-09-30).
 *
 * ★★ Two faults, both measured on the Pi 500: the web client opened /ws/dxcluster with NO session
 *    id (the app always sent one), so a per-VFO server could not tell whose audio to decode; and a
 *    decoder the server refuses must be SAID to the listener and FORGOTTEN — the socket reconnects
 *    every 3 s and re-asserts whatever it believes is attached, which for a refused decoder is a
 *    refusal loop.
 * ★ Runs the REAL decoders.ts (bundled by esbuild) against a fake WebSocket. Silent: no audio, no
 *   network.
 */
import * as esbuild from 'esbuild';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let checks = 0, failures = 0;
const ok = (c, w) => { checks++; if (!c) failures++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${w}`); };

// ── a browser just big enough for decoders.ts ──
const sockets = [];
class FakeWS {
  static OPEN = 1;
  constructor(url) { this.url = url; this.readyState = 0; this.sent = []; sockets.push(this); }
  send(s) { this.sent.push(JSON.parse(s)); }
  close() { this.readyState = 3; this.onclose?.(); }
  open() { this.readyState = 1; this.onopen?.(); }
  deliver(o) { this.onmessage?.({ data: JSON.stringify(o) }); }
}
globalThis.WebSocket = FakeWS;
globalThis.window = { location: { protocol: 'http:', host: '127.0.0.1:48111', hostname: '127.0.0.1', port: '48111' } };
globalThis.location = globalThis.window.location;
globalThis.localStorage = { getItem: () => null, setItem: () => {}, removeItem: () => {} };
const timers = [];
globalThis.setTimeout = (f) => { timers.push(f); return timers.length; };

const built = await esbuild.build({
  entryPoints: [path.join(root, 'web/client/src/decoders.ts')], bundle: true, write: false,
  format: 'esm', platform: 'browser', target: 'es2020', logLevel: 'silent',
});
const mod = await import('data:text/javascript;base64,' + Buffer.from(built.outputFiles[0].text).toString('base64'));

const seen = [];
const dc = new mod.DecoderClient('127.0.0.1:48111', { query: '' }, {
  onRefused: (m, what) => seen.push({ m, what }),
}, 'sess-1234abcd');
dc.connect();
// ★★ 2026-10-10: the socket opens only while something needs it (test_decoder_socket_idle.ts) — not at connect().
ok(sockets.length === 0, 'connected with no decoder: no socket yet');
dc.attach('wefax', { lpm: 120 });
const ws = sockets[0];
ok(!!ws && /\/ws\/dxcluster\?user_session_id=sess-1234abcd/.test(ws.url), `a decoder opens the socket, carrying the session id (${ws?.url})`);
ws.open();
dc.setSpots(true);
ok(ws.sent.some((m) => m.type === 'audio_extension_attach' && m.extension_name === 'wefax' && m.lpm === 120), 'WEFAX asked for, with its LPM');
ok(ws.sent.some((m) => m.type === 'subscribe_digital_spots'), 'spots asked for');
const words = 'All 4 decoder slots on this server are in use — try again shortly.';
ws.deliver({ type: 'decoder_refused', what: 'decoder', ext: 'wefax', reason: 'limit', max: 4, message: words });
ws.deliver({ type: 'decoder_refused', what: 'spots', ext: 'ft8', reason: 'limit', max: 4, message: words });
ok(seen.length === 2 && seen[0].m === words && seen[0].what === 'decoder' && seen[1].what === 'spots',
   'the listener is shown the server\'s own words, for the decoder and for spots');
ok(dc.attached === null && dc.spotsEnabled === false, 'what was refused is forgotten');
// The socket drops: with everything refused nothing is wanted, so there is no reconnect at all — no refusal loop.
ws.close();
for (const f of timers.splice(0)) f();
ok(sockets.length === 1, 'nothing left running: the drop is NOT reconnected (no refusal loop, no idle socket)');
// The listener asks for something else: a new socket, asking only for THAT.
dc.attach('rtty', { shift: 170 });
const ws2 = sockets[1];
ws2?.open();
ok(!!ws2 && ws2.sent.some((m) => m.type === 'audio_extension_attach' && m.extension_name === 'fsk')
   && !ws2.sent.some((m) => m.extension_name === 'wefax' || m.type === 'subscribe_digital_spots'),
   'a new decoder opens a new socket and asks only for that — the refused ones are not re-asked');
ok(/user_session_id=sess-1234abcd/.test(ws2?.url ?? ''), '...and it still says who it is');
console.log(`\n   ${checks - failures} of ${checks} passed`);
process.exit(failures ? 1 : 0);
