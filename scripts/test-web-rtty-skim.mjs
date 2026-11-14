// test-web-rtty-skim.mjs — V12 THE RTTY SKIMMER IN THE WEB CLIENT, through a real vibeserver and the REAL built web page.
//   A fake rtl_tcp replays the synthetic HF clip (`test-rtty-skimmer clip`: four RTTY stations of different settings around
//   14.080 MHz); the server's dial at the clip's centre. Headless Edge then:
//   • RTTY offers VIEW (vibeserver.json "rttyskim": true); SKIMMER hides the manual rows and shows SEGMENT 48 kHz;
//   • every station listed lowest first with its OWN settings, a row read "waiting for parameter detection" first;
//   • a row opens its CARD — the settings at the top, the whole message — and CLOSE goes back;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_RTTY_CLIP=…/clip.cu8 node scripts/test-web-rtty-skim.mjs
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
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_RTTY_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_RTTY_CLIP)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000, CENTRE = 14080000;
// ★ The clip's stations (vibeserver/test-rtty-skimmer.cpp mixedFour): offset, call (null = none), the row's short settings.
const STATIONS = [[-9000, 'G4ABC', '45/170'], [-3000, 'DL1XYZ', '45/170 R'], [3000, null, '50/450'], [10000, 'PA3XYZ', '75/850']];

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webrttysk-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'RTTY skimmer web test', locator: 'IO92nh', adminPass: 'wrttysk-admin',
  radios: [{ serial: 'wrttysk', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE)], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
// ★ The dial at the clip's centre in USB: the SKIMMER sees all five; a tap on EA7XX's row then tunes it into the middle of
//   the passband for ONE SIGNAL — the way a listener goes from the list to one station.
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webrttysk-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.rttyskim === true, `vibeserver.json: rttyskim ${JSON.stringify(st.rttyskim)}`);

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

  console.log('── DECODERS: RTTY, VIEW › SKIMMER (48 kHz) ──');
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     document.getElementById('decodersPanel').classList.add('open'); return false; })()`);
  await sleep(500);
  await ev(`document.querySelector('#decodersPanel [data-dec="rtty"]').click()`);
  ok(await until(`!document.getElementById('rttySettings').hidden && !document.getElementById('rttyView').closest('.mrow').hidden`, 5000),
     'RTTY chosen: its settings, with the VIEW row (this server has the skimmer)');
  ok(await ev(`!document.getElementById('cwList').classList.contains('on')`), 'ONE SIGNAL: no list box yet');
  // ★ a record of every row text drawn, so "waiting for parameter detection" is seen even if it is brief
  await ev(`(() => { window.__waitSeen = 0; new MutationObserver(() => { if (/waiting for parameter detection/.test(document.getElementById('cwRows').textContent)) window.__waitSeen++; })
                     .observe(document.getElementById('cwRows'), { childList: true, subtree: true }); return 1; })()`);
  await ev(`document.querySelector('#rttyView [data-rttyview="skim"]').click()`);
  ok(await until(`!document.getElementById('rttyWidthRow').hidden && document.querySelector('#rttyWidth .on')?.dataset.rttywidth === '48000'`, 3000),
     'SKIMMER shows the segment row, 48 kHz chosen');
  ok(await ev(`[...document.querySelectorAll('#rttySettings .mrow:not(.skimrow)')].every(r => getComputedStyle(r).display === 'none')`),
     'SKIMMER hides the manual rows (the skimmer finds each signal\'s settings)');
  ok(await until(`document.getElementById('decTitle').textContent === 'RTTY SKIMMER' && document.getElementById('cwList').classList.contains('on')`, 3000),
     'the box says RTTY SKIMMER and shows the list');
  const settings = await shot('rttysk-settings-desktop.png');
  await ev(`document.getElementById('decodersPanel').classList.remove('open')`);
  const allSet = `(() => { const w = [...document.querySelectorAll('#cwRows .cwr .w')].map(x => x.textContent);
                          return ${JSON.stringify(STATIONS.map((s) => s[2]))}.every(s => w.includes(s)); })()`;
  ok(await until(allSet, 60000), `every station listed with its own settings (${await ev(`[...document.querySelectorAll('#cwRows .cwr .w')].map(x => x.textContent).join(' | ')`)})`);
  // ★ a row names its call once it has been heard calling twice — give the slowest a moment
  await until(`${JSON.stringify(STATIONS.map((s) => s[1]).filter(Boolean))}.every(c => [...document.querySelectorAll('#cwRows .cwr .c')].some(x => x.textContent === c))`, 30000);
  ok((await ev(`window.__waitSeen`)) > 0, 'a row read "RTTY detected — waiting for parameter detection" before its settings were found');
  await sleep(1500);
  const rows = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#cwRows .cwr')].map(r => ({ f: parseFloat(r.querySelector('.f').textContent), c: r.querySelector('.c').textContent, w: r.querySelector('.w').textContent, tx: r.querySelector('.tx').textContent })))`));
  ok(rows.length === 4 && rows.every((r, i) => i === 0 || r.f >= rows[i - 1].f), `four rows, lowest frequency first (${rows.map((r) => r.f).join(' ')})`);
  for (const [off, call, w] of STATIONS) {
    const r = rows.find((x) => Math.abs(x.f * 1e3 - (CENTRE + off)) <= 15);
    ok(!!r && r.w === w && (!call || r.c === call), `${((CENTRE + off) / 1e3).toFixed(2)} kHz: ${r ? r.w : '—'} ${r ? r.c : ''} "${r ? r.tx.slice(-36) : ''}"`);
  }
  const s2 = await shot('rttysk-list-desktop.png');

  console.log('── a row opens its card: the settings at the top, the whole message ──');
  await ev(`[...document.querySelectorAll('#cwRows .cwr')].find(r => r.querySelector('.c').textContent === 'G4ABC').click()`);
  ok(await until(`document.getElementById('rttyCard').classList.contains('on') && !document.getElementById('cwList').offsetParent`, 3000), 'the card replaces the list');
  ok(await ev(`document.getElementById('rttyCardSet').textContent`) === '45 baud / 170 Hz / Normal', `its settings at the top (${await ev(`document.getElementById('rttyCardSet').textContent`)})`);
  ok(await until(`/CQ CQ CQ DE G4ABC G4ABC G4ABC PSE K/.test(document.getElementById('rttyCardText').textContent)`, 10000),
     `the whole message ("${(await ev(`document.getElementById('rttyCardText').textContent`) || '').slice(0, 60)}…")`);
  const s3 = await shot('rttysk-card-desktop.png');
  await ev(`document.getElementById('rttyCardClose').click()`);
  ok(await until(`!document.getElementById('rttyCard').classList.contains('on') && !!document.getElementById('cwList').offsetParent`, 3000), 'CLOSE: back to the list');

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(2500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    box: document.getElementById('decBox').getBoundingClientRect().right,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#cwRows .cwr, #cwSpots .cws')].map(r => r.getBoundingClientRect().right)),
    rows: document.querySelectorAll('#cwRows .cwr').length })`));
  ok(fit.doc <= 391 && fit.box <= 391 && fit.rowMax <= 391, `nothing scrolls sideways (page ${fit.doc}, box ends ${Math.round(fit.box)}, rows end ${Math.round(fit.rowMax)} px)`);
  ok(fit.rows >= 4, `the list is still there (${fit.rows} rows)`);
  const s4 = await shot('rttysk-list-phone.png');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[settings, s2, s3, s4].join('  ')}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
}
console.log(`\n${fail ? '✗' : '✓'} test-web-rtty-skim: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
