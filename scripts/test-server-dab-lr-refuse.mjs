// test-server-dab-lr-refuse.mjs — a DAB request on a LOCKED RANGE is never dropped silently (2026-11-17, the Lenovo rerun:
// "ON asked … with a block" and then nothing in the log, and a client that showed nothing). The real server, a fake radio:
//   · a locked range SHARED by several listeners at 2.048 MS/s (one block, not the wide mode): refused with a dab_error
//     that names the reason (a shared locked range cannot move onto one multiplex; DAB there is the wide mode's), and
//     the refusal is in the server log;
//   · a locked range for ONE listener: DAB is honoured as before (the range is suspended while DAB runs).
// Needs VIBESERVER_BIN. Silent (no audio is played).
import net from 'node:net'; import fs from 'node:fs'; import os from 'node:os'; import path from 'node:path'; import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const BIN = process.env.VIBESERVER_BIN || '', root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
async function trial(users) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabref-')); for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, fullMode: true, sharing: 'public',
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, users, mode: 'locked', lockFreq: 215072000, rate: 2048000, freq: 215072000, demodMode: 'wfm' }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  await sleep(300);
  const LOG = path.join(dir, 'server.log'); const log = fs.openSync(LOG, 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000'], { env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') }, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
  await sleep(2500);
  const sess = 'ref' + Math.random().toString(16).slice(2, 8);
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sess}&bins=256&proto=1`);
  const au = new WebSocket(`ws://127.0.0.1:${port}/ws/audio?user_session_id=${sess}`);
  const got = []; ws.onmessage = (e) => { if (typeof e.data === 'string') { try { got.push(JSON.parse(e.data)); } catch {} } };
  await new Promise((r) => { ws.onopen = r; }); await sleep(1500);
  const from = got.length;
  ws.send(JSON.stringify({ type: 'dab', on: 1, channel: '10D' }));
  await sleep(6000);
  const after = got.slice(from), text = fs.readFileSync(LOG, 'utf8');
  ws.close(); au.close(); srv.kill('SIGTERM'); rtl.kill('SIGTERM'); await sleep(500);
  fs.rmSync(dir, { recursive: true, force: true });
  return { after, text };
}
console.log('── a locked range shared by three listeners, 2.048 MS/s (not the wide mode) ──');
{ const { after, text } = await trial(3);
  const e = after.find((m) => m.type === 'dab_error');
  ok(!!e, `★ the request is answered with a dab_error (${e ? e.why : 'none'})`);
  ok(!!e && /locked range shared by several listeners/.test(e.why) && /4 MS\/s or more/.test(e.why), '…that names the reason and what DAB needs there');
  ok(!!e && !/cannot reach a DAB multiplex/.test(e.why), '…not "cannot reach a DAB multiplex" (the receiver can)');
  ok(/\[DAB\] ON refused: this receiver is a locked range shared/.test(text), '★ and the refusal is in the server log');
  ok(!after.some((m) => m.type === 'dab' && m.channel !== undefined), 'nothing claims DAB is on'); }
console.log('── a locked range for ONE listener ──');
{ const { after, text } = await trial(1);
  ok(!after.some((m) => m.type === 'dab_error'), 'no refusal');
  ok(/\[DAB\] mode ON: channel 10D/.test(text), `★ DAB is honoured as before (the range suspended): ${(text.match(/\[DAB\] mode ON[^\n]*/) || ['—'])[0]}`); }
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
