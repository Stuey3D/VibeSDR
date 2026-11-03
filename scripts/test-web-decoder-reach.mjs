// test-web-decoder-reach.mjs — V12 (2026-11-03) DECODER REACH in the web client (vibe_decoder_reach.h), in headless Edge
// with --mute-audio (tests are silent): a radio LOCKED to 90.8–93.2 MHz (the owner said SAME on). The DECODERS panel still
// draws SAME, NAVTEX, DSC, APRS, SELCAL, CW, PSK and JTTY — greyed (class unreach, aria-disabled, the reason as the
// title); a click shows the reason and attaches nothing; a decoder that works anywhere (RTTY) is untouched. Screenshots.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-web-decoder-reach.mjs
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
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const FIXED = ['same', 'dsc', 'aprs', 'navtex', 'selcal', 'cw', 'psk', 'jtty'];

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-reach-web-'));
for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Reach web test', locator: 'IO92nh', adminPass: 'reach-web-admin',
  radios: [{ serial: 'reach-web', label: 'Test radio', enabled: true, configured: true, users: 4, blockedModes: '+same' }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srvLog = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '92000000', '--lock-freq', '92000000',
                        '--mode', 'wfm', '--users', '4', '--idle-grace', '0'], { env, stdio: ['ignore', fs.openSync(srvLog, 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
let wsC;
const shots = [];
try {
  await sleep(3000);
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-reach-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); shots.push(f); return f; };
  const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  {   // ★ START RADIO is a real press (the browser's audio gesture) — the audio is muted (--mute-audio)
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
  await sleep(3000);
  // ★ the panel, through the mode menu's DECODERS… door (the way a listener opens it)
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(700);
  await ev(`(() => { const b = [...document.querySelectorAll('.mModeOpt')].find(x => /^DECODERS/.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  ok(await until(`document.getElementById('decodersPanel')?.classList.contains('open')`, 4000), 'the DECODERS panel opens');
  for (const d of FIXED) {
    const r = JSON.parse(await ev(`JSON.stringify((() => { const b = document.querySelector('#decodersPanel [data-dec="${d}"]'); return b ? { hidden: b.hidden, un: b.classList.contains('unreach'), dis: b.getAttribute('aria-disabled'), title: b.title } : null; })())`) || 'null');
    ok(!!r && !r.hidden && r.un && r.dis === 'true' && /^Outside this radio's locked range \(90\.8–93\.2 MHz\)/.test(r.title), `${d}: drawn, greyed, "${r && r.title}"`);
  }
  const rt = JSON.parse(await ev(`JSON.stringify((() => { const b = document.querySelector('#decodersPanel [data-dec="rtty"]'); return b ? { hidden: b.hidden, un: b.classList.contains('unreach') } : null; })())`) || 'null');
  ok(!!rt && !rt.hidden && !rt.un, 'RTTY (works on whatever is tuned): untouched');
  // ★ every tab of the panel, so a screenshot shows the greyed keys wherever they sit
  const tabs = await ev(`[...document.querySelectorAll('#decodersPanel [data-band-tab], #decodersPanel .decTab, #decodersPanel [role="tab"]')].map(t => t.textContent.trim()).join(',')`);
  console.log('   tabs:', tabs || '(none)');
  console.log('   screenshot', await shot('panel'));
  for (const t of String(tabs || '').split(',').filter(Boolean)) {
    await ev(`(() => { const b = [...document.querySelectorAll('#decodersPanel [data-band-tab], #decodersPanel .decTab, #decodersPanel [role="tab"]')].find(x => x.textContent.trim() === ${JSON.stringify(t)}); b && b.click(); })()`);
    await sleep(400);
    console.log('   screenshot', await shot('panel-' + t.toLowerCase().replace(/[^a-z0-9]+/g, '')));
  }
  // ★ a press: the reason, nothing attached
  const before = fs.readFileSync(srvLog, 'utf8').length;
  await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="same"]'); b && b.scrollIntoView(); b && b.click(); })()`);
  ok(await until(`/SAME unavailable — Outside this radio's locked range/.test(document.getElementById('vsPill')?.textContent || '')`, 3000),
     `a click says why: "${await ev(`document.getElementById('vsPill')?.textContent`)}"`);
  ok(await ev(`!document.querySelector('#decodersPanel [data-dec="same"]').classList.contains('on')`), 'and the key is not lit');
  await sleep(1200);
  const after = fs.readFileSync(srvLog, 'utf8').slice(before);
  ok(!/decoder same/i.test(after) && !/attach.*same/i.test(after), 'and nothing reached the server (no attach)');
  console.log('   screenshot', await shot('pill'));
  // ★ the keyboard reaches it: focus, then Enter
  // ★ NAVTEX sits on the HF tab — back to it, as a listener would
  await ev(`(() => { const b = [...document.querySelectorAll('#decodersPanel [data-band-tab], #decodersPanel .decTab, #decodersPanel [role="tab"]')].find(x => x.textContent.trim() === 'HF'); b && b.click(); })()`);
  await sleep(300);
  await ev(`document.querySelector('#decodersPanel [data-dec="navtex"]').focus()`);
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, text: '\r' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
  const focused = await ev(`document.activeElement === document.querySelector('#decodersPanel [data-dec="navtex"]')`);
  ok(focused === true, 'the greyed NAVTEX key takes focus (keyboard and pad reach it — it is not disabled)');
  if (!(await until(`/NAVTEX unavailable/.test(document.getElementById('vsPill')?.textContent || '')`, 1500))) {
    // ★ headless Edge does not always turn a CDP Enter into a click: the pad's own press is el.click() (gamepad.ts)
    await ev(`document.activeElement.click()`);
  }
  ok(await until(`/NAVTEX unavailable/.test(document.getElementById('vsPill')?.textContent || '')`, 3000), 'pressing the focused NAVTEX key says why too');
  // ★ a phone
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(800);
  console.log('   screenshot', await shot('phone'));
} catch (e) { ok(false, `driver: ${e}`); }
finally {
  try { wsC?.close(); } catch {}
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`\n${fail ? 'FAIL' : 'ok'}  ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
