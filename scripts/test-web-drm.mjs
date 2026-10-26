// test-web-drm.mjs — V12 DRM IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (2026-10-26).
//   A fake rtl_tcp replays REAL DRM (RRI 9570 kHz off Stuart's UberSDR, scripts/drm-clip.py); the dial is on it in AM.
//   Headless Edge then:
//   • the mode picker offers DRM (this server lists it); choosing it shows the DRM box;
//   • the box fills from the server's reports: the station's label, the six lights, robustness/QAM/rate, SNR, the codec —
//     and, for this AAC station, the server's sentence saying why there is no audio;
//   • the station bar (VTS) carries the label; the bandwidth row is hidden (it does nothing in DRM);
//   • back to AM: the box goes;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page. Screenshots to VIBE_SHOT_DIR.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_DRM_CLIP=…/rri9570_288k.cu8 node scripts/test-web-drm.mjs
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_DRM_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_DRM_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 288000, CENTRE = 9540000, STATION = 9570000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webdrm-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'DRM web test', locator: 'IO92nh', adminPass: 'wdrm-admin',
  radios: [{ serial: 'wdrm', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(STATION), '--mode', 'am', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webdrm-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(Array.isArray(st.modes) && st.modes.includes('drm'), 'vibeserver.json offers drm');

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
  const shot = async (name) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    const p = path.join(SHOTS, name);
    fs.writeFileSync(p, Buffer.from(r.result.data, 'base64'));
    return p;
  };
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
  await trustedClick('#audioGate button');   // ★ a trusted click starts audio (silent: --mute-audio)
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000),
     'START RADIO pressed (the splash is gone)');
  await sleep(2500);

  console.log('── the mode picker: DRM ──');
  await ev(`document.getElementById('mMode').click()`);
  await sleep(400);
  const opts = await ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].map(b => b.textContent).join(' ')`);
  ok(/\bDRM\b/.test(opts), `the picker offers DRM (${opts})`);
  await ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === 'DRM').click()`);
  ok(await until(`document.getElementById('drmBox')?.classList.contains('open')`, 4000), 'choosing DRM opens the DRM box');
  ok(await until(`/SNR Tiganesti E1/.test(document.querySelector('#drmBody .drmLabel')?.textContent || '')`, 60000),
     `the station's label in the box ("${await ev(`document.querySelector('#drmBody .drmLabel')?.textContent || ''`)}")`);
  ok(await until(`document.querySelectorAll('#drmBody .drmLights .drmLight.on').length === 6`, 30000),
     `all six lights (${await ev(`[...document.querySelectorAll('#drmBody .drmLights .drmLight')].map(l => l.textContent + (l.classList.contains('on') ? '+' : '-')).join(' ')`)})`);
  const sig = await ev(`document.querySelector('#drmBody .drmSigTxt')?.textContent || ''`);
  ok(/Robustness B/.test(sig) && /16-QAM/.test(sig) && /10 kHz/.test(sig), `the signal line reads: "${sig}"`);
  ok(await until(`/SNR \\d/.test(document.querySelector('#drmBody .drmSnr')?.textContent || '')`, 5000), `the SNR: ${await ev(`document.querySelector('#drmBody .drmSnr')?.textContent`)}`);
  const codec = await ev(`document.querySelector('#drmBody .drmCodec')?.textContent || ''`);
  ok(/AAC/.test(codec) && /12 kHz/.test(codec), `the codec line: "${codec.trim()}"`);
  const why = await ev(`document.querySelector('#drmBody .drmWhy')?.textContent || ''`);
  ok(/cannot be played/.test(why), `why there is no audio, in words: "${why.slice(0, 90)}…"`);
  ok(await until(`/SNR Tiganesti E1/.test(document.getElementById('vtsName')?.textContent || '')`, 5000), 'the station bar carries the label');
  ok(await ev(`(() => { const r = document.querySelector('.bwRow'); return !r || r.style.display === 'none'; })()`), 'the bandwidth row is hidden in DRM');
  const s1 = await shot('drm-desktop.png');

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('drmBox').getBoundingClientRect().right,
                                                    left: document.getElementById('drmBox').getBoundingClientRect().left })`));
  ok(fit.doc <= 391 && fit.box <= 391 && fit.left >= 0, `nothing scrolls sideways (page ${fit.doc}, box ${Math.round(fit.left)}–${Math.round(fit.box)} px)`);
  const s2 = await shot('drm-phone.png');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });

  console.log('── back to AM ──');
  await ev(`document.getElementById('mMode').click()`);
  await sleep(300);
  await ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === 'AM').click()`);
  ok(await until(`!document.getElementById('drmBox')?.classList.contains('open')`, 3000), 'AM: the DRM box is gone');
  ok(await ev(`(() => { const r = document.querySelector('.bwRow'); return !r || r.style.display !== 'none'; })()`), 'AM: the bandwidth row is back');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${s1}  ${s2}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(800);
  for (const d of [dir, prof]) { try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }   // ★ keep the Mac clean
}
console.log(`\n${fail ? '✗' : '✓'} test-web-drm: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
