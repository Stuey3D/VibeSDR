// test-web-dsc.mjs — V12 DSC IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (2026-10-26).
//   A fake rtl_tcp replays the synthetic clip (`dsc_bench clip`: a distress alert, then an all-ships safety call, on
//   2187.5 kHz); the server's dial is 2185.8 kHz USB. Headless Edge then:
//   • DECODERS shows DSC (vibeserver.json says "dsc": true); its settings hold the DSC channel keys;
//   • the calls arrive as rows, newest first: the distress alert in red with MAP, the safety call in amber;
//   • MAP opens this server's map window with the position;
//   • a channel key tunes as a DSC receiver does (8414.5 → 8412.8 kHz USB);
//   • the gallery has a DSC chip, and its distress item is highlighted with MAP;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
//   Screenshots (desktop and 390 px) are written to VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_DSC_CLIP=…/hf.cu8 node scripts/test-web-dsc.mjs   (scripts/test-dsc.sh clip hf.cu8)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_DSC_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_DSC_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webdsc-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'DSC web test', locator: 'IO92nh', adminPass: 'wdsc-admin',
  radios: [{ serial: 'wdsc', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', '2187500'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '2185800', '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webdsc-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.dsc === true, `vibeserver.json: dsc ${JSON.stringify(st.dsc)}`);

  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (x, ms) => { for (let t = 0; t < ms; t += 250) { if (await ev(x)) return true; await sleep(250); } return !!(await ev(x)); };
  const shot = async (name) => { const r = await send('Page.captureScreenshot', { format: 'png' }); const p = path.join(SHOTS, name); fs.writeFileSync(p, Buffer.from(r.result.data, 'base64')); return p; };
  const trustedClick = async (sel) => {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || {x:0,y:0,width:0,height:0})`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await trustedClick('#audioGate button');
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000),
     'START RADIO pressed (the splash is gone)');
  await sleep(2500);

  console.log('── DECODERS: the DSC button and its channels ──');
  ok(await until(`!document.querySelector('#decodersPanel [data-dec="dsc"]').hidden`, 8000), 'the DSC button is offered (this server has DSC)');
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     document.getElementById('decodersPanel').classList.add('open'); return false; })()`);
  await sleep(500);
  await ev(`document.querySelector('#decodersPanel [data-dec="dsc"]').click()`);
  ok(await until(`!document.getElementById('dscSettings').hidden && document.getElementById('dscList').classList.contains('on')`, 5000),
     'DSC chosen: its channel keys and the calls list are shown');
  const chans = await ev(`[...document.querySelectorAll('#dscChans button')].map(b => b.textContent).join(' ')`);
  ok(/2187\.5/.test(chans) && /8414\.5/.test(chans), `the channel keys: ${chans}`);
  const settings = await shot('dsc-settings-desktop.png');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── the calls ──');
  const both = `(() => { const r = [...document.querySelectorAll('#dscRows .dscr')];
     return r.some(x => x.classList.contains('distress') && /DISTRESS · MMSI 235012345 \\(United Kingdom\\) · Sinking/.test(x.textContent))
         && r.some(x => x.classList.contains('safety') && /SAFETY · all ships/.test(x.textContent)); })()`;
  ok(await until(both, 60000), `the distress alert (red) and the safety call (amber) are listed (${await ev(`[...document.querySelectorAll('#dscRows .dscr .l')].map(x => x.textContent.slice(0, 30)).join(' | ')`)})`);
  const col = await ev(`getComputedStyle(document.querySelector('#dscRows .dscr.distress .l')).color`);
  ok(col === 'rgb(255, 107, 107)', `the distress line is red (${col})`);
  ok(await ev(`!!document.querySelector('#dscRows .dscr.distress .dscMap')`) && !(await ev(`!!document.querySelector('#dscRows .dscr.safety .dscMap')`)),
     'MAP on the call that sent a position, none on the one that did not');
  ok(await ev(`/2187\\.5 kHz/.test(document.querySelector('#dscRows .dscr.distress .f').textContent)`), 'the row names 2187.5 kHz');
  const s1 = await shot('dsc-calls-desktop.png');

  console.log('── MAP opens this server\'s map with the position ──');
  const before = (await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).filter((t) => t.type === 'page').length;
  await trustedClick('#dscRows .dscr.distress .dscMap');   // ★ a trusted click: a scripted one is a blocked popup
  let mapPage = null;
  for (let i = 0; i < 40 && !mapPage; i++) {
    await sleep(250);
    const l = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json());
    if (l.filter((t) => t.type === 'page').length > before) mapPage = l.find((t) => t.type === 'page' && /DSC positions/.test(t.title || ''));
  }
  ok(!!mapPage, `a map window opened ("${mapPage ? mapPage.title : 'none'}")`);
  if (mapPage) {
    const mw = new WebSocket(mapPage.webSocketDebuggerUrl);
    await new Promise((r) => { mw.onopen = r; });
    let mid = 0; const mp = new Map();
    mw.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && mp.has(m.id)) { mp.get(m.id)(m); mp.delete(m.id); } };
    const mev = (x) => new Promise((res) => { const i = ++mid; mp.set(i, (m) => res(m.result?.result?.value)); mw.send(JSON.stringify({ id: i, method: 'Runtime.evaluate', params: { expression: x, returnByValue: true } })); });
    await sleep(1500);
    const note = await mev(`document.getElementById('note') ? document.getElementById('note').textContent : ''`);
    ok(/DISTRESS · MMSI 235012345/.test(note || ''), `the map window names the call (${(note || '').slice(0, 60)})`);
    const pins = await mev(`JSON.stringify(typeof PINS !== 'undefined' ? PINS.map(p => [p.lat, p.lon]) : [])`);
    ok(/50\.2/.test(pins || '') && /-4\.16/.test(pins || ''), `…with the position pinned (${pins})`);
    mw.close();
  }

  console.log('── a channel key tunes as a DSC receiver does ──');
  await ev(`(() => { document.getElementById('decodersPanel').classList.add('open'); [...document.querySelectorAll('#dscChans button')].find(b => b.textContent === '8414.5').click(); return 1; })()`);
  // ★ The card's readout (#mFreq) is the radio's dial as the page shows it.
  ok(await until(`/\\b8[.,]412(8|[.,]8)?/.test((document.getElementById('mFreq') || {}).textContent || '')`, 6000),
     `the radio is tuned to 8412.8 kHz USB — the dial 1.7 kHz below 8414.5 (readout "${await ev(`(document.getElementById('mFreq') || {}).textContent || ''`)}")`);
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    box: document.getElementById('decBox').getBoundingClientRect().right,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#dscRows .dscr')].map(r => r.getBoundingClientRect().right)),
    rows: document.querySelectorAll('#dscRows .dscr').length })`));
  ok(fit.doc <= 391 && fit.box <= 391 && fit.rowMax <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)}, rows end ${Math.round(fit.rowMax)} px)`);
  ok(fit.rows >= 2, `the calls are still there (${fit.rows} rows)`);
  const s2 = await shot('dsc-calls-phone.png');

  console.log('── the gallery ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await ev(`document.getElementById('galleryBtn').click()`);
  await sleep(800);
  const chips = await ev(`[...document.querySelectorAll('#galFilter [data-kind]')].map(b => b.textContent).join(' ')`);
  ok(/\bDSC\b/.test(chips), `the chips: ${chips}`);
  await ev(`document.querySelector('#galFilter [data-kind="dsc"]').click()`);
  ok(await until(`[...document.querySelectorAll('#galList .sres.galDistress')].some(r => /DISTRESS/.test(r.textContent) && r.querySelector('.galMap'))`, 8000),
     'the distress item is highlighted, with MAP');
  ok(await until(`[...document.querySelectorAll('#galList .sres')].some(r => /DSC 2187\\.5 kHz/.test(r.textContent))`, 4000), "and the day's log for 2187.5 kHz is listed");
  const s3 = await shot('dsc-gallery-desktop.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[settings, s1, s2, s3].join('  ')}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  setTimeout(() => { for (const d of [dir, prof]) try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }, 1500);
}
console.log(`\n${fail ? '✗' : '✓'} test-web-dsc: ${pass} passed, ${fail} failed`);
setTimeout(() => process.exit(fail ? 1 : 0), 1600);
