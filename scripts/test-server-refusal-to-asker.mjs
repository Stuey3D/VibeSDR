// test-server-refusal-to-asker.mjs — a locked control's refusal goes to the listener who asked, never to the radio's
// primary listener (Stuart, 2026-11-13: the Mac app showed "That password was not accepted" — "I never even attempted a
// password entry". A browser's stored bias-T, refused at 22:59:59, had been sent to the owner's socket as an admin refusal).
//   VIBESERVER_BIN=…/vibeserver node scripts/test-server-refusal-to-asker.mjs     (silent: no audio socket is opened)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-refuse-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'refusal test', adminPass: 'refusal-test-admin',
  radios: [{ serial: 'r', label: 'Test radio', enabled: true, configured: true, users: 4 }],
}));
const rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
for (let i = 0; i < 40; i++) { if (await new Promise((r) => { const c = net.connect(rtlPort, '127.0.0.1', () => { c.destroy(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
const port = await freePort();
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), fs.openSync(path.join(dir, 'server.log'), 'a')] });
const open = async (id) => {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`);
  const got = [];
  ws.onmessage = (e) => { if (typeof e.data !== 'string') return; try { got.push(JSON.parse(e.data)); } catch {} };
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  return { ws, got };
};
try {
  let up = false;
  for (let i = 0; i < 150 && !up; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); up = true; } catch { await sleep(100); } }
  ok(up, 'the server is up');
  const A = await open('refuse-owner-a');     // first in: the radio's primary listener
  await sleep(1200);
  const B = await open('refuse-visitor-b');
  await sleep(1500);
  A.got.length = 0; B.got.length = 0;
  B.ws.send(JSON.stringify({ type: 'biasT', on: true }));   // a visitor's stored bias-T, no admin
  await sleep(1500);
  const log = fs.readFileSync(path.join(dir, 'server.log'), 'utf8');
  ok(/refused bias-T — admin password required/.test(log), 'the server refuses the visitor\'s bias-T');
  ok(B.got.some((m) => m.type === 'admin' && m.refused === true), 'the visitor who asked is told (admin refused)');
  ok(!A.got.some((m) => m.type === 'admin' && m.refused === true), '★ the primary listener, who asked nothing, is NOT told — no false "password not accepted"');
  A.ws.close(); B.ws.close();
} finally {
  srv.kill('SIGTERM'); rtl.kill(); await sleep(500);
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log(fail ? `   ${fail} FAILED, ${pass} passed` : `   ${pass} passed, 0 failed`);
process.exit(fail ? 1 : 0);
