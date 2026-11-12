#!/usr/bin/env node
// test-handheld-fm-persist.mjs — THE R36S KEEPS ITS BROADCAST FM ROW THROUGH A POWER-OFF, AND A SERVER DOES NOT CHANGE
// (2026-11-12, Stuart: "the Broadcast FM noise reduction doesnt stay enabled on power off and on on the R36S. I prefer it
// on so I would want this to be remembered setting").
//
// The design (vibe_fm_defaults.h adoptAsDefaults): on the handheld ONLY, the user's change to the row (de-emphasis, ST,
// NR, IMS, CEQ, AUTO BW, NB) becomes that radio's owner defaults in its engine config — the start after a power-off
// begins there, and Start+Select (which deletes the engine config) puts the code's defaults back.
//
//   H. THE HANDHELD (`vibeserver --handheld`, a fake radio + fake hardware service):
//      1. a fresh card: NR off, CEQ on, 50 µs (Region 1);
//      2. NR on, CEQ off, 75 µs from the menu → written to handheld/radio-<serial>.engine.json (fmNr/fmCeq/fmDeemph),
//         temp + rename (no .tmp left);
//      3. the whole engine KILLED (a power-off) and started again: NR on, CEQ off, 75 µs;
//      4. a single change after the restart (AUTO BW off) keeps the rest (NR still on in the file);
//      5. Start+Select: NR off, CEQ on, 50 µs again.
//   S. A NORMAL SERVER (owner defaults NR ON, REMEMBER): a listener turns NR off; the next listener of the same run
//      inherits it (REMEMBER, as before); config.json is BYTE-IDENTICAL (no listener's change in the owner's config);
//      killed and started again, the next listener gets the owner's NR ON.
// ★ SILENT: no audio socket is opened (hwinfo arrives on the spectrum socket); the engine plays nothing itself.
//
// usage: VIBESERVER_BIN=… node scripts/test-handheld-fm-persist.mjs   (~40 s; exit 3 = not run)
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { startFakeHwd } from './fake-vibe-hwd.mjs';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0, passes = 0;
const ok = (c, what) => { console.log(`  ${c ? 'ok  ' : 'FAIL'} ${what}`); c ? passes++ : fails++; };
const FM = 100000000;
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-hhfm-'));
const procs = [];
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGKILL'); } catch {} } });

let rtl = null, rtlPort = 0, srv = null, port = 0;
async function startRtl() {
  rtlPort = await freePort();
  rtl = spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--wfm', '100.0'], { stdio: 'ignore' });
  procs.push(rtl);
  await sleep(400);
}
async function stopSrv(sig = 'SIGTERM') {
  if (!srv) return;
  const s = srv; srv = null;
  const gone = new Promise((r) => s.once('exit', r));
  s.kill(sig);
  await Promise.race([gone, sleep(10000)]);
  await sleep(300);
}
/** A listener's control socket (spectrum) — no audio is ever asked for. */
async function listener(tag) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=hhfm${tag}${Math.random().toString(16).slice(2, 8)}&bins=256&proto=1`);
  const c = { ws, hw: null, n: 0 };
  ws.onmessage = (ev) => {
    if (typeof ev.data !== 'string') return;
    let m; try { m = JSON.parse(ev.data); } catch { return; }
    if (m.type === 'hwinfo') { c.hw = m; c.n++; }
  };
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  c.send = (o) => { if (ws.readyState === 1) ws.send(JSON.stringify(o)); };
  c.until = async (f, ms = 15000) => { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(100); } return false; };
  /** the full state, freshly asked */
  c.fresh = async () => { const n0 = c.n; c.send({ type: 'state' }); await c.until(() => c.n > n0, 4000); return c.hw || {}; };
  c.end = () => { c.send({ type: 'leave' }); ws.close(); };
  await c.until(() => c.hw);
  return c;
}
const row = (h) => `wsp=${h.wsp} ceq=${h.ceq} deemph=${h.deemph} autobw=${h.autobw}`;
const near = (a, b) => Math.abs(a - b) < 1e-9;

try {
  await startRtl();
  // ── H ──
  console.log('H. the handheld keeps the Broadcast FM row through a power-off; Start+Select clears it');
  const HH = path.join(T, 'hh'), HDATA = path.join(HH, 'data'), HCARD = path.join(HH, 'card');
  for (const d of [HDATA, HCARD]) fs.mkdirSync(d, { recursive: true });
  const radios = path.join(HH, 'radios.txt'), resetFlag = path.join(HH, 'reset-radio'), hwdSock = path.join(HH, 'hwd.sock');
  fs.writeFileSync(radios, `HHFM rtlsdr 127.0.0.1:${rtlPort}\n`);
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
      try { const j = await fetch(`http://127.0.0.1:${port}/handheld/session`).then((r) => r.json()); if (j.state === 'running' && j.radio?.serial === 'HHFM') return; } catch {}
      await sleep(100);
    }
    throw new Error('the handheld radio did not start');
  };
  const engine = path.join(HDATA, 'handheld', 'radio-HHFM.engine.json');
  const engineFm = () => { try { const j = JSON.parse(fs.readFileSync(engine, 'utf8')); return (j.radios || [])[0] || {}; } catch { return {}; } };

  await startHh();
  let h = await listener('H1');
  let s = await h.fresh();
  ok(s.wsp === false && s.ceq === true && near(s.deemph, 50e-6), `a fresh card: NR off, CEQ on, 50 µs (${row(s)})`);
  h.send({ type: 'wsp', on: true });
  h.send({ type: 'ceq', on: false });
  h.send({ type: 'deemph', tau: 7.5e-5 });
  await sleep(800);
  s = await h.fresh();
  ok(s.wsp === true && s.ceq === false && near(s.deemph, 75e-6), `the user sets NR on, CEQ off, 75 µs (${row(s)})`);
  ok(await h.until(() => { const r = engineFm(); return r.fmNr === true && r.fmCeq === false && r.fmDeemph === 75; }, 5000),
     `written to handheld/${path.basename(engine)} (fmNr ${engineFm().fmNr}, fmCeq ${engineFm().fmCeq}, fmDeemph ${engineFm().fmDeemph})`);
  ok(engineFm().fmStereo === true && engineFm().fmIms === true && engineFm().fmNb === true && engineFm().fmAutoBw === true,
     '…the switches nobody touched stay as they were');
  ok(engineFm().fmRemember === false, '…and the policy stays RETURN');
  ok(!fs.existsSync(engine + '.tmp'), '…atomically (no .tmp left)');
  h.end();

  await stopSrv('SIGKILL');   // ★ the whole engine, as a power-off takes it (its radio process dies with it)
  await sleep(1500);
  await startHh();
  h = await listener('H2');
  // ★ into broadcast FM: the handheld starts at its own default dial, and AUTO BW is the WFM row's only in WFM
  h.send({ type: 'tune', frequency: FM, mode: 'wfm', bandwidthLow: -100000, bandwidthHigh: 100000 });
  await sleep(1200);
  s = await h.fresh();
  ok(s.wsp === true, `after the engine is killed and started again: NR is still ON (wsp=${s.wsp})`);
  ok(s.ceq === false && near(s.deemph, 75e-6), `…and CEQ off, 75 µs (${row(s)})`);
  h.send({ type: 'autobw', on: false });
  await sleep(800);
  ok(await h.until(() => engineFm().fmAutoBw === false, 5000) && engineFm().fmNr === true && engineFm().fmCeq === false,
     'one more change (AUTO BW off) is kept, and the rest of the row with it');
  h.end();
  await stopSrv();

  fs.writeFileSync(resetFlag, '1');   // ★ Start+Select held at boot (vibe-bootkeys writes this flag)
  await startHh();
  h = await listener('H3');
  h.send({ type: 'tune', frequency: FM, mode: 'wfm', bandwidthLow: -100000, bandwidthHigh: 100000 });
  await sleep(1200);
  s = await h.fresh();
  ok(s.wsp === false && s.ceq === true && near(s.deemph, 50e-6) && s.autobw === true,
     `Start+Select: back to the defaults — NR off, CEQ on, 50 µs, AUTO BW on (${row(s)})`);
  ok(engineFm().fmNr !== true, `…and the engine config no longer holds NR on (fmNr ${engineFm().fmNr})`);
  h.end();
  await stopSrv();
  await hwd.close?.();

  // ── S ──
  console.log('S. a normal server: the owner\'s defaults and REMEMBER as before, and nothing written to its config');
  const SD = path.join(T, 'srv');
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(SD, d), { recursive: true });
  const cfg = path.join(SD, 'config.json');
  fs.writeFileSync(cfg, JSON.stringify({ configured: true, fullMode: true, sharing: 'public', port: await freePort(), locator: 'IO92nh',
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: FM, demodMode: 'wfm', landingFreq: FM,
               users: 5, fmNr: true, fmRemember: true }] }));
  const startSrv = async () => {
    port = await freePort();
    const log = fs.openSync(path.join(SD, 'server.log'), 'a');
    srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', String(FM), '--mode', 'wfm', '--users', '5', '--idle-grace', '0'], {
      env: { ...process.env, VIBESERVER_CONFIG: cfg, VIBESERVER_DATA_DIR: path.join(SD, 'data'), VIBESERVER_RUNTIME_DIR: path.join(SD, 'run'), HOME: SD },
      stdio: ['ignore', log, log] });
    procs.push(srv);
    for (let i = 0; i < 100; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(100); } }
    await sleep(600);
  };
  await startSrv();
  await sleep(2500);   // ★ the server tidies its config at start; the comparison is from here on
  const before = fs.readFileSync(cfg);
  const A = await listener('SA');
  s = await A.fresh();
  ok(s.wsp === true, `the owner's NR ON reaches the first listener (wsp=${s.wsp})`);
  A.send({ type: 'wsp', on: false });
  await sleep(800);
  ok((await A.fresh()).wsp === false, 'a listener turns NR off');
  A.end();
  await sleep(1500);
  const B = await listener('SB');
  s = await B.fresh();
  ok(s.wsp === false, `REMEMBER: the next listener of the same run inherits NR off (wsp=${s.wsp})`);
  B.end();
  await sleep(2500);   // ★ longer than the config watcher's tick: anything that was going to write has written
  ok(Buffer.compare(before, fs.readFileSync(cfg)) === 0, 'config.json is byte-identical — no listener\'s change in the owner\'s config');
  ok(JSON.parse(fs.readFileSync(cfg, 'utf8')).radios[0].fmNr === true, '…the owner\'s fmNr is still true');
  ok(!/Broadcast FM row kept/.test(fs.readFileSync(path.join(SD, 'server.log'), 'utf8')), '…and the handheld rule never ran');
  await stopSrv('SIGKILL');
  await startSrv();
  const C = await listener('SC');
  s = await C.fresh();
  ok(s.wsp === true, `after a restart the server begins from the owner's defaults again: NR ON (wsp=${s.wsp})`);
  C.end();
  await stopSrv();
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
