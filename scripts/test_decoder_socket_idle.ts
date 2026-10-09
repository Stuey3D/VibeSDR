// ★★★ THE DECODER SOCKET OPENS ONLY WHILE SOMETHING NEEDS IT, AND CLOSES AFTER (Stuart, 2026-10-10: "make the connection
// as efficient as possible, minimal data rate" / "light everywhere — light on CPU on client and server and light on
// data"). Both clients: the web's (web/client/src/decoders.ts) used to open at page load and hold it for ever; the
// app's (src/services/DecoderClient.ts) opened on demand but kept it "warm" for the session — on a shared dial the
// server mirrors a running decoder to every decoder socket, so both were sent data they threw away.
// A fake WebSocket and a manual clock: no network, no real waiting.
type Timer = { at: number; fn: () => void; id: number };
let now = 0, nextId = 1;
const timers: Timer[] = [];
(globalThis as any).setTimeout = (fn: () => void, ms: number) => { const t = { at: now + ms, fn, id: nextId++ }; timers.push(t); return t.id; };
(globalThis as any).clearTimeout = (id: number) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); };
const advance = (ms: number) => {
  const end = now + ms;
  for (;;) {
    timers.sort((a, b) => a.at - b.at);
    const t = timers[0];
    if (!t || t.at > end) break;
    timers.shift(); now = t.at; t.fn();
  }
  now = end;
};
const sockets: FakeWS[] = [];
class FakeWS {
  static CONNECTING = 0; static OPEN = 1; static CLOSING = 2; static CLOSED = 3;
  readyState = 0; sent: string[] = []; url: string; binaryType = '';
  onopen: any = null; onmessage: any = null; onclose: any = null; onerror: any = null;
  constructor(url: string) { this.url = url; sockets.push(this); }
  send(s: string) { this.sent.push(s); }
  close() { this.readyState = 3; }
  open() { this.readyState = 1; this.onopen?.({}); }
}
(globalThis as any).WebSocket = FakeWS;
(globalThis as any).location = { protocol: 'http:', host: '127.0.0.1:8080', href: 'http://127.0.0.1:8080/' };
const live = () => sockets.filter((s) => s.readyState === 0 || s.readyState === 1).length;

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

void (async () => {
const { DecoderClient: WebDec } = await import('../web/client/src/decoders.ts');
const { DecoderClient: AppDec } = await import('../src/services/DecoderClient.ts');

console.log('── web client ──');
{
  sockets.length = 0;
  const d = new WebDec('127.0.0.1:8080', { query: '' } as any, {}, 'sess0001');
  d.connect();
  advance(60_000);
  ok(sockets.length === 0, 'page loaded, no decoder: NO socket opened (was: one, held for ever)');
  d.attach('rtty', { shift: 170 } as any);
  ok(sockets.length === 1, 'a decoder started: the socket opens');
  sockets[0].open();
  ok(sockets[0].sent.some((m) => m.includes('audio_extension_attach') && m.includes('"shift":170')), '…and attaches it WITH its parameters on open');
  d.detach();
  advance(3000);
  d.attach('navtex' as any, {});
  ok(sockets.length === 1 && live() === 1, 'switching decoders within the grace: the same socket, no reconnect');
  d.detach();
  advance(9000);
  ok(live() === 0, 'nothing running for 8 s: the socket is closed');
  const made = sockets.length;
  advance(60_000);
  ok(sockets.length === made && live() === 0, '…and stays closed (no reconnect loop)');
  d.setSpots(true);
  ok(live() === 1, 'spots switched on: it opens again');
  sockets[sockets.length - 1].open();
  ok(sockets[sockets.length - 1].sent.some((m) => m.includes('subscribe_digital_spots')), '…and subscribes on open');
  d.close();
}

console.log('── app client ──');
{
  sockets.length = 0;
  const noop = new Proxy({}, { get: () => () => {} });   // every callback a no-op
  const d = new AppDec('http://127.0.0.1:8080', 'sess0002', noop as any);
  advance(60_000);
  ok(sockets.length === 0, 'connected, no decoder: no socket');
  d.start('rtty' as any);
  ok(sockets.length === 1, 'a decoder started: the socket opens');
  sockets[0].open();
  d.stop();
  advance(3000);
  d.start('navtex' as any);
  advance(9000);
  ok(live() === 1, 'restarted within the grace: kept open while the decoder runs');
  d.stop();
  advance(9000);
  ok(live() === 0, 'stopped for 8 s: closed (was: kept warm for the session)');
  d.subscribeChat();
  ok(live() === 1, 'chat: opens');
  sockets[sockets.length - 1].open();
  advance(60_000);
  ok(live() === 1, '…and chat holds it open (chat is long-lived)');
  d.destroy();
}
console.log(`decoder socket idle: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
})();
