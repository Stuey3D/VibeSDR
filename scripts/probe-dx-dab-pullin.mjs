#!/usr/bin/env node
// probe-dx-dab-pullin.mjs — DWELL vs PULL-IN for the DX scanner's DAB sweep (V12, 2026-11-04). docs/v12/DX-SCANNER.md.
//
// The weak 12D reference capture (VIBE_TEST_AUDIO/dab/bench/12D-v4l-2048.vbu8) replayed through fake-rtl-tcp on 12D only,
// at its own level and with extra noise added (a weaker copy of the same multiplex), and for each:
//   DX   the band scan in DX MODE, the cursor seeded on 12D so 12D is the first block visited;
//   OWN  the owner's ordinary band scan (/vibeserver/dab-bandscan?start=1), which walks 5A → 12D first.
// Each prints the server's own line for 12D: lock / first FIB / complete list times and what it found — the numbers the
// DX dwell bounds (kDx*) were chosen against. ★ A replay does not follow the gain the server writes, so the AGC's part is
// not in these numbers (fake-rtl-tcp --dab-mux models that, from bench-clip/mux.cf32, absent here).
// ★ Silent: no browser, no audio device.  usage: VIBESERVER_BIN=… node scripts/probe-dx-dab-pullin.mjs [noise…]
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const TA = process.env.VIBE_TEST_AUDIO || path.join(os.homedir(), 'VibeSDR-test-audio');
const CLIP = path.join(TA, 'dab', 'bench', '12D-v4l-2048.vbu8');
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(CLIP)) { console.log('   not run — VIBESERVER_BIN and the 12D capture needed'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const NOISES = process.argv.slice(2).map(Number).filter((x) => x >= 0);
const levels = NOISES.length ? NOISES : [0, 2, 4, 6];   // extra noise, u8 counts RMS per component

const raw0 = fs.readFileSync(CLIP).subarray(28);
let p = 0; for (let i = 0; i < Math.min(raw0.length, 4e6); i++) { const v = raw0[i] - 127.5; p += v * v; }
const rms = Math.sqrt(p / Math.min(raw0.length, 4e6));
console.log(`12D capture: ${(raw0.length / 2 / 2.048e6).toFixed(1)} s, ${rms.toFixed(2)} counts RMS per component`);
const gauss = () => { let u = 0, v = 0; while (u === 0) u = Math.random(); v = Math.random(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); };

async function run(tag, noise, dx) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-dxpull-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const raw = path.join(dir, 'clip.u8');
  if (noise > 0) {
    const b = Buffer.from(raw0.subarray(0, Math.min(raw0.length, 2.048e6 * 2 * 30)));   // 30 s is plenty for one block
    for (let i = 0; i < b.length; i++) b[i] = Math.max(0, Math.min(255, Math.round(b[i] + noise * gauss())));
    fs.writeFileSync(raw, b);
  } else fs.writeFileSync(raw, raw0.subarray(0, Math.min(raw0.length, 2.048e6 * 2 * 30)));
  const cfg = { configured: true, name: 'DX pull-in', locator: 'IO92nh', adminPass: 'pull-admin',
    radios: [{ serial: 'pull', label: 'Test radio', enabled: true, configured: true, users: 1, rate: 2048000,
               ...(dx ? { dxScan: JSON.stringify({ on: true, bands: 'dab', waitMin: 5 }) } : {}) }] };
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify(cfg));
  if (dx) fs.writeFileSync(path.join(dir, 'data', 'dxlog-pull.json'), JSON.stringify({ v: 1, created: Math.floor(Date.now() / 1000), nextId: 1,
    sweepsFm: 0, sweepsDab: 0, alertSeq: 0, fmCursorKhz: 87500, dabCursor: 34, nextDab: true, bootDone: false, entries: [], alerts: [] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw, '--dab-at', '229072000'], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_DX_WAIT_SEC: '1', VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const logFd = fs.openSync(logPath, 'w');   // ★ the server's lines go to stderr — both streams, one file
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '93200000', '--mode', 'wfm', '--users', '1'],
                    { env, stdio: ['ignore', logFd, logFd] });
  for (let i = 0; i < 100; i++) { const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); }); if (up) break; await sleep(100); }
  await sleep(1500);
  if (!dx) await fetch(`http://127.0.0.1:${port}/vibeserver/dab-bandscan?start=1`).catch(() => {});
  let line = '';
  for (let t = 0; t < 180 && !line; t++) {
    await sleep(1000);
    line = (fs.readFileSync(logPath, 'utf8').split('\n').find((l) => /\[DAB\] band scan: 12D (nothing|")/.test(l)) || '');
  }
  try { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } catch {}
  await sleep(800);
  fs.rmSync(dir, { recursive: true, force: true });
  return line.replace(/.*\[DAB\] band scan: /, '') || '(no line in 180 s)';
}

for (const n of levels) {
  const snrDrop = n > 0 ? 10 * Math.log10((rms * rms + n * n) / (rms * rms)) : 0;
  console.log(`\n── extra noise ${n} counts RMS (total power +${snrDrop.toFixed(1)} dB of noise-like input) ──`);
  console.log(`   DX : ${await run(`dx${n}`, n, true)}`);
  console.log(`   OWN: ${await run(`own${n}`, n, false)}`);
}
