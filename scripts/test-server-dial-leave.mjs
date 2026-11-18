// test-server-dial-leave.mjs — V12 beta83 A LEAVE MOVES THE BANNER (Stuart, 2026-11-18, the Pi 2: four listeners gone for
// 50 minutes and his app still read "ASK TO TUNE · 5/5"; "it looked like another user needed to connect to cause it to
// refresh the usercount"). The banners read the DIAL's `listeners`; the dial went out on a join, never on a leave.
// Against the REAL server (shared dial: no locked range, --users 3) and a fake rtl_tcp:
//   1. three listeners: the first's latest dial says 3;
//   2. the third leaves on purpose (`leave`, not held): the first is sent a dial saying 2 — no arrival needed;
//   3. the second's sockets just drop (held for its 180 s, its place still counted): the dial says 2, not 1 — a hold is a
//      listener (the server's own rule), and the count drops when the hold ends.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-dial-leave.mjs      (silent: no audio socket is opened)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — VIBESERVER_BIN needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dleave-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Dial leave test', locator: 'IO92nh', adminPass: 'dial-leave-admin',
  radios: [{ serial: 'dial-leave', label: 'Test radio', enabled: true, configured: true, users: 3 }] }));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '1000000', '--mode', 'am', '--users', '3'],
                  { env, stdio: ['ignore', ...Array(2).fill(fs.openSync(path.join(dir, 'server.log'), 'w'))] });

function listener(tag) {
  const L = { dials: [] };
  const w = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=dl${tag}${Date.now().toString(16)}&bins=128&proto=1`);
  w.onmessage = (e) => { if (typeof e.data === 'string') { try { const m = JSON.parse(e.data); if (m.type === 'dial') L.dials.push({ n: m.listeners, at: Date.now() }); } catch {} } };
  L.ws = w; L.open = new Promise((r) => { w.onopen = r; });
  L.beat = setInterval(() => { try { w.send(JSON.stringify({ type: 'ping' })); } catch {} }, 8000);
  L.last = () => L.dials.at(-1)?.n;
  return L;
}
const until = async (f, ms = 5000) => { for (let i = 0; i < ms / 50; i++) { if (f()) return true; await sleep(50); } return !!f(); };

const deadline = setTimeout(() => { console.log('   FAIL deadline'); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); process.exit(1); }, 60000);
try {
  for (let i = 0; i < 60; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); if (r.ok) break; } catch {} await sleep(250); }
  const A = listener('a'); await A.open; await sleep(500);
  const B = listener('b'); await B.open; await sleep(500);
  const C = listener('c'); await C.open; await sleep(800);
  ok(await until(() => A.last() === 3), `three listeners: A's dial says ${A.last()}`);

  const t1 = Date.now();
  C.ws.send(JSON.stringify({ type: 'leave' })); await sleep(100); C.ws.close(); clearInterval(C.beat);
  ok(await until(() => A.dials.some((d) => d.at >= t1 && d.n === 2)), `C left on purpose: A is sent a dial saying 2 with nobody arriving (${A.last()})`);

  const t2 = Date.now();
  B.ws.close(); clearInterval(B.beat);   // ★ no `leave`: a dropped phone — held, its place still counted
  await sleep(1500);
  ok(A.last() === 2 && !A.dials.some((d) => d.at >= t2 && d.n === 1), `B's sockets dropped (held): the dial still says 2 — a held place is counted (${A.last()})`);
  const log = fs.readFileSync(path.join(dir, 'server.log'), 'utf8');
  ok(/session is HELD/.test(log), 'B was held (the server log says so)');
  A.ws.close(); clearInterval(A.beat);
} catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
finally {
  clearTimeout(deadline);
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(500);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
