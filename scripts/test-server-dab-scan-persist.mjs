#!/usr/bin/env node
// test-server-dab-scan-persist.mjs — THE LAST DAB BAND SCAN SURVIVES A RESTART (2026-11-12, Stuart on the R36S: "after a
// DAB scan on the R36S if I power off and on again the DAB multiplexes are forgotten and the scan button is present
// again, not sure if this is also the case for the servers too"). It was the case for every server: the list lived in
// the radio process's memory only.
//
// Real server, the bench multiplex through fake-rtl-tcp --dab --dab-at (it plays ONLY on 11D), no browser, silent.
//   A. a single-user radio scans the band: 11D found; the list is written beside the radio's bookmarks, per serial.
//   B. POWER CUT (SIGKILL) and start again: dab_scan_get answers with the same list (block, ensemble, services, `at`) to a
//      fresh client — MULTIPLEXES, not SCAN — and the admin setup endpoint says the same.
//   C. SCAN AGAIN, cancelled at once: the blocks it never reached keep the last scan's ensembles (11D is still listed),
//      and that survives a restart too.
//   D. ANOTHER RADIO on the same machine (another serial, same data directory): no list — one radio's scan never shows
//      on another's.
//   E. a DAMAGED file: no list (the SCAN key shows), and the server carries on.
//   F. a full scan that finds nothing (the clip moved off Band III): the file goes, so the next start offers SCAN.
//   H. THE HANDHELD (`vibeserver --handheld`, the R36S's engine, against a fake radio + fake hardware service): the scan
//      lands in handheld/radio-<serial>.dab-scan.txt beside that radio's memory, is there after the whole engine is
//      killed and started again (the power-off Stuart reported), and Start+Select's reset clears it.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-server-dab-scan-persist.mjs   (exit 3 = not run; ~6 min)
//   VIBE_DAB_CLIP overrides the bench clip (vibeserver/bench-clip/dab-bench-clean.vbu8, made by make-clip.sh).
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { startFakeHwd } from './fake-vibe-hwd.mjs';

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
const HERE = '11D', HERE_HZ = 222064000;
const PASS = 'dab-persist-admin';
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabpersist-'));
const DATA = path.join(T, 'data'), RUN = path.join(T, 'run');
for (const d of [DATA, RUN]) fs.mkdirSync(d, { recursive: true });
const raw = path.join(T, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));

const procs = [];
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGKILL'); } catch {} } });
let srv = null, rtl = null, port = 0, rtlPort = 0;
async function startRtl(atHz) {
  if (rtl) { rtl.kill('SIGKILL'); await sleep(300); }
  rtlPort = await freePort();
  rtl = spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw, '--dab-at', String(atHz)], { stdio: 'ignore' });
  procs.push(rtl);
  await sleep(400);
}
async function startSrv(serial) {
  port = await freePort();
  const cfg = path.join(T, `config-${serial}.json`);
  fs.writeFileSync(cfg, JSON.stringify({ configured: true, name: 'Scan persist test', locator: 'IO92nh', adminPass: PASS,
    radios: [{ serial, label: 'T', enabled: true, configured: true, users: 1 }] }));
  const logPath = path.join(T, `server-${serial}.log`);
  const log = fs.openSync(logPath, 'a');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'], {
    env: { ...process.env, VIBESERVER_CONFIG: cfg, VIBESERVER_DATA_DIR: DATA, VIBESERVER_RUNTIME_DIR: RUN, HOME: T },
    stdio: ['ignore', log, log] });
  procs.push(srv);
  for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); return logPath; } catch { await sleep(250); } }
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
function client(tag) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${tag}${Math.random().toString(16).slice(2, 8)}&bins=256&proto=1`);
  const c = { ws, hw: null, dab: null, scan: null, scans: [], open: new Promise((r) => (ws.onopen = r)) };
  ws.onmessage = (ev) => {
    if (typeof ev.data !== 'string') return;
    let m; try { m = JSON.parse(ev.data); } catch { return; }
    if (m.type === 'hwinfo') c.hw = m;
    else if (m.type === 'dab' && m.channel) c.dab = m;
    else if (m.type === 'dab_scan') { c.scan = m; c.scans.push(m); }
  };
  c.send = (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); };
  c.hb = setInterval(() => c.send({ type: 'dab_resync' }), 8000);   // ★ proto=1: a silent session is released at 30 s
  ws.addEventListener('close', () => clearInterval(c.hb));
  c.until = async (f, ms = 20000) => { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(100); } return false; };
  c.end = () => { c.send({ type: 'leave' }); ws.close(); };
  return c;
}
/** What a client arriving fresh is told — the handheld UI and the web client both ask exactly this on entering DAB. */
async function askList(tag) {
  const c = client(tag);
  await c.open;
  await c.until(() => c.hw);
  c.scan = null;
  c.send({ type: 'dab_scan_get' });
  await c.until(() => c.scan);
  const s = c.scan;
  c.end();
  return s;
}
const auth = async () => {
  const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
};
const fileA = path.join(DATA, 'dab-scan-persist-a.txt');
const brief = (s) => JSON.stringify((s?.found || []).map((h) => [h.block, h.ensemble, h.services.length]));

try {
  await startRtl(HERE_HZ);
  // ── A ──
  console.log('A. a single-user radio scans the band; the list is written per radio');
  await startSrv('persist-a');
  const a = client('A');
  await a.open;
  ok(await a.until(() => a.hw?.dabScan === true), 'the scan is offered (single-user radio)');
  ok(!fs.existsSync(fileA), 'no saved list before the first scan');
  a.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('12B') });
  ok(await a.until(() => a.dab?.channel === '12B'), 'in DAB on 12B');
  a.send({ type: 'dab_scan', on: 1 });
  ok(await a.until(() => a.scan?.finished && !a.scan.running, 300000), 'the scan finishes');
  const first = a.scan;
  ok(first.found.length === 1 && first.found[0].block === HERE && first.found[0].services.length >= 5, `it found ${HERE} (${brief(first)})`);
  ok(first.at > 0 && Math.abs(first.at - Date.now() / 1000) < 60, `…and says when it ended ("at" ${first.at})`);
  ok(fs.existsSync(fileA) && fs.readFileSync(fileA, 'utf8').startsWith('VIBE-DAB-SCAN 1\n'), `written: ${path.basename(fileA)}`);
  ok(!fs.existsSync(fileA + '.tmp'), '…atomically (no .tmp left)');
  ok(!fs.readdirSync(DATA).some((f) => f === 'dab-scan.txt'), '…per serial, never the machine-wide name');
  a.end();
  // ── B ──
  console.log('B. power cut (SIGKILL), start again: the list is still there');
  await stopSrv('SIGKILL');
  await startSrv('persist-a');
  const b = await askList('B');
  ok(b && b.finished === true && b.running === false, `a fresh client is told a finished scan (running ${b?.running}, finished ${b?.finished})`);
  ok(brief(b) === brief(first), `…with the same list (${brief(b)})`);
  ok(JSON.stringify(b.found[0]?.services) === JSON.stringify(first.found[0]?.services), '…every service, SId and label');
  ok(b.found[0]?.hz === HERE_HZ && b.found[0]?.complete === first.found[0]?.complete, '…its frequency and completeness');
  ok(b.at === first.at, `…and the time it ended (${b.at})`);
  const setup = await fetch(`http://127.0.0.1:${port}/vibeserver/dab-bandscan?${await auth()}`).then((r) => r.json());
  ok(brief(setup) === brief(first), `the setup page's endpoint says the same (${brief(setup)})`);
  // ── C ──
  console.log('C. SCAN AGAIN cancelled at once keeps the blocks it never reached');
  const c = client('C');
  await c.open;
  await c.until(() => c.hw);
  c.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('12B') });
  ok(await c.until(() => c.dab?.channel === '12B'), 'in DAB on 12B');
  c.scans.length = 0;
  c.send({ type: 'dab_scan', on: 1 });
  ok(await c.until(() => c.scan?.running && c.scan.done >= 2), 'a rescan is running');
  ok(c.scan.found.length === 0, '…its list starts empty while it runs (it is a new scan)');
  c.send({ type: 'dab_scan', on: 0 });
  ok(await c.until(() => c.scan?.finished && !c.scan.running), 'cancelled');
  ok(c.scan.cancelled === true && c.scan.done < BLOCKS.indexOf(HERE), `…before ${HERE} (done ${c.scan.done})`);
  ok(brief(c.scan) === brief(first), `…and ${HERE} is still listed (${brief(c.scan)})`);
  c.end();
  await stopSrv();
  await startSrv('persist-a');
  const c2 = await askList('C2');
  ok(brief(c2) === brief(first), `…across a restart too (${brief(c2)})`);
  await stopSrv();
  // ── D ──
  console.log('D. another radio on the same machine has its own (empty) list');
  await startSrv('persist-b');
  const d = await askList('D');
  ok(d && (d.found || []).length === 0, `radio B is offered no list (${brief(d)}) — so its SCAN key shows`);
  ok(fs.existsSync(fileA), "…and radio A's file is untouched");
  await stopSrv();
  // ── E ──
  console.log('E. a damaged file: no list, and the server carries on');
  const good = fs.readFileSync(fileA);
  fs.writeFileSync(fileA, 'H 11D \u0000\u0001 garbage\n'.repeat(50));
  await startSrv('persist-a');
  const e = await askList('E');
  ok(e && (e.found || []).length === 0, `no list from a damaged file (${brief(e)})`);
  ok((await fetch(`http://127.0.0.1:${port}/vibeserver.json`)).ok, 'the server is serving');
  await stopSrv();
  fs.writeFileSync(fileA, good);
  // ── F ──
  console.log('F. a full scan that finds nothing removes the list (SCAN is offered again)');
  await startRtl(80000000);   // ★ the clip off Band III: every block is noise
  await startSrv('persist-a');
  const f = client('F');
  await f.open;
  await f.until(() => f.hw);
  f.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('12B') });
  ok(await f.until(() => f.dab?.channel === '12B'), 'in DAB on 12B');
  f.scans.length = 0;
  f.send({ type: 'dab_scan', on: 1 });
  ok(await f.until(() => f.scans.some((m) => m.running)), 'the scan starts');
  ok(await f.until(() => f.scan?.finished && !f.scan.running, 300000), 'and walks the band');
  ok(f.scan.found.length === 0 && !f.scan.cancelled, `nothing found (${brief(f.scan)})`);
  ok(!fs.existsSync(fileA), 'the saved list is gone');
  f.end();
  await stopSrv();
  await startSrv('persist-a');
  const f2 = await askList('F2');
  ok((f2.found || []).length === 0, 'after a restart: no list');
  await stopSrv();
  // ── H ──
  console.log('H. the handheld: the scan survives the engine being killed, and Start+Select clears it');
  await startRtl(HERE_HZ);
  const HH = path.join(T, 'hh'), HDATA = path.join(HH, 'data'), HCARD = path.join(HH, 'card');
  for (const d of [HDATA, HCARD]) fs.mkdirSync(d, { recursive: true });
  const radios = path.join(HH, 'radios.txt'), resetFlag = path.join(HH, 'reset-radio'), hwdSock = path.join(HH, 'hwd.sock');
  fs.writeFileSync(radios, `HHDAB rtlsdr 127.0.0.1:${rtlPort}\n`);
  const hwd = await startFakeHwd(hwdSock);
  const startHh = async () => {
    port = await freePort();
    const log = fs.openSync(path.join(HH, 'handheld.log'), 'a');
    srv = spawn(BIN, ['--handheld', '--port', String(port)], {
      env: { ...process.env, VIBESERVER_DATA_DIR: HDATA, VIBESERVER_CARD_DIR: HCARD, VIBE_HWD_SOCK: hwdSock, HOME: HH,
             VIBE_HANDHELD_FAKE_RADIOS: radios, VIBE_HANDHELD_RESET_FLAG: resetFlag },
      stdio: ['ignore', log, log] });
    procs.push(srv);
    for (let i = 0; i < 300; i++) {
      try { const j = await fetch(`http://127.0.0.1:${port}/handheld/session`).then((r) => r.json()); if (j.state === 'running' && j.radio?.serial === 'HHDAB') return; } catch {}
      await sleep(100);
    }
    throw new Error('the handheld radio did not start');
  };
  const hhFile = path.join(HDATA, 'handheld', 'radio-HHDAB.dab-scan.txt');
  await startHh();
  const h = client('H');
  await h.open;
  ok(await h.until(() => h.hw?.dabScan === true), 'the handheld offers the scan');
  h.send({ type: 'dab', on: 1, channel: BLOCKS.indexOf('12B') });
  ok(await h.until(() => h.dab?.channel === '12B'), 'in DAB on 12B');
  h.send({ type: 'dab_scan', on: 1 });
  ok(await h.until(() => h.scan?.finished && !h.scan.running, 300000), 'the scan finishes');
  const hFirst = h.scan;
  ok(hFirst.found.length === 1 && hFirst.found[0].block === HERE, `it found ${HERE} (${brief(hFirst)})`);
  ok(fs.existsSync(hhFile), `written beside the radio's memory: handheld/${path.basename(hhFile)}`);
  h.end();
  await stopSrv('SIGKILL');   // ★ the whole engine, as a power-off takes it (its radio process dies with it)
  await sleep(1500);
  await startHh();
  const h2 = await askList('H2');
  ok(brief(h2) === brief(hFirst) && h2.at === hFirst.at, `after the engine is killed and started again: the same list (${brief(h2)})`);
  await stopSrv();
  fs.writeFileSync(resetFlag, '1');   // ★ Start+Select held at boot (vibe-bootkeys writes this flag)
  await startHh();
  ok(!fs.existsSync(hhFile), "Start+Select's reset cleared the radio's saved scan");
  const h3 = await askList('H3');
  ok((h3.found || []).length === 0, `…and the radio offers no list (${brief(h3)}) — the SCAN key shows`);
  await stopSrv();
  await hwd.close?.();
} catch (e) {
  console.error(e);
  fails++;
} finally {
  await stopSrv();
  if (rtl) rtl.kill('SIGKILL');
}
console.log(`\n${passes} passed, ${fails} failed`);
if (fails) console.log(`  logs: ${T}`);
else fs.rmSync(T, { recursive: true, force: true });
process.exit(fails ? 1 : 0);
