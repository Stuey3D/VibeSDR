// test-web-freedv.mjs — V12 FreeDV DIGITAL VOICE in the web client, through a real vibeserver and THIS tree's built page
// (web/dist, served through CDP interception in place of the binary's own — test-web-health-tuck.mjs's way, so the page
// under test is the one just built and the server binary need not be rebuilt for a client change).
//   A fake rtl_tcp replays test-freedv's synthetic clip (three stations made by codec2's own transmitter: 700D at centre
//   +10 kHz, 700E +20 kHz, 1600 −10 kHz). Headless Edge, SILENT (--mute-audio --disable-audio-output), then:
//   • the mode picker has a FreeDV key, among the digital modes after the analogue ones, legend "FreeDV";
//   • choosing it sends {"type":"tune",mode:"freedv"} and asks for the report (freedv_sub with no sub);
//   • the VTS line says "FreeDV · searching", then "FreeDV 700D · N dB" once the modem is in sync;
//   • in the picker: no BANDWIDTH row, a FREEDV row of AUTO + the server's sub-modes, AUTO lit by the server's report;
//   • 1600 by hand: lit only once the server's report says so, the line says "FreeDV 1600 · searching"; AUTO goes back;
//   • 390 px: the row is one line inside the menu, nothing scrolls sideways;
//   • leaving FreeDV (USB): the line drops its FreeDV text, the FREEDV row goes and the BANDWIDTH row comes back;
//   • 40 m: the passband is drawn on the LOWER sideband (the server's convention, its report's "sb");
//   • the SIDEBAND row (AUTO / USB / LSB) under the sub-modes: a USB station at 7.21 MHz never syncs on AUTO (LSB there);
//     USB by hand is lit by the server's report, the line says "FreeDV USB · searching" until it syncs, the passband moves
//     above the dial, and the row fits one line at 390 px;
//   • RADE V1 (where the server has it built in and the clip has its station at +30 kHz): a RADE key first in the FREEDV
//     row, "FreeDV RADE · N dB" in sync, then — once the over ends — "FreeDV · last G4ABC" (its end-of-over callsign); RADE
//     by hand lit by the report; five sub-mode keys still one row at 390 px (checked above); a server without libcodec2
//     (VIBE_CODEC2_DISABLE=1) still has the FreeDV key, with AUTO and RADE alone;
//   • a server whose FreeDV is switched off altogether (VIBE_FREEDV_DISABLE=1): no FreeDV key at all.
//   Screenshots: VIBE_SHOT_DIR (default the temp dir).
//   node scripts/build-web.mjs   (then)
//   VIBESERVER_BIN=…/vibeserver VIBE_FREEDV_CLIP=fdv.cu8 VIBE_FREEDV_CLIP_LSB=fdv-lsb.cu8 node scripts/test-web-freedv.mjs
//   (make the clips: test-freedv clip fdv.cu8 288000 20 12; test-freedv clip fdv-lsb.cu8 288000 20 12 lsb)
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_FREEDV_CLIP || '', CLIP_LSB = process.env.VIBE_FREEDV_CLIP_LSB || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
const DIST = path.join(root, 'web/dist');
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_FREEDV_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
if (!fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.log('   (not run — build the page first: node scripts/build-web.mjs)'); process.exit(3); }
const RATE = 288000;

/** One server + one page; `body(p)` drives it. */
async function scenario(title, { clip, centre, freq, env: extraEnv = {} }, body) {
  console.log(`── ${title} ──`);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webfdv-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'FreeDV web test', locator: 'IO92nh', adminPass: 'wfdv-admin',
    radios: [{ serial: 'wfdv', label: 'Test radio', enabled: true, configured: true }] }));
  const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, ...extraEnv, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(freq), '--mode', 'usb', '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webfdv-edge-'));
  let edge = null;
  try {
    for (let i = 0; i < 150; i++) {
      if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
      await sleep(100);
    }
    const H = `http://127.0.0.1:${port}`;
    edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
      // ★ a fulfilled (intercepted) page counts as another address space and its sockets to 127.0.0.1 are blocked otherwise
      '--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessRespectPreflightResults,BlockInsecurePrivateNetworkRequests',
      '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
    let page = null;
    for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
    const ws = new WebSocket(page.webSocketDebuggerUrl);
    let id = 0; const pend = new Map(); const errs = [];
    await new Promise((r) => { ws.onopen = r; });
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
    ws.onmessage = (e) => {
      const m = JSON.parse(e.data);
      if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
      if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception');
      if (m.method === 'Fetch.requestPaused') {
        // ★ THIS tree's page and scripts in place of the binary's own
        const u = new URL(m.params.request.url);
        const isPage = u.pathname === '/' || u.pathname === '/index.html';
        const file = isPage ? path.join(DIST, 'vibesdr.html') : path.join(DIST, u.pathname);
        if (u.origin !== H || !(isPage || u.pathname.startsWith('/vs/')) || !file.startsWith(DIST) || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
          send('Fetch.continueRequest', { requestId: m.params.requestId }); return;
        }
        const type = file.endsWith('.js') ? 'text/javascript' : file.endsWith('.css') ? 'text/css' : 'text/html; charset=utf-8';
        const headers = (m.params.responseHeaders || [])
          .filter((h) => !/^(content-encoding|content-length|content-type|etag|last-modified|cache-control|content-security-policy)$/i.test(h.name))
          .concat([{ name: 'Content-Type', value: type }, { name: 'Cache-Control', value: 'no-store' }]);
        send('Fetch.fulfillRequest', { requestId: m.params.requestId, responseCode: 200, responseHeaders: headers, body: fs.readFileSync(file).toString('base64') });
      }
    };
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
    // ★ Before the page loads: what the page SENDS (tune / bandwidth / freedv_sub) and the freedv reports it RECEIVES.
    await send('Page.addScriptToEvaluateOnNewDocument', { source: `(() => {
      window.__fdv = { out: [], in: [] };
      const S = WebSocket.prototype.send;
      WebSocket.prototype.send = function (d) { try { if (typeof d === 'string' && /"(tune|bandwidth|freedv_sub)"/.test(d)) window.__fdv.out.push(JSON.parse(d)); } catch {} return S.call(this, d); };
      const add = WebSocket.prototype.addEventListener;
      const wrap = (f) => function (e) { if (typeof e.data === 'string' && e.data.startsWith('{"type":"freedv"')) { try { window.__fdv.in.push(JSON.parse(e.data)); } catch {} } return f.call(this, e); };
      Object.defineProperty(WebSocket.prototype, 'onmessage', { configurable: true,
        set(f) { this.__om = f; add.call(this, 'message', wrap((e) => f && f.call(this, e))); }, get() { return this.__om; } });
    })();` });
    await send('Fetch.enable', { patterns: ['Document', 'Script', 'Stylesheet'].map((resourceType) => ({ urlPattern: '*', resourceType, requestStage: 'Response' })) });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    await send('Page.navigate', { url: `${H}/index.html?join` });
    await sleep(4000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
    await until(`!!document.querySelector('#audioGate button')`, 15000);
    await trustedClick('#audioGate button');   // ★ a trusted click starts audio (silent: --mute-audio)
    await sleep(2500);
    const P = {
      ev, until, shot, send, errs,
      openPicker: async () => { if (!(await ev(`!!document.getElementById('mModeMenu')`))) await ev(`document.getElementById('mMode').click()`); await sleep(400); },
      // ★ through the picker's own CLOSE key — it puts the moved width row back (removing the menu would lose it)
      closePicker: async () => { await ev(`[...(document.getElementById('mModeMenu')?.querySelectorAll('button.mModeClose') || [])].find(b => /^\\s*CLOSE\\s*$/i.test(b.textContent))?.click(); 1`); await sleep(250); },
      keys: () => ev(`JSON.stringify([...document.querySelectorAll('#mModeMenu .mModeGrid:not(#mFreeDvSub):not(#mFreeDvSb) > .mModeOpt')].map(b => b.textContent))`).then(JSON.parse),
      pick: async (legend) => { await P.openPicker(); return ev(`(() => { const b = [...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === ${JSON.stringify(legend)}); if (b) b.click(); return !!b; })()`); },
      vts: () => ev(`(document.getElementById('vtsName')?.textContent || '').trim()`),
      lit: () => ev(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt.on')].map(b => b.textContent).join(',')`),
      litSb: () => ev(`[...document.querySelectorAll('#mFreeDvSb .mModeOpt.on')].map(b => b.textContent).join(',')`),
    };
    await body(P);
    ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  } finally {
    try { edge?.kill('SIGTERM'); } catch {}
    srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(800);
    for (const d of [dir, prof]) { try { fs.rmSync(d, { recursive: true, force: true }); } catch {} }
  }
}

const shots = [];
/* ★ RADE is in the server under test when its vibeserver.json lists it — asked of a throwaway server on the same clip. */
let HAVE_RADE = false;
await scenario('(probe: is RADE in this server?)', { clip: CLIP, centre: 14200000, freq: 14210000 }, async (P) => {
  HAVE_RADE = !!(await P.ev(`fetch('/vibeserver.json').then(r => r.json()).then(j => (j.freedvSubs || []).includes('RADE'))`));
  console.log(`   RADE V1 ${HAVE_RADE ? 'in this server' : 'NOT in this server (its checks are not run)'}`);
});

await scenario('20 m, USB: the key, the line, the sub-mode row', { clip: CLIP, centre: 14200000, freq: 14210000 }, async (P) => {
  await P.openPicker();
  const keys = await P.keys();
  const i = keys.indexOf('FreeDV');
  // ★ Stuart 11-20: "wfm nfm am lsb usb cwl cwu sam ecss then digital modes" — FreeDV after every analogue key.
  const lastAnalogue = Math.max(...['WFM', 'NFM', 'AM', 'LSB', 'USB', 'CWL', 'CWU', 'SAM', 'SAM-U', 'SAM-L', 'ECSS-U', 'ECSS-L', 'AM-ST'].map((k) => keys.indexOf(k)));
  ok(i > 0 && i > lastAnalogue, `a FreeDV key, among the digital modes after the analogue ones (${keys.join(' ')})`);
  ok(await P.ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === 'FreeDV')?.title === 'FreeDV digital voice'`), 'its name in the tooltip');
  ok(!(await P.ev(`!!document.getElementById('mFreeDvSub')`)), 'no FREEDV row while the mode is USB');
  await P.closePicker();
  await P.ev(`window.__fdv.out.length = 0; 1`);
  ok(await P.pick('FreeDV'), 'FreeDV chosen');
  ok(await P.until(`window.__fdv.out.some(m => m.type === 'tune' && m.mode === 'freedv')`, 3000), 'the page sends {"type":"tune",mode:"freedv"}');
  ok(await P.until(`window.__fdv.out.some(m => m.type === 'freedv_sub' && !('sub' in m))`, 3000), 'and asks for the report (freedv_sub, no sub)');
  const bw = await P.ev(`JSON.stringify(window.__fdv.out.find(m => m.type === 'bandwidth'))`);
  ok(/"bandwidthLow":50,"bandwidthHigh":2700/.test(bw || ''), `20 m: the passband starts on the upper sideband (${bw})`);
  ok(await P.until(`/^FreeDV/.test(document.getElementById('vtsName')?.textContent || '')`, 6000), `the VTS line names FreeDV ("${await P.vts()}")`);
  ok(await P.until(`/^FreeDV 700D · -?\\d+ dB/.test(document.getElementById('vtsName')?.textContent || '')`, 45000), `in sync: "${await P.vts()}"`);
  const rep = await P.ev(`JSON.stringify(window.__fdv.in[window.__fdv.in.length - 1])`);
  ok(/"sb":"usb"/.test(rep || ''), `the server's report: ${rep}`);
  ok(await P.ev(`document.getElementById('bwHiVal').textContent === '+2.7k' && document.getElementById('bwLoVal').textContent === '+50'`),
     `the passband is the reported side (${await P.ev(`document.getElementById('bwLoVal').textContent + ' … ' + document.getElementById('bwHiVal').textContent`)})`);

  await P.openPicker();
  ok(await P.ev(`!!document.getElementById('mFreeDvSub')`), 'in the picker: the FREEDV row');
  ok(await P.ev(`[...document.querySelectorAll('#mModeMenu .mBwSep')].every(s => s.textContent !== 'BANDWIDTH')`), 'and no BANDWIDTH row (the width is the server\'s)');
  const subs = await P.ev(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt')].map(b => b.textContent).join(' ')`);
  ok(subs === (HAVE_RADE ? 'AUTO RADE 700D 700E 1600' : 'AUTO 700D 700E 1600'), `AUTO and the server's sub-modes: ${subs}`);
  ok(await P.lit() === 'AUTO', `AUTO lit, from the server's report ("${await P.lit()}")`);
  shots.push(await P.shot('freedv-desktop.png'));

  // ★ the row is lit by the server's report, never by the tap
  await P.ev(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt')].find(b => b.textContent === '1600').click(); 1`);
  ok(await P.until(`window.__fdv.out.some(m => m.type === 'freedv_sub' && m.sub === '1600')`, 2000), 'tapping 1600 sends freedv_sub 1600');
  ok(await P.until(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt.on')].map(b => b.textContent).join() === '1600'`, 5000),
     `1600 lit once the server's report says so ("${await P.lit()}")`);
  ok(await P.until(`(document.getElementById('vtsName')?.textContent || '').trim() === 'FreeDV 1600 · searching'`, 15000), `the line: "${await P.vts()}"`);
  await P.ev(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt')].find(b => b.textContent === 'AUTO').click(); 1`);
  ok(await P.until(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt.on')].map(b => b.textContent).join() === 'AUTO'`, 5000), 'AUTO goes back');
  ok(await P.until(`/^FreeDV 700D · -?\\d+ dB/.test(document.getElementById('vtsName')?.textContent || '')`, 45000), `and finds 700D again: "${await P.vts()}"`);

  await P.send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  await P.closePicker(); await P.openPicker();
  const fit = JSON.parse(await P.ev(`JSON.stringify((() => { const m = document.getElementById('mModeMenu').getBoundingClientRect();
    const bs = [...document.querySelectorAll('#mFreeDvSub .mModeOpt')].map(b => b.getBoundingClientRect());
    return { doc: document.documentElement.scrollWidth, mL: m.left, mR: m.right, tops: [...new Set(bs.map(b => Math.round(b.top)))].length,
             l: Math.min(...bs.map(b => b.left)), r: Math.max(...bs.map(b => b.right)), clip: [...document.querySelectorAll('#mFreeDvSub .mModeOpt')].some(b => b.scrollWidth > b.clientWidth + 1) }; })())`));
  ok(fit.doc <= 391 && fit.tops === 1 && fit.l >= fit.mL && fit.r <= fit.mR + 0.5 && !fit.clip,
     `390 px: one row inside the menu, no legend clipped, nothing scrolls sideways (page ${fit.doc}, row ${Math.round(fit.l)}–${Math.round(fit.r)} in ${Math.round(fit.mL)}–${Math.round(fit.mR)})`);
  shots.push(await P.shot('freedv-phone.png'));
  await P.send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await sleep(600);
  await P.closePicker();

  ok(await P.pick('USB'), 'back to USB');
  await sleep(1500);
  ok(!/FreeDV/.test(await P.vts()), `the line drops FreeDV ("${await P.vts()}")`);
  await P.openPicker();
  ok(!(await P.ev(`!!document.getElementById('mFreeDvSub')`)), 'the FREEDV row is gone');
  ok(await P.ev(`[...document.querySelectorAll('#mModeMenu .mBwSep')].some(s => s.textContent === 'BANDWIDTH')`), 'the BANDWIDTH row is back');
  await P.closePicker();
});

if (CLIP_LSB && fs.existsSync(CLIP_LSB)) {
  await scenario('40 m, LSB: the passband on the lower sideband', { clip: CLIP_LSB, centre: 7200000, freq: 7210000 }, async (P) => {
    ok(await P.pick('FreeDV'), 'FreeDV chosen at 7.210 MHz');
    ok(await P.until(`window.__fdv.out.some(m => m.type === 'bandwidth' && m.bandwidthLow === -2700 && m.bandwidthHigh === -50)`, 3000),
       'before the first report: the lower sideband (FreeDV\'s convention below 10 MHz)');
    ok(await P.until(`/^FreeDV 700D · -?\\d+ dB/.test(document.getElementById('vtsName')?.textContent || '')`, 45000), `in sync on LSB: "${await P.vts()}"`);
    ok(/"sb":"lsb"/.test(await P.ev(`JSON.stringify(window.__fdv.in[window.__fdv.in.length - 1])`) || ''), 'the report says lsb');
    ok(await P.ev(`document.getElementById('bwLoVal').textContent === '−2.7k' && document.getElementById('bwHiVal').textContent === '−50'`),
       `the passband is drawn below the dial (${await P.ev(`document.getElementById('bwLoVal').textContent + ' … ' + document.getElementById('bwHiVal').textContent`)})`);
    shots.push(await P.shot('freedv-lsb-desktop.png'));
  });
} else console.log('   (40 m LSB scenario not run — set VIBE_FREEDV_CLIP_LSB)');

await scenario('the sideband override: a USB station at 7.21 MHz', { clip: CLIP, centre: 7200000, freq: 7210000 }, async (P) => {
  ok(await P.pick('FreeDV'), 'FreeDV chosen at 7.210 MHz');
  ok(await P.until(`window.__fdv.in.some(m => m.sb === 'lsb' && m.sbSel === 'auto')`, 6000), 'AUTO: the server demodulates LSB here (the convention)');
  await P.openPicker();
  ok(await P.ev(`[...document.querySelectorAll('#mModeMenu .mBwSep')].some(s => s.textContent === 'FREEDV SIDEBAND')`), 'in the picker: the FREEDV SIDEBAND row, under the sub-modes');
  const sbs = await P.ev(`[...document.querySelectorAll('#mFreeDvSb .mModeOpt')].map(b => b.textContent).join(' ')`);
  ok(sbs === 'AUTO USB LSB', `its keys: ${sbs}`);
  ok(await P.until(`[...document.querySelectorAll('#mFreeDvSb .mModeOpt.on')].map(b => b.textContent).join() === 'AUTO'`, 3000), `AUTO lit, from the report ("${await P.litSb()}")`);
  await P.ev(`[...document.querySelectorAll('#mFreeDvSb .mModeOpt')].find(b => b.textContent === 'USB').click(); 1`);
  ok(await P.until(`window.__fdv.out.some(m => m.type === 'freedv_sub' && m.sb === 'usb')`, 2000), 'tapping USB sends freedv_sub sb usb');
  ok(await P.until(`[...document.querySelectorAll('#mFreeDvSb .mModeOpt.on')].map(b => b.textContent).join() === 'USB'`, 5000), `USB lit once the server's report says so ("${await P.litSb()}")`);
  ok(await P.until(`/^FreeDV 700D · -?\\d+ dB/.test(document.getElementById('vtsName')?.textContent || '')`, 45000), `in sync on USB at 7.21 MHz: "${await P.vts()}"`);
  ok(await P.ev(`document.getElementById('bwHiVal').textContent === '+2.7k' && document.getElementById('bwLoVal').textContent === '+50'`),
     `the passband is drawn above the dial (${await P.ev(`document.getElementById('bwLoVal').textContent + ' … ' + document.getElementById('bwHiVal').textContent`)})`);
  shots.push(await P.shot('freedv-sideband-desktop.png'));
  await P.send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  await P.closePicker(); await P.openPicker();
  const fit = JSON.parse(await P.ev(`JSON.stringify((() => { const m = document.getElementById('mModeMenu').getBoundingClientRect();
    const bs = [...document.querySelectorAll('#mFreeDvSb .mModeOpt')].map(b => b.getBoundingClientRect());
    return { doc: document.documentElement.scrollWidth, mL: m.left, mR: m.right, tops: [...new Set(bs.map(b => Math.round(b.top)))].length,
             l: Math.min(...bs.map(b => b.left)), r: Math.max(...bs.map(b => b.right)) }; })())`));
  ok(fit.doc <= 391 && fit.tops === 1 && fit.l >= fit.mL && fit.r <= fit.mR + 0.5, `390 px: the SIDEBAND row is one line inside the menu (page ${fit.doc})`);
  shots.push(await P.shot('freedv-sideband-phone.png'));
  await P.send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await sleep(600);
  await P.ev(`[...document.querySelectorAll('#mFreeDvSb .mModeOpt')].find(b => b.textContent === 'AUTO').click(); 1`);
  ok(await P.until(`[...document.querySelectorAll('#mFreeDvSb .mModeOpt.on')].map(b => b.textContent).join() === 'AUTO'`, 5000), 'AUTO goes back');
  ok(await P.until(`document.getElementById('bwLoVal').textContent === '−2.7k'`, 5000), 'and the passband returns below the dial');
  await P.closePicker();
});

if (HAVE_RADE) {
  await scenario('RADE V1: the key, the line in sync, the callsign once the over ends', { clip: CLIP, centre: 14200000, freq: 14230000 }, async (P) => {
    ok(await P.pick('FreeDV'), 'FreeDV chosen at 14.230 MHz (the RADE station)');
    ok(await P.until(`/^FreeDV RADE · -?\\d+ dB$/.test((document.getElementById('vtsName')?.textContent || '').trim())`, 45000), `in sync: "${await P.vts()}"`);
    // ★ the over ends 14 s into the clip's 20 s loop, with G4ABC's end-of-over frame
    ok(await P.until(`(document.getElementById('vtsName')?.textContent || '').trim() === 'FreeDV · last G4ABC'`, 45000), `once the over ends: "${await P.vts()}"`);
    await P.openPicker();
    await P.ev(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt')].find(b => b.textContent === 'RADE').click(); 1`);
    ok(await P.until(`window.__fdv.out.some(m => m.type === 'freedv_sub' && m.sub === 'RADE')`, 2000), 'tapping RADE sends freedv_sub RADE');
    ok(await P.until(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt.on')].map(b => b.textContent).join() === 'RADE'`, 5000), `RADE lit once the server's report says so ("${await P.lit()}")`);
    ok(await P.until(`/^FreeDV RADE( · -?\\d+ dB| · last G4ABC)$/.test((document.getElementById('vtsName')?.textContent || '').trim())`, 45000), `by hand: "${await P.vts()}"`);
    shots.push(await P.shot('freedv-rade.png'));
    await P.closePicker();
  });
  await scenario('no libcodec2, RADE built in (VIBE_CODEC2_DISABLE=1): the FreeDV key with RADE alone', { clip: CLIP, centre: 14200000, freq: 14230000, env: { VIBE_CODEC2_DISABLE: '1' } }, async (P) => {
    ok(await P.pick('FreeDV'), 'FreeDV offered and chosen');
    await P.openPicker();
    const subs = await P.ev(`[...document.querySelectorAll('#mFreeDvSub .mModeOpt')].map(b => b.textContent).join(' ')`);
    ok(subs === 'AUTO RADE', `the FREEDV row: ${subs}`);
    await P.closePicker();
    ok(await P.until(`/^FreeDV RADE · -?\\d+ dB$/.test((document.getElementById('vtsName')?.textContent || '').trim())`, 45000), `in sync: "${await P.vts()}"`);
  });
}

await scenario('FreeDV switched off (VIBE_FREEDV_DISABLE=1): no FreeDV key', { clip: CLIP, centre: 14200000, freq: 14210000, env: { VIBE_FREEDV_DISABLE: '1' } }, async (P) => {
  await P.openPicker();
  const keys = await P.keys();
  ok(keys.includes('USB') && !keys.includes('FreeDV'), `no FreeDV key (${keys.join(' ')})`);
  await P.closePicker();
});

console.log(`   screenshots: ${shots.join('  ')}`);
console.log(`\n${fail ? '✗' : '✓'} test-web-freedv: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
