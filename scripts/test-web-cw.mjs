// test-web-cw.mjs — V12 CW IN THE WEB CLIENT, through a real vibeserver and the REAL built web page (2026-10-25).
//   A fake rtl_tcp replays the synthetic HF clip (`cw_bench clip`: five CW stations around 7.030 MHz, each calling
//   "CQ CQ DE <call> <call> K"); the server's dial is on G4ABC in CW. Headless Edge then:
//   • DECODERS shows CW (this server's vibeserver.json says "cw": true), and the gallery has a CW chip;
//   • ONE SIGNAL: G4ABC's text arrives in the decoder box, its row in the list is the primary, and it is spotted;
//   • SKIMMER (48 kHz): every station listed with its call, and every one spotted at its own kHz;
//   • at PHONE WIDTH (390 px) the rows wrap and nothing scrolls sideways; no exception on the page.
//   Screenshots (desktop and 390 px, both views) are written to VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_CW_CLIP=…/clip.cu8 node scripts/test-web-cw.mjs
//   (make the clip: cw_bench clip clip.cu8 960000 40 — scripts/test-cw.sh does)
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_CW_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_CW_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000, CENTRE = 7030000;
// ★ The clip's stations (tools/cw-bench/cw_bench.cpp, clip()): offset from CENTRE, call.
const STATIONS = [[600, 'G4ABC'], [-7200, 'DL1XYZ'], [9800, 'EA3QP'], [16300, 'OK1RR'], [-15600, 'K1ABC']];

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webcw-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'CW web test', locator: 'IO92nh', adminPass: 'wcw-admin',
  radios: [{ serial: 'wcw', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
// ★ The dial at the clip's centre in CW: every station is off it, so the SKIMMER sees all five; a tap on G4ABC's row then
//   tunes to it for ONE SIGNAL — the way a listener goes from the list to one station.
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'cwu', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webcw-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.cw === true, `vibeserver.json: cw ${JSON.stringify(st.cw)}`);

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

  console.log('── DECODERS: the CW button, SKIMMER (48 kHz) ──');
  ok(await until(`!document.querySelector('#decodersPanel [data-dec="cw"]').hidden`, 8000), 'the CW button is offered (this server has CW)');
  // ★ open DECODERS as a listener does (the mode card's door), else straight in
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     document.getElementById('decodersPanel').classList.add('open'); return false; })()`);
  await sleep(500);
  await ev(`document.querySelector('#decodersPanel [data-dec="cw"]').click()`);
  ok(await until(`!document.getElementById('cwSettings').hidden && document.getElementById('cwList').classList.contains('on')`, 5000),
     'CW chosen: its settings (ONE SIGNAL / SKIMMER) and the signals list are shown');
  await ev(`document.querySelector('#cwView [data-cwview="skim"]').click()`);
  ok(await until(`!document.getElementById('cwWidthRow').hidden && document.querySelector('#cwWidth .on')?.dataset.cwwidth === '48000'`, 3000),
     'SKIMMER shows the segment row, 48 kHz chosen');
  const settings = await shot('cw-settings-desktop.png');
  // ★ the panel closed, so the decoder box is what the screenshots show
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  const allSpotted = `(() => { const t = [...document.querySelectorAll('#cwSpots .cws')].map(r => r.textContent).join('|');
                               return ${JSON.stringify(STATIONS.map(([, c]) => c))}.every(c => t.includes(c)); })()`;
  ok(await until(allSpotted, 60000), `every station spotted (${await ev(`[...document.querySelectorAll('#cwSpots .cws .c')].map(c => c.textContent).join(' ')`)})`);
  await sleep(1500);   // ★ the list goes once a second: let the row that names the last call arrive
  const rows = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#cwRows .cwr')].map(r => ({ f: r.querySelector('.f').textContent, c: r.querySelector('.c').textContent, tx: r.querySelector('.tx').textContent })))`));
  for (const [off, call] of STATIONS) {
    const r = rows.find((x) => x.c === call);
    ok(!!r && Math.abs(parseFloat(r.f) * 1e3 - (CENTRE + off)) <= 60 && r.tx.includes(`${call} ${call}`),
       `${call} listed at ${r ? r.f : '—'} kHz (sent at ${((CENTRE + off) / 1e3).toFixed(2)}), its text "${r ? r.tx.slice(-28) : ''}"`);
  }
  ok(!document_has_false(rows), `no row with an invented call (${rows.filter((x) => x.c !== '—').map((x) => x.c).join(' ')})`);
  const s2 = await shot('cw-skimmer-desktop.png');

  console.log('── a tap on G4ABC\'s row tunes to it; ONE SIGNAL ──');
  await ev(`[...document.querySelectorAll('#cwRows .cwr')].find(r => r.querySelector('.c').textContent === 'G4ABC').click()`);
  await sleep(800);
  await ev(`document.querySelector('#cwView [data-cwview="one"]').click()`);
  ok(await until(`/G4ABC G4ABC/.test(document.getElementById('decText').textContent)`, 45000),
     `ONE SIGNAL: G4ABC's text in the decoder box ("${(await ev(`document.getElementById('decText').textContent`) || '').trim().slice(-48)}")`);
  ok(await until(`!!document.querySelector('#cwRows .cwr.primary')`, 10000), 'its row is the primary (highlighted)');
  const one = await ev(`(document.querySelector('#cwRows .cwr.primary')||{}).textContent || ''`);
  ok(/7030\.6\d/.test(one), `the primary row is G4ABC's carrier: ${one.slice(0, 90)}`);
  ok(await until(`[...document.querySelectorAll('#cwSpots .cws')].some(r => /G4ABC/.test(r.textContent))`, 30000), 'G4ABC spotted in SPOTS');
  const s1 = await shot('cw-one-signal-desktop.png');

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    box: document.getElementById('decBox').getBoundingClientRect().right,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#cwRows .cwr, #cwSpots .cws')].map(r => r.getBoundingClientRect().right)),
    rows: document.querySelectorAll('#cwRows .cwr').length })`));
  ok(fit.doc <= 391 && fit.box <= 391 && fit.rowMax <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)}, rows end ${Math.round(fit.rowMax)} px)`);
  ok(fit.rows >= 1, `the list is still there (${fit.rows} rows)`);
  const s3 = await shot('cw-one-signal-phone.png');
  await ev(`document.querySelector('#cwView [data-cwview="skim"]').click()`);
  ok(await until(`document.querySelectorAll('#cwRows .cwr').length >= 5`, 30000), 'SKIMMER again at phone width: the stations listed');
  const fit2 = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#cwRows .cwr, #cwSpots .cws')].map(r => r.getBoundingClientRect().right)) })`));
  ok(fit2.doc <= 391 && fit2.rowMax <= 391, `…and nothing scrolls sideways (page ${fit2.doc}, rows end ${Math.round(fit2.rowMax)} px)`);
  const s4 = await shot('cw-skimmer-phone.png');

  console.log('── the gallery has a CW chip ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  await ev(`document.getElementById('galleryBtn').click()`);
  await sleep(800);
  const chips = await ev(`[...document.querySelectorAll('#galFilter [data-kind]')].map(b => b.textContent).join(' ')`);
  ok(/\bFT8 CW\b/.test(chips), `the chips: ${chips}`);
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[settings, s2, s1, s3, s4].join('  ')}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
}
function document_has_false(rows) { return rows.some((x) => x.c !== '—' && !STATIONS.some(([, c]) => c === x.c)); }
console.log(`\n${fail ? '✗' : '✓'} test-web-cw: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
