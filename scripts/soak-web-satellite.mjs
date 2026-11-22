// soak-web-satellite.mjs — ★★★ THE PAGE STAYS ALIVE IN SATELLITE MODE (docs/v12/WEB-KEEPALIVE.md). Stuart's Pi 500, 2026-11-22:
//   the web client froze and reconnected about once a minute with the SATELLITE window open (ALL SATELLITES unfolded, the
//   half-globe, AUTO DOPPLER retuning the dial) — the server logged "no application message … for 30 s" each time, so the
//   page's own 5 s ping had not gone out for 30 s while he was looking at it.
//   This runs the REAL server (fake rtl_tcp, CelesTrak fixture with its epochs moved to an hour ago, the server placed
//   UNDER ISS so a pass is up) and the BUILT page in headless Edge with --mute-audio (tests are silent), picks ISS with
//   AUTO DOPPLER, unfolds ALL SATELLITES, and soaks. Every SOAK_SAMPLE_S it records: the longest task (PerformanceObserver
//   'longtask'), the worst main-thread timer lateness, the longest gap between pings LEAVING the page, the JS heap, DOM
//   nodes and listeners (CDP Performance.getMetrics). At the end: no "no application message" in the server log, the
//   sockets never reconnected, pings never more than 10 s apart, the heap not growing without bound.
//     VIBESERVER_BIN=…/vibeserver [SOAK_MIN=10] [SOAK_HIDDEN=1] [SOAK_MUTED=1] node scripts/soak-web-satellite.mjs
//   SOAK_HIDDEN=1 also hides the page (CDP Emulation.setFocusEmulationEnabled false + Page.setWebLifecycleState? no —
//   'hidden' is emulated with Emulation.setAutoDarkModeOverride's sibling: see hidePage below) after the pick.
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
const SOAK_MIN = Number(process.env.SOAK_MIN || 10), SAMPLE_S = Number(process.env.SOAK_SAMPLE_S || 30);
const HIDDEN = process.env.SOAK_HIDDEN === '1', MUTED = process.env.SOAK_MUTED === '1';
const RATE = 2400000, PASS = 'soak-sat-admin';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

function locator(lat, lon) {
  const L = 'ABCDEFGHIJKLMNOPQRSTUVWX';
  let x = lon + 180, y = lat + 90;
  const a = L[Math.floor(x / 20)], b = L[Math.floor(y / 10)];
  x %= 20; y %= 10;
  const c = Math.floor(x / 2), d = Math.floor(y);
  x -= c * 2; y -= d;
  return `${a}${b}${c}${d}${L[Math.floor(x * 12)].toLowerCase()}${L[Math.floor(y * 24)].toLowerCase()}`;
}
function freshSats() {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-6digit-2026-11-19.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  return JSON.stringify(d);
}
const children = [];
const dirs = [];
async function server(tag, loc) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-soak-${tag}-`)); dirs.push(dir);
  for (const d of ['data', 'run', 'data/sat', 'edge']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
  const t = Math.floor(Date.now() / 1000);
  fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), `checked ${t}\nattempt ${t}\nseed 0\n`);
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Satellite soak', locator: loc, adminPass: PASS,
    radios: [{ serial: `${tag}-soak`, label: 'Test radio', enabled: true, configured: true, users: 4 }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn('nice', ['-n', '10', process.execPath, path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' });
  children.push(rtl);
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '145800000', '--mode', 'nfm',
    '--users', '4', '--admin-pass', PASS], { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  children.push(srv);
  for (let i = 0; i < 150; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(1000) }); break; } catch { await sleep(100); } }
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, L, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} } };
}
/** sat_list over a bare socket — where is ISS now? */
async function issNow(port) {
  return new Promise((res) => {
    const key = crypto.randomBytes(16).toString('base64');
    const s = net.connect(port, '127.0.0.1');
    let buf = Buffer.alloc(0), hs = false, done = false;
    const fin = (v) => { if (done) return; done = true; try { s.destroy(); } catch {} res(v); };
    setTimeout(() => fin(null), 6000);
    s.on('connect', () => s.write(`GET /ws/user-spectrum?user_session_id=soakfind1&bins=256&proto=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
    s.on('error', () => fin(null));
    const sendText = (o) => { const p = Buffer.from(JSON.stringify(o)); const m = crypto.randomBytes(4); const h = p.length < 126 ? Buffer.from([0x81, 0x80 | p.length]) : Buffer.from([0x81, 0xfe, p.length >> 8, p.length & 255]);
      const x = Buffer.alloc(p.length); for (let i = 0; i < p.length; i++) x[i] = p[i] ^ m[i & 3]; s.write(Buffer.concat([h, m, x])); };
    s.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      if (!hs) { const i = buf.indexOf('\r\n\r\n'); if (i < 0) return; hs = true; buf = buf.subarray(i + 4); sendText({ type: 'sat_list' }); }
      for (;;) {
        if (buf.length < 2) return;
        let len = buf[1] & 127, off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; } else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (buf.length < off + len) return;
        const op = buf[0] & 15, pl = buf.subarray(off, off + len); buf = buf.subarray(off + len);
        if (op === 1) { const t = pl.toString(); if (t.includes('"type":"sat_list"')) { try { fin(JSON.parse(t).sats.find((x) => x.id === 'ISS')); } catch { fin(null); } } }
      }
    });
  });
}

// ★ The page-side recorder, installed before the page's own scripts: every ping that LEAVES the page (WebSocket.send),
//   long tasks, the main thread's timer lateness, socket opens (a reconnect is a second open of the same kind).
const RECORDER = `(() => {
  const S = window.__soak = { pings: [], sends: 0, opens: {}, long: [], lateMax: 0, lateSum: 0, lateN: 0, closes: 0 };
  const send0 = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) {
    S.sends++;
    if (typeof d === 'string' && d.indexOf('"type":"ping"') >= 0) S.pings.push({ t: performance.now(), u: String(this.url).replace(/\\?.*$/, '') });
    if (S.pings.length > 4000) S.pings.splice(0, 2000);
    return send0.call(this, d);
  };
  const WS0 = window.WebSocket;
  window.WebSocket = function (u, p) { const w = p === undefined ? new WS0(u) : new WS0(u, p); const k = String(u).replace(/\\?.*$/, '').replace(/^.*\\/ws\\//, '');
    S.opens[k] = (S.opens[k] || 0) + 1; w.addEventListener('close', () => { S.closes++; }); return w; };
  window.WebSocket.prototype = WS0.prototype; Object.assign(window.WebSocket, { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 });
  try { new PerformanceObserver((l) => { for (const e of l.getEntries()) { S.long.push({ t: e.startTime, d: e.duration }); if (S.long.length > 2000) S.long.splice(0, 1000); } }).observe({ type: 'longtask', buffered: true }); } catch (e) {}
  let last = performance.now();
  setInterval(() => { const n = performance.now(), late = n - last - 250; last = n; if (late > S.lateMax) S.lateMax = late; S.lateSum += Math.max(0, late); S.lateN++; }, 250);
})();`;

let edge = null, wsC = null;
try {
  console.log('── where is ISS now ──');
  const F = await server('find', 'IO92ng');
  const iss = await issNow(F.port);
  F.stop();
  ok(!!iss && Number.isFinite(iss.lat), `ISS sub-point ${iss?.lat}, ${iss?.lon}`);
  const loc = iss ? locator(iss.lat, iss.lon) : 'IO92ng';
  const S = await server('soak', loc);
  console.log(`── soak ${SOAK_MIN} min, the server under ISS (${loc})${HIDDEN ? ', page HIDDEN' : ''}${MUTED ? ', MUTED' : ''} ──`);
  const dbg = await freePort();
  edge = spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    '--autoplay-policy=no-user-gesture-required', `--user-data-dir=${path.join(S.dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  children.push(edge);
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); setTimeout(() => { if (pend.has(i)) { pend.delete(i); res({ timeout: true }); } }, 20000); });
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
  await send('Page.enable'); await send('Runtime.enable'); await send('Performance.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: RECORDER });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 2, mobile: false });
  await send('Page.navigate', { url: `http://127.0.0.1:${S.port}/` });
  const startSel = `[...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`;
  await until(`!!${startSel}`, 15000);
  await click(startSel);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await click(`document.querySelector('#audioGate button')`);
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000), 'START RADIO pressed');
  await sleep(2500);
  // SATELLITE from the mode menu
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(500);
  const satSel = `[...document.querySelectorAll('.mModeOpt')].find(x => /SATELLITE/.test(x.textContent))`;
  ok(await until(`!!${satSel}`, 8000), 'the mode menu offers SATELLITE');
  await ev(`${satSel}?.click()`);
  ok(await until(`!!document.querySelector('.sat-row')`, 10000), 'the SATELLITE window lists birds');
  const nRows = await ev(`document.querySelectorAll('.sat-row').length`);
  console.log(`      ${nRows} rows`);
  // pick ISS (AUTO DOPPLER is on by default — say so if not)
  const issRow = `[...document.querySelectorAll('.sat-row')].find(b => /^ISS/.test(b.querySelector('.sat-n')?.textContent || '') && !b.classList.contains('grey'))`;
  await ev(`${issRow}?.click()`);
  ok(await until(`!!document.querySelector('.sat-row.on')`, 10000), 'ISS picked');
  const dopOn = await ev(`document.querySelector('[data-act="doppler"]')?.getAttribute('aria-pressed')`);
  if (dopOn !== 'true') await ev(`document.querySelector('[data-act="doppler"]')?.click()`);
  await sleep(1500);
  // ★ ALL SATELLITES unfolded, as on Stuart's screen
  await ev(`(() => { const d = document.querySelector('details.sat-list'); if (d) d.open = true; return !!d; })()`);
  ok(await ev(`document.querySelector('details.sat-list')?.open === true`), 'ALL SATELLITES unfolded');
  if (MUTED) { await ev(`(document.getElementById('muteBtn') || document.querySelector('[data-act="mute"]'))?.click()`); }
  if (HIDDEN) {
    // ★ the page as a background tab: visibilityState 'hidden' (CDP has no direct switch in headless; a second target in
    //   front makes this one hidden)
    await fetch(`http://127.0.0.1:${dbg}/json/new?about:blank`, { method: 'PUT' }).catch(() => {});
    await sleep(1000);
  }
  const vis = await ev(`document.visibilityState`);
  console.log(`      visibilityState ${vis}`);
  await ev(`(() => { const s = window.__soak; s.long.length = 0; s.lateMax = 0; s.pings.length = 0; s.opens0 = JSON.stringify(s.opens); })()`);
  const metrics = async () => { const r = await send('Performance.getMetrics'); const m = {}; for (const x of r.result?.metrics || []) m[x.name] = x.value; return m; };
  const m0 = await metrics();
  const t0 = Date.now();
  const rows = [];
  let lastPingT = null;
  console.log('      min  longest-task  late-max  ping-gap-max  heap-MB  nodes  listeners  opens');
  while (Date.now() - t0 < SOAK_MIN * 60e3) {
    await sleep(SAMPLE_S * 1000);
    const s = JSON.parse(await ev(`(() => { const s = window.__soak; const out = { long: Math.max(0, ...s.long.map(x => x.d)), lateMax: s.lateMax, pings: s.pings.slice(), opens: s.opens, closes: s.closes };
      s.long.length = 0; s.lateMax = 0; s.pings.length = 0; return JSON.stringify(out); })()`) || '{}');
    const m = await metrics();
    const spec = (s.pings || []).filter((p) => /spectrum/.test(p.u)).map((p) => p.t);
    let gap = 0;
    if (lastPingT != null && spec.length) gap = spec[0] - lastPingT;
    for (let i = 1; i < spec.length; i++) gap = Math.max(gap, spec[i] - spec[i - 1]);
    if (spec.length) lastPingT = spec[spec.length - 1];
    const row = { min: (Date.now() - t0) / 60e3, long: s.long, late: s.lateMax, gap, heap: (m.JSHeapUsedSize || 0) / 1048576, nodes: m.Nodes, listeners: m.JSEventListeners, opens: JSON.stringify(s.opens), pings: spec.length };
    rows.push(row);
    console.log(`      ${row.min.toFixed(1).padStart(4)}  ${row.long.toFixed(0).padStart(10)} ms  ${row.late.toFixed(0).padStart(6)} ms  ${row.gap.toFixed(0).padStart(9)} ms  ${row.heap.toFixed(1).padStart(7)}  ${String(row.nodes).padStart(5)}  ${String(row.listeners).padStart(8)}  ${row.opens}`);
  }
  const log = S.L();
  const dead = (log.match(/no application message/g) || []).length;
  ok(dead === 0, `the server never released the session for silence (${dead} times)`);
  const opens1 = rows.length ? rows[rows.length - 1].opens : '{}';
  const opens0 = await ev(`window.__soak.opens0`);
  ok(opens1 === opens0, `no socket reopened during the soak (${opens0} → ${opens1})`);
  const gapMax = Math.max(...rows.map((r) => r.gap));
  ok(gapMax < 10000, `pings never more than 10 s apart (worst ${gapMax.toFixed(0)} ms)`);
  const longMax = Math.max(...rows.map((r) => r.long));
  console.log(`      longest task over the soak ${longMax.toFixed(0)} ms; heap ${((m0.JSHeapUsedSize || 0) / 1048576).toFixed(1)} → ${rows.length ? rows[rows.length - 1].heap.toFixed(1) : '?'} MB; nodes ${m0.Nodes} → ${rows.length ? rows[rows.length - 1].nodes : '?'}`);
  ok(longMax < 1000, `no task over 1 s (longest ${longMax.toFixed(0)} ms)`);
  if (rows.length >= 4) {
    const h = rows.map((r) => r.heap), half = Math.floor(h.length / 2);
    const a = h.slice(0, half).reduce((x, y) => x + y, 0) / half, b = h.slice(half).reduce((x, y) => x + y, 0) / (h.length - half);
    ok(b < a * 1.5 + 5, `the heap is not climbing (first half avg ${a.toFixed(1)} MB, second ${b.toFixed(1)} MB)`);
    const n = rows.map((r) => r.nodes);
    ok(n[n.length - 1] < n[0] * 1.5 + 500, `DOM nodes bounded (${n[0]} → ${n[n.length - 1]})`);
  }
  S.stop();
} catch (e) { ok(false, `driver: ${e?.stack || e}`); }
finally {
  try { wsC?.close(); } catch {}
  for (const c of children) { try { c.kill('SIGTERM'); } catch {} }
  await sleep(800);
  for (const d of dirs) { if (fail === 0) { try { fs.rmSync(d, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} } else { try { fs.rmSync(path.join(d, 'edge'), { recursive: true, force: true }); } catch {} console.log(`   (kept ${d})`); } }
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
