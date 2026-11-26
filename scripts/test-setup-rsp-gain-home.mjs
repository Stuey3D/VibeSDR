// test-setup-rsp-gain-home.mjs — an SDRplay's gain settings in ONE place (Stuart, 2026-11-17: "the gain locks for the
// SDRPlay need to be with the radio controls as right now there are multiple stages in the setup menu where the gain is
// setup"). The REAL setup page in headless Edge, on a real server:
//   · on an SDRplay tab, the gain card's body (#gainRows — limits, the AGC lock, the IF AGC target) sits under the radio's
//     own AGC rows (#rspGainHere, after "VibeAGC (SDRplay)"), and the separate "Gain limits" card is hidden;
//   · the lock reads "Lock the AGC on (RF and IF)" and still works where it now sits (radio().agcLock follows it);
//   · on an RTL tab the body is back in its own card, shown; and back on the SDRplay it is moved again, not lost.
// Needs VIBESERVER_BIN and Microsoft Edge; SILENT (--mute-audio, no audio).
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
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Microsoft Edge)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return !!(await f()); };

const ADMIN = 'rspgain-admin-pass', SERIAL = 'rspgain-rtl';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-rspgain-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
let P = 0;
for (let i = 0; i < 20 && !P; i++) {
  const p = await freePort();
  const free = await Promise.all([p + 1].map((q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); })));
  if (free.every(Boolean)) P = p;
}
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({
  configured: true, fullMode: true, name: 'RSP gain home test', adminPass: ADMIN, port: P,
  radios: [{ serial: SERIAL, driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true, users: 1, rate: 2400000,
             freq: 100000000, demodMode: 'wfm' },
           { serial: 'rspgain-rsp', driver: 'sdrplay', label: 'RSP1A', enabled: true, configured: true, users: 1, rate: 2000000,
             freq: 7100000, demodMode: 'lsb' }],
}));
const taken = new Set([P, P + 1]);
const distinctPort = async () => { for (;;) { const q = await freePort(); if (!taken.has(q)) { taken.add(q); return q; } } };
const rtlPort = await distinctPort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: cfgPath,
              VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir };
const doorLog = path.join(dir, 'door.log');
// ★ ONE descriptor for stdout and stderr: two (a 'w' and an 'a') overwrite each other's start-up lines
const doorFd = fs.openSync(doorLog, 'w');
const door = spawn(BIN, [], { env, stdio: ['ignore', doorFd, doorFd] });
let radio = null, radioN = 0, radioLog = '';
const startRadio = () => {
  radioLog = path.join(dir, `radio-${++radioN}.log`);
  const fd = fs.openSync(radioLog, 'w');
  radio = spawn(BIN, ['--radio-serial', SERIAL, '--tcp', `127.0.0.1:${rtlPort}`], { env, stdio: ['ignore', fd, fd] });
};
const stopRadio = async () => { if (!radio) return; radio.kill('SIGTERM'); for (let i = 0; i < 100 && radio.exitCode === null && radio.signalCode === null; i++) await sleep(50); if (radio.exitCode === null && radio.signalCode === null) radio.kill('SIGKILL'); radio = null; };
/** ★ The radio's log, BOUNDED: its first 64 KB (start-up lines) and last 192 KB — never the whole of a growing file. */
const R = () => {
  try {
    const fd = fs.openSync(radioLog, 'r'); const size = fs.fstatSync(fd).size;
    const head = Buffer.alloc(Math.min(size, 65536)); fs.readSync(fd, head, 0, head.length, 0);
    const tl = Math.min(Math.max(0, size - head.length), 196608); const tail = Buffer.alloc(tl); if (tl) fs.readSync(fd, tail, 0, tl, size - tl);
    fs.closeSync(fd); return head.toString() + '\n' + tail.toString();
  } catch { return ''; }
};
const up = async (port) => { for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };
const CDP = await distinctPort();
const br = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', `--remote-debugging-port=${CDP}`,
                        `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
const stopAll = () => { for (const p of [door, radio, rtl, br]) { try { p && p.kill('SIGKILL'); } catch {} } };
process.on('exit', stopAll);
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => { stopAll(); process.exit(1); });
/* ★★★ BOUNDED, WHATEVER GOES WRONG: a hard deadline for the whole test, and a memory watch over THIS process and every
 *  child (server, fake radio, browser) — over the limit, everything is killed and the test fails. */
let peakSelf = 0, peakChild = 0;
const abort = (why) => { console.log(`   FAIL aborted — ${why}`); stopAll(); process.exit(1); };
const guard = setInterval(() => {
  const rss = process.memoryUsage().rss; peakSelf = Math.max(peakSelf, rss);
  if (rss > (1 << 30)) abort(`the test itself reached ${(rss / 1048576).toFixed(0)} MB`);
  const pids = [door, radio, rtl, br].filter((p) => p && p.pid).map((p) => p.pid).join(',');
  const ps = spawn('ps', ['-o', 'rss=', '-p', pids]); let out = '';
  ps.stdout.on('data', (d) => { out += d; });
  ps.on('close', () => { const kb = Math.max(0, ...out.split(/\s+/).filter(Boolean).map(Number)); peakChild = Math.max(peakChild, kb * 1024);
                         if (kb > 2 * 1048576) abort(`a child process reached ${(kb / 1024).toFixed(0)} MB`); });
}, 1000);
const deadline = setTimeout(() => abort('the 240 s deadline passed'), 240000);

let call, js;
const tab = (re) => js(`(() => { const b = [...document.querySelectorAll("#radioTabs button")].find(x => ${re}.test(x.textContent)); if (!b) return false; b.click(); return true; })()`);
const where = () => js(`(() => { const g = document.getElementById("gainRows"); if (!g) return "gone";
  return g.parentElement && g.parentElement.id ? g.parentElement.id : "?"; })()`);
const cardShown = () => js(`!document.getElementById("gainCard").classList.contains("hide")`);

try {
  ok(await up(P), `the front door is up (${P})`);
  startRadio();
  ok(await up(P + 1), `the RTL's own process is up (${P + 1})`);
  let t; for (let i = 0; i < 60 && !t; i++) { try { t = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find((x) => x.type === 'page'); } catch { await sleep(250); } }
  const cws = new WebSocket(t.webSocketDebuggerUrl); await new Promise((r) => { cws.onopen = r; });
  let id = 0; const pend = new Map(); cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  call = (method, params = {}) => new Promise((r) => { const n = ++id; pend.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); setTimeout(() => { if (pend.delete(n)) r({}); }, 10000); });
  js = async (e) => (await call('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await call('Runtime.enable');
  await call('Page.navigate', { url: `http://127.0.0.1:${P}/setup` });
  ok(await until(async () => await js(`(() => { const c = document.getElementById("signinCard"); return !!c && c.offsetParent !== null; })()`), 10000), 'the setup page asks to sign in');
  await js(`document.getElementById("pass").value = ${JSON.stringify(ADMIN)}; document.getElementById("signinBtn").click(); 1`);
  ok(await until(async () => (await js(`document.querySelectorAll("#radioTabs button").length`)) >= 2, 10000), 'signed in: both radios have tabs');

  console.log('── the SDRplay ──');
  ok(await tab('/RSP1A/'), 'the RSP1A tab');
  ok(await until(async () => (await js(`document.getElementById("hw").innerHTML.length`)) > 0, 15000), "the radio's hardware block is drawn");
  // ★ beta96 b96-setupux: sections start closed — "visible there" is judged with this tab's sections open (OPEN ALL, as an owner would)
  ok(await js(`(() => { const b = document.getElementById("secOpenAll"); if (!b) return false; b.click(); return true; })()`), 'OPEN ALL on the RSP1A tab');
  ok(await until(async () => (await where()) === 'rspGainHere', 15000), `★ the gain settings sit under the radio's own AGC rows (${await where()})`);
  ok(await js(`(() => { const h = document.getElementById("rspGainHere"), v = document.getElementById("rspVibeAgc");
    return !!h && !!v && !!(v.compareDocumentPosition(h) & Node.DOCUMENT_POSITION_FOLLOWING) && !!document.getElementById("hw").contains(h); })()`),
    '…after "VibeAGC (SDRplay)", inside "This radio\'s hardware"');
  ok(!(await cardShown()), '★ …and the separate "Gain limits" card is hidden (one place only)');
  ok((await js(`document.querySelector("#gainAgcLockRow .lbl").textContent`)) === 'Lock the AGC on (RF and IF)', 'the lock says it holds both halves');
  ok(await js(`document.getElementById("gainAgcLockRow").offsetParent !== null`), '…and is visible there');
  await js(`(() => { const c = document.getElementById("gainAgcLock"); c.checked = true; c.dispatchEvent(new Event("change")); return 1; })()`);
  ok((await js(`radio().agcLock`)) === 1, '★ ticking it where it now sits still sets this radio\'s agcLock');
  ok(/both AGCs go back on/.test(await js(`document.querySelector("#gainAgcLockRow .note").textContent`) || ''), 'the note says the admin may take both gains and the AGCs come back on after');
  ok(await js(`!document.getElementById("gainLockRspNote").classList.contains("hide")`), 'the per-band lock says an RF figure alone leaves listeners the IF');

  console.log('── the RTL, then back ──');
  ok(await tab('/RTL/'), 'the RTL tab');
  ok(await until(async () => (await where()) === 'gainCard' && await cardShown(), 15000), `★ the gain settings are back in their own card, shown (${await where()})`);
  ok(await js(`radio().agcLock !== 1`), "…the SDRplay's lock is not the RTL's");
  ok(await tab('/RSP1A/'), 'the RSP1A tab again');
  ok(await until(async () => (await where()) === 'rspGainHere', 15000), `★ moved again, not lost with the redrawn block (${await where()})`);
  ok((await js(`radio().agcLock`)) === 1 && await js(`document.getElementById("gainAgcLock").checked`), '…and the lock is still ticked');
  ok((await js(`document.querySelectorAll("#gainRows").length`)) === 1, 'one copy of the gain settings on the page');
} finally {
  clearInterval(guard); clearTimeout(deadline);
  stopAll();
  await sleep(500);
}
console.log(`\n${fail ? '✗' : '✓'} test-setup-rsp-gain-home: ${pass} passed, ${fail} failed (peak RSS: test ${(peakSelf / 1048576).toFixed(0)} MB, children ${(peakChild / 1048576).toFixed(0)} MB)`);
if (fail) console.log(`   (logs in ${dir})`); else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
