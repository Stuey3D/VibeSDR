// test-web-spots-from-dab.mjs — BAND SPOTS CHOSEN WHILE IN DAB, on the served receiver page (V12, 2026-10-27). Stuart, beta22
// on the Pi 2: "switching to full band digital spots whilst in DAB mode doesnt retune to a band, and when i do try to tune all
// it does is move dab multiplex rather than jumping to the bands". DAB holds the dial (the page's SpectrumClient.tune() and
// the server both refuse a tune in it), so Band's tune went nowhere and the arrows still stepped the multiplex.
// A real vibeserver, a fake rtl_tcp replaying the UTC-anchored 20 m clip (`test-digi-spots clip`), headless MUTED Edge:
//   1. into DAB from the mode menu (the server says DAB ON);
//   2. DIGITAL SPOTS: BAND pressed IN DAB → the page leaves DAB (the server says DAB OFF, asked by the listener), the dial is
//      the landing band's centre (20 m), in USB, the readout names the band, the server runs Band, and spots arrive;
//   3. ▲ steps to 17 m (the dial moves to 17 m's centre) — not the next DAB block (no DAB asked for again);
//   4. Band ALREADY lit, DAB entered, Band pressed again → out of DAB onto a band, Band still lit (not switched off);
//   5. no exception on the page.
// ★ SILENT: --mute-audio and --disable-audio-output. Every wait has a deadline; children are killed in finally.
//   VIBESERVER_BIN=…/vibeserver VIBE_DIGI_BIN=…/test-digi-spots node --max-old-space-size=512 scripts/test-web-spots-from-dab.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '', DIGI = process.env.VIBE_DIGI_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !DIGI || !fs.existsSync(DIGI)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_DIGI_BIN)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 2048000, CENTRE = 14083700;   // ★ 20 m's Band centre (as test-web-spots); 2.048 MS/s, or DAB is not offered

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-spotsdab-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const CLIP = path.join(dir, '20m.cu8');
execFileSync(DIGI, ['clip', CLIP, String(RATE), String(CENTRE)], { stdio: 'ignore' });
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Spots from DAB test', locator: 'IO92nh', adminPass: 'wsd-admin',
  radios: [{ serial: 'wsd', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const kids = [];
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE), '--hf-utc'], { stdio: 'ignore' });
kids.push(rtl);
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
kids.push(srv);
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-spotsdab-edge-'));
const log = () => fs.readFileSync(logPath, 'utf8');
const count = (re) => (log().match(re) || []).length;
// ★ Abort if anything we started grows past 1 GB (a runaway is a fault, not a slow test).
const rssGuard = setInterval(() => {
  for (const k of kids) {
    if (!k.pid) continue;
    try { const kb = Number(execFileSync('ps', ['-o', 'rss=', '-p', String(k.pid)]).toString().trim()) || 0;
          if (kb > 1024 * 1024) { console.log(`   FAIL pid ${k.pid} RSS ${Math.round(kb / 1024)} MB > 1 GB — aborting`); process.exitCode = 1; for (const x of kids) { try { x.kill('SIGKILL'); } catch {} } process.exit(1); } } catch {}
  }
}, 2000);
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`, { signal: AbortSignal.timeout(5000) }).then((r) => r.json());
  const band = (id) => (st.spotBands || []).find((b) => b.id === id);
  ok(st.spotsBand === true && band('20m') && band('17m'), `vibeserver.json: spotsBand ${st.spotsBand}, ${(st.spotBands || []).length} bands`);
  ok(st.dab === true, `vibeserver.json: DAB offered (${st.dab})`);

  const edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
  kids.push(edge);
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  if (!page) throw new Error('no browser page');
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await Promise.race([new Promise((r) => { ws.onopen = r; }), sleep(10000)]);
  const send = (method, params = {}) => Promise.race([new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); }), sleep(15000).then(() => ({}))]);
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (x, ms) => { for (let t = 0; t < ms; t += 250) { if (await ev(x)) return true; await sleep(250); } return !!(await ev(x)); };
  const trustedClick = async (sel) => {
    const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || {x:0,y:0,width:0,height:0})`));
    const x = r.x + r.width / 2, y = r.y + r.height / 2;
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
  };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  await until(`!!document.querySelector('#audioGate button')`, 15000);
  await trustedClick('#audioGate button');
  ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000),
     'START RADIO pressed');
  await sleep(2500);

  const freqHz = async () => {
    const main = await ev(`document.getElementById('mFreq').textContent + '|' + document.getElementById('mFreqFine').textContent`);
    const [a, b] = String(main).split('|'); return Math.round((parseFloat(a) * 1e6) + (parseInt(b || '0', 10) * 10));
  };
  const chan = () => ev(`(() => { const c = document.getElementById('mChan'); return c && !c.hidden ? c.textContent : ''; })()`);
  const intoDab = async () => {
    await ev(`document.getElementById('mMode').click(); 1`);
    await until(`[...document.querySelectorAll('.mModeOpt')].some(b => b.textContent === 'DAB')`, 5000);
    await ev(`[...document.querySelectorAll('.mModeOpt')].find(b => b.textContent === 'DAB').click(); 1`);
    return until(`document.getElementById('decTitle')?.textContent === 'DAB'`, 10000);
  };
  const dabOnLines = () => count(/\[DAB\] ON asked by/g);
  const dabOffLines = () => count(/\[DAB\] OFF asked by/g);
  const waitLog = async (fn, ms) => { for (let t = 0; t < ms; t += 250) { if (fn()) return true; await sleep(250); } return fn(); };

  console.log('── 1. into DAB ──');
  ok(await intoDab(), 'the DAB box is up (mode menu → DAB)');
  ok(await waitLog(() => dabOnLines() >= 1, 8000), 'the server says DAB ON');
  await sleep(2500);   // ★ let the server settle on the multiplex (its config arrives with the block's centre)
  const dabHz = await freqHz();
  ok(dabHz > 170e6 && dabHz < 240e6, `the dial is a Band III multiplex (${dabHz})`);

  console.log('── 2. DIGITAL SPOTS: BAND pressed in DAB ──');
  ok(await until(`!document.getElementById('spotsBandBtn').hidden && !document.getElementById('spotsBandBtn').classList.contains('dis')`, 5000), 'Band is offered in DAB');
  await ev(`document.getElementById('spotsBandBtn').click(); 1`);
  ok(await waitLog(() => dabOffLines() >= 1, 8000), 'the server says DAB OFF (asked by this listener)');
  ok(await until(`document.getElementById('decTitle')?.textContent !== 'DAB'`, 8000), `the DAB box has gone ("${await ev(`document.getElementById('decTitle')?.textContent`)}")`);
  ok(await until(`/20 m BAND/.test((document.getElementById('mChan')||{}).textContent||'')`, 10000), `the readout names the band: "${await chan()}"`);
  const f20 = await freqHz();
  ok(Math.abs(f20 - band('20m').hz) <= 20, `the dial is 20 m's centre (${f20} vs ${band('20m').hz})`);
  ok(/USB/i.test(await ev(`document.getElementById('mMode')?.textContent || ''`)), `in USB ("${await ev(`document.getElementById('mMode')?.textContent`)}")`);
  ok(await until(`document.getElementById('spotsBandBtn').classList.contains('on')`, 3000), 'Band is lit');
  ok(await waitLog(() => /digital spots: Band started/.test(log()), 8000), 'the server runs Band');
  ok(await until(`[...document.querySelectorAll('#spotList .sres.spot .mode')].some(m => m.textContent.includes('FT8'))`, 60000),
     `spots arrive (${await ev(`[...document.querySelectorAll('#spotList .sres.spot')].length`)} rows)`);

  console.log('── 3. the arrows step bands, not multiplexes ──');
  const onsBefore = dabOnLines();
  await trustedClick('#mVfoUp');
  ok(await until(`/17 m BAND/.test((document.getElementById('mChan')||{}).textContent||'')`, 6000), `▲: 17 m ("${await chan()}")`);
  const f17 = await freqHz();
  ok(Math.abs(f17 - band('17m').hz) <= 20, `the dial is 17 m's centre (${f17} vs ${band('17m').hz})`);
  await sleep(1000);
  ok(dabOnLines() === onsBefore && (await ev(`document.getElementById('decTitle')?.textContent`)) !== 'DAB', 'no DAB block was asked for');

  console.log('── 4. Band already lit, DAB entered, Band pressed again ──');
  ok(await intoDab(), 'back into DAB with Band lit');
  ok(await waitLog(() => dabOnLines() > onsBefore, 8000), 'the server says DAB ON');
  await sleep(2500);
  const offsBefore = dabOffLines();
  await ev(`document.getElementById('spotsBandBtn').click(); 1`);
  ok(await waitLog(() => dabOffLines() > offsBefore, 8000), 'the press leaves DAB');
  ok(await until(`/(17|20) m BAND/.test((document.getElementById('mChan')||{}).textContent||'') && document.getElementById('spotsBandBtn').classList.contains('on')`, 10000),
     `onto a band, Band still lit ("${await chan()}")`);
  const fb = await freqHz();
  ok([band('17m').hz, band('20m').hz].some((h) => Math.abs(fb - h) <= 20), `the dial is that band's centre (${fb})`);

  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  ws.close();
} catch (e) {
  ok(false, `the test threw: ${e?.message || e}`);
} finally {
  clearInterval(rssGuard);
  for (const k of kids.reverse()) { try { k.kill('SIGTERM'); } catch {} }
  await sleep(500);
  for (const k of kids) { try { k.kill('SIGKILL'); } catch {} }
  fs.rmSync(dir, { recursive: true, force: true });
  fs.rmSync(prof, { recursive: true, force: true });
}
console.log(`\n${fail ? '✗' : '✓'} test-web-spots-from-dab: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
