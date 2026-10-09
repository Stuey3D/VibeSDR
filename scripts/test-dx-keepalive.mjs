// ★★ AN IDLE DECODER SOCKET IS PINGED (2026-10-10). Stuart's Safari console filled with /ws/dxcluster "network connection
// was lost" behind the Cloudflare tunnel: with no decoder attached the socket carried nothing, the tunnel dropped it after
// ~100 s idle, and the page reconnected it for ever. The server now pings it after 25 s of quiet.
//   1. a /ws/dxcluster socket that never sends gets a PING within 30 s;
//   2. answering it (pong) keeps the socket open.
// ★ SILENT: no audio. Loopback only. usage: VIBESERVER_BIN=… node scripts/test-dx-keepalive.mjs   (~35 s)
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dxka-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'DX Keepalive Test', locator: 'IO92nh', adminPass: 'dx-keepalive-pass',
  radios: [{ serial: 'dxka-test', label: 'Test radio', enabled: true, configured: true, users: 3 }],
}));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '3'],
                  { env, stdio: ['ignore', log, log] });
try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  let hs = false, buf = Buffer.alloc(0), pings = 0, closed = false, firstPingAt = 0;
  const t0 = Date.now();
  s.on('connect', () => s.write(`GET /ws/dxcluster?user_session_id=dxka0001 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {}); s.on('close', () => { closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let l = buf[1] & 0x7f, off = 2;
      if (l === 126) { if (buf.length < 4) break; l = buf.readUInt16BE(2); off = 4; }
      else if (l === 127) { if (buf.length < 10) break; l = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + l) break;
      const p = buf.subarray(off, off + l); buf = buf.subarray(off + l);
      if (op === 0x9) {
        pings++; if (!firstPingAt) firstPingAt = Date.now();
        const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q);   // pong
      }
    }
  });
  for (let i = 0; i < 64 && !firstPingAt && !closed; i++) await sleep(500);
  ok(hs, 'the decoder socket opened');
  ok(firstPingAt > 0 && firstPingAt - t0 <= 30_000, `an idle decoder socket is pinged within 30 s (${firstPingAt ? Math.round((firstPingAt - t0) / 1000) + ' s' : 'never'})`);
  await sleep(1500);
  ok(!closed, 'answering the ping keeps it open');
  s.destroy();
} finally {
  try { srv.kill('SIGTERM'); } catch {}
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`test-dx-keepalive: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
