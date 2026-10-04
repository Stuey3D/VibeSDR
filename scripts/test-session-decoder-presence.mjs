// test-session-decoder-presence.mjs — an OPEN DECODER keeps its listener's turn (2026-10-04).
//
// ★★★ THE FAULT (Stuart, Mac app on the Pi 500 Airspy). WEFAX ran 1 h 45 m with the app minimised (waterfall
//     closed) and muted (audio closed). Only the decoder socket was open, and the session limit could not see
//     it: the turn lapsed after the 15-minute break and on return the countdown showed a FRESH 30 minutes —
//     "I was expecting to be in the soft limit still". Now a running decoder keeps the turn, under the same
//     rules as listening. Pinned here on a HARD limit, where the difference is visible in a minute: the
//     decoder-only listener is ended at the end of their ORIGINAL turn, and the cooldown refuses the decoder.
// ★ The listener must not be loopback (exempt), so it connects through the LAN address.
//   usage: VIBESERVER_BIN=… node scripts/test-session-decoder-presence.mjs   (~80 s)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
let lan = process.env.LAN_IP || '';
if (!lan) {
  for (const list of Object.values(os.networkInterfaces())) {
    for (const a of list || []) if (a.family === 'IPv4' && !a.internal && !lan) lan = a.address;
  }
}
if (!lan) { console.log('   not run — no non-loopback IPv4 address to connect from'); process.exit(3); }

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** A minimal WebSocket client, recording every text frame (binary frames are counted, not kept). */
function ws(host, port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, host);
  const st = { txt: [], bin: 0, open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = buf.subarray(0, 12).toString().includes(' 101'); buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) st.bin++;
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.sendText = (t) => { const p = Buffer.from(t); const m = crypto.randomBytes(4); const h = p.length < 126 ? Buffer.from([0x81, 0x80 | p.length]) : Buffer.from([0x81, 0xfe, p.length >> 8, p.length & 255]); const q = Buffer.from(p.map((b, i) => b ^ m[i % 4])); s.write(Buffer.concat([h, m, q])); };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const msgs = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => JSON.parse(t));
async function listen(host, port, sid) {
  const L = { spec: ws(host, port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`) };
  await sleep(300);
  L.aud = ws(host, port, `/ws/audio?user_session_id=${sid}&codec=opus`);
  L.all = () => [...L.spec.txt, ...L.aud.txt];
  L.got = (type) => [...msgs(L.spec, type), ...msgs(L.aud, type)];
  L.close = () => { L.spec.close(); L.aud.close(); };
  return L;
}
/** Wait up to `ms` for `pred`, polling. */
async function until(pred, ms) { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (pred()) return true; await sleep(250); } return pred(); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dxturn-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
// ★ A SHARED DIAL (several listeners, no locked centre) with a HARD one-minute limit — the Pi 500 Airspy's shape.
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '7100000',
                        '--mode', 'usb', '--users', '3', '--session-limit', '1', '--idle-grace', '0'],
                  { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, lan); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
const send = (st, obj) => st.sendText(JSON.stringify(obj));

try {
  console.log(`── shared dial, hard 1-minute limit, listener A from ${lan} ──`);
  const t0 = Date.now();
  const A = await listen(lan, port, 'dddddddd');
  await until(() => msgs(A.spec, 'hwinfo').length > 0, 5000);
  const D = ws(lan, port, '/ws/dxcluster?user_session_id=dddddddd');
  await until(() => D.open, 3000);
  await sleep(300);
  send(D, { type: 'audio_extension_attach', extension_name: 'wefax' });
  ok(await until(() => msgs(D, 'audio_extension_attached').length > 0, 5000), 'A starts WEFAX on the decoder socket');
  await sleep(5000);
  // ★ What the app does when minimised and muted: waterfall and audio close, the decoder stays.
  A.close();
  await sleep(20_000);
  ok(!D.closed && msgs(D, 'session_expired').length === 0, 'decoder-only, inside the turn: not ended');
  const ended = await until(() => msgs(D, 'session_expired').length > 0 || D.closed, 60_000);
  const at = Math.round((Date.now() - t0) / 1000);
  ok(ended && msgs(D, 'session_expired').length > 0,
     `★ decoder-only A is still ON THE TURN and the hard limit ends it (${at}s after A arrived) — before, an open decoder was invisible and ran for ever`);
  ok(at >= 58 && at <= 75, `...at the end of A's ORIGINAL turn, not a fresh one (${at}s)`);
  await until(() => D.closed, 5000);
  ok(D.closed, '...and the decoder socket is closed');
  // ★ And it cannot simply reconnect: the cooldown holds the decoders too.
  const D2 = ws(lan, port, '/ws/dxcluster?user_session_id=dddddddd');
  await until(() => D2.closed, 3000);
  ok(!D2.open, 'reconnecting the decoder inside the cooldown is refused');
  D2.close();
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
}
console.log(`   ${pass} passed, ${fail} failed   (server log: ${path.join(dir, 'server.log')})`);
process.exit(fail ? 1 : 0);
