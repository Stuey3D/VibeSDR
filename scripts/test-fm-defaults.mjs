#!/usr/bin/env node
// test-fm-defaults.mjs — the owner's BROADCAST FM DEFAULTS and REMEMBER / RETURN, end to end on a real vibeserver
// (fake rtl_tcp, the radio's fm* keys in config.json — where the setup page writes them).
//
// ★★★ THE REQUEST (Stuart, 2026-11-03): the owner sets the Broadcast FM options in Setup, and chooses whether the
//     server remembers what the last listener set or goes back to the owner's defaults. And the fault behind it:
//     "I often go into servers and see CEQ turned off" — a browser re-sending its saved CEQ OFF on every connect.
//
// What this pins (the rules are vibe_fm_defaults.h; the pure logic is vibeserver/test-fm-defaults.cpp):
//   1. a FRESH server: NR is OFF, everything else ON, de-emphasis 50 µs (Region 1 locator).
//   2. owner-set defaults reach a new listener (NR ON, CEQ OFF, 75 µs).
//   3. RETURN, shared dial: a listener turns CEQ off; a JOINER sees CEQ off (the dial is the dial — never reset
//      under someone); after both leave, the NEXT listener gets CEQ back ON; the first listener RELOADING (same
//      session) gets their own CEQ OFF back.
//   4. REMEMBER, shared dial: the next listener inherits CEQ OFF.
//   5. RETURN, locked range: a second listener's channel starts with CEQ ON while the first keeps theirs OFF.
//   6. REMEMBER, locked range: the second listener's channel starts with CEQ OFF (the last values anyone set).
//   7. ★ THE WEB PAGE (headless, MUTED) in a browser whose storage remembers CEQ OFF + NR ON — what a browser that
//      once saw them left behind — sends NEITHER on connect, and shows the server's CEQ ON.
// ★ SILENT: raw sockets discard audio; the browser runs --mute-audio --disable-audio-output.
//
//   usage: VIBESERVER_BIN=… node scripts/test-fm-defaults.mjs      (exit 3 = not run)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const FM = 100000000;

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
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
      if (op === 0x1) { st.txt.push(p.toString()); if (st.txt.length > 600) st.txt.splice(0, 300); }
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
/** The newest hwinfo this socket was sent (the Broadcast FM row is in it: wsp, ims, ceq, nb, autobw, deemph, stereo). */
const hw = (st) => { for (let i = st.txt.length - 1; i >= 0; i--) if (st.txt[i].startsWith('{"type":"hwinfo"')) { try { return JSON.parse(st.txt[i]); } catch {} } return null; };
/** A listener: audio + spectrum on one session, as the clients connect. Resolves once it has an hwinfo. */
async function listener(port, sid) {
  const aud = ws(port, `/ws/audio?user_session_id=${sid}&codec=opus`);
  await sleep(150);
  const sp = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`);
  for (let i = 0; i < 60 && !hw(sp); i++) await sleep(100);
  return { sp, aud, hw: () => hw(sp), send: (o) => sp.send(o), close: () => { sp.close(); aud.close(); } };
}
/** Ask for the full state and wait for the hwinfo it brings. */
async function fresh(L) { const n = L.sp.txt.length; L.send({ type: 'state' }); for (let i = 0; i < 30; i++) { await sleep(100); if (L.sp.txt.slice(n).some((t) => t.startsWith('{"type":"hwinfo"'))) break; } return L.hw() || {}; }

const SERVERS = [];
async function server(tag, radio) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-fmdef-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, fullMode: true, sharing: 'public', port: front, locator: 'IO92nh',
    radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: FM, demodMode: 'wfm', landingFreq: FM, ...radio }],
  }));
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--wfm', '100.0'], { stdio: 'ignore' });
  await sleep(300);
  const logPath = path.join(dir, 'server.log');
  const logFd = fs.openSync(logPath, 'w');
  const args = ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', String(FM), '--mode', 'wfm', '--idle-grace', '0'];
  if (radio.users) args.push('--users', String(radio.users));
  if (radio.lockFreq) args.push('--lock-freq', String(radio.lockFreq));
  const srv = spawn(BIN, args, { env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                                        VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir }, stdio: ['ignore', logFd, logFd] });
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  await sleep(600);
  const S = { port, dir, log: () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } },
              stop: async () => { for (const p of [srv, rtl]) { try { p.kill(); } catch {} } await sleep(400); } };
  SERVERS.push(S);
  return S;
}
const sid = (n) => `fmdef000-0000-4000-8000-${String(n).padStart(12, '0')}`;

try {
  console.log('0. the setup page card: drawn, filled from the radio, and saved back with the fm* keys');
  { const src = fs.readFileSync(path.join(root, 'android/app/src/main/cpp/vibe_setup_page.h'), 'utf8');
    const html = src.split('R"HTML(')[1].split(')HTML"')[0];
    const ids = ['fmDefCard', 'fmDeemph', 'fmStereo', 'fmNr', 'fmIms', 'fmCeq', 'fmAutoBw', 'fmNb', 'fmReturn', 'fmRemember', 'fmPolicyHint'];
    ok(ids.every((i) => html.includes(`id="${i}"`)), 'every control of the card is in the markup');
    ok(/\.\.\.fmDefCollect\(\)/.test(html) && /fmDefRender\(r\)/.test(html), 'collectRadio saves it and fill() paints it');
    // ★ The card's own three functions, run against a fake DOM: what the owner sees for a radio, and what is posted.
    const fn = (name) => { const i = html.indexOf(`function ${name}(`); let d = 0, j = html.indexOf('{', i);
      for (let k = j; k < html.length; k++) { if (html[k] === '{') d++; else if (html[k] === '}' && --d === 0) return html.slice(i, k + 1); } return ''; };
    const els = {}; for (const i of [...ids, 'fmDeemphHint', 'users']) els[i] = { checked: false, value: '', textContent: '',
      classList: { _h: false, toggle(_c, on) { this._h = !!on; }, contains() { return this._h; } } };
    els.users.value = '5';
    let R = {};
    const run = new Function('$', 'radio', 'modeBlockSet', `${fn('fmDefRender')}\n${fn('fmDefHints')}\n${fn('fmDefCollect')}\nreturn { fmDefRender, fmDefCollect };`);
    const api = run((i) => els[i], () => R, () => new Set(String(R.blockedModes || '').split(',').filter(Boolean)));
    R = { driver: 'rtlsdr', mode: 'single' }; api.fmDefRender(R);
    ok(els.fmDeemph.value === '-1' && els.fmNr.checked === false && els.fmCeq.checked && els.fmAutoBw.checked && els.fmReturn.checked,
       'a radio with nothing set shows the server\'s defaults: by region, NR off, the rest on, RETURN');
    ok(/nobody else is listening/.test(els.fmPolicyHint.textContent), 'RETURN on a shared dial says WHEN it returns');
    els.fmNr.checked = true; els.fmCeq.checked = false; els.fmDeemph.value = '75'; els.fmRemember.checked = true; els.fmReturn.checked = false;
    const out = api.fmDefCollect();
    ok(out.fmNr === true && out.fmCeq === false && out.fmDeemph === 75 && out.fmRemember === true && out.fmStereo === true,
       'what the owner ticks is what is posted (' + JSON.stringify(out) + ')');
    R = { driver: 'icom' }; api.fmDefRender(R);
    ok(els.fmDefCard.classList.contains() && Object.keys(api.fmDefCollect()).length === 0, 'an Icom draws no card and posts nothing');
    R = { driver: 'rtlsdr', blockedModes: 'wfm' }; api.fmDefRender(R);
    ok(els.fmDefCard.classList.contains(), 'nor does a radio with WFM blocked'); }

  console.log('1. a fresh server (no fm* keys)');
  { const S = await server('fresh', { users: 1 });
    const A = await listener(S.port, sid(1)); const h = await fresh(A);
    ok(h.wsp === false, `NR is OFF (wsp=${h.wsp})`);
    ok(h.ceq === true && h.ims === true && h.nb === true && h.stereo === true, 'ST, IMS, CEQ and NB are ON');
    ok(Math.abs(h.deemph - 50e-6) < 1e-9, `de-emphasis 50 µs in Region 1 (${h.deemph})`);
    ok(/Broadcast FM defaults: \(de-emphasis by region\)/.test(S.log()), 'the server logs the owner\'s defaults at start');
    A.close(); await S.stop(); }

  console.log('2. owner-set defaults reach a new listener');
  { const S = await server('owner', { users: 1, fmNr: true, fmCeq: false, fmDeemph: 75 });
    const A = await listener(S.port, sid(2)); const h = await fresh(A);
    ok(h.wsp === true && h.ceq === false, `NR ON and CEQ OFF as the owner set (wsp=${h.wsp}, ceq=${h.ceq})`);
    ok(Math.abs(h.deemph - 75e-6) < 1e-9, `de-emphasis 75 µs as set (${h.deemph})`);
    A.close(); await S.stop(); }

  console.log('3. RETURN, shared dial');
  { const S = await server('ret-shared', { users: 5 });
    const A = await listener(S.port, sid(31));
    A.send({ type: 'ceq', on: false }); await sleep(400);
    ok((await fresh(A)).ceq === false, 'A turned CEQ off');
    const mark = S.log().length;
    const B = await listener(S.port, sid(32)); const hb = await fresh(B);
    ok(hb.ceq === false, 'B JOINING an occupied dial sees CEQ OFF — the dial is not reset under A');
    ok(!/Broadcast FM back to/.test(S.log().slice(mark)), 'and the server changed nothing for the joiner');
    A.close(); B.close(); await sleep(1200);
    const C = await listener(S.port, sid(33)); const hc = await fresh(C);
    ok(hc.ceq === true, 'C, arriving to an EMPTY dial, gets the owner\'s CEQ ON back');
    ok(/new session — Broadcast FM back to/.test(S.log()), 'the server says it returned the defaults');
    C.close(); await sleep(1200);
    const A2 = await listener(S.port, sid(31)); const ha = await fresh(A2);
    ok(ha.ceq === false, 'A reloading (same session) gets A\'s own CEQ OFF back');
    A2.close(); await S.stop(); }

  console.log('4. REMEMBER, shared dial');
  { const S = await server('rem-shared', { users: 5, fmRemember: true });
    const A = await listener(S.port, sid(41));
    A.send({ type: 'ceq', on: false }); await sleep(400); A.close(); await sleep(1200);
    const C = await listener(S.port, sid(42)); const h = await fresh(C);
    ok(h.ceq === false, 'the next listener inherits CEQ OFF as the last one left it');
    C.close(); await S.stop(); }

  console.log('5. RETURN, locked range (a channel each)');
  { const S = await server('ret-locked', { users: 5, mode: 'locked', lockFreq: FM });
    const A = await listener(S.port, sid(51));
    A.send({ type: 'ceq', on: false }); await sleep(400);
    const B = await listener(S.port, sid(52));
    const hb = await fresh(B), ha = await fresh(A);
    ok(hb.ceq === true, 'B\'s new channel starts with the owner\'s CEQ ON');
    ok(ha.ceq === false, 'A\'s own channel keeps CEQ OFF');
    ok(/Broadcast FM starts at .*return to the owner's defaults/.test(S.log()), 'the server logs where B\'s channel started');
    A.close(); B.close(); await S.stop(); }

  console.log('6. REMEMBER, locked range');
  { const S = await server('rem-locked', { users: 5, mode: 'locked', lockFreq: FM, fmRemember: true });
    const A = await listener(S.port, sid(61));
    A.send({ type: 'ceq', on: false }); await sleep(400);
    const B = await listener(S.port, sid(62)); const hb = await fresh(B);
    ok(hb.ceq === false, 'B\'s channel starts from the last values anyone set (CEQ OFF)');
    A.close(); B.close(); await S.stop(); }

  console.log('7. the web page does not re-assert a remembered CEQ OFF / NR ON');
  if (!fs.existsSync(BROWSER)) console.log('   (no Edge at ' + BROWSER + ' — not run)');
  else {
    const S = await server('web', { users: 5 });
    const cdp = await freePort();
    const br = spawn(BROWSER, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions',
      '--autoplay-policy=no-user-gesture-required', `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(S.dir, 'browser')}`,
      '--window-size=1200,800', 'about:blank'], { stdio: 'ignore' });
    try {
      let target;
      for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
      const cws = new WebSocket(target.webSocketDebuggerUrl);
      await new Promise((r) => (cws.onopen = r));
      let id = 0; const pending = new Map();
      cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
      const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); });
      const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
      const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(150); } return false; };
      const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
      /** Connect and get past the audio gate with a real tap (a synthetic click() does not count as a gesture). */
      const connect = async () => {
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
        return until(`parseFloat(document.getElementById("freqInput").value) > 0`, 40000);
      };
      await call('Runtime.enable'); await call('Page.enable');
      // ★ Every Broadcast FM message the page sends, recorded before its own code runs.
      await call('Page.addScriptToEvaluateOnNewDocument', { source: `
        window.__fmSent = [];
        const o = WebSocket.prototype.send;
        WebSocket.prototype.send = function (d) {
          if (typeof d === 'string' && /"type":"(ceq|wsp|ims|nb|stereo|deemph)"/.test(d)) window.__fmSent.push(d);
          return o.call(this, d);
        };` });
      // ★ A first visit, so the page has written its own prefs; then the storage a browser that once SAW CEQ off and
      //   NR on would have been left with (setToggleTo saved both), and a RELOAD — the real return visit.
      await call('Page.navigate', { url: `http://127.0.0.1:${S.port}/` });
      ok(await connect(), 'the page receives (first visit)');
      const seeded = await js(`(() => { let n = 0; for (const k of Object.keys(localStorage)) { try { const v = JSON.parse(localStorage.getItem(k));
                  if (v && typeof v === 'object' && !Array.isArray(v)) { Object.assign(v, { ceq: false, wsp: true, ims: false, nb: false, stereo: false, deemph: 75 });
                    localStorage.setItem(k, JSON.stringify(v)); n++; } } catch {} } return n; })()`);
      ok(seeded > 0, `its storage now remembers CEQ OFF, NR ON, mono and 75 µs (${seeded} object(s)) — the test tests something`);
      await js('window.__beforeReload = 1; 1');
      await call('Page.reload', { ignoreCache: true });
      await until('!window.__beforeReload && document.readyState === "complete"', 20000);
      const rx = await connect();
      ok(rx, 'the page receives after the reload');
      if (!rx) console.log('     page says: ' + JSON.stringify(await js(`[document.getElementById("freqInput")?.value, location.href, (document.body.innerText || "").slice(0, 400)]`)));
      await sleep(3000);
      const sent = await js(`window.__fmSent.join(' ')`);
      ok(sent === '', `it sent no Broadcast FM setting on connect (${sent || 'none'})`);
      const label = await js(`document.getElementById("ceqBtn").textContent`);
      ok(label === 'CEQ ON', `its CEQ button shows the SERVER's CEQ ON (${label})`);
      ok(!/listener DSP: channel EQ off/.test(S.log()), 'the server never heard CEQ OFF');
      // ★ A PRESS still works, first time — the button, not a stale closure, says what it is.
      await js(`document.getElementById("ceqBtn").click(); 1`);
      await sleep(800);
      ok(/listener DSP: channel EQ off/.test(S.log()), 'one press of CEQ turns it OFF on the server');
      cws.close();
    } finally { try { br.kill(); } catch {} await S.stop(); }
  }
} catch (e) {
  console.log('   FAIL threw: ' + (e && e.stack || e)); fail++;
} finally {
  for (const S of SERVERS) { await S.stop(); try { fs.rmSync(S.dir, { recursive: true, force: true }); } catch {} }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
