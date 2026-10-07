// test-server-decoder-alone.mjs — a RUNNING DECODER keeps the radio's audio chain running (2026-10-07).
//
// ★★★ THE FAULT (Pi 500 Airspy, shared dial, 2026-10-07 11:06:19-27). Stuart's audio and waterfall sockets
//     dropped for 8 s; the server logged "no listeners — idling the audio chain" and the WEFAX decoder,
//     whose socket never closed, received nothing — the chart slid sideways. The idle gate and the park
//     both counted spectrum and audio SOCKETS, but onAudio() feeds the decoders too. And a decoder left
//     running on purpose (app closed, chart still drawing on the server) was starved for good.
// ★ Pinned here on a shared dial with --idle-grace 0, the harshest setting (a park the instant the last
//   socket goes): with only the decoder socket open, WEFAX lines keep arriving at their full rate for 12 s,
//   the server never idles the chain or parks the radio, presence is unchanged (the listener count stays
//   0 — only the idle decision moved), and once the decoder is stopped the radio idles again.
// ★ SILENT: nothing plays audio.
//   usage: VIBESERVER_BIN=… node scripts/test-server-decoder-alone.mjs   (~35 s)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** A minimal WebSocket client (the probes' own), recording binary frames with their arrival time. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) st.bin.push({ t: Date.now(), b: Buffer.from(p) });
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));   // zero mask: payload unchanged
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const isLine = (f) => f.b.length === 9 + 1809 && f.b[0] === 0x01 && f.b.readUInt32BE(5) === 1809;
const linesIn = (st, t0, t1) => st.bin.filter((f) => isLine(f) && f.t >= t0 && f.t < t1);
const texts = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`));
const status = async (port) => (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { cache: 'no-store' })).json();

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-decalone-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const log = fs.openSync(logPath, 'w');
// ★ A SHARED DIAL (several listeners, no locked centre) — the Pi 500 Airspy's shape — parking at once.
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '7100000',
                        '--mode', 'usb', '--users', '3', '--idle-grace', '0'],
                  { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
const serverLog = () => fs.readFileSync(logPath, 'utf8');
const WEFAX = { type: 'audio_extension_attach', extension_name: 'wefax', lpm: 120, image_width: 1809, carrier: 1900, deviation: 400, use_phasing: false };

try {
  console.log('── shared dial, --idle-grace 0: a listener starts WEFAX, then closes audio and waterfall ──');
  const sid = 'aaaaaaaa';
  const spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  await sleep(300);
  const aud = ws(port, `/ws/audio?user_session_id=${sid}`);
  const dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(500);
  dx.send(WEFAX);
  await sleep(4000);
  ok(texts(dx, 'audio_extension_attached').length === 1, 'WEFAX attached on the decoder socket');
  const tA = Date.now();
  await sleep(4000);
  const withListener = linesIn(dx, tA, Date.now()).length;
  ok(withListener >= 6, `with the listener present: ${withListener} lines in 4 s (120 lpm = 8)`);

  // ★ What the app does when minimised and muted — and what a network blip does: both sockets go.
  const logBefore = serverLog().length;
  spec.close(); aud.close();
  await sleep(1500);                                   // the server notices the close
  const t0 = Date.now();
  await sleep(12_000);
  const alone = linesIn(dx, t0, Date.now());
  const gaps = alone.slice(1).map((f, i) => f.t - alone[i].t);
  const maxGap = gaps.length ? Math.max(...gaps) : 12_000;
  ok(alone.length >= 20, `★ decoder ALONE: ${alone.length} lines in 12 s (24 expected) — before the fix, none`);
  ok(maxGap < 2000, `...with no hole in them (longest gap ${maxGap} ms)`);
  const during = serverLog().slice(logBefore);
  ok(!/idling the audio chain/.test(during), 'the server did NOT idle the audio chain under the decoder');
  ok(!/idle park in|dongle capture paused|radio RELEASED/i.test(during), '...nor arm a park or release the radio');
  const st = await status(port);
  ok(st.listeners === 0, `presence is unchanged: the radio still reports ${st.listeners} listeners (a decoder is not a listener there)`);

  // ★ And it lets go when the decoder stops: the idle decision was moved, not removed.
  const logMid = serverLog().length;
  dx.send({ type: 'audio_extension_detach' });
  await sleep(4000);
  const after = serverLog().slice(logMid);
  ok(/no listeners and no decoder running — idling the audio chain/.test(after),
     'decoder stopped with nobody listening: the audio chain idles again');
  const tD = Date.now();
  await sleep(2000);
  ok(linesIn(dx, tD, Date.now()).length === 0, '...and no WEFAX lines are sent once it is stopped');
  dx.close();
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
}
console.log(`   ${pass} passed, ${fail} failed   (server log: ${logPath})`);
process.exit(fail ? 1 : 0);
