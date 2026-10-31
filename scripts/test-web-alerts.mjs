// test-web-alerts.mjs — V12 EAS / SAME and SELCAL IN THE WEB CLIENT, through a real vibeserver and the REAL built web page
//   (2026-10-31). A fake rtl_tcp replays the synthetic clip (tools/alert-bench/alert_clip: one SAME header three times, a
//   SELCAL call BH-DM, NNNN three times — upper-sideband audio, looping every 20 s); the server's dial is 8891 kHz USB.
//   Headless Edge then:
//   • DECODERS shows SELCAL in the HF AVIATION group and SAME on the VHF tab beside ADV RDS (vibeserver.json says
//     "same" / "selcal": true), each with its icon and the shared hint as the status line's title;
//   • SAME: the alert line arrives once ("[SAME] Tornado Warning (TOR) …"), then the end of message;
//   • SELCAL: the call arrives as its code ("[SELCAL] BH-DM …");
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
//   Screenshots are written to VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_ALERT_CLIP=…/alerts.cu8 node scripts/test-web-alerts.mjs   (tools/alert-bench/alert_clip alerts.cu8)
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_ALERT_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_ALERT_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webalerts-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Alerts web test', locator: 'IO92nh', adminPass: 'walr-admin',
  radios: [{ serial: 'walr', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', '8894500'], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '8891000', '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webalerts-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.same === true && st.selcal === true, `vibeserver.json: same ${JSON.stringify(st.same)}, selcal ${JSON.stringify(st.selcal)}`);

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

  console.log('── DECODERS: the SELCAL and SAME buttons ──');
  ok(await until(`!document.querySelector('#decodersPanel [data-dec="selcal"]').hidden && !document.querySelector('#decodersPanel [data-dec="same"]').hidden`, 8000),
     'the SELCAL and SAME buttons are offered (this server has both)');
  await ev(`(() => { const b = [...document.querySelectorAll('button, [role=button], .mMode, #mMode')].find(x => /^USB$/.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await sleep(500);
  const viaMenu = await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS…$/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     return false; })()`);
  ok(!!viaMenu, 'DECODERS… opened from the mode picker');
  await sleep(800);
  const placeOf = (d) => ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="${d}"]'); const g = b && b.closest('.decGroup');
     const h = g && g.querySelector('.decGroupHead');
     return JSON.stringify({ group: h ? h.textContent.trim().toUpperCase() : '', icon: !!(b && b.querySelector('svg')), label: b ? b.textContent.trim() : '',
                             with: g ? [...g.querySelectorAll('[data-dec]')].map(x => x.dataset.dec).join(',') : '' }); })()`);
  // ★ the HF tab (the dial is 8891 kHz): SELCAL in Aviation (decoderBands.ts — HFDL is the app's, not on this page)
  const p1 = await placeOf('selcal'); const pl1 = JSON.parse(p1 || '{}');
  ok(/AVIATION/.test(pl1.group) && /selcal/.test(pl1.with) && pl1.icon && /SELCAL/.test(pl1.label), `SELCAL in the HF Aviation group, with its icon (${p1})`);
  const s0 = await shot('alerts-decoders-hf.png');
  // ★ the VHF tab: SAME beside ADV RDS
  await ev(`(() => { const t = [...document.querySelectorAll('#decBandTabs button')].find(x => /VHF/.test(x.textContent)); if (t) t.click(); return !!t; })()`);
  await sleep(500);
  const p2 = await placeOf('same'); const pl2 = JSON.parse(p2 || '{}');
  ok(/rds,same/.test(pl2.with) && pl2.icon && /SAME/.test(pl2.label), `SAME on the VHF tab beside ADV RDS, with its icon (${p2})`);
  await ev(`document.querySelector('#decodersPanel [data-dec="same"]').click()`);
  ok(await until(`document.getElementById('decBox').classList.contains('open') && /SAME/.test(document.getElementById('decTitle').textContent)`, 5000), 'the decoder box opens, titled SAME');
  ok(/NOAA Weather Radio/.test(await ev(`document.getElementById('decStatus').title`)), 'the status line carries the SAME hint');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);

  console.log('── SAME ──');
  const textHas = (re) => `(${re}).test(document.getElementById('decText').textContent)`;
  ok(await until(textHas(/\[SAME\] Tornado Warning \(TOR\) · National Weather Service · areas 024031 \(MD\), 024033 \(MD\)/), 45000), 'the alert line: event, originator, areas');
  ok(await until(textHas(/\[SAME\] end of message \(NNNN\)/), 30000), 'the end of message');
  await sleep(21000);   // ★ a whole further loop: the same header again is NOT shown twice (one alert per header per 10 min)
  const t1 = await ev(`document.getElementById('decText').textContent`);
  ok((t1.match(/Tornado Warning/g) || []).length === 1, 'the alert shown exactly once across two loops');
  ok(!/SELCAL/.test(t1), 'no SELCAL line in the SAME box');
  const s1 = await shot('alerts-same-desktop.png');

  console.log('── SELCAL ──');
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS…$/.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); })()`);
  await sleep(600);
  await ev(`document.querySelector('#decodersPanel [data-dec="selcal"]').click()`);
  ok(await until(`/SELCAL/.test(document.getElementById('decTitle').textContent)`, 5000), 'the box switches to SELCAL');
  ok(/aeronautical/.test(await ev(`document.getElementById('decStatus').title`)), 'the status line carries the SELCAL hint');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  ok(await until(textHas(/\[SELCAL\] BH-DM · pulses/), 45000), 'the call arrives as its code: "[SELCAL] BH-DM"');
  const t = await ev(`document.getElementById('decText').textContent`);
  ok(!/\[SELCAL\] (?!BH-DM)/.test(t), 'no other code read');
  const s1b = await shot('alerts-selcal-desktop.png');

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('decBox').getBoundingClientRect().right })`));
  ok(fit.doc <= 391 && fit.box <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)} px)`);
  const s2 = await shot('alerts-phone.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[s0, s1, s1b, s2].join('  ')}`);
  if (fail) { console.log('   text: ' + JSON.stringify(t).slice(0, 600)); }
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  setTimeout(() => { for (const d of [dir, prof]) try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }, 1500);
}
console.log(`\n${fail ? '✗' : '✓'} test-web-alerts: ${pass} passed, ${fail} failed`);
setTimeout(() => process.exit(fail ? 1 : 0), 1600);
