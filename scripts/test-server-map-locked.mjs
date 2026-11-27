// test-server-map-locked.mjs — beta96 (Stuart 11-27, Almere's ADS-B radio from the Mac app: CLOSE MAP dropped him on a 1090 MHz
// waterfall "which may have caused the server to exit a mode its not meant to exit"). The REAL server, a fake radio, a radio
// set to SINGLE MODE: ADS-B (radios[0].singleMode "adsb" — the Setup page's pick, not a hand-made blocked list):
//   1. BEFORE anybody connects, the radio's /vibeserver.json says onlyMode "adsb" — what the app reads before it opens a
//      socket, so it can go straight to the map and never show a radio screen (src/utils/mapExit.ts);
//   2. a listener LANDS in ADS-B; hwinfo singleMode "adsb", onlyMode "adsb";
//   3. EVERYTHING a client could send from a map's CLOSE or a waterfall it should never have been shown — ADS-B off, a tune,
//      a mode, a passband, a zoom / reset (the view), the rate, the IF filter, another whole-radio mode (DAB, AIS, the
//      datalink, ISM, NDB, SONDES, the VOR map), a SATELLITE pick — and the radio is STILL in ADS-B: no adsb_off, reports
//      still arriving, onlyMode still "adsb";
//   4. that listener gone, a second one still lands in ADS-B (the mode was never left).
// Needs VIBESERVER_BIN. Silent: no audio socket is played (opened only, as a listener's would be).
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms = 15000) => { for (let i = 0; i < ms / 100; i++) { if (f()) return true; await sleep(100); } return !!f(); };
const RATE = 2400000;

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
  c.count = (type, from = 0) => c.msgs.slice(from).filter((m) => m.type === type).length;
  c.close = () => { clearInterval(c.beat); try { ws.close(); } catch {} try { au.close(); } catch {} };
  return c;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-maplock-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, fullMode: true, sharing: 'public', adminPass: 'maplock-Test-Pass-7',
  radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, users: 3, mode: 'single', rate: RATE,
             freq: 96100000, demodMode: 'wfm', landingFreq: 96100000, singleMode: 'adsb' }] }));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' });
await sleep(400);
const LOG = path.join(dir, 'server.log'); const fd = fs.openSync(LOG, 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--idle-grace', '0'],
  { env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') }, stdio: ['ignore', fd, fd] });
const log = () => fs.readFileSync(LOG, 'utf8');
const listeners = [];
try {
  for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
  await sleep(3000);

  console.log('── 1. /vibeserver.json, before anybody connects ──');
  { const j = await fetch(`http://127.0.0.1:${port}/vibeserver.json`).then((r) => r.json());
    ok(j.server === 'vibeserver', 'a VibeServer answers');
    ok(j.onlyMode === 'adsb', `★ it says onlyMode "adsb" before any socket (${JSON.stringify(j.onlyMode)}) — the app goes straight to the map`); }

  console.log('── 2. a listener lands ──');
  const L = listener(port, 'lst'); listeners.push(L); await L.opened;
  ok(await until(() => L.has('adsb'), 20000), 'the listener lands in ADS-B (an "adsb" report)');
  ok(await until(() => L.hw.onlyMode === 'adsb', 5000) && L.hw.singleMode === 'adsb', `hwinfo: singleMode "adsb", onlyMode "adsb" (${JSON.stringify(L.hw.singleMode)} / ${JSON.stringify(L.hw.onlyMode)})`);

  console.log('── 3. every way a client could ask the radio to leave ADS-B ──');
  const n0 = L.msgs.length;
  const asks = [
    { type: 'adsb', on: 0 },
    { type: 'tune', frequency: 96100000, mode: 'wfm' }, { type: 'tune', hz: 96100000 },
    { type: 'mode', mode: 'wfm' }, { type: 'bandwidth', bandwidthLow: -100000, bandwidthHigh: 100000 },
    { type: 'zoom', frequency: 96100000, zoom: 1 }, { type: 'reset' },
    { type: 'sampleRate', value: 2048000 }, { type: 'tunerbw', value: 1500000 },
    { type: 'dab', on: 1 }, { type: 'ais', on: 1 }, { type: 'avdata', on: 1 }, { type: 'ism', on: 1 },
    { type: 'ndb', on: 1 }, { type: 'sondes', on: 1 }, { type: 'vormap', on: 1 },
    { type: 'sat', op: 'pick', norad: 25544, tx: 0 },
  ];
  for (const a of asks) { L.send(a); await sleep(150); }
  await sleep(4000);
  ok(!L.has('adsb_off', n0), '★ no adsb_off — the radio never left ADS-B');
  const before = L.count('adsb', n0);
  await sleep(3000);
  ok(L.count('adsb', n0) > before, `★ ADS-B reports still arriving afterwards (${before} → ${L.count('adsb', n0)})`);
  ok(L.hw.onlyMode === 'adsb', `hwinfo still onlyMode "adsb" (${JSON.stringify(L.hw.onlyMode)})`);
  ok(!['dab', 'ais', 'avdata', 'ism', 'ndb', 'sondes', 'vormap'].some((m) => L.has(m, n0)), 'no other whole-radio mode started');
  ok(L.msgs.slice(n0).some((m) => m.type === 'adsb_error' && /ADS-B only/.test(m.why || '')), 'the "ADS-B off" was refused in words');
  ok(!/\[ADS-B\] (leave|left)\b/i.test(log()) || /only mode: a listener's "adsb off" refused/.test(log()), 'the server log shows the refusal, not a leave');
  L.close();
  await sleep(2500);

  console.log('── 4. the next listener still lands in ADS-B ──');
  const M = listener(port, 'nxt'); listeners.push(M); await M.opened;
  ok(await until(() => M.has('adsb'), 20000), '★ a second listener lands in ADS-B — the mode was never left');
} catch (e) { fail++; console.log('   FAIL ' + (e?.stack || e)); }
finally {
  for (const c of listeners) c.close();
  srv.kill('SIGTERM'); rtl.kill('SIGTERM'); await sleep(600);
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
