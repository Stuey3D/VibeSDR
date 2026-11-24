// test-server-only-mode.mjs — V12 THE ONLY MODE, any whole-radio mode (Stuart, 2026-11-18: "the same thing we did for DAB
// earlier where it is the only allowed mode, we need to do for all modes as the Almere server has an ADSB only radio and
// I've accidentally closed the ADSB a couple of times meaning that it has to train the gain again"). The REAL server, a
// fake radio:
//   1. ADS-B the one mode the owner's ticks leave (every demodulator and every other whole-radio mode blocked), 2.4 MS/s:
//      a new listener LANDS in ADS-B without asking (logged); hwinfo says onlyMode "adsb" and dabOnly false; their "ADS-B
//      off" is refused with the reason (and logged) and ADS-B stays on; the signed-in admin is told onlyMode "" and MAY
//      leave;
//   2. nothing blocked: onlyMode "", nobody is landed in ADS-B;
//   3. ISM the one mode left: lands, onlyMode "ism", "ISM off" refused + logged;
//   4. AIS the one mode left: lands, onlyMode "ais", "AIS off" refused + logged;
//   5. DAB the one mode left (the bench clip; NOT RUN without it): onlyMode "dab" beside dabOnly true — DAB is one case.
// Needs VIBESERVER_BIN. Silent: no audio is played (the sockets are read, never sounded).
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
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms = 15000) => { for (let i = 0; i < ms / 100; i++) { if (f()) return true; await sleep(100); } return !!f(); };
const ADMIN = 'onlymode-Test-Pass-9';
const DEMODS = ['wfm', 'nfm', 'am', 'sam', 'sau', 'sal', 'ecssu', 'ecssl', 'amst', 'usb', 'lsb', 'cwu', 'cwl', 'drm', 'freedv', 'm17', 'dvsig'];
const WHOLE = ['dab', 'adsb', 'ism', 'ndb', 'ais', 'avdata', 'sondes', 'vormap'];   // ★ V12 + the VOR / ILS map (its own only-mode run: test-server-vormap)
/** The owner's ticks leaving only `mode`. */
const onlyLeaves = (mode) => [...DEMODS, ...WHOLE.filter((m) => m !== mode)].join(',');

function listener(port, tag) {
  const sess = `${tag}${Math.random().toString(16).slice(2, 8)}`;
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sess}&bins=256&proto=1`);
  const au = new WebSocket(`ws://127.0.0.1:${port}/ws/audio?user_session_id=${sess}`);
  ws.binaryType = 'arraybuffer'; au.binaryType = 'arraybuffer';
  const c = { msgs: [], hw: {} };
  c.opened = Promise.all([new Promise((r) => { ws.onopen = r; }), new Promise((r) => { au.onopen = r; })]);
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    try { const m = JSON.parse(e.data); c.msgs.push(m); if (m.type === 'hwinfo') c.hw = m; } catch {}
  };
  c.beat = setInterval(() => { try { ws.send(JSON.stringify({ type: 'ping' })); } catch {} }, 8000);
  c.send = (o) => ws.send(JSON.stringify(o));
  c.has = (type, from = 0) => c.msgs.slice(from).some((m) => m.type === type);
  c.close = () => { clearInterval(c.beat); try { ws.close(); } catch {} try { au.close(); } catch {} };
  return c;
}
async function server(blocked, rate, rtlArgs = () => []) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-onlymode-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, fullMode: true, sharing: 'public', adminPass: ADMIN,
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, users: 3, mode: 'single', rate,
               freq: 96100000, demodMode: 'wfm', landingFreq: 96100000, blockedModes: blocked }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(rate), ...rtlArgs(dir)], { stdio: 'ignore' });
  await sleep(400);
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

/** Lands, says so, refuses a listener's "off" with the reason and logs it. Returns the listener (still open). */
async function landsAndStays(S, mode, tag, words) {
  const L = listener(S.port, tag); await L.opened;
  ok(await until(() => L.has(mode), 20000), `★ the listener LANDS in ${words} without asking (a "${mode}" report)`);
  ok(new RegExp(`\\[ONLY\\] ${mode} only: a new listener lands in it`).test(S.log()), '…and the server says so');
  ok(await until(() => L.hw.onlyMode === mode, 5000), `★ hwinfo: onlyMode "${mode}" (${JSON.stringify(L.hw.onlyMode)})`);
  ok(L.hw.dabOnly === false, '…and dabOnly false, as an older client reads it');
  const n0 = L.msgs.length;
  L.send({ type: mode, on: 0 });
  ok(await until(() => L.msgs.slice(n0).some((m) => m.type === `${mode}_error` && new RegExp(`this receiver is .* only`).test(m.why || '')), 5000),
     `★ "${words} off" refused with the reason (${L.msgs.slice(n0).find((m) => m.type === `${mode}_error`)?.why ?? 'none'})`);
  await sleep(1500);
  ok(/only mode: a listener's ".* off" refused/.test(S.log()) && !L.has(`${mode}_off`, n0), `…logged, and ${words} stays on`);
  return L;
}

try {
  console.log('── 1. ADS-B the only mode left (2.4 MS/s) ──');
  { const S = await server(onlyLeaves('adsb'), 2400000);
    const L = await landsAndStays(S, 'adsb', 'lst', 'ADS-B');
    ok(/ADS-B only — its owner has set it up for ADS-B listeners/.test(L.msgs.find((m) => m.type === 'adsb_error')?.why || ''), 'the words: "this receiver is ADS-B only — its owner has set it up for ADS-B listeners"');
    const A = listener(S.port, 'adm'); await A.opened;
    ok(await unlock(S.port, A), 'the admin unlocks');
    A.send({ type: 'hello' }); await sleep(1500);
    ok(await until(() => A.hw.onlyMode === '', 5000), `★ hwinfo for the signed-in admin: onlyMode "" (${JSON.stringify(A.hw.onlyMode)})`);
    const n1 = A.msgs.length;
    A.send({ type: 'adsb', on: 0 });
    ok(await until(() => A.has('adsb_off', n1), 8000), '★ the signed-in admin MAY leave ADS-B');
    ok(!A.msgs.slice(n1).some((m) => m.type === 'adsb_error'), '…no refusal for the admin');
    ok(/\[ADS-B\] only mode: the signed-in admin leaves/.test(S.log()), '…and the server says who left');
    L.close(); A.close(); await S.stop(); }

  console.log('── 2. nothing blocked ──');
  { const S = await server('', 2400000);
    const L = listener(S.port, 'plain'); await L.opened; await sleep(6000);
    ok(!L.has('adsb') && !/\[ONLY\]/.test(S.log()), 'nobody is landed in ADS-B');
    ok(L.hw.onlyMode === '', `hwinfo: onlyMode "" (${JSON.stringify(L.hw.onlyMode)})`);
    L.close(); await S.stop(); }

  console.log('── 3. ISM the only mode left ──');
  { const S = await server(onlyLeaves('ism'), 2048000);
    const L = await landsAndStays(S, 'ism', 'ism', 'ISM');
    L.close(); await S.stop(); }

  console.log('── 4. AIS the only mode left ──');
  { const S = await server(onlyLeaves('ais'), 2048000);
    const L = await landsAndStays(S, 'ais', 'ais', 'AIS');
    L.close(); await S.stop(); }

  console.log('── 5. DAB the only mode left: one case of the rule ──');
  if (!fs.existsSync(CLIP)) console.log('   (part 5 not run — no bench clip)');
  else {
    const rawClip = (dir) => { const f = path.join(dir, 'clip.u8'); fs.writeFileSync(f, fs.readFileSync(CLIP).subarray(28)); return ['--dab', f]; };
    const S = await server(onlyLeaves('dab'), 2048000, rawClip);
    const L = listener(S.port, 'dab'); await L.opened;
    ok(await until(() => L.hw.dabOnly === true && L.hw.onlyMode === 'dab', 20000), `★ hwinfo: dabOnly true AND onlyMode "dab" (${JSON.stringify(L.hw.onlyMode)})`);
    ok(/DAB only: a new listener lands in DAB/.test(S.log()) && !/\[ONLY\]/.test(S.log()), 'DAB lands by its own path, exactly as before (no [ONLY] landing)');
    L.close(); await S.stop();
  }
  console.log('── 6. the settled ADS-B gain survives a leave (nothing blocked) ──');
  { const S = await server('', 2400000);
    const L = listener(S.port, 'gain'); await L.opened;
    L.send({ type: 'adsb', on: 1 });
    ok(await until(() => L.has('adsb'), 15000), 'in ADS-B');
    ok(await until(() => /\[ADS-B\] gain search \(dB=msg/.test(S.log()) && /\[ADS-B\] gain [0-9.]+ dB: /.test(S.log()), 180000),
       'the entry ramp runs and the rule settles (first entry)');
    await sleep(3000);
    const n0 = L.msgs.length;
    L.send({ type: 'adsb', on: 0 });
    ok(await until(() => L.has('adsb_off', n0), 8000), 'left');
    const kept = S.log().match(/\[ADS-B\] gain ([0-9.]+) dB remembered for the next entry on this radio/);
    ok(!!kept, `★ the settled gain is remembered on the leave (${kept?.[1] ?? 'none'} dB)`);
    if (!kept) console.log(S.log().split('\n').filter((l) => /ADS-B\] gain/.test(l)).join('\n'));
    await sleep(1500);
    L.send({ type: 'adsb', on: 1 });
    ok(await until(() => /\[ADS-B\] gain: back at /.test(S.log()), 15000), '★ re-entered: back at it, no ramp');
    const back = S.log().match(/\[ADS-B\] gain: back at ([0-9.]+) dB/);
    ok(!!kept && !!back && kept[1] === back[1], `…the same gain (${back?.[1] ?? '?'} dB)`);
    ok((S.log().match(/\[ADS-B\] gain: ramp from/g) || []).length === 1, '…and the ramp ran once only');
    L.close(); await S.stop(); }
} catch (e) { fail++; console.log('   FAIL ' + (e?.stack || e)); }
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
