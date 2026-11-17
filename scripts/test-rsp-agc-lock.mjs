// test-rsp-agc-lock.mjs — the REAL server: the owner's AGC lock holds an SDRplay's RF AGC too (Stuart, 2026-11-17).
//
// "there is no way of locking the RF gain on SDRPlay devices with VibeAGC … when i returned to the RSP1A on the lenovo
//  someone had disabled VibeAGC on the RF and changed the gain themselves". The lock (agcLock 1) covered only the IF AGC.
// Now, with it on, a listener who has not unlocked is refused — and told — for:
//   · the RF AGC off (rsp_control rfagc 0), · a manual RF gain (lna), · a manual IF gain (ifgr);
// turning an AGC ON is allowed; the signed-in admin keeps every control; without the lock nothing is refused.
// rsp_control's gate is radio-agnostic, so a fake rtl_tcp radio carries it (as test-rsp-vagc-control.mjs).
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

const ADMIN = 'agclock-Test-Pass-31';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-agclock-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const CFG = path.join(dir, 'config.json');
const writeCfg = (agcLock, extra = {}) => fs.writeFileSync(CFG, JSON.stringify({ configured: true, name: 'AGC lock test', adminPass: ADMIN,
  radios: [{ serial: 'agclock-test', label: 'Test radio', enabled: true, configured: true, users: 3, agcLock, rfAgc: true, ...extra }] }));
writeCfg(1);
const radioCfg = () => JSON.parse(fs.readFileSync(CFG, 'utf8')).radios.find((r) => r.serial === 'agclock-test') || {};
const LOG = path.join(dir, 'server.log');
const logText = () => fs.readFileSync(LOG, 'utf8');
const count = (re) => (logText().match(re) || []).length;

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
  ok(/AGC lock: ON/.test(logText()), 'the lock is on at start-up');

  console.log('── locked: a listener ──');
  const L = await join('lstn0001');
  await sleep(500);
  L.send({ type: 'rsp_control', rfagc: 0 });
  ok(await until(() => /RSP RF AGC off refused/.test(logText())), '★ the RF AGC off: refused');
  ok(await until(() => L.got.some((m) => m.type === 'notice' && /locked the AGC on/.test(m.why || ''))), '…and the listener is told why');
  ok(!/RSP RF AGC off by a listener/.test(logText()) && radioCfg().rfAgc === true, `…nothing applied or saved (rfAgc ${radioCfg().rfAgc})`);
  L.send({ type: 'rsp_control', lna: 3 });
  ok(await until(() => count(/RSP manual gain refused/g) >= 1), '★ a manual RF gain (lna): refused');
  ok(radioCfg().lnaState !== 3, `…not saved (lnaState ${radioCfg().lnaState})`);
  L.send({ type: 'rsp_control', ifgr: 40 });
  ok(await until(() => count(/RSP manual gain refused/g) >= 2), '★ a manual IF gain (ifgr): refused');
  ok(radioCfg().ifGr !== 40, `…not saved (ifGr ${radioCfg().ifGr})`);
  L.send({ type: 'rsp_control', rfagc: 1 });
  ok(await until(() => /RSP RF AGC ON by a listener/.test(logText())), 'turning the RF AGC ON is allowed');

  console.log('── locked: the signed-in admin keeps every control ──');
  const A = await join('ownr0002');
  const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
  A.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex') });
  ok(await until(() => A.got.some((m) => m.type === 'admin' && m.ok === true)), 'unlocked');
  A.send({ type: 'rsp_control', rfagc: 0 });
  ok(await until(() => /RSP RF AGC off by a listener/.test(logText())), '★ the admin may switch the RF AGC off');
  A.send({ type: 'rsp_control', lna: 2 });
  ok(await until(() => radioCfg().lnaState === 2), `★ …and set the RF gain (lnaState ${radioCfg().lnaState})`);
  A.send({ type: 'rsp_control', ifagc: 0, ifgr: 45 });
  ok(await until(() => radioCfg().ifGr === 45 && radioCfg().ifAgc === 0), `★ …and switch the IF AGC off and set the IF gain (ifAgc ${radioCfg().ifAgc}, ifGr ${radioCfg().ifGr})`);
  await sleep(5000);
  ok(!/the RF AGC is back on/.test(logText()), '★ while the admin is signed in, their RF AGC off stands (nothing puts it back)');
  A.close();
  ok(await until(() => /AGC lock: no admin signed in — the RF AGC is back on/.test(logText()), 8000),
     '★ the admin leaves: the RF AGC is back on for listeners (so it really was off)');
  L.close();

  console.log('── a fixed RF gain on "All bands": the IF stays the listener\'s ──');
  await stop();
  writeCfg(0, { gainLimits: 'all:5', gainLocks: 'all:1' });
  fs.appendFileSync(LOG, '\n=== BAND ===\n');
  await start();
  const band = () => logText().split('=== BAND ===')[1] || '';
  const B = await join('lstn0004');
  await sleep(500);
  B.send({ type: 'rsp_control', lna: 1 });
  ok(await until(() => /RSP RF gain refused — the owner has fixed it on this band/.test(band())), '★ a manual RF gain: refused');
  ok(await until(() => B.got.some((m) => m.type === 'notice' && /fixed the RF gain on this band/.test(m.why || ''))), '…and the listener is told the IF is still theirs');
  ok(radioCfg().lnaState !== 1, `…not applied (lnaState ${radioCfg().lnaState})`);
  B.send({ type: 'rsp_control', ifagc: 0, ifgr: 38 });
  ok(await until(() => radioCfg().ifGr === 38 && radioCfg().ifAgc === 0), `★ the IF AGC off and a manual IF gain: allowed (ifAgc ${radioCfg().ifAgc}, ifGr ${radioCfg().ifGr})`);
  B.send({ type: 'rsp_control', ifagc: 1 });
  ok(await until(() => radioCfg().ifAgc === 1), '…and the IF AGC back on');
  B.close();

  console.log('── a locked band WITH an IF figure: the IF is fixed too ──');
  await stop();
  writeCfg(0, { gainLimits: 'all:5', gainLocks: 'all:1', ifGrLimits: 'all:30' });
  fs.appendFileSync(LOG, '\n=== BANDIF ===\n');
  await start();
  const bandIf = () => logText().split('=== BANDIF ===')[1] || '';
  const C = await join('lstn0005');
  await sleep(500);
  C.send({ type: 'rsp_control', ifgr: 44 });
  ok(await until(() => /RSP gain refused — the owner has fixed it on this band/.test(bandIf())), '★ a manual IF gain: refused');
  ok(radioCfg().ifGr !== 44, `…not applied (ifGr ${radioCfg().ifGr})`);
  C.close();

  console.log('── unlocked: nothing refused ──');
  await stop();
  writeCfg(0);
  fs.appendFileSync(LOG, '\n=== RESTART ===\n');
  await start();
  const L2 = await join('lstn0003');
  await sleep(500);
  L2.send({ type: 'rsp_control', rfagc: 0 });
  ok(await until(() => /RSP RF AGC off by a listener/.test(logText().split('=== RESTART ===')[1] || '')), 'the RF AGC off: applied');
  ok(!/refused — the owner has locked the AGC on/.test(logText().split('=== RESTART ===')[1] || ''), '…no refusal');
  L2.close();
} finally {
  await stop(); rtl.kill('SIGTERM');
}
console.log(`\n   ${pass} passed, ${fail} failed`);
if (fail) console.log(`   log: ${LOG}`);
else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
