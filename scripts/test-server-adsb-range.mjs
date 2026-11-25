// test-server-adsb-range.mjs — V12 beta94 THE ADS-B RANGE OUTLINE on a REAL vibeserver (vibe_adsb_range.h; the unit half is
// vibeserver/test-adsb-range.cpp, the map's is test-web-adsb-range.mjs). A fake rtl_tcp replays `test-adsb --write-clip`
// (three aircraft near IO92, ADS-B identification + positions) at 2.4 MS/s; the server is at IO92nh. Pinned:
//   1. ADS-B on → GET /adsb/range fills: three bearings, the furthest catch, the record, the 30-day default window;
//   2. the owner's admin API (GET /vibeserver/admin/adsbrange) refuses without the admin proof (401) and answers with it
//      ("available", the same outline); POST ?window=7 sets the window, ?window=14 is refused (400);
//   3. it survives a restart (the file next to the radio's other state);
//   4. RESET (POST ?reset=1, with the proof) clears it, and GET /adsb/range then serves it EMPTY — and it stays empty
//      while ADS-B is off; without the proof the reset is refused and nothing is cleared;
//   5. the phone's own server screen's door (POST /vibeserver/adsb-range over loopback) takes the window too.
// ★ SILENT: no audio socket is opened.
//   usage: VIBESERVER_BIN=…/vibeserver node scripts/test-server-adsb-range.mjs   (test-adsb is compiled here for the clip)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-adsbrange-srv-'));
const kids = [];
const cleanup = () => { for (const k of kids) { try { k.kill('SIGKILL'); } catch {} } try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} };
process.on('exit', cleanup);
const watchdog = setTimeout(() => { console.log('   FAIL timed out'); process.exit(1); }, 150000);

// ── the clip: test-adsb --write-clip (compiled here, one file) ──
const tadsb = path.join(dir, 'test-adsb'), clip = path.join(dir, 'adsb.u8');
execFileSync('c++', ['-std=c++17', '-O2', '-I', path.join(root, 'vibeserver'), '-I', path.join(root, 'android/app/src/main/cpp'),
  '-o', tadsb, path.join(root, 'vibeserver/test-adsb.cpp')], { stdio: 'inherit' });
execFileSync(tadsb, ['--write-clip', clip, '6'], { stdio: 'ignore' });

const ADMIN = 'adsb-range-admin';
fs.mkdirSync(path.join(dir, 'data'));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Range outline test', locator: 'IO92nh', adminPass: ADMIN,
  radios: [{ serial: 'rangetest', label: 'Range test radio', enabled: true, configured: true, users: 2 }] }));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--adsb', clip], { stdio: 'ignore' });
kids.push(rtl);
await sleep(400);
let srv = null;
async function startServer() {
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '2', '--idle-grace', '0'],
              { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'a'), fs.openSync(path.join(dir, 'server.log'), 'a')] });
  kids.push(srv);
  for (let i = 0; i < 80; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); if (r.ok) return; } catch {} await sleep(250); }
  throw new Error('the server did not come up');
}
async function stopServer() {
  const p = srv; srv = null;
  const gone = new Promise((r) => p.once('exit', r));
  p.kill('SIGTERM');
  await Promise.race([gone, sleep(8000)]);
}
function control() {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  s.on('error', () => {});
  s.on('data', () => {});
  const ready = new Promise((r) => s.on('connect', () => {
    s.write(`GET /ws/user-spectrum?user_session_id=rangetest${Date.now()}&bins=256&proto=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
    setTimeout(r, 1200);
  }));
  const send = (o) => { const b = Buffer.from(JSON.stringify(o)); const h = Buffer.alloc(6); h[0] = 0x81; h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); s.write(Buffer.concat([h, b])); };
  return { ready, send, close: () => { try { s.destroy(); } catch {} } };
}
const range = async () => (await fetch(`http://127.0.0.1:${port}/adsb/range`)).json();
const adminQ = async () => { const n = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${n}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(n).digest('hex')}`; };
const heard = (j) => (Array.isArray(j.km) ? j.km.filter((v) => v > 0).length : -1);

try {
  await startServer();
  let j = await range();
  ok(j.ok === 1 && heard(j) === 0 && j.far === null && j.window === 30, `before ADS-B: an empty outline, the 30-day window (${heard(j)} bearings)`);
  const c = control();
  await c.ready;
  c.send({ type: 'adsb', on: 1 });
  for (let i = 0; i < 60 && heard(j) < 3; i++) { await sleep(500); j = await range(); }
  ok(heard(j) === 3, `ADS-B on: the three aircraft fill three bearings (${heard(j)})`);
  ok(j.far && j.far.km > 5 && j.far.km < 60 && j.record && j.record.km === j.far.km, `…the furthest catch and the record (${JSON.stringify(j.far)})`);
  ok(Math.abs(j.lat - 52.3229) < 0.05 && Math.abs(j.lon - -0.8750) < 0.05, `…centred on the server's locator (${j.lat}, ${j.lon})`);
  // ── the owner's admin API ──
  const noAuth = await fetch(`http://127.0.0.1:${port}/vibeserver/admin/adsbrange`);
  ok(noAuth.status === 401, `the admin API refuses without the admin proof (${noAuth.status})`);
  const a = await (await fetch(`http://127.0.0.1:${port}/vibeserver/admin/adsbrange?${await adminQ()}`)).json();
  ok(a.available === true && heard(a) === 3 && a.record && a.record.km === j.record.km, 'with it: available, the same outline');
  const w7 = await (await fetch(`http://127.0.0.1:${port}/vibeserver/admin/adsbrange?${await adminQ()}&window=7`, { method: 'POST', body: '{}' })).json();
  ok(w7.window === 7 && (await range()).window === 7, 'POST ?window=7: the outline is drawn over 7 days');
  const w14 = await fetch(`http://127.0.0.1:${port}/vibeserver/admin/adsbrange?${await adminQ()}&window=14`, { method: 'POST', body: '{}' });
  ok(w14.status === 400 && (await range()).window === 7, `?window=14 is refused (${w14.status}), the window unchanged`);
  // ── survives a restart ──
  c.send({ type: 'adsb', on: 0 });
  await sleep(1500);
  c.close();
  await stopServer();
  const files = fs.readdirSync(path.join(dir, 'data')).filter((f) => /^adsb-range.*\.bin$/.test(f));
  ok(files.length === 1 && fs.statSync(path.join(dir, 'data', files[0])).size < 80000, `saved beside the radio's state: ${files.join(', ')} (${files.length ? fs.statSync(path.join(dir, 'data', files[0])).size : 0} bytes)`);
  await startServer();
  j = await range();
  ok(heard(j) === 3 && j.window === 7 && j.record, 'after a restart: the outline, its window and the record are back');
  // ── reset ──
  const badReset = await fetch(`http://127.0.0.1:${port}/vibeserver/admin/adsbrange?reset=1`, { method: 'POST', body: '{}' });
  ok(badReset.status === 401 && heard(await range()) === 3, `RESET without the admin proof is refused (${badReset.status}) and clears nothing`);
  const r = await (await fetch(`http://127.0.0.1:${port}/vibeserver/admin/adsbrange?${await adminQ()}&reset=1`, { method: 'POST', body: '{}' })).json();
  ok(heard(r) === 0 && r.far === null && r.record === null && r.window === 7, 'RESET with it: cleared (the record too), the window kept');
  await sleep(2500);
  j = await range();
  ok(heard(j) === 0 && j.far === null && j.record === null && j.allKm.every((v) => v === 0), 'GET /adsb/range then serves it empty');
  // ── the phone's own server screen's door (loopback = the host is the operator) ──
  const lw = await fetch(`http://127.0.0.1:${port}/vibeserver/adsb-range?window=90`, { method: 'POST' });
  ok(lw.ok && (await lw.json()).window === 90 && (await range()).window === 90, 'POST /vibeserver/adsb-range?window=90 over loopback (the app\'s server screen)');
  await stopServer();
  const log = fs.readFileSync(path.join(dir, 'server.log'), 'utf8');
  ok(/range outline reset by/.test(log), 'the reset is in the log');
} catch (e) { fail++; console.log('   FAIL', e && e.stack || e); }
clearTimeout(watchdog);
console.log(`test-server-adsb-range: ${pass} passed, ${fail} failed`);
cleanup();
process.exit(fail ? 1 : 0);
