// test-radio-ports-local.mjs — ★★★ A RADIO BEHIND THE FRONT DOOR ANSWERS THIS MACHINE ONLY ON ITS OWN PORT (2026-11-12).
//
// The security finding: on a multi-radio box the per-radio processes' own ports (base+1+n — 48001-48004 on the Lenovo
// and the Pi 500) listened on 0.0.0.0, while the setup page told owners they were "this machine only" and Stuart
// believed every radio was reached only through the front door. They now bind 127.0.0.1 (setBindLocalOnly), and only
// the BIND changed — trust, caps and locks still follow serveOnLan. This runs the real binary, laid out the way systemd
// runs a Full-mode box (a door, one process per radio started by serial — VIBESERVER_NO_RADIO_SPAWN=1 because a door
// cannot spawn fake radios), and proves:
//   · each radio's own port accepts on 127.0.0.1 and REFUSES on this machine's LAN address (skipped with no LAN);
//   · the door still answers on the LAN, and still serves /r/<id> in full: /vibeserver.json, the waterfall socket
//     (config + SPEC frames) and the audio socket (PCM/Opus frames) — from loopback AND from the LAN address;
//   · ★ the handed-over socket is the CLIENT's: through the door from the LAN address the radio says local:false — the
//     loopback-is-owner trust is not widened by the radio now living on 127.0.0.1;
//   · ★ the FALLBACK: a radio whose hand-off socket cannot be made (a runtime dir too long for sun_path) keeps its port
//     on the network and says so, rather than becoming reachable by nobody.
// SILENT: fake rtl_tcp, no browser, no audio device — audio frames are counted, never played.
// usage: VIBESERVER_BIN=… node scripts/test-radio-ports-local.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || path.join(root, 'vibeserver/build/vibeserver');
if (!fs.existsSync(BIN)) { console.log(`   (not run — no server binary at ${BIN}; set VIBESERVER_BIN)`); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what, extra = '') => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}${c || !extra ? '' : '  — ' + extra}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return !!(await f()); };
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const portFree = (q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, () => s.close(() => r(true))); });
/** true = accepted, false = refused / unreachable. */
const tcp = (host, port, ms = 1500) => new Promise((r) => {
  const c = net.connect({ host, port });
  const t = setTimeout(() => { c.destroy(); r(false); }, ms);
  c.on('connect', () => { clearTimeout(t); c.end(); r(true); });
  c.on('error', () => { clearTimeout(t); r(false); });
});
/** FNV-1a, as vsconfig::radioId — the /r/<id> the door routes. */
const radioId = (serial) => {
  let h = 1469598103934665603n; const M = (1n << 64n) - 1n;
  for (const c of Buffer.from(serial)) { h ^= BigInt(c); h = (h * 1099511628211n) & M; }
  return ((h ^ (h >> 32n)) & 0xffffffffn).toString(16).padStart(8, '0');
};

// ── this machine's LAN address (the check is skipped without one) ──
let LAN = '';
try { LAN = execFileSync('ipconfig', ['getifaddr', 'en0'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim(); } catch {}
if (!LAN) LAN = Object.values(os.networkInterfaces()).flat().find((a) => a && a.family === 'IPv4' && !a.internal)?.address || '';

// ── the box: a door on P, radios A and B behind it on P+1 and P+2 (portForRadio), C on P+3 for the fallback ──
// ★ Short paths: the hand-off socket must fit in sun_path (104 bytes on macOS).
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vrp-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
let P = 0;
for (let i = 0; i < 30 && !P; i++) {
  const p = await freePort();
  if ((await Promise.all([p, p + 1, p + 2, p + 3].map(portFree))).every(Boolean)) P = p;
}
const ADMIN = 'rpl-admin-pass';
const SA = 'rpl-a', SB = 'rpl-b', SC = 'rpl-c';
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, fullMode: true, name: 'Radio ports test', adminPass: ADMIN, port: P,
  radios: [SA, SB, SC].map((serial, i) => ({ serial, driver: 'rtlsdr', label: `RTL ${'ABC'[i]}`, enabled: true, configured: true,
                                            users: 3, rate: 2048000 })),
}));
// ★★ A RUNTIME DIR THE HAND-OFF SOCKET CANNOT LIVE IN: usable (writable) but too deep for sun_path — the fallback case.
let longRun = path.join(dir, 'x');
while (longRun.length < 120) longRun = path.join(longRun, 'deeper-still');
fs.mkdirSync(longRun, { recursive: true });

const procs = [];
// ★★ EVERY PORT DISTINCT. freePort() can hand back P+1 (a radio's own port) — and a fake rtl_tcp sitting on 127.0.0.1
//    there makes the radio fail to bind ("already in use") while the port still answers. Measured on the first run.
const fakePort = async () => { for (;;) { const p = await freePort(); if (p < P || p > P + 3) return p; } };
const fake = async () => { const p = await fakePort(); procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(p), '--rate', '2048000', '--wfm', '96.1'], { stdio: 'ignore' })); return p; };
const fA = await fake(), fB = await fake(), fC = await fake();
await sleep(400);
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: path.join(dir, 'config.json'),
              VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir };
const logs = {};
const run = (name, args, e = env) => {
  logs[name] = path.join(dir, `${name}.log`);
  const fd = fs.openSync(logs[name], 'w');
  procs.push(spawn(BIN, args, { env: e, stdio: ['ignore', fd, fd] }));
};
const L = (name) => { try { return fs.readFileSync(logs[name], 'utf8'); } catch { return ''; } };
run('door', []);
const radio = (serial, fp) => ['--radio-serial', serial, '--tcp', `127.0.0.1:${fp}`, '--freq', '96100000', '--mode', 'wfm'];
run('a', radio(SA, fA));
run('b', radio(SB, fB));
run('c', radio(SC, fC), { ...env, VIBESERVER_RUNTIME_DIR: longRun });
const stop = () => { for (const p of procs) { try { p.kill('SIGTERM'); } catch {} } };
process.on('exit', stop);
process.on('SIGINT', () => { stop(); process.exit(1); });

const getJson = async (base, p) => { try { const r = await fetch(`${base}${p}`, { signal: AbortSignal.timeout(4000) }); return r.ok ? await r.json() : null; } catch { return null; } };
/** A spectrum + audio session through `base` (ws://host:port/r/<id>): config, SPEC frames, audio frames. */
const session = (wsBase) => new Promise((res) => {
  const sid = crypto.randomBytes(8).toString('hex');
  const st = { config: null, spec: 0, audio: 0, err: '' };
  const sp = new WebSocket(`${wsBase}/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
  sp.binaryType = 'arraybuffer';
  let au = null;
  sp.onopen = () => {
    au = new WebSocket(`${wsBase}/ws/audio?user_session_id=${sid}&proto=1`);
    au.binaryType = 'arraybuffer';
    au.onmessage = (m) => { if (typeof m.data !== 'string') st.audio++; };
    au.onerror = () => { st.err += 'audio socket error; '; };
  };
  sp.onmessage = (m) => {
    if (typeof m.data === 'string') { try { const j = JSON.parse(m.data); if (j.type === 'config') st.config = j; } catch {} }
    else if (new TextDecoder().decode(new Uint8Array(m.data, 0, 4)) === 'SPEC') st.spec++;
  };
  sp.onerror = () => { st.err += 'spectrum socket error; '; };
  const t0 = Date.now();
  const tick = setInterval(() => {
    if ((st.config && st.spec >= 3 && st.audio >= 3) || Date.now() - t0 > 10000) {
      clearInterval(tick);
      try { sp.close(); } catch {} try { au && au.close(); } catch {}
      res(st);
    }
  }, 100);
});

try {
  // ── up ──
  ok(await until(() => tcp('127.0.0.1', P), 15000), `the front door is up on ${P}`);
  ok(await until(() => tcp('127.0.0.1', P + 1), 15000) && await until(() => tcp('127.0.0.1', P + 2), 15000),
     `radios A and B answer on 127.0.0.1:${P + 1} and :${P + 2}`);
  ok(/front door on port/.test(L('door')), 'the door is a front door');
  ok(await until(() => /answers this machine only \(127\.0\.0\.1\)/.test(L('a')) && /answers this machine only/.test(L('b')), 5000),
     'both radios say their own port answers this machine only');
  ok(await until(() => /hand-off socket: .*rpl-a\.sock/.test(L('a')) || /ready to accept handed-over connections/.test(L('a')), 8000),
     'radio A took its hand-off socket after the probe (the door can reach it)');

  // ── ★★★ the finding: the radio ports refuse on the LAN ──
  if (LAN) {
    ok(await tcp(LAN, P), `the front door still answers on the LAN (${LAN}:${P})`);
    ok(!(await tcp(LAN, P + 1)), `★★★ radio A's own port REFUSES on the LAN (${LAN}:${P + 1})`);
    ok(!(await tcp(LAN, P + 2)), `★★★ radio B's own port REFUSES on the LAN (${LAN}:${P + 2})`);
  } else {
    console.log('   --   no LAN address on this machine (ipconfig getifaddr en0 is empty): the LAN refusal checks are skipped');
  }

  // ── ★ the fallback: no usable hand-off socket → the port stays on the network, and the log says why ──
  ok(await until(() => tcp('127.0.0.1', P + 3), 15000), `radio C (no usable hand-off socket) is up on ${P + 3}`);
  ok(/front door cannot hand connections to this radio \(socket path too long/.test(L('c')) && /stays open on the network/.test(L('c')),
     '★ radio C says the door cannot reach it and its port stays open on the network', L('c').split('\n').filter((l) => /hand|port/.test(l)).slice(0, 4).join(' | '));
  ok(!/answers this machine only/.test(L('c')), '...and does NOT go local-only');
  if (LAN) ok(await tcp(LAN, P + 3), `★ radio C is still reachable on the LAN (${LAN}:${P + 3}) — reachable the old way beats reachable no way`);

  // ── the door serves /r/<id> in full, from loopback and from the LAN ──
  const bases = [['127.0.0.1', 'loopback'], ...(LAN ? [[LAN, 'the LAN address']] : [])];
  for (const [host, what] of bases) {
    for (const [serial, name] of [[SA, 'A'], [SB, 'B']]) {
      const base = `http://${host}:${P}/r/${radioId(serial)}`;
      const j = await getJson(base, '/vibeserver.json');
      ok(!!j, `${what}: /r/<${name}>/vibeserver.json answers through the door`);
      if (!j) continue;
      // ★★ THE HANDED-OVER SOCKET IS THE CLIENT'S. From the LAN the radio must see the LAN address (local:false);
      //    from loopback, loopback (local:true). Were the door proxying, both would read local:true.
      ok(j.local === (host === '127.0.0.1'), `★ ${what}: radio ${name} sees the CLIENT's address (local=${j.local}) — loopback trust is not widened`);
      const s = await session(`ws://${host}:${P}/r/${radioId(serial)}`);
      ok(!!s.config, `${what}: radio ${name}'s waterfall socket sends its config through the door`, s.err);
      ok(s.spec >= 3, `${what}: radio ${name}'s waterfall frames arrive (${s.spec} SPEC)`, s.err);
      ok(s.audio >= 3, `${what}: radio ${name}'s audio frames arrive (${s.audio}, counted, never played)`, s.err);
    }
  }
  // ★ The two radios are two radios, not one answering twice.
  const ja = await getJson(`http://127.0.0.1:${P}/r/${radioId(SA)}`, '/vibeserver.json');
  const jb = await getJson(`http://127.0.0.1:${P}/r/${radioId(SB)}`, '/vibeserver.json');
  ok(ja && jb && JSON.stringify(ja) !== JSON.stringify(jb), 'A and B answer as different radios');

  // ── the radios list still names the ports (the console and the setup page read them) ──
  const list = await getJson(`http://127.0.0.1:${P}`, '/vibeserver/radios');
  const ports = (list?.radios || []).map((r) => r.port).sort((x, y) => x - y);
  ok(JSON.stringify(ports) === JSON.stringify([P + 1, P + 2, P + 3]), `/vibeserver/radios still lists the radio ports (${ports.join(',')})`);
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
  await sleep(300);
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs: ${Object.values(logs).join(' ')})`);
else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
