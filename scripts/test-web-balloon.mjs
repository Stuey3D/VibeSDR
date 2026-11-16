// test-web-balloon.mjs — V12 BALLOONS IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (the binary's
// embedded page: node scripts/build-web.mjs, then rebuild the server). A fake rtl_tcp replays test-balloon's synthetic clip
// (a payload on 434.200 MHz USB: UKHAS at 300 Bd, an SSDV picture at 600 Bd, Horus v2); the server's dial on it. Headless
// Edge then:
//   • DECODERS opens on the UHF tab (the dial is on 434 MHz) and BALLOONS is there under Amateur (vibeserver.json "balloon");
//   • BALLOONS opens its pins under the key, every row on AUTO, and the hint;
//   • the box lists VIBE1 and HORUS-256 with their positions and heights, and draws the SSDV picture (a decoded <img>,
//     96 × 64) once it completes; the status line names what RTTY and Horus found;
//   • pinning RTTY SPEED re-attaches with the pin lit;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_BALLOON_CLIP=…/clip.cu8 node scripts/test-web-balloon.mjs
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_BALLOON_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_BALLOON_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 288000, CENTRE = 434175000, DIAL = CENTRE + 25000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webballoon-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
// ★ V12 Horus payload names: never the network in a test — today's try counts as done (vibe_horus_names.h)
fs.mkdirSync(path.join(dir, 'data', 'horus'));
fs.writeFileSync(path.join(dir, 'data', 'horus', 'checked'), String(Math.floor(Date.now() / 1000) + 10 * 86400));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Balloon web test', locator: 'JO02ae', adminPass: 'wballoon-admin',
  radios: [{ serial: 'wballoon', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webballoon-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.balloon === true, `vibeserver.json: balloon ${JSON.stringify(st.balloon)}`);

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

  console.log('── DECODERS: the UHF tab, BALLOONS under Amateur ──');
  // ★ the way a listener gets there: the demodulator button, then DECODERS… (or, where BALLOONS is the only decoder left,
  //   the key itself standing in the picker)
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(500);
  const viaPicker = await ev(`(() => { const o = [...document.querySelectorAll('.mModeOpt')]; const d = o.find(x => /^DECODERS/.test(x.textContent.trim()));
                                       if (d) { d.click(); return 'panel'; } const s = o.find(x => /BALLOONS/.test(x.textContent)); if (s) { s.click(); return 'sole'; } return ''; })()`);
  ok(!!viaPicker, `the demodulator button offers the decoders (${viaPicker})`);
  await sleep(800);
  const tabs = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#decBandTabs button')].map(b => ({ t: b.textContent, on: b.classList.contains('on') })))`) || '[]');
  ok(tabs.some((t) => t.t === 'UHF' && t.on), `the panel opens on the UHF tab (${tabs.map((t) => t.t + (t.on ? '*' : '')).join(' ')})`);
  const key = JSON.parse(await ev(`JSON.stringify((() => { const b = document.querySelector('#decodersPanel [data-dec="balloon"]');
      const g = b && b.closest('.decGroup'); return { shown: !!b && !b.hidden && !!b.offsetParent, group: g ? g.querySelector('.decGroupHead').textContent : '' }; })())`));
  ok(key.shown && /amateur/i.test(key.group), `BALLOONS shown, under "${key.group}"`);
  await ev(`document.querySelector('#decodersPanel [data-dec="balloon"]').click()`);
  ok(await until(`!document.getElementById('balloonSettings').hidden && document.querySelectorAll('#balloonBaud button').length === 7`, 4000), 'BALLOONS chosen: its pins under the key (seven speeds)');
  ok(await ev(`['balloonBaud','balloonBits','balloonPol','balloonShift','balloonSpacing'].every(id => document.querySelector('#' + id + ' .on')?.textContent === 'AUTO')`),
     'every pin starts on AUTO');
  ok(await ev(`/USB/.test(document.getElementById('balloonHint').textContent) && /SSDV/.test(document.getElementById('balloonHint').textContent)`), 'the hint says how to tune it');
  ok(await ev(`[...document.querySelectorAll('#balloonPol button')].map(b => b.textContent).join() === 'AUTO,NORM,REV'`), 'POLARITY: AUTO, NORM, REV');
  ok(await until(`document.getElementById('decTitle').textContent === 'BALLOONS'`, 3000), `the box is titled BALLOONS (${await ev(`document.getElementById('decTitle').textContent`)})`);
  ok(await until(`document.getElementById('balloonList').classList.contains('on')`, 3000), 'the box shows the balloon list');
  const settings = await shot('balloon-settings-desktop.png');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── the payloads and the picture ──');
  const both = `(() => { const t = document.getElementById('balloonRows').textContent; return /VIBE1/.test(t) && /HORUS-256/.test(t); })()`;
  ok(await until(both, 75000), `VIBE1 and HORUS-256 listed (${(await ev(`document.getElementById('balloonRows').textContent`) || '').slice(0, 160)})`);
  const rows = await ev(`document.getElementById('balloonRows').textContent`);
  ok(/52\.2\d{4}, 0\.1\d{4}/.test(rows) && /31,\d{3} m/.test(rows), 'positions and heights drawn');
  ok(/42 km\/h · 11 sats · -31 °C · 2\.94 V/.test(rows), 'the Horus numbers (speed, satellites, temperature, battery)');
  ok(await until(`(() => { const i = document.querySelector('#balloonPics img'); return !!i && i.complete && i.naturalWidth === 96 && i.naturalHeight === 64
                           && /complete/.test(document.querySelector('#balloonPics figcaption').textContent); })()`, 60000),
     `the SSDV picture drawn, 96 × 64, complete (${await ev(`document.querySelector('#balloonPics figcaption')?.textContent || 'none'`)})`);
  ok(/RTTY 300 Bd · 8N1/.test(await ev(`document.getElementById('decStatus').textContent`) || '') || /Horus/.test(await ev(`document.getElementById('decStatus').textContent`) || ''),
     `the status line: "${await ev(`document.getElementById('decStatus').textContent`)}"`);
  const s2 = await shot('balloon-list-desktop.png');

  console.log('── a pin re-attaches ──');
  await ev(`(() => { const b = document.querySelector('[data-dec="balloon"]'); window.__reat = 0; return 1; })()`);
  await ev(`(() => { const p = document.getElementById('decodersPanel'); p.classList.add('open'); return 1; })()`);
  await sleep(300);
  await ev(`[...document.querySelectorAll('#balloonBaud button')].find(b => b.textContent === '300').click()`);
  ok(await until(`document.querySelector('#balloonBaud .on')?.textContent === '300'`, 2000), 'RTTY SPEED 300 lit');
  await sleep(1500);
  ok(await ev(both), 'the rows stay in the box across the re-attach (the server starts a fresh list)');
  ok(await until(both, 40000), 'still decoding after the re-attach');
  await ev(`[...document.querySelectorAll('#balloonBaud button')].find(b => b.textContent === 'AUTO').click()`);
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(2500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    box: document.getElementById('decBox').getBoundingClientRect().right,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#balloonRows .bln, #balloonPics img')].map(r => r.getBoundingClientRect().right)) })`));
  ok(fit.doc <= 391 && fit.box <= 391 && fit.rowMax <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)}, rows end ${Math.round(fit.rowMax)} px)`);
  const s3 = await shot('balloon-list-phone.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[settings, s2, s3].join('  ')}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
}
console.log(`\n${fail ? '✗' : '✓'} test-web-balloon: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
