// test-web-ism.mjs — the web client's ISM mode against the REAL server (fake rtl_tcp replaying `test-ism --write-clip`),
// in headless Edge with --mute-audio (tests are silent): START, the demodulator menu's ISM entry, the device box with the
// readable lines, the readout naming the band, the arrows stepping BANDS (▶ → 868, clamped at the top), the zoom held,
// a typed frequency, EXIT ISM giving the dial back. Screenshots to $TMPDIR/vibesdr-ism-*.png.
//   VIBESERVER_BIN=…/vibeserver VIBE_ISM_CLIP=…/clip.u8 node scripts/test-web-ism.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_ISM_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_ISM_CLIP and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webism-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'ISM web test', locator: 'IO92nh', adminPass: 'ism-web-admin',
  radios: [{ serial: 'ism-web', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism', CLIP], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
let wsC;
try {
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-ism-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  // the landing page's START (the radio list), then the receiver's START RADIO gate
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  {   // ★ a TRUSTED click (the gate starts audio, which a script's .click() may not)
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000), 'START RADIO pressed');
  await sleep(2500);
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(700);
  const opts = await ev(`[...document.querySelectorAll('.mModeOpt')].map(b => b.textContent.trim()).join(',')`);
  ok(/\bISM\b/.test(opts || ''), `the demodulator menu offers ISM: ${opts}`);
  await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => x.textContent.trim() === 'ISM'); b && b.click(); return !!b; })()`);
  ok(await until(`!!document.getElementById('ismBox') && !document.getElementById('ismBox').hidden`, 6000), 'the ISM box opens');
  ok(await until(`document.querySelectorAll('#ismList .ismRow').length >= 4`, 20000), `four devices listed: ${await ev(`[...document.querySelectorAll('#ismList .ismTxt')].map(x => x.textContent).join(' | ')`)}`);
  const band = await ev(`document.getElementById('ismBandName')?.textContent`);
  ok(band === '433.92 MHz ISM', `the box names the band: ${band}`);
  const card = await ev(`(document.getElementById('mFreq')?.textContent || '') + ' / ' + (document.getElementById('mChan')?.textContent || '')`);
  ok(/433\.920/.test(card || '') && /ISM 433/.test(card || ''), `the readout shows the band: ${card}`);
  ok(await ev(`document.getElementById('mZoomIn')?.disabled === true`), 'zoom is held');
  console.log('   screenshot', await shot('433'));
  // ★ phone width too (no visible polish gaps): the box must fit with a 16 px gutter
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  const fit = JSON.parse(await ev(`JSON.stringify(document.getElementById('ismBox').getBoundingClientRect())`));
  ok(fit.left >= 15 && fit.right <= 390 - 15, `the box fits a phone: ${Math.round(fit.left)}–${Math.round(fit.right)} of 390`);
  const deckTop = await ev(`Math.min(...['mFreqBox','mVfoDown','mVfoUp','mZoomIn'].map(i => document.getElementById(i)?.getBoundingClientRect().top ?? 1e9))`);
  ok(fit.bottom <= deckTop, `…and clears the control deck: box bottom ${Math.round(fit.bottom)}, deck top ${Math.round(deckTop)}`);
  console.log('   screenshot', await shot('phone'));
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(800);
  await ev(`document.getElementById('ismUp').click()`);
  ok(await until(`document.getElementById('ismBandName')?.textContent === '868 MHz SRD'`, 6000), `▶ steps one band: ${await ev(`document.getElementById('ismBandName')?.textContent`)}`);
  ok(await ev(`document.querySelectorAll('#ismList .ismRow').length >= 4`), 'the list is kept across the step');
  // the arrows on the card step bands too (nudge)
  await ev(`(() => { const b = document.getElementById('mVfoUp'); if (!b) return 0; b.dispatchEvent(new PointerEvent('pointerdown', { button: 0, bubbles: true, pointerId: 1 })); window.dispatchEvent(new PointerEvent('pointerup', { bubbles: true, pointerId: 1 })); b.click(); return 1; })()`);
  ok(await until(`document.getElementById('ismBandName')?.textContent === '915 MHz ISM'`, 6000), `the card's ▶ steps a band: ${await ev(`document.getElementById('ismBandName')?.textContent`)}`);
  ok(await until(`document.getElementById('ismUp')?.disabled === true`, 3000), 'the box ▶ greys at the top band');
  // filter chips
  await ev(`document.querySelector('#ismChips [data-k="tpms"]').click()`);
  await sleep(300);
  const tp = await ev(`[...document.querySelectorAll('#ismList .ismTxt')].map(x => x.textContent).join(' | ')`);
  ok(/^TPMS #1/.test(tp || '') && !/Oregon/.test(tp || ''), `the TPMS filter: ${tp}`);
  await ev(`document.querySelector('#ismChips [data-u="pressure"]').click()`);
  await sleep(300);
  ok(/kPa/.test(await ev(`document.querySelector('#ismList .ismTxt')?.textContent`) || ''), 'pressure units flip to kPa');
  await ev(`document.querySelector('#ismChips [data-k="all"]').click(); document.querySelector('#ismChips [data-u="pressure"]').click(); document.querySelector('#ismChips [data-u="pressure"]').click();`);
  console.log('   screenshot', await shot('915'));
  await ev(`document.getElementById('ismExit').click()`);
  ok(await until(`document.getElementById('ismBox')?.hidden === true`, 6000), 'EXIT ISM closes the box');
  await sleep(1500);
  const after = await ev(`document.getElementById('mFreq')?.textContent`);
  ok(/96\.100/.test(after || ''), `the dial is back: ${after}`);
  ok(await ev(`document.getElementById('mZoomIn')?.disabled === false`), 'zoom is given back');
} catch (e) { ok(false, `driver: ${e}`); }
finally {
  try { wsC?.close(); } catch {}
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
