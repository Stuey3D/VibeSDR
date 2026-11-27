// test-web-digimap-cards.mjs — ★★★ b97 THE AMATEUR (DIGI MODES / SPOTS) MAP ON THE MAP CARD SYSTEM, in a real (headless,
// MUTED) Edge, through the REAL built page (web/dist — run scripts/build-web.mjs first). No server is needed: a stand-in
// serves web/dist, the GPU map's files (web/mapkit + directory/public/mapgl) and a station store answer
// (/vibeserver/stations.json — 3 modes, 3 bands, 2 radios); the page's own link to a radio simply never connects.
// Asserts: VIEWS' DIGI MODES key opens the map window; the OLD chrome is gone (top bar, stats / legend / summary / list
// panels, FIT, the ticks); the cards: LAYERS (only with 2+ layers — APRS / mesh added in the window), STATS (summary, each
// mode, BANDS, RANGE, COUNTRIES), MAP CONTROLS (TIME, RADIO, BAND, MODE, the clock, AUTO / FIT, the KEY), the LIST (tabs,
// sort, a row centres its station), RATE (stations a minute — the store's recent ones); BAND / MODE filter the map; the
// overlap / visibility check at every size (scripts/lib/map-chrome-layout.mjs); ✕ closes the window. Screenshots.
//   node scripts/test-web-digimap-cards.mjs [screenshotDir]      (exit 3 = not run: no Edge or no web/dist)
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { chromeLayoutExpr, setInsetsExpr, MAP_SIZES } from './lib/map-chrome-layout.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const DIST = path.join(root, 'web/dist'), MAPKIT = path.join(root, 'web/mapkit'), DIRGL = path.join(root, 'directory/public/mapgl');
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.argv[2] || '';
if (!fs.existsSync(EDGE)) { console.log('   (not run — Edge needed)'); process.exit(3); }
if (!fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.log('   (not run — build the page first: node scripts/build-web.mjs)'); process.exit(3); }
if (SHOTS) fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'digimap-'));
const procs = [];
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGKILL'); } catch {} } try { fs.rmSync(T, { recursive: true, force: true }); } catch {} });
setTimeout(() => { console.log('   FAIL deadline (240 s)'); process.exit(1); }, 240000).unref();

// ── the station store's answer (vibe_station_store.h): FT8 on 20 m / 40 m, FT4 on 20 m, WSPR on 30 m; two radios ──
const now = () => Math.floor(Date.now() / 1000);
const ST = [
  ['G4ABC', 'IO91', 'FT8', '20m', 51.5, -0.5, 60, 'England', ['r1']], ['EA8XYZ', 'IL18', 'FT8', '20m', 28.5, -16.0, 2900, 'Canary Islands', ['r1']],
  ['W1AW', 'FN31', 'FT8', '20m', 41.5, -72.5, 5400, 'United States', ['r2']], ['JA1ABC', 'PM95', 'FT8', '40m', 35.5, 139.0, 9500, 'Japan', ['r2']],
  ['DL1AA', 'JO62', 'FT4', '20m', 52.5, 13.0, 930, 'Germany', ['r1']], ['OH2BH', 'KP20', 'FT4', '20m', 60.2, 24.9, 1840, 'Finland', ['r1', 'r2']],
  ['VK2XYZ', 'QF56', 'WSPR', '30m', -33.5, 151.0, 17000, 'Australia', ['r2']], ['K1JT', 'FN20', 'WSPR', '30m', 40.4, -74.6, 5600, 'United States', ['r1']],
  ['PY2ABC', 'GG66', 'FT8', '40m', -23.5, -46.5, 9400, 'Brazil', ['r1']],
];
const stations = () => ({ now: now(), window: '24h', since: 0, total: ST.length, count: ST.length, truncated: 0, decodesToday: 1234, spotsSent: 77,
  radios: [{ id: 'r1', name: 'RSP1B', n: 6 }, { id: 'r2', name: 'V4', n: 5 }],
  stations: ST.map(([call, grid, mode, band, lat, lon, km, country, radios], i) => ({ call, grid, mode, band, lat, lon, km, country, radios,
    freq: 14074000, count: 3 + i, snr: -10 + i, lastSnr: -12, first: now() - 7200, last: now() - 30 - i * 40, src: i % 2 ? 1 : 2, modes: [mode], bands: [band] })) });

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.pbf': 'application/x-protobuf', '.png': 'image/png', '.pmtiles': 'application/octet-stream' };
function sendFile(req, res, f) {
  const size = fs.statSync(f).size, type = TYPES[path.extname(f)] || 'application/octet-stream';
  const m = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
  if (m) { const a = Number(m[1]), b = m[2] ? Math.min(Number(m[2]), size - 1) : size - 1;
    res.writeHead(206, { 'content-type': type, 'content-range': `bytes ${a}-${b}/${size}`, 'content-length': b - a + 1, 'accept-ranges': 'bytes' }); fs.createReadStream(f, { start: a, end: b }).pipe(res); }
  else { res.writeHead(200, { 'content-type': type, 'content-length': size }); fs.createReadStream(f).pipe(res); }
}
const inDir = (base, rel) => { const f = path.join(base, path.normalize(rel).replace(/^(\.\.[/\\])+/, '')); return f.startsWith(base) && fs.existsSync(f) && fs.statSync(f).isFile() ? f : null; };
const srv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x'), p = decodeURIComponent(u.pathname);
  if (/stations\.json$/.test(p)) { res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify(stations())); return; }
  if (p === '/' || p === '/index.html') return sendFile(req, res, path.join(DIST, 'vibesdr.html'));
  if (p.startsWith('/vs/')) { const f = inDir(DIST, p); if (f) return sendFile(req, res, f); }
  if (p.startsWith('/mapgl/')) {
    const rel = p.slice(7);
    const f = rel === 'vibemap-style.json' ? path.join(MAPKIT, rel) : rel.startsWith('vendor/') ? inDir(path.join(MAPKIT, 'vendor'), rel.slice(7)) || inDir(path.join(DIRGL, 'vendor'), rel.slice(7))
      : (MAPKIT && inDir(MAPKIT, rel)) || inDir(DIRGL, rel);
    if (f) return sendFile(req, res, f);
  }
  res.writeHead(404); res.end();
});
const PORT = await new Promise((r) => srv.listen(0, '127.0.0.1', () => r(srv.address().port)));

// ── the browser ──
const cdp = await new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
procs.push(spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--no-default-browser-check',
  '--disable-extensions', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--disable-popup-blocking',
  `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1440,900', 'about:blank'], { stdio: 'ignore' }));
const list = async () => { try { return await (await fetch(`http://127.0.0.1:${cdp}/json/list`)).json(); } catch { return []; } };
async function attach(t) {
  const ws = new WebSocket(t.webSocketDebuggerUrl); let id = 0; const pend = new Map(); const errors = [];
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
  await new Promise((r) => (ws.onopen = r));
  const call = (method, params = {}) => Promise.race([new Promise((r) => { const n = ++id; pend.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); }), sleep(15000).then(() => ({}))]);
  const js = async (x, gesture = false) => (await call('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true, userGesture: gesture })).result?.result?.value;
  const until = async (x, ms = 15000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(x)) return true; await sleep(200); } return false; };
  await call('Runtime.enable'); await call('Page.enable');
  return { ws, call, js, until, errors };
}
let page;
for (let i = 0; i < 60 && !page; i++) { page = (await list()).find((t) => t.type === 'page'); if (!page) await sleep(250); }
const P = await attach(page);
const shot = async (W, name) => { if (!SHOTS) return; const r = await W.call('Page.captureScreenshot', { format: 'png' }); if (r.result) { fs.writeFileSync(path.join(SHOTS, name), Buffer.from(r.result.data, 'base64')); console.log(`   (screenshot ${name})`); } };

try {
  console.log('── the page opens the AMATEUR MAP window (VIEWS · DIGI MODES) ──');
  await P.call('Page.navigate', { url: `http://127.0.0.1:${PORT}/` });
  ok(await P.until(`typeof window.__vibeOpenDigiMap === 'function'`, 20000), 'the radio page loaded (no radio behind it)');
  await sleep(1500);
  const before = new Set((await list()).map((t) => t.id));
  await P.js(`window.__vibeOpenDigiMap({ layer: 'spots' }); true`, true);   // ★ VIEWS' DIGI MODES key's own call (the key is wired once a radio answers)
  let mt = null;
  for (let i = 0; i < 60 && !mt; i++) { await sleep(250); mt = (await list()).find((t) => t.type === 'page' && !before.has(t.id)); }
  ok(!!mt, 'the map window opened');
  const M = await attach(mt);
  ok(await M.until(`typeof MC !== 'undefined' && !!MC && !!document.querySelector('.trkView:not([hidden])') && typeof stored !== 'undefined' && stored.size === ${ST.length}`, 25000), `the card system is up and the station store read (${await M.js(`JSON.stringify({ mc: typeof MC, vmc: typeof VibeMapChrome, view: !!document.querySelector('.trkView'), st: typeof stored !== 'undefined' ? stored.size : -1, scripts: document.scripts.length, body: document.body ? document.body.innerHTML.slice(0, 200) : '' })`)})`);
  await M.call('Emulation.setDeviceMetricsOverride', { width: 1440, height: 900, deviceScaleFactor: 1, mobile: false });
  ok(await M.until(`!!window.gl || (typeof vm !== 'undefined' && vm && vm.map && vm.map.loaded())`, 25000), 'the GPU map is drawn in the card system\'s holder');
  await sleep(1500);

  console.log('── the OLD chrome is gone ──');
  ok(await M.js(`['top', 'stats', 'legend', 'summary', 'amList', 'fitBtn', 'tStats', 'tLegend', 'tSummary', 'tList', 'count', 'layers', 'wins', 'rads', 'scope', 'm', 'wrap'].every(id => !document.getElementById(id))`), 'no top bar, no panels, no FIT, no ticks (deleted, not hidden)');
  ok(await M.js(`typeof digiFit === 'undefined' && typeof digiFitMake === 'undefined' && typeof amList === 'undefined'`), 'the window\'s own auto fit and list code are gone (the card system\'s)');

  console.log('── the cards ──');
  ok(await M.js(`!!document.querySelector('.trkTop .exit') && document.querySelector('.trkTop .exit').getAttribute('aria-label') !== ''`), 'the ✕ (top-left) closes the window');
  ok(await M.js(`document.querySelector('.mDock[data-dock="layers"]').hidden`), 'LAYERS is not offered with one layer (no dead control)');
  await M.js(`aprs = cleanAprs([{ key: 'G4XYZ-9', call: 'G4XYZ-9', lat: 52.2, lon: 0.1, age: 60, kind: 'position', km: 80, count: 3 }]); mesh = cleanMesh([{ key: 'mesh:ab', name: 'Hilltop', net: 'MeshCore', lat: 51.9, lon: -1.2, age: 300, what: 'repeater', live: false }]); paintChrome(); render(); true`);
  await sleep(500);
  ok(await M.js(`!document.querySelector('.mDock[data-dock="layers"]').hidden && ['spots', 'aprs', 'mesh'].every(id => !!document.querySelector('.trkCard2 [data-tgl="' + id + '"]'))`), 'with APRS and MESH: LAYERS offers SPOTS · APRS · MESH');
  ok(await M.js(`document.querySelector('.trkCard2 [data-tgl="spots"] .n').textContent`) === String(ST.length), `…each with its count (SPOTS ${ST.length})`);
  await M.js(`document.querySelector('.trkCard2 [data-tgl="spots"]').click(); true`); await sleep(300);
  ok(await M.js(`!show.spots`), 'SPOTS off from LAYERS');
  await M.js(`document.querySelector('.trkCard2 [data-tgl="spots"]').click(); true`);
  const tabs = await M.js(`[...document.querySelectorAll('.trkCard2[data-card="stats"] .stTab')].map(b => b.textContent).join()`);
  ok(/SUMMARY/.test(tabs) && /FT8/.test(tabs) && /FT4/.test(tabs) && /WSPR/.test(tabs) && /BANDS/.test(tabs) && /COUNTRIES/.test(tabs), `STATS: the summary, each mode, BANDS, COUNTRIES (${tabs})`);
  ok(await M.js(`/FURTHEST/.test(document.querySelector('.trkCard2[data-card="stats"] .stBody').textContent) && /17,000 km/.test(document.querySelector('.trkCard2[data-card="stats"] .stBody').textContent)`), 'STATS SUMMARY: FURTHEST VK2XYZ 17,000 km');
  await M.js(`document.querySelector('.trkCard2[data-card="stats"] .stTab[data-st="BANDS"]').click(); true`);
  ok(await M.js(`(() => { const t = document.querySelector('.trkCard2[data-card="stats"] .stBody').textContent; return /30m/.test(t) && /40m/.test(t) && /20m/.test(t) && /JA1ABC/.test(t); })()`), 'STATS BANDS: the furthest station per band');
  ok(await M.js(`[...document.querySelectorAll('.trkCard2[data-card="map"] [data-rng]')].map(b => b.dataset.rng).join()`) === '1h,24h,7d,30d,all', 'MAP CONTROLS TIME: 1 H · 24 H · 7 D · 30 D · ALL (no THIS SESSION: not over a radio)');
  ok(await M.js(`document.querySelectorAll('.slot-ctl [data-band]').length === 3 && document.querySelectorAll('.slot-ctl [data-mode]').length === 3 && document.querySelectorAll('.slot-ctl [data-radio]').length === 3`), 'MAP CONTROLS: BAND (3), MODE (3), RADIO (all + 2) — the spots\' own filters');
  await M.js(`document.querySelector('.slot-ctl [data-band="20m"]').click(); true`); await sleep(300);
  ok(await M.js(`spots.length === ${ST.filter((x) => x[3] !== '20m').length} && spots.every(s => s.band !== '20m')`), 'BAND 20m off: its stations leave the map and the list');
  await M.js(`document.querySelector('.slot-ctl [data-band="20m"]').click(); document.querySelector('.slot-ctl [data-mode="WSPR"]').click(); true`); await sleep(300);
  ok(await M.js(`spots.length === ${ST.filter((x) => x[2] !== 'WSPR').length} && JSON.parse(localStorage.getItem('vibesdr.digiMapFilter')).modeOff.join() === 'WSPR'`), 'MODE WSPR off: gone, and remembered in this browser');
  await M.js(`document.querySelector('.slot-ctl [data-mode="WSPR"]').click(); true`); await sleep(300);
  await M.js(`document.querySelector('.slot-ctl [data-radio="r2"]').click(); true`); await sleep(300);
  ok(await M.js(`spots.every(s => s.radios.includes('r2'))`), 'RADIO V4: only its stations');
  await M.js(`document.querySelector('.slot-ctl [data-radio=""]').click(); true`); await sleep(300);
  ok(await M.js(`/UTC \\d\\d:\\d\\d:\\d\\d/.test(document.getElementById('amClock').textContent)`), 'the clock (UTC) in MAP CONTROLS');
  ok(await M.js(`!document.querySelector('.trkCard2[data-card="map"] .fit').hidden && !document.querySelector('.trkCard2[data-card="map"] .fitOnce').hidden`), 'AUTO and FIT keys in MAP CONTROLS (the shared auto fit on this map)');
  ok(await M.js(`/Colour \\+ letter = mode/.test(document.querySelector('.trkKey .kb').textContent) && !document.querySelector('.keySect').hidden`), 'the KEY (the modes\' colours and letters) in MAP CONTROLS');
  ok(await M.js(`!document.querySelector('.mDock[data-dock="rate"]').hidden && /\\/min/.test(document.querySelector('.mDock[data-dock="rate"]').textContent)`), 'RATE: stations a minute, from the store\'s recent ones');
  ok(await M.js(`document.querySelector('.mDock[data-dock="tune"]').hidden && document.querySelector('.mDock[data-dock="chat"]').hidden && document.querySelector('.lSwap').hidden`), 'no TUNE, no CHAT, no waterfall (not over a radio)');

  console.log('── the LIST ──');
  await M.js(`MC.card('list', true); true`); await sleep(500);
  ok(await M.js(`document.querySelectorAll('.trkCard2[data-card="list"] .rows .trkRow[data-key]').length`) === ST.length, `a row per station (${ST.length})`);
  ok(await M.js(`[...document.querySelectorAll('.trkCard2[data-card="list"] .hd .tab')].map(t => t.dataset.tab).join()`) === 'spots,aprs,mesh', 'tabs: SPOTS · APRS · MESH');
  const firstLatest = await M.js(`document.querySelector('.trkCard2[data-card="list"] .rows .trkRow').dataset.key`);
  await M.js(`document.querySelector('.trkCard2[data-card="list"] .srtT').click(); true`); await sleep(300);
  ok(firstLatest === 'G4ABC' && await M.js(`document.querySelector('.trkCard2[data-card="list"] .rows .trkRow').dataset.key`) === 'G4ABC' && await M.js(`listSort`) === 'nearest', 'latest ⇄ nearest (one toggle)');
  await M.js(`document.querySelector('.trkCard2[data-card="list"] .rows .trkRow[data-key="JA1ABC"]').click(); true`); await sleep(800);
  ok(await M.js(`(() => { const c = map.getCenter(); return Math.abs(c.lat - 35.5) < 1.5 && Math.abs(c.lng - 139) < 3; })()`), 'a row pressed centres its station (AUTO pauses)');
  await shot(M, 'digimap-desktop-1440x900.png');

  console.log('── ★★★ the card system at every size: nothing off screen, nothing overlapping, nothing hidden ──');
  await M.js(`MC.fitNow(); true`);
  for (const [name, W, H, ins, mobile] of MAP_SIZES) {
    await M.call('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: mobile ? 2 : 1, mobile });
    await M.js(setInsetsExpr(ins)); await sleep(1500);
    for (const listOpen of [true, false]) {
      await M.js(`MC.card('list', ${listOpen}); true`); await sleep(700);
      const L = JSON.parse(await M.js(chromeLayoutExpr(ins)));
      const have = L.pieces.map((p) => p.k);
      ok(['x', 'help', 'chip:list', 'chip:layers', 'chip:map', 'chip:stats', 'chip:rate'].every((k) => have.includes(k)) && !L.hidden.length, `${name}, LIST ${listOpen ? 'open' : 'shut'}: ✕, HELP, LIST, LAYERS, MAP, STATS, RATE on screen (${have.join(' ')}${L.hidden.length ? ' · hidden: ' + L.hidden.join(' ') : ''})`);
      ok(!L.outside.length, `${name}, LIST ${listOpen ? 'open' : 'shut'}: everything inside the window${L.short ? ' and the safe rectangle' : ''} (${L.outside.join(' ') || 'all ' + L.pieces.length})`);
      ok(!L.overlaps.length && !L.covered.length, `${name}, LIST ${listOpen ? 'open' : 'shut'}: no two pieces overlap (${JSON.stringify(L.overlaps)} ${L.covered.join(' ')})`);
      if (listOpen) await shot(M, `digimap-layout-${W}x${H}.png`);
    }
    await M.js(setInsetsExpr({ l: null, r: null, b: null }));
  }
  await M.call('Emulation.clearDeviceMetricsOverride');
  ok(!M.errors.length, `no script errors in the map window (${M.errors.slice(0, 3).join(' | ')})`);

  console.log('── ✕ closes the window ──');
  await M.js(`document.querySelector('.trkTop .exit').click(); true`).catch(() => {});
  let gone = false;
  for (let i = 0; i < 20 && !gone; i++) { await sleep(250); gone = !(await list()).some((t) => t.id === mt.id); }
  ok(gone, 'the map window closed');
} catch (e) { ok(false, String(e && e.stack || e)); }
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
