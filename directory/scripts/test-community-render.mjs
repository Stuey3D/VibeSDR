// test-community-render.mjs — THE COMMUNITY MAP PAGE in a real (headless, MUTED) Edge, with fake contributing servers.
//   · a stand-in for the directory: public/ served as the Worker serves it (Range for the map packs) + /api/community;
//   · fake "receivers" on their own ports answering /vibeserver/community.json (?s= honoured) and the NAVTEX text endpoint,
//     with the fixtures (community-fixtures.mjs — the Lenovo-like one is REAL server output), each COUNTING what it is asked.
// Asserts: all three groups on together and independently; the chips (and NAVTEX's own filters) work; the merges; a NAVTEX
// warning WITH ITS AREA drawn on the GPU layer and its card's text fetched from the receiver on opening; the MESSAGES list;
// the RECEIVERS list (LISTEN, busy words, ONLY THIS); only the layers switched on are fetched, a chip switched on fetches
// itself; each receiver once per load; clustering when zoomed out; 10 receivers: first draw, full load, payloads, memory.
// Screenshots at 1280 px and 390 px.
//   node directory/scripts/test-community-render.mjs [screenshotDir]      (exit 3 = not run: no Edge)
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { answers, rebase, contributors, answerFor } from './community-fixtures.mjs';

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
const deadline = setTimeout(() => { console.log('   FAIL deadline (240 s)'); process.exit(1); }, 240000);
const listen = (srv) => new Promise((r) => srv.listen(0, '127.0.0.1', () => r(srv.address().port)));

// ── the receivers (10: the first three are the named ones) ──
const N = 10;
const A = answers(N);
const hits = {}, asked = {}, sizes = {};
const ports = {};
for (const id of Object.keys(A)) {
  hits[id] = 0; asked[id] = [];
  const srv = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const h = { 'content-type': 'application/json', 'access-control-allow-origin': '*', 'cache-control': 'public, max-age=30' };
    if (u.pathname === '/vibeserver/community.json') {
      hits[id]++; asked[id].push(u.searchParams.get('s') || '');
      const body = JSON.stringify(answerFor(rebase(A[id], Math.floor(Date.now() / 1000)), u.searchParams.get('s')));
      sizes[id] = Math.max(sizes[id] || 0, body.length);
      res.writeHead(200, h); res.end(body);
    } else if (u.pathname === '/vibeserver/community/navtex') {
      res.writeHead(200, h); res.end(JSON.stringify({ f: u.searchParams.get('f'), txt: `ZCZC TEST\nTHE MESSAGE TEXT FROM ${id.toUpperCase()} (${u.searchParams.get('f')})\nNNNN\n` }));
    } else { res.writeHead(404); res.end(); }
  });
  ports[id] = await listen(srv);
}

// ── the directory stand-in: ?n= receivers in its list (3 by default) ──
let listHits = 0;
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.pbf': 'application/x-protobuf', '.png': 'image/png' };
let listN = 3;
const dirSrv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  if (u.pathname === '/api/community') {
    listHits++;
    res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'public, max-age=120' });
    res.end(JSON.stringify(contributors((id) => `http://127.0.0.1:${ports[id]}`, listN)));
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
  '--disable-extensions', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--enable-precise-memory-info',
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
const cm = () => js('window.__cm');
const resetHits = () => { for (const k of Object.keys(hits)) { hits[k] = 0; asked[k] = []; } listHits = 0; };
await call('Runtime.enable'); await call('Page.enable');

try {
  console.log('── 3 receivers: the page reads them directly; all three groups on together ──');
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 25000), `all three receivers read (${await js('window.__cm && window.__cm.up')})`);
  let c = await cm();
  ok(JSON.stringify(c.groups) === JSON.stringify(['aviation', 'maritime', 'misc']), 'AVIATION, MARITIME and MISC all on by default');
  ok(await js(`[...document.querySelectorAll('#groups [data-group]')].map(b => b.getAttribute('aria-pressed')).join()`) === 'true,true,true', 'three independent group toggles');
  ok(JSON.stringify(c.modes) === JSON.stringify(['adsb', 'hfdl', 'ndb', 'ais', 'dsc', 'navtex', 'sondes', 'aprs', 'mesh']), `a chip only for what is shared (${c.modes})`);
  ok(await js(`[...document.querySelectorAll('#modes [data-mode]')].every(b => /shared for the last/.test(b.title))`), 'each chip’s tooltip says its window');
  ok(c.heardBy['a:4CA000'] === 2 && Object.entries(c.heardBy).some(([k, n]) => k.startsWith('nw:') && n === 2) && c.heardBy['p:G4ABC-9'] === 2, '★ merged: BAW696, a NAVTEX warning and an APRS station each "heard by 2"');
  ok(['aircraft', 'ship', 'dsc', 'navst', 'navwarn', 'navmet', 'navmsg', 'ndb', 'sonde', 'aprs', 'mesh'].every((k) => c.byKind[k] > 0), `every kind drawn at once: ${JSON.stringify(c.byKind)}`);
  ok(c.gpu.shapes >= 2 && c.gpu.met > 0 && c.gpu.dense > 0, `GPU layers: ${JSON.stringify(c.gpu)} (warning areas + the sonde's track as shapes; forecasts as the NAVTEX map's pills)`);
  console.log(`   (3 receivers: ${JSON.stringify(c.perf)})`);
  ok(Object.keys(hits).slice(0, 3).every((k) => hits[k] === 1), `each receiver asked ONCE on load (${JSON.stringify(Object.fromEntries(Object.entries(hits).slice(0, 3)))})`);
  ok(asked.xcover[0].split(',').sort().join() === 'adsb,ais,aprs,hfdl', `…for the layers it shares and that are on (${asked.xcover[0]})`);
  await sleep(4000);
  await shot('community-all-groups-desktop.png');

  console.log('── the groups are independent; a group off keeps its chips ──');
  await js(`document.querySelector('[data-group=maritime]').click(); true`);
  c = await cm();
  ok(!c.byKind.ship && !c.byKind.navwarn && !c.byKind.dsc && c.byKind.aircraft && c.byKind.aprs, `MARITIME off: aviation and misc stay (${JSON.stringify(c.byKind)})`);
  await js(`document.querySelector('[data-mode=adsb]').click(); document.querySelector('[data-group=aviation]').click(); document.querySelector('[data-group=aviation]').click(); true`);
  c = await cm();
  ok(!c.on.includes('adsb') && c.on.includes('hfdl'), 'AVIATION off and on again: ADS-B stays off as it was left');
  await js(`document.querySelector('[data-mode=adsb]').click(); document.querySelector('[data-group=maritime]').click(); document.querySelector('[data-group=aviation]').click(); document.querySelector('[data-group=misc]').click(); true`);
  c = await cm();
  ok(JSON.stringify(c.groups) === '["maritime"]' && Object.keys(c.byKind).every((k) => ['ship', 'dsc', 'navst', 'navwarn', 'navmet', 'navmsg'].includes(k)), `MARITIME alone: ${Object.keys(c.byKind)}`);
  ok(await js(`localStorage.getItem('vibesdr.communityMap.v2')`) && JSON.parse(await js(`localStorage.getItem('vibesdr.communityMap.v2')`)).groups.join() === 'maritime', 'remembered per viewer (localStorage)');

  console.log('── NAVTEX: its own filters, a warning with its area, the text on opening, the MESSAGES list ──');
  ok(await js(`!document.getElementById('navbar').hidden && document.querySelectorAll('#navbar [data-navt]').length >= 3 && !!document.getElementById('navSt')`), 'the NAVTEX bar: layers, message types, station');
  const area = await js(`(() => { const w = [...ITEM_BY_KEY.values()].find(i => i.kind === 'navwarn' && i.d.shape === 'area'); return w && { key: w.key, ll: [w.lon, w.lat], pts: w.d.pts }; })()`);
  ok(!!area, `a NAVTEX warning with an AREA is on the map (${area && area.key})`);
  ok(await js(`window.__cmShapes.some(f => f.type === 'Polygon' && f.id === ${JSON.stringify(area && area.key)})`) && await js(`gl.queryRenderedFeatures({ layers: ['cm-area'] }).length`) >= 0, '…its area drawn as a polygon on the GPU layer');
  if (area) await js(`gl.fitBounds([[${Math.min(...area.pts.map((p) => p[1]))}, ${Math.min(...area.pts.map((p) => p[0]))}], [${Math.max(...area.pts.map((p) => p[1]))}, ${Math.max(...area.pts.map((p) => p[0]))}]], { padding: 160, duration: 0, maxZoom: 9 }); showCard(${JSON.stringify(area && area.key)}, ${JSON.stringify(area && area.ll)}); true`);
  ok(await until(`/THE MESSAGE TEXT FROM/.test((document.querySelector('.maplibregl-popup .nvTx') || {}).textContent || '')`, 8000), 'its card (the NAVTEX map’s own) fetches the message text from the receiver when opened');
  await sleep(2500);
  await shot('community-navtex-warning-area.png');
  await js(`document.querySelector('.maplibregl-popup-close-button')?.click(); true`);
  const types = await js(`[...document.querySelectorAll('#navbar [data-navt]')].map(b => b.dataset.navt)`);
  for (const t of types.filter((x) => x !== 'E')) await js(`document.querySelector('[data-navt="${t}"]').click(); true`);
  c = await cm();
  ok(c.byKind.navmet > 0 && !c.byKind.navwarn && (c.byKind.navmsg || 0) > 0, `TYPE E only: forecasts and their messages (${JSON.stringify(c.byKind)})`);
  for (const t of types.filter((x) => x !== 'E')) await js(`document.querySelector('[data-navt="${t}"]').click(); true`);
  await js(`(() => { const s = document.getElementById('navSt'); s.value = 'Niton'; s.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`);
  await until(`window.__cm.byKind.navst < 10`, 4000);
  c = await cm();
  ok(c.byKind.navst >= 1 && c.byKind.navst < 10 && await js(`[...ITEM_BY_KEY.values()].filter(i => i.modes[0] === 'navtex').every(i => (i.kind === 'navst' ? i.d.name : i.station) === 'Niton')`),
     `STATION Niton: its letters, its warnings and messages only (${JSON.stringify(c.byKind)})`);
  await js(`(() => { const s = document.getElementById('navSt'); s.value = ''; s.dispatchEvent(new Event('change', { bubbles: true })); return true; })()`);
  await js(`document.getElementById('msgBtn').click(); true`);
  ok(await until(`!document.getElementById('msgs').hidden && document.querySelectorAll('#msgList .msg[data-key]').length > 20`), 'MESSAGES lists the NAVTEX messages');
  await js(`document.querySelector('#msgList .msg[data-key]').click(); true`);
  ok(await until(`/THE MESSAGE TEXT FROM LENOVO/.test(document.querySelector('#msgList pre')?.textContent || '')`, 8000), '…a message opened shows its text, from the receiver with the CLEANEST copy');
  await shot('community-messages-desktop.png');
  await js(`document.querySelector('[data-close=msgs]').click(); for (const g of ['aviation', 'misc']) document.querySelector('[data-group=' + g + ']').click(); true`);

  console.log('── RECEIVERS: the list, LISTEN, picking one, ONLY THIS ──');
  await js(`document.getElementById('rxBtn').click(); true`);
  ok(await until(`!document.getElementById('rx').hidden && document.querySelectorAll('#rxList .row[data-id]').length === 3`), 'the list opens with three receivers');
  const rows = await js(`[...document.querySelectorAll('#rxList .row[data-id]')].map(r => ({ id: r.dataset.id, text: r.textContent, href: r.querySelector('a.listen')?.href }))`);
  const lv = rows.find((r) => r.id === 'lenovo'), pi = rows.find((r) => r.id === 'pi500');
  ok(lv && lv.href === 'https://lenovo.vibeserver.vibesdr.net/', `LISTEN is the directory's link (${lv && lv.href})`);
  ok(lv && /NAVTEX/.test(lv.text) && /SONDES/.test(lv.text) && /MESHCORE/.test(lv.text) && /on the map now/.test(lv.text) && /GB · IO92/.test(lv.text), 'a row: where, what it shares, how many items now');
  ok(pi && /no free slot/.test(pi.text) && /4\/4 listening/.test(pi.text), 'busy as the directory says it');
  await shot('community-receivers-desktop.png');
  await js(`document.querySelector('#rxList [data-only=xcover]').click(); true`);
  ok(await until(`window.__cm.server === 'xcover'`), 'ONLY THIS: the XCover');
  c = await cm();
  ok(Object.keys(c.byKind).every((k) => ['aircraft', 'ship', 'aprs'].includes(k)), `…what it heard only (${JSON.stringify(c.byKind)})`);
  await js(`document.getElementById('onlyChip').click(); document.querySelector('[data-close=rx]').click(); true`);

  console.log('── only what is switched on is fetched; a chip switched on fetches itself ──');
  await sleep(15000);
  ok(Object.keys(hits).slice(0, 3).every((k) => hits[k] === 1), `25 s on, still once each (${JSON.stringify(Object.fromEntries(Object.entries(hits).slice(0, 3)))})`);
  await js(`localStorage.setItem('vibesdr.communityMap.v2', JSON.stringify({ off: ['ais', 'sondes'] })); true`);
  resetHits();
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 20000), 'reloaded with AIS and SONDES switched off');
  ok(!asked.xcover.join().includes('ais') && !asked.lenovo.join().includes('sondes'), `…they are not asked for (${asked.xcover} / ${asked.lenovo})`);
  await js(`document.querySelector('[data-mode=ais]').click(); true`);
  ok(await until(`(window.__cm.byKind.ship || 0) === 2`, 8000) && asked.xcover.slice(-1)[0] === 'ais' && hits.lenovo === 1, `switched on: ONE request for AIS, to the receiver that shares it (${asked.xcover.join(' | ')})`);
  ok(listHits === 1, `/api/community once per load (${listHits})`);
  await js(`localStorage.removeItem('vibesdr.communityMap.v2'); true`);

  console.log('── a phone, 390 px ──');
  await call('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 20000), 'loads on a phone');
  ok(await js(`document.documentElement.scrollWidth <= 390`), `no sideways scroll (${await js('document.documentElement.scrollWidth')})`);
  await sleep(3500);
  await shot('community-all-groups-390.png');
  await js(`document.getElementById('rxBtn').click(); true`);
  const sheet = await js(`(() => { const r = document.getElementById('rx').getBoundingClientRect(); return { w: r.width, top: r.top }; })()`);
  ok(sheet.w === 390 && sheet.top > 200, `RECEIVERS opens as a sheet (${JSON.stringify(sheet)})`);
  await sleep(400);
  await shot('community-receivers-390.png');
  await call('Emulation.clearDeviceMetricsOverride');

  console.log(`── ${N} receivers: layered loading, clustering, the numbers ──`);
  listN = N; resetHits();
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === ${N}`, 30000), `all ${N} read`);
  c = await cm();
  await sleep(1500);
  const heap = await js(`performance.memory ? Math.round(performance.memory.usedJSHeapSize / 1048576) : -1`);
  ok(c.perf.listDraw > 0 && c.perf.full > 0 && c.perf.firstDraw > 0, `receivers listed ${c.perf.listDraw} ms · first receiver's data ${c.perf.firstData} ms · all ${N} in ${c.perf.full} ms · GPU map's first draw ${c.perf.firstDraw} ms (software GL, headless) · JS heap ${heap} MB`);
  await js(`gl.jumpTo({ center: [8, 52], zoom: 2.2 }); true`); await sleep(1500);
  const clusters = await js(`gl.queryRenderedFeatures({ layers: ['cm-clusters'] }).length`);
  ok(clusters > 0, `zoomed out, the dense layers cluster (${clusters} clusters drawn)`);
  await shot('community-10-receivers-clustered.png');
  await js(`gl.jumpTo({ center: [-1.2, 51.9], zoom: 8 }); true`); await sleep(1200);
  ok(await js(`gl.queryRenderedFeatures({ layers: ['cm-dense-pts'] }).length`) > 0, 'zoomed in, full markers');
  ok(Object.values(hits).every((n) => n === 1), `each of ${N} receivers asked once (${Object.values(hits).join(',')})`);
  console.log(`   (largest answer per receiver, bytes: ${JSON.stringify(sizes)})`);
  ok(errors.length === 0, `no script errors (${errors.slice(0, 3).join(' | ')})`);
} catch (e) { fail++; console.log('   FAIL ' + (e && e.stack || e)); }
clearTimeout(deadline);
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
