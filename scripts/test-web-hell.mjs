// test-web-hell.mjs — V12 FELD HELL IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (2026-10-31).
//   A fake rtl_tcp replays the synthetic clip (tools/hell-bench/hell_clip: one Feld Hell over at 1000 Hz, USB audio,
//   looping every 20 s); the server's dial is 14063 kHz USB. Headless Edge then:
//   • DECODERS shows HELL in the HF AMATEUR group (vibeserver.json says "hell": true), with its icon;
//   • the box opens as a PICTURE titled FELD HELL; the status line finds the tone ("drawing · 1000 Hz");
//   • the strips arrive: 210 wide, a whole number of 28-row strips, ink on white paper, and in every strip the lower
//     copy is the upper copy again (the column printed twice — so each line landed at its own place, re-sends included);
//   • a second strip is drawn under the first; CLR puts the page under PREV and starts a new one; SAVE is offered;
//   • at 390 px nothing scrolls sideways; no exception.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_HELL_CLIP=…/hell.cu8 node scripts/test-web-hell.mjs   (tools/hell-bench/hell_clip hell.cu8)
// ★ The page under test is the one compiled into the server: run scripts/build-web.mjs and rebuild vibeserver first.
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_HELL_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_HELL_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000, DIAL = 14063000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webhell-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Feld Hell web test', locator: 'IO92nh', adminPass: 'whl-admin',
  radios: [{ serial: 'whl', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(DIAL + 3500)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webhell-edge-'));
let edge = null, keep = false;
// ★ A hard deadline: whatever happens, nothing is left running.
const deadline = setTimeout(() => { console.log('   FAIL deadline (180 s)'); try { edge?.kill('SIGKILL'); } catch {} srv.kill('SIGKILL'); rtl.kill('SIGKILL'); process.exit(1); }, 180000);
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.hell === true && Array.isArray(st.modes) && st.modes.includes('hell'), `vibeserver.json: hell ${st.hell}, in modes ${Array.isArray(st.modes) && st.modes.includes('hell')}`);

  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  // ★ the server's own Hell lines as they arrived (0x01 frames, 9 + 210 bytes) — so a bad page says whose fault it is
  const wire = { lines: 0, black: 0, last: [] };
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Network.webSocketFrameReceived' && m.params.response.opcode === 2) {
                            const b = Buffer.from(m.params.response.payloadData, 'base64');
                            if (b[0] === 1 && b.length === 9 + 210) { wire.lines++; let z = 0; for (let i = 9; i < b.length; i++) if (b[i] < 96) z++;
                              if (z === 210) wire.black++; wire.last.push(z); if (wire.last.length > 56) wire.last.shift(); } }
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
  await send('Page.enable'); await send('Runtime.enable'); await send('Network.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await trustedClick('#audioGate button');
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000),
     'START RADIO pressed (the splash is gone)');
  await sleep(2500);

  console.log('── DECODERS: the HELL key ──');
  ok(await until(`!document.querySelector('#decodersPanel [data-dec="hell"]').hidden`, 8000), 'the key is offered');
  // ★ the mode picker (the USB key) holds the DECODERS… door
  await ev(`(() => { const b = [...document.querySelectorAll('button, [role=button], .mMode, #mMode')].find(x => /^USB$/.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await sleep(500);
  const openPanel = () => ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS…$/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; } return false; })()`);
  ok(!!(await openPanel()), 'DECODERS… opened from the mode picker');
  await sleep(800);
  const place = await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="hell"]'); const g = b && b.closest('.decGroup');
     const h = g && g.querySelector('.decGroupHead');
     return JSON.stringify({ group: h ? h.textContent.trim().toUpperCase() : '', icon: !!(b && b.querySelector('svg')),
                             with: g ? [...g.querySelectorAll('[data-dec]')].map(x => x.dataset.dec).join(',') : '' }); })()`);
  const pl = JSON.parse(place || '{}');
  ok(/AMATEUR/.test(pl.group) && /thor,hell/.test(pl.with) && pl.icon, `in the HF Amateur group beside THOR, with its icon (${place})`);
  const s0 = await shot('hell-decoders.png');
  await ev(`document.querySelector('#decodersPanel [data-dec="hell"]').click()`);
  ok(await until(`document.getElementById('decBox').classList.contains('open') && document.getElementById('decTitle').textContent === 'FELD HELL'`, 5000), 'the box opens, titled FELD HELL');
  ok(await ev(`document.getElementById('decImage').classList.contains('on') && document.getElementById('decText').classList.contains('off')`), 'it is a picture box (no text pane)');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── the strips ──');
  ok(await until(`/^drawing · (9[5-9]\\d|10[0-4]\\d) Hz$/.test(document.getElementById('decStatus').textContent)`, 45000),
     `the status line finds the tone ("${await ev(`document.getElementById('decStatus').textContent`)}")`);
  // ★ the page is read off the VISIBLE canvas: what the listener sees
  const readPage = `(() => { const c = document.getElementById('decImage'); const x = c.getContext('2d'); const w = c.width, h = c.height;
     if (!w || !h) return JSON.stringify({ w, h });
     const d = x.getImageData(0, 0, w, h).data; let ink = 0, paper = 0, mism = 0, inked = 0, clear = 0;
     for (let i = 0; i < w * h; i++) { const v = d[i * 4]; if (d[i * 4 + 3] === 0) clear++; if (v < 96) ink++; else if (v > 200) paper++; }
     for (let s = 0; s + 56 <= h; s += 28) for (let r = 0; r < 14; r++) for (let k = 0; k < w; k++) {
       const a = d[((s + r) * w + k) * 4], b = d[((s + r + 14) * w + k) * 4]; if (a !== b) mism++; if (a < 96) inked++; }
     return JSON.stringify({ w, h, ink: ink / (w * h), paper: paper / (w * h), mism, inked, clear }); })()`;
  ok(await until(`(() => { const c = document.getElementById('decImage'); return c.width === 210 && c.height >= 56; })()`, 45000), 'a second strip is drawn under the first');
  let pg = JSON.parse(await ev(readPage) || '{}');
  /* ★ ONE read of 20 (2026-10-31) came back with every pixel transparent — the page's own white paper included, so not
   *  the server's lines (they were checked on the wire: none all ink). A page the listener sees is never transparent
   *  (it is filled white before a line is drawn), so a wholly transparent read is the headless readback, read again
   *  once and SAID; anything else fails as it is. */
  if (pg.clear === pg.w * pg.h && pg.w) { console.log('   (note: the canvas read back wholly transparent — read again)'); await sleep(400); pg = JSON.parse(await ev(readPage) || '{}'); }
  ok(!pg.clear, `the page is opaque (${pg.clear} transparent pixels)`);
  ok(pg.w === 210 && pg.h % 28 === 0, `210 wide, a whole number of 28-row strips (${pg.w} × ${pg.h})`);
  ok(pg.ink > 0.04 && pg.ink < 0.45 && pg.ink + pg.paper > 0.9, `ink on white paper (ink ${(pg.ink * 100).toFixed(1)} %, paper ${(pg.paper * 100).toFixed(1)} %)`);
  // ★ finished strips only: the strip being drawn can be read between its rows' re-send
  ok(pg.mism === 0 && pg.inked > 200, `every finished strip's lower copy is its upper copy again (${pg.mism} pixels differ, ${pg.inked} inked)`);
  ok(await ev(`document.getElementById('decSave').style.display !== 'none'`), 'SAVE is offered');
  const s1 = await shot('hell-strips.png');

  console.log('── CLR ──');
  await ev(`document.getElementById('decClr').click()`);
  ok(await until(`document.getElementById('decPrev').style.display !== 'none' && document.getElementById('decPrev').textContent === 'PREV'`, 3000), 'CLR puts the page under PREV');
  ok(await until(`(() => { const c = document.getElementById('decImage'); return c.width === 210 && c.height === 28; })()`, 3000), 'and starts a new page, one strip tall');
  await ev(`document.getElementById('decPrev').click()`);
  const prevH = await ev(`document.getElementById('decImage').height`);
  ok(prevH >= 56 && await ev(`document.getElementById('decPrev').textContent === 'LIVE'`), `PREV shows the old page (${prevH} rows)`);
  await ev(`document.getElementById('decPrev').click()`);

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('decBox').getBoundingClientRect().right })`));
  ok(fit.doc <= 391 && fit.box <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)} px)`);
  const s2 = await shot('hell-phone.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[s0, s1, s2].join('  ')}`);
  console.log(`   wire: ${wire.lines} Hell lines from the server, ${wire.black} all ink`);
  if (fail) { console.log('   page: ' + JSON.stringify(pg) + '  server log: ' + logPath + '  last lines\' ink: ' + wire.last.join(',')); keep = true; }
} finally {
  clearTimeout(deadline);
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  setTimeout(() => { for (const d of keep ? [prof] : [dir, prof]) try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }, 1500);
}
console.log(`\n${fail ? '✗' : '✓'} test-web-hell: ${pass} passed, ${fail} failed`);
setTimeout(() => process.exit(fail ? 1 : 0), 1600);
