// test-web-drm-constellation.mjs — V12 THE DRM CONSTELLATION + RECEPTION METER in the web client, through a real vibeserver
// and the REAL built web page (2026-11-12, docs/v12/DRM.md "The constellation" / "The reception meter").
//   A fake rtl_tcp replays a SYNTHETIC DRM signal (vibeserver/drm-synth-clip.cpp — the test transmitter, mode B, 16-QAM,
//   24 dB), so no off-air recording is needed. Headless Edge (SILENT: --mute-audio --disable-audio-output) then:
//   • DRM chosen: the card's signal bar becomes DRM's meter — three bars, "DRM strong · …", and the reading box the
//     DECODER's SNR ("SNR 24 dB"), never the passband meter's; its tooltip says in plain words why the two differ;
//   • the compact box: no constellation (and none asked for — the page sends drm_scopes only with the pane open);
//   • SIGNAL ▸: the constellation at the top of the pane — the canvas DRAWN (yellow MSC dots in 16 clusters, the SDC and FAC
//     colours present), the legend "MSC 16-QAM · SDC 4-QAM · FAC 4-QAM", the CONSTELLATION heading in DAB's scope style;
//   • it is refreshed (a new drm_iq about once per super frame, the canvas changes);
//   • ◂ STATION: the page tells the server to stop (no drm_iq arrives after it);
//   • an OLDER server (simulated by dropping drm_iq before the page sees it): the pane has no constellation, no empty box;
//   • 390 px: nothing scrolls sideways; no exception on the page.
//   Screenshots: VIBE_SHOT_DIR (default the temp dir); VIBE_DRM_SHOT=<path> also writes the desktop Signal pane there.
//   VIBESERVER_BIN=…/vibeserver VIBE_DRM_SYNTH=…/drm-synth.cu8 node scripts/test-web-drm-constellation.mjs
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_DRM_SYNTH || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_DRM_SYNTH)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 288000, CENTRE = 9540000, STATION = 9570000;

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webdrmc-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'DRM constellation web test', locator: 'IO92nh', adminPass: 'wdrmc-admin',
  radios: [{ serial: 'wdrmc', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(STATION), '--mode', 'am', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webdrmc-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
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
  const shot = async (name, also) => {
    const r = await send('Page.captureScreenshot', { format: 'png' });
    const p = path.join(SHOTS, name);
    const buf = Buffer.from(r.result.data, 'base64');
    fs.writeFileSync(p, buf);
    if (also) { try { fs.writeFileSync(also, buf); } catch {} }
    return p;
  };
  const trustedClick = async (sel) => {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || {x:0,y:0,width:0,height:0})`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  };
  await send('Page.enable'); await send('Runtime.enable');
  // ★ Before the page loads: count what the page SENDS and RECEIVES on its sockets (drm_scopes out, drm_iq in), and a switch
  //   that drops drm_iq before the page sees it — an OLDER server, which never sends one.
  await send('Page.addScriptToEvaluateOnNewDocument', { source: `(() => {
    window.__drm = { out: [], iq: 0, iqAt: [], dropIq: false };
    const S = WebSocket.prototype.send;
    WebSocket.prototype.send = function (d) { try { if (typeof d === 'string' && d.includes('drm_scopes')) window.__drm.out.push(JSON.parse(d)); } catch {} return S.call(this, d); };
    const add = WebSocket.prototype.addEventListener;
    const wrap = (ws, f) => function (e) {
      if (typeof e.data === 'string' && e.data.startsWith('{"type":"drm_iq"')) { window.__drm.iq++; window.__drm.iqAt.push(Date.now()); if (window.__drm.dropIq) return; }
      return f.call(this, e);
    };
    Object.defineProperty(WebSocket.prototype, 'onmessage', { configurable: true,
      set(f) { this.__om = f; add.call(this, 'message', wrap(this, (e) => f && f.call(this, e))); }, get() { return this.__om; } });
  })();` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await trustedClick('#audioGate button');   // ★ a trusted click starts audio (silent: --mute-audio)
  await sleep(2500);

  console.log('── DRM chosen: the meter ──');
  await ev(`document.getElementById('mMode').click()`);
  await sleep(400);
  await ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === 'DRM').click()`);
  ok(await until(`document.getElementById('drmBox')?.classList.contains('open')`, 4000), 'choosing DRM opens the DRM box');
  ok(await until(`/VIBE TEST/.test(document.querySelector('#drmBody .drmLabel')?.textContent || '')`, 60000), 'the synthetic station locks ("VIBE TEST")');
  ok(await until(`!document.getElementById('mDabQ').hidden && /^DRM strong$/.test(document.getElementById('mDabQLbl').textContent)`, 15000),
     `the signal bar is DRM's meter: "${await ev(`document.getElementById('mDabQ').textContent`)}" (level ${await ev(`document.getElementById('mDabQ').dataset.level`)})`);
  // ★ the box is the meter's ~6 s mean of the decoder's SNR, the DRM box the latest report: equal once the lock has settled
  const same = `(() => { const b = document.getElementById('mSnr').textContent, r = Number((document.querySelector('#drmBody .drmSnr')?.textContent || '').replace(/[^0-9.]/g, ''));
                         return /^SNR \\d+ dB$/.test(b) && Math.abs(Number(b.replace(/\\D/g, '')) - r) <= 2; })()`;
  await until(same, 15000);
  const box = await ev(`document.getElementById('mSnr').textContent`);
  const rep = Number(await ev(`(document.querySelector('#drmBody .drmSnr')?.textContent || '').replace(/[^0-9.]/g, '')`));
  ok(await ev(same),
     `the reading box is the DECODER's SNR: "${box}" (the DRM box says ${rep} dB)`);
  ok(/Decoded SNR/.test(await ev(`document.getElementById('mSnr').title`)) && /signal meter/.test(await ev(`document.getElementById('mSnr').title`)),
     'its tooltip says in plain words why it differs from the signal meter');
  ok(/decoded SNR \d+ dB/.test(await ev(`document.getElementById('mDabQDet').textContent`)), `the bar names it: "${await ev(`document.getElementById('mDabQDet').textContent`)}"`);
  ok(await ev(`document.getElementById('mSig').style.width === '0%'`), 'the passband gradient is emptied (the bars carry the level)');
  await shot('drm-meter-desktop.png');

  console.log('── the compact box: no constellation, none asked for ──');
  ok(await ev(`!window.__drm.out.some(m => m.on === 1)`) && await ev(`window.__drm.iq === 0`), 'the station box asks for nothing and gets nothing');

  console.log('── SIGNAL ▸ : the constellation ──');
  await trustedClick('#drmPane');
  ok(await until(`window.__drm.out.some(m => m.on === 1)`, 3000), 'opening the pane asks the server for the cells (drm_scopes on)');
  ok(await until(`!document.getElementById('drmSigScopes').hidden`, 5000), 'the constellation appears at the top of the pane');
  ok(await ev(`(() => { const s = document.getElementById('drmSigScopes'), r = document.querySelector('#drmSignal h4'); return !!s && !!r && s.getBoundingClientRect().bottom <= r.getBoundingClientRect().top + 1; })()`),
     'above the LOCK group, as DAB\'s scopes sit above its rows');
  ok(await ev(`document.querySelector('#drmSigScopes span')?.textContent === 'CONSTELLATION'`) && await ev(`document.getElementById('drmSigScopes').classList.contains('dabScopes')`),
     'DAB\'s scope style: the CONSTELLATION heading, the .dabScopes box');
  const leg = await ev(`document.getElementById('drmConstLeg').textContent.replace(/\\s+/g, ' ').trim()`);
  ok(/MSC 16-QAM/.test(leg) && /SDC 4-QAM/.test(leg) && /FAC 4-QAM/.test(leg), `the legend: "${leg}"`);
  // what the canvas holds: the MSC yellow in 16 clusters, the SDC/FAC colours present
  const pix = JSON.parse(await ev(`JSON.stringify((() => {
    const c = document.getElementById('drmConst'), g = c.getContext('2d'), d = g.getImageData(0, 0, c.width, c.height).data;
    let y = 0, b = 0, p = 0; const cells = new Set();
    for (let i = 0; i < d.length; i += 4) {
      const R = d[i], G = d[i + 1], B = d[i + 2], A = d[i + 3]; if (A < 200) continue;
      const x = (i / 4) % c.width, yy = Math.floor(i / 4 / c.width);
      if (R > 230 && G > 210 && B < 140) { y++; cells.add(Math.floor(x / (c.width / 4)) * 4 + Math.floor(yy / (c.height / 4))); }
      else if (R < 140 && G > 180 && B > 230) b++;
      else if (R > 230 && G < 160 && B > 190) p++;
    }
    return { yellow: y, blue: b, pink: p, quadrants: cells.size, tip: document.getElementById('drmSigScopes').title };
  })())`));
  ok(pix.yellow > 300 && pix.quadrants === 16, `the MSC is drawn in DAB's yellow and fills all 16 cells of the 16-QAM grid (${pix.yellow} px, ${pix.quadrants}/16)`);
  ok(pix.blue > 20 && pix.pink > 20, `the SDC (blue, ${pix.blue} px) and FAC (pink, ${pix.pink} px) are drawn too`);
  ok(/Tight dots/.test(pix.tip), 'the picture has its plain-words tooltip');
  const before = await ev(`document.getElementById('drmConst').toDataURL().length + ':' + window.__drm.iq`);
  await sleep(3500);
  const after = await ev(`document.getElementById('drmConst').toDataURL().length + ':' + window.__drm.iq`);
  const n0 = Number(before.split(':')[1]), n1 = Number(after.split(':')[1]);
  ok(n1 - n0 >= 2 && n1 - n0 <= 4, `refreshed about once per super frame (${n1 - n0} new in 3.5 s)`);
  const sp = await shot('drm-constellation-desktop.png', process.env.VIBE_DRM_SHOT || '');
  // the meter stays the meter while the pane is open
  ok(/^DRM strong$/.test(await ev(`document.getElementById('mDabQLbl').textContent`)), 'the meter is unchanged by the pane');

  console.log('── 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, box: document.getElementById('drmBox').getBoundingClientRect().right,
      sc: document.getElementById('drmSigScopes').getBoundingClientRect().right, q: document.getElementById('mDabQ').getBoundingClientRect().right })`));
  ok(fit.doc <= 391 && fit.sc <= fit.box + 0.5 && fit.q <= 391, `nothing scrolls sideways (page ${fit.doc}, scope to ${Math.round(fit.sc)}, box to ${Math.round(fit.box)}, meter to ${Math.round(fit.q)} px)`);
  const sph = await shot('drm-constellation-phone.png');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await sleep(800);

  console.log('── ◂ STATION: the cells stop ──');
  await trustedClick('#drmPane');
  ok(await until(`window.__drm.out.length >= 2 && window.__drm.out[window.__drm.out.length - 1].on === 0`, 3000), 'closing the pane tells the server (drm_scopes off)');
  await sleep(1500);
  const k0 = await ev(`window.__drm.iq`); await sleep(4000);
  ok(await ev(`window.__drm.iq`) === k0, `no drm_iq arrives after it (${k0} in all)`);

  console.log('── an OLDER server (no drm_iq): no constellation, no empty box ──');
  await ev(`window.__drm.dropIq = true`);
  await sleep(7000);   // ★ longer than DRM_IQ_STALE_MS, so the last real picture has expired
  await trustedClick('#drmPane');
  await sleep(4000);
  ok(await ev(`document.getElementById('drmSigScopes').hidden`), 'the pane has no constellation');
  ok(await ev(`document.querySelectorAll('#drmSignal h4').length === 6`), 'and its rows are all there');
  await trustedClick('#drmPane');

  console.log('── back to AM ──');
  await ev(`document.getElementById('mMode').click()`);
  await sleep(300);
  await ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === 'AM').click()`);
  ok(await until(`document.getElementById('mDabQ').hidden`, 3000), 'AM: the meter is gone, the signal bar is back');
  ok(await ev(`!/Decoded SNR/.test(document.getElementById('mSnr').title)`), 'AM: the reading box\'s own tooltip is back');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${sp}  ${sph}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(800);
  for (const d of [dir, prof]) { try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }
}
console.log(`\n${fail ? '✗' : '✓'} test-web-drm-constellation: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
