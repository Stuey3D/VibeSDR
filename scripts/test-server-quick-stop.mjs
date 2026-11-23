// test-server-quick-stop.mjs — A RADIO TOLD TO STOP MUST STOP PROMPTLY, HOWEVER SOON AFTER IT STARTED. docs/v12/SHUTDOWN-HANG.md
//
// ★★★ THE FAULT (Stuart's Lenovo, 2026-11-20 → 22, beta90 and beta91, 12 times in 3 days): a radio process asked to stop
//     logged "local SDR stopped" and then sat until the 8 s watchdog: "shutdown did not complete in 8s — stuck at: (before
//     or after the radio's stop) — exiting anyway", the main thread in futex_wait_queue. One was started 22:59:06 and told
//     to stop 22:59:07 by the radio reconcile — one second into its life.
//
//   1. SIGTERM at 0.3 / 0.6 / 1.0 / 1.5 / 2.5 s after the process starts (before, during and just after start-up), each
//      several times: exit 0, no signal, well under the watchdog (< 3 s), the watchdog never needed;
//   2. SIGTERM after normal running with a listener connected: the same.
//  The watchdog is set to 30 s here (VIBE_STOP_GRACE_S) so a hang shows as a long exit rather than being cut off at 8 s.
// ★ SILENT: no audio device; the listener's frames are read and dropped. Bounded: every wait has a deadline, children
//   are killed in finally.
//
//   usage: VIBESERVER_BIN=… node --max-old-space-size=512 scripts/test-server-quick-stop.mjs [rounds]
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
const ROUNDS = Math.max(1, Number(process.argv[2]) || 3);
const LIMIT_MS = 3000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', () => {});
  return { close() { try { s.destroy(); } catch {} } };
}

/** One life: start fake rtl_tcp + the server, `act` decides when to SIGTERM; returns the exit and how long it took. */
async function life(act, extraEnv = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-quickstop-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, fullMode: true, sharing: 'public', port: front,
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: 14074000, demodMode: 'usb',
               landingFreq: 14074000, users: 10 }],
  }));
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), VIBE_STOP_GRACE_S: '30', ...extraEnv };
  const logPath = path.join(dir, 'server.log');
  const logFd = fs.openSync(logPath, 'w');
  let srv = null, exit = null, t0 = 0, sampled = '';
  try {
    await sleep(250);
    srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--idle-grace', '0'],
                { env, stdio: ['ignore', logFd, logFd] });
    srv.on('exit', (code, sig) => { exit = { code, sig }; });
    const born = Date.now();
    await act({ port, born, term: () => { t0 = Date.now(); srv.kill('SIGTERM'); } });
    const deadline = t0 + 35000;
    while (!exit && Date.now() < deadline) {
      await sleep(25);
      // ★ a hang: take the stacks while it is stuck (macOS `sample`), once, after 4 s
      if (!exit && !sampled && Date.now() - t0 > 4000 && process.platform === 'darwin') {
        try { sampled = execFileSync('sample', [String(srv.pid), '1', '-mayDie'], { encoding: 'utf8', timeout: 15000, maxBuffer: 16 << 20 }); }
        catch (e) { sampled = 'sample failed: ' + e.message; }
        const f = path.join(dir, 'hang-stacks.txt'); fs.writeFileSync(f, sampled); console.log(`      (stuck — stacks in ${f})`);
      }
    }
    return { exit, ms: t0 ? Date.now() - t0 : -1, log: fs.readFileSync(logPath, 'utf8'), logPath, dir };
  } finally {
    if (srv && !exit) { try { srv.kill('SIGKILL'); } catch {} }
    try { rtl.kill('SIGTERM'); } catch {}
    fs.closeSync(logFd);
  }
}

function judge(r, what) {
  const how = r.exit ? (r.exit.sig || `code ${r.exit.code}`) : 'STILL RUNNING';
  // ★ a SIGTERM before main() has installed its handler ends the process by the default action — that IS a prompt stop
  const early = r.exit && r.exit.sig === 'SIGTERM' && !/VibeServer|local SDR/.test(r.log);
  ok(r.exit && (early || (r.exit.sig === null && r.exit.code === 0)) && r.ms < LIMIT_MS,
     `${what}: exited cleanly in ${r.ms} ms (${how})${r.exit && r.ms < LIMIT_MS ? '' : '   ' + r.logPath}`);
  ok(!/did not complete in/.test(r.log), `${what}: the watchdog was not needed`);
  if (r.exit && r.ms < LIMIT_MS && !/did not complete in/.test(r.log)) { try { fs.rmSync(r.dir, { recursive: true, force: true }); } catch {} }
}

console.log('── a stop in the first seconds of the radio process ──');
for (let round = 0; round < ROUNDS; round++) {
  for (const at of [300, 600, 1000, 1500, 2500]) {
    const r = await life(async ({ born, term }) => { while (Date.now() - born < at) await sleep(10); term(); });
    judge(r, `SIGTERM ${at} ms after start (round ${round + 1})`);
  }
}

console.log('── a stop after normal running, with a listener ──');
for (let round = 0; round < ROUNDS; round++) {
  const r = await life(async ({ port, term }) => {
    for (let i = 0; i < 100; i++) {
      const up = await new Promise((res) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); res(true); }); c.on('error', () => res(false)); });
      if (up) break;
      await sleep(100);
    }
    const SID = crypto.randomUUID();
    const a = ws(port, `/ws/audio?user_session_id=${SID}&codec=opus`);
    await sleep(500);
    const s = ws(port, `/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=256`);
    await sleep(3000);
    term();
    await sleep(100);
    a.close(); s.close();
  });
  judge(r, `SIGTERM after 4 s running with a listener (round ${round + 1})`);
}

// ── 3. a static destructor that never returns (a library's finaliser) must not hold the exit ──
console.log('── an exit-time destructor that blocks (VIBE_TEST_EXIT_WEDGE) ──');
for (const at of [1000, 4000]) {
  const r = await life(async ({ born, term }) => { while (Date.now() - born < at) await sleep(10); term(); }, { VIBE_TEST_EXIT_WEDGE: '1' });
  judge(r, `SIGTERM ${at} ms after start, a blocking finaliser at exit`);
  ok(r.log.includes('local SDR stopped'), `SIGTERM ${at} ms after start: the orderly stop ran to the end first`);
}

console.log(`\nquick stop: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
