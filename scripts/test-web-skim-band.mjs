// test-web-skim-band.mjs — V12 beta90 THE SKIMMERS' BAND PICKER in the web client's decoder box (Stuart 2026-11-21: "cw/rtty
// skimmer could have a band selection in the decoder box header and the normal tune buttons could fine tune it if
// needed"), through a real vibeserver and the REAL built web page. The fake rtl_tcp replays test-rtty-skimmer's clip
// around 14.080 MHz (an HF radio). Headless Edge then:
//   • vibeserver.json carries skimBands (cw / rtty / psk for the server's region);
//   • RTTY › SKIMMER shows BAND — only bands whose whole window this radio tunes — reading "20 m · tuned" at 14.080 (48 kHz:
//     20 m's rule-centre is 14.094); picking 20 m puts the dial at segment bottom + 24 kHz; at 24 kHz the same pick = + 12;
//   • fine-tuning with the frequency box (a normal tune) does not snap back: "20 m · tuned";
//   • CW › SKIMMER: BAND too, a pick at 96 kHz = segment bottom + 48 kHz; ONE SIGNAL hides it; no exception on the page.
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_RTTY_CLIP=…/clip.cu8 node scripts/test-web-skim-band.mjs
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

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webskimband-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Skimmer band web test', locator: 'IO92nh', adminPass: 'wskimband-admin',
  radios: [{ serial: 'wskimband', label: 'Test radio', enabled: true, configured: true }] }));
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
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webskimband-edge-'));
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

  const H2 = `http://127.0.0.1:${port}`;
  const sb = st.skimBands;
  ok(!!sb && Array.isArray(sb.cw) && Array.isArray(sb.rtty) && Array.isArray(sb.psk) && sb.region >= 1, `vibeserver.json: skimBands (region ${sb && sb.region}, ${sb ? sb.cw.length : 0} CW bands)`);
  const r20 = sb.rtty.find((b) => b.id === '20m'), c20 = sb.cw.find((b) => b.id === '20m');
  const dial = async () => Math.round(await ev(`window.__vibeWf()?.spec.frequency || 0`));
  const enter = async (v) => {
    await ev(`(() => { const p = document.getElementById('freqPanel'); if (!p || !p.classList.contains('open')) document.getElementById('mFreqBox').click(); })()`);
    await sleep(150);
    await ev(`(() => { const i = document.getElementById('freqInput'); i.value = ${JSON.stringify(v)}; i.dispatchEvent(new Event('input')); document.getElementById('freqGo').click(); })()`);
    await sleep(600);
    await ev(`document.getElementById('freqPanel')?.classList.remove('open')`);
  };
  const pick = async (kind, id) => { if (process.env.DBG) console.log('PICK', kind, id, await ev(`String(!!document.getElementById('rttySettings'))`)); await ev(`(() => { const s = document.getElementById('${kind}Band'); s.value = ${JSON.stringify(id)}; s.dispatchEvent(new Event('change')); })()`); await sleep(800); };
  const label = (kind) => ev(`(() => { const s = document.getElementById('${kind}Band'); return s.options[s.selectedIndex]?.textContent || ''; })()`);

  console.log('── RTTY › SKIMMER: BAND ──');
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^DECODERS/.test(x.textContent.trim()) && x.offsetParent); if (b) { b.click(); return true; }
                     document.getElementById('decodersPanel').classList.add('open'); return false; })()`);
  await sleep(500);
  await ev(`document.querySelector('#decodersPanel [data-dec="rtty"]').click()`);
  await sleep(500);
  ok(await ev(`document.getElementById('rttyBandRow').hidden`), 'ONE SIGNAL: no BAND row');
  await ev(`document.querySelector('#rttyView [data-rttyview="skim"]').click()`);
  ok(await until(`!document.getElementById('rttyBandRow').hidden && document.getElementById('rttyBand').options.length > 0`, 4000), 'SKIMMER: the BAND row is drawn');
  const opts = JSON.parse(await ev(`JSON.stringify([...document.getElementById('rttyBand').options].map(o => o.value))`));
  ok(opts.includes('20m') && opts.every((v) => v === '' || sb.rtty.some((b) => b.id === v)), `…only the server's RTTY bands (${opts.join(',')})`);
  ok((await label('rtty')) === '20 m \u00b7 tuned' && (await dial()) === 14080000, `at 14.080 (48 kHz) it reads "${await label('rtty')}" — not 20 m's rule-centre, never snapped`);
  await pick('rtty', '20m');
  ok((await dial()) === r20.lo + 24000 && (await label('rtty')) === '20 m', `picking 20 m: the dial goes to ${(r20.lo + 24000) / 1e3} kHz (segment bottom + 24 kHz) — now ${await dial()}`);
  await ev(`document.querySelector('#rttyWidth [data-rttywidth="24000"]').click()`); await sleep(400);
  ok((await label('rtty')) === '20 m \u00b7 tuned', 'at 24 kHz the same dial is no longer the rule-centre: "20 m · tuned" (the dial is not moved by a width change)');
  await pick('rtty', '20m');
  ok((await dial()) === r20.lo + 12000, `…picking 20 m again at 24 kHz: ${(r20.lo + 12000) / 1e3} kHz (+ 12) — now ${await dial()}`);
  if (process.env.DBG) console.log('BEFORE-ENTER', await ev(`String(!!document.getElementById('rttySettings'))`));
  await enter(((r20.lo + 14000) / 1e6).toFixed(4));   // the frequency box takes MHz
  ok((await dial()) === r20.lo + 14000 && (await label('rtty')) === '20 m \u00b7 tuned', `fine-tuned with the frequency box to ${(r20.lo + 14000) / 1e3} kHz: "${await label('rtty')}", no snap back`);
  const s1 = await shot('skimband-rtty-desktop.png');

  console.log('── CW › SKIMMER: BAND ──');
  await ev(`(() => { const p = document.getElementById('decodersPanel'); if (!p.classList.contains('open')) p.classList.add('open'); })()`);
  await ev(`document.querySelector('#decodersPanel [data-dec="cw"]').click()`);
  await sleep(500);
  await ev(`document.querySelector('#cwView [data-cwview="skim"]').click()`);
  await ev(`document.querySelector('#cwWidth [data-cwwidth="96000"]').click()`);
  ok(await until(`!document.getElementById('cwBandRow').hidden`, 4000), 'CW SKIMMER: the BAND row is drawn');
  if (c20) {
    await pick('cw', '20m');
    ok((await dial()) === c20.lo + 48000 && (await label('cw')) === '20 m', `20 m CW at 96 kHz: ${(c20.lo + 48000) / 1e3} kHz (segment bottom + 48) — now ${await dial()}`);
  }
  await ev(`document.querySelector('#cwView [data-cwview="one"]').click()`); await sleep(300);
  ok(await ev(`document.getElementById('cwBandRow').hidden`), 'ONE SIGNAL hides it again');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshot: ${s1}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
}
console.log(`\n${fail ? '✗' : '✓'} test-web-skim-band: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
