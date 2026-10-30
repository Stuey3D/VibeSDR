// test-rsp-vagc-control.mjs — the REAL server: the radio menu's AGC choice for an SDRplay (2026-10-30).
//
// Stuart, on his unlocked Lenovo RSP1A with VibeAGC running: "i assume this isnt vibeagc" / "no option for it here
// either". The menu's AGC row sends rsp_control {vagcmode: 0 auto | 1 VibeAGC | 2 the SDRplay's own}. This proves the
// SERVER's half on a fake rtl_tcp radio — the gate and the save are radio-agnostic, so no RSP is needed:
//   · a listener who has not unlocked is REFUSED (adminGate) — nothing applied, nothing saved;
//   · the owner (unlocked over the socket, the menu's own path) is APPLIED LIVE — the shim's setter logs the change at
//     once, no restart — and SAVED to this radio's entry as rspVibeAgc, both ways, and clamped to 0..2;
//   · a restart keeps it (the setter is given the saved value at start-up);
//   · and rfAgc, which the menu's RF AGC key has persisted since 09-11, is now folded into the radio's entry too.
// ★ The take-over / hand-back on a running RSP is the heartbeat's (vsRspVagcTakeOver / the stop branch) and needs the
//   radio; it is exercised on hardware, not here.
// ★ The admin password is this test's own fixture, in a throwaway config. Needs VIBESERVER_BIN; SILENT (no audio).
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const ADMIN = 'vagc-Test-Pass-47';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-vagc-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const CFG = path.join(dir, 'config.json');
fs.writeFileSync(CFG, JSON.stringify({ configured: true, name: 'VibeAGC control test', adminPass: ADMIN,
  radios: [{ serial: 'vagc-test', label: 'Test radio', enabled: true, configured: true, users: 3 }] }));
const radioCfg = () => JSON.parse(fs.readFileSync(CFG, 'utf8')).radios.find((r) => r.serial === 'vagc-test') || {};
const LOG = path.join(dir, 'server.log');
const logText = () => fs.readFileSync(LOG, 'utf8');

const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: CFG, VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
delete env.VIBE_RSP_VIBEAGC;   // ★ the bench override would mask the owner's word
let srv = null;
const start = async () => {
  const log = fs.openSync(LOG, 'a');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm'],
              { env, stdio: ['ignore', log, log] });
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  await sleep(3000);   // ★ the admin handlers register a few seconds after the port opens
};
const stop = () => new Promise((r) => { if (!srv) return r(); srv.once('exit', () => r()); srv.kill('SIGTERM'); });
const join = (sid) => new Promise((res, rej) => {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256`);
  const got = [];
  ws.on('message', (d) => { const s = String(d); if (s[0] === '{') { try { got.push(JSON.parse(s)); } catch {} } });
  ws.on('open', () => res({ ws, got, send: (o) => ws.send(JSON.stringify(o)), close: () => ws.close() }));
  ws.on('error', rej);
});
const until = async (f, ms = 3000) => { const t = Date.now(); while (Date.now() - t < ms) { if (f()) return true; await sleep(50); } return !!f(); };

try {
  await start();
  const base = `http://127.0.0.1:${port}`;

  console.log('── a listener who has not unlocked: refused ──');
  const L = await join('lstn0001');
  await sleep(500);
  L.send({ type: 'rsp_control', vagcmode: 1 });
  ok(await until(() => /refused the AGC choice/.test(logText())), '★ refused by adminGate ("refused the AGC choice")');
  await sleep(300);
  ok(!/the owner chose/.test(logText()) && !/owner setting ON/.test(logText()), '…nothing applied');
  ok(radioCfg().rspVibeAgc === undefined || radioCfg().rspVibeAgc === 0, `…nothing saved (rspVibeAgc ${radioCfg().rspVibeAgc})`);
  ok(await until(() => L.got.some((m) => m.type === 'admin' && m.refused === true)), '…and the listener is told (admin refused)');

  console.log('── the owner, unlocked over the socket: applied live, saved, both ways ──');
  const A = await join('ownr0002');
  const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
  A.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex') });
  ok(await until(() => A.got.some((m) => m.type === 'admin' && m.ok === true)), 'unlocked');
  A.send({ type: 'rsp_control', vagcmode: 1 });
  ok(await until(() => /owner setting ON/.test(logText())), '★ VibeAGC: applied at once — the shim\'s setter took it, no restart');
  ok(await until(() => radioCfg().rspVibeAgc === 1), `★ …and saved to this radio's entry (rspVibeAgc ${radioCfg().rspVibeAgc})`);
  A.send({ type: 'rsp_control', vagcmode: 2 });
  ok(await until(() => /owner setting OFF/.test(logText())), 'SDRPLAY: applied');
  ok(await until(() => radioCfg().rspVibeAgc === 2), `…saved (${radioCfg().rspVibeAgc})`);
  A.send({ type: 'rsp_control', vagcmode: 0 });
  ok(await until(() => /owner setting automatic/.test(logText())), 'AUTO: applied');
  ok(await until(() => radioCfg().rspVibeAgc === 0), `…saved (${radioCfg().rspVibeAgc})`);
  A.send({ type: 'rsp_control', vagcmode: 9 });
  ok(await until(() => radioCfg().rspVibeAgc === 2), `out of range is clamped to 0..2 (${radioCfg().rspVibeAgc})`);
  A.send({ type: 'rsp_control', rfagc: 1 });
  ok(await until(() => radioCfg().rfAgc === true), `rfAgc from the menu is now folded into the radio's entry (${radioCfg().rfAgc})`);
  ok(radioCfg().rspVibeAgc === 2, '…without disturbing the AGC choice');
  const fileNow = JSON.parse(fs.readFileSync(CFG, 'utf8'));
  ok(fileNow.adminPass === ADMIN && fileNow.radios.length === 1, 'the rest of the file is untouched (password, radio list)');
  L.close(); A.close();

  console.log('── a restart keeps it ──');
  await stop();
  fs.appendFileSync(LOG, '\n=== RESTART ===\n');
  await start();
  const after = logText().split('=== RESTART ===')[1] || '';
  ok(/VibeAGC \(RSP\): owner setting OFF/.test(after), '★ the saved SDRPLAY choice is given to the shim at start-up');
} finally {
  await stop(); rtl.kill('SIGTERM');
}
console.log(`\n   ${pass} passed, ${fail} failed`);
if (fail) console.log(`   log: ${LOG}`);
process.exit(fail ? 1 : 0);
