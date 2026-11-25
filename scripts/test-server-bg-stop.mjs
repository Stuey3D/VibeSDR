// test-server-bg-stop.mjs — ★★★ beta94: a CLEAN STOP WITH BACKGROUND DECODING RUNNING EXITS CLEANLY.
//
// The abort (beta93 and every beta94 branch, the Mac 2026-11-25 15:27 and 15:31): SIGTERM → LocalSdrShim::stop →
// stopLocked stopped the background runner, then `delete impl` ran ~BgRunner, whose stop() published to the shim's
// BgRegistry — already destroyed (declared after the runner). Its dead mutex threw "mutex lock failed: Invalid argument"
// out of a noexcept destructor: std::terminate, SIGABRT. Every apt upgrade, service restart and settings save that
// restarts the server goes down this path once background decoding has run.
//
// The real server on a fake rtl_tcp, a background profile (four APRS VFOs) that starts at once (VIBESERVER_BG_WAIT_SEC=1):
//   · nobody listening → the profile runs; SIGTERM → exit code 0, no signal, no "terminat" in the log;
//   · the profile stopped by a listener's arrival, then SIGTERM — the runner is NOT running at the stop (the shim's
//     stop skips it), so only its destructor runs: the case that aborted even with nothing decoding;
//   · and with no profile at all (the reference).
// Silent: no audio is played (no client takes audio). Exit 3 = not run (no VIBESERVER_BIN).
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** A spectrum socket (a listener arriving) — opened, then closed; nothing is read but the handshake. */
function listener(port, session) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  s.on('connect', () => s.write(`GET /ws/user-spectrum?user_session_id=${session}&bins=256 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', () => {});
  return { close: () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); } };
}

const RATE = 2400000, FREQ = 145800000;
const VFOS = []; for (let i = 0; i < 4; i++) VFOS.push({ hz: 145000000 + i * 100000 + 25000, mode: 'nfm', dec: 'aprs' });
const PROFILE = { on: true, waitMin: 1, keepInRange: false, profiles: [{ centre: FREQ, from: '00:00', to: '00:00', vfos: VFOS }] };

async function run(tag, { withBg = true, listenerFirst = false } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bgstop-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'BG stop test', adminPass: 'bg-stop-admin', decoderMax: 16,
    radios: [{ serial: 'bgstop', label: 'Test radio', enabled: true, configured: true, users: 1, ...(withBg ? { bgDecode: JSON.stringify(PROFILE) } : {}) }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_BG_WAIT_SEC: '1',
                VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(FREQ), '--mode', 'nfm', '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  const exited = new Promise((r) => srv.on('exit', (code, sig) => r({ code, sig })));
  const L = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
  const count = (re) => (L().match(re) || []).length;
  const waitFor = async (pred, ms) => { for (let t = 0; t < ms && !pred(); t += 100) await sleep(100); return pred(); };
  const START = /background decoding: profile 1, RF centre/g;
  let res;
  try {
    await waitFor(() => /listening|VibeServer/i.test(L()), 10000);
    if (withBg) {
      const started = await waitFor(() => count(START) >= 1, 15000);
      ok(started, `${tag}: the background profile runs (nobody listening)`);
      if (listenerFirst && started) {
        listener(port, 'bgstop0001');   // ★ left open: their session is held, the profile stays stopped
        const stopped = await waitFor(() => /background decoding stopped \u2014 a listener arrived/.test(L()), 10000);
        ok(stopped, `${tag}: a listener arrived and background decoding stopped`);
        await sleep(500);
      }
    } else {
      await sleep(1500);
    }
  } finally {
    srv.kill('SIGTERM');
    res = await Promise.race([exited, sleep(30000).then(() => null)]);
    if (!res) { srv.kill('SIGKILL'); await exited; }
    rtl.kill('SIGTERM');
  }
  const log = L();
  ok(!!res, `${tag}: SIGTERM → the server exits within 30 s`);
  ok(res && res.code === 0 && !res.sig, `${tag}: …with exit code 0 and no signal (${res ? `code ${res.code}, signal ${res.sig}` : 'killed'})`);
  ok(!/terminat|mutex lock failed|Abort/i.test(log), `${tag}: no std::terminate in its log${/terminat|mutex lock failed/i.test(log) ? ` — "${(log.match(/.*(terminat|mutex lock failed).*/i) || [''])[0].slice(0, 160)}"` : ''}`);
  if (withBg && !listenerFirst) ok(/background decoding stopped .*the server is stopping/.test(log), `${tag}: the stop said so ("background decoding stopped — the server is stopping")`);
  if (fail) console.log(`   server log: ${logPath}`);
  else fs.rmSync(dir, { recursive: true, force: true });
}

console.log('── no background profile (the reference) ──');
await run('no profile', { withBg: false });
console.log('── background decoding running, then SIGTERM ──');
await run('running');
console.log('── background decoding stopped by a listener, then SIGTERM (the runner idle at the stop) ──');
await run('stopped first', { listenerFirst: true });

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
