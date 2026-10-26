// test-web-jtty.mjs — V12 JTTY END TO END: a real vibeserver and the REAL built web page (2026-10-26).
//   A fake rtl_tcp replays the synthetic clip (`jtty_bench clip`: "CQ G4ABC CQ" at 14.0910 MHz, then "DL1XYZ 599 042"
//   at 14.0915 MHz, both JTTY, overlapping); the server's dial is 14.090 MHz USB. Headless Edge then:
//   • DECODERS shows JTTY (vibeserver.json says "jtty": true); its settings hold the band keys this radio reaches;
//   • both stations arrive as rows, LOWEST FREQUENCY FIRST, each with its call and message, coloured by confidence;
//   • the SPOTS list G4ABC calling CQ and DL1XYZ in an exchange;
//   • a tap on a row tunes there (USB, the dial 1500 Hz below the station); a band key tunes its calling frequency;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
//   Screenshots (desktop and 390 px) are written to VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_JTTY_CLIP=…/clip.cu8 node scripts/test-web-jtty.mjs   (scripts/test-jtty.sh clip clip.cu8)
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_JTTY_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_JTTY_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webjtty-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'JTTY web test', locator: 'IO92nh', adminPass: 'wjtty-admin',
  radios: [{ serial: 'wjtty', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', '14091500'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '14090000', '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webjtty-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.jtty === true, `vibeserver.json: jtty ${JSON.stringify(st.jtty)}`);

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

  console.log('── DECODERS: the JTTY button and its bands ──');
  ok(await until(`!document.querySelector('#decodersPanel [data-dec="jtty"]').hidden`, 8000), 'the JTTY button is offered (this server has JTTY)');
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     document.getElementById('decodersPanel').classList.add('open'); return false; })()`);
  await sleep(500);
  await ev(`document.querySelector('#decodersPanel [data-dec="jtty"]').click()`);
  ok(await until(`!document.getElementById('jttySettings').hidden && document.getElementById('jttyList').classList.contains('on')`, 5000),
     'JTTY chosen: its band keys and the stations list are shown');
  const dials = await ev(`[...document.querySelectorAll('#jttyDials button')].map(b => b.textContent).join(' ')`);
  ok(/\b20m\b/.test(dials) && /\b40m\b/.test(dials), `the band keys: ${dials}`);
  ok(await ev(`document.getElementById('decTitle').textContent`) === 'JTTY', 'the box is titled JTTY');
  const settings = await shot('jtty-settings-desktop.png');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── the stations ──');
  const both = `(() => { const r = [...document.querySelectorAll('#jttyRows .jtr')];
     return r.some(x => /G4ABC/.test(x.querySelector('.c').textContent) && /CQ G4ABC CQ/.test(x.querySelector('.tx').textContent))
         && r.some(x => /DL1XYZ 599 042/.test(x.querySelector('.tx').textContent)); })()`;
  ok(await until(both, 60000), `both stations are listed with their messages (${await ev(`[...document.querySelectorAll('#jttyRows .jtr')].map(x => x.textContent.slice(0, 60)).join(' | ')`)})`);
  const order = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#jttyRows .jtr .f')].map(x => parseFloat(x.textContent)))`));
  ok(order.length >= 2 && order.every((v, i) => !i || order[i - 1] <= v), `lowest frequency first (${order.join(', ')})`);
  ok(/14091\.0/.test(String(order[0])) || order[0] === 14091, `G4ABC's row reads 14091.00 kHz (${order[0]})`);
  const col = await ev(`getComputedStyle(document.querySelector('#jttyRows .jtr .tx')).color`);
  ok(['rgb(91, 227, 107)', 'rgb(232, 197, 71)', 'rgb(255, 138, 61)', 'rgb(255, 75, 75)'].includes(col), `the message is coloured by its confidence (${col})`);
  ok(await until(`[...document.querySelectorAll('#jttySpots .jts')].some(r => /G4ABC/.test(r.textContent) && r.classList.contains('cq'))
              && [...document.querySelectorAll('#jttySpots .jts')].some(r => /DL1XYZ/.test(r.textContent) && /QSO/.test(r.textContent))`, 10000),
     `SPOTS: G4ABC calling CQ, DL1XYZ in an exchange (${await ev(`[...document.querySelectorAll('#jttySpots .jts')].map(x => x.textContent).join(' | ')`)})`);
  const s1 = await shot('jtty-stations-desktop.png');

  console.log('── a tap tunes there; a band key tunes its calling frequency ──');
  await ev(`[...document.querySelectorAll('#jttyRows .jtr')].find(x => /G4ABC/.test(x.textContent)).click()`);
  ok(await until(`/14[.,]089/.test((document.getElementById('mFreq') || {}).textContent || '')`, 6000),
     `a tap on G4ABC's row tunes 14.0895 MHz USB (readout "${await ev(`(document.getElementById('mFreq') || {}).textContent || ''`)}")`);
  await ev(`(() => { document.getElementById('decodersPanel').classList.add('open'); [...document.querySelectorAll('#jttyDials button')].find(b => b.textContent === '20m').click(); return 1; })()`);
  ok(await until(`/14[.,]090/.test((document.getElementById('mFreq') || {}).textContent || '')`, 6000),
     `the 20m key tunes 14.090 MHz (readout "${await ev(`(document.getElementById('mFreq') || {}).textContent || ''`)}")`);
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    box: document.getElementById('decBox').getBoundingClientRect().right,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#jttyRows .jtr, #jttySpots .jts')].map(r => r.getBoundingClientRect().right)),
    rows: document.querySelectorAll('#jttyRows .jtr').length })`));
  ok(fit.doc <= 391 && fit.box <= 391 && fit.rowMax <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)}, rows end ${Math.round(fit.rowMax)} px)`);
  ok(fit.rows >= 2, `the stations are still there (${fit.rows} rows)`);
  const s2 = await shot('jtty-stations-phone.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[settings, s1, s2].join('  ')}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  setTimeout(() => { for (const d of [dir, prof]) try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }, 1500);
}
console.log(`\n${fail ? '✗' : '✓'} test-web-jtty: ${pass} passed, ${fail} failed`);
setTimeout(() => process.exit(fail ? 1 : 0), 1600);
