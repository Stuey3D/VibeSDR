// agc-hop.mjs — VibeAGC end to end: a REAL vibeserver on a fake rtl_tcp whose gain is real (fake_rtl_tcp_gain.py),
// hopping between two close SSB signals the way Stuart did on the Portable (2026-10-20). SILENT — the audio socket
// is opened (a listener is what makes a receiver run) and its packets are discarded.
//
//   VIBESERVER_BIN=… node tools/agc-bench/agc-hop.mjs --scenario synth|synth-overload|real [--out DIR] [--json] [--assert]
//   (~4.5 min a scenario; needs python3 + numpy for the fake source)
//
//   synth           two USB stations 3 kHz apart (14.200 / 14.203), the loud one the biggest thing in the capture;
//                   60 s on A, then 12 hops A<->B 15 s apart. Measures gain moves and reversals during the hops,
//                   and the forgets the server logged.
//   synth-overload  the same, and a carrier 19 dB over the operating point arrives at 120 s: must be cut fast.
//   real            the V4L's own 40 m capture (~/VibeSDR-test-audio/iq/v4l_40m_7150000_250k.cu8), LSB, hopping
//                   between the two strong signals 2.9 kHz apart.
// Prints a summary; --json prints it as one JSON line. Exit 0 always (the caller judges) unless the run broke.
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const arg = (k, d) => { const i = process.argv.indexOf(`--${k}`); return i >= 0 ? process.argv[i + 1] : d; };
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('not run — set VIBESERVER_BIN'); process.exit(3); }
const scenario = arg('scenario', 'synth');
const outDir = arg('out', fs.mkdtempSync(path.join(os.tmpdir(), 'agc-hop-')));
fs.mkdirSync(outDir, { recursive: true });
const asJson = process.argv.includes('--json');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const RATE = 250000, ADMIN = 'agc-bench-admin-7';
let A, B, mode, fakeArgs, overloadAt = -1;
const hopStart = Number(arg('settle', 60)), hops = Number(arg('hops', 12)), hopEvery = Number(arg('every', 15));
if (scenario === 'real') {
  const f = path.join(os.homedir(), 'VibeSDR-test-audio/iq/v4l_40m_7150000_250k.cu8');
  if (!fs.existsSync(f)) { console.log('not run — no', f); process.exit(3); }
  // The two strong signals sit at -0.67 and +2.26 kHz from the recorded centre (measured, tools note); LSB dials
  // ~1.4 kHz above their energy.
  A = 7150000 - 671 + 1400; B = 7150000 + 2258 + 1400; mode = 'lsb';
  fakeArgs = ['--file', f, '--file-centre', '7150000', '--g0', '207'];
} else {
  A = 14200000; B = 14203000; mode = 'usb';
  fakeArgs = ['--synth', '--a-hz', String(A), '--b-hz', String(B), '--g0', '280', '--seed', '3'];
  if (scenario === 'synth-overload') { overloadAt = 120; fakeArgs.push('--blowtorch-at', '120', '--blowtorch-hz', '14230000', '--blowtorch-dbfs', '-1'); }
}
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-agc-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'AGC bench', adminPass: ADMIN,
  radios: [{ serial: 'agc-bench', label: 'bench', enabled: true, configured: true, users: 1, rtlAgc: true }] }));
const rtlPort = await freePort(), port = await freePort();
const fakeLog = path.join(outDir, 'fake.log');
const fake = spawn('nice', ['-n', '10', 'python3', path.join(here, 'fake_rtl_tcp_gain.py'), '--port', String(rtlPort), '--rate', String(RATE),
                    '--log', fakeLog, ...fakeArgs], { stdio: ['ignore', 'ignore', fs.openSync(path.join(outDir, 'fake.err'), 'w')] });
for (let i = 0; i < 200; i++) {   // the synthetic source takes a few seconds to build its content
  if (await new Promise((r) => { const c = net.connect(rtlPort, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
  await sleep(200);
}
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srvLog = path.join(outDir, 'server.log');
const srv = spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE),
                  '--freq', String(A), '--mode', mode, '--verbose'],
                  { env, stdio: ['ignore', fs.openSync(srvLog, 'w'), fs.openSync(srvLog, 'a')] });
const base = `http://127.0.0.1:${port}`, wsBase = `ws://127.0.0.1:${port}`;
let result = { scenario };
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  await sleep(2000);
  const SID = 'agchop-' + Date.now();
  const spec = new WebSocket(`${wsBase}/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=1024`);
  const audio = new WebSocket(`${wsBase}/ws/audio?user_session_id=${SID}&codec=opus`);
  audio.onmessage = () => {}; audio.onerror = () => {};
  await new Promise((r, j) => { spec.onopen = r; spec.onerror = j; });
  spec.onmessage = () => {};
  const send = (o) => spec.send(JSON.stringify(o));
  const tune = (f) => { send({ type: 'zoom', frequency: f, binBandwidth: 25 }); send({ type: 'tune', frequency: f, mode }); };
  // ★ Start at 40.0 dB, set by hand (so the server KNOWS where the tuner is — rtl_tcp has no getter), then hand it to
  //   VibeAGC: at 40 dB the loud station overloads the converter, so the first thing the loop must do is cut.
  tune(A);
  send({ type: 'gain', value: 400 });
  await sleep(1500);
  send({ type: 'gain', auto: true });
  const t0 = Date.now(); const wall0 = t0 / 1000;
  const samples = [];   // [t, gainTenths, moves60, rev60]
  let polling = true;
  (async () => {
    while (polling) {
      try {
        const j = await fetch(`${base}/vibeserver.json`).then((r) => r.json());
        const r0 = (j.radios && j.radios[0]) || j;
        const g = Number.isFinite(r0.gainNow) ? r0.gainNow : j.gainNow;
        samples.push([(Date.now() - t0) / 1000, g, j.agcMoves60s, j.agcReversals60s]);
      } catch {}
      await sleep(1000);
    }
  })();
  await sleep(hopStart * 1000);
  const hopTimes = [];
  for (let h = 0; h < hops; h++) {
    hopTimes.push((Date.now() - t0) / 1000);
    tune(h % 2 === 0 ? B : A);
    await sleep(hopEvery * 1000);
  }
  polling = false; await sleep(1200);
  // ── the record, as the owner downloads it ──
  const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
  const proof = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
  const unauth = await fetch(`${base}/vibeserver/admin/agc`);
  const rec = await fetch(`${base}/vibeserver/admin/agc?${proof}`);
  const recText = rec.status === 200 ? await rec.text() : '';
  fs.writeFileSync(path.join(outDir, 'agc-record.jsonl'), recText);
  spec.close(); audio.close();
  // ── judge ──
  // ★ THE HARDWARE TRUTH: every gain command the "dongle" received, from the fake's own log.
  const writes = fs.readFileSync(fakeLog, 'utf8').split('\n').map((l) => l.split(' '))
    .filter((w) => w[1] === 'gain').map((w) => [Number(w[0]) - wall0, Number(w[2])]);
  const hop0 = hopTimes[0];
  const before = writes.filter((w) => w[0] < hop0), during = writes.filter((w) => w[0] >= hop0);
  let moves = 0, reversals = 0, lastDir = 0, prev = before.length ? before[before.length - 1][1] : null;
  for (const [, g] of during) {
    const d = prev === null ? 0 : Math.sign(g - prev); prev = g;
    if (d) { moves++; if (lastDir && d !== lastDir) reversals++; lastDir = d; }
  }
  const gainSeries = during.map((w) => w[1]);
  const log = fs.readFileSync(srvLog, 'utf8');
  const fullForgets = (log.match(/AGC: forgetting what it learned \(retuned/g) || []).length;
  const chanForgets = (log.match(/AGC: another station, same front end/g) || []).length;
  let cutAfter = null;
  if (overloadAt > 0) {
    // ★ the fake times the blowtorch from the server's FIRST connection — its first log line — not from t0
    const fakeT0 = Number(fs.readFileSync(fakeLog, 'utf8').split('\n')[0].split(' ')[0]) - wall0;
    const at = fakeT0 + overloadAt;
    const pre = writes.filter((w) => w[0] < at).pop();
    const firstLower = writes.find((w) => w[0] >= at && pre && w[1] < pre[1]);
    cutAfter = firstLower ? +(firstLower[0] - at).toFixed(1) : null;
  }
  let recLines = 0, recOk = true, recTunes = 0, recChannel = 0;
  for (const line of recText.split('\n').filter(Boolean)) {
    recLines++; try { const o = JSON.parse(line); if (o.k === 'tune') { recTunes++; if (o.forget === 'channel') recChannel++; } } catch { recOk = false; }
  }
  result = { scenario, hops, gainMovesDuringHops: moves, reversalsDuringHops: reversals, fullForgetsOnRetune: fullForgets,
             channelForgets: chanForgets, overloadCutAfterS: cutAfter,
             gainWritesBeforeHops: before.map((w) => w[1] / 10), gainDbDuringHops: gainSeries.map((g) => g / 10),
             recordStatus: rec.status, recordUnauthStatus: unauth.status, recordLines: recLines, recordJsonOk: recOk,
             recordTunes: recTunes, recordChannelTunes: recChannel, out: outDir };
} finally { srv.kill('SIGTERM'); fake.kill('SIGTERM'); }
if (asJson) console.log(JSON.stringify(result));
else for (const [k, v] of Object.entries(result)) console.log(`  ${k.padEnd(22)} ${JSON.stringify(v)}`);
/* --assert: the 11.0.9 behaviour as a pass/fail (2026-10-20). Hopping between two close SSB signals must not move
 * the gain or forget the front end; an overload arriving mid-QSO must still be cut within 5 s, and the record
 * must be there for the owner and only for the owner. The pre-11.0.9 build fails every one of the first two
 * (measured: 24-26 gain moves, 17-23 reversals, 13 full forgets in 12 hops). */
if (process.argv.includes('--assert')) {
  const fails = [];
  const ov = scenario === 'synth-overload';
  if (result.fullForgetsOnRetune > 1) fails.push(`full forgets on retune: ${result.fullForgetsOnRetune} (only the first tune may)`);
  if (!ov && result.gainMovesDuringHops > 1) fails.push(`gain moves during hops: ${result.gainMovesDuringHops}`);
  if (result.reversalsDuringHops > 1) fails.push(`reversals during hops: ${result.reversalsDuringHops}`);
  if (ov && !(result.overloadCutAfterS !== null && result.overloadCutAfterS <= 5)) fails.push(`overload cut after ${result.overloadCutAfterS} s`);
  if (result.recordStatus !== 200 || result.recordUnauthStatus !== 401 || !result.recordJsonOk || result.recordChannelTunes !== hops)
    fails.push(`record: ${result.recordStatus}/${result.recordUnauthStatus} json ${result.recordJsonOk} channel tunes ${result.recordChannelTunes}`);
  for (const f of fails) console.log(`  FAIL ${f}`);
  console.log(fails.length ? `  ${scenario}: FAILED` : `  ${scenario}: ok`);
  process.exit(fails.length ? 1 : 0);
}
