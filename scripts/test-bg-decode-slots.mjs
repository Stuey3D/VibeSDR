// test-bg-decode-slots.mjs — LISTENERS FIRST, BACKGROUND IN ITS OWN ORDER, across the radios of one box (Stuart, 2026-10-22:
// "the decoder slots are for real listeners as highest priority so a background decode will have to drop off until its
// free again"). Two radio processes, as systemd runs them, one slot directory; three decoder slots on the box.
//   · radio A's background profile takes all three slots;
//   · a listener on radio B starts a decoder: it is NOT refused, it starts at once, and exactly A's LOWEST VFO pauses;
//   · the listener's decoder stops: that VFO resumes.
// SILENT: fake rtl_tcp, no audio device. usage: VIBESERVER_BIN=… node scripts/test-bg-decode-slots.mjs   (~40 s)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [] };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  st.has = (k) => st.txt.some((t) => t.includes(k));
  return st;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bgsl-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const BG = { on: true, waitMin: 5, profiles: [{ centre: 500000, from: '00:00', to: '00:00', vfos: [
  { hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }, { hz: 518500, mode: 'lsb', dec: 'navtex' }] }] };
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Slots test', adminPass: 'x', decoderMax: 3,
  radios: [
    { serial: 'bgsl-a', driver: 'sdrplay', label: 'A', enabled: true, configured: true, users: 4, rate: 2048000, bgDecode: JSON.stringify(BG) },
    { serial: 'bgsl-b', driver: 'rtlsdr', label: 'B', enabled: true, configured: true, users: 4, rate: 2048000 },
  ],
}));
const rtlA = await freePort(), rtlB = await freePort(), portA = await freePort(), portB = await freePort();
const rtls = [rtlA, rtlB].map((p) => spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(p), '--rate', '2048000'], { stdio: 'ignore' }));
await sleep(300);
const env = { ...process.env, VIBESERVER_BG_WAIT_SEC: '2', VIBESERVER_CONFIG: path.join(dir, 'config.json'),
              VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logA = path.join(dir, 'a.log'), logB = path.join(dir, 'b.log');
const start = (serial, rtl, port, log) => spawn(BIN, ['--radio-serial', serial, '--tcp', `127.0.0.1:${rtl}`, '--port', String(port), '--freq', '7074000', '--mode', 'usb'],
                                                { env, stdio: ['ignore', fs.openSync(log, 'w'), fs.openSync(log, 'a')] });
const A = start('bgsl-a', rtlA, portA, logA), B = start('bgsl-b', rtlB, portB, logB);
const stop = () => { for (const p of [A, B, ...rtls]) { try { p.kill('SIGTERM'); } catch {} } };
process.on('exit', stop);
const LA = () => fs.readFileSync(logA, 'utf8'), LB = () => fs.readFileSync(logB, 'utf8');
const until = async (f, ms = 10000) => { for (let t = 0; t < ms; t += 100) { if (f()) return true; await sleep(100); } return f(); };

try {
  ok(await until(() => /profile 1, RF centre 500\.0 kHz, 3 VFO\(s\), 3 decoder\(s\) running/.test(LA()), 20000),
     'radio A\'s background profile takes all 3 of the box\'s decoder slots');
  console.log('── a listener on radio B starts a decoder ──');
  const spec = ws(portB, '/ws/user-spectrum?user_session_id=bgsl0001&bins=256');
  await sleep(1200);
  const dx = ws(portB, '/ws/dxcluster?user_session_id=bgsl0001');
  await sleep(600);
  const t0 = Date.now();
  dx.send({ type: 'audio_extension_attach', extension_name: 'navtex' });
  ok(await until(() => dx.has('audio_extension_attached'), 5000), `★ the listener's NAVTEX starts (${Date.now() - t0} ms) — not refused`);
  ok(Date.now() - t0 < 2000, '...without a user-visible wait');
  ok(!dx.has('decoder_refused'), '...and no refusal was sent');
  ok(/VFO 3 \(518\.5 kHz NAVTEX\) paused — a listener needed a decoder slot/.test(LA()), '★ on radio A exactly the LOWEST VFO (3) paused');
  ok(!/VFO [12] \(.*\) paused/.test(LA()), '...VFOs 1 and 2 kept running');
  console.log('── the listener leaves ──');
  // ★ d93ad592d: sockets that just drop are a HELD session (a suspended phone) — its decoder runs on and keeps its slot
  //   for up to 3 min. Leaving on purpose (`leave`, the app's back-to-the-list) is never held: the slot frees at once.
  spec.send({ type: 'leave' }); await sleep(200);
  dx.close(); spec.close();
  ok(await until(() => /VFO 3 \(518\.5 kHz NAVTEX\) resumed/.test(LA()), 8000), '★ the slot frees: VFO 3 resumes');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
  await sleep(300);
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs: ${logA} ${logB})`);
process.exit(fail ? 1 : 0);
