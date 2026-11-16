// test-paging-local-only.mjs — V12 PAGING never leaves the device it was heard on, through the REAL binary (2026-11-16).
//
// The desktop VibeServer now COMPILES the paging engine, for the R36S handheld's radio process. This drives that binary as
// a network client would and proves the build alone opens nothing (the rule is vibe_paging_gate.h; the in-process half —
// pages DO reach the handheld's own socket, never the gallery or the log — is vibeserver/test-paging-gate.cpp):
//   1. A SERVER (fake rtl_tcp radio, serving on the LAN): vibeserver.json has no "paging" and no "pagingModes"; a decoder
//      socket from the machine's LAN address asking for paging is REFUSED, and so is one from 127.0.0.1 (a tunnel or a
//      proxy arrives as loopback) — and neither gets a single binary frame.
//   2. THE HANDHELD (`vibeserver --handheld` + a fake radio): the supervisor answers 127.0.0.1 only — a connection to the
//      LAN address is refused at the door; its vibeserver.json offers no paging for a radio that is not plugged into it
//      (the test radio is rtl_tcp); an attach is refused in the handheld's own words, with no frames.
// Silent: no audio socket is opened at all.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-paging-local-only.mjs     (exit 3 = not run)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { startFakeHwd } from './fake-vibe-hwd.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { try { if (await f()) return true; } catch { /* not yet */ } await sleep(200); } try { return !!(await f()); } catch { return false; } };
/** This machine's first non-loopback IPv4 address — a client "on the LAN" without leaving the machine. */
const lanIp = Object.values(os.networkInterfaces()).flat().find((a) => a && a.family === 'IPv4' && !a.internal)?.address || '';

function ws(host, port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, host);
  const st = { bin: [], txt: [], open: false, error: null };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', (e) => { st.error = e.code || String(e); });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = / 101 /.test(buf.subarray(0, j).toString()); buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) st.bin.push(Buffer.from(p));
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
  const keep = setInterval(() => { if (st.open) st.send({ type: 'ping' }); }, 8000);
  s.on('close', () => clearInterval(keep));
  st.close = () => { clearInterval(keep); try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch { /* gone */ } s.end(); };
  return st;
}
const refusal = (st) => st.txt.map((t) => { try { return JSON.parse(t); } catch { return null; } }).find((j) => j && j.type === 'decoder_refused') || null;
/** Ask a decoder socket for paging; true when it was refused AND sent no binary frame in the next 3 s. */
async function askPaging(host, port, sid, words) {
  const spec = ws(host, port, `/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
  await sleep(400);
  const dx = ws(host, port, `/ws/dxcluster?user_session_id=${sid}`);
  if (!(await until(() => dx.open, 5000))) { spec.close(); dx.close(); return { opened: false }; }
  spec.send({ type: 'tune', frequency: 153350000, mode: 'nfm' });
  await sleep(300);
  dx.send({ type: 'audio_extension_attach', extension_name: 'paging', mode: 'auto' });
  await until(() => refusal(dx), 4000);
  await sleep(3000);
  const r = refusal(dx);
  spec.close(); dx.close();
  return { opened: true, refused: !!r, words: r ? r.message || '' : '', frames: dx.bin.length, attached: dx.txt.some((t) => t.includes('audio_extension_attached')), ok: !!r && (!words || (r.message || '').includes(words)) };
}

const procs = [], shortDirs = [];
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-paging-local-'));
const fakeRtl = async () => {
  const p = await freePort();
  procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(p), '--rate', '2048000'], { stdio: 'ignore' }));
  await sleep(400);
  return p;
};
try {
  // ── 1. a server ──
  console.log('── 1. a server (serving on the LAN) ──');
  {
    const d = path.join(dir, 'server'); fs.mkdirSync(path.join(d, 'data'), { recursive: true }); fs.mkdirSync(path.join(d, 'run'));
    fs.writeFileSync(path.join(d, 'config.json'), JSON.stringify({ configured: true, name: 'Paging gate test', locator: 'IO92nh', adminPass: 'paging-gate-admin',
      radios: [{ serial: 'paging-srv', label: 'Test radio', enabled: true, configured: true }] }));
    const rtl = await fakeRtl(), port = await freePort();
    const log = fs.openSync(path.join(d, 'server.log'), 'w');
    procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtl}`, '--port', String(port), '--freq', '153350000', '--mode', 'nfm', '--users', '2', '--admin-pass', 'paging-gate-admin'],
      { env: { ...process.env, VIBESERVER_CONFIG: path.join(d, 'config.json'), VIBESERVER_DATA_DIR: path.join(d, 'data'), VIBESERVER_RUNTIME_DIR: path.join(d, 'run') }, stdio: ['ignore', log, log] }));
    const vs = async () => { try { return await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) })).json(); } catch { return null; } };
    ok(await until(async () => !!(await vs()), 20000), 'the server answers');
    const j = await vs();
    ok(j && !('paging' in j) && !('pagingModes' in j), `vibeserver.json offers no paging (paging: ${JSON.stringify(j && j.paging)})`);
    if (lanIp) {
      const r = await askPaging(lanIp, port, 'pglan001', 'never decoded by a server');
      ok(r.opened, `a decoder socket from the LAN address (${lanIp}) opens`);
      ok(r.refused && !r.attached, `…asking for paging is refused (${JSON.stringify(r.words)})`);
      ok(r.frames === 0, `…and it got no binary frame at all (${r.frames})`);
    } else ok(true, '(no LAN address on this machine — the LAN half is not run; the loopback half below covers the same gate)');
    const r = await askPaging('127.0.0.1', port, 'pglo0001', 'never decoded by a server');
    ok(r.opened && r.refused && !r.attached, `from 127.0.0.1 too (a tunnel or proxy arrives as loopback): refused (${JSON.stringify(r.words)})`);
    ok(r.frames === 0, `…no binary frame (${r.frames})`);
    const text = fs.readFileSync(path.join(d, 'server.log'), 'utf8');
    ok(/decoder paging refused/.test(text) && !/decoder attached: paging/.test(text), 'the server log says refused, never attached');
  }

  // ── 2. the handheld ──
  console.log('── 2. the handheld (vibeserver --handheld) ──');
  {
    // ★ a SHORT directory: the radio's hand-off socket (data/handheld/run/<serial>.sock) must fit sun_path (104 bytes on macOS)
    const d = fs.mkdtempSync('/tmp/vpg-'); shortDirs.push(d);
    const data = path.join(d, 'data'), card = path.join(d, 'card'), radios = path.join(d, 'radios.txt');
    fs.mkdirSync(data, { recursive: true }); fs.mkdirSync(card);
    const hwd = await startFakeHwd(path.join(d, 'hwd.sock'));
    const rtl = await fakeRtl(), port = await freePort();
    fs.writeFileSync(radios, `PAGINGHH rtlsdr 127.0.0.1:${rtl}\n`);
    const log = fs.openSync(path.join(d, 'handheld.log'), 'a');
    procs.push(spawn(BIN, ['--handheld', '--port', String(port)], {
      env: { ...process.env, VIBESERVER_DATA_DIR: data, VIBESERVER_CARD_DIR: card, VIBE_HWD_SOCK: path.join(d, 'hwd.sock'),
             VIBE_HANDHELD_FAKE_RADIOS: radios, VIBE_HANDHELD_RESET_FLAG: path.join(d, 'reset-radio') }, stdio: ['ignore', log, log] }));
    const sess = async () => (await fetch(`http://127.0.0.1:${port}/handheld/session`, { signal: AbortSignal.timeout(5000) })).json();
    ok(await until(async () => (await sess()).state === 'running', 30000), 'the handheld runs the radio');
    if (lanIp) {
      const err = await new Promise((res) => { const s = net.connect(port, lanIp); s.on('connect', () => { s.destroy(); res('connected'); }); s.on('error', (e) => res(e.code)); });
      ok(err !== 'connected', `the handheld's door is closed to the LAN address (${lanIp}: ${err})`);
    }
    const j = await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`)).json();
    ok(j.handheld === true, 'its vibeserver.json is the handheld profile');
    ok(!('paging' in j), 'no paging offered for a radio that is not plugged into it (the test radio is rtl_tcp — somebody else\'s receiver)');
    const r = await askPaging('127.0.0.1', port, 'pghh0001', 'handheld');
    ok(r.opened && r.refused && !r.attached, `an attach is refused in the handheld's words (${JSON.stringify(r.words)})`);
    ok(r.frames === 0, `…no binary frame (${r.frames})`);
    hwd.close?.();
  }
} finally {
  for (const p of procs) { try { p.kill('SIGTERM'); } catch { /* gone */ } }
  await sleep(500);
  for (const d of shortDirs) fs.rmSync(d, { recursive: true, force: true });
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
