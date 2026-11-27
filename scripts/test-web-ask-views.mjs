#!/usr/bin/env node
// test-web-ask-views.mjs — ★★★ ASK BEFORE TUNING STILL LETS YOU LOOK (Stuart, 2026-11-27, on the Sony's web page: "one
// limitation of the lock that prevents tuning until asked, i have no way of getting to the gallery"; then: "we want to
// prevent decoder/demodulator changing but still need access to the maps/gallery").
// A real vibeserver with --ask-before-tuning (users 4) on a fake rtl_tcp; listener 1 (a raw socket) is in first and holds the
// dial; THE BUILT PAGE (web/dist, served in front of the server by a small proxy) joins second and is held back. Headless
// MUTED Edge (tests are silent). Checked:
//   1. the page is locked (body.askLocked) — and the mode pill still OPENS its menu;
//   2. a demodulator pick in the menu nudges ("ask in chat") and does NOT tune: nothing sent, the server's mode unchanged;
//   3. DECODERS… opens the panel; a decoder key there (RTTY) nudges and does not switch on — nothing sent, no box;
//   4. GALLERY (VIEWS) opens the gallery; the DX LOG / maps keys are lit (not dimmed), the decoder keys dimmed;
//   5. AIR & SEA (the viewer) — where drawn — opens with no TUNE keys and sends nothing that tunes;
//   6. ONE DECODER LEFT (the door replaced by its key): the menu still carries the VIEWS, and GALLERY opens from there;
//      the sole decoder's key is dimmed and nudges.
// SHOTS=<dir> saves pictures; PASSTHRU=1 serves the binary's own page instead (the fail-before run).
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-ask-views.mjs   (exit 3 = not run)
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const DIST = process.env.WEB_DIST || path.join(ROOT, 'web/dist');
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }
if (!process.env.PASSTHRU && !fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.error('no built page — node scripts/build-web.mjs first — not run'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(BROWSER)) { console.error('no Edge — not run'); process.exit(3); }
const SHOTS = process.env.SHOTS || '';
if (SHOTS) fs.mkdirSync(SHOTS, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0, passes = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; else passes++; };

/** A minimal WebSocket client (test-ask-before-tuning.mjs's), text frames only. */
function rawWs(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], byType: {} };
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
      if (op === 0x1) { const t = p.toString(); const m = /"type":"([a-z_]+)"/.exec(t); if (m) st.byType[m[1]] = t; }
    }
  });
  st.last = (type) => (st.byType[type] ? JSON.parse(st.byType[type]) : null);
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}

const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-askviews-'));
const procs = [];
let holder = null;
const rtlPort = await freePort();
procs.push(spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' }));
await sleep(400);
const D = path.join(T, 'srv');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(D, d), { recursive: true });
const sport = await freePort();
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(sport), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm',
                       '--users', '4', '--ask-before-tuning'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(D, 'config.json'), VIBESERVER_DATA_DIR: path.join(D, 'data'), VIBESERVER_RUNTIME_DIR: path.join(D, 'run'), HOME: D },
  stdio: 'ignore' }));
for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${sport}/vibeserver.json`); break; } catch { await sleep(250); } }

// ★ The page from web/dist; everything else (HTTP and the WebSocket upgrades) straight through to the server.
const port = await freePort();
const proxy = http.createServer((req, res) => {
  const vs = /^(?:\/r\/[^/]+)?\/vs\/([A-Za-z0-9._-]+\.(?:js|css))(?:[?#]|$)/.exec(req.url);
  if (vs && !process.env.PASSTHRU) {
    try { const b = fs.readFileSync(path.join(DIST, 'vs', vs[1]));
      res.writeHead(200, { 'Content-Type': vs[1].endsWith('.css') ? 'text/css' : 'text/javascript; charset=utf-8', 'Cache-Control': 'no-store' }); res.end(b);
    } catch { res.writeHead(404).end(); }
    return;
  }
  if (!process.env.PASSTHRU && (req.url === '/' || req.url.startsWith('/?'))) {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store' });
    res.end(fs.readFileSync(path.join(DIST, 'vibesdr.html')));
    return;
  }
  const up = http.request({ host: '127.0.0.1', port: sport, path: req.url, method: req.method, headers: req.headers }, (r) => {
    res.writeHead(r.statusCode || 502, r.headers); r.pipe(res);
  });
  up.on('error', () => { try { res.writeHead(502).end(); } catch {} });
  req.pipe(up);
});
proxy.on('upgrade', (req, sock, head) => {
  const up = net.connect(sport, '127.0.0.1', () => {
    up.write(`${req.method} ${req.url} HTTP/1.1\r\n` + Object.entries(req.headers).map(([k, v]) => `${k}: ${v}`).join('\r\n') + '\r\n\r\n');
    if (head && head.length) up.write(head);
    up.pipe(sock); sock.pipe(up);
  });
  up.on('error', () => sock.destroy()); sock.on('error', () => up.destroy());
});
await new Promise((r) => proxy.listen(port, '127.0.0.1', r));

const cdp = await freePort();
procs.push(spawn(BROWSER, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions', '--autoplay-policy=no-user-gesture-required',
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=420,900', 'about:blank'], { stdio: 'ignore' }));

let target;
for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0; const pending = new Map(); const errors = [];
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(200); } return false; };
const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1, pointerType: 'mouse' }); };
/** A real pointer press in the middle of the element `sel` (scrolled into view first); false when it is not on screen. */
const press = async (sel) => {
  const c = await js(`(() => { const e = ${sel}; if (!e) return null; e.scrollIntoView({ block: 'center' }); const r = e.getBoundingClientRect();
    return r.width > 0 && r.height > 0 ? [r.left + r.width / 2, r.top + r.height / 2] : null; })()`);
  if (!c) return false;
  if (process.env.DEBUG) console.log('  [debug] press', sel.slice(0, 60), c, await js(`document.elementFromPoint(${c[0]}, ${c[1]})?.outerHTML.slice(0, 120)`));
  await tap(c[0], c[1]); return true;
};
const shot = async (name) => { if (!SHOTS) return; const r = await call('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, name); fs.writeFileSync(f, Buffer.from(r.result.data, 'base64')); console.log(`  (screenshot ${f})`); };
const SNIFF = `(() => { window.__sent = []; const o = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) { try { if (typeof d === 'string') window.__sent.push(d); } catch {} return o.apply(this, arguments); }; })();`;
/** The page's messages since n that would move the radio or switch a decoder. */
const moved = (n) => js(`window.__sent.slice(${n}).filter(s => /"type":"(tune|mode|bandwidth|dab|adsb|ais|ism|ndb|vormap|avdata|sondes|decoder[a-z_]*|rtty[a-z_]*|navtex[a-z_]*|mode_resume)"/.test(s))`);
const nudged = () => until(`!!document.getElementById("mChat")?.classList.contains("askNudge")`, 2500);
/** nudgeAsk() is once per 2.5 s and its flash lasts 3 s: wait both out before the next reach. */
const nudgeRest = () => sleep(3200);
const menuKey = (text) => `[...document.querySelectorAll("#mModeMenu .mModeOpt")].find(b => b.textContent.trim() === ${JSON.stringify(text)})`;
const dim = (sel) => js(`(() => { const e = ${sel}; return !!e && parseFloat(getComputedStyle(e).opacity) < 0.5; })()`);

try {
  holder = rawWs(sport, '/ws/user-spectrum?user_session_id=hold0001&bins=256');   // ★ in first: holds the dial
  await sleep(1200);
  await call('Runtime.enable'); await call('Page.enable');
  await call('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
  await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  for (let i = 0; i < 4; i++) {
    await until(`!!document.getElementById("btnConnect") && document.readyState === "complete"`);
    await sleep(1500);
    await js(`document.getElementById("btnConnect")?.click(); 1`);
    if (await until(`!!document.getElementById("audioGate") || parseFloat(document.getElementById("freqInput")?.value) > 0`, 8000)) break;
  }
  for (let i = 0; i < 6; i++) {
    if (!(await press(`document.querySelector("#audioGate button")`))) break;
    if (await until(`!document.getElementById("audioGate")`, 3000)) break;
  }
  await until(`parseFloat(document.getElementById("freqInput").value) > 0`);
  await until(`/WFM/.test(document.getElementById("mMode")?.textContent || "")`, 10000);   // ★ settled on the room's dial
  await sleep(1500);
  for (let i = 0; i < 6; i++) {   // ★ the audio gate can come back once the stream starts
    if (!(await press(`document.querySelector("#audioGate button")`))) break;
    await until(`!document.getElementById("audioGate")`, 3000);
    await sleep(800);
  }

  // ── 1. locked, and the pill still opens ──
  ok(await until(`document.body.classList.contains("askLocked")`, 10000), '1. the page joined second and is held back (body.askLocked)');
  ok(holder.last('config')?.mode === 'wfm' || holder.last('config') === null, `   the holder's radio is in WFM (${holder.last('config')?.mode})`);
  ok(!(await dim(`document.getElementById("mMode")`)), '   the mode pill is NOT dimmed — it is the way to the gallery');
  await press(`document.getElementById("mMode")`);
  ok(await until(`!!document.getElementById("mModeMenu")`, 3000), '★ the mode pill opens its menu while locked');
  await shot('ask-views-menu.png');

  // ── 2. a demodulator pick nudges and does not tune ──
  if (await js(`!!document.getElementById("mModeMenu")`)) {
    ok(await dim(menuKey('AM')), '   the demodulators are dimmed (locked, never missing)');
    ok(!(await dim(menuKey('DECODERS…'))), '   DECODERS… is lit');
    const n0 = await js(`window.__sent.length`);
    await press(menuKey('AM'));
    ok(await nudged(), '2. ★ AM pressed while locked: the "ask in chat" nudge (the chat key flashes)');
    await sleep(1200);
    const m = await moved(n0);
    ok(m.length === 0, `   …and nothing that tunes is sent${m.length ? ': ' + m.join(' ') : ''}`);
    ok(holder.last('config')?.mode === 'wfm', `   …the server's radio is still WFM (${holder.last('config')?.mode})`);
    ok(/WFM/i.test(await js(`document.getElementById("mMode").textContent`)), `   …and the pill still reads WFM`);
    await nudgeRest();

    // ── 3. DECODERS… → a decoder key nudges, does not switch on ──
    await press(menuKey('DECODERS…'));
    ok(await until(`document.getElementById("decodersPanel").classList.contains("open")`, 3000), '3. ★ DECODERS… opens the decoders panel while locked');
    await shot('ask-views-panel.png');
    // ★ RTTY on HF: the band tab for HF first (the panel opens on the dial's band — VHF at 96.1 MHz); the tabs stay live
    await js(`[...document.querySelectorAll("#decBandTabs button")].find(b => /^HF/.test(b.textContent.trim()))?.click(); 1`);
    await sleep(300);
    const rtty = `document.querySelector('#decodersPanel [data-dec="rtty"]')`;
    ok(await dim(rtty), '   the RTTY key is dimmed');
    const n1 = await js(`window.__sent.length`);
    await press(rtty);
    ok(await nudged(), '   ★ RTTY pressed while locked: the nudge');
    await sleep(1200);
    const m1 = await moved(n1);
    ok(m1.length === 0 && !(await js(`${rtty}.classList.contains("on")`)), `   …and the decoder is NOT switched on${m1.length ? ': ' + m1.join(' ') : ''}`);
    ok(!(await dim(`document.getElementById("galleryBtn")`)), '   GALLERY is lit');
    await nudgeRest();

    // ── 4. GALLERY opens ──
    await press(`document.getElementById("galleryBtn")`);
    ok(await until(`document.getElementById("galleryPanel").classList.contains("open")`, 3000), '4. ★ GALLERY opens while locked');
    await shot('ask-views-gallery.png');
    await js(`document.getElementById("galClose")?.click(); 1`);
    await sleep(400);

    // ── 5. the AIR & SEA viewer — no TUNE keys, nothing tunes ──
    await js(`document.getElementById("mMode").click(); 1`);
    if (await until(`!!${menuKey('DECODERS…')}`, 2000)) await js(`${menuKey('DECODERS…')}.click(); 1`);
    await sleep(400);
    const av = await js(`!document.getElementById("avMapBtn")?.hidden`);
    if (av) {
      const n2 = await js(`window.__sent.length`);
      await press(`document.getElementById("avMapBtn")`);
      await sleep(1500);
      ok(await until(`!!document.querySelector(".trkView:not([hidden])")`, 4000), '5. the AIR & SEA viewer opens while locked');
      const tuneKeys = await js(`[...document.querySelectorAll(".trkView:not([hidden]) .tune button, .trkView:not([hidden]) .tunePop button")].filter(b => b.offsetParent).length`);
      ok(tuneKeys === 0, `   …with no TUNE keys (${tuneKeys}) — a viewer: nothing in it moves the radio`);
      ok((await moved(n2)).length === 0, '   …and sends nothing that tunes');
      await shot('ask-views-airsea.png');
      for (let i = 0; i < 3 && await js(`!!document.querySelector(".trkView:not([hidden])")`); i++) {
        await call('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
        await call('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
        await sleep(500);
      }
      ok(!(await js(`!!document.querySelector(".trkView:not([hidden])")`)), '   …and closes');
    } else console.log('  (5. no AIR & SEA key on this radio — the viewer is not drawn here)');
    for (let i = 0; i < 3 && await js(`!!document.querySelector("#decodersPanel.open, #galleryPanel.open, #mModeMenu")`); i++) {
      await call('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
      await call('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
      await sleep(400);
    }

    // ── 6. one decoder left: the door is replaced by its key, and the VIEWS come into the menu ──
    const all = ['spots', 'ft8', 'navtex', 'wefax', 'sstv', 'time', 'rds', 'cw', 'psk', 'aprs', 'dsc', 'sitora', 'same', 'selcal', 'hfdl', 'hell', 'jtty',
                 'ncdxf', 'balloon', 'tones', 'ale', 'vor', 'sarsat', 'mfsk', 'rtty2', 'ism', 'adsb', 'ais', 'ndb', 'vormap', 'avdata', 'sondes', 'drm'];
    await js(`(() => { const s = window.__vibeWf().spec; s.cb.onBlockedModes(${JSON.stringify(all)}); return 1; })()`);
    await sleep(600);
    const left = await js(`[...document.querySelectorAll('#decodersPanel [data-dec]')].filter(b => !b.hidden).map(b => b.dataset.dec)`);
    console.log(`  decoders left after the block: ${JSON.stringify(left)}`);
    await press(`document.getElementById("mMode")`);
    await until(`!!document.getElementById("mModeMenu")`, 3000);
    await shot('ask-views-sole.png');
    if (process.env.DEBUG) console.log('  [debug] menu keys', await js(`[...document.querySelectorAll("#mModeMenu button")].map(b => b.textContent.trim())`));
    ok(!(await js(`!!${menuKey('DECODERS…')}`)) && (left.length !== 1 || await js(`!!${menuKey(String(left[0]).toUpperCase())}`)),
       `6. one decoder left: its own key stands where DECODERS… stood (${left.join(', ')})`);
    ok(await js(`!!document.querySelector('#mModeViews [data-view="galleryBtn"]')`), '   ★ …and the menu carries the VIEWS (GALLERY)');
    if (left.length === 1) {
      const sole = menuKey(String(left[0]).toUpperCase());
      ok(await dim(sole), '   the sole decoder\'s key is dimmed');
      const n3 = await js(`window.__sent.length`);
      await press(sole);
      ok(await nudged(), '   ★ …and nudges when pressed');
      await sleep(800);
      ok((await moved(n3)).length === 0, '   …sending nothing');
      await nudgeRest();
      if (!(await js(`!!document.getElementById("mModeMenu")`))) { await press(`document.getElementById("mMode")`); await until(`!!document.getElementById("mModeMenu")`, 3000); }
    }
    ok(!(await dim(`document.querySelector('#mModeViews [data-view="galleryBtn"]')`)), '   GALLERY in the menu is lit');
    await press(`document.querySelector('#mModeViews [data-view="galleryBtn"]')`);
    ok(await until(`document.getElementById("galleryPanel").classList.contains("open")`, 3000), '   ★ GALLERY opens from the menu with one decoder left, while locked');
  }
  ok(holder.last('config')?.mode === 'wfm' && Number(holder.last('config')?.vfo) === 96100000,
     `the server's radio never moved (${holder.last('config')?.mode} ${holder.last('config')?.vfo})`);
  ok(errors.length === 0, 'no JavaScript errors' + (errors.length ? ': ' + errors.slice(0, 2).join(' | ') : ''));
} catch (e) { console.error(e); fails++; }
try { ws.close(); } catch {}
try { holder?.close(); } catch {}
for (const p of procs) { try { p.kill(); } catch {} }
proxy.close();
await sleep(600);
fs.rmSync(T, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m (${passes} passed)` : `\n\x1b[32mall ${passes} passed\x1b[0m`);
process.exit(fails ? 1 : 0);
