// test-server-fctlm-iq.mjs — ★★★ beta94 FUNCUBE TELEMETRY ON IQ through the REAL server (Stuart 11-25: "or again use iq").
//   The fake rtl_tcp replays VIBE_FCTLM_CLIP (vibeserver/test-fctlm --write-clip: two FUNcube frames at 145.935 700 MHz,
//   0.7 kHz above AO-73's listed telemetry). The pick attaches fctlm with "iq":true; the host cuts the IQ slice and the
//   decoder reads it — frames arrive, its offset is the signal's from the listing, and the listener's passband is the pick's.
//   ★★★ beta94 AO-73's own telemetry now opens its transponder's finder (vibe_sat_engine.h linkXpdr — the finder decodes it
//   too: scripts/test-server-xpdr.mjs), so here AO-73's transponder is taken out of the fixture: a bird with telemetry and
//   NO transponder within reach keeps the telemetry-only pick, on IQ.
// Silent: no audio socket is opened.   VIBESERVER_BIN=… VIBE_FCTLM_CLIP=… node scripts/test-server-fctlm-iq.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
const RATE = 2400000, PASS = 'sat-test-admin';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
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
  st.close = () => { clearInterval(keep); try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

/** Maidenhead (6 characters) for a point. */
function locator(lat, lon) {
  const L = 'ABCDEFGHIJKLMNOPQRSTUVWX';
  let x = lon + 180, y = lat + 90;
  const a = L[Math.floor(x / 20)], b = L[Math.floor(y / 10)];
  x %= 20; y %= 10;
  const c = Math.floor(x / 2), d = Math.floor(y);
  x -= c * 2; y -= d;
  return `${a}${b}${c}${d}${L[Math.floor(x * 12)].toLowerCase()}${L[Math.floor(y * 24)].toLowerCase()}`;
}

/** vibesdr.net's sats.json fixture (built by the mirror's own code from CelesTrak's 2026-11-19 data, plus TEST-101095:
 *  ERMINAZ-2V's real elements under a test-only downlink) with every EPOCH moved to an hour ago. */
function freshSats() {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-6digit-2026-11-19.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  const ao = d.satellites.find((s) => s.norad === 39444);   // ★★★ beta94 telemetry only (see the header)
  if (ao) ao.transmitters = ao.transmitters.filter((t) => t.consumer === 'fc-bpsk');
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  return JSON.stringify(d);
}
async function adminGet(port, p) {
  try {
    const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`, { signal: AbortSignal.timeout(3000) }).then((r) => r.json())).nonce;
    const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
    return await fetch(`http://127.0.0.1:${port}${p}?${q}`, { signal: AbortSignal.timeout(3000) }).then((r) => r.json());
  } catch { return null; }
}
/** ★ The sync's shared state: just checked — so the server under test asks vibesdr.net NOTHING (no network in tests). */
function syncState(extra = '') {
  const t = Math.floor(Date.now() / 1000);
  return `checked ${t}\nattempt ${t}\nseed 0\n${extra}`;
}

async function server(tag, { loc = 'IO92ng', radio = {}, orbits = true, freq = 437800000, mode = 'nfm', state = '', env: extraEnv = {}, args = [], rtlArgs = [] } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run', 'data/sat']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  if (orbits) fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
  fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), state || syncState());
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Satellite test', locator: loc, adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, ...radio }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE), ...rtlArgs], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(freq), '--mode', mode,
                          '--users', '4', '--admin-pass', PASS, ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, status, L, pid: srv.pid, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} } };
}
const sock = async (S, id) => { const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`); await until(() => sp.open, 3000); await sleep(800); return sp; };


const CLIP = process.env.VIBE_FCTLM_CLIP || '';
if (!CLIP || !fs.existsSync(CLIP)) { console.log('   not run — set VIBESERVER_BIN and VIBE_FCTLM_CLIP (test-fctlm --write-clip c.u8 2400000 145935000 145935700)'); process.exit(3); }
const picked = (sp) => jsons(sp, 'sat').filter((m) => m.state === 'picked');
const lastPicked = (sp) => picked(sp).slice(-1)[0] || null;

try {
  const rtlArgs = ['--hf', CLIP, '--hf-centre', '145935000'];
  console.log('── AO-73 telemetry on IQ (the clip: FUNcube BPSK at 145.935 700, 0.7 kHz above the listing) ──');
  const S = await server('fciq', { freq: 145932000, mode: 'usb', rtlArgs });
  try {
    const sp = await sock(S, 'fciq00001');
    sp.send({ type: 'sat_list' });
    await until(() => !!lastOf(sp, 'sat_list'), 4000);
    sp.send({ type: 'sat', op: 'pick', norad: 39444, tx: 0, doppler: false });   // ★★★ beta94 the telemetry is AO-73's only downlink here
    ok(await until(() => lastPicked(sp)?.norad === 39444, 4000), 'AO-73 telemetry picked, Doppler off');
    const p = lastPicked(sp);
    ok(p?.decoder === 'fctlm' && p?.params?.iq === true, `the pick's decoder reads IQ ${JSON.stringify(p?.params)}`);
    ok(/decoder attached: fctlm \(FUNcube BPSK 1200\) on IQ/.test(S.L()), 'the log: fctlm on IQ');
    const dx = ws(S.port, '/ws/dxcluster?user_session_id=fciq00001');
    await until(() => dx.open, 3000); await sleep(300);
    const n0 = (S.L().match(/decoder attached: fctlm/g) || []).length;
    dx.send({ type: 'audio_extension_attach', extension_name: 'fctlm', ...p.params });
    ok(await until(() => dx.txt.some((t) => t.includes('"audio_extension_attached"')), 3000), 'the decoder socket attached');
    await sleep(300);
    ok((S.L().match(/decoder attached: fctlm/g) || []).length === n0, '★ it JOINED the pick\'s decoder (not restarted)');
    const tlm = () => jsons(dx, 'sat_tlm').map((m) => m.data).filter(Boolean);
    ok(await until(() => tlm().length >= 2, 40000), `★★★ frames through the real server's IQ path: ${tlm().map((d) => `${d.frame || d.name || ''} seq ${d.seq}`).join(', ')}`);
    ok(tlm().every((d) => d.seq === 777), `every frame the clip's (seq ${tlm().map((d) => d.seq).join(' ')})`);
    const st = jsons(dx, 'fctlm_status').map((m) => m.data).filter(Boolean).slice(-1)[0];
    ok(st && Math.abs(st.afcHz - 700) < 60, `the decoder's offset = the signal's from the listing: ${st?.afcHz} Hz (700)`);
    ok(st && st.good >= 2 && st.failed === 0, `status: ${st?.good} good, ${st?.failed} failed`);
    // the listener's audio passband is the pick's (the box on the waterfall), not the decoder's
    ok(lastOf(sp, 'config')?.bandwidthLow === 100 && lastOf(sp, 'config')?.bandwidthHigh === 6500, `the listener's passband 100–6500 (${lastOf(sp, 'config')?.bandwidthLow}…${lastOf(sp, 'config')?.bandwidthHigh})`);
    dx.close(); sp.close();
  } finally { S.stop(); }
} catch (e) { console.log('   FAIL threw:', e?.stack || e); fail++; }
console.log(`\n${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
