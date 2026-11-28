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
import { chromeLayoutExpr, setInsetsExpr, MAP_SIZES } from '../../scripts/lib/map-chrome-layout.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const CM_AVI = new Set(['adsb', 'acars', 'vdl2', 'hfdl', 'ndb', 'vor']);   // ★★ 11-28 the aviation group: distances in NM
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
const hits = {}, asked = {}, sizes = {}, front = {}, byRadio = {}, tableHits = {}, trailHits = [];
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
    } else if (u.pathname === '/vibeserver/aircraft.json') {
      // ★ the machine's aircraft table (the route): the XCover has the HFDL aircraft's own datalink from/to; nobody a route for BAW69x
      tableHits[id] = (tableHits[id] || 0) + 1;
      const rows = id === 'xcover' ? [{ key: '4CA000', flight: 'BAW696' }, { key: 'A1B2C3', flight: 'UAL917', from: 'EGLL', to: 'KIAD' }] : [{ key: '4CA000', flight: 'BAW696' }];
      res.writeHead(200, h); res.end(JSON.stringify({ now: Math.floor(Date.now() / 1000), count: rows.length, aircraft: rows }));
    } else if (/^(?:\/r\/[A-Za-z0-9_-]+)?\/adsb\/trail\/[0-9a-f]{6}$/.test(u.pathname)) {
      // ★ the ADS-B trail is per RADIO: only the Pi 500's last V4 decodes ADS-B; everyone else knows nothing (empty pts)
      trailHits.push(id + u.pathname);
      const mine = id === 'pi500' && u.pathname.startsWith('/r/' + PI500_RADIOS[2] + '/') && u.pathname.endsWith('/4ca000');
      // 0.3 NM steps west of Cambridge, a 60 NM hole (lost behind the hills — dashed), then on to the position
      const pts = mine ? [...Array.from({ length: 20 }, (_, i) => [51.2, -3.5 + i * 0.008, 30000 + i * 100]), ...Array.from({ length: 20 }, (_, i) => [51.95, -1.0 + i * 0.05, 35000])] : [];
      res.writeHead(200, h); res.end(JSON.stringify({ type: 'adsb_trail', hex: u.pathname.slice(-6), pts }));
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

// ── a stand-in for adsbdb.com (NO live calls in a test — the real one is blocked below): its own answer shapes ──
const routeAsks = [];
const APT = { LHR: ['EGLL', 51.4706, -0.461941, 'London', 'London Heathrow Airport'], FRA: ['EDDF', 50.0333, 8.57046, 'Frankfurt am Main', 'Frankfurt am Main Airport'],
              MAD: ['LEMD', 40.4719, -3.56264, 'Madrid', 'Adolfo Suárez Madrid–Barajas Airport'], LIS: ['LPPT', 38.7813, -9.13592, 'Lisbon', 'Humberto Delgado Airport'] };
const apt = (k) => ({ iata_code: k, icao_code: APT[k][0], latitude: APT[k][1], longitude: APT[k][2], municipality: APT[k][3], name: APT[k][4], country_iso_name: 'XX' });
const ROUTES_MOCK = { BAW696: ['LHR', 'FRA'], BAW698: ['MAD', 'LIS'] };   // BAW697: unknown
const routeSrv = http.createServer((req, res) => {
  const cs = decodeURIComponent(req.url.replace(/^\/v0\/callsign\//, ''));
  routeAsks.push(cs);
  const r = ROUTES_MOCK[cs];
  res.writeHead(r ? 200 : 404, { 'content-type': 'application/json', 'access-control-allow-origin': '*' });
  res.end(JSON.stringify(r ? { response: { flightroute: { callsign: cs, airline: { name: 'British Airways' }, origin: apt(r[0]), destination: apt(r[1]) } } } : { response: 'unknown callsign' }));
});
const RP = await listen(routeSrv);
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
await call('Network.enable'); await call('Network.setBlockedURLs', { urls: ['*adsbdb.com*', '*hexdb.io*'] });   // ★ never the live route source
const mockRoutes = () => js(`CommunityMap.ROUTE.api = 'http://127.0.0.1:${RP}/v0/callsign/'; true`);
// ★★★ b97 THE CARD SYSTEM (map-chrome.js): the old header rows, bars and panels are gone — the drivers press the chips' own
//     buttons (they exist while a card is shut: parked, not removed). AUTO keeps the map framed, so a driver that moves the
//     map by code holds it first (a code jumpTo is not a person's gesture — AUTO would glide straight back).
const tgl = (id) => js(`(() => { const b = document.querySelector('.trkCard2 [data-tgl="${id}"]'); if (!b) return false; b.click(); return true; })()`);
const tglOn = (id) => js(`document.querySelector('.trkCard2 [data-tgl="${id}"]')?.getAttribute('aria-pressed')`);
const hold = () => js(`MC.fitPause(); true`);
const card = (id, on = true) => js(`MC.card('${id}', ${on}); true`);
const cardOpen = (id) => js(`(() => { const c = document.querySelector('.trkCards .trkCard2[data-card="${id}"]'); return !!c && !c.hidden && c.getBoundingClientRect().height > 0; })()`);
const keyText = () => js(`document.querySelector('.trkKey .kb').textContent`);

try {
  console.log('── 3 receivers: the page reads them directly; all three groups on together ──');
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 25000), `all three receivers read (${await js('window.__cm && window.__cm.up')})`);
  let c = await cm();
  await mockRoutes();
  ok(JSON.stringify(c.groups) === JSON.stringify(['aviation', 'maritime', 'misc']), 'AVIATION, MARITIME and MISC all on by default');
  ok(await js(`['aviation', 'maritime', 'misc'].map(g => document.querySelector('.trkCard2 [data-tgl="g:' + g + '"]')?.getAttribute('aria-pressed')).join()`) === 'true,true,true', 'three independent group toggles (in LAYERS)');
  ok(JSON.stringify(c.modes) === JSON.stringify(['adsb', 'hfdl', 'ndb', 'ais', 'dsc', 'navtex', 'sondes', 'aprs', 'mesh']), `a chip only for what is shared (${c.modes})`);
  // ★★ 11-28 (Stuart: "the MeshCore layer is shown with a plain GREEN DOT") — its chip and its card row wear the map's own MeshCore marker
  const mg = await js(`(() => { const mk = KIT.ismMapIcon({ kind: 'meshcore', ntype: 'repeater', seen: 0 }).html;
    const les = [...document.querySelectorAll('.mDock[data-dock="layers"] .lsum .le')];
    const le = les.find((e) => /MeshCore/.test(e.title || ''));
    const row = document.querySelector('.trkCard2 [data-tgl="mesh"]');
    const inner = (h) => { const d = document.createElement('div'); d.innerHTML = h; const g = d.querySelector('svg'); return g ? g.innerHTML : ''; };   // the DOM's own serialisation, both sides
    return { chip: !!le && inner(le.querySelector('.lg').innerHTML) === inner(mk) && /#ff9f43/.test(le.innerHTML) && !le.querySelector('circle[r="6"]'),
             row: !!row && !!row.querySelector('.lgm svg') && inner(row.querySelector('.lgm').innerHTML) === inner(mk) && !row.querySelector('.dot'),
             air: !!document.querySelector('.trkCard2 [data-tgl="adsb"] .dot') }; })()`);
  ok(mg.chip, '★★ MESHCORE in the LAYERS chip: the map’s own MeshCore marker (orange box + mast, the kit’s ismMapIcon), not a dot');
  ok(mg.row && mg.air, '★★ …and its LAYERS card row wears that marker in the dot’s place (the aircraft rows keep their dots)');
  ok(await js(`CM.MODES.filter(m => window.__cm.modes.includes(m.id)).every(m => /shared for the last/.test(document.querySelector('.trkCard2 [data-tgl="' + m.id + '"]')?.title || ''))`), 'each LAYERS chip’s tooltip says its window');
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
  // ★★ beta96 EACH AIRCRAFT ITS OWN GLYPH (Stuart 11-26: "the correct icon for the aircraft type like the ADS-B map"): the
  //    ADS-B map's classes (KIT = adsblayer.js iconClassOf + its shapes), from a beta96 row's aircraft data (field 11) or, from
  //    an older server (the XCover's rows: no field 11), the type alone; the merge never blanks what another server knew
  const acs = await js(`(() => { const rev = new Map([...iconNames].map(([svg, n]) => [n, svg])); const f = gl.getSource('cm-d-aircraft').serialize().data.features;
    const o = {}; for (const k of ['a:4CA000', 'a:4CA001', 'a:4CA002', 'a:A1B2C3']) { const it = ITEM_BY_KEY.get(k), ft = f.find((x) => x.properties.id === k);
      const col = CM.MODE[CM.primaryMode(it, F.off)].colour, cls = acClass(it.d);
      o[k] = { cls, glyph: !!ft && rev.get(ft.properties.ic) === KIT.aircraftIconSvg(cls, col, 34, 0), rot: ft && ft.properties.rot, type: it.d.type, wtc: it.d.wtc || '' }; }
    return o; })()`);
  ok(acs && acs['a:4CA000'].cls === 'heavy' && acs['a:4CA000'].type === 'B77W' && acs['a:4CA000'].wtc === 'H', `★★ BAW696 (B77W, wake H — a beta96 server's field 11) draws the HEAVY glyph; the XCover's newer row with no type did not blank it (${JSON.stringify(acs && acs['a:4CA000'])})`);
  ok(acs && acs['a:4CA001'].cls === 'heli', `★★ an EC35 (description letter H) draws the HELICOPTER (${JSON.stringify(acs && acs['a:4CA001'])})`);
  ok(acs && acs['a:4CA002'].cls === 'jet', `nothing known: the jet (${JSON.stringify(acs && acs['a:4CA002'])})`);
  ok(acs && acs['a:A1B2C3'].cls === 'heavy', `★ an older server's row (no field 11): its type alone, B77W → heavy (${JSON.stringify(acs && acs['a:A1B2C3'])})`);
  ok(acs && Object.values(acs).every((a) => a.glyph), '★ every one is DRAWN with its class\'s own shape from the kit, in its mode\'s colour, on the GPU layer');
  ok(acs && acs['a:4CA000'].rot === 92 && acs['a:4CA001'].rot === 90 && acs['a:A1B2C3'].rot === 0, `…pointing along its track (the newest fix's 92°; no track = north): ${acs && acs['a:4CA000'].rot}, ${acs && acs['a:A1B2C3'].rot}`);
  ok(await js(`(() => { const k = document.querySelector('.trkKey .kb'); return !document.querySelector('.keySect').hidden && /Heavy \\(wide-body\\)/.test(k.textContent) && /Helicopter or gyroplane/.test(k.textContent) && /Light aircraft/.test(k.textContent) && k.querySelectorAll('svg[viewBox="0 0 40 40"]').length >= 11; })()`),
     '★ the KEY (in MAP CONTROLS) shows the glyph classes, drawn from the kit');

  await sleep(4000);
  await shot('community-all-groups-desktop.png');

  console.log('── the SEA AREAS: the server map\'s outlines and names, one static file, under the markers ──');
  ok(await js(`(() => { const b = document.querySelector('.trkCard2 [data-tgl="sea"]'), all = [...document.querySelectorAll('.trkCard2 [data-tgl]')].map(x => x.dataset.tgl);
     return !!b && b.getAttribute('aria-pressed') === 'true' && all.indexOf('sea') > all.indexOf('g:maritime') && all.indexOf('sea') < all.indexOf('g:misc') && /SEA AREAS/.test(b.textContent); })()`),
     'a SEA AREAS chip in LAYERS, in MARITIME, ON by default');
  ok(await until(`window.__cm.sea && window.__cm.sea.drawn`, 10000), `drawn: ${JSON.stringify(await js('window.__cm.sea && { areas: __cm.sea.areas, bytes: __cm.sea.bytes }'))}`);
  ok(seaHits === 1, `the sea-areas file fetched ONCE, from beside the page (${seaHits})`);
  const order = await js(`gl.getStyle().layers.map(l => l.id)`);
  ok(order.indexOf('cm-sea-name-s') < order.indexOf('cm-area') && order.indexOf('cm-sea-line-s') < order.indexOf('cm-dp-mesh') && order.indexOf('cm-sea-red') < order.indexOf('cm-rx'), 'its layers sit UNDER the warnings, the markers and the receivers');
  await hold(); await js(`gl.jumpTo({ center: [-4, 55.5], zoom: 4.6 }); true`); await sleep(2500);
  const seaIds = await js(`[...new Set(gl.queryRenderedFeatures({ layers: ['cm-sea-line-s', 'cm-sea-line-c', 'cm-sea-red', 'cm-sea-amber'] }).map(f => f.properties.id))]`);
  ok(['dogger', 'fisher', 'shannon', 'rockall', 'malin', 'irishsea', 'gbight'].every((x) => seaIds.includes(x)), `around the UK, Ireland and the North Sea: ${seaIds.length} areas outlined (${seaIds.slice(0, 12).join(', ')}…)`);
  const names = await js(`gl.queryRenderedFeatures({ layers: ['cm-sea-name-s', 'cm-sea-name-o', 'cm-sea-name-c'] }).map(f => f.properties.id)`);
  const unnamed = await js(`window.__cm.sea.unnamed`);
  ok(names.length >= 5 && unnamed.length > 0 && !names.some((x) => unnamed.includes(x)), `names drawn (${names.length}); none where a NAVTEX forecast arrow labels the area (${unnamed.join(', ')})`);
  ok(/Met Office/.test(await js(`document.querySelector('.maplibregl-ctrl-attrib')?.textContent || ''`)), '…and the attribution line says so');
  await sleep(1500);
  await shot('community-sea-areas.png');
  await tgl('sea');
  ok(await until(`window.__cm.sea && !window.__cm.sea.on && gl.getLayoutProperty('cm-sea-line-s', 'visibility') === 'none'`, 4000)
     && JSON.parse(await js(`localStorage.getItem('vibesdr.communityMap.v2')`)).seaAreas === false, 'SEA AREAS off: hidden, and remembered per viewer');
  await sleep(600);
  ok((await js(`gl.queryRenderedFeatures({ layers: ['cm-sea-line-s', 'cm-sea-name-s'] }).length`)) === 0, '…nothing of it drawn');
  await tgl('sea');
  ok(await until(`window.__cm.sea.on && gl.getLayoutProperty('cm-sea-line-s', 'visibility') === 'visible'`, 4000) && seaHits === 1, '…on again, from the file already in (no second fetch)');
  await tgl('g:maritime');
  ok(await until(`!window.__cm.sea.on`, 3000), 'MARITIME off takes the sea areas with it');
  await tgl('g:maritime');

  console.log('── AUTO / FIT (MAP CONTROLS): every item drawn now, the shared auto fit of every server map ──');
  ok(await js(`(() => { const a = document.querySelector('.trkCard2[data-card="map"] .fit'), f = document.querySelector('.trkCard2[data-card="map"] .fitOnce'); return !!a && !!f && !a.hidden && !f.hidden && f.textContent === 'FIT'; })()`), 'AUTO and FIT keys in MAP CONTROLS (the map\'s own auto fit)');
  // ★ the auto fit leaves out the furthest few per cent and pads for the chips: "in view" = all but 5 % of the points
  const inBox = `(() => { const b = gl.getBounds(); const pts = fitPoints(); const n = pts.filter(([lo, la]) => b.contains([lo, la])).length; return pts.length > 0 && n >= Math.floor(pts.length * 0.95); })()`;
  await hold(); await js(`gl.jumpTo({ center: [120, -30], zoom: 7 }); true`); await sleep(300);
  ok(!(await js(inBox)), '(moved away: not everything in view)');
  await js(`document.querySelector('.trkCard2[data-card="map"] .fitOnce').click(); true`);
  ok(await until(inBox, 6000), `FIT: the ${await js('fitPoints().length')} drawn points in view (the glide)`);
  await tgl('g:aviation'); await tgl('g:misc'); await sleep(300);
  await js(`document.querySelector('.trkCard2[data-card="map"] .fitOnce').click(); true`);
  ok(await until(inBox, 6000) && await js(`fitPoints().length < ${JSON.stringify(c.shown)}`), `…MARITIME alone: FIT frames only what it draws (${await js('fitPoints().length')} points)`);
  const nAll = await js(`fitPoints().length`);
  await js(`document.querySelector('[data-rng="900"]').click(); true`); await sleep(300);
  const n15 = await js(`fitPoints().length`);
  ok(n15 <= nAll && await js(`F.maxAge === 900`), `…and keeps to the HEARD IN window (TIME in MAP CONTROLS; 15 min: ${n15} points)`);
  await js(`document.querySelector('[data-rng="86400"]').click(); true`); await tgl('g:aviation'); await tgl('g:misc');

  console.log('── the receivers wear the directory map\'s radio ──');
  ok(await js(`/M14\.2 11\.6v6\.4/.test(RX_SVG({ listeners: 0, maxListeners: 4 }, false)) && /#28A745/.test(RX_SVG({ listeners: 0, maxListeners: 4 }, false)) && /#e05050/.test(RX_SVG({ listeners: 4, maxListeners: 4 }, false))
     && /#ffd23f/.test(RX_SVG({ pin: true }, false)) && /#ffb833/.test(RX_SVG({}, true))`), 'the radio glyph, in the directory’s state colours (padlock for a PIN; amber plate when selected)');
  await hold(); await js(`gl.jumpTo({ center: [-1, 51.6], zoom: 6.3 }); true`); await sleep(1500);
  ok((await js(`gl.queryRenderedFeatures({ layers: ['cm-rx'] }).length`)) === 3, 'three receivers drawn at their grid centres');
  await js(`pick('pi500', true); true`); await sleep(1500);
  ok(await js(`document.querySelector('.trkCard2[data-card="servers"] .rxRow.sel')?.dataset.id === 'pi500'`) && await cardOpen('servers'), 'a receiver chosen: selected in the RECEIVERS card, which opens');
  await shot('community-receivers-radio-glyph.png');
  await card('servers', false); await js(`selected = ''; render(); true`);

  console.log('── a click picks the point over the area under it ──');
  {
    // ★ Stuart 11-26: an aircraft over a NAVTEX area opened the AREA's card. Find any point drawn over an area and click it.
    await js(`MC.fitNow(); true`); await sleep(3000); await hold();
    const r = await js(`(() => {
      const pts = gl.queryRenderedFeatures({ layers: ['cm-pts'].concat(densePtLayers()).filter(l => gl.getLayer(l)) });
      for (const f of pts) {
        const p = gl.project(f.geometry.coordinates);
        if (!gl.queryRenderedFeatures(p, { layers: ['cm-area'] }).length) continue;
        gl.fire('click', { point: p, lngLat: gl.unproject(p), originalEvent: {} });
        const html = document.querySelector('.maplibregl-popup')?.innerHTML || '';
        return { found: true, id: f.properties.id, html: html.slice(0, 400) };
      }
      return { found: false };
    })()`);
    if (r.found) ok(r.html && !/Drawn as an area/.test(r.html), 'a point over an area opens the POINT\'s card (' + r.id + '), not the area\'s');
    else console.log('   note: no point over an area in this view (fixture) — click priority not exercised');
    await js(`document.querySelector('.maplibregl-popup-close-button')?.click(); true`);
  }

  console.log('── an aircraft\'s ROUTE in its card and its TRACK while the card is open ──');
  {
    // ★ BAW696 (ADS-B, heard by the XCover and the Pi 500): no route in either server's data → adsbdb (mocked) LHR → FRA, on it
    const ll = (k) => js(`(() => { const i = ITEM_BY_KEY.get('${k}'); return i && [i.lon, i.lat]; })()`);
    await js(`gl.jumpTo({ center: [-1.6, 51.7], zoom: 7.6 }); true`); await sleep(800);   // ★ past the clusters (zoom 7+)
    trailHits.length = 0; for (const k of Object.keys(tableHits)) delete tableHits[k];
    await js(`showCard('a:4CA000', ${JSON.stringify(await ll('a:4CA000'))}); true`);
    ok(await until(`window.__cmRoute && window.__cmRoute.key === 'a:4CA000'`, 10000), 'the card opens and its route is looked up');
    let rt = await js('window.__cmRoute');
    ok(rt.src === 'adsbdb' && /London \(LHR\) → Frankfurt am Main \(FRA\)/.test(rt.line) && !rt.struck, `BAW696: "${rt.line}" from the route source`);
    ok(/adsbdb\.com/.test(rt.words) && /David Taylor & Jim Mason/.test(rt.words) && /on it/.test(rt.words), `…credited, small, and judged against its position (${rt.words})`);
    ok(tableHits.xcover === 1 && tableHits.pi500 === 1, `★ the servers' own data asked FIRST — each heard-by receiver's table once (${JSON.stringify(tableHits)})`);
    ok(routeAsks.filter((x) => x === 'BAW696').length === 1, `one route request (${routeAsks.join(',')})`);
    ok(await until(`window.__cmTrail && window.__cmTrail.src === 'server'`, 10000), '★★ its TRACK: the ADS-B trail from the radio that decoded it');
    let tr = await js('window.__cmTrail');
    ok(tr.n >= 40 && tr.gaps === 1 && tr.solid === 2 && tr.colour === '#ffb000', `…drawn solid, a 60 NM hole DASHED, in ADS-B's colour (${JSON.stringify(tr)})`);
    ok(trailHits.length <= 4 && trailHits.some((h) => h.startsWith('pi500/r/' + PI500_RADIOS[2] + '/adsb/trail/4ca000')) && trailHits.every((h) => !/vibeserver\.vibesdr/.test(h)),
       `…asked radio by radio, at most 4, over each server's own tunnel (${trailHits.join(' | ')})`);
    ok(/Pi 500/.test(await js(`document.querySelector('.maplibregl-popup [data-trk]').textContent`)), `the card says whose trail it is: "${await js(`document.querySelector('.maplibregl-popup [data-trk]').textContent`)}"`);
    await sleep(1200);
    const drawn = await js(`({ line: gl.queryRenderedFeatures({ layers: ['cm-trail-line'] }).length, gap: gl.queryRenderedFeatures({ layers: ['cm-trail-gap'] }).length })`);
    ok(drawn.line > 0 && drawn.gap > 0, `on the GPU map: the solid line and the dash both rendered (${JSON.stringify(drawn)})`);
    const order = await js(`gl.getStyle().layers.map(l => l.id)`);
    ok(order.indexOf('cm-trail-line') < order.indexOf('cm-dp-mesh') && order.indexOf('cm-trail-gap') < order.indexOf('cm-pts'), '…under the markers');
    await shot('community-aircraft-route-trail.png');
    // ★ the click rule still holds with the trail drawn: a click on the aircraft opens the aircraft, not anything under the trail
    //   (zoomed in so BAW696's glyph is not under its two neighbours' — three aircraft 0.1° apart in the fixture)
    await js(`(() => { const i = ITEM_BY_KEY.get('a:4CA000'); gl.jumpTo({ center: [i.lon, i.lat], zoom: 10 }); return true; })()`); await sleep(900);
    const hit = await js(`(() => { const i = ITEM_BY_KEY.get('a:4CA000'); const p = gl.project([i.lon, i.lat]); gl.fire('click', { point: p, lngLat: gl.unproject(p), originalEvent: {} });
      return document.querySelector('.maplibregl-popup')?.textContent || ''; })()`);
    ok(/BAW696/.test(hit) && /Route/.test(hit), `a click on the aircraft (its trail drawn under it) opens ITS card (${hit.slice(0, 60)}…)`);
    await until(`window.__cmTrail && window.__cmTrail.src === 'server'`, 6000);
    ok(trailHits.length <= 4 && (await js('window.__cmTrail')).src === 'server', `…its trail back at once, not asked again within ${30} s (${trailHits.length} requests in all)`);
    await js(`gl.jumpTo({ center: [-1.6, 51.7], zoom: 7.6 }); true`);
    await js(`document.querySelector('.maplibregl-popup-close-button').click(); true`); await sleep(300);
    ok(await js(`window.__cmTrail === null && gl.getSource('cm-trail').serialize().data.features.length === 0`), '★ the card closed: the track CLEARED');

    // ★ BAW697: adsbdb says "unknown callsign" — said plainly; asked once however many times the card opens
    await js(`showCard('a:4CA001', ${JSON.stringify(await ll('a:4CA001'))}); true`);
    ok(await until(`window.__cmRoute && window.__cmRoute.key === 'a:4CA001'`, 8000) && /^unknown$/.test((await js('window.__cmRoute')).line) && /nothing for BAW697/.test((await js('window.__cmRoute')).words),
       `BAW697: "route unknown", plainly (${(await js('window.__cmRoute')).words})`);
    await js(`document.querySelector('.maplibregl-popup-close-button').click(); window.__cmRoute = null; showCard('a:4CA001', ${JSON.stringify(await ll('a:4CA001'))}); true`);
    await until(`window.__cmRoute && window.__cmRoute.key === 'a:4CA001'`, 8000);
    ok(routeAsks.filter((x) => x === 'BAW697').length === 1, `…opened again: from the session's cache, no second request (${routeAsks.join(',')})`);
    ok(await js(`(() => { try { return 'BAW697' in JSON.parse(sessionStorage.getItem(CommunityMap.ROUTE.cacheKey)); } catch { return false; } })()`), '…kept in sessionStorage for the visit');

    // ★ BAW698: a route that does not fit where the aircraft is (MAD → LIS, aircraft over Cambridge) — struck through, said why
    await js(`showCard('a:4CA002', ${JSON.stringify(await ll('a:4CA002'))}); true`);
    ok(await until(`window.__cmRoute && window.__cmRoute.key === 'a:4CA002'`, 8000), 'BAW698 looked up');
    rt = await js('window.__cmRoute');
    ok(rt.struck && /far from it/.test(rt.words), `★ a route the aircraft is nowhere near is struck through and SAID to be doubtful ("${rt.words.slice(0, 90)}…")`);

    // ★ UAL917 (HFDL — no ADS-B trail anywhere): the route from the XCover's OWN datalink data; the track from this page's fixes
    const now = Math.floor(Date.now() / 1000);
    await js(`TRACKS.set('a:A1B2C3', [[54.0, -36.0, ${now - 1700}], [54.2, -34.5, ${now - 1500}], [54.5, -30.2, ${now - 240}]]); true`);
    const n0 = routeAsks.length, th0 = trailHits.length;
    await js(`gl.jumpTo({ center: [-33, 54.3], zoom: 5 }); showCard('a:A1B2C3', ${JSON.stringify(await ll('a:A1B2C3'))}); true`);
    ok(await until(`window.__cmRoute && window.__cmRoute.key === 'a:A1B2C3'`, 8000), 'UAL917 looked up');
    rt = await js('window.__cmRoute');
    ok(rt.src === 'datalink' && /EGLL → KIAD/.test(rt.line) && /datalink messages, heard by XCover/.test(rt.words) && routeAsks.length === n0,
       `★ the SERVER's route wins and says where it came from ("${rt.line}" — ${rt.words}); the outside source not asked`);
    await sleep(400);
    tr = await js('window.__cmTrail');
    ok(tr && tr.src === 'client' && tr.n === 3 && tr.gaps === 1 && tr.solid === 1 && tr.colour === '#c084fc' && trailHits.length === th0,
       `no server trail for a datalink aircraft (none asked): the fixes since the map opened, the 21-min gap dashed, in HFDL's colour (${JSON.stringify(tr)})`);
    ok(/since you opened the map/.test(await js(`document.querySelector('.maplibregl-popup [data-trk]').textContent`)), '…and the card says that is all it is');
    await sleep(1200);
    await shot('community-aircraft-datalink-track.png');
    await js(`document.querySelector('.maplibregl-popup-close-button').click(); true`); await sleep(200);
    ok(await js(`window.__cmTrail === null`), '…cleared on close');
    ok(/dashed across a gap of over five minutes/.test(await keyText()), 'the KEY explains the track line and its dash');
  }

  console.log('── the LAYERS chips\' totals, STATS and the LIST\'s facts (the old status line) ──');
  await sleep(500);
  const PILLS = `Object.fromEntries(CM.MODES.map(m => [m.id, document.querySelector('.trkCard2 [data-tgl="' + m.id + '"] .n')?.textContent]).filter(([, v]) => v !== undefined))`;
  const pills = await js(PILLS);
  c = await cm();
  ok(Object.values(pills).every((v) => /^\d+$/.test(v)) && Number(pills.ais) === c.byKind.ship && Number(pills.dsc) === c.byKind.dsc && Number(pills.ndb) === c.byKind.ndb && Number(pills.sondes) === c.byKind.sonde
     && Number(pills.aprs) === c.byKind.aprs && Number(pills.mesh) === c.byKind.mesh && Number(pills.navtex) === (c.byKind.navst || 0) + (c.byKind.navwarn || 0) + (c.byKind.navmet || 0),
     `a total in every pill, the status line's numbers (${JSON.stringify(pills)})`);
  const stl = await js(`document.querySelector('.trkCard2[data-card="list"] .facts').textContent`);
  ok(/\b4 aircraft \(all sources combined\)/.test(stl) && new RegExp(`\\b${Number(pills.ais) + Number(pills.dsc)} ships \\(AIS \\+ DSC combined\\)`).test(stl) && !/\d DSC\b/.test(stl),
     `the LIST's facts give COMBINED totals, the chips their breakdown (${stl.slice(0, 120)}…)`);
  ok(await js(`(() => { const s = MC.view.root.querySelector('.trkCard2[data-card="stats"] .stBody').textContent; return /RECEIVERS/.test(s) && /3\\/3/.test(s) && /AIRCRAFT \\(ALL SOURCES COMBINED\\)/.test(s); })()`), 'STATS: NOW — the receivers sharing and the combined totals');
  ok(await js(`[...document.querySelectorAll('.trkCard2[data-card="stats"] .stTab')].map(b => b.textContent).join() .includes('ADS-B')`), '…and a tab per mode on the map (NEWEST, FURTHEST from the receiver that heard it)');
  // ★★ 11-28 STATS FURTHEST: aviation in NAUTICAL MILES, each mode from its own fixes (an aircraft's ADS-B furthest is an ADS-B fix)
  const st = await js(`(() => { const o = MC.stats; let got = null; MC.stats = (s, f) => { got = s; return o(s, f); };
    try { drawStats(Math.floor(Date.now() / 1000)); } finally { MC.stats = o; }
    return got.filter((x) => x.mode !== 'NOW').map((x) => { const r = x.rows.find((y) => y.label === 'FURTHEST'); const it = r && ITEM_BY_KEY.get(r.key);
      const id = CM.MODES.find((m) => m.label === x.mode)?.id;
      return { mode: x.mode, id, v: r ? r.value : '', own: !r || it.kind !== 'aircraft' || !!(it.fix && it.fix[id] && it.fix[id].some((f) => f.lat === r.lat && f.lon === r.lon)) }; }); })()`);
  const avi = st.filter((x) => CM_AVI.has(x.id) && x.v);
  ok(avi.length >= 1 && avi.every((x) => /^\d[\d,]* NM$/.test(x.v)) && st.filter((x) => !CM_AVI.has(x.id) && x.v).every((x) => / km$/.test(x.v)),
     `★★ STATS FURTHEST: aviation in NM, the rest in km (${st.map((x) => x.mode + ' ' + x.v).join(' · ')})`);
  ok(st.every((x) => x.own), '★★ …each aircraft mode’s FURTHEST is at one of ITS OWN fixes of that aircraft');
  ok(['adsb', 'acars', 'vdl2', 'hfdl'].reduce((a, m) => a + Number(pills[m] || 0), 0) === c.byKind.aircraft, `…the aviation pills add up to the aircraft on the map (${c.byKind.aircraft}) — each counted once`);
  const navp = await js(`Object.fromEntries([...document.querySelectorAll('.trkCard2 [data-navl]')].map(b => [b.dataset.navl, Number(b.querySelector('.n')?.textContent)]))`);
  ok(navp.stations === c.byKind.navst && navp.warnings === c.byKind.navwarn && navp.weather === c.byKind.navmet && navp.messages === c.byKind.navmsg, `…and in NAVTEX's own (${JSON.stringify(navp)})`);
  await js(`document.querySelector('[data-rng="900"]').click(); true`); await sleep(400);
  const p15 = await js(PILLS);
  ok(Number(p15.ndb) <= Number(pills.ndb) && Number(p15.navtex) < Number(pills.navtex) && Object.values(p15).some((v) => v === '0'), `…live with HEARD IN (15 min: ${JSON.stringify(p15)}); a 0 is shown, the chip stays`);
  await js(`document.querySelector('[data-rng="86400"]').click(); true`); await tgl('ais'); await sleep(300);
  ok(await js(`document.querySelector('.trkCard2 [data-tgl="ais"] .n').textContent`) === pills.ais, '…a chip switched off still says what it would show');
  await tgl('ais');

  console.log('── the KEY ──');
  ok(await js(`(() => { const k = document.querySelector('.trkCard2[data-card="map"] details.keySect'); return !!k && !k.hidden && !k.open; })()`), 'the KEY: a fold in MAP CONTROLS, shut');
  await card('map'); await js(`document.querySelector('.trkCard2[data-card="map"] details.keySect').open = true; true`); await sleep(300);
  const secs = await js(`[...document.querySelectorAll('.trkKey .ks')].map(e => e.textContent)`);
  ok(['RECEIVERS', 'AIRCRAFT', 'NDB', 'SHIPS (AIS)', 'DSC POSITIONS', 'NAVTEX STATIONS', 'NAVTEX WARNINGS', 'NAVTEX WEATHER', 'SEA AREAS', 'RADIOSONDES', 'APRS', 'MESHCORE'].every((x) => secs.includes(x)) && !secs.includes('VOR / ILS'),
     `a section for each layer on, none for one nobody shares (${secs.join(' · ')})`);
  ok(await js(`document.querySelectorAll('.trkKey .kr svg').length`) > 40 && /Gale force 8/.test(await keyText()) && /Ship still/.test(await keyText()),
     '…drawn with the server maps’ own rows and glyphs (ship, DSC, hazards, wind arrows, sea areas)');
  await shot('community-key-desktop.png');
  await tgl('g:aviation'); await sleep(300);
  const secs2 = await js(`[...document.querySelectorAll('.trkKey .ks')].map(e => e.textContent)`);
  ok(!secs2.includes('AIRCRAFT') && !secs2.includes('NDB') && secs2.includes('SHIPS (AIS)'), 'AVIATION off: its sections leave the key');
  await tgl('g:aviation');
  await js(`document.querySelector('.trkCard2[data-card="map"] details.keySect').open = false; true`); await card('map', false);

  console.log('── the groups are independent; a group off keeps its chips ──');
  await tgl('g:maritime');
  c = await cm();
  ok(!c.byKind.ship && !c.byKind.navwarn && !c.byKind.dsc && c.byKind.aircraft && c.byKind.aprs, `MARITIME off: aviation and misc stay (${JSON.stringify(c.byKind)})`);
  await tgl('adsb'); await tgl('g:aviation'); await tgl('g:aviation');
  c = await cm();
  ok(!c.on.includes('adsb') && c.on.includes('hfdl'), 'AVIATION off and on again: ADS-B stays off as it was left');
  await tgl('adsb'); await tgl('g:maritime'); await tgl('g:aviation'); await tgl('g:misc');
  c = await cm();
  ok(JSON.stringify(c.groups) === '["maritime"]' && Object.keys(c.byKind).every((k) => ['ship', 'dsc', 'navst', 'navwarn', 'navmet', 'navmsg'].includes(k)), `MARITIME alone: ${Object.keys(c.byKind)}`);
  ok(await js(`localStorage.getItem('vibesdr.communityMap.v2')`) && JSON.parse(await js(`localStorage.getItem('vibesdr.communityMap.v2')`)).groups.join() === 'maritime', 'remembered per viewer (localStorage)');

  console.log('── NAVTEX: its own filters, a warning with its area, the text on opening, the MESSAGES list ──');
  ok(await js(`!document.querySelector('.ctlSect').hidden && document.querySelectorAll('.slot-ctl [data-navt]').length >= 3 && !!document.getElementById('navSt') && document.querySelectorAll('.slot-ctl [data-navl]').length === 4`), 'NAVTEX in MAP CONTROLS: its layers, message types, station');
  const area = await js(`(() => { const w = [...ITEM_BY_KEY.values()].find(i => i.kind === 'navwarn' && i.d.shape === 'area'); return w && { key: w.key, ll: [w.lon, w.lat], pts: w.d.pts }; })()`);
  ok(!!area, `a NAVTEX warning with an AREA is on the map (${area && area.key})`);
  ok(await js(`window.__cmShapes.some(f => f.type === 'Polygon' && f.id === ${JSON.stringify(area && area.key)})`) && await js(`gl.queryRenderedFeatures({ layers: ['cm-area'] }).length`) >= 0, '…its area drawn as a polygon on the GPU layer');
  if (area) await hold(), await js(`gl.fitBounds([[${Math.min(...area.pts.map((p) => p[1]))}, ${Math.min(...area.pts.map((p) => p[0]))}], [${Math.max(...area.pts.map((p) => p[1]))}, ${Math.max(...area.pts.map((p) => p[0]))}]], { padding: 160, duration: 0, maxZoom: 9 }); showCard(${JSON.stringify(area && area.key)}, ${JSON.stringify(area && area.ll)}); true`);
  ok(await until(`/THE MESSAGE TEXT FROM/.test((document.querySelector('.maplibregl-popup .nvTx') || {}).textContent || '')`, 8000), 'its card (the NAVTEX map’s own) fetches the message text from the receiver when opened');
  await sleep(2500);
  await shot('community-navtex-warning-area.png');
  await js(`document.querySelector('.maplibregl-popup-close-button')?.click(); true`);
  const types = await js(`[...document.querySelectorAll('.slot-ctl [data-navt]')].map(b => b.dataset.navt)`);
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
  await card('list');
  ok(await until(`(() => { const t = document.querySelector('.trkCard2[data-card="list"] .hd .tab[aria-selected="true"]'); return (!t || t.dataset.tab === 'msgs') && document.querySelectorAll('.trkCard2[data-card="list"] .rows .trkRow[data-key^="nx:"], .trkCard2[data-card="list"] .rows .trkRow').length > 20; })()`), 'the LIST is the NAVTEX MESSAGES (its first tab)');
  await js(`document.querySelector('.trkCard2[data-card="list"] .rows .trkRow').click(); true`);
  ok(await until(`/THE MESSAGE TEXT FROM LENOVO/.test(document.querySelector('.trkCard2[data-card="list"] .rows .nvTx')?.textContent || '')`, 8000), '…a message opened shows its text in place, from the receiver with the CLEANEST copy');
  await shot('community-messages-desktop.png');
  await tgl('g:aviation'); await tgl('g:misc');
  ok(await until(`[...document.querySelectorAll('.trkCard2[data-card="list"] .hd .tab')].map(t => t.dataset.tab).join() === 'msgs,air,sea,navtex,other'`, 4000), '…and a tab for each kind on the map: MESSAGES, AIRCRAFT, SHIPS, NAVTEX, OTHER');
  await js(`document.querySelector('.trkCard2[data-card="list"] .hd .tab[data-tab="air"]').click(); true`);
  ok(await until(`document.querySelectorAll('.trkCard2[data-card="list"] .rows .trkRow[data-key^="a:"]').length === window.__cm.byKind.aircraft`, 4000), '…AIRCRAFT: a row each');
  await js(`window.__cmRoute = null; document.querySelector('.trkCard2[data-card="list"] .rows .trkRow[data-key="a:4CA000"]').click(); true`);
  ok(await until(`/BAW696/.test(document.querySelector('.maplibregl-popup')?.textContent || '')`, 6000), '…a row pressed: the map goes to it and opens its card');
  await js(`document.querySelector('.maplibregl-popup-close-button')?.click(); document.querySelector('.trkCard2[data-card="list"] .hd .tab[data-tab="msgs"]').click(); true`);
  await card('list', false);

  console.log('── RECEIVERS: the list, LISTEN, picking one, ONLY THIS ──');
  ok(await js(`document.querySelector('[data-cbtn="servers"] .cap')?.textContent`) === 'RECEIVERS · 3', 'the SERVERS chip is RECEIVERS here');
  await js(`document.querySelector('[data-cbtn="servers"]').click(); true`);
  ok(await until(`document.querySelectorAll('.trkCard2[data-card="servers"] .rxRow[data-id]').length === 3`), 'the RECEIVERS card opens with three receivers');
  const rows = await js(`[...document.querySelectorAll('.trkCard2[data-card="servers"] .rxRow[data-id]')].map(r => ({ id: r.dataset.id, text: r.textContent, href: r.querySelector('a.listen')?.href }))`);
  const lv = rows.find((r) => r.id === 'lenovo'), pi = rows.find((r) => r.id === 'pi500');
  ok(lv && lv.href === 'https://lenovo.vibeserver.vibesdr.net/', `LISTEN is the directory's link (${lv && lv.href})`);
  ok(lv && /NAVTEX/.test(lv.text) && /SONDES/.test(lv.text) && /MESHCORE/.test(lv.text) && /on the map now/.test(lv.text) && /GB · IO92/.test(lv.text), 'a row: where, what it shares, how many items now');
  ok(pi && /no free slot/.test(pi.text) && /4\/4 listening/.test(pi.text), 'busy as the directory says it');
  await shot('community-receivers-desktop.png');
  await js(`document.querySelector('.trkCard2[data-card="servers"] [data-only=xcover]').click(); true`);
  ok(await until(`window.__cm.server === 'xcover'`), 'ONLY THIS: the XCover');
  c = await cm();
  ok(Object.keys(c.byKind).every((k) => ['aircraft', 'ship', 'aprs'].includes(k)), `…what it heard only (${JSON.stringify(c.byKind)})`);
  ok(await js(`!!document.getElementById('onlyChip')`), '…said in MAP CONTROLS (ONLY XCover ✕)');
  await js(`document.getElementById('onlyChip').click(); true`); await card('servers', false);
  ok(await until(`window.__cm.server === ''`), '…and undone there');

  console.log('── only what is switched on is fetched; a chip switched on fetches itself ──');
  await sleep(15000);
  ok(Object.keys(hits).slice(0, 3).every((k) => hits[k] === want(k)), `25 s on, still once each (per radio) (${JSON.stringify(Object.fromEntries(Object.entries(hits).slice(0, 3)))})`);
  await js(`localStorage.setItem('vibesdr.communityMap.v2', JSON.stringify({ off: ['ais', 'sondes'] })); true`);
  resetHits();
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === 3`, 20000), 'reloaded with AIS and SONDES switched off');
  ok(!asked.xcover.join().includes('ais') && !asked.lenovo.join().includes('sondes'), `…they are not asked for (${asked.xcover} / ${asked.lenovo})`);
  await tgl('ais');
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
  await card('map'); await js(`document.querySelector('.trkCard2[data-card="map"] details.keySect').open = true; true`); await sleep(900);
  ok(await js(`(() => { const r = document.querySelector('.trkCards .trkCard2[data-card="map"]').getBoundingClientRect(); return r.width > 200 && r.left >= -4 && r.right <= 394 && document.documentElement.scrollWidth <= 390; })()`), 'MAP CONTROLS with the KEY fits a phone (no sideways scroll)');
  await shot('community-key-390.png');
  await card('map', false);
  await card('servers'); await sleep(900);
  const sheet = await js(`(() => { const r = document.querySelector('.trkCards .trkCard2[data-card="servers"]').getBoundingClientRect(); return { l: r.left, r: r.right, w: r.width }; })()`);
  ok(sheet.w > 300 && sheet.l >= -4 && sheet.r <= 394, `RECEIVERS opens as a card on the phone (${JSON.stringify(sheet)})`);
  await sleep(400);
  await shot('community-receivers-390.png');
  await card('servers', false);
  {
    await mockRoutes();
    const ll = await js(`(() => { const i = ITEM_BY_KEY.get('a:4CA000'); return i && [i.lon, i.lat]; })()`);
    await hold(); await js(`window.__cmRoute = null; gl.jumpTo({ center: [-1.3, 51.4], zoom: 5.6 }); showCard('a:4CA000', ${JSON.stringify(ll)}); true`);
    ok(await until(`window.__cmRoute && window.__cmRoute.key === 'a:4CA000' && window.__cmTrail && window.__cmTrail.src === 'server'`, 10000), 'a phone: the aircraft card with its route and track');
    await sleep(1500);
    ok(await js(`(() => { const r = document.querySelector('.maplibregl-popup').getBoundingClientRect(); return r.left >= 0 && r.right <= 390 && document.documentElement.scrollWidth <= 390; })()`),
       `…the card fits a phone (${JSON.stringify(await js(`(() => { const r = document.querySelector('.maplibregl-popup').getBoundingClientRect(); return [Math.round(r.left), Math.round(r.right)]; })()`))})`);
    await shot('community-aircraft-route-390.png');
    await js(`document.querySelector('.maplibregl-popup-close-button').click(); true`);
  }
  await call('Emulation.clearDeviceMetricsOverride');

  console.log('── ★★★ b97 the card system at every size: nothing off screen, nothing overlapping, nothing hidden ──');
  for (const [name, W, H, ins, mobile] of MAP_SIZES) {
    await call('Emulation.setDeviceMetricsOverride', { width: W, height: H, deviceScaleFactor: mobile ? 2 : 1, mobile });
    await call('Page.navigate', { url: PAGE });
    await until(`window.__cm && window.__cm.up === 3`, 20000);
    await js(setInsetsExpr(ins)); await sleep(1500);
    for (const listOpen of [true, false]) {
      await card('list', listOpen); await sleep(700);
      const L = JSON.parse(await js(chromeLayoutExpr(ins)));
      const want = ['x', 'help', 'chip:list', 'chip:layers', 'chip:servers', 'chip:map', 'chip:stats'];
      const have = L.pieces.map((p) => p.k);
      ok(want.every((k) => have.includes(k)) && !L.hidden.length, `${name}, LIST ${listOpen ? 'open' : 'shut'}: ✕, HELP, LIST, LAYERS, RECEIVERS, MAP, STATS all on screen (${have.join(' ')}${L.hidden.length ? ' · hidden: ' + L.hidden.join(' ') : ''})`);
      ok(!L.outside.length, `${name}, LIST ${listOpen ? 'open' : 'shut'}: everything inside the window${L.short ? ' and the safe rectangle' : ''} (${L.outside.join(' ') || 'all ' + L.pieces.length})`);
      ok(!L.overlaps.length && !L.covered.length, `${name}, LIST ${listOpen ? 'open' : 'shut'}: no two pieces overlap (${JSON.stringify(L.overlaps)} ${L.covered.join(' ')})`);
      ok(!/RATE|CHAT|TUNE/.test(have.join()) && !have.includes('chip:rate') && !have.includes('chip:chat') && !have.includes('chip:tune'), `${name}: a viewer — no TUNE, CHAT or RATE`);
      if (listOpen) await shot(`community-layout-${W}x${H}.png`);
    }
    if (/956|667|915×412/.test(name)) ok(JSON.parse(await js(chromeLayoutExpr(ins))).short, `${name}: the short-landscape layout (the top row)`);
    await js(setInsetsExpr({ l: null, r: null, b: null }));
  }
  await call('Emulation.clearDeviceMetricsOverride');

  console.log(`── ${N} receivers: layered loading, clustering, the numbers ──`);
  listN = N; resetHits();
  await call('Page.navigate', { url: PAGE });
  ok(await until(`window.__cm && window.__cm.up === ${N}`, 30000), `all ${N} read`);
  c = await cm();
  await sleep(1500);
  const heap = await js(`performance.memory ? Math.round(performance.memory.usedJSHeapSize / 1048576) : -1`);
  ok(c.perf.listDraw > 0 && c.perf.full > 0 && c.perf.firstDraw > 0, `receivers listed ${c.perf.listDraw} ms · first receiver's data ${c.perf.firstData} ms · all ${N} in ${c.perf.full} ms · GPU map's first draw ${c.perf.firstDraw} ms (software GL, headless) · JS heap ${heap} MB`);
  await hold(); await js(`gl.jumpTo({ center: [8, 52], zoom: 2.2 }); true`); await sleep(1500);
  const clusters = await js(`gl.queryRenderedFeatures({ layers: clusterLayers() }).length`);
  ok(clusters > 0, `zoomed out, the dense layers cluster (${clusters} clusters drawn)`);
  await shot('community-10-receivers-clustered.png');
  await js(`gl.jumpTo({ center: [-1.2, 51.9], zoom: 8 }); true`); await sleep(1200);
  ok(await js(`gl.queryRenderedFeatures({ layers: densePtLayers() }).length`) > 0, 'zoomed in, full markers');
  // ★ Stuart 11-27: "it drops to the grouped numbers quicker than i expected" — a county-to-region view (zoom 6, SE England
  //   in his recording) shows every aircraft; grouping is for the country/continent view only (clusterMaxZoom 5, radius 22).
  await js(`gl.jumpTo({ center: [0.5, 51.8], zoom: 6.0 }); true`); await sleep(1200);
  ok(await js(`gl.queryRenderedFeatures({ layers: clusterLayers() }).length`) === 0, 'zoom 6 (a region, e.g. SE England): no grouped numbers, every aircraft drawn');
  ok(Object.entries(hits).every(([k, n]) => n === want(k)), `each of ${N} receivers asked once (per radio) (${Object.values(hits).join(',')})`);
  console.log(`   (largest answer per receiver, bytes: ${JSON.stringify(sizes)})`);
  console.log('── ★★★ b97 zoomed out stays rich (Stuart 11-27: "the map starts looking barren as you zoom out") — SE England, ~150 mesh + ~100 aircraft ──');
  {
    await call('Page.navigate', { url: PAGE });
    await until(`window.__cm && window.__cm.up >= 3`, 30000);
    // ★ a synthetic receiver's answer, through the page's own reader (CM.itemsOf) — no network, the data rules untouched
    await js(`(() => { const now = Math.floor(Date.now() / 1000), a = [], m = [];
      let r = 7; const rnd = () => (r = (r * 16807) % 2147483647) / 2147483647;
      for (let i = 0; i < 100; i++) a.push(['5' + (100000 + i).toString(16).toUpperCase(), 50.6 + rnd() * 2.2, -2.2 + rnd() * 4.2, 5, 30000, Math.round(rnd() * 360), 420, 1, 'TST' + i, '', 'A320']);
      for (let i = 0; i < 150; i++) { const near = i < 110; m.push(['ab' + i.toString(16).padStart(4, '0'), 'Node ' + i, i % 4 === 3 ? 'companion' : 'repeater',
        near ? 51.25 + rnd() * 0.75 : 51.6 + rnd() * 1.4, near ? -0.75 + rnd() * 1.3 : -2.0 + rnd() * 1.6, now - 60, 3, 0]); }
      const items = CM.itemsOf({ on: true, now, a, m }, { id: 'syn', name: 'Synthetic', rid: '' });
      DATA.set('syn', { radios: new Map([['', { sections: { a, m }, items }]]), items }); render(); return items.length; })()`);
    await hold();
    const rows = [];
    for (const z of [8, 7, 6, 5]) {
      await js(`gl.jumpTo({ center: [-0.3, 51.55], zoom: ${z} }); true`); await sleep(2200);
      const r = await js(`(() => { const ind = gl.queryRenderedFeatures({ layers: densePtLayers() }), cl = gl.queryRenderedFeatures({ layers: clusterLayers() });
        const kinds = (fs) => fs.reduce((o, f) => (o[f.layer.id.replace(/^cm-(dpa?|cl)-/, '')] = (o[f.layer.id.replace(/^cm-(dpa?|cl)-/, '')] || 0) + 1, o), {});
        const glyphs = cl.map((f) => { const L = gl.getLayer(f.layer.id); return { k: f.layer.id, n: f.properties.point_count, img: gl.getLayoutProperty(f.layer.id, 'icon-image') ? 1 : 0, src: f.source }; });
        const labels = gl.queryRenderedFeatures({ layers: densePtLayers() }).filter((f) => f.properties.lb).length;
        return { z: ${z}, ind: ind.length, cl: cl.length, inK: kinds(ind), clK: kinds(cl), glyphs, labelOp: gl.getPaintProperty('cm-dp-mesh', 'text-opacity'), mixed: cl.filter((f) => !/^cm-d-/.test(f.source)).length }; })()`);
      rows.push(r);
      await shot(`community-zoom-${z}.png`);
      console.log(`   (zoom ${z}: ${r.ind} individual icons ${JSON.stringify(r.inK)} · ${r.cl} clusters ${JSON.stringify(r.clK)})`);
    }
    const at = (z) => rows.find((r) => r.z === z);
    // ★ what is DRAWN (an icon, or a cluster wearing its kind's glyph): each step out keeps at least 40 % — it thins, it never collapses
    const marks = (z) => at(z).ind + at(z).cl;
    for (const z of [7, 6, 5]) ok(marks(z) >= Math.round(marks(z + 1) * 0.4), `zoom ${z + 1} → ${z}: no collapse — ${marks(z)} drawn (${at(z).ind} icons + ${at(z).cl} glyph clusters) from ${marks(z + 1)} (≥ 40 %)`);
    ok(at(7).ind >= 40 && at(6).ind >= 40, `zooms 7 and 6 (no clustering yet): ${at(7).ind} and ${at(6).ind} individual icons — collision thins, nothing collapses`);
    ok([8, 7, 6, 5].every((z) => (at(z).inK.mesh || 0) + (at(z).clK.mesh || 0) >= 8 && (at(z).inK.aircraft || 0) + (at(z).clK.aircraft || 0) >= 8), 'mesh AND aircraft stay drawn at every zoom (icons or their own clusters)');
    ok(at(5).ind + at(5).cl >= 40 && at(5).ind + at(5).cl >= Math.round(at(6).ind * 0.6), `zoom 5 (SE England and beyond): ${at(5).ind} icons + ${at(5).cl} kind-glyph clusters — not a few blank discs`);
    ok([8, 7, 6].every((z) => at(z).cl === 0) && at(5).cl > 0, 'clusters only at zoom 5 and below (clusterMaxZoom 5)');
    ok(rows.every((r) => r.glyphs.every((g) => /^cm-cl-(mesh|aircraft|ship|aprs)$/.test(g.k) && g.img && g.src === 'cm-d-' + g.k.slice(6))), 'every cluster is drawn as its kind\'s GLYPH from its own layer\'s source — mesh with mesh, aircraft with aircraft');
    ok(await js(`clusterLayers().every((l) => gl.getLayer(l.replace('cm-cl-', 'cm-cln-')) && gl.getLayer(l.replace('cm-cl-', 'cm-clb-')))`), '…each with its count badge');
    ok(await js(`(() => { const e = gl.getLayoutProperty('cm-cl-mesh', 'icon-image'); return Array.isArray(e) && e[0] === 'case'; })()`), '…the commonest sub-kind draws it (the repeater square for a repeater cluster)');
    ok(await js(`DENSE_KINDS.every((k) => { const c = gl.getSource('cm-d-' + k).serialize(); return c.cluster === true && c.clusterMaxZoom === 5 && c.clusterRadius === 22; })`), 'per layer, the earlier rule: clusterMaxZoom 5, radius 22');
    ok(await js(`gl.getLayoutProperty('cm-dp-mesh', 'icon-allow-overlap') === false && !!gl.getLayoutProperty('cm-dp-mesh', 'symbol-sort-key') && gl.getLayer('cm-dpa-mesh').minzoom === ALL_ZOOM`), 'collision thins the icons (sorted: repeaters, then the newest) below zoom 9; every icon from 9');
    await js(`gl.jumpTo({ center: [-0.3, 51.55], zoom: 8.5 }); true`); await sleep(1500);
    const lab85 = await js(`gl.queryRenderedFeatures({ layers: densePtLayers() }).filter((f) => f.properties.lb).length > 0`);
    ok(JSON.stringify(rows[0].labelOp) === JSON.stringify(['interpolate', ['linear'], ['zoom'], 7, 0, 8, 1]) && lab85 && await js(`JSON.stringify(gl.getLayoutProperty('cm-dp-mesh', 'text-field')).includes('"step"')`) && await js(`gl.getLayoutProperty('cm-dp-mesh', 'text-optional') === true`),
       'labels fade out first (7 → 8) and are optional — a label never hides an icon');
    await js(`DATA.delete('syn'); render(); true`);
  }
  console.log('── ★★ 11-28 the KEYBOARD / PAD drive the map (gamepad.ts, as the radio maps) and HELP is this map\'s own ──');
  {
    await call('Emulation.clearDeviceMetricsOverride');
    await call('Emulation.setDeviceMetricsOverride', { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
    await call('Page.navigate', { url: PAGE });
    await until(`window.__cm && window.__cm.up === 3`, 20000); await sleep(1500);
    await card('list', false); await sleep(500);
    const KEYS = { ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, Enter: 13, Escape: 27, '+': 187, '-': 189 };
    const key = async (k) => {
      const base = { key: k, code: k.startsWith('Arrow') ? k : k === '+' ? 'Equal' : k === '-' ? 'Minus' : k, windowsVirtualKeyCode: KEYS[k], nativeVirtualKeyCode: KEYS[k] };
      await call('Input.dispatchKeyEvent', Object.assign({ type: 'rawKeyDown' }, base));
      if (k.length === 1) await call('Input.dispatchKeyEvent', Object.assign({ type: 'char', text: k }, base));
      await call('Input.dispatchKeyEvent', Object.assign({ type: 'keyUp' }, base));
      await sleep(140);
    };
    const hl = `(() => { const e = document.querySelector('.gpFocus'); if (!e) return ''; return e.dataset.cbtn ? 'chip:' + e.dataset.cbtn : e.dataset.tgl ? 'tgl:' + e.dataset.tgl : e.matches('.trkCard2[data-card="list"]') ? 'list-box' : e.closest('.trkRow') ? 'row' : e.className.split(' ')[0] || e.tagName; })()`;
    const centre0 = await js(`gl.getCenter().toArray().join()`), zoom0 = await js(`gl.getZoom()`);
    // the walk: every chip is a stop, and the arrows never pan the map
    const seen = new Set();
    for (const k of ['ArrowRight', 'ArrowDown', 'ArrowDown', 'ArrowDown', 'ArrowDown', 'ArrowDown', 'ArrowDown', 'ArrowUp', 'ArrowRight', 'ArrowRight', 'ArrowRight', 'ArrowRight', 'ArrowLeft', 'ArrowLeft', 'ArrowLeft', 'ArrowLeft', 'ArrowLeft', 'ArrowDown', 'ArrowUp', 'ArrowUp', 'ArrowUp']) { await key(k); seen.add(await js(hl)); }
    const chips = await js(`[...document.querySelectorAll('[data-cbtn]')].filter((b) => b.getClientRects().length && !b.closest('[hidden]')).map((b) => 'chip:' + b.dataset.cbtn)`);
    ok(chips.length >= 5 && chips.every((c) => seen.has(c)), `★★ arrows walk the highlight to every chip (${chips.filter((c) => !seen.has(c)).join(' ') || 'all ' + chips.length}; walked ${[...seen].join(' ')})`);
    ok(await js(`gl.getCenter().toArray().join()`) === centre0 && await js(`gl.getZoom()`) === zoom0 && await js(`!gl.keyboard.isEnabled()`), '★★ …and never pan or zoom the map (MapLibre’s own keyboard handler is off)');
    // LAYERS: onto its chip, Enter opens it, Enter on a row toggles it, Esc shuts it with the highlight back on the chip
    const goto = async (want, max = 24) => { for (let i = 0; i < max && (await js(hl)) !== want; i++) { const order = ['ArrowDown', 'ArrowRight', 'ArrowUp', 'ArrowLeft']; await key(order[Math.floor(i / 6) % 4]); } return (await js(hl)) === want; };
    ok(await goto('chip:layers'), 'the highlight reaches the LAYERS chip');
    await key('Enter'); await sleep(400);
    ok(await js(`MC.view.isCardOpen('layers')`), '★★ Enter on LAYERS opens its card');
    await key('ArrowDown');
    let h = await js(hl);
    for (let i = 0; i < 6 && !/^tgl:(adsb|ais|mesh|ndb|aprs)$/.test(h); i++) { await key('ArrowDown'); h = await js(hl); }
    const was = await js(`document.querySelector('.gpFocus')?.getAttribute('aria-pressed')`);
    await key('Enter');
    const now = await js(`document.querySelector('.trkCard2 [data-tgl="${h.slice(4)}"]')?.getAttribute('aria-pressed')`);
    ok(/^tgl:/.test(h) && was !== now, `★★ …the arrows move into its rows and Enter toggles one (${h}: ${was} → ${now})`);
    await key('Enter'); await sleep(200);
    await key('Escape'); await sleep(400);
    ok(!(await js(`MC.view.isCardOpen('layers')`)) && (await js(hl)) === 'chip:layers', `★★ Esc shuts LAYERS, the highlight back on its chip (${await js(hl)})`);
    // − + zoom the map (the help's keys)
    const z1 = await js(`gl.getZoom()`); await key('+'); await sleep(700);
    const z2 = await js(`gl.getZoom()`); await key('-'); await sleep(700);
    ok(z2 > z1 && (await js(`gl.getZoom()`)) < z2, `− + zoom the map (${z1.toFixed(2)} → ${z2.toFixed(2)} → ${(await js('gl.getZoom()')).toFixed(2)})`);
    // the LIST: Enter on its chip opens it, Enter goes into the rows, Enter on a row opens that item's card, Esc shuts the card back onto the row
    ok(await goto('chip:list'), 'the highlight reaches the LIST chip');
    await key('Enter'); await sleep(600);
    // ★ the AIRCRAFT tab (as test-web-map-pad picks its tab): rows with a place on the map (MESSAGES' rows open in place)
    await js(`(() => { const t = document.querySelector('.trkList .hd .tab[data-tab="air"]'); if (t && t.getAttribute('aria-selected') !== 'true') t.click(); return 1; })()`); await sleep(700);
    let at = await js(hl);
    // ★ the radio maps' test-web-map-pad walk: a seeded random walk until the LIST stop is lit (it is ONE stop: no row on the way)
    let seed = 7, rows = 0, n = 0;
    const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
    for (; n < 160 && at !== 'list-box'; n++) { await key(['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'][Math.floor(rnd() * 4)]); at = await js(hl); if (at === 'row') rows++; }
    ok(at === 'list-box' && !rows, `the walk reaches the LIST as ONE stop, never a row on the way (${n} presses, ${rows} rows)`);
    if (at === 'list-box') { await key('Enter'); at = await js(hl); }
    ok(at === 'row', `★★ …into the LIST's rows (${at})`);
    const rowKey = await js(`document.querySelector('.gpFocus')?.closest('[data-key]')?.dataset.key || ''`);
    await key('Enter'); await sleep(1600);
    ok(await js(`!!(popup && popup.isOpen())`), `★★ Enter on a row opens its item's card (${rowKey})`);
    ok(await js(`!!document.querySelector('.maplibregl-popup .gpFocus')`), '★★ …and the highlight moves INTO the card (its first control), as on the radio maps');
    await key('Escape'); await sleep(500);
    ok(await js(`!(popup && popup.isOpen())`) && (await js(hl)) === 'row' && await js(`MC.view.isCardOpen('list')`), `★★ Esc shuts the item's card first — the highlight back on its row, the LIST still open (${await js(hl)})`);
    await key('Escape');
    const left = await js(hl);
    // ★ the radio maps' rule (test-web-map-pad): the item chosen from the LIST goes first — BACK clears it, then shuts the card
    let esc = 0; for (; esc < 3 && await js(`MC.view.isCardOpen('list')`); esc++) await key('Escape');
    ok(left === 'list-box' && !(await js(`MC.view.isCardOpen('list')`)) && esc <= 2, `…Esc again leaves the rows (${left}), then clears the chosen item and shuts the LIST (${esc} more)`);
    // HELP: this map's own — a line per chip PRESENT (by its caption), none of the radio map's
    await card('help'); await sleep(500);
    const hp = await js(`(() => { const b = document.querySelector('.trkCard2[data-card="help"]'); const rows = [...b.querySelectorAll('.helpBody .hRowH')].map((r) => r.dataset.help);
      const caps = Object.fromEntries([...document.querySelectorAll('[data-cbtn]')].map((x) => [x.dataset.cbtn, (x.querySelector('.cap')?.textContent || '').trim()]));
      const named = [...b.querySelectorAll('.helpBody .hRowH b')].map((x) => x.textContent.trim());
      const shown = [...document.querySelectorAll('.mDock[data-dock]')].filter((d) => !d.hidden && d.getClientRects().length).map((d) => d.dataset.dock).filter((d) => d !== 'scale');
      return { rows, named, shown, caps, text: b.textContent }; })()`);
    const chipRows = hp.rows.filter((r) => r !== 'x');
    ok(hp.rows[0] === 'x' && /directory/.test(hp.text), '★★ HELP: the ✕ = back to the directory');
    ok(chipRows.every((r) => hp.shown.includes(r)) && hp.shown.every((d) => chipRows.includes(d)), `★★ HELP names every chip the map HAS and no other (help ${chipRows.join(' ')} · chips ${hp.shown.join(' ')})`);
    ok(chipRows.every((r, i) => !hp.caps[r] || hp.named[i + 1] === hp.caps[r]), `…each under its own caption (${hp.named.join(', ')})`);
    const radioOnly = ['SERVERS', 'leave this server', 'Leave this server', 'TUNE', 'CHAT', 'SERVER HEALTH', 'TIME (', 'WATERFALL', 'waterfall', 'tune down', '< > tune', 'shares the radio', 'LIVE (in LAYERS)'];
    ok(radioOnly.every((w) => !hp.text.includes(w)), `★★ none of the radio map's words (${radioOnly.filter((w) => hp.text.includes(w)).join(' | ') || 'none'})`);
    ok(/grid square/.test(hp.text) && /HEARD IN/.test(hp.text) && /keyboard/.test(hp.text) && /Esc back/.test(hp.text), '…and what this map shows: the grid-square privacy, HEARD IN, the controller / keyboard lines');
    await shot('community-help.png');
    await card('help', false);
  }
  ok(errors.length === 0, `no script errors (${errors.slice(0, 3).join(' | ')})`);
} catch (e) { fail++; console.log('   FAIL ' + (e && e.stack || e)); }
clearTimeout(deadline);
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
