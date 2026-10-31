// test-web-textmodes.mjs — V12 MFSK / OLIVIA / THOR IN THE WEB CLIENT, through a real vibeserver and the REAL built web
//   page (2026-10-31). A fake rtl_tcp replays the synthetic clip (tools/textmode-bench/tm_clip: OLIVIA 16/500 at 1500 Hz
//   and MFSK16 at 800 Hz, USB audio, looping every 20 s); the server's dial is 14071.5 kHz USB. Headless Edge then:
//   • DECODERS shows MFSK, OLIVIA and THOR in the HF AMATEUR group (vibeserver.json says so), with their icons;
//   • OLIVIA: the MODE row is the server's list (AUTO first); AUTO names "OLIVIA 16/500" and the text arrives;
//   • MFSK: AUTO names "MFSK16" and its text arrives; at 390 px nothing scrolls sideways; no exception.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_TM_CLIP=…/tm.cu8 node scripts/test-web-textmodes.mjs   (tools/textmode-bench/tm_clip tm.cu8)
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_TM_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_TM_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webtm-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Text modes web test', locator: 'IO92nh', adminPass: 'wtm-admin',
  radios: [{ serial: 'wtm', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', '14075000'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '14071500', '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webtm-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.mfsk === true && st.olivia === true && st.thor === true && Array.isArray(st.oliviaModes) && st.oliviaModes[0]?.id === 'auto',
     `vibeserver.json: mfsk ${st.mfsk}, olivia ${st.olivia} (${(st.oliviaModes || []).length} modes), thor ${st.thor}`);

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

  console.log('── DECODERS: the MFSK, OLIVIA and THOR buttons ──');
  ok(await until(`['mfsk','olivia','thor'].every(d => !document.querySelector('#decodersPanel [data-dec="' + d + '"]').hidden)`, 8000), 'the three keys are offered');
  await ev(`(() => { const b = [...document.querySelectorAll('button, [role=button], .mMode, #mMode')].find(x => /^USB$/.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await sleep(500);
  const openPanel = () => ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS…$/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; } return false; })()`);
  ok(!!(await openPanel()), 'DECODERS… opened from the mode picker');
  await sleep(800);
  const place = await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="olivia"]'); const g = b && b.closest('.decGroup');
     const h = g && g.querySelector('.decGroupHead');
     return JSON.stringify({ group: h ? h.textContent.trim().toUpperCase() : '', icon: !!(b && b.querySelector('svg')),
                             with: g ? [...g.querySelectorAll('[data-dec]')].map(x => x.dataset.dec).join(',') : '' }); })()`);
  const pl = JSON.parse(place || '{}');
  ok(/AMATEUR/.test(pl.group) && /mfsk,olivia,thor/.test(pl.with) && pl.icon, `in the HF Amateur group, with icons (${place})`);
  const s0 = await shot('tm-decoders.png');
  await ev(`document.querySelector('#decodersPanel [data-dec="olivia"]').click()`);
  ok(await until(`document.getElementById('decBox').classList.contains('open') && /OLIVIA/.test(document.getElementById('decTitle').textContent)`, 5000), 'the box opens, titled OLIVIA');
  const row = await ev(`[...document.querySelectorAll('#tmModes .btn')].map(b => b.textContent + (b.classList.contains('on') ? '*' : '')).join('|')`);
  ok(/^AUTO\*\|OLIVIA 4\/125/.test(row || '') && /CONTESTIA 32\/1000/.test(row || ''), `the MODE row is the server's list, AUTO lit (${(row || '').slice(0, 80)}…)`);
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  console.log('── OLIVIA ──');
  const textHas = (re) => `(${re}).test(document.getElementById('decText').textContent)`;
  // ★ the clip loops every 20 s, so a pass can be joined mid-block: the end of the over is the check
  ok(await until(textHas(/DE G4XYZ OLIVIA K/), 60000), 'the OLIVIA text arrives');
  const stO = await ev(`document.getElementById('decStatus').textContent`);
  ok(/OLIVIA 16\/500/.test(stO) || /OLIVIA 16\/500/.test(await ev(`document.getElementById('decText').textContent`)), `AUTO names OLIVIA 16/500 ("${stO}")`);
  const s1 = await shot('tm-olivia.png');
  console.log('── MFSK ──');
  await openPanel(); await sleep(600);   // (the key is clicked directly: the panel need not be in view)
  await ev(`document.querySelector('#decodersPanel [data-dec="mfsk"]').click()`);
  ok(await until(`/MFSK/.test(document.getElementById('decTitle').textContent)`, 5000), 'the box switches to MFSK');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  ok(await until(textHas(/MFSK TEST PSE K/), 60000), 'the MFSK16 text arrives');
  const t = await ev(`document.getElementById('decText').textContent`);
  const after = t.slice(t.lastIndexOf('[MFSK16'));
  ok(/^\[MFSK16 · \d+ Hz\]/.test(after) && !/OLIVIA/.test(after), 'AUTO names MFSK16; no OLIVIA text after it');
  const s1b = await shot('tm-mfsk.png');

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('decBox').getBoundingClientRect().right })`));
  ok(fit.doc <= 391 && fit.box <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)} px)`);
  const s2 = await shot('tm-phone.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[s0, s1, s1b, s2].join('  ')}`);
  if (fail) { console.log('   text: ' + JSON.stringify(t).slice(0, 600)); }
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  setTimeout(() => { for (const d of [dir, prof]) try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }, 1500);
}
console.log(`\n${fail ? '✗' : '✓'} test-web-textmodes: ${pass} passed, ${fail} failed`);
setTimeout(() => process.exit(fail ? 1 : 0), 1600);
