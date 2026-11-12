#!/usr/bin/env node
// test-web-dab-vfo.mjs — DAB'S VFO IS THE ENSEMBLE on the served receiver page (V12, 2026-11-12; Stuart, a B210 on a ~7 MHz
// span with 7D's multiplex filling ~1.5 MHz under a ~200 kHz passband: "we need an extra wide VFO for things like DAB").
// Real server, the bench multiplex played on 7D (194.064 MHz) by fake-rtl-tcp, a real headless MUTED Edge:
//   1. into DAB on 7D: the needle reads "DAB 7D · 194.064M" and the drawn passband is the 1.536 MHz block about the
//      block's centre, at the RTL's own DAB view (what that view is is printed — an RTL's capture is 2.048 MS/s);
//   2. at a WIDE span (a B210's ~7 MHz — the renderer's span is set to it, since a fake RTL cannot capture 7 MHz in DAB):
//      the passband still spans ~1.536 MHz, now a fifth of the view, and is filled;
//   3. at a view NARROWER than the block (1.2 MHz): clipped to the view, and the acrylic panels are not drawn (no wash
//      over the whole view) — nor any frost, even with the owner's frost turned up;
//   4. drawn only: entering DAB and drawing the block sent no `bandwidth` message (the server's filter is untouched);
//   5. back out of DAB: the ordinary passband and LISTEN label return.
// SHOT=<png> saves the page in DAB at the RTL view (the before/after pictures: run once with the old server binary).
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-dab-vfo.mjs   (exit 3 = not run)
//        DAB_CLIP=<.vbu8> if the bench clip is not in this checkout (vibeserver/bench-clip/make-clip.sh makes it).
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const CLIP = process.env.DAB_CLIP || path.join(ROOT, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.error('no bench clip (vibeserver/bench-clip/make-clip.sh, or DAB_CLIP=) — not run'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(BROWSER)) { console.error('no Edge — not run'); process.exit(3); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };

const BLOCK = 194_064_000;   // 7D
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-dabvfo-'));
const raw = path.join(T, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const procs = [];
const rtlPort = await freePort();
procs.push(spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw, '--dab-at', String(BLOCK)], { stdio: 'ignore' }));
await sleep(400);

const D = path.join(T, 'srv');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(D, d), { recursive: true });
fs.writeFileSync(path.join(D, 'config.json'), JSON.stringify({ configured: true, name: 'Web DAB VFO', locator: 'IO92nh', adminPass: 'web-dabvfo-admin',
  radios: [{ serial: 'webdabvfo', label: 'T', enabled: true, configured: true, users: 1 }] }));
const port = await freePort();
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(D, 'config.json'), VIBESERVER_DATA_DIR: path.join(D, 'data'), VIBESERVER_RUNTIME_DIR: path.join(D, 'run'), HOME: D },
  stdio: 'ignore' }));
for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }

const cdp = await freePort();
procs.push(spawn(BROWSER, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions', '--autoplay-policy=no-user-gesture-required',
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' }));

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
const shot = async (file) => { if (!file) return; const r = await call('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(file, Buffer.from(r.result.data, 'base64')); console.log(`  (screenshot ${file})`); };

// ★ Every message the page sends, recorded before its own scripts run.
const SNIFF = `(() => { window.__sent = []; const o = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) { try { if (typeof d === 'string') window.__sent.push(d); } catch {} return o.apply(this, arguments); }; })();`;
const WF = `window.__vibeWf()`;
/** The drawn passband in Hz, from what the last frame drew (wf.vfoDrawn, device px) — null on a page without it (the old one). */
const DRAWN = `(() => { const v = ${WF}; const d = v && v.wf.vfoDrawn; if (!d) return null; const span = v.wf.spanHz;
  return { widthHz: (d.hiX - d.loX) / d.W * span,
           visFrac: (Math.min(d.W, d.hiX) - Math.max(0, d.loX)) / d.W, fill: d.fill, frost: d.frost, label: d.label, span,
           loX: d.loX, hiX: d.hiX, W: d.W }; })()`;

try {
  await call('Runtime.enable'); await call('Page.enable');
  await call('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
  await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!!document.getElementById("btnConnect")`);
  // ★ land on 7D, where the clip plays (dabRememberedChannel reads this)
  await js(`localStorage.setItem("vibesdr_web_prefs_v1", JSON.stringify({ dabChannel: "7D" })); 1`);
  await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!!document.getElementById("btnConnect")`);
  await js(`document.getElementById("btnConnect").click(); 1`);
  await until(`!!document.getElementById("audioGate")`, 20000);
  for (let i = 0; i < 6; i++) {
    const at = await js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null; const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!at) break;
    await tap(at[0], at[1]);
    if (await until(`!document.getElementById("audioGate")`, 3000)) break;
  }
  await until(`parseFloat(document.getElementById("freqInput").value) > 0`);
  const sentBefore = await js(`window.__sent.length`);
  await js(`document.getElementById("mMode").click(); 1`);
  await until(`[...document.querySelectorAll(".mModeOpt")].some(b => b.textContent === "DAB")`);
  await js(`[...document.querySelectorAll(".mModeOpt")].find(b => b.textContent === "DAB").click(); 1`);
  ok(await until(`document.getElementById("decTitle")?.textContent === "DAB"`, 20000), 'into DAB from the mode menu');
  ok(await until(`/7D/.test(document.getElementById("dabMuxLbl")?.textContent || "")`, 20000), `on 7D (${await js(`document.getElementById("dabMuxLbl")?.textContent`)})`);
  ok(await until(`document.querySelectorAll("#dabStations .dabSvc").length > 0`, 30000), 'the multiplex is received (its stations are listed)');
  await until(`Math.abs(${WF}.spec.frequency - ${BLOCK}) < 2`, 10000);
  await sleep(4000);   // let the waterfall fill with the block for the picture

  // ── 1. the RTL's own DAB view ──
  const v = await js(`(() => { const v = ${WF}; return { span: v.wf.spanHz, centre: v.wf.centerHz, dial: v.spec.frequency, lo: v.wf.filterLow, hi: v.wf.filterHigh, wide: v.wf.vfoWide, label: v.wf.vfoWideLabel }; })()`);
  console.log(`  the RTL's DAB view: ${(v.span / 1e6).toFixed(3)} MHz about ${(v.centre / 1e6).toFixed(3)} MHz, dial ${(v.dial / 1e6).toFixed(3)}`);
  await shot(process.env.SHOT);
  ok(v.wide === true && v.lo === -768000 && v.hi === 768000, `the passband is the block: ${v.lo} … ${v.hi} Hz about the dial (wide ${v.wide})`);
  ok(v.label === 'DAB 7D · 194.064M', `the needle reads "${v.label}"`);
  let d = await js(DRAWN);
  ok(!!d && Math.abs(d.widthHz - 1_536_000) < 2_000, `drawn ${d ? (d.widthHz / 1e6).toFixed(3) : '—'} MHz wide at the RTL view (${d ? Math.round(d.visFrac * 100) : '—'} % of it)`);
  ok(!!d && d.fill === (d.visFrac < 0.9), `…${d?.fill ? 'filled' : 'edges only'}, as its ${d ? Math.round(d.visFrac * 100) : '—'} % of the view says`);
  ok(!!d && d.label === 'DAB 7D · 194.064M', 'the frame drew that label');

  // ── 2. a wide span, as the B210's ──
  const setSpan = (hz) => js(`(() => { const w = ${WF}.wf; Object.defineProperty(w, "spanHz", { configurable: true, get: () => ${hz}, set: () => {} });
    Object.defineProperty(w, "centerHz", { configurable: true, get: () => ${BLOCK}, set: () => {} }); return 1; })()`);
  const unSpan = () => js(`(() => { const w = ${WF}.wf; delete w.spanHz; delete w.centerHz; w.spanHz = ${v.span}; w.centerHz = ${v.centre}; return 1; })()`);
  await setSpan(7_000_000); await sleep(500);
  d = await js(DRAWN);
  ok(!!d && Math.abs(d.widthHz - 1_536_000) < 2_000 && Math.abs(d.visFrac - 1.536 / 7) < 0.01, `7 MHz span: ${d ? (d.widthHz / 1e6).toFixed(3) : '—'} MHz, ${d ? Math.round(d.visFrac * 100) : '—'} % of the view (the multiplex's own share)`);
  ok(!!d && d.fill === true && d.frost === false, '…filled in the acrylic style, not frosted');
  ok(!!d && Math.abs((d.loX + d.hiX) / 2 - d.W / 2) < 2, '…centred on the block');

  // ── 3. narrower than the block: clip, no wash, no frost ──
  await js(`${WF}.wf.vfoFrost = 6; 1`);
  await setSpan(1_200_000); await sleep(500);
  d = await js(DRAWN);
  ok(!!d && d.loX < 0 && d.hiX > d.W, `1.2 MHz view: the block runs off both sides (${d ? Math.round(d.loX) : '—'} … ${d ? Math.round(d.hiX) : '—'} of ${d?.W})`);
  ok(!!d && d.fill === false && d.frost === false, '…so no acrylic panels and no frost (frost 6 set) — the multiplex is not washed over');
  await js(`${WF}.wf.vfoFrost = 0; 1`);
  await unSpan(); await sleep(300);

  // ── 4. drawn only ──
  const bw = await js(`window.__sent.slice(${sentBefore}).filter(s => s.includes('"type":"bandwidth"'))`);
  ok(Array.isArray(bw) && bw.length === 0, `no bandwidth message sent while in DAB (${bw?.length ?? '?'})`);

  // ── 5. out of DAB ──
  const exitBtn = await js(`(() => { const b = [...document.querySelectorAll("#decBox button")].find(b => /EXIT DAB/i.test(b.textContent)); if (!b) return false; b.click(); return true; })()`);
  if (!exitBtn) {
    await js(`document.getElementById("mMode").click(); 1`);
    await until(`[...document.querySelectorAll(".mModeOpt")].some(b => b.textContent === "WFM")`);
    await js(`[...document.querySelectorAll(".mModeOpt")].find(b => b.textContent === "WFM").click(); 1`);
  }
  ok(await until(`!${WF}.wf.vfoWide`, 15000), 'out of DAB: no wide passband');
  ok(await until(`/^LISTEN /.test(${WF}.wf.vfoDrawn?.label || "") && ${WF}.wf.filterHigh <= 250000`, 15000),
     `…the LISTEN label and the listener's own filter are back (${await js(`${WF}.wf.vfoDrawn?.label`)}, ±${await js(`${WF}.wf.filterHigh`)} Hz)`);
  ok(errors.length === 0, 'no JavaScript errors' + (errors.length ? ': ' + errors.slice(0, 2).join(' | ') : ''));
} catch (e) { console.error(e); fails++; }
try { ws.close(); } catch {}
for (const p of procs) { try { p.kill(); } catch {} }
await sleep(600);
fs.rmSync(T, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : '\n\x1b[32mall passed\x1b[0m');
process.exit(fails ? 1 : 0);
