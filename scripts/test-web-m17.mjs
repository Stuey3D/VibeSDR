// test-web-m17.mjs — V12 M17 DIGITAL VOICE in the web client, through a real vibeserver and THIS tree's built page (web/dist,
// served through CDP interception in place of the binary's own — test-web-freedv.mjs's way, so the page under test is the
// one just built and the server binary need not be rebuilt for a client change).
//   A fake rtl_tcp replays test-m17's synthetic clip (our own M17 transmitter from the spec at 433.425 MHz: a voice over from
//   G4ABC with META text "VIBE TEST", then an SMS to M0XYZ, on a 10 s loop). Headless Edge, SILENT (--mute-audio
//   --disable-audio-output), then:
//   • the mode picker has an M17 key, beside NFM, its name in the tooltip;
//   • choosing it sends {"type":"tune",mode:"m17"} and asks for the report (m17_status);
//   • the VTS line says "M17 · searching" or names the station, "M17 · G4ABC" once in sync, "M17 · last G4ABC" between
//     overs; the line's ticker carries the META text and then the SMS ("SMS G4ABC → M0XYZ: Hello from VibeServer");
//   • in the picker: no BANDWIDTH row (the receiver's channel is the server's);
//   • 390 px: nothing scrolls sideways;
//   • leaving M17 (NFM): the line drops its M17 text and the BANDWIDTH row comes back;
//   • a server whose libcodec2 did not load (VIBE_CODEC2_DISABLE=1): no M17 key at all.
//   Screenshots: VIBE_SHOT_DIR (default the temp dir).
//   node scripts/build-web.mjs   (then)
//   VIBESERVER_BIN=…/vibeserver VIBE_M17_CLIP=m17.cu8 node scripts/test-web-m17.mjs   (test-m17 clip m17.cu8 288000 10 20)
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_M17_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
const DIST = path.join(root, 'web/dist');
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_M17_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
if (!fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.log('   (not run — build the page first: node scripts/build-web.mjs)'); process.exit(3); }
const RATE = 288000;

/** One server + one page; `body(p)` drives it. */
async function scenario(title, { clip, centre, freq, env: extraEnv = {} }, body) {
  console.log(`── ${title} ──`);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webm17-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'M17 web test', locator: 'IO92nh', adminPass: 'wm17-admin',
    radios: [{ serial: 'wm17', label: 'Test radio', enabled: true, configured: true }] }));
  const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, ...extraEnv, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(freq), '--mode', 'nfm', '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webm17-edge-'));
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
    // ★ Before the page loads: what the page SENDS (tune / bandwidth / m17_status) and the m17 reports it RECEIVES.
    await send('Page.addScriptToEvaluateOnNewDocument', { source: `(() => {
      window.__m17 = { out: [], in: [] };
      const S = WebSocket.prototype.send;
      WebSocket.prototype.send = function (d) { try { if (typeof d === 'string' && /"(tune|bandwidth|m17_status)"/.test(d)) window.__m17.out.push(JSON.parse(d)); } catch {} return S.call(this, d); };
      const add = WebSocket.prototype.addEventListener;
      const wrap = (f) => function (e) { if (typeof e.data === 'string' && e.data.startsWith('{"type":"m17"')) { try { window.__m17.in.push(JSON.parse(e.data)); } catch {} } return f.call(this, e); };
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
      keys: () => ev(`JSON.stringify([...document.querySelectorAll('#mModeMenu .mModeGrid > .mModeOpt')].map(b => b.textContent))`).then(JSON.parse),
      pick: async (legend) => { await P.openPicker(); return ev(`(() => { const b = [...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === ${JSON.stringify(legend)}); if (b) b.click(); return !!b; })()`); },
      vts: () => ev(`(document.getElementById('vtsName')?.textContent || '').trim()`),
      // ★ in line mode the ticker (#vtsRtInner) carries the whole line — name, then text; its raw text is on the dataset
      text: () => ev(`(document.getElementById('vtsRtInner')?.dataset.rt || '').trim()`),
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
const CENTRE = 433400000, STATION = CENTRE + 25000;

await scenario('70 cm: the key, the line, its text', { clip: CLIP, centre: CENTRE, freq: STATION }, async (P) => {
  await P.openPicker();
  const keys = await P.keys();
  const i = keys.indexOf('M17');
  ok(i > 0 && keys[i - 1] === 'NFM', `an M17 key, beside NFM (${keys.join(' ')})`);
  ok(await P.ev(`[...document.querySelectorAll('#mModeMenu .mModeOpt')].find(b => b.textContent === 'M17')?.title === 'M17 digital voice'`), 'its name in the tooltip');
  await P.closePicker();
  await P.ev(`window.__m17.out.length = 0; 1`);
  ok(await P.pick('M17'), 'M17 chosen');
  ok(await P.until(`window.__m17.out.some(m => m.type === 'tune' && m.mode === 'm17')`, 3000), 'the page sends {"type":"tune",mode:"m17"}');
  ok(await P.until(`window.__m17.out.some(m => m.type === 'm17_status')`, 3000), 'and asks for the report (m17_status)');
  ok(await P.until(`/^M17 · /.test(document.getElementById('vtsName')?.textContent || '')`, 6000), `the VTS line names M17 ("${await P.vts()}")`);
  ok(await P.until(`(document.getElementById('vtsName')?.textContent || '').trim() === 'M17 · G4ABC'`, 25000), `in sync: "${await P.vts()}"`);
  ok(await P.until(`/VIBE TEST/.test(document.getElementById('vtsRtInner')?.dataset.rt || '')`, 25000), `the META text in the line's text ("${await P.text()}")`);
  shots.push(await P.shot('m17-desktop.png'));
  ok(await P.until(`/SMS G4ABC → M0XYZ: Hello from VibeServer/.test(document.getElementById('vtsRtInner')?.dataset.rt || '')`, 25000), `the SMS ("${await P.text()}")`);
  ok(await P.until(`(document.getElementById('vtsName')?.textContent || '').trim() === 'M17 · last G4ABC'`, 25000), `between overs: "${await P.vts()}"`);
  const rep = await P.ev(`JSON.stringify(window.__m17.in.filter(r => r.state === 'sync').at(-1) || null)`);
  ok(/"src":"G4ABC"/.test(rep || ''), `the server's report: ${rep}`);

  await P.openPicker();
  ok(await P.ev(`[...document.querySelectorAll('#mModeMenu .mBwSep')].every(s => s.textContent !== 'BANDWIDTH')`), 'in the picker: no BANDWIDTH row (the channel is the server\'s)');
  await P.closePicker();

  await P.send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const doc = await P.ev(`document.documentElement.scrollWidth`);
  ok(doc <= 391, `390 px: nothing scrolls sideways (page ${doc} px), the line "${await P.vts()}"`);
  shots.push(await P.shot('m17-phone.png'));
  await P.send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await sleep(600);

  ok(await P.pick('NFM'), 'back to NFM');
  await sleep(1500);
  // ★ the ticker may keep its last text while hidden — what matters is that nothing of M17 is SHOWN
  const shown = await P.ev(`!!document.getElementById('vtsRt')?.classList.contains('show') && !!document.getElementById('vts')?.classList.contains('show')`);
  ok(!/M17/.test(await P.vts()) && (!shown || !/M17|SMS|VIBE TEST/.test(await P.text())),
     `the line drops M17 ("${await P.vts()}"; the ticker ${shown ? `shows "${await P.text()}"` : 'hidden'})`);
  await P.openPicker();
  ok(await P.ev(`[...document.querySelectorAll('#mModeMenu .mBwSep')].some(s => s.textContent === 'BANDWIDTH')`), 'the BANDWIDTH row is back');
  await P.closePicker();
});

await scenario('no libcodec2 (VIBE_CODEC2_DISABLE=1): no M17 key', { clip: CLIP, centre: CENTRE, freq: STATION, env: { VIBE_CODEC2_DISABLE: '1' } }, async (P) => {
  await P.openPicker();
  const keys = await P.keys();
  ok(keys.includes('NFM') && !keys.includes('M17'), `no M17 key (${keys.join(' ')})`);
  await P.closePicker();
});

console.log(`   screenshots: ${shots.join('  ')}`);
console.log(`\n${fail ? '✗' : '✓'} test-web-m17: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
