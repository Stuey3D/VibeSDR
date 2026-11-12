// test-web-menu-cats.mjs — the web MENU's folding categories (web/client/src/menuCats.ts, index.html #menu) and SAVE FOR
// ALL VIBESDR.NET SERVERS carrying the whole menu (portable.ts VIEW_KEYS, directory/public/store.html), in a REAL
// headless, MUTED Edge against a REAL vibeserver on a fake dongle. Stuart, 2026-11-12: "collapsable catagories like we
// do with the r36s … VFO Centre/Fit stay at the top above all like now all the options default collapsed … the save
// for all .vibesdr.net servers button needs to be at the bottom of the collapsed menus just above the about so that all
// settings are saved and transferred".
//
//  Checked:
//   • VFO CENTRE / CENTRE / FIT visible above every category; SDR HARDWARE · DISPLAY · CONTROLS · ABOUT, all folded on a
//     first visit, nothing inside a folded one drawn;
//   • each opens and folds; one open at a time; the open one is remembered in localStorage and comes back on a reload;
//   • the save block sits below every category and just above ABOUT; SERVER (unlocked owner only) is unfolded, above it;
//   • EVERY listener control in DISPLAY and CONTROLS has a pref, and that pref is a VIEW_KEY (a new row without one
//     fails here); SAVE FOR ALL writes every one to the store and nothing else (no ppm, no tuned, no rsp_*); a second
//     server's page (another origin, no settings of its own) opens with every one of them;
//   • the keyboard and a simulated pad: headers are stops, a folded body's rows are not; Enter / ✕ opens; Escape / ○
//     folds it with the highlight left on its header and the menu still open; again closes the menu;
//   • SERVER (2026-11-12, one owner password box): shown to a listener on a protected receiver, with the box; ONE
//     password input in the page; SDR HARDWARE's "unlock under SERVER below" line folds it and lands on the box;
//     unlocking swaps the box for SERVER ADMIN + LOCK, LOCK brings the box back (and the server agrees);
//   • an idle re-lock (the server's own {"type":"admin","relocked":true} message, delivered on the page's socket — the
//     server sends it only after minutes with no command at all, and the page's own traffic counts) folds the
//     categories and the next MENU lands on SERVER's password box;
//   • RESET beside SAVE FOR ALL: asks first; YES clears the store's defaults (bookmarks kept), puts this server's menu
//     settings back to factory (the same as a server that never had any), leaves the hardware prefs alone, and another
//     server that followed the defaults lets go of its copy;
//   • the keyboard and the pad reach the password box and RESET;
//   • 390 px phone width: no sideways scroll with any category open.
// ★ How (as test-web-chunk-cache.mjs): a local https front (self-signed, trusted by SPKI for this browser only) answers
//   for a/b.vibeserver.vibesdr.net (both → the one test server; two ORIGINS, so two sets of settings) and for
//   vibeserver.vibesdr.net/store (this tree's directory/public/store.html). Nothing leaves the machine.
// ★ SILENT: --mute-audio --disable-audio-output, a throwaway profile.
//
// Usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-menu-cats.mjs   (exit 3 = not run)
//   SHOTS=<dir>   also save the menu at desktop and 390 px in three looks (default, SOLID, light controls)
//   DESKTOP=1     save Web-menu-collapsed.png, Web-menu-hardware-open.png and Web-menu-server-unlock.png to ~/Desktop
import { spawn, execFileSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — needs VIBESERVER_BIN (built from this tree) and Microsoft Edge'); process.exit(3); }
const SHOTS = process.env.SHOTS || '';
const DESKTOP = process.env.DESKTOP ? path.join(os.homedir(), 'Desktop') : '';
if (SHOTS) fs.mkdirSync(SHOTS, { recursive: true });

// ★ portable.ts VIEW_KEYS, read from the source (the test must not trust a copy of its own).
const portableSrc = fs.readFileSync(path.join(root, 'web/client/src/portable.ts'), 'utf8');
const VIEW_KEYS = [...(/export const VIEW_KEYS = \[([\s\S]*?)\] as const/.exec(portableSrc)?.[1] || '').matchAll(/'([A-Za-z0-9_]+)'/g)].map((m) => m[1]);
const storeSrc = fs.readFileSync(path.join(root, 'directory/public/store.html'), 'utf8');
const STORE_KEYS = [...(/var VIEW_KEYS = \[([\s\S]*?)\];/.exec(storeSrc)?.[1] || '').matchAll(/'([A-Za-z0-9_]+)'/g)].map((m) => m[1]);

// ★★ Every listener control in DISPLAY and CONTROLS → the pref it is saved under ('' = an action key, saves nothing).
//    Keyed by the control's id, or its nearest id'd container (a .seg of buttons, a row of swatches).
const PREF_OF = {
  palette: 'palette', specRatio: 'specRatio', dispReset: '',
  wfScrollSeg: 'wfScroll', wfBoostSeg: 'wfBoost', wfSpeedSeg: 'wfSpeed', wfDataSeg: 'wfData', wfCoarse: 'wfCoarse',
  autoContrast: 'autoContrast', minDb: 'minDb', maxDb: 'maxDb', bright: 'wfBrightness', contrast: 'wfContrast',
  sharp: 'wfSharpness', spatialSmooth: 'spatialSmooth',
  specShow: 'specShow', smooth: 'smoothingFrames', specFloor: 'specFloor', specPeak: 'specPeakScale', specAlpha: 'specAlpha',
  peakHold: 'peakHold', knownLabels: 'knownLabels',
  vfoColor: 'vfoColor', vfoGlow: 'vfoIntensity', vfoFrost: 'vfoFrost',
  ctlBgSw: 'ctlBg', ctlBtnSw: 'ctlBtn', ctlFontSw: 'ctlFont', ctlSolid: 'ctlSolid', ctlReset: '',
  wheelAction: 'wheelAction', ptrFreqTg: 'ptrFreq', btnPadGuide: '', padTuckTg: 'padTuck',
};
// ★ Never carried: the radio's, the owner's, where you were tuned. Planted in the page's prefs before the save.
const PLANTED = { ppm: 7, rsp_ifagc: false, biasT: true, tuned: { x: { hz: 7100000, mode: 'lsb' } }, tunerBw: 700000 };

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const tmp = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'vs-menucats-'));
const procs = [];

// ── the server: a fake dongle and an admin password ───────────────────────────────────────────────────────────
const ADMIN = 'menu-cats-admin';
for (const d of ['data', 'run']) fs.mkdirSync(path.join(tmp, d));
fs.writeFileSync(path.join(tmp, 'config.json'), JSON.stringify({ configured: true, adminPass: ADMIN, name: 'Menu test',
  locator: 'IO92nh', mode: 'single', sharing: 'local', users: 4, web: true }));
const rtlPort = await freePort(), srvPort = await freePort();
procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--wfm', '96.1'], { stdio: 'ignore' }));
await sleep(300);
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(srvPort), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(tmp, 'config.json'), VIBESERVER_DATA_DIR: path.join(tmp, 'data'),
         VIBESERVER_RUNTIME_DIR: path.join(tmp, 'run'), HOME: tmp }, stdio: 'ignore' }));
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(srvPort, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}

// ── the https front ─────────────────────────────────────────────────────────────────────────────────────────────────
execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=vibeserver.vibesdr.net',
  '-addext', 'subjectAltName=DNS:vibeserver.vibesdr.net,DNS:*.vibeserver.vibesdr.net',
  '-keyout', path.join(tmp, 'key.pem'), '-out', path.join(tmp, 'cert.pem')], { stdio: 'ignore' });
const cert = fs.readFileSync(path.join(tmp, 'cert.pem'));
const spki = crypto.createHash('sha256').update(new crypto.X509Certificate(cert).publicKey.export({ type: 'spki', format: 'der' })).digest('base64');
const front = https.createServer({ key: fs.readFileSync(path.join(tmp, 'key.pem')), cert }, (req, res) => {
  const host = (req.headers.host || '').split(':')[0];
  if (host === 'vibeserver.vibesdr.net') {
    if ((req.url || '').split('?')[0] === '/store') {
      res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
      res.end(fs.readFileSync(path.join(root, 'directory/public/store.html')));
    } else { res.writeHead(404); res.end(); }
    return;
  }
  const up = http.request({ host: '127.0.0.1', port: srvPort, method: req.method, path: req.url, headers: req.headers }, (r) => {
    res.writeHead(r.statusCode || 502, r.headers); r.pipe(res);
  });
  up.on('error', () => { try { res.writeHead(502); res.end(); } catch { /* gone */ } });
  req.pipe(up);
});
front.on('upgrade', (req, sock, head) => {
  const up = net.connect(srvPort, '127.0.0.1', () => {
    up.write(`${req.method} ${req.url} HTTP/1.1\r\n` + Object.entries(req.headers).map(([k, v]) => `${k}: ${v}`).join('\r\n') + '\r\n\r\n');
    if (head?.length) up.write(head);
    sock.pipe(up).pipe(sock);
  });
  up.on('error', () => sock.destroy()); sock.on('error', () => up.destroy());
});
const frontPort = await freePort();
await new Promise((r) => front.listen(frontPort, '127.0.0.1', r));

// ── the browser ─────────────────────────────────────────────────────────────────────────────────────────────────────
const CDP = await freePort();
const prof = path.join(tmp, 'edge');
procs.push(spawn(EDGE, ['--headless=new', `--remote-debugging-port=${CDP}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions',
  '--mute-audio', '--disable-audio-output', '--autoplay-policy=no-user-gesture-required', `--user-data-dir=${prof}`,
  `--host-resolver-rules=MAP *.vibesdr.net 127.0.0.1:${frontPort},MAP vibesdr.net 127.0.0.1:${frontPort}`,
  `--ignore-certificate-errors-spki-list=${spki}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' }));

// ★ The simulated pad (test-web-gamepad.mjs's): a standard two-stick pad driven through window.__btn.
const FAKE_PAD = `
  window.__pad = { id: 'Test pad (STANDARD GAMEPAD)', index: 0, connected: true, mapping: 'standard', timestamp: 0,
    axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  navigator.getGamepads = () => [window.__pad, null, null, null];
  window.__btn = (i, on) => { window.__pad.buttons[i] = { pressed: on, touched: on, value: on ? 1 : 0 }; };
  window.__connect = () => window.dispatchEvent(Object.assign(new Event('gamepadconnected'), { gamepad: window.__pad }));
  // ★ The page's sockets, so the test can hand one a message exactly as the server would send it.
  window.__sockets = [];
  { const WS = window.WebSocket; window.WebSocket = class extends WS { constructor(...a) { super(...a); window.__sockets.push(this); } }; }
`;

let target = null;
for (let i = 0; i < 60 && !target; i++) {
  await sleep(200);
  try { target = (await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).find((t) => t.type === 'page'); } catch { /* not up yet */ }
}
const ws = new WebSocket(target.webSocketDebuggerUrl);
let id = 0; const pend = new Map(); const contexts = []; const errors = [];
ws.onmessage = (e) => {
  const m = JSON.parse(e.data);
  if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); return; }
  if (m.method === 'Runtime.executionContextCreated') contexts.push(m.params.context);
  if (m.method === 'Runtime.executionContextsCleared') contexts.length = 0;
  if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
};
await new Promise((r) => { ws.onopen = r; });
const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
await send('Page.enable'); await send('Runtime.enable');
await send('Page.addScriptToEvaluateOnNewDocument', { source: FAKE_PAD });
const js = async (expression, extra = {}) => {
  const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true, userGesture: true, ...extra });
  if (r.result?.exceptionDetails) return { error: r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text };
  return r.result?.result?.value;
};
const until = async (expr, ms = 10000) => { for (let t = 0; t < ms; t += 150) { if (await js(expr) === true) return true; await sleep(150); } return false; };
const inStore = async (expr) => {
  const ctx = contexts.find((c) => c.origin === 'https://vibeserver.vibesdr.net');
  return ctx ? js(expr, { contextId: ctx.id }) : { error: 'no store frame' };
};
const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await send('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
const key = async (k) => {
  const vk = { ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, Enter: 13, Escape: 27 }[k];
  const text = k === 'Enter' ? '\r' : undefined;
  await send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', key: k, windowsVirtualKeyCode: vk, text, unmodifiedText: text });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: k, windowsVirtualKeyCode: vk });
  await sleep(90);
};
const press = async (i) => { await js(`__btn(${i}, true); 1`); await sleep(120); await js(`__btn(${i}, false); 1`); await sleep(120); };
const shot = async (file, clipMenu = false) => {
  let clip;
  await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 2, y: 2 });   // ★ no hover glow left on a header
  if (clipMenu) {
    // ★ The whole menu card, even where it scrolls: grow the card for the picture, then put it back.
    const r = await js(`(() => { const m = document.getElementById('menu'); m.dataset.oldMh = m.style.maxHeight; m.style.maxHeight = 'none'; m.style.transition = 'none'; m.style.top = '12px'; m.style.transform = 'translate(-50%, 0)';
      const b = m.getBoundingClientRect(); return { x: Math.max(0, b.left - 12), y: Math.max(0, b.top - 12), w: b.width + 24, h: b.height + 24 }; })()`);
    clip = { x: r.x, y: r.y, width: r.w, height: r.h, scale: 1 };
  }
  const r = await send('Page.captureScreenshot', { format: 'png', ...(clip ? { clip, captureBeyondViewport: true } : {}) });
  if (clipMenu) await js(`(() => { const m = document.getElementById('menu'); m.style.maxHeight = m.dataset.oldMh || ''; m.style.top = ''; m.style.transform = ''; m.style.transition = ''; })()`);
  fs.writeFileSync(file, Buffer.from(r.result.data, 'base64'));
  console.log(`        saved ${file}`);
};

/** Open server `h`'s page, start the radio, wait for the store frame. */
const visit = async (h) => {
  await send('Page.navigate', { url: `https://${h}.vibeserver.vibesdr.net/` });
  await until(`document.readyState === 'complete' && (!!document.getElementById('btnConnect') || !!document.getElementById('audioGate'))`, 20000);
  await js(`(() => { const b = document.getElementById('btnConnect'); if (b && b.offsetParent) b.click(); return 1; })()`);
  await until(`!!document.getElementById('audioGate')`, 20000);
  for (let i = 0; i < 6; i++) {
    const at = await js(`(() => { const b = document.querySelector('#audioGate button'); if (!b) return null; const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
    if (!at) break;
    await tap(at[0], at[1]);
    if (await until(`!document.getElementById('audioGate')`, 3000)) break;
  }
  await until(`parseFloat(document.getElementById('freqInput').value) > 0`, 15000);
  await until(`!!document.querySelector('iframe[src^="https://vibeserver.vibesdr.net"]')`, 8000);
  await sleep(800);
};
const openMenu = async () => { await js(`document.getElementById('menu').classList.contains('open') || document.getElementById('mMenu').click(); 1`); await until(`document.getElementById('menu').classList.contains('open')`, 3000); await sleep(250); };
const closeMenu = async () => { await js(`document.getElementById('menu').classList.remove('open'); 1`); await sleep(250); };
const HEADERS = `[...document.querySelectorAll('#menu > button.mCat')].filter((b) => b.getBoundingClientRect().height > 2)`;
const state = () => js(`(() => { const o = {}; for (const b of document.querySelectorAll('#menu > button.mCat')) {
  const body = document.querySelector('#menu .mCatBody[data-cat="' + b.dataset.cat + '"]');
  o[b.dataset.cat] = { exp: b.getAttribute('aria-expanded') === 'true', shown: !body.hidden && body.getBoundingClientRect().height > 2 }; } return o; })()`);
const hdr = (cat) => js(`document.querySelector('#menu > button.mCat[data-cat="${cat}"]').click(); 1`).then(() => sleep(150));
const saved = () => js(`(() => { try { return localStorage.getItem('vibesdr_menu_cat_v1'); } catch { return 'ERR'; } })()`);

/** The DOM state of every control in PREF_OF — what a listener SEES, to compare between two servers' pages. */
const READ_CONTROLS = `(() => { const o = {}; const map = ${JSON.stringify(PREF_OF)};
  for (const [id, k] of Object.entries(map)) { if (!k) continue; const el = document.getElementById(id); if (!el) { o[k] = 'MISSING'; continue; }
    if (el.tagName === 'SELECT' || el.tagName === 'INPUT') o[k] = el.value;
    else if (el.tagName === 'BUTTON') o[k] = el.classList.contains('on') + ':' + el.textContent;
    else { const on = [...el.querySelectorAll('button')].filter((b) => b.classList.contains('on') || b.getAttribute('aria-pressed') === 'true' || b.classList.contains('sel'));
           o[k] = on.map((b) => b.textContent.trim() + '|' + (b.title || '') + '|' + JSON.stringify(b.dataset)).join(','); } }
  return o; })()`;

try {
  console.log('the web MENU — folding categories and SAVE FOR ALL (real Edge, muted; real vibeserver; local store)');
  ok(VIEW_KEYS.length > 20 && JSON.stringify([...VIEW_KEYS].sort()) === JSON.stringify([...STORE_KEYS].sort()),
     `portable.ts VIEW_KEYS and store.html's copy are the same list (${VIEW_KEYS.length} keys)`);

  await visit('a');
  ok(await js(`!!document.getElementById('saveAllBlock') && !document.getElementById('saveAllBlock').hidden`), 'on a *.vibeserver.vibesdr.net page the save block is offered');
  await js(`(() => { try { localStorage.removeItem('vibesdr_menu_cat_v1'); } catch {} return 1; })()`);
  await send('Page.reload'); await visit('a');

  // ── 1. The first look ──
  console.log('── first look ──');
  await openMenu();
  const titles = await js(`${HEADERS}.map((b) => b.textContent.trim())`);
  ok(JSON.stringify(titles) === JSON.stringify(['SDR HARDWARE', 'DISPLAY', 'CONTROLS', 'ABOUT']), `the categories, in order: ${titles}`);
  const st0 = await state();
  ok(Object.values(st0).every((s) => !s.exp && !s.shown), 'all folded on a first visit');
  ok(await js(`(() => { const first = ${HEADERS}[0].getBoundingClientRect().top;
    return ['mPanelLock', 'mPanelCentre', 'mPanelFit'].every((id) => { const r = document.getElementById(id).getBoundingClientRect(); return r.height > 2 && r.bottom <= first; }); })()`),
     'VFO CENTRE, CENTRE and FIT are visible, above every category');
  ok(await js(`[...document.querySelectorAll('#menu .mCatBody')].every((b) => [...b.querySelectorAll('button,input,select')].every((e) => e.getBoundingClientRect().height < 2))`),
     'nothing inside a folded category is drawn');
  ok(await js(`(() => { const b = document.getElementById('srvBlock'), i = document.getElementById('adminPwInput');
      return !b.hidden && b.getBoundingClientRect().height > 2 && b.contains(i) && i.getBoundingClientRect().height > 2 && !b.closest('.mCatBody'); })()`),
     'SERVER is shown to a listener on a protected receiver, unfolded, with the password box');
  ok(await js(`document.getElementById('adminServerRow').hidden && document.getElementById('btnServerAdmin').getBoundingClientRect().height < 2`),
     '…and no SERVER ADMIN until unlocked');
  ok(await js(`document.querySelectorAll('#menu input[type=password]').length === 1 && document.querySelectorAll('[id=adminPwInput]').length === 1`),
     'ONE password input in the menu (moved, not copied)');
  ok(await js(`(() => { const sv = document.getElementById('srvBlock'), sa = document.getElementById('saveAllBlock'), c = document.getElementById('mCatControlsBody');
      return !!(c.compareDocumentPosition(sv) & Node.DOCUMENT_POSITION_FOLLOWING) && !!(sv.compareDocumentPosition(sa) & Node.DOCUMENT_POSITION_FOLLOWING); })()`),
     'SERVER sits after CONTROLS, above ALL SERVERS and ABOUT');
  const order = await js(`(() => { const sv = document.getElementById('saveAllBlock'), ab = document.querySelector('#menu > button.mCat[data-cat="about"]');
    const cats = [...document.querySelectorAll('#menu > button.mCat, #menu > .mCatBody, #srvBlock')].filter((n) => n.dataset.cat !== 'about');
    const afterAll = cats.every((c) => c.compareDocumentPosition(sv) & Node.DOCUMENT_POSITION_FOLLOWING);
    const beforeAbout = !!(sv.compareDocumentPosition(ab) & Node.DOCUMENT_POSITION_FOLLOWING);
    const between = [...document.querySelectorAll('#menu > *')].filter((n) => (sv.compareDocumentPosition(n) & Node.DOCUMENT_POSITION_FOLLOWING) && (n.compareDocumentPosition(ab) & Node.DOCUMENT_POSITION_FOLLOWING));
    const r = sv.getBoundingClientRect(), a = ab.getBoundingClientRect(), c = document.getElementById('srvBlock').getBoundingClientRect();
    return { afterAll, beforeAbout, between: between.length, drawn: r.top >= c.bottom && r.bottom <= a.top }; })()`);
  ok(order.afterAll && order.beforeAbout && order.between === 0 && order.drawn, `SAVE FOR ALL sits below every category and just above ABOUT (${JSON.stringify(order)})`);
  // ★ SERVER (the password box, always shown on a protected receiver — Stuart, 2026-11-12) is the one thing allowed to
  //   push the folded menu past the card at 1280×900: every category header and SAVE FOR ALL still fit without it.
  const mh = await js(`(() => { const m = document.getElementById('menu'), c = document.getElementById('mCatControls'), sa = document.getElementById('saveAllBlock');
    return [m.scrollHeight, m.clientHeight, Math.round(sa.getBoundingClientRect().top - c.getBoundingClientRect().bottom - parseFloat(getComputedStyle(sa).marginTop))]; })()`);
  ok(mh[0] - mh[2] <= mh[1] + 1, `the folded menu, less SERVER, fits without scrolling at 1280×900 (${mh[0]} px of content, ${mh[2]} of it SERVER, in ${mh[1]})`);
  if (DESKTOP) await shot(path.join(DESKTOP, 'Web-menu-collapsed.png'), true);
  if (SHOTS) await shot(path.join(SHOTS, 'desktop-collapsed.png'), true);

  // ── 2. Open and fold each; one at a time; remembered ──
  console.log('── open and fold ──');
  for (const cat of ['hw', 'display', 'controls', 'about']) {
    await hdr(cat);
    const s = await state();
    ok(Object.entries(s).every(([k, v]) => (k === cat) === (v.exp && v.shown)) && (await saved()) === cat, `${cat}: opens, the others stay folded, remembered (${await saved()})`);
  }
  await hdr('display'); await hdr('controls');
  ok((await state()).display.shown === false && (await state()).controls.shown, 'opening CONTROLS folds DISPLAY (one at a time, as the R36S)');
  await hdr('controls');
  ok(Object.values(await state()).every((v) => !v.exp && !v.shown) && (await saved()) === null, 'pressed again: folded, and nothing remembered');
  await hdr('hw');
  ok(await js(`(() => { const b = document.getElementById('mCatHwBody'); return ['radioName', 'adminJump', 'rowGain', 'rowRate'].every((id) => b.contains(document.getElementById(id)))
      && b.contains(document.getElementById('rowAntenna')) && document.getElementById('adminJump').getBoundingClientRect().height > 2 && !b.querySelector('input[type=password]'); })()`),
     'SDR HARDWARE holds the receiver, the "unlock under SERVER below" line (shown), gain, sample rate and the antenna — and no password box');
  ok(await js(`/Locked by the operator — unlock under SERVER below/.test(document.getElementById('adminJump').textContent)`), '…the line reads "Locked by the operator — unlock under SERVER below"');
  if (DESKTOP) await shot(path.join(DESKTOP, 'Web-menu-hardware-open.png'), true);
  if (SHOTS) await shot(path.join(SHOTS, 'desktop-hardware-open.png'), true);
  await js(`document.getElementById('adminJump').click(); 1`);
  await sleep(300);
  ok(!(await state()).hw.exp && await js(`document.activeElement?.id === 'adminPwInput'`) && await js(`(() => { const r = document.getElementById('adminPwInput').getBoundingClientRect(), m = document.getElementById('menu').getBoundingClientRect();
      return r.height > 2 && r.top >= m.top && r.bottom <= m.bottom; })()`),
     'pressing it folds SDR HARDWARE and lands the cursor in SERVER\'s password box, in view');
  await js(`document.activeElement?.blur(); 1`);
  await hdr('hw');
  await hdr('display');
  await closeMenu();
  await send('Page.reload'); await visit('a'); await openMenu();
  ok((await state()).display.shown && !(await state()).hw.shown, 'a reload comes back with DISPLAY open (localStorage)');
  await hdr('display');
  ok(await js(`(() => { const b = document.getElementById('mCatControlsBody'); return ['wheelAction', 'rowPtrFreq', 'btnPadGuide', 'padTuckTg', 'ctlSolid'].every((id) => b.contains(document.getElementById(id))); })()`),
     'CONTROLS holds the colours, the mouse rows and the game controller');

  // ── 3. Every listener control has a travelling pref ──
  console.log('── SAVE FOR ALL carries the whole menu ──');
  const owners = await js(`(() => { const out = new Set();
    for (const id of ['mCatDisplayBody', 'mCatControlsBody']) for (const e of document.getElementById(id).querySelectorAll('button, input, select')) {
      let o = e.id ? e : e.parentElement; while (o && !o.id) o = o.parentElement; out.add(o.id); }
    return [...out]; })()`);
  const unknown = owners.filter((o) => !(o in PREF_OF));
  ok(unknown.length === 0, `every control in DISPLAY and CONTROLS is accounted for (${owners.length}${unknown.length ? '; NEW: ' + unknown.join(', ') : ''})`);
  const wanted = [...new Set(Object.values(PREF_OF).filter(Boolean))];
  const notTravelling = wanted.filter((k) => !VIEW_KEYS.includes(k));
  ok(notTravelling.length === 0, `…and each one's pref is a VIEW_KEY (${notTravelling.join(', ') || 'all ' + wanted.length})`);

  // Change every one of them from its default, the way a listener would.
  await js(`(() => {
    const click = (sel) => { const b = document.querySelector(sel); b && b.click(); };
    const range = (id) => { const e = document.getElementById(id); const lo = +e.min, hi = +e.max, v = +e.value;
      e.value = String(v === lo + Math.round((hi - lo) / 3) ? hi : lo + Math.round((hi - lo) / 3)); e.dispatchEvent(new Event('input', { bubbles: true })); e.dispatchEvent(new Event('change', { bubbles: true })); };
    const pal = document.getElementById('palette'); pal.selectedIndex = (pal.selectedIndex + 2) % pal.options.length; pal.dispatchEvent(new Event('change', { bubbles: true }));
    for (const id of ['specRatio', 'autoContrast', 'bright', 'contrast', 'sharp', 'smooth', 'specFloor', 'specPeak', 'specAlpha', 'vfoGlow', 'vfoFrost', 'ctlSolid']) range(id);
    click('#wfScrollSeg [data-wfscroll="eco"]'); click('#wfBoostSeg [data-wfboost="0"]'); click('#wfSpeedSeg [data-wfspeed="30"]');
    click('#wfDataSeg [data-wfdata="low"]'); click('#wfCoarse [data-coarse="manual"]');
    range('minDb'); range('maxDb');
    for (const id of ['spatialSmooth', 'specShow', 'peakHold', 'knownLabels', 'ptrFreqTg', 'padTuckTg']) document.getElementById(id).click();
    const c = document.getElementById('vfoColor'); c.value = '#20ff40'; c.dispatchEvent(new Event('input', { bubbles: true }));
    click('#wheelAction [data-v="1"]');
    for (const id of ['ctlBgSw', 'ctlBtnSw', 'ctlFontSw']) { const b = document.getElementById(id).querySelectorAll('button')[3]; b && b.click(); }
    return 1; })()`);
  await sleep(300);
  const local = await js(`JSON.parse(localStorage.getItem('vibesdr_web_prefs_v1') || '{}')`);
  const missingLocal = wanted.filter((k) => !(k in local));
  ok(missingLocal.length === 0, `every one was saved on this server (${missingLocal.join(', ') || wanted.length + ' prefs'})`);
  await js(`(() => { const p = JSON.parse(localStorage.getItem('vibesdr_web_prefs_v1') || '{}'); Object.assign(p, ${JSON.stringify(PLANTED)});
    localStorage.setItem('vibesdr_web_prefs_v1', JSON.stringify(p)); return 1; })()`);
  const onA = await js(READ_CONTROLS);
  await js(`document.getElementById('dispSaveAll').click(); 1`);
  const said = await until(`/menu settings are now your default/.test(document.body.innerText)`, 6000);
  ok(said, 'SAVE FOR ALL says it saved the menu settings');
  const view = await inStore(`(JSON.parse(localStorage.getItem('vsPortable') || 'null') || {}).view || {}`);
  const lost = wanted.filter((k) => !(k in view) || JSON.stringify(view[k]) !== JSON.stringify(local[k]));
  ok(lost.length === 0, `the store holds every listed setting, as set (${lost.join(', ') || wanted.length + ' of ' + wanted.length})`);
  const extra = Object.keys(view).filter((k) => !VIEW_KEYS.includes(k));
  ok(extra.length === 0 && !Object.keys(PLANTED).some((k) => k in view), `…and nothing else — no ppm, rsp_*, bias-T, tuned or IF filter (${Object.keys(view).length} keys${extra.length ? '; EXTRA ' + extra : ''})`);
  console.log(`        carried: ${Object.keys(view).sort().join(', ')}`);

  // ── 4. Another server's page picks it all up ──
  await visit('b');
  const onB = await js(READ_CONTROLS);
  const diff = Object.keys(onA).filter((k) => onA[k] !== onB[k]);
  ok(diff.length === 0, `server B opens with every one of them (${diff.map((k) => `${k}: ${onA[k]} ≠ ${onB[k]}`).join('; ') || Object.keys(onA).length + ' controls the same'})`);
  ok(await js(`!('ppm' in JSON.parse(localStorage.getItem('vibesdr_web_prefs_v1') || '{}'))`), "…and none of A's hardware prefs");

  // ── 5. Keyboard: headers are stops, a folded body's rows are not ──
  console.log('── keyboard and pad ──');
  await js(`(() => { try { localStorage.removeItem('vibesdr_menu_cat_v1'); } catch {} return 1; })()`);
  await send('Page.reload'); await visit('b');
  await openMenu();
  const seen = [];
  for (let i = 0; i < 40; i++) {
    await key('ArrowDown');
    const f = await js(`(() => { const h = document.querySelector('.gpFocus'); if (!h) return null;
      const body = h.closest('.mCatBody'); return { t: (h.id || '') + ':' + h.textContent.trim().slice(0, 30), inFolded: !!body && body.hidden, inMenu: document.getElementById('menu').contains(h) }; })()`);
    if (f && (!seen.length || seen[seen.length - 1].t !== f.t)) seen.push(f);
  }
  const stops = seen.map((f) => f.t);
  ok(seen.every((f) => f.inMenu && !f.inFolded), `the highlight walks the menu and never into a folded category (${stops.length} stops)`);
  ok(['SDR HARDWARE', 'DISPLAY', 'CONTROLS', 'ABOUT'].every((t) => stops.some((s) => s.endsWith(':' + t))),
     `…every header is a stop: ${stops.join(' · ')}`);
  // ★ SAVE FOR ALL and RESET share a row, as the box and UNLOCK do: down lands on one, left / right reach the other.
  for (let i = 0; i < 30 && !(await js(`/^(dispSaveAll|dispResetAll)$/.test(document.querySelector('.gpFocus')?.id || '')`)); i++) await key('ArrowUp');
  const rowPair = [await js(`document.querySelector('.gpFocus')?.id`)];
  await key(rowPair[0] === 'dispResetAll' ? 'ArrowLeft' : 'ArrowRight'); rowPair.push(await js(`document.querySelector('.gpFocus')?.id`));
  ok(rowPair.includes('dispSaveAll') && rowPair.includes('dispResetAll'), `SAVE FOR ALL and RESET are both reached (${rowPair.join(' → ')})`);
  for (let i = 0; i < 10 && !/^(adminPwInput|adminPwGo)$/.test(await js(`document.querySelector('.gpFocus')?.id || ''`)); i++) await key('ArrowUp');
  const pwPair = [await js(`document.querySelector('.gpFocus')?.id`)];
  await key(pwPair[0] === 'adminPwGo' ? 'ArrowLeft' : 'ArrowRight'); pwPair.push(await js(`document.querySelector('.gpFocus')?.id`));
  ok(pwPair.includes('adminPwInput') && pwPair.includes('adminPwGo'), `…and the password box and UNLOCK beside it (${pwPair.join(' → ')})`);
  ok(stops.some((s) => s.startsWith('adminPwInput')) && stops.some((s) => s.startsWith('dispSaveAll')), '…and so are SERVER\'s password box and SAVE FOR ALL (keyboard, straight down)');
  for (let i = 0; i < 30 && !(await js(`document.querySelector('.gpFocus')?.dataset.cat === 'display'`)); i++) await key('ArrowUp');
  ok(await js(`document.querySelector('.gpFocus')?.dataset.cat === 'display'`), 'the highlight reaches the DISPLAY header');
  await key('Enter');
  ok((await state()).display.shown, 'Enter opens it');
  await key('ArrowDown');
  ok(await js(`!!document.querySelector('.gpFocus')?.closest('#mCatDisplayBody')`), '…and down goes into it');
  await key('Escape');
  ok(!(await state()).display.shown && await js(`document.getElementById('menu').classList.contains('open')`), 'Escape folds it — the menu stays open');
  ok(await js(`document.querySelector('.gpFocus')?.dataset.cat === 'display'`), '…with the highlight back on its header');
  await key('Escape');
  ok(await until(`!document.getElementById('menu').classList.contains('open')`, 3000), 'Escape again closes the menu');

  // ── 6. The pad ──
  await js(`__connect(); 1`);
  await openMenu();
  for (let i = 0; i < 30 && !(await js(`document.querySelector('.gpFocus')?.dataset.cat === 'controls'`)); i++) await press(13);
  ok(await js(`document.querySelector('.gpFocus')?.dataset.cat === 'controls'`), 'the D-pad reaches the CONTROLS header');
  await press(0);
  ok((await state()).controls.shown, '✕ opens it');
  await press(13);
  ok(await js(`!!document.querySelector('.gpFocus')?.closest('#mCatControlsBody')`), '…the D-pad goes into it');
  await press(1);
  ok(!(await state()).controls.shown && await js(`document.getElementById('menu').classList.contains('open') && document.querySelector('.gpFocus')?.dataset.cat === 'controls'`),
     '○ folds it, the highlight on its header, the menu still open');
  // ★ The pad reaches SERVER's password box and RESET (the D-pad, down from CONTROLS).
  const padSeen = new Set();
  for (let i = 0; i < 20; i++) { await press(13); const f = await js(`document.querySelector('.gpFocus')?.id || ''`); padSeen.add(f); if (f === 'dispSaveAll') break; }
  await press(15); padSeen.add(await js(`document.querySelector('.gpFocus')?.id || ''`));
  ok(padSeen.has('adminPwInput') && padSeen.has('dispResetAll'), `the D-pad reaches the password box and RESET (${[...padSeen].join(' · ')})`);
  await press(0);
  ok(await until(`!document.getElementById('resetAllAsk').hidden`, 2000), '✕ on RESET asks first');
  const askSeen = [];
  for (let i = 0; i < 6 && !(await js(`document.querySelector('.gpFocus')?.id === 'resetAllNo'`)); i++) {
    await press(13);
    askSeen.push(await js(`document.querySelector('.gpFocus')?.id || ''`));
  }
  ok(await js(`document.querySelector('.gpFocus')?.id === 'resetAllNo'`), `…and the D-pad reaches CANCEL (${askSeen.join(' · ')})`);
  await press(0);
  ok(await until(`document.getElementById('resetAllAsk').hidden`, 2000), '✕ on CANCEL puts the question away, nothing reset');
  for (let i = 0; i < 30 && !(await js(`document.querySelector('.gpFocus')?.dataset.cat === 'controls'`)); i++) await press(12);
  await press(1);
  ok(await until(`!document.getElementById('menu').classList.contains('open')`, 3000), '○ again closes the menu');
  ok(await js(`!document.getElementById('menu').classList.contains('open') && localStorage.getItem('vibesdr_menu_cat_v1') === null`), 'and nothing is left open for next time');

  // ── 7. Phone width ──
  console.log('── 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(600);
  await openMenu();
  for (const cat of ['', 'hw', 'display', 'controls', 'about']) {
    if (cat) await hdr(cat);
    const o = await js(`(() => { const m = document.getElementById('menu'); return { sw: m.scrollWidth, cw: m.clientWidth, doc: document.documentElement.scrollWidth, win: innerWidth }; })()`);
    ok(o.sw <= o.cw + 1 && o.doc <= o.win + 1, `${cat || 'folded'}: no sideways scroll (menu ${o.sw}/${o.cw}, page ${o.doc}/${o.win})`);
    if (SHOTS) await shot(path.join(SHOTS, `phone-${cat || 'folded'}.png`), true);
    if (cat) await hdr(cat);
  }
  if (SHOTS) {
    for (const [name, solid, light] of [['solid', 100, false], ['light', 0, true]]) {
      await js(`(() => { const e = document.getElementById('ctlSolid'); e.value = '${solid}'; e.dispatchEvent(new Event('input', { bubbles: true }));
        ${light ? `document.getElementById('ctlBgSw').querySelectorAll('button')[7]?.click(); document.getElementById('ctlFontSw').querySelectorAll('button')[5]?.click();` : ''} return 1; })()`);
      await sleep(300);
      await hdr('controls');
      await shot(path.join(SHOTS, `phone-controls-${name}.png`), true);
      await hdr('controls');
      await send('Emulation.clearDeviceMetricsOverride'); await sleep(500);
      await hdr('display');
      await shot(path.join(SHOTS, `desktop-display-${name}.png`), true);
      await hdr('display');
      await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true }); await sleep(500);
    }
  }
  await send('Emulation.clearDeviceMetricsOverride'); await sleep(400);

  // ── 8. The owner: the box in SERVER; SERVER ADMIN + LOCK; LOCK; an idle re-lock lands on the box ──
  console.log('── the owner ──');
  await js(`(() => { const i = document.getElementById('adminPwInput'); i.value = ${JSON.stringify(ADMIN)}; document.getElementById('adminPwGo').click(); return 1; })()`);
  ok(await until(`!document.getElementById('adminServerRow').hidden`, 6000), 'unlocked through SERVER\'s box');
  ok(await js(`(() => { const s = document.getElementById('adminServerRow'), v = document.getElementById('saveAllBlock'), b = document.getElementById('srvBlock');
      return b.contains(s) && !!(s.compareDocumentPosition(v) & Node.DOCUMENT_POSITION_FOLLOWING) && document.getElementById('adminPwRow').hidden
      && ['btnServerAdmin', 'adminLock'].every((id) => document.getElementById(id).getBoundingClientRect().height > 2); })()`),
     'unlocked: the box gives way to SERVER ADMIN and LOCK, above SAVE FOR ALL');
  ok(await js(`document.getElementById('adminJump').hidden`), '…and SDR HARDWARE\'s locked line is gone');
  if (SHOTS) await shot(path.join(SHOTS, 'desktop-owner-folded.png'), true);
  await js(`document.getElementById('adminLock').click(); 1`);
  ok(await until(`!document.getElementById('adminPwRow').hidden && document.getElementById('adminServerRow').hidden && !document.getElementById('adminJump').hidden`, 3000),
     'LOCK: SERVER ADMIN and LOCK go, the box comes back');
  await sleep(1200);
  ok(await js(`!/not accepted/.test(document.body.innerText) && document.getElementById('gain')?.closest('.mrow') !== null`), '…without a "password not accepted" (LOCK\'s own answer)');
  ok(await js(`document.getElementById('biasT') ? document.getElementById('biasT').disabled : true`), '…and the protected controls are locked again');
  // Unlock again, for the re-lock.
  await js(`(() => { const i = document.getElementById('adminPwInput'); i.value = ${JSON.stringify(ADMIN)}; document.getElementById('adminPwGo').click(); return 1; })()`);
  ok(await until(`!document.getElementById('adminServerRow').hidden`, 6000), 'unlocked again after LOCK (the server took the password)');
  if (DESKTOP || SHOTS) {
    await openMenu();
    await js(`document.getElementById('srvBlock').scrollIntoView({ block: 'nearest' }); 1`);
  }
  await hdr('display');
  await closeMenu();
  const handed = await js(`(() => { let n = 0; for (const s of window.__sockets) if (s.readyState === 1) {
      s.dispatchEvent(new MessageEvent('message', { data: JSON.stringify({ type: 'admin', ok: false, relocked: true, idleMin: 30 }) })); n++; } return n; })()`);
  const relocked = await until(`!document.getElementById('adminPwRow').hidden && document.getElementById('adminServerRow').hidden`, 5000);
  ok(relocked, `the idle re-lock message (handed to ${handed} open sockets) re-locks the page`);
  ok(await js(`/unlock again in MENU → SERVER/.test(document.body.innerText)`), '…and its pill says where to unlock again: MENU → SERVER');
  await sleep(500);
  ok(Object.values(await state()).every((v) => !v.exp) && await js(`localStorage.getItem('vibesdr_menu_cat_v1') === null`), 'the re-lock folded the categories, so SERVER sits right under them');
  await openMenu();
  ok(await until(`document.activeElement?.id === 'adminPwInput' || document.querySelector('.gpFocus')?.id === 'adminPwInput'`, 2000) && await js(`(() => { const r = document.getElementById('adminPwInput').getBoundingClientRect(), m = document.getElementById('menu').getBoundingClientRect();
      return r.height > 2 && r.top >= m.top && r.bottom <= m.bottom; })()`), '…and the next MENU lands on SERVER\'s password box');
  if (DESKTOP) await shot(path.join(DESKTOP, 'Web-menu-server-unlock.png'), true);
  if (SHOTS) await shot(path.join(SHOTS, 'desktop-server-unlock.png'), true);
  await js(`document.activeElement?.blur(); 1`);
  await closeMenu();

  // ── 9. RESET: the store's defaults and this server's menu — never the hardware ──
  console.log('── RESET ──');
  const bmBefore = await inStore(`JSON.stringify((JSON.parse(localStorage.getItem('vsPortable') || 'null') || {}).bookmarks ?? null)`);
  await js(`(() => { const p = JSON.parse(localStorage.getItem('vibesdr_web_prefs_v1') || '{}'); Object.assign(p, ${JSON.stringify(PLANTED)}, { wfSpeed: 30, palette: 'gray' });
    localStorage.setItem('vibesdr_web_prefs_v1', JSON.stringify(p)); return 1; })()`);
  ok(Object.keys(await inStore(`(JSON.parse(localStorage.getItem('vsPortable') || 'null') || {}).view || {}`)).length > 20, 'before: the store holds the all-servers defaults');
  await openMenu();
  await js(`document.getElementById('dispResetAll').click(); 1`);
  ok(await until(`!document.getElementById('resetAllAsk').hidden`, 2000) && await js(`(() => { const t = document.getElementById('resetAllText').textContent;
      return /Are you sure\?/.test(t) && /all VibeSDR\.net servers/.test(t) && /Other servers keep anything you set on them individually/.test(t) && /hardware/.test(t); })()`),
     'RESET asks "Are you sure?" — what is cleared, and that other servers keep their own');
  await js(`document.getElementById('resetAllYes').click(); 1`);
  await sleep(1500);
  await visit('b');
  const viewAfter = await inStore(`(JSON.parse(localStorage.getItem('vsPortable') || 'null') || {}).view || {}`);
  ok(Object.keys(viewAfter).length === 0, `the store's all-servers defaults are cleared (${Object.keys(viewAfter).length} keys left)`);
  ok(await inStore(`JSON.stringify((JSON.parse(localStorage.getItem('vsPortable') || 'null') || {}).bookmarks ?? null)`) === bmBefore, '…and its bookmarks are untouched');
  const lpAfter = await js(`JSON.parse(localStorage.getItem('vibesdr_web_prefs_v1') || '{}')`);
  // ★ A page writes a few of its own defaults as it loads (wfCoarse 'auto'); factory is what a server that never had a
  //   setting writes — checked against server C below.
  const stillSet = VIEW_KEYS.filter((k) => k in lpAfter && k !== 'volume' && k !== 'freqUnit');
  // ★ (bias-T is not checked: the page takes it from the SERVER's word on every connect, reset or not.)
  ok(['ppm', 'rsp_ifagc', 'tunerBw'].every((k) => JSON.stringify(lpAfter[k]) === JSON.stringify(PLANTED[k])) && JSON.stringify(lpAfter.tuned?.x) === JSON.stringify(PLANTED.tuned.x),
     `…and the hardware prefs (ppm, rsp_ifagc, tuned, IF filter) are untouched (${JSON.stringify({ ppm: lpAfter.ppm, rsp_ifagc: lpAfter.rsp_ifagc, tunerBw: lpAfter.tunerBw })})`);
  ok(await js(`/menu is back to how it came/.test(document.body.innerText)`), '…and it says so after the reload');
  const onBReset = await js(READ_CONTROLS);
  // ★ Factory, as a server that never had a setting shows it: a third origin, nothing stored, an empty store.
  await visit('c');
  const onC = await js(READ_CONTROLS);
  const cSets = await js(`JSON.parse(localStorage.getItem('vibesdr_web_prefs_v1') || '{}')`);
  const notOwn = stillSet.filter((k) => JSON.stringify(lpAfter[k]) !== JSON.stringify(cSets[k]));
  ok(notOwn.length === 0, `this server's menu settings are back to factory (${notOwn.map((k) => k + '=' + JSON.stringify(lpAfter[k])).join(', ') || 'only what a fresh page writes itself: ' + (stillSet.join(', ') || 'none')})`);
  const notFactory = Object.keys(onC).filter((k) => onC[k] !== onBReset[k]);
  ok(notFactory.length === 0, `…every menu control on this server shows its factory value (${notFactory.map((k) => `${k}: ${onBReset[k]} ≠ ${onC[k]}`).join('; ') || Object.keys(onC).length + ' controls'})`);
  // ★ Server A followed the defaults (SAVE FOR ALL dropped its own) and keeps a copy of them: the clear reaches it.
  await visit('a');
  const onAReset = await js(READ_CONTROLS);
  const aStuck = Object.keys(onC).filter((k) => onC[k] !== onAReset[k]);
  ok(aStuck.length === 0 && await js(`localStorage.getItem('vsPortableMasterCopy') === null`), `another server that followed the defaults lets go of them (${aStuck.join(', ') || 'factory'})`);

  ok(errors.length === 0, `no page errors${errors.length ? ': ' + errors.slice(0, 3).join(' | ') : ''}`);
} catch (e) {
  ok(false, 'threw: ' + (e?.stack || e));
} finally {
  try { ws.close(); } catch { /* gone */ }
  for (const p of procs) { try { p.kill(); } catch { /* gone */ } }
  front.close();
  await sleep(500);
  fs.rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
