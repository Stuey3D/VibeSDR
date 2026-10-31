// test-web-textmodes.mjs — V12 THE MFSK KEY (MFSK, OLIVIA, CONTESTIA, THOR) IN THE WEB CLIENT, through a real vibeserver and the REAL built web
//   page (2026-10-31). A fake rtl_tcp replays the synthetic clip (tools/textmode-bench/tm_clip: OLIVIA 16/500 at 1500 Hz
//   and MFSK16 at 800 Hz, USB audio, looping every 20 s); the server's dial is 14071.5 kHz USB. Headless Edge then:
//   • ★ ONE key (2026-10-31): DECODERS shows MFSK — MFSK16/32, OLIVIA, CONTESTIA, THOR — in the HF AMATEUR group, no
//     OLIVIA or THOR keys; vibeserver.json's mfskModes grouped by family;
//   • first open: AUTO lit, FAMILY row = the server's families, no MODE row; AUTO prints only what was sent and names
//     only the two real signals; FAMILY → OLIVIA: its MODE row (ANY lit, 64/2000 listed), the OLIVIA over; → MFSK: MFSK16;
//   • AUTO first, manual for the session: re-opened → the manual family kept; after a reload → AUTO again;
//   • at 390 px nothing scrolls sideways; no exception.
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
  // ★★ V12 (2026-10-31): ONE key, "mfsk" — MFSK16/32, OLIVIA, CONTESTIA and THOR — its choices grouped by family
  const groups = [...new Set((st.mfskModes || []).map((m) => m.group).filter(Boolean))].join(',');
  ok(st.mfsk === true && st.olivia === undefined && st.thor === undefined && st.mfskModes?.[0]?.id === 'auto' && groups === 'MFSK,OLIVIA,CONTESTIA,THOR'
     && (st.mfskModes || []).some((m) => m.id === 'olivia-64-2000'),
     `vibeserver.json: one key (mfsk ${st.mfsk}, ${(st.mfskModes || []).length} choices in ${groups}; no olivia/thor keys; OLIVIA 64/2000 listed)`);

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

  console.log('── DECODERS: ONE key for MFSK, OLIVIA, CONTESTIA and THOR ──');
  ok(await until(`!document.querySelector('#decodersPanel [data-dec="mfsk"]').hidden && !document.querySelector('#decodersPanel [data-dec="olivia"]') && !document.querySelector('#decodersPanel [data-dec="thor"]')`, 8000), 'one MFSK key offered (no OLIVIA or THOR keys)');
  await ev(`(() => { const b = [...document.querySelectorAll('button, [role=button], .mMode, #mMode')].find(x => /^USB$/.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await sleep(500);
  const openPanel = () => ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS…$/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; } return false; })()`);
  ok(!!(await openPanel()), 'DECODERS… opened from the mode picker');
  await sleep(800);
  const place = await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="mfsk"]'); const g = b && b.closest('.decGroup');
     const h = g && g.querySelector('.decGroupHead');
     return JSON.stringify({ group: h ? h.textContent.trim().toUpperCase() : '', icon: !!(b && b.querySelector('svg')),
                             with: g ? [...g.querySelectorAll('[data-dec]')].map(x => x.dataset.dec).join(',') : '' }); })()`);
  const pl = JSON.parse(place || '{}');
  ok(/AMATEUR/.test(pl.group) && /jtty,mfsk,hell/.test(pl.with) && pl.icon, `in the HF Amateur group, with its icon (${place})`);
  const s0 = await shot('tm-decoders.png');
  const rows = () => ev(`JSON.stringify({ g: [...document.querySelectorAll('#tmGroups .btn')].map(b => b.textContent + (b.classList.contains('on') ? '*' : '')).join('|'),
     m: document.getElementById('tmModeRow').hidden ? '' : [...document.querySelectorAll('#tmModes .btn')].map(b => b.textContent + (b.classList.contains('on') ? '*' : '')).join('|') })`).then((x) => JSON.parse(x || '{}'));
  const pickG = (g) => ev(`(() => { const b = [...document.querySelectorAll('#tmGroups .btn')].find(x => x.textContent === ${JSON.stringify(g)}); if (b) b.click(); return !!b; })()`);
  await ev(`document.querySelector('#decodersPanel [data-dec="mfsk"]').click()`);
  ok(await until(`document.getElementById('decBox').classList.contains('open') && /MFSK/.test(document.getElementById('decTitle').textContent)`, 5000), 'the box opens, titled MFSK');
  let r = await rows();
  ok(r.g === 'AUTO*|MFSK|OLIVIA|CONTESTIA|THOR' && r.m === '', `first open: AUTO lit, FAMILY row the server's families, no MODE row under AUTO (${r.g} / ${r.m})`);
  const textHas = (re) => `(${re}).test(document.getElementById('decText').textContent)`;
  // ★ the clip loops every 20 s, so the box is opened mid-over: a WHOLE over must still arrive within three loops, and
  //   nothing may be printed that was not sent (every passage between the "[MODE · Hz]" lines is a piece of an over)
  const OMSG = 'CQ DE G4XYZ OLIVIA K', MMSG = 'CQ CQ DE G4XYZ MFSK TEST PSE K';
  const pieces = (msgs, from = '') => `(() => { let t = document.getElementById('decText').textContent; ${from ? `t = t.slice(Math.max(0, t.lastIndexOf(${JSON.stringify(from)})));` : ''}
     const segs = t.split(/\\n?\\[[^\\]]*\\]\\n?/).map(s => s.replace(/\\n/g, '')).filter(s => s.trim());
     const bad = segs.filter(s => !${JSON.stringify(msgs)}.some(m => m.repeat(4).includes(s))); return JSON.stringify(bad); })()`;
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  console.log('── AUTO (both signals in the passband) ──');
  ok(await until(`${textHas(/CQ DE G4XYZ OLIVIA K/)} || ${textHas(/CQ CQ DE G4XYZ MFSK TEST PSE K/)}`, 60000), 'AUTO: a whole over arrives');
  const badA = await ev(pieces([OMSG, MMSG]));
  ok(badA === '[]', `AUTO: nothing printed that was not sent (${badA})`);
  const placesA = await ev(`JSON.stringify([...document.getElementById('decText').textContent.matchAll(/\\[([^\\]]*)\\]/g)].map(m => m[1]))`);
  ok(JSON.parse(placesA || '[]').length > 0 && JSON.parse(placesA || '[]').every((h) => /^(OLIVIA 16\/500 · 1[45]\d\d|MFSK16 · [78]\d\d) Hz$/.test(h)), `AUTO named only the two real signals (${placesA})`);
  console.log('── FAMILY → OLIVIA (the fallback row, RTTY\'s pattern) ──');
  await openPanel(); await sleep(400);
  ok(!!(await pickG('OLIVIA')), 'FAMILY → OLIVIA pressed');
  await sleep(400);
  r = await rows();
  ok(r.g === 'AUTO|MFSK|OLIVIA*|CONTESTIA|THOR' && /^ANY\*\|4\/125\|/.test(r.m) && /64\/2000/.test(r.m), `OLIVIA lit; its MODE row: ANY lit, every sub-mode incl. 64/2000 (${r.m})`);
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  await ev(`document.getElementById('decText').textContent = ''`);
  ok(await until(textHas(/CQ DE G4XYZ OLIVIA K/), 60000), 'OLIVIA (any): a whole OLIVIA over arrives');
  const badO = await ev(pieces([OMSG]));
  ok(badO === '[]', `nothing printed that was not sent (${badO})`);
  const placesO = await ev(`JSON.stringify([...document.getElementById('decText').textContent.matchAll(/\\[([^\\]]*)\\]/g)].map(m => m[1]))`);
  ok(JSON.parse(placesO || '[]').every((h) => /^OLIVIA 16\/500 · 1[45]\d\d Hz$/.test(h)), `named only the OLIVIA signal (${placesO})`);
  const s1 = await shot('tm-olivia.png');
  console.log('── FAMILY → MFSK ──');
  await openPanel(); await sleep(400);
  await pickG('MFSK'); await sleep(400);
  r = await rows();
  ok(r.g === 'AUTO|MFSK*|OLIVIA|CONTESTIA|THOR' && r.m === 'ANY*|16|32', `MFSK lit; MODE: ANY, 16, 32 (${r.m})`);
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  ok(await until(textHas(/CQ CQ DE G4XYZ MFSK TEST PSE K/), 60000), 'MFSK (any): a whole MFSK16 over arrives');
  const t = await ev(`document.getElementById('decText').textContent`);
  const after = t.slice(t.lastIndexOf('[MFSK16'));
  ok(/^\[MFSK16 · \d+ Hz\]/.test(after) && !/OLIVIA/.test(after), 'named MFSK16; no OLIVIA text after it');
  const badM = await ev(pieces([MMSG], '[MFSK16'));
  ok(badM === '[]', `nothing printed that was not sent (${badM})`);
  const s1b = await shot('tm-mfsk.png');
  console.log('── AUTO first; a manual choice for the session only (decoderSession.ts) ──');
  await openPanel(); await sleep(400);
  await ev(`document.querySelector('#decodersPanel [data-dec="mfsk"]').click()`); await sleep(600);   // off
  await ev(`document.querySelector('#decodersPanel [data-dec="mfsk"]').click()`); await sleep(600);   // on again
  r = await rows();
  ok(r.g === 'AUTO|MFSK*|OLIVIA|CONTESTIA|THOR', `re-opened in the same session: the manual MFSK kept (${r.g})`);
  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('decBox').getBoundingClientRect().right })`));
  ok(fit.doc <= 391 && fit.box <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)} px)`);
  const s2 = await shot('tm-phone.png');
  console.log('── a new session (the page reloaded): AUTO again ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await trustedClick('#audioGate button');
  await sleep(2500);
  await until(`!document.querySelector('#decodersPanel [data-dec="mfsk"]').hidden`, 8000);
  await ev(`document.querySelector('#decodersPanel [data-dec="mfsk"]').click()`); await sleep(800);
  r = await rows();
  ok(r.g === 'AUTO*|MFSK|OLIVIA|CONTESTIA|THOR' && r.m === '', `after a reload: AUTO lit, nothing manual kept (${r.g})`);
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
