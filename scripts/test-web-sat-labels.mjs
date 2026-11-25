#!/usr/bin/env node
// test-web-sat-labels.mjs — beta91 SATELLITE LABELS ON THE WATERFALL, end to end (vibe_sat_engine.h skyLabels, the shim's
// sat_labels, knownSignals.ts satLabelSignals, waterfall.ts). Stuart 2026-11-22: "we dont even need to be in satellite
// mode either, i could simply be listening to 2m band and if it appears we can label it".
// Synthetic only: a fake rtl_tcp (no satellite on it — the label is the MODEL's), the mirror-built sats.json fixture
// with ORBCOMM label-only birds (sats-orbcomm-SYNTHETIC-2026-11-22.json), every EPOCH moved to an hour ago (as
// test-server-sat.mjs does), no network (the sync state says "just checked"):
//   A. where the ISS is now (sat_list), then a server UNDER it on 145.800 NFM (2 m, NOT satellite mode): the spectrum
//      socket gets `sat_labels` with the ISS's 2 m downlinks at their Doppler-shifted frequency, inside the capture only
//      (no 70 cm entry), never an ORBCOMM bird outside 137 MHz; they move between messages; ★★ 2026-11-24 sat_list lists
//      ORBCOMM as regular picks (decoder "orbcomm"), the other 14 birds as before;
//   B. THE BINARY'S OWN PAGE in a real headless MUTED Edge: the label "ISS Voice/SSTV" is drawn, in the satellite sky
//      blue, and a screenshot is written (SAT_LABELS_SHOT, default the OS temp dir);
//   C. the owner's switch "satlabels" off: only the empty message; and a server with no location sends nothing named.
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-sat-labels.mjs   (exit 3 = not run)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
const SHOT = process.env.SAT_LABELS_SHOT || path.join(os.tmpdir(), 'sat-labels-web.png');
const RATE = 2400000, PASS = 'sat-label-admin';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };
const procs = [], dirs = [];
const cleanup = () => { for (const p of procs) { try { p.kill('SIGTERM'); } catch {} } for (const d of dirs) { try { fs.rmSync(d, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} } };
process.on('exit', cleanup);
const deadline = setTimeout(() => { console.log('   FAIL deadline (240 s)'); cleanup(); process.exit(1); }, 240000);

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x1) { const t = p.toString(); if (t.includes('"type":"sat')) st.txt.push(t); }
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
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };
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
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-orbcomm-SYNTHETIC-2026-11-22.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  return JSON.stringify(d);
}
const syncState = () => { const t = Math.floor(Date.now() / 1000); return `checked ${t}\nattempt ${t}\nseed 0\n`; };

async function server(tag, { loc = 'IO92ng', radio = {}, freq = 145800000 } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  dirs.push(dir);
  for (const d of ['data', 'run', 'data/sat']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
  fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), syncState());
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Satellite labels test', ...(loc ? { locator: loc } : {}), adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, rate: RATE, ...radio }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' }));
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), VIBESERVER_SATS_TEST_URL: 'http://127.0.0.1:9/none' };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(freq), '--mode', 'nfm',
                          '--users', '4', '--admin-pass', PASS], { env, stdio: ['ignore', log, log] });
  procs.push(srv);
  const status = async () => { try { return await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) })).json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, dir, stop: () => { try { srv.kill('SIGTERM'); } catch {} } };
}
const sock = async (S, id) => { const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`); await until(() => sp.open, 3000); await sleep(800); return sp; };

let issLoc = '';
try {
  console.log('── A. where the ISS is now; a server under it on 2 m ──');
  {
    const S = await server('slfind');
    const sp = await sock(S, 'slfind001');
    sp.send({ type: 'sat_list' });
    ok(await until(() => !!lastOf(sp, 'sat_list'), 5000), 'sat_list answered');
    const l = lastOf(sp, 'sat_list');
    const iss = l?.sats?.find((s) => s.id === 'ISS');
    ok(!!iss && Number.isFinite(iss.lat), `ISS sub-point ${iss?.lat}, ${iss?.lon}`);
    {   // ★★ 2026-11-24 ORBCOMM is a REGULAR satellite now (docs/v12/ORBCOMM.md): picks with the ORBCOMM decoder, use packet
      const orb = (l?.sats ?? []).filter((s) => /ORBCOMM/.test(s.name));
      ok(!!l && l.sats.length - orb.length === 14 && orb.length > 0
         && orb.every((s) => s.tx.length > 0 && s.tx.every((t) => t.decoder === 'orbcomm' && t.use === 'packet')),
         `★ sat_list: the 14 birds + ${orb.length} ORBCOMM picks, each with the ORBCOMM decoder (${l?.sats?.length} birds)`);
    }
    issLoc = iss ? locator(iss.lat, iss.lon) : 'IO92ng';
    sp.close(); S.stop();
  }
  const S = await server('sl2m', { loc: issLoc });
  const sp = await sock(S, 'sl2m00001');
  ok(await until(() => jsons(sp, 'sat_labels').some((m) => m.sats.length), 12000), `sat_labels arrives under the ISS (${issLoc}), not in satellite mode`);
  const m0 = jsons(sp, 'sat_labels').find((m) => m.sats.length);
  // ★ 11-25: the server is put under the ISS's sub-point of a moment ago, so its FIRST label can land at closest approach,
  //   where the true Doppler is ~0 (23 Hz, then −202 Hz 4 s later — the server now starts within about a second). Judge
  //   the first label once the bird is past overhead (|shift| > 50 Hz), within the same window.
  const vOf = (m) => m?.sats?.find((s) => s.id === 'ISS' && s.tx === 'Voice / SSTV');
  await until(() => jsons(sp, 'sat_labels').some((m) => Math.abs((vOf(m)?.hz ?? 145800000) - 145800000) > 50), 12000);
  const v = vOf(jsons(sp, 'sat_labels').find((m) => Math.abs((vOf(m)?.hz ?? 145800000) - 145800000) > 50)) ?? vOf(m0);
  ok(!!v && Math.abs(v.hz - 145800000) > 50 && Math.abs(v.hz - 145800000) < 4000 && v.m === 'nfm' && v.el > 30,
     `ISS Voice / SSTV at its Doppler-shifted ${v ? (v.hz / 1e6).toFixed(4) : '?'} MHz (${v ? v.hz - 145800000 : '?'} Hz, el ${v?.el})`);
  ok(!!m0?.sats?.find((s) => s.id === 'ISS' && s.tx === 'APRS'), 'its APRS downlink too');
  ok(!!m0 && m0.sats.every((s) => s.hz >= 145800000 - RATE / 2 && s.hz <= 145800000 + RATE / 2), '★ only inside this radio\'s capture (no 70 cm entry)');
  ok(!!m0 && !m0.sats.some((s) => s.b), 'no ORBCOMM outside 137 MHz');
  ok(await until(() => jsons(sp, 'sat_labels').length >= 2, 9000), `again ~4 s later (${jsons(sp, 'sat_labels').length} messages)`);
  const ms = jsons(sp, 'sat_labels').map((m) => m.sats.find((s) => s.id === 'ISS' && s.tx === 'Voice / SSTV')?.hz).filter(Boolean);
  ok(ms.length >= 2 && ms[ms.length - 1] !== ms[0], `the mark follows the Doppler (${ms.map((h) => h - 145800000).join(' → ')} Hz)`);

  console.log('── B. the binary\'s own page in headless Edge ──');
  if (!fs.existsSync(EDGE)) console.log('   (Edge not found — page check skipped)');
  else {
    const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-sl-edge-')); dirs.push(T);
    const cdp = await freePort();
    procs.push(spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions',
      '--autoplay-policy=no-user-gesture-required', `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1280,900', 'about:blank'],
      { stdio: 'ignore' }));
    let target;
    for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
    const cws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((r) => (cws.onopen = r));
    let id = 0; const pending = new Map(); const errors = [];
    cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
      if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
    const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); });
    const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
    const jsUntil = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(200); } return false; };
    const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1, pointerType: 'mouse' }); };
    const FT = `(() => { window.__ft = []; const f = CanvasRenderingContext2D.prototype.fillText;
      CanvasRenderingContext2D.prototype.fillText = function (t) { if (window.__ft.length > 3000) window.__ft.splice(0, 1500); window.__ft.push({ t: String(t), fill: String(this.fillStyle) }); return f.apply(this, arguments); }; })();`;
    await call('Runtime.enable'); await call('Page.enable');
    await call('Page.addScriptToEvaluateOnNewDocument', { source: FT });
    await call('Page.navigate', { url: `http://127.0.0.1:${S.port}/` });
    for (let i = 0; i < 4; i++) {
      await jsUntil(`!!document.getElementById("btnConnect") && document.readyState === "complete"`);
      await sleep(1500);
      await js(`document.getElementById("btnConnect")?.click(); 1`);
      if (await jsUntil(`!!document.getElementById("audioGate") || parseFloat(document.getElementById("freqInput")?.value) > 0`, 8000)) break;
    }
    await jsUntil(`!!document.querySelector("#audioGate button")`, 5000);
    for (let i = 0; i < 6; i++) {
      const g = await js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null; const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
      if (!g) break;
      await tap(g[0], g[1]);
      if (await jsUntil(`!document.getElementById("audioGate")`, 3000)) break;
    }
    ok(await jsUntil(`!!(window.__vibeWf && window.__vibeWf()) && parseFloat(document.getElementById("freqInput").value) > 0`, 15000), 'the page is up and listening');
    const SATS = `((window.__vibeWf().wf.knownSignals) || []).filter(s => s.src === 'sat').map(s => s.label)`;
    const PLACED = `(window.__vibeWf().wf.knownPlaced || []).map(p => p.sig.label)`;
    ok(await jsUntil(`${SATS}.includes('ISS Voice/SSTV')`, 15000), `the satellites reach the label list (${(await js(SATS))?.join(' | ')})`);
    ok(await jsUntil(`${PLACED}.includes('ISS Voice/SSTV')`, 8000), `★ "ISS Voice/SSTV" is drawn on the ordinary waterfall (${(await js(PLACED))?.join(' | ')})`);
    await sleep(800);
    const ft = (await js(`window.__ft`)).filter((r) => r.t === 'ISS Voice/SSTV');
    ok(ft.length > 0 && ft.some((r) => r.fill.toLowerCase() === '#a8d8ff'), `in the satellites' sky blue (${ft[0]?.fill})`);
    const shot = await call('Page.captureScreenshot', { format: 'png' });
    if (shot.result?.data) { fs.writeFileSync(SHOT, Buffer.from(shot.result.data, 'base64')); console.log(`      screenshot: ${SHOT}`); }
    ok(!errors.length, `no page errors${errors.length ? ': ' + errors.slice(0, 2).join(' / ') : ''}`);
    try { cws.close(); } catch {}
  }
  sp.close(); S.stop();

  console.log('── C. the owner\'s switch; no location ──');
  {
    const O = await server('sloff', { loc: issLoc, radio: { blockedModes: 'satlabels' } });
    const so = await sock(O, 'sloff0001');
    await sleep(6000);
    const all = jsons(so, 'sat_labels');
    ok(all.length >= 1 && all.every((m) => m.sats.length === 0), `"satlabels" switched off: only the empty message (${all.length})`);
    so.close(); O.stop();
    const N = await server('slnoloc', { loc: '' });
    const sn = await sock(N, 'slnoloc01');
    await sleep(6000);
    ok(jsons(sn, 'sat_labels').every((m) => m.sats.length === 0), 'no location: nothing named');
    sn.close(); N.stop();
  }
} catch (e) {
  console.log('   FAIL', e?.stack || e); fail++;
} finally {
  clearTimeout(deadline);
  console.log(`\n${pass} passed, ${fail} failed`);
  cleanup();
  process.exit(fail ? 1 : 0);
}
