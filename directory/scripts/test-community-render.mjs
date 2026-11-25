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
import { answers, rebase, contributors, answerFor, radioAnswer, PI500_RADIOS } from './community-fixtures.mjs';

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
const hits = {}, asked = {}, sizes = {}, front = {}, byRadio = {};
const ports = {};
const MULTI = new Set(contributors(() => '', N).servers.filter((s) => s.radios).map((s) => s.id));   // ★ the Pi 500 and its copies
const want = (id) => (MULTI.has(id) ? PI500_RADIOS.length : 1);   // requests per load: one per radio, ONE for a single-radio server
for (const id of Object.keys(A)) {
  hits[id] = 0; asked[id] = []; front[id] = 0; byRadio[id] = {};
  const srv = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const h = { 'content-type': 'application/json', 'access-control-allow-origin': '*', 'cache-control': 'public, max-age=30' };
    const rm = /^(?:\/r\/([A-Za-z0-9_-]+))?\/vibeserver\/community\.json$/.exec(u.pathname);
    const ans = rm && radioAnswer(A[id], rm[1] || '', MULTI.has(id));
    if (rm && ans) {
      hits[id]++; asked[id].push(u.searchParams.get('s') || '');
      if (rm[1]) byRadio[id][rm[1]] = (byRadio[id][rm[1]] || 0) + 1; else front[id]++;
      const body = JSON.stringify(answerFor(rebase(ans, Math.floor(Date.now() / 1000)), u.searchParams.get('s')));
      sizes[id] = Math.max(sizes[id] || 0, body.length);
      res.writeHead(200, h); res.end(body);
    } else if (u.pathname === '/vibeserver/community/navtex') {
      res.writeHead(200, h); res.end(JSON.stringify({ f: u.searchParams.get('f'), txt: `ZCZC TEST\nTHE MESSAGE TEXT FROM ${id.toUpperCase()} (${u.searchParams.get('f')})\nNNNN\n` }));
    } else { res.writeHead(404); res.end(); }
  });
  ports[id] = await listen(srv);
}

// ── the directory stand-in: ?n= receivers in its list (3 by default) ──
let listHits = 0, seaHits = 0;
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
  if (p === '/community-sea-areas.json') seaHits++;
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
const resetHits = () => { for (const k of Object.keys(hits)) { hits[k] = 0; asked[k] = []; front[k] = 0; byRadio[k] = {}; } listHits = 0; };
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
  ok(Object.keys(hits).slice(0, 3).every((k) => hits[k] === want(k)), `each receiver asked ONCE on load — the two-radio-plus Pi 500 once PER RADIO (${JSON.stringify(Object.fromEntries(Object.entries(hits).slice(0, 3)))})`);
  ok(hits.lenovo === 1 && hits.xcover === 1 && front.lenovo === 1 && front.xcover === 1, '★ a single-radio server: exactly ONE request, at its front door');
  ok(front.pi500 === 0 && PI500_RADIOS.every((r) => byRadio.pi500[r] === 1), `★★ the multi-radio Pi 500: each radio at its own /r/<id>/, the front door not asked twice (${JSON.stringify(byRadio.pi500)})`);
  ok(await js(`DATA.get('pi500').radios.size`) === PI500_RADIOS.length, `★★ every radio's answer kept (they land in parallel: none may overwrite another) — ${await js(`DATA.get('pi500').radios.size`)} of ${PI500_RADIOS.length}`);
  ok(await js(`[...ITEMS].some(i => i.kind === 'aircraft' && i.heard.some(h => h.sid === 'pi500')) && [...ITEMS].some(i => i.kind === 'dsc' && i.heard.some(h => h.sid === 'pi500'))`) && c.up === 3, `★★ its aircraft (from the V4s, not the front door's DSC radio) are on the map, the server counted ONCE (${c.up} of 3 sharing)`);
  ok(await js(`(() => { const p = [...ITEMS].filter(i => i.heard.some(h => h.sid === 'pi500')); const keys = p.map(i => i.key); return keys.length === new Set(keys).size && p.every(i => i.heard.filter(h => h.sid === 'pi500').length === 1); })()`),
     '★ the two identical V4 answers merged: one item per key, the Pi 500 once in each "heard by"');
  ok(asked.xcover[0].split(',').sort().join() === 'adsb,ais,aprs,hfdl', `…for the layers it shares and that are on (${asked.xcover[0]})`);
  await sleep(4000);
  await shot('community-all-groups-desktop.png');

  console.log('── the SEA AREAS: the server map\'s outlines and names, one static file, under the markers ──');
  ok(await js(`(() => { const b = document.querySelector('#modes [data-sea]'); return !!b && b.getAttribute('aria-pressed') === 'true' && b.closest('.set') === document.querySelector('[data-mode=ais]').closest('.set') && /SEA AREAS/.test(b.textContent); })()`),
     'a SEA AREAS chip in the MARITIME row, ON by default');
  ok(await until(`window.__cm.sea && window.__cm.sea.drawn`, 10000), `drawn: ${JSON.stringify(await js('window.__cm.sea && { areas: __cm.sea.areas, bytes: __cm.sea.bytes }'))}`);
  ok(seaHits === 1, `the sea-areas file fetched ONCE, from beside the page (${seaHits})`);
  const order = await js(`gl.getStyle().layers.map(l => l.id)`);
  ok(order.indexOf('cm-sea-name-s') < order.indexOf('cm-area') && order.indexOf('cm-sea-line-s') < order.indexOf('cm-dense-pts') && order.indexOf('cm-sea-red') < order.indexOf('cm-rx'), 'its layers sit UNDER the warnings, the markers and the receivers');
  await js(`gl.jumpTo({ center: [-4, 55.5], zoom: 4.6 }); true`); await sleep(2500);
  const seaIds = await js(`[...new Set(gl.queryRenderedFeatures({ layers: ['cm-sea-line-s', 'cm-sea-line-c', 'cm-sea-red', 'cm-sea-amber'] }).map(f => f.properties.id))]`);
  ok(['dogger', 'fisher', 'shannon', 'rockall', 'malin', 'irishsea', 'gbight'].every((x) => seaIds.includes(x)), `around the UK, Ireland and the North Sea: ${seaIds.length} areas outlined (${seaIds.slice(0, 12).join(', ')}…)`);
  const names = await js(`gl.queryRenderedFeatures({ layers: ['cm-sea-name-s', 'cm-sea-name-o', 'cm-sea-name-c'] }).map(f => f.properties.id)`);
  const unnamed = await js(`window.__cm.sea.unnamed`);
  ok(names.length >= 5 && unnamed.length > 0 && !names.some((x) => unnamed.includes(x)), `names drawn (${names.length}); none where a NAVTEX forecast arrow labels the area (${unnamed.join(', ')})`);
  ok(/Met Office/.test(await js(`document.querySelector('.maplibregl-ctrl-attrib')?.textContent || ''`)), '…and the attribution line says so');
  await sleep(1500);
  await shot('community-sea-areas.png');
  await js(`document.querySelector('#modes [data-sea]').click(); true`);
  ok(await until(`window.__cm.sea && !window.__cm.sea.on && gl.getLayoutProperty('cm-sea-line-s', 'visibility') === 'none'`, 4000)
     && JSON.parse(await js(`localStorage.getItem('vibesdr.communityMap.v2')`)).seaAreas === false, 'SEA AREAS off: hidden, and remembered per viewer');
  await sleep(600);
  ok((await js(`gl.queryRenderedFeatures({ layers: ['cm-sea-line-s', 'cm-sea-name-s'] }).length`)) === 0, '…nothing of it drawn');
  await js(`document.querySelector('#modes [data-sea]').click(); true`);
  ok(await until(`window.__cm.sea.on && gl.getLayoutProperty('cm-sea-line-s', 'visibility') === 'visible'`, 4000) && seaHits === 1, '…on again, from the file already in (no second fetch)');
  await js(`document.querySelector('[data-group=maritime]').click(); true`);
  ok(await until(`!window.__cm.sea.on`, 3000), 'MARITIME off takes the sea areas with it');
  await js(`document.querySelector('[data-group=maritime]').click(); true`);

  console.log('── FIT: every item drawn now, as the server maps\' key ──');
  ok(await js(`(() => { const b = document.getElementById('fitBtn'), r = b.getBoundingClientRect(), m = document.getElementById('map').getBoundingClientRect(); return b.textContent === 'FIT' && Math.abs(m.right - r.right - 10) < 2 && Math.abs(r.top - m.top - 10) < 2; })()`), 'a FIT key, top right of the map');
  const inBox = `(() => { const b = gl.getBounds(); const pts = fitPoints(); return pts.length > 0 && pts.every(([lo, la]) => b.contains([lo, la])); })()`;
  await js(`gl.jumpTo({ center: [120, -30], zoom: 7 }); true`); await sleep(300);
  ok(!(await js(inBox)), '(moved away: not everything in view)');
  await js(`document.getElementById('fitBtn').click(); true`); await sleep(1200);
  ok(await js(inBox), `FIT: all ${await js('fitPoints().length')} drawn points in view`);
  await js(`document.querySelector('[data-group=aviation]').click(); document.querySelector('[data-group=misc]').click(); true`); await sleep(300);
  await js(`document.getElementById('fitBtn').click(); true`); await sleep(1200);
  const shipBox = await js(`window.__cmFit`);
  ok(await js(inBox) && await js(`fitPoints().length < ${JSON.stringify(c.shown)}`), `…MARITIME alone: FIT frames only what it draws (${shipBox.n} points, box ${shipBox.box.map((x) => x.toFixed(1))})`);
  await js(`document.querySelector('[data-age="900"]').click(); true`); await sleep(300);
  const n15 = await js(`fitPoints().length`);
  ok(n15 <= shipBox.n, `…and keeps to the HEARD IN window (15 min: ${n15} points)`);
  await js(`document.querySelector('[data-age="86400"]').click(); document.querySelector('[data-group=aviation]').click(); document.querySelector('[data-group=misc]').click(); true`);

  console.log('── the receivers wear the directory map\'s radio ──');
  ok(await js(`/M14\.2 11\.6v6\.4/.test(RX_SVG({ listeners: 0, maxListeners: 4 }, false)) && /#28A745/.test(RX_SVG({ listeners: 0, maxListeners: 4 }, false)) && /#e05050/.test(RX_SVG({ listeners: 4, maxListeners: 4 }, false))
     && /#ffd23f/.test(RX_SVG({ pin: true }, false)) && /#ffb833/.test(RX_SVG({}, true))`), 'the radio glyph, in the directory’s state colours (padlock for a PIN; amber plate when selected)');
  await js(`gl.jumpTo({ center: [-1, 51.6], zoom: 6.3 }); true`); await sleep(1500);
  ok((await js(`gl.queryRenderedFeatures({ layers: ['cm-rx'] }).length`)) === 3, 'three receivers drawn at their grid centres');
  await js(`pick('pi500', true); true`); await sleep(1500);
  ok(await js(`document.querySelector('#rxList .row.sel')?.dataset.id === 'pi500' && !document.getElementById('rx').hidden`), 'a receiver chosen: selected in the RECEIVERS panel');
  await shot('community-receivers-radio-glyph.png');
  await js(`document.querySelector('[data-close=rx]').click(); selected = ''; render(); true`);

  console.log('── the pills\' totals ──');
  await sleep(500);
  const pills = await js(`Object.fromEntries([...document.querySelectorAll('#modes [data-mode]')].map(b => [b.dataset.mode, b.querySelector('.n')?.textContent]))`);
  c = await cm();
  ok(Object.values(pills).every((v) => /^\d+$/.test(v)) && Number(pills.ais) === c.byKind.ship && Number(pills.dsc) === c.byKind.dsc && Number(pills.ndb) === c.byKind.ndb && Number(pills.sondes) === c.byKind.sonde
     && Number(pills.aprs) === c.byKind.aprs && Number(pills.mesh) === c.byKind.mesh && Number(pills.navtex) === (c.byKind.navst || 0) + (c.byKind.navwarn || 0) + (c.byKind.navmet || 0),
     `a total in every pill, the status line's numbers (${JSON.stringify(pills)})`);
  const stl = await js(`document.getElementById('status').textContent`);
  ok(/\b4 aircraft \(all sources combined\)/.test(stl) && new RegExp(`\\b${Number(pills.ais) + Number(pills.dsc)} ships \\(AIS \\+ DSC combined\\)`).test(stl) && !/\d DSC\b/.test(stl),
     `the status line gives COMBINED totals, the pills their breakdown (${stl.slice(0, 120)}…)`);
  ok(['adsb', 'acars', 'vdl2', 'hfdl'].reduce((a, m) => a + Number(pills[m] || 0), 0) === c.byKind.aircraft, `…the aviation pills add up to the aircraft on the map (${c.byKind.aircraft}) — each counted once`);
  const navp = await js(`Object.fromEntries([...document.querySelectorAll('#navbar [data-navl]')].map(b => [b.dataset.navl, Number(b.querySelector('.n')?.textContent)]))`);
  ok(navp.stations === c.byKind.navst && navp.warnings === c.byKind.navwarn && navp.weather === c.byKind.navmet && navp.messages === c.byKind.navmsg, `…and in NAVTEX's own (${JSON.stringify(navp)})`);
  await js(`document.querySelector('[data-age="900"]').click(); true`); await sleep(400);
  const p15 = await js(`Object.fromEntries([...document.querySelectorAll('#modes [data-mode]')].map(b => [b.dataset.mode, b.querySelector('.n')?.textContent]))`);
  ok(Number(p15.ndb) <= Number(pills.ndb) && Number(p15.navtex) < Number(pills.navtex) && await js(`!!document.querySelector('#modes .n.z')`), `…live with HEARD IN (15 min: ${JSON.stringify(p15)}); a 0 is shown dim, the pill stays`);
  await js(`document.querySelector('[data-age="86400"]').click(); document.querySelector('[data-mode=ais]').click(); true`); await sleep(300);
  ok(await js(`document.querySelector('[data-mode=ais] .n').textContent`) === pills.ais, '…a pill switched off still says what it would show');
  await js(`document.querySelector('[data-mode=ais]').click(); true`);

  console.log('── the KEY ──');
  ok(await js(`(() => { const b = document.getElementById('keyBtn'), r = b.getBoundingClientRect(), m = document.getElementById('map').getBoundingClientRect(); return b.textContent === 'KEY' && Math.abs(m.right - r.right - 10) < 2 && r.bottom > m.bottom - 80 && document.getElementById('key').hidden; })()`), 'a KEY button, bottom right; the key closed');
  await js(`document.getElementById('keyBtn').click(); true`); await sleep(300);
  const secs = await js(`[...document.querySelectorAll('#key .ks')].map(e => e.textContent)`);
  ok(['RECEIVERS', 'AIRCRAFT', 'NDB', 'SHIPS (AIS)', 'DSC POSITIONS', 'NAVTEX STATIONS', 'NAVTEX WARNINGS', 'NAVTEX WEATHER', 'SEA AREAS', 'RADIOSONDES', 'APRS', 'MESHCORE'].every((x) => secs.includes(x)) && !secs.includes('VOR / ILS'),
     `a section for each layer on, none for one nobody shares (${secs.join(' · ')})`);
  ok(await js(`document.querySelectorAll('#key .kr svg').length`) > 40 && /Gale force 8/.test(await js(`document.getElementById('key').textContent`)) && /Ship still/.test(await js(`document.getElementById('key').textContent`)),
     '…drawn with the server maps’ own rows and glyphs (ship, DSC, hazards, wind arrows, sea areas)');
  await shot('community-key-desktop.png');
  await js(`document.querySelector('[data-group=aviation]').click(); true`); await sleep(300);
  const secs2 = await js(`[...document.querySelectorAll('#key .ks')].map(e => e.textContent)`);
  ok(!secs2.includes('AIRCRAFT') && !secs2.includes('NDB') && secs2.includes('SHIPS (AIS)'), 'AVIATION off: its sections leave the key');
  await js(`document.querySelector('[data-group=aviation]').click(); document.getElementById('keyBtn').click(); true`);
  ok(await js(`document.getElementById('key').hidden`), '…KEY again closes it');

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
  ok(Object.keys(hits).slice(0, 3).every((k) => hits[k] === want(k)), `25 s on, still once each (per radio) (${JSON.stringify(Object.fromEntries(Object.entries(hits).slice(0, 3)))})`);
  await js(`localStorage.setItem('vibesdr.communityMap.v2', JSON.stringify({ off: ['ais', 'sondes'] })); true`);
  resetHits();
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 20000), 'reloaded with AIS and SONDES switched off');
  ok(!asked.xcover.join().includes('ais') && !asked.lenovo.join().includes('sondes'), `…they are not asked for (${asked.xcover} / ${asked.lenovo})`);
  await js(`document.querySelector('[data-mode=ais]').click(); true`);
  ok(await until(`(window.__cm.byKind.ship || 0) === 2`, 8000) && asked.xcover.slice(-1)[0] === 'ais' && hits.lenovo === 1 && hits.pi500 === want('pi500'), `switched on: ONE request for AIS, to the receiver that shares it (${asked.xcover.join(' | ')})`);
  ok(listHits === 1, `/api/community once per load (${listHits})`);
  await js(`localStorage.removeItem('vibesdr.communityMap.v2'); true`);

  console.log('── a phone, 390 px ──');
  await call('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 20000), 'loads on a phone');
  ok(await js(`document.documentElement.scrollWidth <= 390`), `no sideways scroll (${await js('document.documentElement.scrollWidth')})`);
  await sleep(3500);
  await shot('community-all-groups-390.png');
  await js(`document.getElementById('keyBtn').click(); true`); await sleep(400);
  ok(await js(`(() => { const r = document.getElementById('key').getBoundingClientRect(); return r.left >= 0 && r.right <= 390 && document.documentElement.scrollWidth <= 390; })()`), 'the KEY fits a phone (no sideways scroll)');
  await shot('community-key-390.png');
  await js(`document.getElementById('keyBtn').click(); true`);
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
  ok(Object.entries(hits).every(([k, n]) => n === want(k)), `each of ${N} receivers asked once (per radio) (${Object.values(hits).join(',')})`);
  console.log(`   (largest answer per receiver, bytes: ${JSON.stringify(sizes)})`);
  ok(errors.length === 0, `no script errors (${errors.slice(0, 3).join(' | ')})`);
} catch (e) { fail++; console.log('   FAIL ' + (e && e.stack || e)); }
clearTimeout(deadline);
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
