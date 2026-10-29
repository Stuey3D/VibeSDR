// test-web-sitora.mjs — V12 SITOR-A IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (2026-10-29).
//   A fake rtl_tcp replays the synthetic clip (tools/sitor-bench/sitor_clip: a whole SITOR-A link on 4209.5 kHz — the
//   call to 002320204 from 364775427, identification, traffic, a change-over each way, the end); the server's dial is
//   4208.5 kHz USB, so the tones sit at 1000 Hz. Headless Edge then:
//   • DECODERS shows SITOR-A in the HF MARITIME group (vibeserver.json says "sitora": true), with the ship icon;
//   • the text arrives, each passage under its station's label ("[2 · PEARDBY] …"), the call and identity lines in [ ];
//   • the status line names the link state, who is printing and the RQ count;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
//   Screenshots (desktop and 390 px) are written to VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_SITORA_CLIP=…/link.cu8 node scripts/test-web-sitora.mjs   (tools/sitor-bench/sitor_clip link.cu8)
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_SITORA_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_SITORA_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-websitora-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'SITOR-A web test', locator: 'IO92nh', adminPass: 'wsit-admin',
  radios: [{ serial: 'wsit', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', '4209500'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '4208500', '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-websitora-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.sitora === true, `vibeserver.json: sitora ${JSON.stringify(st.sitora)}`);

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

  console.log('── DECODERS: the SITOR-A button ──');
  ok(await until(`!document.querySelector('#decodersPanel [data-dec="sitora"]').hidden`, 8000), 'the SITOR-A button is offered (this server has SITOR-A)');
  // ★ opened as a listener opens it — the mode key, then DECODERS… — so the band tabs and groups are laid out
  await ev(`(() => { const b = [...document.querySelectorAll('button, [role=button], .mMode, #mMode')].find(x => /^USB$/.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await sleep(500);
  const viaMenu = await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS…$/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     return false; })()`);
  ok(!!viaMenu, 'DECODERS… opened from the mode picker');
  await sleep(800);
  // ★ the HF tab (the dial is 4208.5 kHz) — the group head of the box the key sits in (decoderBandsDom.ts)
  const place = await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="sitora"]'); const g = b && b.closest('.decGroup');
     const h = g && g.querySelector('.decGroupHead');
     return JSON.stringify({ group: h ? h.textContent.trim().toUpperCase() : '', icon: !!(b && b.querySelector('svg')), label: b ? b.textContent.trim() : '',
                             with: g ? [...g.querySelectorAll('[data-dec]')].map(x => x.dataset.dec).join(',') : '' }); })()`);
  const pl = JSON.parse(place || '{}');
  ok(/MARITIME/.test(pl.group) && /navtex,dsc,sitora/.test(pl.with) && pl.icon && /SITOR-A/.test(pl.label), `in the Maritime group beside NAVTEX and DSC, with its icon (${place})`);
  const s0 = await shot('sitora-decoders-desktop.png');
  await ev(`document.querySelector('#decodersPanel [data-dec="sitora"]').click()`);
  ok(await until(`document.getElementById('decBox').classList.contains('open') && /SITOR-A/.test(document.getElementById('decTitle').textContent)`, 5000), 'the decoder box opens, titled SITOR-A');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── the link ──');
  const textHas = (re) => `(${re}).test(document.getElementById('decText').textContent)`;
  // ★ the clip loops every 45 s: the page may join mid-link; the next pass is read from its call
  ok(await until(textHas(/\[call to VVOTVTM \(002320204\)\]/), 70000), 'the call is reported: "[call to VVOTVTM (002320204)]"');
  ok(await until(textHas(/\[caller PEARDBY \(364775427\)\]/), 20000), 'the caller\'s identification: "[caller PEARDBY (364775427)]"');
  ok(await until(textHas(/\[1 · PEARDBY\] QTH 43\.10N 009\.25E ETA LA SPEZIA/), 30000), 'the master\'s passage, under its label');
  const midStatus = await ev(`document.getElementById('decStatus').textContent`);
  ok(/TRAFFIC|IDLE|CHANGE-OVER/.test(midStatus) && /RQ \d+/.test(midStatus), `the status line: "${midStatus}"`);
  ok(/printing PEARDBY|printing VVOTVTM|printing station/.test(midStatus), '…naming who is printing');
  ok(await until(textHas(/\[2 · VVOTVTM\] BERTH 4 CONFIRMED, PILOT AT 0530Z\+\?/), 40000), 'the slave\'s reply after the change-over, under its own label');
  ok(await until(textHas(/\[1 · PEARDBY\] THANKS QRU\n\[end of communication\]/), 30000), 'the master\'s last line and the end of communication');
  const t = await ev(`document.getElementById('decText').textContent`);
  // ★ one whole pass, from its call: each passage exactly once (the blocks the IRS asked for again printed once)
  const pass2 = t.slice(t.lastIndexOf('[call to VVOTVTM'));
  const once = (pass2.match(/ETA LA SPEZIA 0600Z/g) || []).length === 1 && (pass2.match(/PILOT AT 0530Z/g) || []).length === 1;
  ok(once && /QTH 43\.10N 009\.25E ETA LA SPEZIA 0600Z\nPLEASE ADVISE BERTH \(MV TEST\)\+\?/.test(pass2), 'the whole master passage, exactly once (no duplicated text)');
  ok(/\[identity 002320204 confirmed by its check-sums\]/.test(pass2), 'the called station\'s identity confirmed by its check-sums');
  ok(!/\]\s*[^\n\[]*_{3,}/.test(pass2.split('[end of communication]')[0]), 'no run of unreadable characters within the link');
  const s1 = await shot('sitora-link-desktop.png');

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('decBox').getBoundingClientRect().right })`));
  ok(fit.doc <= 391 && fit.box <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)} px)`);
  const s2 = await shot('sitora-link-phone.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[s0, s1, s2].join('  ')}`);
  if (fail) { console.log('   text: ' + JSON.stringify(t).slice(0, 600)); }
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  setTimeout(() => { for (const d of [dir, prof]) try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }, 1500);
}
console.log(`\n${fail ? '✗' : '✓'} test-web-sitora: ${pass} passed, ${fail} failed`);
setTimeout(() => process.exit(fail ? 1 : 0), 1600);
