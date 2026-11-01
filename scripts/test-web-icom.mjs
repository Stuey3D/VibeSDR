#!/usr/bin/env node
// test-web-icom.mjs — the WEB CLIENT on an ICOM (driver "icom", receive only), end to end: the real server against
// the fake IC-7300 (tools/icom/fake-icom), a real headless MUTED browser, and THIS tree's web/dist served in front
// of the server by a small proxy (so the test needs no server rebuild — the binary's embedded page may be older).
//
//   • the Icom panel is drawn from the radio's own lists; every IQ-side control is hidden (and comes back the moment
//     the page is not on an Icom);
//   • a LISTENER under the owner lock sees "Everyone hears what the radio is tuned to", the tuning controls disabled;
//   • the signed-in ADMIN's mode, FIL, span (button and zoom key) and ATT changes REACH THE FAKE RADIO — checked on the
//     fake's own counters, and the panel lights only from the radio's answer;
//   • the S-meter is the radio's (S-units), the TX banner appears when the owner keys, no page exceptions;
//   • phone width: no horizontal overflow; default/silver/black and transparency on/off: the panel stays legible.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-icom.mjs   (fake-icom beside it; exit 3 = not run)
// ★ SILENT: --mute-audio --disable-audio-output, and the server's audio is a test tone to a socket.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';

const BIN = process.env.VIBESERVER_BIN || '';
const FAKE = process.env.FAKE_ICOM_BIN || (BIN ? path.join(path.dirname(BIN), 'fake-icom') : '');
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const DIST = path.join(ROOT, 'web/dist');
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(FAKE)) { console.log('   not run — set VIBESERVER_BIN (fake-icom beside it)'); process.exit(3); }
if (!fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.log('   not run — build the page first: node scripts/build-web.mjs'); process.exit(3); }
if (!fs.existsSync(BROWSER)) { console.log(`   not run — no browser at ${BROWSER}`); process.exit(3); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const until = async (f, ms = 5000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await f()) return true; await sleep(100); } return !!(await f()); };

const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-icom-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(T, d));
const ADMIN_PW = 'icom-admin-pass';
// ★ No config file: a CONFIGURED one makes the binary the multi-radio front door on 48000 and drops --icom/--port.

// ── the fake radio ──
const fake = spawn(FAKE, ['--echo'], { stdio: ['pipe', 'pipe', 'pipe'] });
let fakeOut = '';
fake.stdout.on('data', (d) => { fakeOut += d; }); fake.stderr.on('data', (d) => { fakeOut += d; });
await until(() => /FAKE_ICOM_PTY (\S+)/.test(fakeOut), 3000);
const pty = (fakeOut.match(/FAKE_ICOM_PTY (\S+)/) || [])[1];
const stats = async () => {
  const n0 = fakeOut.length; fake.stdin.write('stats\n');
  await until(() => fakeOut.slice(n0).includes('FAKE_ICOM_STATS'), 2000);
  const m = fakeOut.slice(n0).match(/FAKE_ICOM_STATS (.*)/);
  const o = {}; if (m) for (const kv of m[1].trim().split(' ')) { const [k, v] = kv.split('='); o[k] = Number(v); }
  return o;
};

// ── the server ──
const port = await freePort(), proxyPort = await freePort(), cdp = await freePort();
const log = fs.openSync(path.join(T, 'server.log'), 'w');
const srv = spawn(BIN, ['--icom', pty, '--icom-tty-ok', '--icom-audio', 'tone', '--port', String(port), '--users', '4', '--idle-grace', '0', '--admin-pass', ADMIN_PW], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(T, 'config.json'), VIBESERVER_DATA_DIR: path.join(T, 'data'), VIBESERVER_RUNTIME_DIR: path.join(T, 'run'), HOME: T },
  stdio: ['ignore', log, log] });

// ── the proxy: THIS tree's page and scripts; everything else (APIs, WebSockets) to the server ──
const MIME = { '.js': 'text/javascript', '.css': 'text/css', '.html': 'text/html; charset=utf-8' };
const proxy = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  const isPage = u.pathname === '/' || u.pathname === '/index.html';
  const isScript = u.pathname.startsWith('/vs/') && !u.pathname.includes('..');
  if (isPage || isScript) {
    const f = isPage ? path.join(DIST, 'vibesdr.html') : path.join(DIST, u.pathname);
    if (fs.existsSync(f)) {
      res.writeHead(200, { 'content-type': MIME[path.extname(f)] || 'application/octet-stream', 'cache-control': 'no-store' });
      res.end(fs.readFileSync(f)); return;
    }
  }
  const up = http.request({ host: '127.0.0.1', port, path: req.url, method: req.method, headers: { ...req.headers, host: `127.0.0.1:${port}` } }, (r) => {
    res.writeHead(r.statusCode || 502, r.headers); r.pipe(res);
  });
  up.on('error', () => { try { res.writeHead(502); res.end(); } catch {} });
  req.pipe(up);
});
proxy.on('upgrade', (req, sock, head) => {
  const up = net.connect(port, '127.0.0.1', () => {
    let h = `${req.method} ${req.url} HTTP/1.1\r\n`;
    for (let i = 0; i < req.rawHeaders.length; i += 2) h += `${req.rawHeaders[i]}: ${req.rawHeaders[i + 1]}\r\n`;
    up.write(h + '\r\n'); if (head && head.length) up.write(head);
    sock.pipe(up); up.pipe(sock);
  });
  up.on('error', () => sock.destroy()); sock.on('error', () => up.destroy());
});
await new Promise((r) => proxy.listen(proxyPort, '127.0.0.1', r));

const br = spawn(BROWSER, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions',
  '--autoplay-policy=no-user-gesture-required', `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`,
  '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });

async function cleanup() {
  for (const p of [br, srv]) { try { p.kill(); } catch {} }
  try { fake.stdin.write('quit\n'); } catch {}
  await sleep(400); try { fake.kill(); } catch {}
  proxy.close(); await sleep(300);
  fs.rmSync(T, { recursive: true, force: true });
}

async function page() {
  let target;
  for (let i = 0; i < 60 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0; const pending = new Map(); const errors = [];
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const wait = (expr, ms = 10000) => until(() => js(expr), ms);
  const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
  await call('Runtime.enable'); await call('Page.enable');
  return { ws, call, js, wait, tap, errors };
}
const startRadio = async (p) => {
  await p.wait(`!!document.getElementById("audioGate")`, 20000);
  for (let i = 0; i < 6; i++) {
    const at = await p.js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null;
      const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!at) break;
    await p.tap(at[0], at[1]);
    if (await p.wait(`!document.getElementById("audioGate")`, 3000)) break;
  }
};
// ★ The card's pads act on POINTERDOWN (a hold repeats), so they need a real tap, not el.click().
const tapId = async (p, id) => { const at = await p.js(`(() => { const r = document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();
  return [r.left + r.width / 2, r.top + r.height / 2]; })()`); await p.tap(at[0], at[1]); await sleep(150); };
const go = async (p, url) => { await p.call('Page.navigate', { url }); await sleep(800); };
// ★ "Hidden" = computed display none on the element or an ancestor below <body> that is not a closed PANEL — so a
//   control is judged hidden by the Icom rule, not merely because the menu it lives in is shut.
const HIDDEN_JS = `(sel) => { const els = Array.from(document.querySelectorAll(sel)); if (!els.length) return 'missing';
  return els.every((el) => { for (let e = el; e && e !== document.body; e = e.parentElement) {
    if (['menu','audioPanel','decodersPanel','chatPanel'].includes(e.id)) return false;
    if (getComputedStyle(e).display === 'none') return true; } return false; }); }`;

try {
  ok(!!pty, `the fake radio is on ${pty}`);
  ok(await until(async () => { try { return (await fetch(`http://127.0.0.1:${proxyPort}/vibeserver.json`)).ok; } catch { return false; } }, 10000), 'server up, proxy serving this tree’s page');
  const p = await page();

  // ════ 1. A LISTENER (owner lock ON by default) ════
  console.log('── a listener ──');
  await go(p, `http://127.0.0.1:${proxyPort}/`);
  await p.wait(`!!document.getElementById("btnConnect")`, 15000);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await startRadio(p);
  ok(await p.wait(`document.body.classList.contains("icom")`, 15000), 'the page knows it is an Icom (body.icom)');
  ok(await p.wait(`!document.getElementById("icomCtls").hidden && document.querySelectorAll("#icomMode button").length === 10`),
     'the Icom panel is drawn with the radio’s 10 modes');
  const labels = await p.js(`Array.from(document.querySelectorAll("#icomMode button")).map(b => b.textContent).join(",")`);
  ok(/USB-D/.test(labels) && /RTTY-R/.test(labels) && !/WFM/.test(labels), `modes labelled as the radio writes them (${labels})`);
  ok(await p.js(`document.querySelectorAll("#icomFil button").length === 3 && /FIL2 · 2\\.4k/.test(document.getElementById("icomFil").textContent)`),
     'FIL1/2/3 with the SSB widths');
  ok(await p.js(`document.querySelectorAll("#icomSpan button").length === 8`), 'the span ladder: 8 rungs');
  ok(await p.js(`!!document.getElementById("icomAtt") && !!document.getElementById("icomPreamp") && !!document.getElementById("icomAgc") && !!document.getElementById("icomRfGain") && document.querySelectorAll("#icomDsp button").length === 3`),
     'ATT, preamp, AGC, RF gain and the radio’s NB/NR/ANF');
  ok(/Icom IC-7300/.test(await p.js(`document.getElementById("radioName").textContent`)), 'RECEIVER names the Icom, receive only');
  for (const [sel, what] of [['#rowRate', 'sample rate'], ['#rowGain', 'gain slider'], ['#rowAgc', 'VibeAGC'], ['#rowTunerBw', 'IF filter'],
                             ['.rtlOnly', 'bias-T / digital AGC / direct sampling'], ['.rtlRspOnly', 'ppm'], ['#rspCtls', 'the RSP panel'],
                             ['.sqlBlock', 'our squelch'], ['#nbxRow', 'our noise blanker'], ['#iqRow', 'raw IQ out'],
                             ['#nfmVoiceRow', 'NFM voice'], ['#deemphSeg', 'de-emphasis'], ['#stereoBtn', 'stereo/IMS/CEQ/NB processing'],
                             ['#mPanelLock', 'the VFO-centre lock']]) {
    // ★ No '#wfRateSeg': ca18ad08f (GitHub #57) removed the waterfall DATA RATE row for everybody — the speed keys decide it.
    ok((await p.js(`(${HIDDEN_JS})(${JSON.stringify(sel)})`)) === true, `hidden on an Icom: ${what}`);
  }
  ok(await p.js(`(() => { document.body.classList.remove("icom"); const d = (el) => !!el && getComputedStyle(el).display !== "none";
      const back = d(document.getElementById("rowGain")) && d(document.querySelector(".sqlBlock"))
        && d(document.getElementById("deemphSeg").closest(".mrow")); document.body.classList.add("icom"); return back; })()`),
     'and they come back the moment the page is not on an Icom (one class, nothing left hidden)');
  ok(await p.wait(`document.getElementById("mMode").textContent.trim() === "USB-D"`), `the card’s mode readout is the radio’s (USB-D)`);
  ok(await p.wait(`/^S\\d/.test(document.getElementById("mSnr").textContent.trim())`), `the S-meter is the radio’s, in S-units (${await p.js(`document.getElementById("mSnr").textContent`)})`);
  ok(await p.wait(`!document.getElementById("icomFollow").hidden && document.getElementById("icomFollow").textContent.startsWith("Everyone hears what the radio is tuned to")`),
     'a listener under the owner lock is told: Everyone hears what the radio is tuned to');
  ok(await p.js(`Array.from(document.querySelectorAll("#icomMode button, #icomFil button, #icomSpan button")).every(b => b.disabled)`), 'mode, FIL and span are disabled for them');
  ok(await p.js(`Array.from(document.querySelectorAll("#icomAtt button, #icomDsp button")).every(b => b.disabled) && document.getElementById("icomRfGain").disabled`), 'and the RF settings (admin-only by default)');
  ok(await p.js(`document.body.classList.contains("icomFollow") && /LISTENERS FOLLOW THE RADIO/.test(document.getElementById("mShared").textContent)`), 'the banner says listeners follow the radio');
  const st0 = await stats();
  await tapId(p, 'mZoomIn'); await tapId(p, 'mVfoUp');
  await sleep(800);
  const st1 = await stats();
  ok(st1.freq === st0.freq && st1.span === st0.span, `a listener’s zoom/tune is stopped at the page — the radio did not move (${st1.freq}, ${st1.span})`);
  ok(p.errors.length === 0, `no page exceptions as a listener${p.errors.length ? ': ' + p.errors.slice(0, 3).join(' | ') : ''}`);

  // ════ 2. THE ADMIN ════
  console.log('── the signed-in admin ──');
  await go(p, `http://127.0.0.1:${proxyPort}/`);
  ok(await p.wait(`!document.getElementById("btnAdmin").hidden`, 15000), 'the splash offers ADMIN');
  await p.js(`document.getElementById("btnAdmin").click(); document.getElementById("gateAdminPw").value = ${JSON.stringify(ADMIN_PW)}; 1`);
  await p.js(`document.getElementById("btnConnect").click(); 1`);
  await startRadio(p);
  ok(await p.wait(`document.body.classList.contains("icom") && !document.body.classList.contains("icomFollow")`, 15000), 'admin: the owner lock does not apply');
  ok(await p.wait(`document.getElementById("icomFollow").hidden && !document.querySelector("#icomMode button").disabled`), 'admin: no follow note, the controls are live');
  // mode → CW
  await p.js(`document.querySelector('#icomMode button[data-v="cw"]').click(); 1`);
  ok(await until(async () => (await stats()).mode === 3, 4000), 'a MODE tap reaches the fake radio (CW)');
  ok(await p.wait(`document.querySelector('#icomMode .on')?.dataset.v === "cw"`), 'and the button lights from the radio’s answer');
  ok(await p.wait(`document.getElementById("mMode").textContent.trim() === "CW"`), 'the card says CW');
  ok(await p.wait(`/FIL2 · 500/.test(document.getElementById("icomFil").textContent)`), 'FIL widths follow the mode family (CW: 500)');
  // FIL 3
  await p.js(`document.querySelector('#icomFil button[data-v="3"]').click(); 1`);
  ok(await p.wait(`document.querySelector('#icomFil .on')?.dataset.v === "3"`), 'a FIL tap: the server reports FIL3 back (from the radio’s state)');
  // span button → 100 kHz
  await p.js(`document.querySelector('#icomSpan button[data-v="100000"]').click(); 1`);
  ok(await until(async () => (await stats()).span === 100000, 4000), 'a SPAN tap reaches the fake radio (100 kHz)');
  ok(await p.wait(`document.querySelector('#icomSpan .on')?.dataset.v === "100000"`), 'and lights from the sweep');
  // zoom key → one rung narrower
  await tapId(p, 'mZoomIn');
  ok(await until(async () => (await stats()).span === 50000, 4000), 'the ZOOM + key steps the radio’s span ladder one rung (50 kHz)');
  await tapId(p, 'mZoomOut');
  ok(await until(async () => (await stats()).span === 100000, 4000), 'ZOOM − one rung wider (100 kHz) — never capped at the current span');
  // ATT
  await p.js(`document.querySelector('#icomAtt button[data-v="20"]').click(); 1`);
  ok(await p.wait(`document.querySelector('#icomAtt .on')?.dataset.v === "20"`), 'ATT 20 dB: lit from the radio’s report');
  // mode back to USB-D through the CARD's picker, as a listener would use it
  await p.js(`document.getElementById("mMode").click(); 1`);
  ok(await p.wait(`!!document.getElementById("mModeMenu") && /USB-D/.test(document.getElementById("mModeMenu").textContent) && !/WFM/.test(document.getElementById("mModeMenu").textContent)`),
     'the card’s mode picker offers the radio’s modes (no WFM)');
  ok(await p.js(`!!document.getElementById("mIcomFil") && document.querySelectorAll("#mIcomFil button").length === 3`), 'with FIL1/2/3 in place of the bandwidth sliders');
  await p.js(`Array.from(document.querySelectorAll("#mModeMenu .mModeOpt")).find(b => b.textContent === "USB-D").click(); 1`);
  ok(await until(async () => { const s = await stats(); return s.mode === 1 && s.data === 1; }, 4000), 'picking USB-D there reaches the radio (USB + DATA)');
  // TX
  fake.stdin.write('tx 1\n');
  ok(await p.wait(`document.getElementById("mShared").classList.contains("icomTx") && /TRANSMITTING/.test(document.getElementById("mShared").textContent) && !document.getElementById("mShared").hidden`),
     'the owner keys the radio: the TX banner (receive paused)');
  ok(await p.wait(`document.getElementById("mSnr").textContent.trim() === "TX"`), 'the meter reads TX');
  await sleep(1500);
  ok(await p.js(`document.getElementById("linkBars").className !== "q0"`), 'and the frozen scope is not called a dead link');
  fake.stdin.write('tx 0\n');
  ok(await p.wait(`!document.getElementById("mShared").classList.contains("icomTx")`), 'unkey: the banner goes');

  // ════ 3. LAYOUT AND LOOK ════
  console.log('── phone width and themes ──');
  await p.js(`document.getElementById("mMenu").click(); 1`);
  await p.call('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(700);
  const ov = await p.js(`(() => { const c = document.getElementById("icomCtls"); return { doc: document.documentElement.scrollWidth, win: innerWidth,
     panel: c.scrollWidth, panelW: c.clientWidth, vis: c.getClientRects().length > 0 }; })()`);
  ok(ov.vis && ov.doc <= ov.win && ov.panel <= ov.panelW + 1, `390 px wide: no horizontal overflow (page ${ov.doc}/${ov.win}, panel ${ov.panel}/${ov.panelW})`);
  await p.call('Emulation.clearDeviceMetricsOverride');
  // ★ Contrast of a panel button's text against the first opaque background behind it, per look.
  const CONTRAST = `(() => { const b = document.querySelector("#icomAtt button:not(.on)") || document.querySelector("#icomAtt button");
    const rgb = (s) => (s.match(/[\\d.]+/g) || []).map(Number);
    const lum = ([r, g, b]) => { const f = (c) => { c /= 255; return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }; return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b); };
    let bg = null; for (let e = b; e; e = e.parentElement) { const c = rgb(getComputedStyle(e).backgroundColor); if (c.length >= 3 && (c.length < 4 || c[3] > 0.5)) { bg = c; break; } }
    if (!bg) bg = [0, 0, 0];
    const fg = rgb(getComputedStyle(b).color); const a = lum(fg), z = lum(bg);
    return Math.round(((Math.max(a, z) + 0.05) / (Math.min(a, z) + 0.05)) * 10) / 10; })()`;
  for (const [name, look] of [['default', {}], ['silver', { ctlBg: '#c8c8c8', ctlBtn: '#c8c8c8' }], ['black', { ctlBg: '#111111', ctlBtn: '#111111' }],
                              ['transparency off', { ctlSolid: 100 }], ['transparency full', { ctlSolid: 0 }]]) {
    await p.js(`(() => { let o = {}; try { o = JSON.parse(localStorage.getItem("vibesdr_web_prefs_v1") || "{}"); } catch {}
      for (const k of ["ctlBg","ctlBtn","ctlFont","ctlSolid"]) delete o[k]; Object.assign(o, ${JSON.stringify(look)});
      localStorage.setItem("vibesdr_web_prefs_v1", JSON.stringify(o)); return 1; })()`);
    await go(p, `http://127.0.0.1:${proxyPort}/`);
    await p.wait(`!!document.getElementById("btnConnect")`, 15000);
    await p.js(`document.getElementById("btnConnect").click(); 1`);
    await startRadio(p);
    await p.wait(`document.querySelectorAll("#icomAtt button").length > 0`, 15000);
    await p.js(`document.getElementById("mMenu").click(); 1`);
    await sleep(500);
    const c = await p.js(CONTRAST);
    ok(c >= 3, `${name}: the panel’s buttons are legible (contrast ${c}:1)`);
  }
  ok(p.errors.length === 0, `no page exceptions${p.errors.length ? ': ' + p.errors.slice(0, 3).join(' | ') : ''}`);
  const st = await stats();
  ok(st.violations === 0, `★★★ the fake radio saw ZERO disallowed frames from all of that (${st.frames} frames)`);
  p.ws.close();
} catch (e) {
  fail++; console.log('   FAIL threw:', e && e.stack || e);
} finally {
  await cleanup();
}
console.log(`web on an Icom: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
