// test-web-drm.mjs — V12 DRM IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (2026-10-26).
//   A fake rtl_tcp replays REAL DRM (RRI 9570 kHz off Stuart's UberSDR, scripts/drm-clip.py); the dial is on it in AM.
//   Headless Edge then:
//   • the mode picker offers DRM (this server lists it); choosing it shows the DRM box;
//   • the box fills from the server's reports: the station's label, the six lights, robustness/QAM/rate, SNR, the codec —
//     and, for this AAC station, the server's sentence saying why there is no audio;
//   • the station bar (VTS) carries the label; the bandwidth row is hidden (it does nothing in DRM);
//   • back to AM: the box goes;
//   • the SIGNAL key swaps the box for the Signal pane (V12 2026-10-26): its six groups, delay spread against the guard,
//     Doppler, lock times, MERs, errors; a tooltip on every row; DAB's colours; one pad stop; STATION brings the box back;
//   • the zoom note (V12 2026-10-29, DRM_ZOOM_NOTE): this rtl_tcp radio has the IF filter on Auto, so the server says a
//     zoom can retune it (`zoomMovesRadio`) — one dim line under the box and under the Signal pane, in the box's own font;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways, box or pane; no exception on the page. Screenshots to VIBE_SHOT_DIR.
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
  // ★ a FAAD2 server (VIBE_DRM_FAAD2) PLAYS this AAC station; one without says why it is silent
  const playing = await ev(`!!document.querySelector('#drmBody .drmCodec .drmLight.on')`);
  if (playing) ok(/FAAD2/.test(codec), `audio playing, the decoder named: "${codec.trim()}"`);
  else {
    const why = await ev(`document.querySelector('#drmBody .drmWhy')?.textContent || ''`);
    ok(/cannot be played/.test(why), `why there is no audio, in words: "${why.slice(0, 90)}…"`);
  }
  ok(await until(`/SNR Tiganesti E1/.test(document.getElementById('vtsName')?.textContent || '')`, 5000), 'the station bar carries the label');
  ok(await ev(`(() => { const r = document.querySelector('.bwRow'); return !r || r.style.display === 'none'; })()`), 'the bandwidth row is hidden in DRM');
  // ★★ V12 the zoom note: the server's word (`zoomMovesRadio` — an RTL, IF filter on Auto), drawn as one dim line
  const NOTE = 'Zooming here can retune the radio (IF filter on Auto) — DRM resyncs for a few seconds after a big zoom.';
  ok(await until(`document.querySelector('#drmBody .drmNote')?.textContent === ${JSON.stringify(NOTE)}`, 5000),
     `the zoom note under the box: "${await ev(`document.querySelector('#drmBody .drmNote')?.textContent || ''`)}"`);
  const nst = JSON.parse(await ev(`JSON.stringify((() => { const n = getComputedStyle(document.querySelector('#drmBody .drmNote')), m = getComputedStyle(document.querySelector('#drmBody .drmSmall') || document.querySelector('#drmBody .drmNote'));
      return { c: n.color, mc: m.color, f: n.fontFamily, bf: getComputedStyle(document.getElementById('drmBox')).fontFamily, fs: n.fontSize }; })())`));
  ok(nst.c === nst.mc && nst.f === nst.bf && nst.fs === '11px', `…dim like the box's small line, in the box's font (${nst.c}, ${nst.fs})`);
  const s1 = await shot('drm-desktop.png');

  // ── ★★ V12 THE SIGNAL PANE (2026-10-26): DAB's kind of advanced pane, behind the SIGNAL key in the box's header ──
  console.log('── the Signal pane ──');
  ok(await ev(`document.getElementById('drmBox').hasAttribute('data-navbox')`), 'the box is ONE pad stop ([data-navbox]) — Enter hands it the D-pad');
  ok(await ev(`/SIGNAL/.test(document.getElementById('drmPane')?.textContent || '')`), `the header's key reads "${await ev(`document.getElementById('drmPane')?.textContent`)}"`);
  await trustedClick('#drmPane');
  ok(await until(`!document.getElementById('drmSignal').hidden && document.getElementById('drmBody').hidden`, 3000), 'SIGNAL swaps the box for the pane');
  ok(await ev(`/STATION/.test(document.getElementById('drmPane').textContent)`), 'the key now names where it goes back to: STATION');
  const heads = await ev(`[...document.querySelectorAll('#drmSignal h4')].map(h => h.textContent).join(' ')`);
  ok(heads === 'LOCK SIGNAL QUALITY ERRORS AUDIO STATION', `the groups: ${heads}`);
  const row = (k) => `([...document.querySelectorAll('#drmSignal .row')].find(r => r.firstElementChild.textContent === ${JSON.stringify(k)})?.lastElementChild.textContent || '')`;
  ok(await until(`/ms of 5\\.33 ms guard/.test(${row('Delay spread')})`, 15000), `delay spread, against the guard: "${await ev(row('Delay spread'))}"`);
  ok(await until(`/Hz/.test(${row('Doppler spread')})`, 8000), `Doppler spread: "${await ev(row('Doppler spread'))}"`);
  ok(/\d+\.\d s/.test(await ev(row('MSC after'))), `time to the MSC: "${await ev(row('MSC after'))}"`);
  ok(/^16-QAM · rate 0\.\d+/.test(await ev(row('Main channel'))), `the main channel: "${await ev(row('Main channel'))}"`);
  ok(/dB/.test(await ev(row('SDC MER'))) && /dB/.test(await ev(row('MSC MER'))), `MSC / SDC MER: ${await ev(row('MSC MER'))} / ${await ev(row('SDC MER'))}`);
  ok(/bad of/.test(await ev(row('FAC'))), `FAC errors, not passes: "${await ev(row('FAC'))}"`);
  ok(await ev(row('Label')) === 'SNR Tiganesti E1', 'the station group carries the label');
  ok(await ev(`[...document.querySelectorAll('#drmSignal .row')].every(r => (r.title || '').length > 20)`), 'every row has its plain-English tooltip');
  ok(await ev(`document.querySelectorAll('#drmSignal .tl.ok, #drmSignal .tl.warn, #drmSignal .tl.bad').length >= 8`), 'traffic lights on the values with a nominal range');
  // the pane's colours are DAB's (one rule, two readers)
  const colours = JSON.parse(await ev(`JSON.stringify((() => { const d = document.createElement('div'); d.id = 'dabSignal'; d.style.display = 'block';
      d.innerHTML = '<div class="row"><span>x</span><span><span class="tl ok">1</span></span></div>'; document.body.appendChild(d);
      const a = getComputedStyle(d.querySelector('.row > span:first-child')).color, b = getComputedStyle(d.querySelector('.tl.ok')).color; d.remove();
      const r = document.querySelector('#drmSignal .row'); const ok1 = document.querySelector('#drmSignal .tl.ok');
      return { dabLbl: a, dabOk: b, drmLbl: getComputedStyle(r.firstElementChild).color, drmOk: ok1 ? getComputedStyle(ok1).color : '' }; })())`));
  ok(colours.dabLbl === colours.drmLbl && colours.dabOk === colours.drmOk, `labels and lights in DAB's colours (${colours.drmLbl}, ${colours.drmOk})`);
  ok(await ev(`(() => { const n = document.querySelector('#drmSignal .drmNote'), rows = document.querySelectorAll('#drmSignal .row');
      return !!n && n.textContent === ${JSON.stringify(NOTE)} && !!rows.length && n.getBoundingClientRect().top >= rows[rows.length - 1].getBoundingClientRect().bottom; })()`),
     'the zoom note under the Signal pane\'s rows too');
  const clear = async () => JSON.parse(await ev(`JSON.stringify((() => { const h = document.getElementById('srvHealth'), b = document.getElementById('drmPane').getBoundingClientRect();
      const r = h && getComputedStyle(h).display !== 'none' && h.getBoundingClientRect().height > 0 ? h.getBoundingClientRect() : null;
      return { key: Math.round(b.top), pill: r ? Math.round(r.bottom) : null, hit: !!r && b.top < r.bottom && b.right > r.left }; })())`));
  const c1 = await clear();
  ok(!c1.hit, `the pane's header key is clear of the health pill (key at ${c1.key} px, pill to ${c1.pill ?? '—'} px)`);
  const s3 = await shot('drm-signal-desktop.png');

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('drmBox').getBoundingClientRect().right,
                                                    left: document.getElementById('drmBox').getBoundingClientRect().left,
                                                    rows: Math.max(...[...document.querySelectorAll('#drmSignal .row')].map(r => r.getBoundingClientRect().right)) })`));
  const noteR = await ev(`Math.round(document.querySelector('#drmSignal .drmNote')?.getBoundingClientRect().right || 0)`);
  ok(noteR > 0 && noteR <= fit.box + 0.5, `390 px: the pane's zoom note wraps inside the box (to ${noteR} px)`);
  ok(fit.doc <= 391 && fit.box <= 391 && fit.left >= 0 && fit.rows <= fit.box + 0.5,
     `the Signal pane: nothing scrolls sideways (page ${fit.doc}, box ${Math.round(fit.left)}–${Math.round(fit.box)}, rows to ${Math.round(fit.rows)} px)`);
  const c2 = await clear();
  ok(!c2.hit, `390 px: the header key is clear of the health pill (key at ${c2.key} px, pill to ${c2.pill ?? '—'} px)`);
  const s4 = await shot('drm-signal-phone.png');
  await trustedClick('#drmPane');
  ok(await until(`document.getElementById('drmSignal').hidden && !document.getElementById('drmBody').hidden`, 3000), 'STATION brings the compact box back');
  const fit2 = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('drmBox').getBoundingClientRect().right,
                                                    left: document.getElementById('drmBox').getBoundingClientRect().left })`));
  const noteR2 = await ev(`Math.round(document.querySelector('#drmBody .drmNote')?.getBoundingClientRect().right || 0)`);
  ok(noteR2 > 0 && noteR2 <= fit2.box + 0.5, `390 px: the box's zoom note wraps inside it (to ${noteR2} px)`);
  ok(fit2.doc <= 391 && fit2.box <= 391 && fit2.left >= 0, `the box: nothing scrolls sideways (page ${fit2.doc}, box ${Math.round(fit2.left)}–${Math.round(fit2.box)} px)`);
  const s2 = await shot('drm-phone.png');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });

  console.log('── back to AM ──');
  await ev(`document.getElementById('mMode').click()`);
  await sleep(300);
  await ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === 'AM').click()`);
  ok(await until(`!document.getElementById('drmBox')?.classList.contains('open')`, 3000), 'AM: the DRM box is gone');
  ok(await ev(`(() => { const r = document.querySelector('.bwRow'); return !r || r.style.display !== 'none'; })()`), 'AM: the bandwidth row is back');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${s1}  ${s3}  ${s4}  ${s2}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(800);
  for (const d of [dir, prof]) { try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }   // ★ keep the Mac clean
}
console.log(`\n${fail ? '✗' : '✓'} test-web-drm: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
