// test-community-render.mjs — THE COMMUNITY MAP PAGE in a real (headless, MUTED) Edge, with three fake contributing servers.
//   · a stand-in for the directory: public/ served as the Worker serves it (Range for the map packs) + /api/community;
//   · three "receivers" on their own ports answering /vibeserver/community.json with the fixtures (community-fixtures.mjs —
//     two of the bodies are real server output), each COUNTING what it is asked.
// Asserts: the page loads, reads all three, merges the shared aircraft and DSC call ("heard by 2"), draws markers; the
// scope / mode filters; the RECEIVERS list (rows, layers, busy words, the directory's LISTEN link), picking a row, ONLY
// THIS; the request floor (each receiver once per load, nothing more inside the minute; /api/community once); then
// screenshots at 1280 px and 390 px.
//   node directory/scripts/test-community-render.mjs [screenshotDir]      (exit 3 = not run: no Edge)
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { answers, rebase, contributors } from './community-fixtures.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const PUB = path.join(here, '..', 'public');
const SHOTS = process.argv[2] || '';
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   (not run — Edge needed)'); process.exit(3); }
if (SHOTS) fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const procs = [];
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'commap-'));
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGKILL'); } catch {} } try { fs.rmSync(T, { recursive: true, force: true }); } catch {} });
const deadline = setTimeout(() => { console.log('   FAIL deadline (180 s)'); process.exit(1); }, 180000);
const listen = (srv) => new Promise((r) => srv.listen(0, '127.0.0.1', () => r(srv.address().port)));

// ── the three receivers ──
const A = answers();
const hits = {};
const ports = {};
for (const id of Object.keys(A)) {
  hits[id] = 0;
  const srv = http.createServer((req, res) => {
    if (req.url.startsWith('/vibeserver/community.json')) {
      hits[id]++;
      const body = JSON.stringify(rebase(A[id], Math.floor(Date.now() / 1000)));
      res.writeHead(200, { 'content-type': 'application/json', 'access-control-allow-origin': '*', 'cache-control': 'public, max-age=30' });
      res.end(body);
    } else { res.writeHead(404); res.end(); }
  });
  ports[id] = await listen(srv);
}

// ── the directory stand-in ──
let listHits = 0;
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.pbf': 'application/x-protobuf', '.png': 'image/png' };
const dirSrv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  if (u.pathname === '/api/community') {
    listHits++;
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'public, max-age=120' });
    res.end(JSON.stringify(contributors((id) => `http://127.0.0.1:${ports[id]}`)));
    return;
  }
  let p = decodeURIComponent(u.pathname);
  if (p === '/community-map') p = '/community-map.html';
  const f = path.join(PUB, path.normalize(p).replace(/^(\.\.[/\\])+/, ''));
  if (!f.startsWith(PUB) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
  const size = fs.statSync(f).size, type = TYPES[path.extname(f)] || 'application/octet-stream';
  const m = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
  if (m) {
    const a = Number(m[1]), b = m[2] ? Math.min(Number(m[2]), size - 1) : size - 1;
    res.writeHead(206, { 'content-type': type, 'content-range': `bytes ${a}-${b}/${size}`, 'content-length': b - a + 1, 'accept-ranges': 'bytes' });
    fs.createReadStream(f, { start: a, end: b }).pipe(res);
  } else { res.writeHead(200, { 'content-type': type, 'content-length': size }); fs.createReadStream(f).pipe(res); }
});
const DP = await listen(dirSrv);
const PAGE = `http://127.0.0.1:${DP}/community-map`;

// ── the browser ──
const cdpPort = await new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
procs.push(spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--no-default-browser-check',
  '--disable-extensions', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist',
  `--remote-debugging-port=${cdpPort}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' }));
let target;
for (let i = 0; i < 60 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdpPort}/json`)).json()).find((t) => t.type === 'page'); } catch {} if (!target) await sleep(250); }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0; const pending = new Map(); const errors = [];
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
  if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text); };
const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
const until = async (expr, ms = 15000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(200); } return false; };
const shot = async (name) => { if (!SHOTS) return; const r = await call('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(path.join(SHOTS, name), Buffer.from(r.result.data, 'base64')); console.log(`   (screenshot ${name})`); };
await call('Runtime.enable'); await call('Page.enable');

try {
  console.log('── the page reads the three receivers directly ──');
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 20000), `all three receivers read (${JSON.stringify(await js('window.__cm && window.__cm.up'))})`);
  const cm = await js('window.__cm');
  ok(cm.servers === 3 && cm.items === 9, `9 items from 3 receivers after the merge (${cm.items})`);
  ok(cm.heardBy['a:4CA000'] === 2, '★ BAW696 heard by two receivers = one marker, "heard by 2"');
  ok(Object.entries(cm.heardBy).some(([k, n]) => k.startsWith('d:') && n === 2), '★ the DSC distress heard by two = one');
  ok(JSON.stringify(cm.modes) === JSON.stringify(['adsb', 'hfdl', 'ndb', 'ais', 'dsc', 'navtex']), `mode filters only for what is shared (${cm.modes})`);
  ok(await js(`document.querySelectorAll('#modes [data-mode]').length`) === 6, 'six mode chips, no ACARS / VDL2 (nobody shares them)');
  ok(await js(`!!window.__cm && (window.GLVM !== undefined || true)`) && await js(`!!document.querySelector('#map canvas, #map .leaflet-pane')`), 'the map is drawn');
  const gpu = await js(`!!document.querySelector('#map canvas.maplibregl-canvas')`);
  console.log(`   (map renderer: ${gpu ? 'GPU (MapLibre)' : 'Leaflet fallback'})`);
  await sleep(3500);   // the basemap's tiles
  await shot('community-map-desktop.png');

  console.log('── the filters ──');
  await js(`document.querySelector('[data-scope=aviation]').click(); true`);
  let b = await js('window.__cm.byKind');
  ok(b.aircraft === 4 && b.ndb === 1 && !b.ship && !b.dsc && !b.navtex, `AVIATION: 4 aircraft + 1 NDB (${JSON.stringify(b)})`);
  ok(await js(`document.querySelectorAll('#modes [data-mode]').length`) === 3, 'AVIATION shows only the aviation mode chips');
  await js(`document.querySelector('[data-scope=maritime]').click(); true`);
  b = await js('window.__cm.byKind');
  ok(b.ship === 2 && b.dsc === 1 && b.navtex === 1 && !b.aircraft, `MARITIME: 2 ships, 1 DSC, 1 NAVTEX (${JSON.stringify(b)})`);
  await shot('community-map-maritime.png');
  await js(`document.querySelector('[data-scope=both]').click(); document.querySelector('[data-mode=adsb]').click(); true`);
  b = await js('window.__cm.byKind');
  ok(b.aircraft === 1, `ADS-B off: only the HFDL aircraft left (${b.aircraft})`);
  await js(`document.querySelector('[data-mode=adsb]').click(); document.querySelector('[data-age="900"]').click(); true`);
  ok(await js('window.__cm.shown') < 9, `15 MIN: the older items go (${await js('window.__cm.shown')})`);
  await js(`document.querySelector('[data-age="86400"]').click(); true`);

  console.log('── RECEIVERS: the list, LISTEN, picking one, ONLY THIS ──');
  await js(`document.getElementById('rxBtn').click(); true`);
  ok(await until(`!document.getElementById('rx').hidden && document.querySelectorAll('#rxList .row[data-id]').length === 3`), 'the list opens with three receivers');
  const rows = await js(`[...document.querySelectorAll('#rxList .row[data-id]')].map(r => ({ id: r.dataset.id, text: r.textContent, href: r.querySelector('a.listen')?.href }))`);
  const lv = rows.find((r) => r.id === 'lenovo'), pi = rows.find((r) => r.id === 'pi500');
  ok(lv && lv.href === 'https://lenovo.vibeserver.vibesdr.net/', `LISTEN is the directory's link (${lv && lv.href})`);
  ok(lv && /NAVTEX/.test(lv.text) && /NDB/.test(lv.text) && /DSC/.test(lv.text) && /3 on the map now/.test(lv.text) && /GB · IO92/.test(lv.text), 'a row: where, what it shares, how many items now');
  ok(pi && /no free slot/.test(pi.text) && /4\/4 listening/.test(pi.text), `busy as the directory says it ("${pi && pi.text.match(/no free slot[^A-Z]*/)}")`);
  await shot('community-receivers-desktop.png');
  await js(`document.querySelector('#rxList .row[data-id=lenovo]').click(); true`);
  ok(await until(`window.__cm.selected === 'lenovo' && !!document.querySelector('.rxm.sel')`), 'picking a row selects and highlights the receiver on the map');
  await js(`document.querySelector('#rxList [data-only=lenovo]').click(); true`);
  ok(await until(`window.__cm.server === 'lenovo' && window.__cm.shown === 3`), `ONLY THIS: the map shows what the Lenovo heard (${await js('window.__cm.shown')})`);
  ok(await js(`!!document.getElementById('onlyChip')`), 'and says so (a chip to clear it)');
  await sleep(1500);
  await shot('community-only-desktop.png');
  await js(`document.getElementById('onlyChip').click(); true`);
  ok(await until(`window.__cm.server === '' && window.__cm.shown === 9`), 'clearing it brings everything back');

  console.log('── the floor: each receiver once per load, the list once ──');
  await sleep(20000);
  ok(Object.values(hits).every((n) => n === 1), `20 s later each receiver has been asked once (${JSON.stringify(hits)})`);
  ok(listHits === 1, `/api/community asked once (${listHits})`);

  console.log('── a phone, 390 px ──');
  await call('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 20000), 'loads on a phone');
  ok(await js(`document.documentElement.scrollWidth <= 390`), `no sideways scroll (${await js('document.documentElement.scrollWidth')})`);
  await sleep(3500);
  await shot('community-map-390.png');
  await js(`document.getElementById('rxBtn').click(); true`);
  ok(await until(`!document.getElementById('rx').hidden`), 'RECEIVERS opens as a sheet');
  const sheet = await js(`(() => { const r = document.getElementById('rx').getBoundingClientRect(); return { w: r.width, top: r.top, h: r.height }; })()`);
  ok(sheet.w === 390 && sheet.top > 200, `the sheet: full width from below the filters (${JSON.stringify(sheet)})`);
  await sleep(500);
  await shot('community-receivers-390.png');
  ok(errors.length === 0, `no script errors (${errors.slice(0, 3).join(' | ')})`);
} catch (e) { fail++; console.log('   FAIL ' + (e && e.stack || e)); }
clearTimeout(deadline);
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
