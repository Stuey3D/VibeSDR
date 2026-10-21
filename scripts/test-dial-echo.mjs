#!/usr/bin/env node
// test-dial-echo.mjs — THE SHARED-DIAL CONTRACT, end to end: when one listener tunes, NOBODY ELSE SENDS A `tune`.
//
// ★★★ The Lenovo (RSP1A, shared dial, ask before tuning), 2026-10-22 00:07–00:10, the server's own log:
//       tune -> 666.000 kHz asked by session [8f30…]   (Safari, tuning)
//       tune -> 666.000 kHz asked by session [574d…]   (Stuart's V12 iPhone — nobody touched it)
//       tune -> 693.000 kHz asked by session [8f30…]
//       tune -> 684.000 kHz asked by session [574d…]   ← a step BEHIND: the radio dragged back under the tuner
//     The iPhone's LocalAudioPlayer forwarded every change of the screen's frequency — including the screen
//     ADOPTING the room's dial — as a `tune` on its audio socket. Fixed by src/services/audioTuneForward.ts.
//
//   A  — a raw client (the person tuning): ten 100 kHz steps, 150 ms apart.
//   B  — the SERVED WEB PAGE in a real headless, MUTED browser, just listening. Must send no `tune`.
//   C  — the APP's audio-socket tune door, driven by its real rule (audioTuneForward.ts) over real sockets: it
//        adopts each config the way VibeServerWsClient does and "renders" it ~120 ms late, as a phone does.
//        1. with the OLD rule (the seal alone) — the echo is REPRODUCED from the server's log;
//        2. with the NEW rule — the server hears nothing from C, and the dial ends where A left it;
//        3. C's own person still tunes: a step, and a step straight back inside the round trip, both go.
// ★ SILENT: --mute-audio and --disable-audio-output on the browser; the raw sockets play nothing.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-dial-echo.mjs   (exit 3 = not run)
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { shouldForwardTune } from '../src/services/audioTuneForward.ts';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };

/** A minimal WebSocket client (the probes' own). `onText` sees every text frame. */
function ws(port, pathq, onText) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [] };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x1) { const t = p.toString(); st.txt.push(t); if (st.txt.length > 400) st.txt.splice(0, 200); onText?.(t); }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}

/**
 * THE APP'S AUDIO-SOCKET TUNE DOOR, over real sockets. The screen side is VibeServerWsClient's config adopt
 * (status.frequency/mode ← vfo/mode, status.serverDial ← the server's word, in the same step); the effect side
 * is LocalAudioPlayer's, called with the snapshot a lagging render would hold. `rule` is 'old' (the seal alone,
 * as shipped) or 'new' (shouldForwardTune).
 */
function appClient(port, sid, rule) {
  const lagMs = 120;
  const status = { frequency: 0, mode: 'wfm', bandwidthLow: -100000, bandwidthHigh: 100000, serverDial: null };
  const tj = (s) => JSON.stringify({ type: 'tune', frequency: s.frequency, mode: s.mode, bandwidthLow: s.bandwidthLow, bandwidthHigh: s.bandwidthHigh });
  let hello = '', sealed = true, seq = 0, lastSeq = 0, lastSendAt = -Infinity, lastDeps = '', sent = 0, lastLocalTuneAt = -Infinity;
  const aud = ws(port, `/ws/audio?user_session_id=${sid}&codec=opus`);
  const effect = (snap) => {
    const deps = `${snap.frequency}|${snap.mode}|${snap.bandwidthLow}|${snap.bandwidthHigh}|${seq}`;
    if (deps === lastDeps) return;                      // React: the deps did not change, the effect does not run
    lastDeps = deps;
    if (!hello) { hello = tj(snap); lastSeq = seq; return; }   // the socket's opening position (assertTune false: unsent)
    const seqAdvanced = seq !== lastSeq; lastSeq = seq;
    const askedJson = tj(snap);
    let send;
    if (rule === 'old') {
      if (sealed) { if (!seqAdvanced && askedJson === hello) return; sealed = false; }
      send = true;
    } else {
      const d = shouldForwardTune({ asked: snap, serverDial: snap.serverDial, seqAdvanced, sealed, helloTune: hello,
                                    askedJson, msSinceSend: Date.now() - lastSendAt });
      if (d.unseal) sealed = false;
      send = d.send;
    }
    if (!send) return;
    lastSendAt = Date.now();
    sent++;
    aud.send(JSON.parse(askedJson));
  };
  const render = () => { const snap = JSON.parse(JSON.stringify(status)); setTimeout(() => effect(snap), lagMs); };
  const sp = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`, (t) => {
    if (!t.startsWith('{"type":"config"')) return;
    const m = JSON.parse(t);
    const sv = Number(m.vfo);
    if (!(sv > 0)) return;
    status.serverDial = { frequency: sv, ...(typeof m.mode === 'string' && m.mode ? { mode: m.mode } : {}) };
    const settled = Date.now() - lastLocalTuneAt > 1500;
    if (settled && Math.abs(sv - status.frequency) > 100) {
      status.frequency = sv;
      if (typeof m.mode === 'string' && m.mode) status.mode = m.mode;
    }
    render();
  });
  return {
    get sent() { return sent; },
    /** A PERSON on this phone: the drum (no counter bump), or a counted site (bump). */
    personTune(hz, bump) { lastLocalTuneAt = Date.now(); status.frequency = hz; if (bump) seq++; render(); },
    close() { sp.close(); aud.close(); },
  };
}

const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dial-echo-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, 'config.json'), JSON.stringify({ configured: true, name: 'Dial echo test', locator: 'IO92nh',
  adminPass: 'dial-echo-admin-pass', radios: [{ serial: 'dialecho', label: 'T', enabled: true, configured: true, users: 5 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--wfm', '100.0'], { stdio: 'ignore' });
await sleep(400);
const logPath = path.join(T, 'server.log');
const logFd = fs.openSync(logPath, 'w');
// ★ --users 5 with no locked centre = ONE SHARED DIAL.
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '100000000', '--mode', 'wfm',
                        '--users', '5', '--idle-grace', '0'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, 'config.json'), VIBESERVER_DATA_DIR: path.join(T, 'data'),
         VIBESERVER_RUNTIME_DIR: path.join(T, 'run'), HOME: T },
  stdio: ['ignore', logFd, logFd] });
const log = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
/** Every `tune` the server heard since `mark`, as [kHz, session]. */
const tunes = (mark) => [...log().slice(mark).matchAll(/tune -> ([\d.]+) kHz asked by session \[([^\]]*)\]/g)].map((m) => [m[1], m[2]]);
const by = (mark, sid) => tunes(mark).filter((t) => t[1] === sid);
let br = null;
async function cleanup() { for (const p of [br, srv, rtl]) { try { p?.kill(); } catch {} } await sleep(500); fs.rmSync(T, { recursive: true, force: true }); }

const SID_A = 'aaaaaaaa-0000-4000-8000-00000000000a';
const SID_C1 = 'cccccccc-0000-4000-8000-0000000000c1';
const SID_C2 = 'cccccccc-0000-4000-8000-0000000000c2';
/** A walks the dial: ten 100 kHz steps, 150 ms apart — a person holding the step key. Returns the last Hz. */
async function walk(A, fromHz, dir) {
  let hz = fromHz;
  for (let k = 0; k < 10; k++) { hz += dir * 100000; A.send({ type: 'tune', frequency: hz, mode: 'wfm' }); await sleep(150); }
  await sleep(1200);
  return hz;
}
const lastVfo = (st) => { for (let i = st.txt.length - 1; i >= 0; i--) { const t = st.txt[i]; if (t.startsWith('{"type":"config"')) { const v = JSON.parse(t).vfo; if (v > 0) return v; } } return 0; };

try {
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  await sleep(500);

  const audA = ws(port, `/ws/audio?user_session_id=${SID_A}&codec=opus`);
  await sleep(400);
  const A = ws(port, `/ws/user-spectrum?user_session_id=${SID_A}&bins=256`);
  await sleep(1200);

  // ── B: the served web page, listening ──
  let cws = null, js = null;
  if (!fs.existsSync(BROWSER)) console.log('   (no Edge at ' + BROWSER + ' — the web page half not run)');
  else {
    br = spawn(BROWSER, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions',
      '--autoplay-policy=no-user-gesture-required', `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`,
      '--window-size=1200,800', 'about:blank'], { stdio: 'ignore' });
    let target;
    for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
    cws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((r) => (cws.onopen = r));
    let id = 0; const pending = new Map();
    cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
    const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); });
    js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
    const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(150); } return false; };
    const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
    // ★ Every `tune` the page sends — wrapped before the page's own code runs.
    await call('Runtime.enable'); await call('Page.enable');
    await call('Page.addScriptToEvaluateOnNewDocument', { source: `
      window.__tuneSent = [];
      const o = WebSocket.prototype.send;
      WebSocket.prototype.send = function (d) {
        if (typeof d === 'string' && /"type":"tune"/.test(d)) window.__tuneSent.push(d);
        return o.call(this, d);
      };` });
    await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await until(`!!document.getElementById("btnConnect")`, 20000);
    await js(`document.getElementById("btnConnect").click(); 1`);
    await until(`!!document.getElementById("audioGate")`, 20000);
    for (let i = 0; i < 6; i++) {
      const at = await js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null;
        const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
      if (!at) break;
      await tap(at[0], at[1]);
      if (await until(`!document.getElementById("audioGate")`, 3000)) break;
    }
    const rx = await until(`parseFloat(document.getElementById("freqInput").value) > 0`, 40000);
    ok(rx, 'B (the web page) is receiving');
    if (!rx) console.log('     page says: ' + JSON.stringify(await js(`[document.getElementById("freqInput")?.value, (document.body.innerText || "").slice(0, 300)]`)));
    await sleep(1500);
    await js(`window.__tuneSent.length = 0; 1`);
  }

  // ── 1. THE OLD RULE: the echo, reproduced ──
  console.log('── 1. the app\'s audio socket with the OLD rule (the seal alone) ──');
  {
    const C = appClient(port, SID_C1, 'old');
    await sleep(2000);
    const mark = log().length;
    const end = await walk(A, 100000000, -1);
    const echoes = by(mark, SID_C1);
    ok(echoes.length >= 5, `REPRODUCED: while A tuned, C sent ${echoes.length} tunes back (${echoes.map((t) => t[0]).join(' ')})`);
    const aSeq = by(mark, SID_A).map((t) => t[0]);
    const all = tunes(mark);
    const behind = all.some((t, i) => t[1] === SID_C1 && i > 0 && aSeq.indexOf(t[0]) >= 0 && aSeq.indexOf(t[0]) < aSeq.indexOf(all.slice(0, i).filter((x) => x[1] === SID_A).at(-1)?.[0]));
    console.log(`     (${behind ? 'and at least one a step BEHIND the tuner — the drag-back' : 'none behind this run — timing'}; A ended on ${end / 1e3} kHz)`);
    C.close();
    await sleep(1500);
  }

  // ── 2. THE NEW RULE: nothing ──
  console.log('── 2. the app\'s audio socket with the NEW rule (audioTuneForward.ts) ──');
  {
    const C = appClient(port, SID_C2, 'new');
    await sleep(2000);
    if (js) await js(`window.__tuneSent.length = 0; 1`);
    const mark = log().length;
    let end = await walk(A, 99000000, +1);
    end = await walk(A, end, -1);
    ok(by(mark, SID_C2).length === 0, `C sent NOTHING while A tuned twenty steps (heard from C: ${by(mark, SID_C2).map((t) => t[0]).join(' ') || 'nothing'})`);
    ok(lastVfo(A) === end, `the dial ended where A left it: ${lastVfo(A) / 1e3} kHz (wanted ${end / 1e3})`);
    if (js) {
      const sent = await js(`window.__tuneSent.slice()`);
      ok(sent.length === 0, `B (the web page) sent no tune either (${sent.length ? sent.slice(0, 3).join(' | ') : 'nothing'})`);
      ok(await js(`Math.round(parseFloat(document.getElementById("freqInput").value) * 1000)`) === Math.round(end / 1000),
         `…and B's readout followed the dial to ${end / 1e6} MHz`);
    }
    // ── 3. C's own person still tunes ──
    console.log('── 3. a person on the app still tunes ──');
    await sleep(1500);
    const m3 = log().length;
    C.personTune(end + 200000, false);   // the drum — no counter bump
    await sleep(800);
    ok(by(m3, SID_C2).map((t) => t[0]).join(' ') === ((end + 200000) / 1e3).toFixed(3),
       `the drum (no counter bump) reaches the radio: ${by(m3, SID_C2).map((t) => t[0]).join(' ') || 'nothing'}`);
    await sleep(2000);
    const m4 = log().length;
    const here = end + 200000;
    C.personTune(here + 100000, false);
    await sleep(40);
    C.personTune(here, false);           // straight back, inside the round trip
    await sleep(1200);
    const back = by(m4, SID_C2).map((t) => t[0]);
    ok(back.length === 2 && back[1] === (here / 1e3).toFixed(3), `a step and straight back: both go, the radio ends on ${here / 1e3} (${back.join(' ')})`);
    ok(lastVfo(A) === here, `…the server agrees (${lastVfo(A) / 1e3} kHz)`);
    await sleep(2000);
    const m5 = log().length;
    C.personTune(here, true);            // a counted re-assert of the SAME frequency (a restore)
    await sleep(800);
    ok(by(m5, SID_C2).length === 1, 'a counted re-assert of the same frequency still goes (the restore)');
    C.close();
  }
  if (cws) cws.close();
  A.close(); audA.close();
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : '\n\x1b[32mall passed\x1b[0m');
process.exit(fails ? 1 : 0);
