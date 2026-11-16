#!/usr/bin/env node
// test-web-dx-dial.mjs — DX CATCHES ON THE DIAL, end to end (V12, 2026-11-16; src/utils/knownSignals.ts dxDialSignals,
// vibe_dxscan.h dialJson). Stuart: "show DX catches on the dial as signal labels too, marked as sporadic".
// A real vibeserver started on a SEEDED DX log (written as the server writes it — a week of scanning cannot be run here),
// the fake dongle on 93.2 MHz, and THE BINARY'S OWN PAGE (node scripts/build-web.mjs, then build vibeserver) in a real
// headless MUTED Edge:
//   A. GET /vibeserver/dxlog.json?dial=1: only the DX catches heard this week — never a Local, a Suspect, a Provisional,
//      a catch older than a week, or a catch on a Local's channel; one a channel; OIRT 69.80 ranged; the 12C DAB catch;
//   B. the page: the catch in view (93.5 MHz) is drawn — "RAI R1 · DX · 2 h ago", ITALIC in the DX colour — and the others
//      are in its list; the catch on the local's 92.5 is not; a CLICK on the label tunes 93.5 MHz;
//   C. ★★ NEVER A LEAK: the owner's "admin lock on viewing" on — dial=1 is 403 for a listener (200 for the owner), hwinfo
//      offers no DX Log, and the page holds no DX label.
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-dx-dial.mjs   (exit 3 = not run)
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — Edge needed)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const procs = [];
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dxdial-'));
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGTERM'); } catch {} } try { fs.rmSync(path.join(T, 'browser'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
const deadline = setTimeout(() => { console.log('   FAIL deadline (300 s)'); process.exit(1); }, 300000);

const ADMIN = 'dx-dial-admin';
const now = Math.floor(Date.now() / 1000), H = 3600, DAY = 86400;
const fmE = (id, pi, khz, ps, cls, heard, first = now - 3 * DAY) => ({ id, band: 'FM', key: `FM:${pi.toString(16).toUpperCase().padStart(4, '0')}:${khz}`,
  class: cls, khz, piN: pi, ps, psVariants: ps ? [ps] : [], firstHeard: first, lastHeard: heard, timesHeard: 12, presence: 0.05, lastSweep: 1 });
const ENTRIES = [
  fmE(1, 0xC201, 92500, 'BBC R4', 'local', now - 300, now - 20 * DAY),    // the local
  fmE(2, 0x5301, 92500, 'ON LOCAL', 'dx', now - 2 * H),                   // a catch on the local's channel → not on the dial
  fmE(3, 0x5201, 93500, 'RAI R1', 'dx', now - 2 * H - 60),                // ★ in view: drawn
  fmE(4, 0x5202, 98800, 'RNE 1', 'dx', now - 5 * H),
  fmE(5, 0x5203, 98800, 'OLDER', 'dx', now - 9 * H),                      // same channel, heard earlier → not
  fmE(6, 0, 69800, '', 'dx', now - 3 * DAY),                              // OIRT, no RDS
  fmE(7, 0x5204, 100500, 'TOO OLD', 'dx', now - 9 * DAY, now - 9 * DAY),  // beyond the week
  fmE(8, 0xC201, 92600, 'BBC R4', 'suspect', now - H),                    // the local's skirt
  fmE(9, 0x5205, 101000, 'BASELINE', 'provisional', now - H),
  { id: 10, band: 'DAB', key: 'DAB:E0F1:12C', class: 'dx', blk: 33, block: '12C', eidN: 0xE0F1, ecc: 0xE1, ensemble: 'Far Away',
    services: [{ sid: 0xF1, label: 'Far Radio' }], khz: 227360, firstHeard: now - 2 * DAY, lastHeard: now - DAY - H, timesHeard: 3, presence: 0.02, lastSweep: 1 },
];

async function startServer(tag, viewAdmin) {
  const dir = fs.mkdtempSync(path.join(T, `${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  fs.writeFileSync(path.join(dir, 'data', 'dxlog-dx-test.json'), JSON.stringify({ v: 1, created: now - 30 * DAY, nextId: 11, sweepsFm: 5, sweepsDab: 5,
    alertSeq: 0, fmCursorKhz: 87500, dabCursor: 0, nextDab: false, bootDone: true, entries: ENTRIES, alerts: [] }));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'DX Dial Test', locator: 'IO92nh', adminPass: ADMIN,
    radios: [{ serial: 'dx-test', label: 'Test radio', enabled: true, configured: true, users: 2, rate: 2400000,
               dxScan: JSON.stringify({ on: false, bands: 'fm', waitMin: 5, viewAdmin }) }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000',
                                      '--fm-rds', '93.5:5201:RAI_R1'], { stdio: 'ignore' }));
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '93200000', '--mode', 'wfm', '--users', '2'],
                   { env, stdio: ['ignore', log, log] }));
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  const base = `http://127.0.0.1:${port}`;
  return { port, base,
    adminQ: async () => { const nonce = (await fetch(`${base}/vibeserver/auth`).then((r) => r.json())).nonce;
      return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`; } };
}

// ── the browser ──
const cdp = await freePort();
procs.push(spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions',
  '--autoplay-policy=no-user-gesture-required', `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1280,900', 'about:blank'],
  { stdio: 'ignore' }));
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
const FT = `(() => { window.__ft = []; const f = CanvasRenderingContext2D.prototype.fillText;
  CanvasRenderingContext2D.prototype.fillText = function (t) { if (window.__ft.length > 3000) window.__ft.splice(0, 1500); window.__ft.push({ t: String(t), font: this.font, fill: String(this.fillStyle) }); return f.apply(this, arguments); }; })();`;

async function openPage(port) {
  await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  for (let i = 0; i < 4; i++) {
    await until(`!!document.getElementById("btnConnect") && document.readyState === "complete"`);
    await sleep(1500);
    await js(`document.getElementById("btnConnect")?.click(); 1`);
    if (await until(`!!document.getElementById("audioGate") || parseFloat(document.getElementById("freqInput")?.value) > 0`, 8000)) break;
  }
  // ★ START RADIO's gate needs a trusted click (the audio gate); it may come up a moment after the dial does
  await until(`!!document.querySelector("#audioGate button")`, 5000);
  for (let i = 0; i < 6; i++) {
    const g = await js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null; const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!g) break;
    await tap(g[0], g[1]);
    if (await until(`!document.getElementById("audioGate")`, 3000)) break;
  }
  return until(`!!(window.__vibeWf && window.__vibeWf()) && parseFloat(document.getElementById("freqInput").value) > 0`, 15000);
}
const LIST = `((window.__vibeWf().wf.knownSignals) || []).filter(s => s.src === 'dx').map(s => s.label)`;
const PLACED = `(window.__vibeWf().wf.knownPlaced || []).map(p => p.sig.label)`;

try {
  await call('Runtime.enable'); await call('Page.enable');
  await call('Page.addScriptToEvaluateOnNewDocument', { source: FT });

  console.log('── A. GET /vibeserver/dxlog.json?dial=1 ──');
  const S = await startServer('open', false);
  const d = await fetch(`${S.base}/vibeserver/dxlog.json?dial=1&limit=100`).then((r) => r.json());
  const ps = (d.entries || []).map((e) => e.ps || e.ensemble || String(e.freqKhz));
  console.log('      dial:', ps.join(' | '));
  ok(d.dial === true && ['RAI R1', 'RNE 1', 'Far Away'].every((n) => ps.includes(n)) && d.entries.some((e) => e.freqKhz === 69800 && e.range === 'oirt'),
     'the week\'s DX catches: RAI R1, RNE 1, OIRT 69.80, the 12C multiplex');
  ok(!['BBC R4', 'ON LOCAL', 'OLDER', 'TOO OLD', 'BASELINE'].some((n) => ps.includes(n)),
     '★ never a Local, a catch on a Local\'s channel, an older catch on a channel, one beyond the week, a Suspect or a Provisional');

  console.log('── B. the page (the binary\'s own) ──');
  ok(await openPage(S.port), 'the page is up and listening');
  ok(await until(`${LIST}.length >= 3`, 15000), `the DX catches reach the dial list (${(await js(LIST))?.join(' | ')})`);
  const list = await js(LIST);
  ok(list.includes('RAI R1 · DX · 2 h ago') && list.some((l) => l.startsWith('69.80 · DX · 3 days ago')), 'labelled "name · DX · when"; OIRT to two decimals');
  ok(!list.some((l) => l.startsWith('ON LOCAL')), '★ the local\'s channel carries no DX label');
  ok(await until(`${PLACED}.includes('RAI R1 · DX · 2 h ago')`, 8000), `the catch in view (93.5 MHz) is drawn (${(await js(PLACED))?.join(' | ')})`);
  await sleep(600);
  const ft = (await js(`window.__ft`)).filter((r) => r.t === 'RAI R1 · DX · 2 h ago');
  ok(ft.length > 0 && ft.every((r) => /italic/.test(r.font) && r.fill.toLowerCase() === '#ffe45c'), `★ ITALIC in the DX colour (${ft[0]?.font} ${ft[0]?.fill})`);
  // a click on its box tunes it
  const box = await js(`(() => { const { wf } = window.__vibeWf(); const c = wf.canvas, r = c.getBoundingClientRect();
    const p = wf.knownPlaced.find(q => q.sig.label === 'RAI R1 · DX · 2 h ago'); if (!p) return null;
    const k = c.width / c.clientWidth, ky = c.height / c.clientHeight, dpr = k;
    const y = wf.markerLabelY(c.height, p.row) - 9 * dpr + 6.5 * dpr; return [r.left + (p.lx + p.w / 2) / k, r.top + y / ky]; })()`);
  ok(!!box, 'its box is on the page');
  if (box) {
    const under = await js(`document.elementFromPoint(${box[0]}, ${box[1]})?.id || document.elementFromPoint(${box[0]}, ${box[1]})?.tagName`);
    ok(under === 'wf', `the point is the waterfall canvas (${under}, ${box.map((v) => v.toFixed(0)).join(',')})`);
    await tap(box[0], box[1]);
    ok(await until(`Math.abs(parseFloat(document.getElementById("freqInput").value) - 93.5) < 0.001`, 5000),
       `★ a click on the DX label tunes it (${await js(`document.getElementById("freqInput").value`)} MHz)`);
  }
  ok(!errors.length, `no page errors${errors.length ? ': ' + errors.slice(0, 2).join(' / ') : ''}`);

  console.log('── C. the owner keeps the log private (viewAdmin) ──');
  const P = await startServer('private', true);
  const r403 = await fetch(`${P.base}/vibeserver/dxlog.json?dial=1&limit=100`);
  ok(r403.status === 403, `a listener's dial=1 is refused (${r403.status})`);
  const rOwn = await fetch(`${P.base}/vibeserver/dxlog.json?dial=1&limit=100&${await P.adminQ()}`);
  ok(rOwn.status === 200 && (await rOwn.json()).entries.some((e) => e.ps === 'RAI R1'), 'the owner still gets it');
  ok(await openPage(P.port), 'the page is up on the private server');
  await sleep(4000);
  ok((await js(LIST)).length === 0 && !(await js(PLACED)).some((l) => l.includes(' · DX · ')), '★ no DX label on a listener\'s page — nothing leaks');
} catch (e) {
  console.log('   FAIL', e?.stack || e); fail++;
} finally {
  clearTimeout(deadline);
  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail ? 1 : 0);
}
