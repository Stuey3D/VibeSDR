// test-web-tones.mjs — V12 DTMF + 5-TONE IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (the binary's
// embedded page: node scripts/build-web.mjs, then rebuild the server). A fake rtl_tcp replays test-tones' synthetic NFM clip
// (DTMF "*73#1234", a ZVEI-1 call "12345", a CCIR call "40719") on 145.5875 MHz — 2 m, an amateur frequency. Headless Edge then:
//   • DECODERS opens on the VHF tab (the dial is on 2 m) and DTMF / 5-TONE is there under Amateur (vibeserver.json "tones");
//   • the key opens DECODE (DTMF, 5-TONE both lit) and 5-TONE STANDARD (AUTO lit, then the eight tables) and the hint;
//   • the box shows the DTMF string and both calls with their digits; the status line says digits are shown on 2 m;
//   • pinning CCIR re-attaches with the pin lit and only the CCIR call follows; DTMF off is never both off;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_TONES_CLIP=…/clip.cu8 node scripts/test-web-tones.mjs
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_TONES_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_TONES_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 288000, DIAL = 145587500, CENTRE = DIAL - 25000;   // ★ 2 m: the clip's channel 25 kHz above its centre

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webtones-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Tones web test', locator: 'JO02ae', adminPass: 'wtones-admin',
  radios: [{ serial: 'wtones', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'nfm', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webtones-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.tones === true, `vibeserver.json: tones ${JSON.stringify(st.tones)}`);

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

  console.log('── DECODERS: the VHF tab, DTMF / 5-TONE under Amateur ──');
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(500);
  const viaPicker = await ev(`(() => { const o = [...document.querySelectorAll('.mModeOpt')]; const d = o.find(x => /^DECODERS/.test(x.textContent.trim()));
                                       if (d) { d.click(); return 'panel'; } const s = o.find(x => /5-TONE/.test(x.textContent)); if (s) { s.click(); return 'sole'; } return ''; })()`);
  ok(!!viaPicker, `the demodulator button offers the decoders (${viaPicker})`);
  await sleep(800);
  const tabs = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#decBandTabs button')].map(b => ({ t: b.textContent, on: b.classList.contains('on') })))`) || '[]');
  ok(tabs.some((t) => t.t === 'VHF' && t.on), `the panel opens on the VHF tab (${tabs.map((t) => t.t + (t.on ? '*' : '')).join(' ')})`);
  const key = JSON.parse(await ev(`JSON.stringify((() => { const b = document.querySelector('#decodersPanel [data-dec="tones"]');
      const g = b && b.closest('.decGroup'); return { shown: !!b && !b.hidden && !!b.offsetParent, group: g ? g.querySelector('.decGroupHead').textContent : '' }; })())`));
  ok(key.shown && /amateur/i.test(key.group), `DTMF / 5-TONE shown, under "${key.group}"`);
  await ev(`document.querySelector('#decodersPanel [data-dec="tones"]').click()`);
  ok(await until(`!document.getElementById('tonesSettings').hidden && document.querySelectorAll('#tonesStd button').length === 9`, 4000),
     'chosen: its options under the key (AUTO and eight standards)');
  ok(await ev(`[...document.querySelectorAll('#tonesWhich .on')].map(b => b.textContent).join() === 'DTMF,5-TONE' && document.querySelector('#tonesStd .on')?.textContent === 'AUTO'`),
     'DTMF and 5-TONE lit, the standard on AUTO');
  ok(await ev(`/amateur bands/.test(document.getElementById('tonesHint').textContent)`), 'the hint says where digits are shown');
  ok(await until(`document.getElementById('decTitle').textContent === 'DTMF / 5-TONE'`, 3000), `the box is titled (${await ev(`document.getElementById('decTitle').textContent`)})`);
  const settings = await shot('tones-settings-desktop.png');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── the lines ──');
  const all = `(() => { const t = document.getElementById('decText').textContent; return /DTMF  \\*73#1234/.test(t) && /ZVEI-1 5-tone  12345/.test(t) && /CCIR 5-tone  40719/.test(t); })()`;
  ok(await until(all, 30000), `the DTMF string and both calls with their digits (${(await ev(`document.getElementById('decText').textContent`) || '').slice(-200)})`);
  ok(await until(`/2 m amateur: digits shown/.test(document.getElementById('decStatus').textContent)`, 5000), `the status line: "${await ev(`document.getElementById('decStatus').textContent`)}"`);
  const s2 = await shot('tones-lines-desktop.png');

  console.log('── the manual controls ──');
  await ev(`document.getElementById('decodersPanel').classList.add('open')`);
  await sleep(300);
  await ev(`[...document.querySelectorAll('#tonesStd button')].find(b => b.textContent === 'CCIR').click()`);
  ok(await until(`document.querySelector('#tonesStd .on')?.textContent === 'CCIR'`, 2000), 'CCIR lit');
  await ev(`[...document.querySelectorAll('#tonesWhich button')].find(b => b.textContent === 'DTMF').click()`);
  ok(await until(`[...document.querySelectorAll('#tonesWhich .on')].map(b => b.textContent).join() === '5-TONE'`, 2000), 'DTMF off');
  await ev(`[...document.querySelectorAll('#tonesWhich button')].find(b => b.textContent === '5-TONE').click()`);
  ok(await until(`[...document.querySelectorAll('#tonesWhich .on')].map(b => b.textContent).join() === 'DTMF'`, 2000), 'switching the last one off switches the other on (never both off)');
  await ev(`[...document.querySelectorAll('#tonesWhich button')].find(b => b.textContent === '5-TONE').click()`);
  await ev(`[...document.querySelectorAll('#tonesWhich button')].find(b => b.textContent === 'DTMF').click()`);
  ok(await until(`[...document.querySelectorAll('#tonesWhich .on')].map(b => b.textContent).join() === '5-TONE'`, 2000), '5-TONE only again');
  await ev(`document.getElementById('decText').textContent = ''`);
  ok(await until(`/CCIR 5-tone  40719/.test(document.getElementById('decText').textContent)`, 20000), 'pinned to CCIR: the CCIR call');
  await sleep(7000);
  const after = await ev(`document.getElementById('decText').textContent`);
  ok(!/ZVEI-1/.test(after) && !/DTMF/.test(after), `…and neither the ZVEI-1 call nor DTMF (${after.trim().split('\n').length} line(s))`);
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(2500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('decBox').getBoundingClientRect().right })`));
  ok(fit.doc <= 391 && fit.box <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)} px)`);
  await ev(`document.getElementById('decodersPanel').classList.add('open')`);
  await sleep(500);
  const fitOpts = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, std: Math.max(0, ...[...document.querySelectorAll('#tonesStd button')].map(b => b.getBoundingClientRect().right)) })`));
  ok(fitOpts.doc <= 391 && fitOpts.std <= 391, `the options fit too (standards end ${Math.round(fitOpts.std)} px)`);
  const s3 = await shot('tones-phone.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[settings, s2, s3].join('  ')}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
}
console.log(`\n${fail ? '✗' : '✓'} test-web-tones: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
