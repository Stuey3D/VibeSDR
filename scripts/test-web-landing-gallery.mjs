// test-web-landing-gallery.mjs — ★★★ V12 beta96 THE GALLERY ON THE LANDING (Stuart 11-27: "along with the air/sea maps on
// the landing screen we could put the servers gallery there too so a user who just wants to look at meteor pics doesnt tie
// up the entire radio") and THE SERVER PIN OVER IT (Stuart 11-27: "if a server has a pin on the entire server … the pin
// applies to gallery and maps on the landing screen too"). A real vibeserver from this tree on the fake dongle, the REAL
// built page, headless Edge with --mute-audio (silent).
//   A. a gallery with items: the landing shows GALLERY with its caption ("… today · newest …"); it opens the gallery panel
//      with NO radio — no WebSocket at all, the server's listener count stays 0; no MAP keys (they need a live map); a text
//      item and a picture open; BACK; CLOSE returns to the landing; Escape does too.
//   B. an empty gallery: no GALLERY key.
//   C. a SERVER PIN, the page asked from this Mac's LAN address (loopback is exempt): GALLERY and AIR & SEA MAP drawn with
//      NO count and no gallery / aircraft read made; a press asks for the PIN and opens nothing; a wrong PIN says so; the
//      right PIN opens the gallery, and the caption comes back after it.
// Screenshots: $SHOTS (default: the OS temp dir).  usage: VIBESERVER_BIN=… node scripts/test-web-landing-gallery.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
const SHOTS = process.env.SHOTS || os.tmpdir();
fs.mkdirSync(SHOTS, { recursive: true });
const LAN = Object.values(os.networkInterfaces()).flat().find((a) => a && a.family === 'IPv4' && !a.internal)?.address;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const kids = [], dirs = [];
process.on('exit', () => { for (const k of kids) { try { k.kill('SIGTERM'); } catch {} } for (const d of dirs) { try { fs.rmSync(d, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} } });
// ★ a 2×2 PNG (a METEOR picture stands in)
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFklEQVR4nGP4z8DAwMDAxMDAwMDAAAANHQEDasKb6QAAAABJRU5ErkJggg==', 'base64');

async function server(tag, { items = true, pin = '' } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-lgal-${tag}-`)); dirs.push(dir);
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  const g = path.join(dir, 'data', 'gallery'); fs.mkdirSync(g);
  const now = Math.floor(Date.now() / 1000);
  let index = '';
  if (items) {
    const add = (f, body, row) => { fs.writeFileSync(path.join(g, f), body); index += JSON.stringify({ f, o: 'background', hz: 137900000, m: 'lrpt', d: '', p: 'Profile 1', r: 'Test radio', bytes: body.length, ...row }) + '\n'; };
    add('20261127-080000-ais-161k975-1000.txt', 'AIS day log\n', { k: 'ais', t: now - 5 * 3600, title: 'AIS 2026-11-27', hz: 161975000, m: 'nfm' });
    add('20261127-090000-navtex-518k0-1001.txt', 'ZCZC GA01\nLANDING GALLERY TEST MESSAGE\nNNNN\n', { k: 'navtex', t: now - 4 * 3600, title: 'NAVTEX GA01', hz: 518000, m: 'usb' });
    add('20261127-100000-lrpt-137m9-1002.png', PNG, { k: 'lrpt', t: now - 3 * 3600 - 120, title: 'METEOR-M2 4 composite' });
  }
  fs.writeFileSync(path.join(g, 'index.jsonl'), index);
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Landing gallery test', adminPass: 'lgal-admin', pin,
    radios: [{ serial: 'lgal', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000 }] }));
  const rtlPort = await freePort(), port = await freePort();
  kids.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' }));
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--users', '4'],
                    { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  kids.push(srv);
  for (let i = 0; i < 120; i++) { try { if ((await fetch(`http://127.0.0.1:${port}/vibeserver.json`)).ok) break; } catch {} await sleep(250); }
  return { port, dir, srv };
}
async function browser(dir) {
  const dbg = await freePort();
  const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  kids.push(edge);
  let list = null;
  for (let i = 0; i < 40 && !list; i++) { await sleep(250); try { list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json(); } catch {} }
  const wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  const netlog = { urls: [], status: new Map() };
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
    if (m.method === 'Network.requestWillBeSent') netlog.urls.push(String(m.params?.request?.url || ''));
    if (m.method === 'Network.responseReceived') netlog.status.set(String(m.params?.response?.url || ''), m.params?.response?.status); };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  const shot = async (name) => { const r = await send('Page.captureScreenshot', { format: 'png' }); if (r.result?.data) fs.writeFileSync(path.join(SHOTS, name), Buffer.from(r.result.data, 'base64')); };
  await send('Page.enable'); await send('Runtime.enable'); await send('Network.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `window.__ws=0;const W=window.WebSocket;window.WebSocket=function(u,p){window.__ws++;window.__wsUrl=String(u);return new W(u,p);};window.WebSocket.prototype=W.prototype;Object.assign(window.WebSocket,W);` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  return { ev, until, send, shot, netlog, close: () => { try { wsC.close(); } catch {} edge.kill(); } };
}
const listeners = async (port) => { try { return (await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`)).json()).listeners; } catch { return -1; } };
const G = `document.getElementById('splashGallery')`;
const PANEL_OPEN = `document.getElementById('galleryPanel').classList.contains('open')`;
const SPLASH_UP = `!document.getElementById('splash').classList.contains('hidden')`;

console.log('── A. the landing\'s GALLERY — no radio taken ──');
{
  const S = await server('items');
  const B = await browser(S.dir);
  try {
    await B.send('Page.navigate', { url: `http://127.0.0.1:${S.port}/` });
    ok(await B.until(`!!${G} && !${G}.hidden`, 15000), 'the landing shows GALLERY (the server has items)');
    const cap = await B.ev(`${G}.textContent`);
    ok(/^GALLERY · 3 today · newest METEOR 3 h ago$/.test(cap), `its caption: "${cap}"`);
    await B.shot('01-landing-gallery-key.png');
    const galReads = B.netlog.urls.filter((u) => /gallery\.json\?offset=0&limit=1&since=86400/.test(u)).length;
    ok(galReads === 1, `one small read for the key, not polled (${galReads})`);
    await B.ev(`${G}.click()`);
    ok(await B.until(`${PANEL_OPEN} && document.querySelectorAll('#galList .sres[role=button]').length >= 3`, 8000), 'it opens the gallery panel with the items');
    ok(await B.ev(`document.getElementById('splash').classList.contains('hidden')`) === true, '…over the landing, no radio started');
    ok(await B.ev(`window.__ws`) === 0, `no WebSocket at all (${await B.ev('window.__wsUrl || "none"')})`);
    ok(await listeners(S.port) === 0, 'the server counts no listener');
    ok(await B.ev(`document.querySelectorAll('#galList .galMap').length`) === 0, 'no MAP keys in the landing\'s gallery (the AIS day has none here)');
    await B.shot('02-gallery-open.png');
    // a text item
    await B.ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find((r) => /NAVTEX GA01/.test(r.textContent)).click()`);
    ok(await B.until(`!document.getElementById('galView').hidden && /LANDING GALLERY TEST MESSAGE/.test(document.getElementById('galText').textContent)`, 6000), 'a NAVTEX item opens as its text');
    await B.ev(`document.getElementById('galBack').click()`);
    ok(await B.until(`document.getElementById('galView').hidden && !document.getElementById('galList').hidden`, 3000), 'BACK returns to the list');
    // a picture
    await B.ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find((r) => /METEOR/.test(r.textContent)).click()`);
    ok(await B.until(`(() => { const i = document.getElementById('galImg'); return !i.hidden && i.complete && i.naturalWidth === 2; })()`, 6000), 'a METEOR picture opens');
    await B.shot('03-gallery-picture.png');
    await B.ev(`document.getElementById('galBack').click()`);
    await B.ev(`document.getElementById('galClose').click()`);
    ok(await B.until(`${SPLASH_UP} && !${PANEL_OPEN}`, 3000), 'CLOSE returns to the landing');
    ok(await B.ev(`document.activeElement === ${G}`) === true, '…with GALLERY focused (keyboard / pad land where they left)');
    await B.ev(`${G}.click()`);
    await B.until(PANEL_OPEN, 3000);
    await B.send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
    await B.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
    ok(await B.until(`${SPLASH_UP} && !${PANEL_OPEN}`, 3000), 'Escape returns to the landing too');
    ok(await B.ev(`window.__ws`) === 0 && await listeners(S.port) === 0, 'still no WebSocket, still no listener');
  } finally { B.close(); S.srv.kill('SIGTERM'); }
}
console.log('── B. an empty gallery ──');
{
  const S = await server('empty', { items: false });
  const B = await browser(S.dir);
  try {
    await B.send('Page.navigate', { url: `http://127.0.0.1:${S.port}/` });
    await B.until(`!!document.getElementById('btnConnect')`, 8000);
    await sleep(2500);
    ok(await B.ev(`!!${G} && ${G}.hidden`) === true, 'nothing kept: no GALLERY key');
  } finally { B.close(); S.srv.kill('SIGTERM'); }
}
console.log(`── C. a SERVER PIN (the page from ${LAN || 'no LAN address'}) ──`);
if (!LAN) console.log('   (not run — no LAN address; loopback is exempt from the PIN)');
else {
  const S = await server('pin', { pin: '4242' });
  const B = await browser(S.dir);
  try {
    await B.send('Page.navigate', { url: `http://${LAN}:${S.port}/` });
    ok(await B.until(`!!${G} && !${G}.hidden`, 15000), 'GALLERY is drawn on a PIN server');
    await sleep(1500);
    const as = await B.ev(`document.getElementById('splashAirSea').hidden ? null : document.getElementById('splashAirSea').textContent`);
    ok(await B.ev(`${G}.textContent`) === 'GALLERY' && as === 'AIR & SEA MAP', `…with NO count, beside AIR & SEA MAP with none ("${await B.ev(`${G}.textContent`)}", "${as}")`);
    const leaked = B.netlog.urls.filter((u) => /gallery\.json|aircraft\.json|ais-ships|airsea\/join/.test(u));
    ok(leaked.length === 0, `nothing is read before the PIN (${leaked.length} gallery / aircraft requests)`);
    await B.shot('04-pin-landing.png');
    await B.ev(`${G}.click()`);
    ok(await B.until(`!document.getElementById('splashViewPin').hidden`, 3000), 'a press asks for the PIN');
    ok(await B.ev(`!${PANEL_OPEN} && ${SPLASH_UP}`) === true, '…and opens nothing');
    ok(await B.ev(`document.activeElement?.id`) === 'splashViewPinInput', '…the PIN box focused');
    await B.shot('05-pin-asked.png');
    await B.ev(`document.getElementById('splashViewPinInput').value = '1111'; document.getElementById('splashViewPinGo').click(); true`);
    ok(await B.until(`/does not open anything here/.test(document.getElementById('splashViewPinMsg').textContent)`, 6000), 'a wrong PIN: "That PIN does not open anything here."');
    ok(await B.ev(`!${PANEL_OPEN} && ${G}.textContent === 'GALLERY'`) === true, '…still nothing open, still no count');
    await B.ev(`document.getElementById('splashViewPinInput').value = '4242'; document.getElementById('splashViewPinGo').click(); true`);
    ok(await B.until(`${PANEL_OPEN} && document.querySelectorAll('#galList .sres[role=button]').length >= 3`, 8000), 'the right PIN opens the gallery with its items');
    ok(await B.ev(`document.getElementById('splashViewPinInput').value`) === '', 'the PIN is off the screen');
    await B.shot('06-pin-gallery-open.png');
    await B.ev(`document.getElementById('galClose').click()`);
    ok(await B.until(`${SPLASH_UP} && /^GALLERY · 3 today/.test(${G}.textContent)`, 5000), `back on the landing, the caption now shows ("${await B.ev(`${G}.textContent`)}")`);
    ok(await B.ev(`window.__ws`) === 0 && await listeners(S.port) === 0, 'no WebSocket, no listener');
  } finally { B.close(); S.srv.kill('SIGTERM'); }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
