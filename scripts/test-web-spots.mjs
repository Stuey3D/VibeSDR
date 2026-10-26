// test-web-spots.mjs — V12 DIGITAL SPOTS IN THE WEB CLIENT, through a real vibeserver and the REAL built web page
// (2026-10-26). A fake rtl_tcp replays the UTC-anchored 20 m clip (`test-digi-spots clip`: FT8, FT4, FT2 and JS8 stations
// on their own dials); the server's dial is 20 m's Band centre. Headless Edge then:
//   • both buttons are offered — DIGITAL SPOTS: SINGLE MODE and DIGITAL SPOTS: BAND (vibeserver.json lists 20 m);
//   • BAND: the readout names the band ("20 m BAND"), the box title says so, and spots arrive in the list with a MODE
//     column in each mode's colour and letter — FT8, FT4, FT2 and JS8 from their own dials;
//   • the tuning arrow steps BANDS (20 m → 17 m: the dial moves to 17 m's centre, the readout follows) and back;
//   • the MAP is coloured by MODE: every marker's fill and letter is its mode's (src/utils/digiModes.ts), and the legend
//     lists the modes this server decodes;
//   • at PHONE WIDTH (390 px) nothing scrolls sideways; no exception on the page.
//   Screenshots go to VIBE_SHOT_DIR (default: the OS temp folder).
// ★ SILENT: headless Edge with --mute-audio and --disable-audio-output.
//   VIBESERVER_BIN=…/vibeserver VIBE_DIGI_BIN=…/test-digi-spots node scripts/test-web-spots.mjs
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
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
if (!BIN || !fs.existsSync(BIN) || !DIGI || !fs.existsSync(DIGI)) { console.log('   (not run — set VIBESERVER_BIN and VIBE_DIGI_BIN)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }
const RATE = 960000, CENTRE = 14080800;   // ★ 20 m's Band centre with FT8 + FT4 + FT2 + JS8 (digi_bands.h spanOf)
// ★ The modes' styles, from the one table both clients draw with.
const STYLE = { FT8: { colour: '#E69F00', letter: '8' }, FT4: { colour: '#56B4E9', letter: '4' },
                FT2: { colour: '#F0E442', letter: '2' }, JS8: { colour: '#009E73', letter: 'J' } };
const styleSrc = fs.readFileSync(path.join(root, 'src/utils/digiModes.ts'), 'utf8');
for (const [m, s] of Object.entries(STYLE)) if (!styleSrc.includes(`${m}: `) || !styleSrc.includes(`colour: '${s.colour}'`)) { console.log(`   FAIL the test's ${m} style is not digiModes.ts's`); process.exit(1); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webspots-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const CLIP = path.join(dir, '20m.cu8');
execFileSync(DIGI, ['clip', CLIP, String(RATE), String(CENTRE)], { stdio: 'ignore' });
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Spots web test', locator: 'IO92nh', adminPass: 'wsp-admin',
  radios: [{ serial: 'wsp', label: 'Test radio', enabled: true, configured: true }] }));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                     '--hf', CLIP, '--hf-centre', String(CENTRE), '--hf-utc'], { stdio: 'ignore' });
await sleep(400);
// ★ The GPU map's files from the repo (as the .deb installs them), so the map window can run its GPU renderer here too.
const env = { ...process.env, VIBESERVER_MAPGL_DIR: path.join(root, 'directory/public/mapgl'), VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(CENTRE), '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webspots-edge-'));
let edge = null;
try {
  for (let i = 0; i < 150; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const st = await fetch(`${H}/vibeserver.json`).then((r) => r.json());
  ok(st.spotsBand === true && (st.spotBands || []).some((b) => b.id === '20m'), `vibeserver.json: spotsBand ${st.spotsBand}, ${(st.spotBands || []).length} bands`);

  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', `--user-data-dir=${prof}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' });
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

  const freqHz = async () => {
    const main = await ev(`document.getElementById('mFreq').textContent + '|' + document.getElementById('mFreqFine').textContent`);
    const [a, b] = String(main).split('|'); return Math.round((parseFloat(a) * 1e6) + (parseInt(b || '0', 10) * 10));
  };
  const chan = () => ev(`(() => { const c = document.getElementById('mChan'); return c && !c.hidden ? c.textContent : ''; })()`);

  console.log('── the two buttons ──');
  ok(await until(`!document.getElementById('spotsBandBtn').hidden`, 8000), 'DIGITAL SPOTS: BAND is offered (this radio reaches 20 m)');
  ok(await ev(`document.getElementById('spotsBtn').textContent.trim()`) === 'DIGITAL SPOTS: SINGLE MODE', 'the old button is DIGITAL SPOTS: SINGLE MODE');
  ok(!(await ev(`document.getElementById('spotsBandBtn').classList.contains('dis')`)), 'Band is not greyed (the owner has it on)');

  console.log('── BAND ──');
  await ev(`document.getElementById('spotsBandBtn').click()`);
  ok(await until(`document.getElementById('spotsBandBtn').classList.contains('on') && !document.getElementById('spotsBtn').classList.contains('on')`, 3000), 'BAND lit, SINGLE MODE not');
  ok(await until(`/20 M BAND/.test(document.getElementById('decTitle').textContent)`, 3000), `the box title: "${await ev(`document.getElementById('decTitle').textContent`)}"`);
  ok(await until(`/20 m BAND/.test((document.getElementById('mChan')||{}).textContent||'')`, 5000), `the readout names the band: "${await chan()}"`);
  ok(await until(`['FT8', 'FT4', 'FT2', 'JS8'].every(md => [...document.querySelectorAll('#spotList .sres.spot .mode')].some(m => m.textContent.includes(md)))`, 60000),
     `spots of all FOUR modes listed (${await ev(`[...document.querySelectorAll('#spotList .sres.spot')].map(r => r.querySelector('.mode').textContent + ' ' + r.querySelector('.call').textContent).join(', ')`)})`);
  const rows = JSON.parse(await ev(`JSON.stringify([...document.querySelectorAll('#spotList .sres.spot')].map(r => ({ mode: r.querySelector('.mode').textContent.replace(/^./, ''), letter: r.querySelector('.mode i').textContent,
      bg: getComputedStyle(r.querySelector('.mode i')).backgroundColor, call: r.querySelector('.call').textContent, band: r.querySelector('.band').textContent })))`));
  const hex = (c) => '#' + (c.match(/\d+/g) || []).slice(0, 3).map((x) => Number(x).toString(16).padStart(2, '0')).join('').toUpperCase();
  ok(rows.length >= 6 && rows.every((r) => STYLE[r.mode] && r.letter === STYLE[r.mode].letter && hex(r.bg) === STYLE[r.mode].colour && r.band === '20m'),
     `every row's MODE column in its mode's colour and letter: ${rows.map((r) => `${r.mode}/${r.letter}/${hex(r.bg)} ${r.call}`).join(', ')}`);
  ok(!rows.some((r) => r.call === 'ZL3LSB'), 'the lower-sideband station is never listed');
  const shotBand = await shot('spots-band-desktop.png');

  console.log('── the arrows step bands ──');
  await trustedClick('#mVfoUp');
  ok(await until(`/17 m BAND/.test((document.getElementById('mChan')||{}).textContent||'')`, 5000), `▲: 17 m ("${await chan()}")`);
  const f17 = await freqHz();
  const b17 = st.spotBands.find((b) => b.id === '17m');
  ok(b17 && Math.abs(f17 - b17.hz) <= 20, `the dial is 17 m's centre (${f17} vs ${b17 && b17.hz})`);
  await trustedClick('#mVfoDown');
  ok(await until(`/20 m BAND/.test((document.getElementById('mChan')||{}).textContent||'')`, 5000), `▼: back to 20 m ("${await chan()}")`);
  ok(Math.abs((await freqHz()) - CENTRE) <= 20, `the dial is 20 m's centre again (${await freqHz()})`);

  console.log('── the map, coloured by mode ──');
  const before = new Set((await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).map((t) => t.id));
  await ev(`document.getElementById('mapBtn').click()`);
  let mapT = null;
  for (let i = 0; i < 40 && !mapT; i++) { await sleep(250); mapT = (await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).find((t) => t.type === 'page' && !before.has(t.id)); }
  ok(!!mapT, 'the map window opened');
  if (mapT) {
    const mws = new WebSocket(mapT.webSocketDebuggerUrl);
    let mid = 0; const mp = new Map();
    mws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && mp.has(m.id)) { mp.get(m.id)(m); mp.delete(m.id); } };
    await new Promise((r) => { mws.onopen = r; });
    const msend = (method, params = {}) => new Promise((res) => { const i = ++mid; mp.set(i, res); mws.send(JSON.stringify({ id: i, method, params })); });
    const mev = async (x) => (await msend('Runtime.evaluate', { expression: x, returnByValue: true })).result?.result?.value;
    for (let i = 0; i < 40; i++) { if (await mev(`typeof spots !== 'undefined' && document.querySelectorAll('#legend .lrow[data-mode]').length >= 4`)) break; await sleep(250); }
    const pts = JSON.parse(await mev(`JSON.stringify(spots.map(s => ({ m: s.mode, c: s.colour, l: s.letter })))`) || '[]');
    ok(pts.length >= 3 && pts.every((p) => STYLE[p.m] && p.c.toUpperCase() === STYLE[p.m].colour && p.l === STYLE[p.m].letter),
       `every marker is its MODE's colour and letter (${pts.map((p) => `${p.m}:${p.c}:${p.l}`).join(' ')})`);
    const legend = JSON.parse(await mev(`JSON.stringify([...document.querySelectorAll('#legend .lrow[data-mode]')].map(r => ({ m: r.dataset.mode, l: r.querySelector('.mk').textContent, bg: getComputedStyle(r.querySelector('.mk')).backgroundColor })))`) || '[]');
    ok(legend.map((x) => x.m).join(' ') === 'FT8 FT4 FT2 JS8' && legend.every((x) => x.l === STYLE[x.m].letter && hex(x.bg) === STYLE[x.m].colour),
       `the legend lists the server's modes with their colours and letters (${legend.map((x) => `${x.m}/${x.l}/${hex(x.bg)}`).join(' ')})`);
    ok(/BY MODE/.test(await mev(`document.getElementById('stats').textContent`)), 'the stats panel counts BY MODE');
    // ★ On the GPU map the letter rides the circle layer (vibemapgl-compat 'lc-circle-labels'); on the Leaflet fallback (no
    //   WebGL 2 in this headless browser) the legend and the colours are the cue.
    const glKit = await mev(`typeof GL !== 'undefined' && !!GL`);
    for (let i = 0; glKit && i < 40; i++) { if (await mev(`typeof vm !== 'undefined' && !!vm && !!vm.map && !!vm.map.getLayer('lc-circle-labels')`)) break; await sleep(250); }
    const gl = glKit && await mev(`typeof vm !== 'undefined' && !!vm && !!vm.map && !!vm.map.getLayer('lc-circle-labels')`);
    if (glKit) {
      ok(gl, 'GPU map: the letter layer (lc-circle-labels) is on the map');
      const feats = JSON.parse(await mev(`JSON.stringify((vm.map.getSource('lc-circles')._data.features || []).map(f => [f.properties.fillColor, f.properties.label, f.properties.labelColor]))`) || '[]');
      ok(feats.length >= 3 && feats.every(([c, l]) => Object.values(STYLE).some((st) => st.colour === String(c).toUpperCase() && st.letter === l)),
         `GPU map: every circle carries its mode's fill and letter (${feats.map((f) => f.join('/')).join(' ')})`);
    } else {
      // ★ The Leaflet fallback (no WebGL 2 in this headless browser): its canvas circles carry no text, so the letters ride
      //   small non-interactive markers above them.
      const ls = JSON.parse(await mev(`JSON.stringify([...document.querySelectorAll('.mkl b')].map(b => [b.textContent, getComputedStyle(b).color]))`) || '[]');
      ok(ls.length >= 3 && ls.every(([l]) => Object.values(STYLE).some((st) => st.letter === l)), `Leaflet fallback: each marker carries its mode's letter (${ls.map((x) => x[0]).join(' ')})`);
    }
    const r = await msend('Page.captureScreenshot', { format: 'png' });
    fs.writeFileSync(path.join(SHOTS, 'spots-map-by-mode.png'), Buffer.from(r.result.data, 'base64'));
    mws.close();
    await fetch(`http://127.0.0.1:${cdp}/json/close/${mapT.id}`).catch(() => {});
  }

  console.log('── phone width, 390 px ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1500);
  const fit = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth,
    rowMax: Math.max(0, ...[...document.querySelectorAll('#spotList .sres.spot')].map(r => r.getBoundingClientRect().right)),
    rows: document.querySelectorAll('#spotList .sres.spot').length })`));
  ok(fit.doc <= 391 && fit.rowMax <= 391 && fit.rows >= 1, `nothing scrolls sideways (page ${fit.doc}, rows end ${Math.round(fit.rowMax)} px, ${fit.rows} rows)`);
  const shotPhone = await shot('spots-band-phone.png');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });

  console.log('── back to SINGLE MODE ──');
  await ev(`document.getElementById('spotsBtn').click()`);
  ok(await until(`document.getElementById('spotsBtn').classList.contains('on') && !document.getElementById('spotsBandBtn').classList.contains('on')`, 3000), 'SINGLE MODE lit, BAND not');
  ok(await until(`!/BAND/.test((document.getElementById('mChan')||{}).textContent||'') && document.getElementById('decTitle').textContent.includes('DIGITAL SPOTS') && !/BAND/.test(document.getElementById('decTitle').textContent)`, 8000),
     `the readout no longer names a band ("${await chan()}"); the box is "${await ev(`document.getElementById('decTitle').textContent`)}"`);
  await trustedClick('#mVfoUp');
  await sleep(600);
  ok(Math.abs((await freqHz()) - CENTRE) < 20000, `▲ in Single tunes by the step again, not a band (${await freqHz()})`);
  ok(/digital spots: Band started/.test(fs.readFileSync(logPath, 'utf8')) && /digital spots: Single mode started/.test(fs.readFileSync(logPath, 'utf8')),
     'the server ran Band, then Single');
  ok(errs.length === 0, `no exception on the page${errs.length ? ': ' + errs.slice(0, 2).join(' | ') : ''}`);
  console.log(`   screenshots: ${[shotBand, path.join(SHOTS, 'spots-map-by-mode.png'), shotPhone].join('  ')}`);
} finally {
  try { edge?.kill('SIGTERM'); } catch {}
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(300);
  fs.rmSync(dir, { recursive: true, force: true });
  fs.rmSync(prof, { recursive: true, force: true });
}
console.log(`\n${fail ? '✗' : '✓'} test-web-spots: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
