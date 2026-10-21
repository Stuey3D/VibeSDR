#!/usr/bin/env node
// test-dab-burst.mjs — the REAL server and the SERVED web page, end to end: hammering the multiplex button tunes the
// radio once, on the block the user stopped on; a lone press tunes at once.
//
// ★★★ Stuart, 2026-10-21: "on DAB we need to cache the tunes so if I connect at 12b and rapidly tune down to 7D
//     hammering the multiplex button multiple times a second to get it down quick, dont tune the SDR just hold it and
//     tune on final move. However if I am tuning a multiplex and holding there for a second to see the waterfall then
//     tuning again we need to tune as normal". Then: "I tried to rapid tune the Pi2 earlier and it kept snapping back".
//   1. SERVER (a raw socket — an old app, or anybody's client that sends a `dab` per press): 10 block steps in 500 ms
//      re-acquire twice at most (the first press + the last block), the same 10 as one clump once, a lone step at
//      once, an exit mid-burst never tunes the held step. Counted from the server's own "[DAB] mode ON" lines.
//   2. WEB PAGE (a real headless, MUTED browser): ten tune-down presses (< ,) move the label ten blocks and send ONE `dab`,
//      the block stopped on, ~450 ms after the last press; the label never goes back to a block it left; a press,
//      a look, a press sends two; leaving DAB mid-burst sends no block.
// ★ No bench clip needed: the server reports the block it is on whether or not a multiplex locks.
// ★ SILENT: --mute-audio and --disable-audio-output on the browser; the raw sockets play nothing.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-dab-burst.mjs   (exit 3 = not run)
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); if (!c) fails++; };
const B = ['5A','5B','5C','5D','6A','6B','6C','6D','7A','7B','7C','7D','8A','8B','8C','8D','9A','9B','9C','9D',
           '10A','10N','10B','10C','10D','11A','11N','11B','11C','11D','12A','12N','12B','12C','12D',
           '13A','13B','13C','13D','13E','13F'];

/** A minimal WebSocket client (the probes' own). */
function ws(port, pathq) {
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
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  const frame = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    return Buffer.concat([h, b]);
  };
  st.send = (o) => s.write(frame(o));
  st.sendClump = (list) => s.write(Buffer.concat(list.map(frame)));
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}

const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dab-burst-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(T, d));
fs.writeFileSync(path.join(T, 'config.json'), JSON.stringify({ configured: true, name: 'DAB burst test', locator: 'IO92nh',
  adminPass: 'dab-burst-admin-pass', radios: [{ serial: 'dabburst', label: 'T', enabled: true, configured: true, users: 5 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(400);
const logPath = path.join(T, 'server.log');
const logFd = fs.openSync(logPath, 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm',
                        '--users', '5', '--idle-grace', '0'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, 'config.json'), VIBESERVER_DATA_DIR: path.join(T, 'data'),
         VIBESERVER_RUNTIME_DIR: path.join(T, 'run'), HOME: T },
  stdio: ['ignore', logFd, logFd] });
const log = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
/** The blocks the server actually tuned since `mark` (its own "[DAB] mode ON: channel X" lines). */
const tunedSince = (mark) => [...log().slice(mark).matchAll(/\[DAB\] mode ON: channel (\w+)/g)].map((m) => m[1]);
let br = null;
async function cleanup() { for (const p of [br, srv, rtl]) { try { p?.kill(); } catch {} } await sleep(500); fs.rmSync(T, { recursive: true, force: true }); }

try {
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  await sleep(500);

  // ── 1. THE SERVER, from a client that does not coalesce ──
  console.log('── the server: a `dab` per press from an old client ──');
  const SID = 'dabb0000-burst-0000-0000-000000000001';
  const aud = ws(port, `/ws/audio?user_session_id=${SID}&codec=opus`);
  await sleep(800);
  const sp = ws(port, `/ws/user-spectrum?user_session_id=${SID}&bins=256`);
  await sleep(1200);
  sp.send({ type: 'dab', on: 1, channel: B.indexOf('12B') });
  let up = false;
  for (let i = 0; i < 50 && !up; i++) { await sleep(100); up = sp.txt.some((t) => t.includes('"type":"dab"') && t.includes('"channel":"12B"')); }
  ok(up, 'in DAB on 12B (the server reports it)');
  await sleep(1500);

  {
    const mark = log().length;
    for (let k = 1; k <= 10; k++) { sp.send({ type: 'dab', on: 1, channel: B.indexOf('12B') - k }); await sleep(50); }
    await sleep(1500);
    const t = tunedSince(mark);
    const want = B[B.indexOf('12B') - 10];
    ok(t.length >= 1 && t[t.length - 1] === want, `10 presses in 500 ms: the radio ends on ${want} (tuned: ${t.join(' ') || 'nothing'})`);
    ok(t.length <= 2, `…in ${t.length} re-acquires, not 10 (the first press goes at once; the rest wait for the last)`);
  }
  await sleep(1500);
  {
    const mark = log().length, from = B.indexOf('10D');
    const list = Array.from({ length: 10 }, (_, k) => ({ type: 'dab', on: 1, channel: from - 1 - k }));
    sp.sendClump(list);
    await sleep(1500);
    const t = tunedSince(mark);
    ok(t.length === 1 && t[0] === B[from - 10], `the same 10 arriving as one clump: ONE re-acquire, on ${B[from - 10]} (tuned: ${t.join(' ') || 'nothing'})`);
  }
  await sleep(1500);
  {
    const mark = log().length, want = B[B.indexOf('12B')];
    const t0 = Date.now();
    sp.send({ type: 'dab', on: 1, channel: B.indexOf('12B') });
    let at = -1;
    for (let i = 0; i < 100 && at < 0; i++) { if (tunedSince(mark).includes(want)) at = Date.now() - t0; else await sleep(10); }
    ok(at >= 0 && at < 250, `a LONE press is tuned at once (${at} ms to the server's "mode ON: channel ${want}")`);
  }
  await sleep(1500);
  {
    const mark = log().length;
    sp.send({ type: 'dab', on: 1, channel: B.indexOf('12A') }); await sleep(50);
    sp.send({ type: 'dab', on: 1, channel: B.indexOf('11D') }); await sleep(50);
    sp.send({ type: 'dab', on: 1, channel: B.indexOf('11C') }); await sleep(50);
    sp.send({ type: 'dab', on: 0 });
    await sleep(1500);
    const t = tunedSince(mark);
    ok(t.join(' ') === '12A', `leaving DAB mid-burst: only the first press was tuned, the held step never (tuned: ${t.join(' ') || 'nothing'})`);
    ok(sp.txt.slice(-40).some((x) => x.includes('"type":"dab_off"')), '…and the exit itself went through (dab_off)');
  }
  sp.close(); aud.close();
  await sleep(1500);

  // ── 2. THE WEB PAGE ──
  console.log('── the served web page: ten presses, one tune ──');
  if (!fs.existsSync(BROWSER)) { console.log('   (no Edge at ' + BROWSER + ' — browser half not run)'); }
  else {
    br = spawn(BROWSER, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions',
      '--autoplay-policy=no-user-gesture-required', `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`,
      '--window-size=1200,800', 'about:blank'], { stdio: 'ignore' });
    let target;
    for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
    const cws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((r) => (cws.onopen = r));
    let id = 0; const pending = new Map(); const errors = [];
    cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
      if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
    const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); });
    const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
    const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(150); } return false; };
    const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
    // ★ Every `dab` the page sends, timestamped — wrapped before the page's own code runs.
    await call('Runtime.enable'); await call('Page.enable');
    await call('Page.addScriptToEvaluateOnNewDocument', { source: `
      window.__dabSent = [];
      const o = WebSocket.prototype.send;
      WebSocket.prototype.send = function (d) {
        if (typeof d === 'string' && /"type":"dab"/.test(d)) window.__dabSent.push([performance.now(), d]);
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
    const rx = await until(`parseFloat(document.getElementById("freqInput").value) > 0`, 25000);
    ok(rx, 'the page is receiving');
    if (!rx) console.log('     page says: ' + JSON.stringify((await js(`document.body.innerText`) || '').slice(0, 400)));
    await js(`document.getElementById("mMode").click(); 1`);
    await until(`[...document.querySelectorAll(".mModeOpt")].some(b => b.textContent === "DAB")`);
    await js(`[...document.querySelectorAll(".mModeOpt")].find(b => b.textContent === "DAB").click(); 1`);
    const LBL = `(() => { const m = /\\((\\w+)\\)/.exec(document.getElementById("dabMuxLbl")?.textContent || ""); return m ? m[1] : ""; })()`;
    ok(await until(`${LBL} !== ""`, 15000), `in DAB on the page (label: ${await js(LBL)})`);
    await sleep(2500);
    const start = await js(LBL);
    const si = B.indexOf(start);
    const key = (k) => js(`(document.activeElement && document.activeElement.blur && document.activeElement.blur(),
                          window.dispatchEvent(new KeyboardEvent("keydown", { key: "${k}", bubbles: true })), 1)`);
    await js(`window.__dabSent.length = 0; 1`);
    const seen = [];
    for (let k = 0; k < 10; k++) { await key(','); seen.push(await js(LBL)); await sleep(120); }
    const want = B[si - 10];
    ok(seen.join(' ') === B.slice(si - 10, si).reverse().join(' '), `ten < presses walk the label ten blocks at once (${start} → ${seen.join(' ')})`);
    ok((await js(`window.__dabSent.length`)) === 0, 'nothing was sent while the presses were coming');
    // Watch the label from here: it must never go back to a block it has left.
    await js(`window.__lbl = []; window.__lblT = setInterval(() => { const m = /\\((\\w+)\\)/.exec(document.getElementById("dabMuxLbl")?.textContent || ""); window.__lbl.push(m ? m[1] : ""); }, 40); 1`);
    await sleep(2500);
    const sent = await js(`window.__dabSent.map(x => x[1])`);
    ok(sent.length === 1 && JSON.parse(sent[0]).channel === si - 10, `ONE \`dab\` sent, for ${want} (sent: ${sent.join(' | ') || 'nothing'})`);
    const lbls = await js(`(clearInterval(window.__lblT), [...new Set(window.__lbl)])`);
    ok(lbls.length === 1 && lbls[0] === want, `the label held ${want} while the server caught up — no snap-back (saw: ${lbls.join(' ')})`);
    ok(await until(`${LBL} === "${want}"`, 5000) && tunedSince(0).includes(want), `…and the server tuned ${want}`);

    // Press, look, press: two sends.
    await js(`window.__dabSent.length = 0; 1`);
    await key('.'); await sleep(1200); await key('.'); await sleep(1200);
    const two = await js(`window.__dabSent.map(x => JSON.parse(x[1]).channel)`);
    ok(two.join(',') === `${si - 9},${si - 8}`, `press, look a second, press: two sends, each as normal (${two.map((i) => B[i]).join(' ')})`);

    // Leaving DAB mid-burst: no block is sent.
    await js(`window.__dabSent.length = 0; 1`);
    await key(','); await key(',');
    await js(`document.getElementById("dabExit").click(); 1`);
    await sleep(1200);
    const ex = await js(`window.__dabSent.map(x => x[1])`);
    ok(ex.length === 1 && JSON.parse(ex[0]).on === 0, `EXIT mid-burst: only the exit is sent, the burst dies (${ex.join(' | ')})`);
    ok(errors.length === 0, 'no JavaScript errors' + (errors.length ? ': ' + errors.slice(0, 2).join(' | ') : ''));
    cws.close();
  }
} catch (e) { console.error(e); fails++; }
await cleanup();
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : '\n\x1b[32mall passed\x1b[0m');
process.exit(fails ? 1 : 0);
