// test-server-bg-locked.mjs — ★★★ b97 (Stuart 11-28): ON A LOCKED RANGE, BACKGROUND DECODING RUNS ALL THE TIME.
//
// A locked radio's centre never moves, so every background VFO stays in its window whoever listens: no "wait 300 s after
// the last listener", no stopping (or capping) when listeners arrive — only the CPU budget gives way. The wait and the
// "a listener arrived → background stops" behaviour are for UNLOCKED radios, which must retune to reach the profile.
// The real server on a fake rtl_tcp (2.4 MS/s):
//   · LOCKED (shared, 4 users), four APRS VFOs in range, NO wait override: the profile starts at once; three listeners
//     connect — every VFO keeps decoding (no pause, no "last user slot", no stop); the log says it runs all the time;
//   · UNLOCKED, the real wait (1 min, no override): nothing starts in 8 s; with the 1 s test wait it starts, and a
//     listener's arrival stops it (the dial goes back to them).
// Silent: no client takes audio. Exit 3 = not run (no VIBESERVER_BIN).
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
function listener(port, session) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  s.on('connect', () => s.write(`GET /ws/user-spectrum?user_session_id=${session}&bins=256 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {}); s.on('data', () => {});
  return { close: () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); } };
}

const RATE = 2400000, FREQ = 145800000;
const VFOS = []; for (let i = 0; i < 4; i++) VFOS.push({ hz: 145400000 + i * 100000 + 25000, mode: 'nfm', dec: 'aprs' });
const PROFILE = { on: true, waitMin: 1, keepInRange: false, profiles: [{ centre: FREQ, from: '00:00', to: '00:00', vfos: VFOS }] };

async function server(tag, radio, extraEnv) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bglock-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'BG locked test', adminPass: 'bg-locked-admin', decoderMax: 16,
    radios: [{ serial: 'bglock', driver: 'rtlsdr', label: 'Test radio', enabled: true, configured: true, rate: RATE, freq: FREQ, demodMode: 'nfm', bgDecode: JSON.stringify(PROFILE), ...radio }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, ...extraEnv, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  if (!extraEnv.VIBESERVER_BG_WAIT_SEC) delete env.VIBESERVER_BG_WAIT_SEC;
  const logPath = path.join(dir, 'server.log');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(FREQ), '--mode', 'nfm', '--users', String(radio.users || 1)],
                    { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  const exited = new Promise((r) => srv.on('exit', (code, sig) => r({ code, sig })));
  const L = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
  const waitFor = async (pred, ms) => { for (let t = 0; t < ms && !pred(); t += 100) await sleep(100); return pred(); };
  const stop = async () => {
    srv.kill('SIGTERM');
    const res = await Promise.race([exited, sleep(30000).then(() => null)]);
    if (!res) { srv.kill('SIGKILL'); await exited; }
    rtl.kill('SIGTERM');
    if (fail) console.log(`   server log (${tag}): ${logPath}`); else fs.rmSync(dir, { recursive: true, force: true });
  };
  await waitFor(() => /listening|VibeServer/i.test(L()), 10000);
  return { port, L, waitFor, stop };
}
const START = /background decoding: profile 1, RF centre[^\n]*4 VFO\(s\)/;

console.log('── a LOCKED range (shared, 4 users): runs at once and alongside three listeners ──');
{
  const s = await server('locked', { mode: 'locked', lockFreq: FREQ, users: 4 }, {});
  ok(await s.waitFor(() => START.test(s.L()), 15000), 'the profile starts at once — no wait (the real 1 min wait, no test override)');
  ok(/range is locked: it runs all the time/.test(s.L()), 'the log says it runs all the time on a locked range (not "wait 60 s")');
  const ls = [listener(s.port, 'bglock0001'), listener(s.port, 'bglock0002'), listener(s.port, 'bglock0003')];
  await sleep(6000);
  const log = s.L();
  ok(!/background decoding stopped/.test(log), '★★★ three listeners on: background decoding never stopped');
  ok(!/paused — a listener took this radio's last user slot/.test(log) && !/VFO \d+ \([^)]*\) paused/.test(log),
     '★★★ …and no VFO paused for them (no user-slot cap on a shared locked radio)');
  ls.forEach((l) => l.close());
  await s.stop();
}
console.log('── an UNLOCKED radio: still waits, and still stops for a listener ──');
{
  const s = await server('unlocked-wait', { users: 1 }, {});
  await sleep(8000);
  ok(!START.test(s.L()), 'the real wait (1 min): nothing has started 8 s in');
  await s.stop();
}
{
  const s = await server('unlocked-stop', { users: 1 }, { VIBESERVER_BG_WAIT_SEC: '1' });
  ok(await s.waitFor(() => START.test(s.L()), 15000), 'with the 1 s test wait the profile starts (nobody listening)');
  const l = listener(s.port, 'bgunlk0001');
  ok(await s.waitFor(() => /background decoding stopped — a listener arrived/.test(s.L()), 10000), 'a listener arrives: background decoding stops (the dial is theirs)');
  l.close();
  await s.stop();
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
