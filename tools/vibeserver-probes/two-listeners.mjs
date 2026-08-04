// Regression probe for BUG-vibeserver-broadcast-blocks.
//
// Listener A reads normally. Listener B connects, then STOPS READING ENTIRELY (no drain) — the
// exact "slow listener" that froze the live demo. The test asserts A keeps receiving.
//
//   node two-listeners.mjs <port>
//
// ★ Before the fix this must FAIL (A starves once B's window fills). A regression test that
//   passes on the broken code proves nothing — that trap cost a whole cycle on 2026-08-03.
import net from 'node:net';
import crypto from 'node:crypto';

const PORT = Number(process.argv[2] || 48000);
const RUN_MS = Number(process.env.RUN_MS || 20000);
const STOP_READING_AT_MS = 3000;

function wsConnect(name, path, onFrame, { drain = true } = {}) {
  const key = crypto.randomBytes(16).toString('base64');
  const sock = net.connect(PORT, process.env.HOST || '127.0.0.1');
  let handshook = false, buf = Buffer.alloc(0), paused = false;
  sock.on('connect', () => {
    sock.write(
      `GET ${path} HTTP/1.1\r\nHost: ${process.env.HOST || '127.0.0.1'}:${PORT}\r\nUpgrade: websocket\r\n` +
      `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
  });
  sock.on('data', (d) => {
    if (paused) return;
    buf = Buffer.concat([buf, d]);
    if (!handshook) {
      const i = buf.indexOf('\r\n\r\n');
      if (i < 0) return;
      handshook = true;
      buf = buf.subarray(i + 4);
    }
    // Minimal unmasked-frame parser: server->client frames are never masked.
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f;
      let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const payload = buf.subarray(off, off + len);
      buf = buf.subarray(off + len);
      if (op === 0x9) {                                   // ping -> pong, or we get dropped at 20s
        const p = Buffer.alloc(2 + payload.length + 4);
        p[0] = 0x8a; p[1] = 0x80 | payload.length;
        p.writeUInt32BE(0, 2); payload.copy(p, 6);
        sock.write(p);
        continue;
      }
      onFrame(op, payload);
    }
  });
  sock.on('error', (e) => console.log(`  ${name}: socket error ${e.code}`));
  sock.on('close', () => console.log(`  ${name}: closed at ${((Date.now()-t0)/1000).toFixed(1)}s`));
  return {
    sock,
    stopReading() { paused = true; sock.pause(); },   // sock.pause() stops draining -> window fills
  };
}

const t0 = Date.now();
const count = { A: 0, B: 0 };
const lastAt = { A: 0, B: 0 };

console.log(`probing ${process.env.HOST || '127.0.0.1'}:${PORT} — A reads, B stops reading at ${STOP_READING_AT_MS}ms\n`);

const A = wsConnect('A', '/ws/user-spectrum?user_session_id=probeA&bins=4096',
  (op) => { if (op === 0x2) { count.A++; lastAt.A = Date.now(); } });

setTimeout(() => {
  const B = wsConnect('B', '/ws/user-spectrum?user_session_id=probeB&bins=4096',
    (op) => { if (op === 0x2) { count.B++; lastAt.B = Date.now(); } });
  setTimeout(() => {
    console.log(`  B: stops reading now (A had ${count.A} frames)\n`);
    B.stopReading();
  }, STOP_READING_AT_MS);
}, 1000);

let mark = count.A;
const tick = setInterval(() => {
  const t = ((Date.now() - t0) / 1000).toFixed(0).padStart(2);
  console.log(`  t=${t}s   A=${String(count.A).padStart(4)} frames (+${count.A - mark})   B=${count.B}`);
  mark = count.A;
}, 2000);

setTimeout(() => {
  clearInterval(tick);
  const quietMs = Date.now() - lastAt.A;
  const aFramesAfterStall = count.A - mark;
  console.log('\n─────────────────────────────────────────────');
  console.log(`A received ${count.A} frames total; last one ${quietMs} ms ago`);
  const ok = count.A > 50 && quietMs < 3000;
  console.log(ok
    ? 'PASS — A kept streaming while B was stalled.'
    : 'FAIL — A starved while B was stalled (this is the bug).');
  process.exit(ok ? 0 : 1);
}, RUN_MS);
