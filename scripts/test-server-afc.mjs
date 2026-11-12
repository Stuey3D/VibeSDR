#!/usr/bin/env node
// test-server-afc.mjs — AUTOMATIC FREQUENCY CORRECTION ("auto-PPM", vibe_afc.h) END TO END (V12, 2026-11-12).
//
// Real server, the bench multiplex through fake-rtl-tcp --dab --clock-ppm 4: a dongle whose crystal runs 4 ppm fast, so
// DAB 7D lands ~776 Hz low (the B210 tester's "781 Hz (4.02 ppm)", mirrored). No browser, silent.
//   A. AUTO OFF (autoPpm:false): nothing changes — the correction stays 0, DAB keeps reading the full offset, no change
//      is logged, no store is written.
//   B. AN UPGRADE (a config with NO autoPpm key): it is ON, and NOTHING is applied for the first 100 s of clean lock
//      (rule 6 — agreement over time first); then it learns ~4 ppm (+ the clip's own offset), glides it in, checks it,
//      and DAB's measured offset falls to ~0. The log has one line for the change: from → to, the reference, MER, offset.
//      The radio's own ppm is never written (the fake hears no 0x05).
//   C. RESTART (SIGKILL): the learned value is back at once, from the per-radio store, before DAB has even locked; the
//      offset is ~0 from the first report.
//   D. THE OWNER'S MANUAL PPM (2): the saved value was learned on 0, so it is NOT restored; the radio is trimmed by 2
//      (the fake hears 0x05 2, and only that), and auto-PPM learns the remaining ~2 ON TOP of it.
//   E. RESET (the setup page's endpoint): back to the manual 2 exactly, at once; TURN OFF (an admin's socket): off,
//      persisted in the config as autoPpm:false, the offset back to the full residual.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-server-afc.mjs   (exit 3 = not run; ~12 min)
//   VIBE_DAB_CLIP overrides the bench clip (vibeserver/bench-clip/dab-bench-clean.vbu8).
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const CLIP = process.env.VIBE_DAB_CLIP || path.join(ROOT, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.error('no bench clip (vibeserver/bench-clip/make-clip.sh, or VIBE_DAB_CLIP) — not run'); process.exit(3); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0, passes = 0;
const ok = (c, what) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); c ? passes++ : fails++; };
const BLOCKS = ['5A','5B','5C','5D','6A','6B','6C','6D','7A','7B','7C','7D','8A','8B','8C','8D','9A','9B','9C','9D',
  '10A','10N','10B','10C','10D','11A','11N','11B','11C','11D','12A','12N','12B','12C','12D','13A','13B','13C','13D','13E','13F'];
const CLOCK = 4;
const PASS = 'afc-admin';
const SERIAL = 'afc-t1';
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-afc-'));
const DATA = path.join(T, 'data'), RUN = path.join(T, 'run');
for (const d of [DATA, RUN]) fs.mkdirSync(d, { recursive: true });
const raw = path.join(T, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const STORE = path.join(DATA, `afc-${SERIAL}.txt`);
const CFG = path.join(T, 'config.json');

const procs = [];
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGKILL'); } catch {} } });
let srv = null, rtl = null, port = 0, rtlPort = 0, rtlLog = '';
async function startRtl() {
  if (rtl) { rtl.kill('SIGKILL'); await sleep(300); }
  rtlPort = await freePort();
  rtlLog = '';
  rtl = spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw,
    '--clock-ppm', String(CLOCK)], { stdio: ['ignore', 'pipe', 'ignore'] });
  rtl.stdout.on('data', (d) => { rtlLog += d.toString(); });
  procs.push(rtl);
  await sleep(400);
}
function writeCfg(radio) {
  fs.writeFileSync(CFG, JSON.stringify({ configured: true, name: 'AFC test', locator: 'IO92nh', adminPass: PASS,
    radios: [{ serial: SERIAL, label: 'T', enabled: true, configured: true, users: 1, ...radio }] }));
}
let logPath = '';
async function startSrv(tag) {
  port = await freePort();
  logPath = path.join(T, `server-${tag}.log`);
  const log = fs.openSync(logPath, 'a');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'], {
    env: { ...process.env, VIBESERVER_CONFIG: CFG, VIBESERVER_DATA_DIR: DATA, VIBESERVER_RUNTIME_DIR: RUN, HOME: T },
    stdio: ['ignore', log, log] });
  procs.push(srv);
  for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); return; } catch { await sleep(250); } }
  throw new Error('server did not start');
}
async function stopSrv(sig = 'SIGTERM') {
  if (!srv) return;
  const s = srv; srv = null;
  const gone = new Promise((r) => s.once('exit', r));
  s.kill(sig);
  await Promise.race([gone, sleep(10000)]);
  await sleep(300);
}
const serverLog = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
const changeLines = () => serverLog().split('\n').filter((l) => /auto-PPM: [+-]\d+\.\d+ → [+-]\d+\.\d+ ppm from DAB/.test(l));
async function client(tag, admin = false) {
  let q = '';
  if (admin) {
    const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
    q = `&vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
  }
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${tag}${Math.random().toString(16).slice(2, 8)}&bins=256&proto=1${q}`);
  const c = { ws, hw: null, dab: null, dabs: [], open: new Promise((r) => (ws.onopen = r)) };
  ws.onmessage = (ev) => {
    if (typeof ev.data !== 'string') return;
    let m; try { m = JSON.parse(ev.data); } catch { return; }
    if (m.type === 'hwinfo') c.hw = m;
    else if (m.type === 'dab' && m.channel) { c.dab = m; c.dabs.push({ t: Date.now(), locked: m.locked, ppm: m.offsetPpm, hz: m.offsetHz }); }
  };
  c.send = (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); };
  c.hb = setInterval(() => c.send({ type: 'dab_resync' }), 8000);   // ★ proto=1: a silent session is released at 30 s
  ws.addEventListener('close', () => clearInterval(c.hb));
  c.until = async (f, ms = 20000) => { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(200); } return false; };
  c.end = () => { c.send({ type: 'leave' }); ws.close(); };
  await c.open;
  return c;
}
const authQ = async () => {
  const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
};
const afcHttp = async (qs = '', method = 'GET') => fetch(`http://127.0.0.1:${port}/vibeserver/autoppm?${qs}${qs ? '&' : ''}${await authQ()}`, { method }).then((r) => r.json());
/** The DAB report's offset, as the median of the last `n` locked reports. */
const recentPpm = (c, n = 6) => { const v = c.dabs.filter((d) => d.locked && typeof d.ppm === 'number').slice(-n).map((d) => d.ppm).sort((a, b) => a - b); return v.length ? v[v.length >> 1] : NaN; };
const f2 = (x) => (typeof x === 'number' ? x.toFixed(2) : String(x));

try {
  await startRtl();
  // ── A ──
  console.log('A. auto OFF: nothing changes');
  writeCfg({ autoPpm: false });
  await startSrv('a');
  let c = await client('A');
  ok(await c.until(() => c.hw), 'hwinfo arrives');
  ok(c.hw.afc && c.hw.afc.on === false && c.hw.afc.state === 'off', `hwinfo.afc says off (${JSON.stringify(c.hw.afc)})`);
  c.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('7D') });
  ok(await c.until(() => c.dab?.channel === '7D' && c.dab.locked, 30000), 'DAB 7D locks');
  await sleep(45000);
  const base = recentPpm(c);
  ok(Math.abs(base + CLOCK) < 1.0, `DAB reads the full offset: ${f2(base)} ppm (a +${CLOCK} ppm clock; the clip's own offset is the rest)`);
  const intrinsic = -base - CLOCK;   // ★ the clip's own offset, as a clock error
  console.log(`     (the bench clip's own offset, as a clock error: ${f2(intrinsic)} ppm)`);
  ok(changeLines().length === 0, 'no change logged');
  ok(!fs.existsSync(STORE), 'no store written');
  c.end(); await stopSrv();

  // ── B ──
  console.log('B. an upgrade (no autoPpm key): on, nothing until confident, then learns and corrects');
  writeCfg({});
  await startSrv('b');
  c = await client('B');
  ok(await c.until(() => c.hw?.afc), 'hwinfo.afc arrives');
  ok(c.hw.afc.on === true && c.hw.afc.state === 'learning' && c.hw.afc.inForce === 0, `ON by default, learning, nothing in force (${c.hw.afc.line})`);
  const t0 = Date.now();
  c.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('7D') });
  ok(await c.until(() => c.dab?.channel === '7D' && c.dab.locked, 30000), 'DAB 7D locks');
  await sleep(Math.max(0, 100000 - (Date.now() - t0)));
  ok(changeLines().length === 0 && c.hw.afc.inForce === 0, `100 s of clean lock: still NOTHING applied (rule 6; ${c.hw.afc.why || c.hw.afc.state})`);
  ok(await c.until(() => changeLines().length >= 1, 120000), 'then one change is logged');
  const line = changeLines()[0] || '';
  console.log(`     log: ${line.replace(/^.*auto-PPM: /, '')}`);
  ok(/\+0\.00 → \+\d\.\d\d ppm from DAB 7D \(MER \d+\.\d dB, offset -\d+ Hz\)/.test(line), 'the line says from → to, the reference, MER and offset');
  const learned = Number((line.match(/→ ([+-]\d+\.\d+) ppm/) || [])[1]);
  ok(Math.abs(learned - (CLOCK + intrinsic)) < 0.25, `it learned ${f2(learned)} ppm (expected ${f2(CLOCK + intrinsic)})`);
  ok(await c.until(() => c.hw.afc.state === 'applied' && Math.abs(c.hw.afc.inForce - learned) < 0.01, 90000), `glided in and confirmed (${c.hw.afc.state}, in force ${f2(c.hw.afc.inForce)})`);
  await sleep(8000);
  const after = recentPpm(c);
  ok(Math.abs(after) < 0.25, `DAB's measured offset fell from ${f2(base)} to ${f2(after)} ppm`);
  ok(/^Auto-corrected \+\d\.\d ppm from DAB 7D, just now$/.test(c.hw.afc.line), `the owner's line: "${c.hw.afc.line}"`);
  // the glide: no single report jumps more than a glide's worth (0.1 ppm/s; reports ~1/s)
  const seq = c.dabs.filter((d) => d.locked && typeof d.ppm === 'number');
  let jump = 0; for (let i = 1; i < seq.length; i++) jump = Math.max(jump, Math.abs(seq[i].ppm - seq[i - 1].ppm) / Math.max(0.5, (seq[i].t - seq[i - 1].t) / 1000));
  ok(jump < 0.6, `it glided: the largest change between DAB reports was ${f2(jump)} ppm/s (no jump)`);
  ok(fs.existsSync(STORE) && /^VIBE-AFC 1\nppm \d\.\d{4} at \d+ hw 0\nsrc DAB 7D\n/.test(fs.readFileSync(STORE, 'utf8')), `stored per radio: ${path.basename(STORE)}`);
  ok(!/\] ppm /.test(rtlLog) && !/ppm [1-9-]/.test(rtlLog), "the radio's own ppm was never written (the fake heard no ppm command)");
  await sleep(20000);
  ok(changeLines().length <= 2 && !/reverted|suspended/.test(serverLog()), `steady: ${changeLines().length} change(s), no reverts`);
  c.end(); await stopSrv('SIGKILL');

  // ── C ──
  console.log('C. restart: the learned value is back at once');
  await startSrv('c');
  c = await client('C');
  ok(await c.until(() => c.hw?.afc), 'hwinfo.afc arrives');
  ok(c.hw.afc.state === 'applied' && Math.abs(c.hw.afc.ppm - learned) < 0.01, `restored before any DAB: ${f2(c.hw.afc.ppm)} ppm (${c.hw.afc.line})`);
  c.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('7D') });
  ok(await c.until(() => c.dab?.channel === '7D' && c.dab.locked, 30000), 'DAB 7D locks');
  await sleep(6000);
  ok(Math.abs(recentPpm(c, 4)) < 0.25, `and DAB reads ${f2(recentPpm(c, 4))} ppm from the first reports`);
  c.end(); await stopSrv();

  // ── D ──
  console.log("D. the owner's manual ppm (2): respected, auto learns on top");
  writeCfg({ ppm: 2 });
  rtlLog = '';
  await startSrv('d');
  c = await client('D');
  ok(await c.until(() => c.hw?.afc), 'hwinfo.afc arrives');
  ok(c.hw.ppm === 2 && c.hw.afc.manual === 2, `the manual ppm is 2 (hwinfo ppm ${c.hw.ppm})`);
  ok(c.hw.afc.state === 'learning' && c.hw.afc.inForce === 0, 'the value learned on a manual 0 is NOT restored on a manual 2');
  ok(serverLog().includes('not restored'), 'and the log says why');
  c.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('7D') });
  ok(await c.until(() => c.dab?.channel === '7D' && c.dab.locked, 30000), 'DAB 7D locks');
  ok(await c.until(() => changeLines().length >= 1, 240000), 'it learns again');
  const learned2 = Number(((changeLines()[0] || '').match(/→ ([+-]\d+\.\d+) ppm/) || [])[1]);
  ok(Math.abs(learned2 - (CLOCK - 2 + intrinsic)) < 0.25, `…${f2(learned2)} ppm ON TOP of the manual 2 (expected ${f2(CLOCK - 2 + intrinsic)})`);
  const ppmCmds = [...rtlLog.matchAll(/ppm (-?\d+)/g)].map((m) => Number(m[1]));
  ok(ppmCmds.length >= 1 && ppmCmds.every((v) => v === 2), `the radio was only ever sent the owner's 2 (${JSON.stringify(ppmCmds)})`);
  ok(await c.until(() => c.hw.afc.state === 'applied', 90000), 'applied and confirmed');
  await sleep(8000);
  ok(Math.abs(recentPpm(c)) < 0.25, `DAB reads ${f2(recentPpm(c))} ppm`);

  // ── E ──
  console.log('E. Reset and Turn off');
  const r1 = await afcHttp('reset=1', 'POST');
  ok(r1.state === 'learning' && r1.ppm === 0 && r1.manual === 2, `Reset: nothing learned, the manual 2 exactly (${r1.line})`);
  ok(await c.until(() => c.hw.afc.inForce === 0, 5000), 'in force at once: 0 on top of the manual');
  ok(!/^ppm /m.test(fs.readFileSync(STORE, 'utf8')), 'the store forgets it');
  ok(serverLog().includes('the owner RESET it'), 'logged');
  await sleep(6000);
  ok(Math.abs(recentPpm(c, 4) + (CLOCK - 2 + intrinsic)) < 0.3, `DAB reads the uncorrected ${f2(recentPpm(c, 4))} ppm again`);
  const adm = await client('E', true);
  ok(await adm.until(() => adm.hw), 'an admin connects');
  adm.send({ type: 'autoppm', on: false });
  ok(await adm.until(() => adm.hw?.afc?.on === false && adm.hw.afc.state === 'off', 8000), `Turn off (an admin's socket): off (${adm.hw?.afc?.line})`);
  await sleep(1500);
  const cfgNow = JSON.parse(fs.readFileSync(CFG, 'utf8'));
  ok(cfgNow.radios?.[0]?.autoPpm === false, 'persisted: the config says autoPpm:false');
  ok(cfgNow.radios?.[0]?.ppm === 2, '…and the manual ppm is still 2');
  adm.end(); c.end();
} catch (e) {
  console.error(e); fails++;
} finally {
  await stopSrv();
  if (rtl) rtl.kill('SIGKILL');
}
console.log(`\n${passes} passed, ${fails} failed  (logs in ${T})`);
process.exit(fails ? 1 : 0);
