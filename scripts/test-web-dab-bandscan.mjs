#!/usr/bin/env node
// test-web-dab-bandscan.mjs — the DAB BAND SCAN on the served receiver page (V12, 2026-10-26; M7EIW: "a scan function for
// DAB which also shows the results"). Real server, the bench multiplex played ONLY on 11D (fake-rtl-tcp --dab-at), a real
// headless MUTED Edge, desktop and then 390 px.
//   1. single-user radio: the DAB box offers SCAN; pressing it shows the scan pane with progress and CANCEL;
//   2. the result list fills as it goes and ends with 11D — name, frequency, "12 stations", quality — and the closing
//      line "… stations added to bookmarks"; CANCEL gives way to STATIONS;
//   3. a row expands to its station list; at 390 px nothing overflows sideways and the row's parts stay on one line;
//   4. tapping a station tunes it: the station list is back, on 11D, that station playing;
//   5. a SHARED radio (--users 3): no SCAN key at all.
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-dab-bandscan.mjs   (exit 3 = not run)
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const CLIP = path.join(ROOT, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.error('no bench clip (vibeserver/bench-clip/make-clip.sh) — not run'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(BROWSER)) { console.error('no Edge — not run'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };

const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-dabband-'));
const raw = path.join(T, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const procs = [];
const rtlPort = await freePort();
procs.push(spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw, '--dab-at', '222064000'], { stdio: 'ignore' }));
await sleep(400);

async function server(users, tag) {
  const D = path.join(T, tag);
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(D, d), { recursive: true });
  fs.writeFileSync(path.join(D, 'config.json'), JSON.stringify({ configured: true, name: 'Web band scan', locator: 'IO92nh', adminPass: 'web-dabband-admin',
    radios: [{ serial: 'webdab' + tag, label: 'T', enabled: true, configured: true, users }] }));
  const port = await freePort();
  const p = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', String(users)], {
    env: { ...process.env, VIBESERVER_CONFIG: path.join(D, 'config.json'), VIBESERVER_DATA_DIR: path.join(D, 'data'), VIBESERVER_RUNTIME_DIR: path.join(D, 'run'), HOME: D },
    stdio: 'ignore' });
  procs.push(p);
  for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  return { port, proc: p };
}
const cdp = await freePort();
const br = spawn(BROWSER, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions', '--autoplay-policy=no-user-gesture-required',
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
procs.push(br);
async function open(url) {
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
  const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
  await call('Runtime.enable'); await call('Page.enable');
  await call('Page.navigate', { url });
  return { ws, js, until, tap, call, errors };
}
const startRadio = async (p) => {
  await p.until(`!!document.getElementById("audioGate")`, 20000);
  for (let i = 0; i < 6; i++) {
    const at = await p.js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null;
      const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!at) break;
    await p.tap(at[0], at[1]);
    if (await p.until(`!document.getElementById("audioGate")`, 3000)) break;
  }
};
const intoDab = async (p) => {
  await p.until(`!!document.getElementById("btnConnect")`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await startRadio(p);
  await p.until(`parseFloat(document.getElementById("freqInput").value) > 0`);
  await p.js(`document.getElementById("mMode").click(); 1`);
  await p.until(`[...document.querySelectorAll(".mModeOpt")].some(b => b.textContent === "DAB")`);
  await p.js(`[...document.querySelectorAll(".mModeOpt")].find(b => b.textContent === "DAB").click(); 1`);
  return p.until(`document.getElementById("decTitle")?.textContent === "DAB" && document.getElementById("dabPane")?.style.display !== "none"`, 20000);
};
const shown = (id) => `(() => { const e = document.getElementById("${id}"); return !!e && e.style.display !== "none" && e.offsetParent !== null; })()`;

try {
  console.log('the DAB band scan on the served page — a single-user radio, the bench multiplex on 11D');
  const one = await server(1, 'one');
  const p = await open(`http://127.0.0.1:${one.port}/`);
  ok(await intoDab(p), 'into DAB from the mode menu');
  ok(await p.until(shown('dabScanBtn'), 10000), 'the DAB box offers SCAN on a single-user radio');
  await p.js(`document.getElementById("dabScanBtn").click(); 1`);
  ok(await p.until(shown('dabScanPane'), 5000), 'pressing it shows the scan pane');
  ok(await p.until(`/^Scanning \\d+ \\/ 41 blocks/.test(document.getElementById("dabScanLine").textContent)`, 10000),
     `progress reads "${await p.js(`document.getElementById("dabScanLine").textContent`)}"`);
  ok(await p.js(shown('dabScanStop')) && !(await p.js(shown('dabScanBack'))) && !(await p.js(shown('dabScanBtn'))), 'CANCEL while it runs; no second SCAN key');
  ok(await p.until(`parseFloat(document.querySelector("#dabScanBar b").style.width) > 0`, 15000), 'the progress bar moves');
  ok(await p.until(`!!document.querySelector('.dabScanHd[data-blk="11D"]')`, 200000), '11D appears in the list while the scan is still going');
  ok(await p.until(`/added to bookmarks/.test(document.getElementById("dabScanLine").textContent)`, 120000),
     `it ends: "${await p.js(`document.getElementById("dabScanLine").textContent`)}"`);
  const row = await p.js(`(() => { const h = document.querySelector('.dabScanHd[data-blk="11D"]'); return h && { ens: h.querySelector(".ens").textContent, cnt: h.querySelector(".cnt").textContent, q: h.querySelector(".q").textContent }; })()`);
  ok(row && /VibeSDR Bench/.test(row.ens) && /222\.064 MHz/.test(row.ens), `the row names the ensemble and its frequency (${row?.ens})`);
  ok(row && row.cnt === '12 stations' && row.q === 'STRONG', `…its station count and quality (${row?.cnt}, ${row?.q})`);
  ok(/12 stations added to bookmarks/.test(await p.js(`document.getElementById("dabScanLine").textContent`)), 'the closing line says how many were added to bookmarks');
  ok(await p.js(shown('dabScanBack')) && !(await p.js(shown('dabScanStop'))), 'CANCEL gives way to STATIONS');
  await p.js(`document.querySelector('.dabScanHd[data-blk="11D"]').click(); 1`);
  ok(await p.until(`document.querySelectorAll(".dabScanSvc").length === 13`), 'the row expands to its 12 stations (+ tune to the block)');

  // ── 390 px ──
  await p.call('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(800);
  const fit = await p.js(`(() => {
    const pane = document.getElementById("dabScanPane"), box = document.getElementById("decBox");
    const hd = document.querySelector('.dabScanHd[data-blk="11D"]'), r = hd.getBoundingClientRect();
    const kids = [...hd.children].map(k => k.getBoundingClientRect());
    return { paneOver: pane.scrollWidth - pane.clientWidth, docOver: document.documentElement.scrollWidth - innerWidth,
             oneLine: kids.every(k => Math.abs(k.top + k.height / 2 - (r.top + r.height / 2)) < 12),
             inside: kids.every(k => k.right <= r.right + 1 && k.left >= r.left - 1), boxW: box.getBoundingClientRect().width };
  })()`);
  ok(fit && fit.paneOver <= 1 && fit.docOver <= 1, `at 390 px nothing overflows sideways (pane ${fit?.paneOver}, page ${fit?.docOver})`);
  ok(fit && fit.oneLine && fit.inside, `…the block, name, count and quality sit on one line inside the row (box ${Math.round(fit?.boxW)} px)`);
  await p.call('Page.captureScreenshot', { format: 'png' }).then((r) => { if (process.env.SHOT) fs.writeFileSync(process.env.SHOT, Buffer.from(r.result.data, 'base64')); });
  await p.call('Emulation.clearDeviceMetricsOverride');
  await sleep(500);

  // ── tap a station ──
  const target = await p.js(`(() => { const s = document.querySelectorAll(".dabScanSvc")[3]; return s && { sid: s.dataset.sid, label: s.textContent }; })()`);
  await p.js(`document.querySelectorAll(".dabScanSvc")[3].click(); 1`);
  ok(await p.until(shown('dabStations'), 5000), 'tapping a station goes back to the station list');
  ok(await p.until(`document.querySelector('#dabStations .dabSvc.on')?.dataset.sid === "${target.sid}"`, 25000), `…on 11D, with that station playing (${target.label})`);
  ok(await p.js(`/11D/.test(document.getElementById("dabMuxLbl").textContent)`), `the multiplex bar reads 11D (${await p.js(`document.getElementById("dabMuxLbl").textContent`)})`);
  ok(p.errors.length === 0, 'no JavaScript errors' + (p.errors.length ? ': ' + p.errors.slice(0, 2).join(' | ') : ''));
  p.ws.close();
  one.proc.kill(); await sleep(500);

  console.log('a shared radio: no SCAN key');
  const shared = await server(3, 'shared');
  const q = await open(`http://127.0.0.1:${shared.port}/`);
  ok(await intoDab(q), 'into DAB');
  await sleep(2500);
  ok(!(await q.js(shown('dabScanBtn'))) && (await q.js(`document.getElementById("dabScanBtn").style.display`)) === 'none', 'the SCAN key is not drawn (absent, not greyed)');
  ok(q.errors.length === 0, 'no JavaScript errors');
  q.ws.close();
} catch (e) { console.error(e); fails++; }
for (const p of procs) { try { p.kill(); } catch {} }
await sleep(600);
fs.rmSync(T, { recursive: true, force: true });
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : '\n\x1b[32mall passed\x1b[0m');
process.exit(fails ? 1 : 0);
