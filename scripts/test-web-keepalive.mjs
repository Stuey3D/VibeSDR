// test-web-keepalive.mjs — ★★★ A HIDDEN, SILENT TAB KEEPS ITS SESSION (docs/v12/WEB-KEEPALIVE.md). Stuart's Pi 500, 2026-11-22:
//   behind Discord, muted by APRS's silent-decoder rule, the page froze and reconnected once a minute — the server logged
//   "no application message … for 30 s" because the page's 5 s ping ran on window.setInterval and the browser had
//   throttled a hidden, silent tab's timers to about once a minute.
//   The REAL server (fake rtl_tcp) and the BUILT page (the one compiled into the server) in headless Edge with --mute-audio
//   (tests are silent). Headless Edge does not throttle by itself, so the page is put in the state Safari puts it in:
//   document.hidden / visibilityState 'hidden' with a visibilitychange (the page's own 5 fps idle rate follows), and the
//   MAIN THREAD's window timers aligned to 60 s — at most one run a minute per interval, timeouts pushed to the next
//   minute, animation frames held — exactly the throttle WebKit's EnabledIncreasing reaches and Chromium's intensive
//   throttling applies. Worker timers are left alone, as both engines leave them.
//   Two runs of HIDE_S (default 100 s) each: 1. NFM with the speaker MUTED (the gain at 0 — the silent-decoder mute);
//   2. ISM, a mode with nothing to hear (the output held quiet — setQuiet). Each: the server never releases the session,
//   no socket reopens, and the page is connected and drawing when it is shown again.
//     VIBESERVER_BIN=…/vibeserver [HIDE_S=100] [EXPECT_DROP=1] node scripts/test-web-keepalive.mjs
//   EXPECT_DROP=1 inverts the verdict for the CONTROL run against a server carrying the old page (it must reproduce the
//   fault, or this test proves nothing).
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
const HIDE_S = Number(process.env.HIDE_S || 100), EXPECT_DROP = process.env.EXPECT_DROP === '1';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// ★ installed before the page's own scripts: the throttle (off until window.__thr), the hidden state, and a recorder of
//   pings leaving the page on its main-thread sockets and of socket opens (a reconnect is a second open of one kind)
const HARNESS = `(() => {
  const ALIGN = 60000;
  const si = window.setInterval.bind(window), st = window.setTimeout.bind(window), raf = window.requestAnimationFrame && window.requestAnimationFrame.bind(window);
  window.__thr = false;
  window.setInterval = function (fn, ms, ...a) {
    let last = -1e12;
    return si(function () {
      if (window.__thr) { const n = performance.now(); if (n - last < ALIGN) return; last = n; }
      return typeof fn === 'function' ? fn.apply(this, a) : (0, eval)(fn);
    }, ms);
  };
  window.setTimeout = function (fn, ms, ...a) {
    if (window.__thr) ms = Math.max(ms || 0, ALIGN - (performance.now() % ALIGN));
    return st(fn, ms, ...a);
  };
  const held = [];
  if (raf) window.requestAnimationFrame = function (cb) { if (window.__thr) { held.push(cb); return 0; } return raf(cb); };
  let hidden = false;
  Object.defineProperty(document, 'hidden', { configurable: true, get: () => hidden });
  Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => hidden ? 'hidden' : 'visible' });
  window.__hide = (on) => {
    hidden = on; window.__thr = on;
    document.dispatchEvent(new Event('visibilitychange'));
    if (!on && raf) { const h = held.splice(0); for (const cb of h) raf(cb); }
  };
  const S = window.__ka = { pings: [], opens: {} };
  const send0 = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) {
    if (typeof d === 'string' && d.indexOf('"type":"ping"') >= 0) { S.pings.push(performance.now()); if (S.pings.length > 2000) S.pings.splice(0, 1000); }
    return send0.call(this, d);
  };
  const WS0 = window.WebSocket;
  window.WebSocket = function (u, p) { const w = p === undefined ? new WS0(u) : new WS0(u, p);
    const k = String(u).replace(/\\?.*$/, '').replace(/^.*\\/ws\\//, ''); S.opens[k] = (S.opens[k] || 0) + 1; return w; };
  window.WebSocket.prototype = WS0.prototype; Object.assign(window.WebSocket, { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 });
})();`;

const children = [];
let dir = '';
let wsC = null;
try {
  dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-keepalive-'));
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Keep-alive test', locator: 'IO90bs', adminPass: 'keepalive-admin',
    radios: [{ serial: 'ka-test', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
  children.push(spawn('nice', ['-n', '10', process.execPath, path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' }));
  await sleep(400);
  const logFd = fs.openSync(path.join(dir, 'server.log'), 'w');
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  children.push(spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '144800000', '--mode', 'nfm', '--users', '1'],
    { env, stdio: ['ignore', logFd, logFd] }));
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  for (let i = 0; i < 150; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(1000) }); break; } catch { await sleep(100); } }
  children.push(spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' }));
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params }));
    setTimeout(() => { if (pend.has(i)) { pend.delete(i); res({ timeout: true }); } }, 20000); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  const click = async (sel) => {
    const r = JSON.parse(await ev(`JSON.stringify((${sel})?.getBoundingClientRect() || null)`) || 'null');
    if (!r) return false;
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
    return true;
  };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: HARNESS });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  const startSel = `[...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`;
  await until(`!!${startSel}`, 15000);
  await click(startSel);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await click(`document.querySelector('#audioGate button')`);
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000), 'START RADIO pressed');
  await sleep(3000);
  const proto = /proto=(\d+)/.exec(L())?.[1];
  const fpsNow = async () => Number(await ev(`(() => { const m = /([0-9.]+)\\s*fps/i.exec(document.body.innerText); return m ? m[1] : -1; })()`));

  async function hiddenRun(tag, prep) {
    console.log(`── ${tag}: hidden ${HIDE_S} s, main-thread timers at one a minute ──`);
    await prep();
    await sleep(1500);
    const deadBefore = (L().match(/no application message/g) || []).length;
    const opens0 = await ev(`JSON.stringify(window.__ka.opens)`);
    await ev(`(() => { window.__ka.pings.length = 0; window.__hide(true); return document.visibilityState; })()`);
    const t0 = Date.now();
    await sleep(HIDE_S * 1000);
    const pings = JSON.parse(await ev(`JSON.stringify(window.__ka.pings)`) || '[]');
    const clock = await ev(`window.__kaClock ? window.__kaClock() : ''`);
    await ev(`window.__hide(false)`);
    await sleep(6000);
    const dead = (L().match(/no application message/g) || []).length - deadBefore;
    const opens1 = await ev(`JSON.stringify(window.__ka.opens)`);
    let gap = 0; for (let i = 1; i < pings.length; i++) gap = Math.max(gap, pings[i] - pings[i - 1]);
    console.log(`      ${pings.length} spectrum pings left the page in ${((Date.now() - t0) / 1000).toFixed(0)} s (largest gap ${(gap / 1000).toFixed(1)} s)${clock ? ', clock ' + clock : ''}; server releases ${dead}; opens ${opens0} → ${opens1}`);
    if (EXPECT_DROP) {
      ok(dead > 0 && opens0 !== opens1, `${tag}: CONTROL — the old page IS released for silence (${dead}×) and reconnects (${opens0} → ${opens1}): the harness reproduces the fault`);
      return;
    }
    ok(dead === 0, `${tag}: the server never released the session for silence (${dead}×)`);
    ok(opens0 === opens1, `${tag}: no socket reopened (${opens0} → ${opens1})`);
    ok(pings.length >= Math.floor(HIDE_S / 6) && gap < 10000, `${tag}: pings kept going while hidden (${pings.length}, gap ≤ ${(gap / 1000).toFixed(1)} s)`);
    const st = await ev(`(() => { const t = document.body.innerText; return /reconnecting|connection lost|not connected/i.test(t) ? 'says: ' + (t.match(/[^\\n]*(reconnecting|connection lost|not connected)[^\\n]*/i) || [''])[0] : 'ok'; })()`);
    ok(st === 'ok', `${tag}: shown again, the page says nothing about a lost connection (${st})`);
  }

  console.log(`   (sockets declare proto ${proto ?? '?'})`);
  await hiddenRun('NFM, speaker MUTED', async () => {
    await ev(`window.__vibeAudio && (window.__vibeAudio.muted = true)`);
    ok(await ev(`!!window.__vibeAudio && window.__vibeAudio.muted === true`), 'NFM: the speaker muted (gain 0)');
    if (!EXPECT_DROP) ok(await ev(`window.__vibeAudio.presenceOn === true`), 'NFM muted: the inaudible presence runs (the page still "plays")');
  });
  await hiddenRun('ISM, nothing to hear', async () => {
    await ev(`document.getElementById('mMode')?.click()`);
    await sleep(500);
    const sel = `[...document.querySelectorAll('.mModeOpt')].find(x => /^\\s*ISM/.test(x.textContent))`;
    const has = await until(`!!${sel}`, 6000);
    if (has) await ev(`${sel}.click()`);
    ok(has && await until(`!!window.__vibeAudio && window.__vibeAudio.quiet === true`, 8000), 'ISM: on, the audio output held quiet');
    if (!EXPECT_DROP) ok(await ev(`window.__vibeAudio.presenceOn === false`), 'ISM quiet: NO presence — the output stays let go (the AirPods rule)');
  });
} catch (e) { ok(false, `driver: ${e?.stack || e}`); }
finally {
  try { wsC?.close(); } catch {}
  for (const c of children) { try { c.kill('SIGTERM'); } catch {} }
  await sleep(800);
  if (dir) { if (fail === 0) fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); else { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true }); } catch {} console.log(`   (kept ${dir})`); } }
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
