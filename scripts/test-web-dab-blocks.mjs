#!/usr/bin/env node
// test-web-dab-blocks.mjs — DAB BLOCK LABELS AND THE BLOCK SWAPPER on the receiver page (V12, src/utils/dabBlockView.ts;
// Stuart, 2026-11-15, the Almere USRP B210 at 56 MS/s showing all of Band III in DAB on 7D: "add the spectrum labels to
// show the blocks and, if in the memory, show the MUX name next to it — DAB 7D – MTVNL, DAB 8A – 8A regio Alphen. And
// allow clicking the waterfall to tune to the specific DAB block … simply a block swapper").
// Real server, the bench multiplex on 7D (fake-rtl-tcp --dab), a real headless MUTED Edge, THE BUILT PAGE (web/dist —
// `node scripts/build-web.mjs` first) served in front of the server by a small proxy (everything else — the sockets,
// /vibeserver.json — goes to the server), so the page under test is this checkout's, not the one compiled into the binary:
//   1. into DAB on 7D at the RTL's own view (one multiplex): no block labels, and a click sends no block change;
//   2. a WIDE view (the renderer's span set to 8 MHz about 7D — a fake RTL cannot capture 8 MHz): 7C, 7D, 8A and 8B are
//      labelled, 7D with the ensemble's own name, in the tuned colour; 8A with the name the last band scan gave it; no two
//      labels overlap, nor LISTEN's box;
//   3. the hover hint over 8A reads "go to 8A — <name>" with a pointer; between blocks, none;
//   4. a click inside 8A sends exactly the `dab` block change the ▶ arrow sends (the arrow then steps 8A → 8B); a click
//      in the guard between two blocks sends nothing; held back by ask-before-tuning, a click sends nothing and nudges;
//   5. zoomed in on the wide radio (2 MHz): the labels go, and a click is no longer a block swap;
//   6. a phone-width page (390 px) on the wide view: labels drawn, none overlapping, all on screen;
//   7. ★ 2026-11-16 (Stuart, the Almere B210: "we are missing all of the DAB block labels and the known multiplex name for
//      all known MUX's", "raise the VFO coloured labels above so they dont foul the next block along too") — ALL of Band
//      III on screen (56 MHz) with a scan that named most blocks: every block in view labelled, the known names shown, the
//      tuned block's name on the row above the block rows and LISTEN above that, nothing overlapping.
// SHOTS=<dir> saves the pictures; DEBUG=1 logs the proxy and the page console; PASSTHRU=1 serves the binary's own page instead.
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-dab-blocks.mjs (exit 3 = not run)
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const CLIP = process.env.DAB_CLIP || path.join(ROOT, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
const DIST = process.env.WEB_DIST || path.join(ROOT, 'web/dist');
if (!BIN || !fs.existsSync(BIN)) { console.error('set VIBESERVER_BIN to a built vibeserver — not run'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.error('no bench clip (vibeserver/bench-clip/make-clip.sh, or DAB_CLIP=) — not run'); process.exit(3); }
if (!fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.error('no built page — node scripts/build-web.mjs first — not run'); process.exit(3); }
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(BROWSER)) { console.error('no Edge — not run'); process.exit(3); }
const SHOTS = process.env.SHOTS || '';
if (SHOTS) fs.mkdirSync(SHOTS, { recursive: true });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };

// Band III (src/services/dabBlocks.ts)
const B = { '7B': 190_640_000, '7C': 192_352_000, '7D': 194_064_000, '8A': 195_936_000, '8B': 197_648_000, '8C': 199_360_000 };
const IDX = { '7C': 10, '7D': 11, '8A': 12, '8B': 13 };
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-dabblocks-'));
const raw = path.join(T, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const procs = [];
const rtlPort = await freePort();
procs.push(spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw, '--dab-at', String(B['7D'])], { stdio: 'ignore' }));
await sleep(400);

const D = path.join(T, 'srv');
for (const d of ['data', 'run']) fs.mkdirSync(path.join(D, d), { recursive: true });
fs.writeFileSync(path.join(D, 'config.json'), JSON.stringify({ configured: true, name: 'Web DAB blocks', locator: 'IO92nh', adminPass: 'web-dabblocks-admin',
  radios: [{ serial: 'webdabblk', label: 'T', enabled: true, configured: true, users: 1 }] }));
const sport = await freePort();
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(sport), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(D, 'config.json'), VIBESERVER_DATA_DIR: path.join(D, 'data'), VIBESERVER_RUNTIME_DIR: path.join(D, 'run'), HOME: D },
  stdio: 'ignore' }));
for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${sport}/vibeserver.json`); break; } catch { await sleep(250); } }

// ★ The page from web/dist; everything else (HTTP and the WebSocket upgrades) straight through to the server.
const port = await freePort();
const proxy = http.createServer((req, res) => {
  const vs = /^(?:\/r\/[^/]+)?\/vs\/([A-Za-z0-9._-]+\.(?:js|css))(?:[?#]|$)/.exec(req.url);
  if (vs && !process.env.PASSTHRU) {
    if (process.env.DEBUG) console.log('  [proxy] dist', req.url);
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
    if (process.env.DEBUG) console.log('  [proxy]', req.method, req.url, r.statusCode);
    res.writeHead(r.statusCode || 502, r.headers); r.pipe(res);
  });
  up.on('error', () => { try { res.writeHead(502).end(); } catch {} });
  req.pipe(up);
});
proxy.on('upgrade', (req, sock, head) => {
  if (process.env.DEBUG) console.log('  [proxy] upgrade', req.url);
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
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' }));

let target;
for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0; const pending = new Map(); const errors = [];
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  if (process.env.DEBUG && (m.method === 'Runtime.consoleAPICalled' || m.method === 'Log.entryAdded')) console.log('  [page]', JSON.stringify(m.params).slice(0, 300));
  if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(200); } return false; };
const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1, pointerType: 'mouse' }); };
const hover = (x, y) => call('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y, button: 'none', pointerType: 'mouse' });
const shot = async (name) => { if (!SHOTS) return; const r = await call('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, name); fs.writeFileSync(f, Buffer.from(r.result.data, 'base64')); console.log(`  (screenshot ${f})`); };

const SNIFF = `(() => { window.__sent = []; const o = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) { try { if (typeof d === 'string') window.__sent.push(d); } catch {} return o.apply(this, arguments); }; })();`;
const WF = `window.__vibeWf()`;
/** The labels the last frame placed (device px), the DAB ones marked. */
const PLACED = `(() => { const w = ${WF}.wf, dpr = Math.min(2, window.devicePixelRatio || 1);
  const m = document.createElement("canvas").getContext("2d"); m.font = (10 * dpr) + "px ui-monospace, Menlo, monospace";
  return (w.knownPlaced || []).map(p => ({ label: p.sig.label, dab: p.sig.dabIdx !== undefined, cur: !!p.sig.dabCurrent, lx: p.lx, w: p.w, row: p.row,
    span: p.span || 1, lines: p.lines || null, block: p.sig.block || '', grade: p.sig.grade ? p.sig.grade.g : '', x: p.x,
    W: w.canvas.width, text: Math.max(...(p.lines || [p.sig.label]).map(t => m.measureText(t).width)) })); })()`;
/** LISTEN's box on its row (0, or above the block rows on a wide DAB view — wf.liveRow), markerLabel's geometry, device px. */
const LISTEN_BOX = `(() => { const w = ${WF}.wf, d = w.vfoDrawn, dpr = Math.min(2, window.devicePixelRatio || 1); if (!d || w.readoutMerged) return null;   // ★ merged into the tuned box
  const m = document.createElement("canvas").getContext("2d"); m.font = (10 * dpr) + "px ui-monospace, Menlo, monospace";
  const tw = m.measureText(d.label).width, lx = d.nX + 6 * dpr + tw > d.W ? d.nX - tw - 6 * dpr : d.nX + 6 * dpr;
  return { row: w.liveRow || 0, x0: lx - 3 * dpr, x1: lx + tw + 3 * dpr }; })()`;
/** A point on the spectrum (the live scale) at frequency hz, in page CSS px. */
const at = (hz) => js(`(() => { const v = ${WF}; const c = v.wf.canvas, r = c.getBoundingClientRect();
  return [r.left + v.wf.hzToX(${hz}, c.clientWidth), r.top + Math.max(30, 0.12 * r.height)]; })()`);
const dabSent = (from) => js(`window.__sent.slice(${from}).filter(s => s.includes('"type":"dab"')).map(s => JSON.parse(s))`);
const setSpan = (hz, centre) => js(`(() => { const w = ${WF}.wf; Object.defineProperty(w, "spanHz", { configurable: true, get: () => ${hz}, set: () => {} });
  Object.defineProperty(w, "centerHz", { configurable: true, get: () => ${centre}, set: () => {} }); return 1; })()`);
/** Any two labels on one row overlapping, a box narrower than its text, or a label over LISTEN's box. */
const overlaps = (ps, listen) => {
  if (ps.some(p => p.w < p.text)) return true;
  if (listen && ps.some(p => listen.row >= p.row && listen.row < p.row + p.span && p.lx < listen.x1 && listen.x0 < p.lx + p.w)) return true;
  for (let r = 0; r <= 10; r++) { const row = ps.filter(p => r >= p.row && r < p.row + p.span).sort((a, b) => a.lx - b.lx);   // ★ a two-line box on both rows
    for (let k = 1; k < row.length; k++) if (row[k].lx < row[k - 1].lx + row[k - 1].w) return true; }
  return false;
};

try {
  await call('Runtime.enable'); await call('Page.enable'); if (process.env.DEBUG) await call('Log.enable');
  await call('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
  await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!!document.getElementById("btnConnect")`);
  await js(`localStorage.setItem("vibesdr_web_prefs_v1", JSON.stringify({ dabChannel: "7D" })); 1`);
  await call('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  // ★ the page may settle its own URL once on load (a reload of "/?"), so the START press is retried until it takes
  for (let i = 0; i < 4; i++) {
    await until(`!!document.getElementById("btnConnect") && document.readyState === "complete"`);
    await sleep(1500);
    await js(`document.getElementById("btnConnect")?.click(); 1`);
    if (await until(`!!document.getElementById("audioGate") || !!document.querySelector("#dabStations, #freqInput") && parseFloat(document.getElementById("freqInput")?.value) > 0`, 8000)) break;
  }
  for (let i = 0; i < 6; i++) {
    const g = await js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null; const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!g) break;
    await tap(g[0], g[1]);
    if (await until(`!document.getElementById("audioGate")`, 3000)) break;
  }
  await until(`parseFloat(document.getElementById("freqInput").value) > 0`);
  await js(`document.getElementById("mMode").click(); 1`);
  await until(`[...document.querySelectorAll(".mModeOpt")].some(b => b.textContent === "DAB")`);
  await js(`[...document.querySelectorAll(".mModeOpt")].find(b => b.textContent === "DAB").click(); 1`);
  ok(await until(`document.getElementById("decTitle")?.textContent === "DAB"`, 20000), 'into DAB from the mode menu');
  if (process.env.DEBUG) console.log(await js(`document.body.innerText.slice(0, 600)`), errors);
  ok(await until(`/7D/.test(document.getElementById("dabMuxLbl")?.textContent || "")`, 20000), 'on 7D');
  ok(await until(`document.querySelectorAll("#dabStations .dabSvc").length > 0`, 30000), 'the multiplex is received');
  await until(`/ — /.test(document.getElementById("dabMuxLbl")?.textContent || "")`, 15000);
  const ens = (await js(`document.getElementById("dabMuxLbl")?.textContent || ""`)).split(' — ')[1] || '';
  console.log(`  the ensemble on 7D: "${ens}"`);
  // ★ every DAB report seen (window.__lastDab), and test grades merged into each (window.__q) — the page's own road
  await js(`(() => { const s = ${WF}.spec, o = s.cb.onDab; s.cb.onDab = (d) => { window.__lastDab = d;
    return o(window.__q ? { ...d, blockQ: { ...(d.blockQ || {}), ...window.__q } } : d); }; return 1; })()`);
  await sleep(3000);

  // ── 0. ★★ the server GRADES the block it decodes (vibe_dab_block_quality.h), once settled (6 s + a 10-s span). Every FIB
  //    reads on the bench multiplex, so it is working or marginal — and the 5.8-s clip's LOOP SEAM loses a super frame or
  //    two in every span (an audible gap: the counters below), which is exactly what marginal means; never "not working".
  ok(await until(`["good", "marginal"].includes(window.__lastDab?.blockQ?.["7D"]?.g) && window.__lastDab.blockQ["7D"].fib >= 0.9`, 40000),
     `the server grades 7D from what it measured: ${JSON.stringify(await js('window.__lastDab?.blockQ || null'))}`);
  const srv7D = await js(`window.__lastDab?.blockQ?.["7D"]?.g || ""`);
  console.log(`  the bench audio so far: ${await js('JSON.stringify({ auIn: window.__lastDab?.auIn, auBad: window.__lastDab?.auBad, mp2In: window.__lastDab?.mp2In, mp2Bad: window.__lastDab?.mp2Bad })')}`);

  // ── 1. the RTL's own view: one multiplex, nothing changes ──
  const span0 = await js(`${WF}.wf.spanHz`), centre0 = await js(`${WF}.wf.centerHz`);
  console.log(`  the RTL's DAB view: ${(span0 / 1e6).toFixed(3)} MHz`);
  ok(await js(`Array.isArray(${WF}.wf.dabBlocks) && ${WF}.wf.dabBlocks.length === 41`), 'in DAB the waterfall holds the 41 blocks');
  let ps = await js(PLACED);
  ok(!ps.some(p => p.dab), `narrow view: no block labels (${ps.length} labels placed)`);
  await shot('dab-blocks-narrow-rtl.png');
  let n0 = await js(`window.__sent.length`);
  { const p = await at(B['7D'] + 400_000); await tap(p[0], p[1]); }
  await sleep(1200);
  ok((await dabSent(n0)).length === 0 && (await js(`window.__sent.slice(${n0}).filter(s => s.includes('"type":"tune"')).length`)) === 0,
     'narrow view: a click sends no block change and no tune (as before)');

  // ── 2. a wide view, with a scan's name for 8A ──
  await js(`(${WF}.spec.cb.onDabScan)({ type: "dab_scan", running: false, finished: true, done: 41, total: 41, seq: 1,
    found: [{ block: "8A", channel: ${IDX['8A']}, hz: ${B['8A']}, ensemble: "8A regio Alphen", fib: 0.98, mer: 20, complete: true, services: [] }] }); 1`);
  await setSpan(8_000_000, B['7D']); await sleep(600);
  ps = await js(PLACED);
  const dabL = ps.filter(p => p.dab).map(p => (p.lines || [p.label]).join(' / '));
  console.log(`  wide view labels: ${dabL.join(' | ')}`);
  const tunedBox = ps.find(p => p.cur);
  ok(!!tunedBox && tunedBox.lines[0] === '194.064M: DAB 7D' && !!tunedBox.lines[1] && tunedBox.lines[1].startsWith(ens.slice(0, 10).trim()),
     `ONE tuned box: the readout over the ensemble's own name (${tunedBox?.lines?.join(' / ')})`);
  ok(await js(`${WF}.wf.readoutMerged === true`), '…and the needle draws no separate readout');
  ok(dabL.includes('8A / 8A regio Alphen'), '8A: two lines, the block over the name the band scan gave it');
  ok(['7C', '8B'].every(b => dabL.includes(b)), 'the unnamed neighbours read plain "7C" / "8B" on one line (nothing invented)');
  ok(ps.filter(p => p.cur).length === 1, 'the block being received is the one drawn in the tuned colour');
  const pxOf = await js(`(() => { const w = ${WF}.wf; return [w.hzToX(${B['7D']} - 768000), w.hzToX(${B['7D']} + 768000)]; })()`);
  ok(ps.filter(p => p.dab && !p.cur).every(p => p.lx <= 0 || p.lx + p.w >= p.W || Math.abs(p.lx + p.w / 2 - p.x) <= 1),
     'every block label is centred on its own block');
  ok(!!tunedBox && Math.abs(tunedBox.lx + tunedBox.w / 2 - tunedBox.x) <= 2 && pxOf[0] < tunedBox.x && tunedBox.x < pxOf[1], 'the tuned box is centred on the needle');
  ok(!!tunedBox && ps.filter(p => !p.cur).every(p => p.row + p.span - 1 < tunedBox.row), '…above every block row');
  ok(!overlaps(ps, await js(LISTEN_BOX)), 'no two labels overlap, nor LISTEN\'s box, and every box holds its text');
  ok(ps.every(p => p.lx >= 0 && p.lx + p.w <= p.W + 1), 'every label is on screen');
  await shot('dab-blocks-wide-desktop.png');
  // ★★ the grades: a bar on each graded block's box, from the server's blockQ (here injected beside its own)
  { const now = Math.floor(Date.now() / 1000);
    await js(`window.__q = { "7C": { g: "marginal", mer: 9.1, fib: 0.8, t: ${now - 60} }, "8A": { g: "bad", mer: 7, fib: 0, t: ${now - 120} },
      "8B": { g: "good", mer: 18, fib: 1, t: ${now - 9 * 86400} } }; 1`);
    ok(await until(`(${PLACED}).some(p => p.block === "8A" && p.grade === "bad")`, 4000), 'a grade from the server marks its block (8A not working)');
    ps = await js(PLACED);
    const g = Object.fromEntries(ps.filter(p => p.dab).map(p => [p.block, p.grade]));
    ok(g['7C'] === 'marginal' && g['7D'] === srv7D && g['8B'] === '', `7C marginal, 7D the server's own grade (${srv7D}), 8B's week-old grade not shown (${JSON.stringify(g)})`);
    await shot('dab-blocks-grades.png');
  }

  // ── 3. the hover hint ──
  { const p = await at(B['8A'] + 200_000); await hover(p[0], p[1]); await hover(p[0] + 1, p[1]); }
  ok(await until(`document.getElementById("ptrFreq").classList.contains("show")`, 3000), 'hovering over 8A shows a hint');
  const hint = await js(`document.querySelector("#ptrFreq .m").textContent`);
  ok(/^go to 8A — 8A regio Alphen · not decoding, MER 7 dB, measured \d\d:\d\d$/.test(hint), `…reading "${hint}"`);
  ok((await js(`${WF}.wf.canvas.style.cursor`)) === 'pointer', '…with a pointer');
  await shot('dab-blocks-hover.png');
  { const p = await at((B['7D'] + B['8A']) / 2); await hover(p[0], p[1]); await hover(p[0] + 1, p[1]); }
  ok(await until(`!document.getElementById("ptrFreq").classList.contains("show")`, 3000), 'between two blocks: no hint');
  ok((await js(`${WF}.wf.canvas.style.cursor`)) === '', '…and the ordinary cursor');

  // ── 4. the click ──
  n0 = await js(`window.__sent.length`);
  { const p = await at((B['7D'] + B['8A']) / 2); await tap(p[0], p[1]); }
  await sleep(1200);
  ok((await dabSent(n0)).length === 0, 'a click in the guard between 7D and 8A sends nothing');
  // held back: ask before tuning
  await js(`${WF}.spec.tuneLocked = true; 1`);
  n0 = await js(`window.__sent.length`);
  { const p = await at(B['8A']); await tap(p[0], p[1]); }
  await sleep(1200);
  ok((await dabSent(n0)).length === 0, 'held back by ask-before-tuning: a click on 8A sends nothing');
  ok(await js(`!!document.getElementById("mChat")?.classList.contains("askNudge")`), '…and the "ask in chat" nudge shows (the chat key flashes)');
  ok(/7D/.test(await js(`document.getElementById("dabMuxLbl")?.textContent || ""`)), '…the box still says 7D');
  await js(`${WF}.spec.tuneLocked = false; 1`);
  n0 = await js(`window.__sent.length`);
  { const p = await at(B['8A'] + 300_000); await tap(p[0], p[1]); }
  ok(await until(`window.__sent.slice(${n0}).some(s => s.includes('"type":"dab"'))`, 3000), 'a click inside 8A sends a block change');
  const click = await dabSent(n0);
  ok(click.length === 1 && click[0].on === 1 && click[0].channel === IDX['8A'] && click[0].sid === undefined, `…once, to 8A: ${JSON.stringify(click)}`);
  ok(await until(`/8A/.test(document.getElementById("dabMuxLbl")?.textContent || "")`, 3000), '…and the DAB box moves to 8A');
  // ★ (8A's name may be gone by now: the server answers the block change with its own scan state, which knows no 8A)
  ok(await until(`(${PLACED}).find(p => p.cur)?.block === "8A"`, 3000), `…and 8A becomes the highlighted label (${(await js(PLACED)).find(p => p.cur)?.lines?.join(' / ')})`);
  await shot('dab-blocks-after-click.png');
  // the arrow, for comparison: ▶ from 8A sends the same shape of message for 8B
  await sleep(1500);
  n0 = await js(`window.__sent.length`);
  { const r = await js(`(() => { const b = document.getElementById("mVfoUp").getBoundingClientRect(); return [b.left + b.width / 2, b.top + b.height / 2]; })()`);
    await tap(r[0], r[1]); }   // ★ a real press — holdSweep listens for pointers, not click()
  ok(await until(`window.__sent.slice(${n0}).some(s => s.includes('"type":"dab"'))`, 4000), 'the ▶ arrow sends its block change');
  const arrow = await dabSent(n0);
  ok(arrow.length === 1 && arrow[0].channel === IDX['8B'] && JSON.stringify(Object.keys(arrow[0]).sort()) === JSON.stringify(Object.keys(click[0]).sort()),
     `…the same message as the click's (${JSON.stringify(arrow[0])} vs ${JSON.stringify(click[0])})`);

  // ── 5. zoomed in on the wide radio ──
  await setSpan(2_000_000, B['8B']); await sleep(600);
  ps = await js(PLACED);
  ok(!ps.some(p => p.dab), 'zoomed in to 2 MHz: the block labels go');
  n0 = await js(`window.__sent.length`);
  { const p = await at(B['8B'] + 900_000); await tap(p[0], p[1]); }
  await sleep(1200);
  ok((await dabSent(n0)).length === 0, '…and a click is no longer a block swap');

  // ── 6. a phone-width page on the wide view ──
  await setSpan(16_000_000, B['8A']);
  await call('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 3, mobile: true });
  await sleep(1500);
  ps = await js(PLACED);
  console.log(`  phone labels: ${ps.filter(p => p.dab).map(p => (p.lines || [p.label]).join(' / ')).join(' | ')}`);
  ok(ps.some(p => p.dab), 'phone width: block labels drawn');
  ok(!overlaps(ps, await js(LISTEN_BOX)) && ps.every(p => p.lx >= 0 && p.lx + p.w <= p.W + 1), '…none overlapping, all on screen');
  ok(ps.some(p => p.cur), '…the block being received among them');
  await shot('dab-blocks-wide-phone.png');
  await call('Emulation.clearDeviceMetricsOverride');

  // ── 7. all of Band III, most blocks named by a scan ──
  {
    const names = { '5A': 'NL Lokaal 5A', '5C': 'DAB-NL Regio', '6B': 'Flevoland', '6C': 'NPO', '6D': 'Noord-Holland', '7A': 'Lokaal Zuid',
      '7B': 'Utrecht Regio', '7C': 'Commercieel 2', '8A': '8A regio Alphen', '8B': 'Gelderland', '8C': 'Amsterdam Lokaal',
      '8D': 'Brabant', '9A': 'Regionaal 9A', '9B': 'Zuid-Holland', '9C': 'DAB+ Rotterdam', '11C': 'Commercieel', '12A': 'Landelijk 12A' };
    const blocks = await js(`(() => { const b = ${WF}.wf.dabBlocks || []; return b.map(x => ({ name: x.bare.slice(4), hz: x.hz, idx: x.dabIdx })); })()`);
    const found = blocks.filter(b => names[b.name]).map(b => ({ block: b.name, channel: b.idx, hz: b.hz, ensemble: names[b.name], fib: 0.98, mer: 20, complete: true, services: [] }));
    await js(`(${WF}.spec.cb.onDabScan)({ type: "dab_scan", running: false, finished: true, done: 41, total: 41, seq: 2, found: ${JSON.stringify(found)} }); 1`);
    await setSpan(56_000_000, 202_000_000); await sleep(800);
    ps = await js(PLACED);
    const lo = 202_000_000 - 28_000_000, hi = 202_000_000 + 28_000_000;
    const inView = blocks.filter(b => b.hz >= lo && b.hz <= hi).map(b => b.name);
    const shown = new Set(ps.filter(p => p.dab).map(p => p.block));
    const missing = inView.filter(b => !shown.has(b));
    console.log(`  all of Band III: ${ps.filter(p => p.dab).length} block labels on ${Math.max(...ps.map(p => p.row + p.span))} rows`);
    ok(missing.length === 0, `every block in view is labelled (${inView.length})${missing.length ? ' — missing ' + missing.join(', ') : ''}`);
    const namedIn = inView.filter(b => names[b] && b !== ps.find(p => p.cur)?.block);
    const namedShown = namedIn.filter(b => ps.some(p => p.dab && !p.cur && p.block === b && p.span === 2 && p.lines?.[1])).length;
    ok(namedShown === namedIn.length, `every name the scan gave is shown, under its block (${namedShown} of ${namedIn.length})`);
    const cur = ps.find(p => p.cur), top = Math.max(...ps.filter(p => !p.cur).map(p => p.row + p.span - 1));
    ok(!!cur && cur.row > top && (await js(`${WF}.wf.readoutMerged`)), `the tuned box (readout + name) is above every block row (${cur?.row} > ${top}), no separate readout`);
    // a full Band III with MIXED grades, for the picture
    const now = Math.floor(Date.now() / 1000), mix = {};
    ['5A', '5C', '6B', '6C', '7B', '8B', '9B', '11C', '12A'].forEach(b => (mix[b] = { g: 'good', mer: 17, fib: 1, t: now - 300 }));
    ['7A', '8C', '9A'].forEach(b => (mix[b] = { g: 'marginal', mer: 9, fib: 0.8, t: now - 600 }));
    ['6D', '8D'].forEach(b => (mix[b] = { g: 'bad', mer: 7, fib: 0, t: now - 900 }));
    await js(`window.__q = ${JSON.stringify(mix)}; 1`);
    ok(await until(`(${PLACED}).some(p => p.block === "6D" && p.grade === "bad")`, 4000), 'mixed grades drawn across Band III');
    ok(!overlaps(ps, await js(LISTEN_BOX)) && ps.every(p => p.lx >= 0 && p.lx + p.w <= p.W + 1), '…none overlapping, all on screen');
    await shot('dab-blocks-band3.png');
  }

  ok(errors.length === 0, 'no JavaScript errors' + (errors.length ? ': ' + errors.slice(0, 2).join(' | ') : ''));
} catch (e) { console.error(e); fails++; }
try { ws.close(); } catch {}
for (const p of procs) { try { p.kill(); } catch {} }
proxy.close();
await sleep(600);
fs.rmSync(T, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
console.log(fails ? `\n\x1b[31m${fails} failed\x1b[0m` : '\n\x1b[32mall passed\x1b[0m');
process.exit(fails ? 1 : 0);
