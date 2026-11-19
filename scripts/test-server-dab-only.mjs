// test-server-dab-only.mjs — V12 DAB ONLY (Stuart, 2026-11-17: "if DAB is the only allowed mode then the exit button goes,
// also the exit button needs to be removed on existing servers that have been locked to DAB"). The REAL server, a fake radio:
//   1. a WHOLE-RADIO DAB server whose owner blocks every other mode: a new listener LANDS in DAB without asking; hwinfo says
//      dabOnly; their "DAB off" is refused with the reason (and logged); the signed-in admin is told dabOnly false and MAY
//      leave;
//   2. the same server with nothing blocked: nobody is landed in DAB;
//   3. a WIDE LOCKED RANGE (8 MS/s replay of 10C/10D/11A) blocked the same way: a listener lands on a multiplex of their own
//      without asking, the waterfall keeps flowing behind the DAB view, and leaving is refused.
// Needs VIBESERVER_BIN, the bench clip (part 1–2) and the wide capture (part 3; NOT RUN without it). Silent.
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
const CLIP = process.env.VIBE_DAB_CLIP || path.join(root, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
const CAP = process.env.DAB_WIDE_CAPTURE || path.join(os.homedir(), 'VibeSDR-test-audio/dab-wide/2026-11-16/dab-10C-10D-11A-215140kHz-8Msps.cu8');
if (!fs.existsSync(CLIP)) { console.log('   not run — no bench clip (vibeserver/bench-clip/make-clip.sh)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms = 15000) => { for (let i = 0; i < ms / 100; i++) { if (f()) return true; await sleep(100); } return !!f(); };
const ADMIN = 'dabonly-Test-Pass-9';
const OTHERS = 'wfm,nfm,am,sam,sau,sal,ecssu,ecssl,amst,usb,lsb,cwu,cwl,drm,freedv,m17,dvsig,adsb,ism,ndb,ais,avdata,sondes';

function listener(port, tag) {
  const sess = `${tag}${Math.random().toString(16).slice(2, 8)}`;
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sess}&bins=256&proto=1`);
  const au = new WebSocket(`ws://127.0.0.1:${port}/ws/audio?user_session_id=${sess}`);
  ws.binaryType = 'arraybuffer'; au.binaryType = 'arraybuffer';
  const c = { msgs: [], bins: 0, hw: {} };
  c.opened = Promise.all([new Promise((r) => { ws.onopen = r; }), new Promise((r) => { au.onopen = r; })]);
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') { c.bins++; return; }
    try { const m = JSON.parse(e.data); c.msgs.push(m); if (m.type === 'hwinfo') c.hw = m; } catch {}
  };
  c.beat = setInterval(() => { try { ws.send(JSON.stringify({ type: 'ping' })); } catch {} }, 8000);
  c.send = (o) => ws.send(JSON.stringify(o));
  c.dab = () => c.msgs.filter((m) => m.type === 'dab' && m.channel !== undefined).at(-1);
  c.close = () => { clearInterval(c.beat); try { ws.close(); } catch {} try { au.close(); } catch {} };
  return c;
}
async function server(cfg, rtlArgs, rate) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabonly-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify(cfg));
  fs.writeFileSync(path.join(dir, 'benchmark.json'), JSON.stringify({ v: 1, at: 1, rows: [],
    dabStations: { n: 200, perStationPct: 2, frontPct: 7, memKB: 1300, muxFrontPct: 5, ddc8Pct: 8, cores: 8 } }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, ['--max-old-space-size=512', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(rate), ...rtlArgs(dir)], { stdio: 'ignore' });
  await sleep(rate > 3e6 ? 2500 : 400);
  const LOG = path.join(dir, 'server.log'); const fd = fs.openSync(LOG, 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(rate), '--idle-grace', '0'],
    { env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') }, stdio: ['ignore', fd, fd] });
  for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
  await sleep(3000);
  return { port, log: () => fs.readFileSync(LOG, 'utf8'),
    stop: async () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); await sleep(600); fs.rmSync(dir, { recursive: true, force: true }); } };
}
async function unlock(port, c) {
  const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  c.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex') });
  return until(() => c.msgs.some((m) => m.type === 'admin' && m.ok === true), 5000);
}
const rawClip = (dir) => { const f = path.join(dir, 'clip.u8'); fs.writeFileSync(f, fs.readFileSync(CLIP).subarray(28)); return ['--dab', f]; };
const whole = (blocked) => ({ configured: true, fullMode: true, sharing: 'public', adminPass: ADMIN,
  radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, users: 3, mode: 'single', rate: 2048000,
             freq: 96100000, demodMode: 'wfm', blockedModes: blocked }] });

try {
  console.log('── 1. a whole-radio DAB server, every other mode blocked ──');
  { const S = await server(whole(OTHERS), rawClip, 2048000);
    const L = listener(S.port, 'lst'); await L.opened;
    ok(await until(() => (L.dab()?.services?.length ?? 0) > 0, 30000), `★ the listener LANDS in DAB without asking (${L.dab()?.channel ?? 'no report'})`);
    ok(/DAB only: a new listener lands in DAB/.test(S.log()), '…and the server says so');
    ok(await until(() => L.hw.dabOnly === true, 5000), '★ hwinfo: dabOnly true (EXIT DAB goes)');
    const n0 = L.msgs.length;
    L.send({ type: 'dab', on: 0 });
    ok(await until(() => L.msgs.slice(n0).some((m) => m.type === 'dab_error' && /DAB only/.test(m.why || '')), 5000), '★ "DAB off" refused with the reason');
    ok(/DAB only: a listener's "DAB off" refused/.test(S.log()) && !L.msgs.slice(n0).some((m) => m.type === 'dab_off'), '…logged, and DAB stays on');
    const A = listener(S.port, 'adm'); await A.opened;
    ok(await unlock(S.port, A), 'the admin unlocks');
    A.send({ type: 'hello' }); await sleep(1500);
    const n1 = A.msgs.length;
    A.send({ type: 'dab', on: 0 });
    ok(await until(() => A.msgs.slice(n1).some((m) => m.type === 'dab_off'), 8000), '★ the signed-in admin MAY leave DAB');
    ok(!A.msgs.slice(n1).some((m) => m.type === 'dab_error'), '…no refusal for the admin');
    L.close(); A.close(); await S.stop(); }

  console.log('── 2. the same server, nothing blocked ──');
  { const S = await server(whole(''), rawClip, 2048000);
    const L = listener(S.port, 'plain'); await L.opened; await sleep(8000);
    ok(!L.dab() && !/DAB only/.test(S.log()), 'nobody is landed in DAB');
    ok(L.hw.dabOnly !== true, 'hwinfo: dabOnly not true');
    L.close(); await S.stop(); }

  console.log('── 3. a wide locked range (8 MS/s), every other mode blocked ──');
  if (!fs.existsSync(CAP)) console.log('   (part 3 not run — no wide capture)');
  else {
    const PHYS = 215140000, LOCK = PHYS - 15000;
    const cut = (dir) => { const f = path.join(dir, 'wide.cu8'); const fd = fs.openSync(CAP, 'r'); const b = Buffer.alloc(8000000 * 2 * 8); fs.readSync(fd, b, 0, b.length, 8000000 * 2 * 5); fs.closeSync(fd); fs.writeFileSync(f, b); return ['--dab-map', `${PHYS}=${f}`]; };
    const S = await server({ configured: true, fullMode: true, sharing: 'public', adminPass: ADMIN,
      radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, users: 8, mode: 'locked', lockFreq: LOCK, rate: 8000000,
                 freq: LOCK, demodMode: 'wfm', dabOwnStation: 1, blockedModes: OTHERS }] }, cut, 8000000);
    const L = listener(S.port, 'lr'); await L.opened;
    ok(await until(() => ['10C', '10D', '11A'].includes(L.dab()?.channel), 30000), `★ the listener LANDS on a multiplex of their own without asking (${L.dab()?.channel ?? 'no report'})`);
    ok(/\[DAB-LR\] DAB only: a listener lands on/.test(S.log()), '…in the locked range (the radio does not move)');
    ok(await until(() => L.hw.dabOnly === true, 5000), 'hwinfo: dabOnly true');
    const b0 = L.bins; await sleep(5000);
    ok(L.bins - b0 >= 5, `★ the waterfall keeps flowing behind the DAB view (${L.bins - b0} frames in 5 s)`);
    const n0 = L.msgs.length;
    L.send({ type: 'dab', on: 0 });
    ok(await until(() => L.msgs.slice(n0).some((m) => m.type === 'dab_error' && /DAB only/.test(m.why || '')), 5000), '★ leaving is refused with the reason');
    ok(/\[DAB-LR\] DAB only: a listener's "DAB off" refused/.test(S.log()), '…and logged');
    L.close(); await S.stop();
  }
} catch (e) { fail++; console.log('   FAIL ' + (e?.stack || e)); }
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
