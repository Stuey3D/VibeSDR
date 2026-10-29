// test-setup-dab-faad2.mjs — THE DAB+ DECODER CHOICE IS GONE, AND AN OLD CONFIG THAT CARRIES IT STILL LOADS (V12, 2026-10-29).
// ★ History: the setup page offered "DAB+ audio decoder" (FAAD2 or FFmpeg — config dabAacDecoder, before that the
//   dabFaad2 bool), and this test proved the owner's choice survived the radio persisting its own state. FFmpeg is no
//   longer part of VibeServer: FAAD2 is the Linux and Mac servers' only DAB+ decoder, so a choice of one would be a dead
//   control and it was removed (AGENTS.md).
// This drives the real setup page in headless MUTED Edge against the Pi's layout (a front door + the radio's process on a
// fake rtl_tcp, started the way systemd starts vibeserver@<serial>), from a config.json written by the old version —
// "dabAacDecoder": 2 and "dabFaad2": true on the radio and at the top level:
//   both processes start → the page draws no decoder choice → hwinfo carries none of the old fields → a listener enters
//   DAB → the radio is saved through the page, then persists its own state (the DAB block, as the last listener leaves) →
//   the radio restarts cleanly. ★ The old keys stay in the file UNTOUCHED: config.json keeps every key this version does
//   not know (vibeserver_config.cpp keepUnknown — so a downgrade keeps its settings); this version ignores them.
// ★ SILENT: no audio is played (--mute-audio, fake radio, nothing decoded). Exit 3 = not run.
//   usage: VIBESERVER_BIN=… node scripts/test-setup-dab-faad2.mjs
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

const ADMIN = 'faad2-admin-pass', SERIAL = 'faad-rtl';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-faad2-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
let P = 0;
for (let i = 0; i < 20 && !P; i++) {
  const p = await freePort();
  const free = await Promise.all([p + 1].map((q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); })));
  if (free.every(Boolean)) P = p;
}
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({
  configured: true, fullMode: true, name: 'DAB+ decoder test', adminPass: ADMIN, port: P,
  dabAacDecoder: 2, dabFaad2: true,   // ★ written by the old version — must load cleanly and be ignored
  radios: [{ serial: SERIAL, driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true, users: 4, rate: 2048000,
             freq: 100000000, demodMode: 'wfm', dabAacDecoder: 2, dabFaad2: true }],
}));
// ★★ EVERY PORT DISTINCT. freePort() can hand back P+1 (the radio's own port) — and a radio told to read rtl_tcp from its
//    own HTTP port feeds on itself (the first run of this test grew to 17 GB before it was killed).
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
/** ★ The old decoder keys on the radio's entry in config.json, as "dabAacDecoder=2,dabFaad2=true" (kept as unknown keys). */
const oldKeys = () => { const r = JSON.parse(fs.readFileSync(cfgPath, 'utf8')).radios.find((x) => x.serial === SERIAL) || {};
                        return ['dabAacDecoder', 'dabFaad2'].filter((k) => k in r).map((k) => `${k}=${r[k]}`).join(','); };
const OLD = 'dabAacDecoder=2,dabFaad2=true';

/** A WebSocket listener on the radio — enough to enter and leave DAB. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1'); const st = { texts: [] }; let buf = Buffer.alloc(0), upg = false;
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (buf.length > (8 << 20)) { buf = Buffer.alloc(0); return; }   // ★ bounded: never hold more than one big frame
    if (!upg) { const e = buf.indexOf('\r\n\r\n'); if (e < 0) return; buf = buf.subarray(e + 4); upg = true; }
    for (;;) {
      if (buf.length < 2) return; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; } else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) return; const op = buf[0] & 0x0f, body = buf.subarray(off, off + len);
      if (op === 1) { st.texts.push(body.toString()); if (st.texts.length > 50) st.texts.shift(); }   // ★ bounded else if (op === 9) { try { s.write(Buffer.concat([Buffer.from([0x8a, 0x80 | body.length, 0, 0, 0, 0]), body])); } catch {} }
      buf = buf.subarray(off + len);
    }
  });
  return { st, send(o) { const b = Buffer.from(JSON.stringify(o)); const h = Buffer.alloc(b.length < 126 ? 6 : 8); h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); } try { s.write(Buffer.concat([h, b])); } catch {} },
    close() { try { s.destroy(); } catch {} } };
}

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
const deadline = setTimeout(() => abort('the 180 s deadline passed'), 180000);

let call, js;
async function openSetup() {
  await call('Page.navigate', { url: `http://127.0.0.1:${P}/setup` });
  ok(await until(async () => await js(`(() => { const c = document.getElementById("signinCard"); return !!c && c.offsetParent !== null; })()`), 10000), 'the setup page asks to sign in');
  await js(`document.getElementById("pass").value = ${JSON.stringify(ADMIN)}; document.getElementById("signinBtn").click(); 1`);
  // ★ open the radio's tab, as the owner does — its hardware (and so the switch) comes from the radio's own process
  await until(async () => await js(`document.querySelectorAll("#radioTabs button").length > 0`), 10000);
  await js(`(() => { const b = [...document.querySelectorAll("#radioTabs button")].find(x => /RTL/.test(x.textContent)); b && b.click(); return 1; })()`);
  ok(await until(async () => await js(`!!document.getElementById("dabScanLabels")`), 15000), 'signed in: the radio\'s settings are drawn');
  ok(await js(`!document.getElementById("dabAacDecoder") && !document.getElementById("dabFaad2Box")`) === true,
     'no "DAB+ audio decoder" choice on the page (FAAD2 is the only one)');
}
async function saveRadio() {
  const visible = await js(`(() => { const b = document.getElementById("saveRadioBtn"); return !!b && !b.classList.contains("hide") && !b.disabled; })()`);
  const before = fs.statSync(cfgPath).mtimeMs;
  await js(`document.getElementById(${JSON.stringify(visible ? 'saveRadioBtn' : 'saveBtn')}).click(); 1`);
  ok(await until(() => fs.statSync(cfgPath).mtimeMs > before, 10000), `saved through the page (${visible ? 'Save radio settings' : 'Save and start'})`);
  await sleep(1500);
}

try {
  ok(await up(P), `the front door is up (${P})`);
  startRadio();
  ok(await up(P + 1), `the radio's own process is up (${P + 1})`);
  let t; for (let i = 0; i < 60 && !t; i++) { try { t = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find((x) => x.type === 'page'); } catch { await sleep(250); } }
  const cws = new WebSocket(t.webSocketDebuggerUrl); await new Promise((r) => { cws.onopen = r; });
  let id = 0; const pend = new Map(); cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  // ★ every browser call has its own 10 s limit (a dead browser must fail the test, not hang it)
  call = (method, params = {}) => new Promise((r) => { const n = ++id; pend.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); setTimeout(() => { if (pend.delete(n)) r({}); }, 10000); });
  js = async (e) => (await call('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await call('Runtime.enable');

  ok(!/dabAacDecoder|dabFaad2|ffmpeg/i.test(R()), 'the radio started from the old config without a word about the old keys or FFmpeg');
  const hw = await (await fetch(`http://127.0.0.1:${P + 1}/vibeserver/hardware`)).text().catch(() => '');
  ok(/"dabHw"/.test(hw) && !/dabFaad2|dabAacDecoder/.test(hw), 'hwinfo carries none of the old decoder fields');

  await openSetup();
  // ★ a listener goes into DAB on the radio
  const SID = crypto.randomUUID();
  const audio = ws(P + 1, `/ws/audio?user_session_id=${SID}&codec=opus`);
  await sleep(600);
  const spec = ws(P + 1, `/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=256`);
  await sleep(1500);
  spec.send({ type: 'dab', on: 1, channel: '12B' });
  ok(await until(() => /\[DAB\] mode ON/.test(R()), 8000), 'a listener is in DAB on the radio');
  // ★ the old WebSocket message is gone too: sent anyway, it changes nothing and breaks nothing
  spec.send({ type: 'dabAacDecoder', v: 2 }); spec.send({ type: 'dabFaad2', on: 0 });

  await saveRadio();
  ok(oldKeys() === OLD, `★ saving through the page leaves the old keys exactly as written, unused (${oldKeys() || 'none'})`);
  const savedAt = fs.statSync(cfgPath).mtimeMs;
  // ★★ the radio persists on its own: the listener leaves DAB, and then leaves altogether (the block is remembered)
  spec.send({ type: 'dab', on: 0 });
  await sleep(1500);
  audio.close(); spec.close();
  await sleep(4000);
  ok(fs.statSync(cfgPath).mtimeMs > savedAt && /remembering block|dabChannel/.test(R() + fs.readFileSync(cfgPath, 'utf8')),
     'the radio persisted its own state after the save');
  ok(oldKeys() === OLD, `★★ and after the radio's own write they are still exactly as written, unused (${oldKeys() || 'none'})`);
  ok(!/dabAacDecoder|dabFaad2|ffmpeg/i.test(R()), 'the radio never acted on them or mentioned FFmpeg');
  await stopRadio(); startRadio();
  ok(await up(P + 1), 'the radio restarts cleanly');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  clearInterval(guard); clearTimeout(deadline);
  stopAll();
  await sleep(500);
}
console.log(`   memory peak: this test ${(peakSelf / 1048576).toFixed(0)} MB, largest child ${(peakChild / 1048576).toFixed(0)} MB`);
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs in ${dir})`); else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
