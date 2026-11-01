// test-web-navtex-map.mjs — V12 THE NAVTEX MAP, stage 1: the STATIONS RECEIVED (2026-11-01), in the REAL web page from a
// REAL vibeserver whose gallery holds a NAVTEX set (headless Edge, --mute-audio):
//   · GET /vibeserver/gallery/navtex-stations.json folds the gallery per station (the label every client shows)
//   · the gallery's standing "NAVTEX · stations received" row and its MAP; the NAVTEX box's MAP
//   · a marker per station with its letter, great-circle lines from the receiver, distances, decluttered names
//   · a station's card (first / last heard, messages, kinds, newest ids); the time filter; ALL by default
//   · LIVE: a message added to the gallery while the map is open appears without a reload
//   VIBESERVER_BIN=… VIBE_NAVTEX_SET=<dir with index.jsonl + files/> [VIBE_SHOT_DIR=…] node scripts/test-web-navtex-map.mjs
// ★ The set is NOT in git (a real server's gallery, ~4 500 messages); scripts/navtex-fixtures/ holds a small curated one.
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', SET = process.env.VIBE_NAVTEX_SET || path.join(root, 'scripts/navtex-fixtures');
const SHOTS = process.env.VIBE_SHOT_DIR || os.tmpdir();
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (![BIN, EDGE, path.join(SET, 'index.jsonl')].every((f) => f && fs.existsSync(f))) { console.log('   not run — VIBESERVER_BIN, a NAVTEX set and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 300 s'); process.exit(1); }, 300_000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-webnavmap-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run', 'edge', 'data/gallery']) fs.mkdirSync(path.join(dir, d), { recursive: true });
// ★ the set becomes this server's gallery: its index lines (kept as the gallery wrote them) and the files they name
const gal = path.join(dir, 'data/gallery');
const lines = fs.readFileSync(path.join(SET, 'index.jsonl'), 'utf8').split('\n').filter((l) => l.startsWith('{'));
const keep = [];
for (const l of lines) { const j = JSON.parse(l); const src = path.join(SET, 'files', j.f); if (fs.existsSync(src)) { fs.copyFileSync(src, path.join(gal, j.f)); keep.push(l); } }
keep.reverse();   // ★ the gallery's index is oldest first; gallery.json (where the set came from) is newest first
fs.writeFileSync(path.join(gal, 'index.jsonl'), keep.join('\n') + '\n');
console.log(`   a gallery of ${keep.length} NAVTEX messages`);
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'NAVTEX map web test', locator: 'IO92nh', adminPass: 'nav-map-admin',
  galleryMb: 100, radios: [{ serial: 'nav-map', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
await sleep(400);
// ★ the real basemap, as an installed server has it: the GPU map's packs and style (test-web-spots' way) and the bundled
//   vector map — without them the page draws an empty grid and the screenshots show no land
const MAPGL = path.join(dir, 'mapgl'); fs.mkdirSync(MAPGL);
for (const n of fs.readdirSync(path.join(root, 'directory/public/mapgl'))) fs.symlinkSync(path.join(root, 'directory/public/mapgl', n), path.join(MAPGL, n));
fs.copyFileSync(path.join(root, 'web/mapkit/vibemap-style.json'), path.join(MAPGL, 'vibemap-style.json'));
const env = { ...process.env, VIBESERVER_MAPGL_DIR: MAPGL, VIBESERVER_MAPDATA_DIR: path.join(root, 'assets/mapdata'), VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '518000', '--mode', 'usb', '--users', '1'],
                  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1400,900', 'about:blank'], { stdio: 'ignore' });
const H = `http://127.0.0.1:${port}`;
let wsC;
try {
  for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
  await sleep(1500);
  console.log('── the server: the stations received ──');
  const t0 = Date.now();
  const all = await fetch(`${H}/vibeserver/gallery/navtex-stations.json`).then((r) => r.json());
  const ms = Date.now() - t0;
  const names = all.stations.map((s) => `${s.b1} ${s.name}${s.conf === 'confident' ? '' : ' (' + s.conf + ')'} ${s.km} km ×${s.n}`);
  console.log(`   ${all.stations.length} stations, ${all.messages} messages, ${all.unplaced} not placed (${ms} ms):\n     ` + names.join('\n     '));
  ok(all.stations.length >= 5 && all.messages > 0, `stations folded from the gallery: ${all.stations.length}`);
  ok(all.rx && Math.abs(all.rx.lat - 52.3) < 0.2, 'the receiver is the server\'s own position (IO92nh)');
  ok(all.stations.every((s) => s.first <= s.last && s.n >= s.uniq && s.recent.length > 0 && s.recent.length <= 12), 'each: first ≤ last, received ≥ different, newest ids (≤ 12)');
  // ★ co-sited letters are ONE marker (src/services/navtexMap.ts navSites): the same name and place, a "?" on its own
  const sitesOf = (j) => { const g = []; for (const x of j.stations) { if (x.conf !== 'ambiguous' && g.some((y) => !y.q && y.name === x.name && Math.abs(y.lat - x.lat) < 0.5 && Math.abs(y.lon - x.lon) < 0.8)) continue; g.push({ q: x.conf === 'ambiguous', name: x.name, lat: x.lat, lon: x.lon }); } return g.length; };
  const nSites = sitesOf(all);
  ok(nSites < all.stations.length, `co-sited letters grouped: ${all.stations.length} stations at ${nSites} sites`);
  const day = await fetch(`${H}/vibeserver/gallery/navtex-stations.json?since=3600`).then((r) => r.json());
  ok(day.messages <= all.messages, `the time filter: 1 h holds ${day.messages} of ${all.messages}`);

  console.log('── the page ──');
  const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
  wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, name); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms2) => { for (let i = 0; i < ms2 / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
  const view = `[...document.querySelectorAll('.trkView')].find(v => !v.hidden)`;
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `${H}/` });
  await send('Emulation.setDeviceMetricsOverride', { width: 1400, height: 900, deviceScaleFactor: 1, mobile: false });
  await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
  await sleep(3000);
  for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn')?.onclick`)); i++) await sleep(250);
  await ev(`document.getElementById('galleryBtn').click()`);
  ok(await until(`!!document.querySelector('#galList .galNav')`, 10000), 'the gallery has a standing "NAVTEX · stations received" row');
  const row = await ev(`document.querySelector('#galList .galNav')?.textContent || ''`);
  ok(new RegExp(`${nSites} sites · ${all.stations.length} stations · ${all.messages} messages`).test(row), `…which counts them: ${row.slice(0, 140)}`);
  console.log('   screenshot', await shot('navtex-gallery-row.png'));
  await ev(`document.querySelector('#galList .galNav .galMap').click()`);
  // ★ the first opening of a big gallery: the warnings index is built while the map is up, and the map says so
  if (keep.length > 400) {
    ok(await until(`/Building the map from [\\d,]+ saved messages… \\d+ %/.test(${view}?.querySelector('.trkNote')?.textContent || '')`, 15000),
       `the backfill is shown: "${await ev(`${view}?.querySelector('.trkNote')?.textContent || ''`)}"`);
    console.log('   screenshot', await shot('navtex-map-building.png'));
    const tb = Date.now();
    ok(await until(`!${view}?.querySelector('.trkNote')`, 180000), `…and the note goes when it is built (${Math.round((Date.now() - tb) / 1000)} s, 2 s polls)`);
  }
  // ★ the list holds stations, the weather and the warnings (section heads); count only the stations' rows here
  const stationRows = `(() => { const out = []; for (const el of ${view}.querySelectorAll('.trkList .rows > *')) { if (el.classList.contains('sec')) { if (out.sec) break; out.sec = el.textContent; continue; } if (el.classList.contains('trkRow')) out.push(el); } return out; })()`;
  ok(await until(`!!${view} && ${stationRows}.length === ${nSites}`, 20000), `MAP: the sites in its list (${await ev(`${view} ? ${stationRows}.length : 0`)})`);
  ok(await until(`document.querySelectorAll('.vtl-mk').length >= ${nSites}`, 15000), `a marker per site: ${await ev(`document.querySelectorAll('.vtl-mk').length`)}`);
  await sleep(4000);
  const arcs = Number(await ev(`document.querySelector('[data-vtl-arcs]')?.getAttribute('data-vtl-arcs') || 0`));
  ok(arcs === nSites, `one great-circle line from the receiver to each site: ${arcs}`);
  ok(JSON.parse(await ev(`JSON.stringify([...${view}.querySelectorAll('[data-tgl]')].map(b => b.textContent + ':' + b.getAttribute('aria-pressed')))`)).join(' ')
     === 'STATIONS:true WARNINGS:true WEATHER:true', 'the layers: STATIONS · WARNINGS · WEATHER, all on');
  ok(JSON.parse(await ev(`JSON.stringify([...${view}.querySelectorAll('[data-rng]')].map(b => b.textContent + ':' + b.getAttribute('aria-pressed')))`)).join(' ')
     === '1 H:false 24 H:false 7 DAYS:false 30 DAYS:false ALL:true', 'the time filter: five ranges, ALL by default (a received-stations view)');
  const st = await ev(`${view}.querySelector('.st').textContent`);
  ok(/\d+ sites · \d+ stations · \d+ messages · furthest/.test(st), `status: ${st}`);
  const dc = JSON.parse(await ev(`JSON.stringify((() => { const hit = (a, b) => a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    const l = [...document.querySelectorAll('.vtl-mk .vtl-lb')].filter(x => getComputedStyle(x).display !== 'none').map(x => x.getBoundingClientRect());
    let o = 0; for (let i = 0; i < l.length; i++) for (let j = i + 1; j < l.length; j++) if (hit(l[i], l[j])) o++; return { o, n: l.length }; })())`));
  ok(dc.o === 0, `no two shown names overlap (${dc.n} shown)`);
  console.log('   screenshot', await shot('navtex-map.png'));
  // a station's card
  await ev(`(() => { const r = ${stationRows}.find(x => /Oostende|Cullercoats|Niton/.test(x.textContent)); r && r.click(); })()`);
  await sleep(2500);
  const card = await ev(`(document.querySelector('.vtl-card:not([hidden])') || {}).textContent || ''`);
  ok(/Named by the message itself or its station label: \d+ of \d+ messages?/.test(card) && /Nearest [A-Z] station on \d+(\.\d)? kHz/.test(card) && /schedule slot/.test(card) && !/\d+ %/.test(card), `the why, in plain words: ${(card.match(/Named by[^·]*|Ended inside[^·]*/g) || []).join(' | ')}`);
  ok(/First heard \d{4}-\d\d-\d\d/.test(card) && /[A-Z] · \d+(\.\d)? kHz · \d+ received, \d+ different/.test(card) && /Newest: [A-Z]{2}\d\d/.test(card), `a station's card: ${card.slice(0, 220)}`);
  // ★ the tidied card (2026-11-01): the essentials shown, the rest under a closed "More"
  const ess = JSON.parse(await ev(`JSON.stringify((() => { const c = document.querySelector('.vtl-card:not([hidden])'); const d = c && c.querySelector('details.navMore');
    const vis = c ? [...c.querySelectorAll('.pop > div')].map(x => x.textContent).join(' | ') : ''; return { more: !!d && !d.open, vis, h: c ? c.getBoundingClientRect().height : 0 }; })())`));
  ok(ess.more && /Last heard .* · \d+ messages?/.test(ess.vis) && /kHz/.test(ess.vis) && !/First heard/.test(ess.vis), `the card's essentials first, "More" closed: ${ess.vis.slice(0, 200)} (${Math.round(ess.h)} px tall)`);
  ok(/Niton/.test(card) ? (card.match(/ · \d+(\.\d)? kHz · \d+ received/g) || []).length >= 2 : true, 'a co-sited card lists each of its letters');
  console.log('   screenshot', await shot('navtex-map-station.png'));
  console.log('── the warnings ──');
  const tw = Date.now();
  const W = await fetch(`${H}/vibeserver/gallery/navtex-warnings.json`).then((r) => r.json());
  const wms = Date.now() - tw;
  const shapes = W.warnings.reduce((o, w) => { const k = w.shape || (w.pts.length > 1 ? 'points' : 'one'); o[k] = (o[k] || 0) + 1; return o; }, {});
  const withSt = W.warnings.filter((w) => w.st).length, withQ = W.warnings.filter((w) => w.q).length, faded = W.warnings.filter((w) => w.expired).length;
  const specific = W.warnings.filter((w) => w.cls !== 'general').length;
  console.log(`   ${W.receptions} receptions → ${W.warnings.length ? W.total : 0} warnings with a position (${W.live} messages live, ${W.indexed} indexed, ${W.pending} pending; ${wms} ms)`);
  console.log(`   classes ${JSON.stringify(W.classes)}; shapes ${JSON.stringify(shapes)}; station ${withSt}, quality ${withQ}, expired/cancelled ${faded}, a specific icon ${specific}`);
  ok(W.pending === 0 && W.indexed === W.live, 'the index is built: every live message indexed');
  ok(W.warnings.length > 0 && withSt > W.warnings.length * 0.7, `the card's station for most (${withSt} of ${W.warnings.length})`);
  ok(withQ > W.warnings.length * 0.5, `…and the decode quality (${withQ})`);
  ok(!JSON.stringify(W.classes).includes('"met"'), 'a forecast is never a warning marker');
  const M = await fetch(`${H}/vibeserver/gallery/navtex-met.json`).then((r) => r.json());
  const gales = M.areas.filter((a) => a.fc[0].gale >= 8).length;
  console.log(`   met: ${M.areas.length} sea areas with a forecast, ${gales} gale or worse in their latest; history up to ${Math.max(0, ...M.areas.map((a) => a.fc.length))}`);
  ok(M.areas.length > 0 && M.areas.every((a) => a.fc.length >= 1 && a.fc.every((f, i) => !i || f.t <= a.fc[i - 1].t)), 'the latest forecast per area first, its history after');
  // the page: markers, shapes, clusters
  await ev(`${view}.querySelector('.fit').click()`); await sleep(2500);
  const clusters = Number(await ev(`document.querySelector('[data-vtl-clusters]')?.getAttribute('data-vtl-clusters') || 0`));
  const nShapes = Number(await ev(`document.querySelector('[data-vtl-shapes]')?.getAttribute('data-vtl-shapes') || 0`));
  const markers = Number(await ev(`document.querySelectorAll('.vtl-mk').length`));
  console.log(`   on the map: ${markers} markers, ${clusters} gathered, ${nShapes} shapes`);
  // ★★ LATEST WINS: only what is in force is drawn — under ALL too; a named list is a marker an item, with no shape
  const drawn = W.warnings.filter((w) => !w.expired && !w.cancelled && !w.supersededBy);
  const lists = drawn.filter((w) => Array.isArray(w.items) && w.items.length === w.pts.length && !w.shape);
  console.log(`   in force ${drawn.length} of ${W.warnings.length} (cancelled ${W.cancelledN}, superseded ${W.supersededN}, expired ${W.expiredN}); named lists ${lists.length} → ${lists.reduce((n, w) => n + w.items.length, 0)} markers`);
  ok(nShapes === drawn.filter((w) => w.pts.length > 1 && !lists.includes(w)).length, `a shape for every multi-point warning in force, none for a named list (${nShapes})`);
  ok(await ev(`![...document.querySelectorAll('.vtl-mk')].some(e => /cancelled|superseded|expired/.test(e.getAttribute('aria-label') || ''))`), 'ALL draws no cancelled, superseded or expired warning');
  ok(await ev(`[...document.querySelectorAll('.trkRow')].every(r => !/cancelled|superseded|· expired/.test(r.textContent))`), '…nor lists one');
  for (const w of lists.slice(0, 3)) {
    const n = Number(await ev(`[...document.querySelectorAll('.vtl-mk')].filter(e => (e.getAttribute('aria-label') || '').includes('Warning ${w.id}, ')).length`));
    ok(n === w.items.length, `${w.id}'s named list: ${n} markers, one an item (${w.items.map((x) => x.n).join(', ')})`);
  }
  if (drawn.length > 60) ok(clusters > 0 && await ev(`document.querySelectorAll('.vtl-mk.vtl-clhid').length > 0`), `zoomed out, warnings gathered into numbered markers (${clusters})`);
  ok(await ev(`[...document.querySelectorAll('.vtl-mk')].filter(e => !e.classList.contains('vtl-clhid') && !e.classList.contains('vtl-yhid')).every(e => e.getAttribute('role') === 'button' && e.tabIndex === 0 && (e.getAttribute('aria-label') || '').length > 8)`),
     'every marker is a button in the tab order, with its words');
  console.log('   screenshot', await shot('navtex-map-warnings.png'));
  // ★★ DECLUTTER (2026-11-01): all three layers on, over the UK — the weather labels give way to station names and
  //    warnings; nothing shown overlaps. At the default zoom (fitted on the UK) and one zoom out.
  const overlaps = `JSON.stringify((() => { const hit = (a, b) => a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    const shown = (e) => e && getComputedStyle(e).display !== 'none' && !e.closest('.vtl-clhid') && !e.closest('.vtl-yhid');
    const mks = [...document.querySelectorAll('.vtl-mk')].filter(shown);
    const met = mks.filter(m => /^Sea area/.test(m.getAttribute('aria-label') || '')).map(m => m.querySelector('.vtl-ic').getBoundingClientRect());
    const other = mks.filter(m => !/^Sea area/.test(m.getAttribute('aria-label') || '')).map(m => m.querySelector('.vtl-ic').getBoundingClientRect());
    const names = [...document.querySelectorAll('.vtl-mk .vtl-lb')].filter(x => shown(x) && !x.classList.contains('vtl-lbhide') && x.closest('.vtl-labels')).map(x => x.getBoundingClientRect());
    let o = 0; for (const r of met) { for (const q of other) if (hit(r, q)) o++; for (const q of names) if (hit(r, q)) o++; }
    return { o, met: met.length, other: other.length, names: names.length, yielded: Number(document.querySelector('[data-vtl-yielded]')?.getAttribute('data-vtl-yielded') || 0) };
  })())`;
  for (const look of ['default', 'zoomout', 'uk']) {
    await ev(`(() => { const x = document.querySelector('.vtl-card:not([hidden]) .vtl-x'); x && x.click(); })()`); await sleep(400);
    await ev(`${view}.querySelector('.fit').click()`); await sleep(1800);   // ★ the default: the view as it opens, fitted
    if (look === 'zoomout') { await ev(`(() => { const b = document.querySelector('.maplibregl-ctrl-zoom-out, .leaflet-control-zoom-out'); b && b.click(); })()`); await sleep(1500); }
    if (look === 'uk') {   // ★ the crowded UK close up: focus a UK station (the view's own focus zoom), its card closed
      await ev(`(() => { const r = ${stationRows}.find(x => /Cullercoats/.test(x.textContent)); r && r.click(); })()`); await sleep(1800);
      await ev(`(() => { const x = document.querySelector('.vtl-card:not([hidden]) .vtl-x'); x && x.click(); })()`); await sleep(600);
    }
    const d = JSON.parse(await ev(overlaps));
    ok(d.o === 0, `declutter (${look}): ${d.met} weather labels shown, ${d.yielded} gave way, ${d.other} other markers, ${d.names} names — no weather label over a marker or a name (${d.o})`);
    console.log('   screenshot', await shot(`navtex-map-declutter-${look}.png`));
  }
  // ★ the v6 icons on the map: the platforms (most of them unlit) and the cables and pipelines — each opened from its row
  for (const [name, re] of [['platforms', /^platform$/], ['cables', /^(cable|cable_telecom|pipeline|pipeline_gas)$/], ['power-cables', /^cable_power$/]]) {
    const w = drawn.filter((x) => re.test(x.cls)).sort((a, b) => (b.items?.length || 0) - (a.items?.length || 0) || Number(b.unlit) - Number(a.unlit))[0];
    if (!w) { console.log(`   (no ${name} in force in this set)`); continue; }
    await ev(`(() => { const r = [...document.querySelectorAll('.trkRow')].find(x => x.dataset.key === ${JSON.stringify(`w:${w.id}:${w.first}`)}); r && r.click(); })()`); await sleep(2200);
    await ev(`(() => { const x = document.querySelector('.vtl-card:not([hidden]) .vtl-x'); x && x.click(); })()`); await sleep(700);
    // ★ close enough that the warnings stand apart (below zoom 8 they gather into numbered markers)
    for (let z = 0; z < 2; z++) { await ev(`(() => { const b = document.querySelector('.maplibregl-ctrl-zoom-in, .leaflet-control-zoom-in'); b && b.click(); })()`); await sleep(1200); }
    const kinds = JSON.stringify(drawn.filter((x) => re.test(x.cls)).map((x) => x.cls + (x.unlit ? '+unlit' : '')));
    ok(Number(await ev(`[...document.querySelectorAll('.vtl-mk')].filter(e => (e.getAttribute('aria-label') || '').startsWith('Warning ${w.id}')).length`)) >= 1, `${name} on the map: ${w.id} (${kinds})`);
    console.log('   screenshot', await shot(`navtex-map-${name}.png`));
  }
  await ev(`${view}.querySelector('.fit').click()`); await sleep(1500);
  // a cluster: Enter zooms in to it
  if (clusters) {
    const z0 = await ev(`(() => { const e = [...document.querySelectorAll('.vtl-mk')].find(x => { const b = x.querySelector('.vtl-cl'); return b && !b.hidden; }); if (!e) return ''; e.focus(); return e.getAttribute('aria-label'); })()`);
    console.log('   screenshot', await shot('navtex-map-clusters.png'));
    await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
    await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter' });
    await sleep(1500);
    ok(/gathered/.test(z0), `a gathered marker says so: "${String(z0).slice(0, 90)}"`);
  }
  // a warning's card, from a non-English message when there is one
  const pick = W.warnings.find((w) => w.hit && w.st && !w.expired) || W.warnings.find((w) => w.st && !w.expired) || W.warnings[0];
  const key = `w:${pick.id}:${pick.first}`;
  await ev(`(() => { const r = ${view}.querySelector('.trkRow[data-key="${key}"]'); r && r.click(); return !!r; })()`);
  ok(await until(`/Best copy/.test((document.querySelector('.vtl-card:not([hidden])') || {}).textContent || '')`, 8000), `a warning's card opens from its row (${pick.id})`);
  ok(await until(`(() => { const p = document.querySelector('.vtl-card:not([hidden]) .nvTx'); return p && !/^Loading/.test(p.textContent) && /ZCZC|NAVTEX/.test(p.textContent); })()`, 8000), '…its full gallery text, fetched when it opened');
  const wcard = await ev(`document.querySelector('.vtl-card:not([hidden])').textContent`);
  ok(new RegExp(pick.id).test(wcard) && /First heard\s*\d{4}-\d\d-\d\d/.test(wcard) && /Last heard/.test(wcard) && /\d+ times? here/.test(wcard), 'the card: id, first / last heard, times heard');
  ok(/Station/.test(wcard) && (pick.sc === 'likely' ? /Suspected /.test(wcard) : pick.sc === 'ambiguous' ? /\?/.test(wcard) : true), `…the station as the server says (${pick.sc}: ${pick.st})`);
  if (pick.hit) ok(new RegExp(`\\(from [A-Z][a-z]+: ${pick.hit.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\)`).test(wcard) && !/translat/i.test(wcard.split('The message')[0]), `the summary names the message's own word, never as a translation: ${(wcard.match(/\(from [^)]*\)/) || [''])[0]}`);
  console.log('   screenshot', await shot('navtex-map-warning-card.png'));
  // keyboard: a marker by Enter → its card, the focus in it; Escape → closed, the focus back
  await ev(`${view}.querySelector('.vtl-card .vtl-x')?.click()`);
  await ev(`(() => { const e = [...document.querySelectorAll('.vtl-mk[role=button]')].find(x => !x.classList.contains('vtl-clhid') && !(x.querySelector('.vtl-cl') && !x.querySelector('.vtl-cl').hidden)); e && e.focus(); })()`);
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter' });
  ok(await until(`!!document.querySelector('.vtl-card:not([hidden])') && document.querySelector('.vtl-card').contains(document.activeElement)`, 4000), 'keyboard: Enter on a marker opens its card and moves the focus into it');
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Escape', code: 'Escape', windowsVirtualKeyCode: 27 });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Escape', code: 'Escape' });
  ok(await until(`!document.querySelector('.vtl-card:not([hidden])') && document.activeElement?.classList.contains('vtl-mk')`, 4000), '…Escape closes it, the focus back on the marker');
  // the weather
  if (M.areas.length) {
    // ★ a CURRENT forecast (one over a day old is listed as "no current forecast" and never drawn — latest wins)
    const cur = M.areas.filter((a) => M.now - a.fc[0].t <= M.stale);
    const stale = M.areas.filter((a) => M.now - a.fc[0].t > M.stale);
    if (stale.length) ok(await ev(`!document.querySelector('.vtl-mk[aria-label^="Sea area ${stale[0].name}:"]') && /no current forecast/.test(${view}.querySelector('.trkRow[data-key="m:${stale[0].id}"]')?.textContent || '')`),
      `${stale[0].name}'s newest forecast is over a day old: not drawn, listed as "no current forecast" (${stale.length} such areas)`);
    const ar = (cur.length ? cur : M.areas).slice().sort((a, b) => (b.fc[0].gale >= 8) - (a.fc[0].gale >= 8))[0];
    await ev(`(() => { const r = ${view}.querySelector('.trkRow[data-key="m:${ar.id}"]'); r && r.click(); })()`);
    ok(await until(`/Earlier forecasts|Forecast/.test((document.querySelector('.vtl-card:not([hidden])') || {}).textContent || '')`, 6000), `a sea area's card: ${ar.name}`);
    const mc = await ev(`document.querySelector('.vtl-card:not([hidden])').textContent`);
    ok(/Station/.test(mc) && /Heard/.test(mc), `…its forecast, when heard and the station: ${mc.slice(0, 160)}`);
    await sleep(800);
    console.log('   screenshot', await shot('navtex-map-met.png'));
    await ev(`${view}.querySelector('.vtl-card .vtl-x')?.click()`);
  }
  // layers off and on
  const before = Number(await ev(`document.querySelectorAll('.vtl-mk').length`));
  await ev(`${view}.querySelector('[data-tgl="warnings"]').click()`); await sleep(1800);
  const without = Number(await ev(`document.querySelectorAll('.vtl-mk').length`));
  ok(without < before && Number(await ev(`document.querySelector('[data-vtl-shapes]')?.getAttribute('data-vtl-shapes') || 0`)) === 0, `WARNINGS off: ${before} → ${without} markers, no shapes`);
  ok(await ev(`localStorage.getItem('vibesdr.navMapLayers')`) === JSON.stringify({ stations: true, warnings: false, weather: true }), '…remembered');
  await ev(`${view}.querySelector('[data-tgl="weather"]').click()`); await sleep(1500);
  await ev(`${view}.querySelector('.fit').click()`); await sleep(1500);
  console.log('   screenshot', await shot('navtex-map-stations-only.png'));
  await ev(`${view}.querySelector('[data-tgl="weather"]').click()`); await ev(`${view}.querySelector('[data-tgl="warnings"]').click()`);
  await sleep(2500);
  // LIVE: a new warning while the map is open pulses, the map not reloaded
  await ev(`(() => { window.__navMarker = 1; const of = window.fetch; window.fetch = async (u, o) => { const r = await of(u, o); if (!String(u).includes('navtex-warnings.json')) return r;
    const j = await r.json(); j.warnings.unshift({ id: 'ZZ99', khz: 518, first: j.now, last: j.now, n: 1, lost: 0, unlit: 0, expired: 0, cls: 'wreck', f: 'none.txt', pts: [[51.2, 1.6]] });
    return new Response(JSON.stringify(j), { headers: { 'content-type': 'application/json' } }); }; })()`);
  await ev(`${view}.querySelector('[data-rng="all"]').click()`);
  ok(await until(`!!document.querySelector('.vtl-mk.vtl-pulse') && window.__navMarker === 1`, 12000), 'LIVE: a new warning appears pulsing, without the map being reloaded');
  // 7 DAYS by keyboard
  await ev(`${view}.querySelector('[data-rng="7d"]').focus()`);
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, text: '\r' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter' });
  const wk = await fetch(`${H}/vibeserver/gallery/navtex-stations.json?since=${7 * 86400}`).then((r) => r.json());
  ok(await until(`${stationRows}.length === ${sitesOf(wk)}`, 15000), `7 DAYS (keyboard): ${sitesOf(wk)} sites`);
  ok(await ev(`localStorage.getItem('vibesdr.navMapRange') === '7d'`), '…remembered');
  await ev(`${view}.querySelector('[data-rng="all"]').click()`);
  await sleep(1500);
  await ev(`${view}.querySelector('.exit').click()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 4000), 'CLOSE MAP');
} catch (e) { fail++; console.log('   FAIL', e?.message || e); }
finally {
  clearTimeout(watchdog);
  try { wsC?.close(); } catch { /* gone */ }
  edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(600);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
