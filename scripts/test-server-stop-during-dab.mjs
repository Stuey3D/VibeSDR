// test-server-stop-during-dab.mjs — A STOP THAT LANDS ON A DAB START MUST STILL STOP, PROMPTLY AND CLEANLY.
//
// ★★★ THE FAULT (Pi 500, 2026-10-16 20:26:53, radio 00000002, RTL V4): the setup page's save restarted
//     every radio. A listener had just arrived and asked for DAB in the same second as the SIGTERM. The
//     radio logged "Stopping…", then — AFTER it — "sample rate: 2048000 … tcp=0", "[DAB] mode ON", and
//     "[DAB] ★ asked for 2.048 MS/s and the radio reports 0.000 MS/s"; "capture clean" a minute later;
//     and systemd SIGKILLed it at the 90 s stop timeout. The front door showed NO radios meanwhile.
//     Cause: the DAB request's connection thread was INSIDE setSampleRate() (which stops the reader and
//     the DSP thread itself) when the stop began, so the stop found nothing to stop, walked on, and the
//     rate change then STARTED EVERYTHING AGAIN on a receiver being dismantled; the stop then waited for
//     ever on that connection thread. On the Mac against rtl_tcp the same race CRASHES instead: SIGSEGV
//     on a nulled `p` in setSampleRate, or SIGABRT from ~Impl destroying a thread the rate change had
//     restarted (3 of 3 runs before the fix). See "NOTHING MAY ARRIVE" in stopLocked().
//
//   1. a rate change held open (VIBE_TEST_RATE_HOLD_MS) when SIGTERM arrives, with more DAB, NFM-voice
//      and de-emphasis requests sent after it: exit 0 (no signal), quickly, "local SDR stopped" logged;
//   2. a plain stop exits promptly (the rtl_tcp reader used to sit out a 5 s poll on every stop);
//   3. the shutdown WATCHDOG: a stop wedged on purpose (VIBE_TEST_STOP_WEDGE) is cut off after
//      VIBE_STOP_GRACE_S, and the journal line names the step it was stuck at.
// ★ SILENT: audio frames are received and discarded, never played.
//
//   usage: VIBESERVER_BIN=… node scripts/test-server-stop-during-dab.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
const CLIP = path.join(root, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** A minimal WebSocket client that only sends; whatever the server sends is read and dropped. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', () => {});
  return {
    send(o) {
      const b = Buffer.from(JSON.stringify(o));
      const h = Buffer.alloc(b.length < 126 ? 6 : 8);
      h[0] = 0x81;
      if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
      try { s.write(Buffer.concat([h, b])); } catch {}   // zero mask: payload unchanged
    },
    close() { try { s.destroy(); } catch {} },
  };
}

/** One server life: start it, connect a listener, run `act(ctx)` (which sends SIGTERM), wait for the exit. */
async function life(extraEnv, act, waitMs = 15000) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-stopdab-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, fullMode: true, sharing: 'public', port: front,
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: 100000000, demodMode: 'wfm',
               landingFreq: 100000000, users: 10 }],
  }));
  const rtlArgs = [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'];
  if (fs.existsSync(CLIP)) rtlArgs.push('--dab', CLIP);
  const rtl = spawn(process.execPath, rtlArgs, { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  const logPath = path.join(dir, 'server.log');
  const logFd = fs.openSync(logPath, 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--idle-grace', '0'],
                    { env, stdio: ['ignore', logFd, logFd] });
  let exit = null;
  srv.on('exit', (code, sig) => { exit = { code, sig }; });
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  await sleep(500);
  const SID = crypto.randomUUID();
  const socks = [];
  const open = (q) => { const w = ws(port, q); socks.push(w); return w; };
  open(`/ws/audio?user_session_id=${SID}&codec=opus`);
  await sleep(700);
  const spec = open(`/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=256`);
  await sleep(1200);
  let t0 = 0;
  await act({ spec, open, SID, term: () => { t0 = Date.now(); srv.kill('SIGTERM'); } });
  while (!exit && Date.now() - t0 < waitMs) await sleep(50);
  const ms = Date.now() - t0;
  if (!exit) srv.kill('SIGKILL');
  for (const w of socks) w.close();
  rtl.kill('SIGTERM');
  await sleep(200);
  return { exit, ms, log: fs.readFileSync(logPath, 'utf8'), logPath };
}

// ── 1. SIGTERM while a DAB rate change is in flight ──
console.log('── SIGTERM while a DAB start is changing the sample rate ──');
for (const after of [0, 300, 700]) {
  const r = await life({ VIBE_TEST_RATE_HOLD_MS: '1500', VIBE_STOP_GRACE_S: '30' }, async ({ spec, open, SID, term }) => {
    spec.send({ type: 'dab', on: 1, channel: '12B' });   // the rate change starts and is held open 1.5 s
    await sleep(150);
    term();
    await sleep(after);
    // ★ What the app sends on (re)connect, arriving while the stop holds the lifecycle lock.
    const late = open(`/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=256`);
    await sleep(40);
    late.send({ type: 'dab', on: 1, channel: '12B' });
    late.send({ type: 'nfmvoice', on: 1 });
    late.send({ type: 'deemph', tau: 50e-6 });
    spec.send({ type: 'nfmvoice', on: 0 });
  });
  const how = r.exit ? (r.exit.sig || `code ${r.exit.code}`) : 'STILL RUNNING';
  ok(r.exit && r.exit.sig === null && r.exit.code === 0, `+${after} ms: exited cleanly (${how}, ${r.ms} ms)   ${r.exit && r.exit.code === 0 ? '' : r.logPath}`);
  ok(r.exit && r.ms < 6000, `+${after} ms: within 6 s of the signal (${r.ms} ms)`);
  ok(r.log.includes('local SDR stopped'), `+${after} ms: the stop ran to the end ("local SDR stopped")`);
  ok(!/did not complete in/.test(r.log), `+${after} ms: without the watchdog having to cut it off`);
}

// ── 2. a plain stop is prompt ──
console.log('── a plain stop ──');
{
  const r = await life({ VIBE_STOP_GRACE_S: '30' }, async ({ term }) => { term(); });
  ok(r.exit && r.exit.code === 0 && r.exit.sig === null, `exited cleanly (${r.exit ? (r.exit.sig || 'code ' + r.exit.code) : 'STILL RUNNING'})`);
  ok(r.exit && r.ms < 5000, `within 5 s of the signal (${r.ms} ms; it was 5.1-5.8 s against rtl_tcp before — the reader sat out a 5 s poll)`);
}

// ── 3. the watchdog cuts off a stop that will not finish, and names where it was ──
console.log('── a wedged stop is cut off by the watchdog ──');
{
  const r = await life({ VIBE_TEST_STOP_WEDGE: '1', VIBE_STOP_GRACE_S: '2' }, async ({ term }) => { term(); }, 10000);
  ok(r.exit && r.exit.code === 0 && r.exit.sig === null, `exited (${r.exit ? (r.exit.sig || 'code ' + r.exit.code) : 'STILL RUNNING'})`);
  ok(r.exit && r.ms < 5000, `within grace + a second or two (${r.ms} ms)`);
  ok(/shutdown did not complete in 2s — stuck at: a test wedge/.test(r.log), 'the journal line names the step it was stuck at');
}

console.log(`\nstop during dab: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
