// test-web-ism-gallery.mjs — ISM IN THE WEB GALLERY, through a real vibeserver and the REAL built web page (V12, 2026-10-25).
//   A listener's ISM session on the synthetic 433 MHz clip (fake rtl_tcp --ism) fills the gallery for real; then headless
//   Edge opens GALLERY: the ISM chip is there beside FT8; it lists the day's log and summary card (LISTENER); the card
//   opens as text with its device table; DOWNLOAD saves as "ISM 433.92 <date>.txt"; no TRANSLATE key on our own English;
//   and at PHONE WIDTH (390 px) the chips wrap and nothing scrolls sideways. Screenshots of both widths are written to
//   VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output; the node listener opens no audio socket.
//   VIBESERVER_BIN=… VIBE_ISM_CLIP=… node scripts/test-web-ism-gallery.mjs
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_ISM_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_ISM_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webismgal-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'ISM gallery web test', locator: 'IO92nh', adminPass: 'wig-admin',
  radios: [{ serial: 'wig', label: 'Test radio', enabled: true, configured: true, users: 2, rate: 2400000 }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism', CLIP], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '2'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webismgal-edge-'));
let edge = null;
const today = new Date().toISOString().slice(0, 10);
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  console.log('── a listener\'s ISM session fills the gallery ──');
  const ws0 = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=wig00001&bins=256&proto=1`);
  let last = null, off = false;
  ws0.onmessage = (e) => { if (typeof e.data !== 'string') return; const j = JSON.parse(e.data); if (j.type === 'ism') last = j; if (j.type === 'ism_off') off = true; };
  await new Promise((r) => { ws0.onopen = r; });
  await sleep(800);
  ws0.send(JSON.stringify({ type: 'ism', on: 1, band: '433' }));
  for (let i = 0; i < 200 && !(last && last.devices.length >= 4); i++) await sleep(100);
  ok(!!last && last.devices.length >= 4, `ISM heard ${last ? last.devices.length : 0} devices`);
  ws0.send(JSON.stringify({ type: 'ism', on: 0 }));
  for (let i = 0; i < 50 && !off; i++) await sleep(100);
  ws0.close();
  await sleep(800);
  const g = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=25&kind=ism`).then((r) => r.json());
  ok(g.total === 2, `the server lists 2 ISM items (${g.total})`);

  console.log('── the web page: the ISM chip, the list, the card, the download ──');
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
  const shot = async (name) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    const p = path.join(SHOTS, name);
    fs.writeFileSync(p, Buffer.from(r.result.data, 'base64'));
    return p;
  };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  // ★ the splash's START RADIO (the browser wants a click before audio — silent here: --mute-audio), so the screenshots
  //   show the gallery over the radio, as a listener sees it
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  for (let i = 0; i < 60 && !(await ev(`!!document.querySelector('#audioGate button')`)); i++) await sleep(250);
  {   // ★ a TRUSTED click — the gate starts audio, which a script's .click() may not (test-web-ism.mjs, the same)
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button')?.getBoundingClientRect() || {x:0,y:0,width:0,height:0})`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  }
  let started = false;
  for (let i = 0; i < 32 && !started; i++) { started = await ev(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`); if (!started) await sleep(250); }
  ok(started, 'START RADIO pressed (the splash is gone)');
  for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn').onclick`)); i++) await sleep(250);
  await sleep(2500);
  const rows = () => ev(`JSON.stringify([...document.querySelectorAll('#galList .sres[role=button]')].map(r => r.textContent))`).then(JSON.parse);
  const untilRows = async (n, ms = 6000) => { for (let t = 0; t < ms; t += 150) { const r = await rows(); if (r.length === n) return r; await sleep(150); } return rows(); };
  await ev(`document.getElementById('galleryBtn').click()`);
  const chips = await ev(`[...document.querySelectorAll('#galFilter [data-kind]')].map(b => b.textContent).join(' ')`);
  ok(chips === 'ALL NAVTEX RTTY WEFAX SSTV FT8 CW ISM', `the chips: ${chips}`);
  await ev(`document.querySelector('#galFilter [data-kind="ism"]').click()`);
  let r = await untilRows(2);
  ok(r.length === 2, `ISM: 2 rows — ${r.map((x) => x.slice(0, 70)).join(' | ')}`);
  ok(r.every((x) => x.includes('LISTENER') && x.includes('433.9200 MHz')), 'each a listener\'s, at 433.9200 MHz');
  ok(r.some((x) => /ISM 433\.92 MHz · \d{4}-\d\d-\d\d · 4 devices: 2 weather, 1 TPMS, 1 doorbell \/ remote/.test(x)), 'the card\'s title in the list');
  ok(r.some((x) => x.includes('every reading')), 'the day\'s log in the list');
  ok(r.every((x) => !/ · {2}· /.test(x) && !x.includes(' ·  · ')), 'no empty demodulator gap in the source line');
  const desk = await shot('ism-gallery-desktop-list.png');
  // open the card
  await ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /devices:/.test(r.textContent)).click()`);
  for (let i = 0; i < 40 && !(await ev(`!document.getElementById('galView').hidden && /The last reading/.test(document.getElementById('galText').textContent)`)); i++) await sleep(150);
  const view = await ev(`JSON.stringify({ text: document.getElementById('galText').textContent, meta: document.getElementById('galViewMeta').textContent,
    dl: document.getElementById('galDl').download, tr: !document.getElementById('galTr').hidden })`).then(JSON.parse);
  ok(view.text.includes('ISM summary · 433.92 MHz ISM') && view.text.includes(' · TPMS · TPMS #1 · type B · 32 PSI · 18 °C'), 'the card opens as text, its device table by nickname');
  ok(view.meta.includes("a listener's decoder") && !view.meta.includes('MHz  ·'), `the meta line: ${view.meta}`);
  ok(view.dl === `ISM 433.92 ${today} summary.txt`, `DOWNLOAD saves as "${view.dl}"`);
  ok(!view.tr, 'no TRANSLATE key on our own English');
  const deskCard = await shot('ism-gallery-desktop-card.png');
  await ev(`document.getElementById('galBack').click()`);
  await untilRows(2);
  await ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /every reading/.test(r.textContent)).click()`);
  for (let i = 0; i < 40 && !(await ev(`/Oregon Scientific/.test(document.getElementById('galText').textContent)`)); i++) await sleep(150);
  const logView = await ev(`JSON.stringify({ text: document.getElementById('galText').textContent, dl: document.getElementById('galDl').download })`).then(JSON.parse);
  ok(/\n\d\d:\d\d:\d\d {2}433\.9\d\d {2}Oregon Scientific THGR810 \/ THGN801 · 18\.4 °C/.test(logView.text), 'the log opens: "HH:MM:SS  433.920  Oregon Scientific …"');
  ok(logView.dl === `ISM 433.92 ${today}.txt`, `DOWNLOAD saves the log as "${logView.dl}"`);
  await ev(`document.getElementById('galBack').click()`);

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  const fit = await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, panelW: document.getElementById('galleryPanel').clientWidth,
    panelS: document.getElementById('galleryPanel').scrollWidth, chipsBottom: Math.max(...[...document.querySelectorAll('#galFilter .btn')].map(b => b.getBoundingClientRect().right)),
    open: document.getElementById('galleryPanel').classList.contains('open') })`).then(JSON.parse);
  ok(fit.open, 'the gallery is still open at phone width');
  ok(fit.panelS <= fit.panelW + 1 && fit.chipsBottom <= 390, `nothing scrolls sideways in the gallery (content ${fit.panelS} ≤ panel ${fit.panelW}; chips end at ${Math.round(fit.chipsBottom)} px)`);
  const phone = await shot('ism-gallery-phone-list.png');
  await ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /devices:/.test(r.textContent)).click()`);
  await sleep(1200);
  const fit2 = await ev(`JSON.stringify({ panelW: document.getElementById('galleryPanel').clientWidth, panelS: document.getElementById('galleryPanel').scrollWidth })`).then(JSON.parse);
  ok(fit2.panelS <= fit2.panelW + 1, `the open card fits too (the text wraps or scrolls inside its own box): ${fit2.panelS} ≤ ${fit2.panelW}`);
  const phoneCard = await shot('ism-gallery-phone-card.png');
  ok(errs.length === 0, `no exception in the page${errs.length ? ': ' + errs[0] : ''}`);
  console.log(`   screenshots: ${desk}\n                ${deskCard}\n                ${phone}\n                ${phoneCard}`);
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  try { edge && edge.kill(); } catch {} try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {}
  await sleep(600);
  try { fs.rmSync(prof, { recursive: true, force: true }); } catch {}
  if (!fail) try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
  else console.log(`   (server log: ${logPath})`);
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
