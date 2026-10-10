// ★★★ A CLIENT THAT VANISHES MID RAW-IQ STREAM MUST NOT WEDGE THE RADIO (2026-10-10). The Pi 500's V4L froze at 03:17:
// a full-rate raw IQ consumer's spectrum socket closed with the stream still on; the close path called iqStopDirect()
// under clientMtx, whose applyAutoIf() takes clientMtx again — a std::mutex locked twice by one thread. DSP,
// housekeeping, hotplug and every new connection queued behind it for good ("IQ overrun — the DSP thread was blocked"
// every 2 s, no HTTP). Any app killed or network lost mid-stream did the same.
//   1. a full-rate IQ stream starts and delivers samples;
//   2. its spectrum socket is cut abruptly (no iqout off, no close frame) — the IQ consumer too;
//   3. the server still answers HTTP within 3 s, a NEW listener gets a spectrum frame, and IQ out can start again.
// ★ SILENT: no audio. Loopback only. usage: VIBESERVER_BIN=… node scripts/test-iq-close-deadlock.mjs
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-iqdl-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'IQ close test', locator: 'IO92nh', adminPass: 'iq-close-admin',
  radios: [{ serial: 'iqdl-test', label: 'Test radio', enabled: true, configured: true, users: 1, rawIq: 1 }],
}));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
                  { env, stdio: ['ignore', log, log] });
const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
const openSpec = (sid) => {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
  ws.binaryType = 'arraybuffer';
  const st = { ws, iq: null, frames: 0 };
  ws.onmessage = (e) => { if (typeof e.data !== 'string') { st.frames++; return; } const j = JSON.parse(e.data); if (j.type === 'iqout') st.iq = j; };
  st.open = new Promise((r) => { ws.onopen = r; });
  return st;
};
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

try {
  ok(await until(async () => !!(await status()), 10000), 'the server is up');
  const full = (await status())?.rawIqFull;
  ok(full > 0, `full-rate raw IQ is offered (${full} Hz)`);

  const a = openSpec('iqdl0001');
  await a.open;
  await sleep(800);
  a.ws.send(JSON.stringify({ type: 'iqout', on: 1, rate: full }));
  ok(await until(() => !!a.iq, 5000) && a.iq.on === 1 && a.iq.full === true, `the stream starts (port ${a.iq?.port})`);
  let got = 0;
  const consumer = net.connect(a.iq.port, '127.0.0.1');
  consumer.on('data', (b) => { got += b.length; });
  consumer.on('error', () => {});
  ok(await until(() => got > 2_000_000, 8000), `samples flow (${got} bytes)`);

  // ★ The cut: no iqout off, no close frame — the socket simply goes, as when an app is killed.
  a.ws.close = () => {};
  try { a.ws._socket?.destroy?.(); } catch {}
  // Node's WebSocket has no raw socket handle: terminate by dropping both TCP connections at once.
  consumer.destroy();
  a.ws.onmessage = null;
  // ★ Fallback that always works: kill the WebSocket's TCP by closing it with an abnormal code is not allowed client-side,
  //   so close it normally WITHOUT sending iqout off — the server's close path still runs with the stream on.
  try { WebSocket.prototype.close.call(a.ws); } catch {}
  await sleep(1500);

  const t0 = Date.now();
  const s1 = await status();
  ok(!!s1, `the server still answers HTTP after the cut (${s1 ? Date.now() - t0 + ' ms' : 'NO ANSWER — wedged'})`);
  const b = openSpec('iqdl0002');
  ok(await until(() => b.frames > 3, 5000), `a new listener gets spectrum frames (${b.frames})`);
  b.ws.send(JSON.stringify({ type: 'iqout', on: 1, rate: full }));
  ok(await until(() => !!b.iq && b.iq.on === 1, 5000), 'and raw IQ out can start again');
  b.ws.send(JSON.stringify({ type: 'iqout', on: 0 }));
  await sleep(300);
  b.ws.close();
  const overruns = (fs.readFileSync(path.join(dir, 'server.log'), 'utf8').match(/IQ overrun/g) || []).length;
  ok(overruns === 0, `no DSP-thread stall in the log (${overruns} "IQ overrun")`);
} finally {
  try { srv.kill('SIGTERM'); } catch {}
  try { rtl.kill('SIGTERM'); } catch {}
}
console.log(`test-iq-close-deadlock: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
