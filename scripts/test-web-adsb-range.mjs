#!/usr/bin/env node
// test-web-adsb-range.mjs — V12 beta94 THE ADS-B RANGE OUTLINE on the map (web/mapkit/adsblayer.js; the server's half is
// vibeserver/test-adsb-range.cpp). The real MapLibre (directory/public/mapgl/vendor) and the layer in headless Edge with
// --mute-audio (tests are silent), against a fixture GET /adsb/range: the outline drawn from the fixture's bins (a polygon
// with one vertex per heard bearing, under the aircraft), the RANGE toggle (off hides it, and the host is told — so it is
// remembered: a layer made again with those prefs starts off), the key's "Furthest: … · Furthest ever: …" and its all-time
// dotted line (off by default), no RANGE button from a server without the endpoint, the dark and light themes.
// Screenshots: $VIBE_SHOTS (a directory) if set.
//   node scripts/test-web-adsb-range.mjs
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
const SHOTS = process.env.VIBE_SHOTS || '';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// ── the fixture: a jagged outline, far to the south-east (the sea), short behind a hill to the west; ten bearings unheard ──
const km = [], allKm = [];
for (let i = 0; i < 360; i++) {
  const v = i >= 250 && i < 260 ? 0 : 230 + 120 * Math.cos((i - 140) * Math.PI / 180) + 35 * Math.sin(i * 0.9) + ((i * 37) % 23);
  km.push(Math.round(v * 10) / 10);
  allKm.push(v ? Math.round((v + (i > 100 && i < 170 ? 140 : 15)) * 10) / 10 : 0);
}
let far = 0; for (let i = 1; i < 360; i++) if (km[i] > km[far]) far = i;
const RANGE = { ok: 1, window: 30, lat: 52.29, lon: -0.85, since: 1762128000, now: 1764072000, km, day: km.map(() => 20416), allKm, days: 23,
  far: { km: km[far], brg: far, at: 1762128000 + 86400 * 5 }, record: { km: 512, brg: 160, at: 1762128000 } };
let hits = 0;

const PAGE = (theme, prefs, base, norx) => '<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="/v/maplibre-gl.css">'
  + '<style>html,body,#m{margin:0;height:100%;background:' + (theme === 'light' ? '#efece4' : '#05080a') + '}</style></head><body><div id="m"></div>'
  + '<script src="/v/maplibre-gl.js"></script><script src="/v/adsblayer.js"></script><script>'
  + 'window.onerror=function(m){window.err=String(m)};window.saved=[];window.map=new maplibregl.Map({container:"m",style:{version:8,sources:{},layers:[{id:"bg",type:"background",paint:{"background-color":"' + (theme === 'light' ? '#efece4' : '#05080a') + '"}}]},center:[-0.3,52.0],zoom:5.4});'
  + 'map.on("load",function(){window.layer=VibeAdsbLayer.create(map,{receiver:' + (norx ? 'null' : '{lat:52.29,lon:-0.85}') + ',theme:"' + theme + '",imageBase:' + JSON.stringify(base) + ','
  + 'prefs:Object.assign({autoZoom:false},' + JSON.stringify(prefs) + '),onPrefs:function(p){saved.push(p)}});'
  + 'layer.update([{hex:"4ca97b",lat:51.6,lon:1.2,gs:420,track:90,alt_baro:35000,flight:"RYR123",seen:0},{hex:"400f11",lat:53.4,lon:-2.6,gs:300,track:200,alt_baro:12000,flight:"BAW45",seen:12},'
  + '{hex:"3c6444",lat:50.4,lon:-1.9,gs:380,track:10,alt_baro:30000,flight:"DLH9",seen:40}]);window.ready=1;});'
  + '</script></body></html>';
const FILES = { '/v/maplibre-gl.css': 'directory/public/mapgl/vendor/maplibre-gl.css', '/v/maplibre-gl.js': 'directory/public/mapgl/vendor/maplibre-gl.js',
                '/v/adsblayer.js': 'web/mapkit/adsblayer.js' };
const srv = http.createServer((q, r) => {
  const u = new URL(q.url, 'http://x');
  if (u.pathname === '/page') { r.setHeader('content-type', 'text/html'); return r.end(PAGE(u.searchParams.get('theme'), JSON.parse(u.searchParams.get('prefs') || '{}'), u.searchParams.get('base'), u.searchParams.get('norx') === '1')); }
  if (u.pathname === '/adsb/range') { hits++; r.setHeader('content-type', 'application/json'); return r.end(JSON.stringify(RANGE)); }
  const f = FILES[u.pathname];
  if (!f) { r.statusCode = 404; return r.end(); }
  r.setHeader('content-type', f.endsWith('.css') ? 'text/css' : 'text/javascript'); r.end(fs.readFileSync(path.join(root, f)));
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${srv.address().port}`;
const dbg = await freePort();
const ud = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-adsbrange-'));
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  '--enable-unsafe-swiftshader', `--user-data-dir=${path.join(ud, 'edge')}`, '--window-size=1100,800', 'about:blank'], { stdio: 'ignore' });
const done = (code) => { try { edge.kill(); } catch {} srv.close(); setTimeout(() => { try { fs.rmSync(ud, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }); } catch {} process.exit(code); }, 500); };
const watchdog = setTimeout(() => { console.log('   FAIL timed out'); done(1); }, 420000);   // ★ 11-28 +2 min: the facing checks (a fresh map, the 3.5 s datalink glide)

try {
  let page = null;
  for (let i = 0; i < 40 && !page; i++) { await sleep(250); try { page = (await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json()).find((t) => t.type === 'page' && t.url === 'about:blank'); } catch {} }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? ('ERR ' + r.result?.exceptionDetails?.exception?.description); };
  let navs = 0;
  // ★ a few maps in one headless Edge (SwiftShader): a later one can take well over 15 s to load — waited for, up to 40 s
  const open = async (theme, prefs, base = '/adsb/', norx = false) => {
    const n = ++navs;   // ★ wait for THIS page: the last one's window.ready answers until the navigation lands
    // ★ beta95: headless Edge was marking the page HIDDEN after a couple of maps (MapLibre then never fires "load" —
    //   no frames): brought to the front, focus emulated, before each one
    await send('Page.bringToFront'); await send('Emulation.setFocusEmulationEnabled', { enabled: true });
    await send('Page.navigate', { url: `${origin}/page?n=${n}&theme=${theme}&prefs=${encodeURIComponent(JSON.stringify(prefs))}&base=${encodeURIComponent(base)}${norx ? '&norx=1' : ''}` });
    for (let i = 0; i < 160; i++) { await sleep(250); if ((await ev(`location.search.startsWith('?n=${n}&') && window.ready === 1`)) === true) break; }
    if ((await ev('window.ready === 1')) !== true) console.log('   (map not ready)', await ev(`JSON.stringify({ vis: document.visibilityState, focus: document.hasFocus(), style: window.map && map.isStyleLoaded(), err: String(window.err), gl: !!(window.map && map.painter) })`));
    await sleep(1200);
  };
  const shown = async () => JSON.parse(await ev('JSON.stringify(layer.rangeShown())'));
  const feats = async () => JSON.parse(await ev(`JSON.stringify(map.querySourceFeatures('vsadsb-range').map(f => f.properties.k + ':' + f.geometry.type))`));
  const btn = `[...document.querySelectorAll('.vsadsb-ctl button')].find(b => b.textContent === 'RANGE')`;
  // ★★★ beta96 rangeShape (gaps kept, dips closed, needles softened): the vertices drawn are the bearings it leaves > 0 —
  //   the layer's own rule, run here from the same file
  const mctx = { console, Math, Number, String, Set, Map, Array, Object, JSON, isFinite, setTimeout, clearTimeout }; mctx.globalThis = mctx;
  vm.createContext(mctx); vm.runInContext(fs.readFileSync(path.join(root, 'web/mapkit/adsblayer.js'), 'utf8'), mctx);
  // ★ b96-rangefeed: one vertex per BOUNDARY between bins with either side heard (rangeRing)
  const lends = (a) => { const v = mctx.VibeAdsbLayer.maths.rangeShape(a), n = v.length; let c = 0; for (let i = 0; i < n; i++) if (Math.max(v[i], v[(i - 1 + n) % n]) > 0) c++; return c; };
  const heard = lends(km), heardAll = lends(allKm);

  // an older server: no /adsb/range → no button, nothing drawn
  const before = hits;
  await open('dark', {}, '/old/');
  const old = await ev(`JSON.stringify({ ready: window.ready === 1, hidden: ${btn}.hidden, shown: layer.rangeShown() })`);
  ok(old === '{"ready":true,"hidden":true,"shown":null}', `a server without the outline: no RANGE button, nothing drawn (${old})`);
  ok(hits === before, '…(it asked the old server, not this one)');
  // ★★ beta95 THE LIVE OUTLINE: the same server-less map grows its own from the positions it is sent — the second
  //    consistent position counts (the first proves nothing), a jump that is not flight does not
  const step = (lat, lon, seenPos) => `layer.update([{hex:"77aa11",lat:${lat},lon:${lon},gs:420,track:90,alt_baro:35000,flight:"EZY77",seen:0,seen_pos:${seenPos}}])`;
  await ev(step(51.6, 1.200, 0)); await sleep(1800);
  let live = await ev('JSON.stringify(layer.rangeShown())');
  ok(live === 'null', `live: a first-seen aircraft's first position proves nothing… (${live})`);
  await ev(step(51.6, 1.205, 0)); await sleep(1800);
  live = JSON.parse(await ev('JSON.stringify(layer.rangeShown())'));
  ok(live && live.live === 1 && live.window === null, `live: …the next grows the outline at once, with no server outline (${live && live.live} bearing)`);
  ok((await ev(`${btn}.hidden`)) === false, 'live: the RANGE button appears with it');
  ok(live && / NM at \d+°/.test(live.title) && /since this map opened/.test(live.title), `live: its title, in NM, since the map opened ("${live && live.title}")`);
  await ev(step(40.0, -20.0, 0)); await sleep(1800);
  ok(JSON.parse(await ev('JSON.stringify(layer.rangeShown())')).live === 1, 'live: a jump that is not flight (1 000 NM in a second) grows nothing');
  // ★★ b96-rangefeed (Stuart 11-27): THE LIVE OUTLINE AS FAST AS THE DATA, ADS-B ONLY.
  //   · a DATALINK position (sparse) never grows it, and is never the first of an ADS-B pair: an ADS-B fix after it waits
  //     for its own second one;
  //   · a position's own time counts, not the glide's (clamped at 60 s): an edge-of-range aircraft whose last fix was two
  //     minutes old, then a fresh one 25 NM on, is flight (730 kt) — it grows the outline at once, not one fix later.
  const row = (hex, b, nm, seenPos, sparse) => `(() => { const p = VibeAdsbLayer.maths.destination(52.29, -0.85, ${b}, ${nm}); layer.update([{hex:"${hex}",lat:p.lat,lon:p.lon,alt_baro:35000,seen:0,seen_pos:${seenPos}${sparse ? ',sparse:true' : ''}}]); return 1; })()`;
  const liveN = async () => JSON.parse(await ev('JSON.stringify(layer.rangeShown())')).live;
  const n0 = await liveN();
  await ev(row('aa0001', 300.5, 150, 0, true)); await sleep(1000); await ev(row('aa0001', 300.5, 150.1, 0, true)); await sleep(1800);
  ok((await liveN()) === n0, `live: a datalink (sparse) aircraft's two fixes grow nothing (${await liveN()} bearings)`);
  await ev(row('aa0001', 300.5, 150.2, 0, false)); await sleep(1800);
  ok((await liveN()) === n0, 'live: …and its ADS-B fix after a datalink one is a FIRST fix (no pair across the two)');
  await ev(row('aa0001', 300.5, 150.3, 0, false)); await sleep(1800);
  ok((await liveN()) === n0 + 1, `live: …its second ADS-B fix grows it (${await liveN()} bearings)`);
  await ev(row('aa0002', 40.5, 120, 120, false)); await sleep(1100);
  await ev(row('aa0002', 40.5, 145, 0, false)); await sleep(1800);
  ok((await liveN()) === n0 + 2, `live: a fix two minutes old then a fresh one 25 NM on is flight, counted at once (${await liveN()} bearings)`);
  // ★★ b96-2 "NOT AT THIS SITE" (Stuart 11-27): an aircraft the server marked offsite (an off-site feed's only) is drawn but
  //    never offered to the live outline; the same aircraft's next fix without the mark (heard here too) is a FIRST fix
  const rowOff = (hex, b, nm) => `(() => { const p = VibeAdsbLayer.maths.destination(52.29, -0.85, ${b}, ${nm}); layer.update([{hex:"${hex}",lat:p.lat,lon:p.lon,alt_baro:35000,seen:0,seen_pos:0,offsite:true}]); return 1; })()`;
  await ev(rowOff('aa0003', 200.5, 180)); await sleep(1000); await ev(rowOff('aa0003', 200.5, 180.1)); await sleep(1000); await ev(rowOff('aa0003', 200.5, 180.2)); await sleep(1800);
  ok((await liveN()) === n0 + 2, `live: an off-site feed's aircraft (offsite) — three fixes grow nothing (${await liveN()} bearings)`);
  ok((await ev(`layer.focus('aa0003') !== false`)) === true, '…and it is on the map all the same (its card opens)');
  await ev(row('aa0003', 200.5, 180.3, 0, false)); await sleep(1800);
  ok((await liveN()) === n0 + 2, 'live: …its first fix without the mark is a FIRST fix (no pair across the two)');
  await ev(row('aa0003', 200.5, 180.4, 0, false)); await sleep(1800);
  ok((await liveN()) === n0 + 3, `live: …the next grows it (${await liveN()} bearings)`);
  // (before the themes' maps: each opens a fresh layer, so these aircraft disturb nothing after)
  // ★★ 11-28 FACING ITS WAY FROM TWO FIXES (Stuart: "with HFDL, UberSDR shows the planes pointing the correct direction")
  {
    const E = 0.5 / Math.cos(55.25 * Math.PI / 180);
    const dl = (hex, lat, lon, seenPos, extra = '') => `layer.update([{hex:"${hex}",lat:${lat},lon:${lon},gs:470,alt_baro:35000,seen:0,seen_pos:${seenPos},sparse:true,posSrc:"HFDL"${extra}}])`;
    const one = async (hex) => JSON.parse(await ev(`JSON.stringify(layer.shown().find((a) => a.hex === '${hex}') || null)`));
    await ev(dl('bb0001', 55.0, -20.0, 600)); await sleep(400);
    let a = await one('bb0001');
    ok(a && a.face == null && a.track == null, `one HFDL fix: the default orientation (${JSON.stringify(a && { face: a.face, track: a.track })})`);
    await ev(dl('bb0001', 55.5, -20.0 + E, 0)); await sleep(4500);   // ★ past the datalink glide (SPARSE_GLIDE_MS 3.5 s)
    a = await one('bb0001');
    ok(a && Math.abs(a.face - 45) < 1.5 && a.track == null, `★★ two HFDL fixes NE of each other: it faces ≈ 45° (${a && a.face && a.face.toFixed(1)}), with no track to fly on`);
    const p0 = a && [a.lat, a.lon]; await sleep(3000); a = await one('bb0001');
    ok(a && Math.abs(a.lat - 55.5) < 1e-4 && Math.abs(a.lon - (-20 + E)) < 1e-4 && Math.abs(p0[0] - a.lat) < 1e-6, `★★ …and it is NOT dead-reckoned on that heading (gs 470: still at its fix ${JSON.stringify(p0)} → ${JSON.stringify(a && [a.lat, a.lon])})`);
    await ev(dl('bb0001', 55.509, -20.0 + E, 0)); await sleep(500);
    a = await one('bb0001');
    ok(a && Math.abs(a.face - 45) < 1.5, `a fix 1 km on: the heading unchanged (${a && a.face && a.face.toFixed(1)})`);
    await ev(dl('bb0002', 50.0, 0.0, 1900)); await sleep(300); await ev(dl('bb0002', 51.0, 1.0, 0)); await sleep(500);
    a = await one('bb0002');
    ok(a && a.face == null, 'the previous fix over 30 min older: no derived heading');
    await ev(dl('bb0003', 50.0, 0.0, 60)); await sleep(300); await ev(`layer.update([{hex:"bb0003",lat:50.5,lon:0.5,gs:470,track:270,alt_baro:35000,seen:0,seen_pos:0,sparse:true,posSrc:"ADS-C"}])`); await sleep(600);
    a = await one('bb0003');
    ok(a && a.track === 270 && a.face == null && Math.abs(a.hdg - 270) < 5, `a reported track wins (${a && a.track} / ${a && a.hdg && a.hdg.toFixed(0)})`);
    await ev(dl('bb0004', 50.0, 0.0, 0, ',track:135,trkSrc:"derived"')); await sleep(600);
    a = await one('bb0004');
    ok(a && a.track == null && a.face === 135, `the server's derived heading ("trkSrc":"derived") turns the icon, never a track (${a && a.face} / ${a && a.track})`);
  }
  for (const theme of ['dark', 'light']) {
    await open(theme, {});
    let s = await shown();
    ok(s && s.points === heard, `${theme}: the outline drawn from the fixture — one vertex per heard bearing (${s && s.points} of ${heard})`);
    ok(s && s.allPoints === 0, `${theme}: the all-time dotted line is off by default`);
    ok((await feats()).includes('w:Polygon'), `${theme}: a filled polygon on the GPU map`);
    const order = await ev(`JSON.stringify(map.getStyle().layers.map(l => l.id).filter(i => /^vsadsb-(range-line|rings|icons)$/.test(i)))`);
    ok(order === '["vsadsb-range-line","vsadsb-rings","vsadsb-icons"]', `${theme}: under the rings and the aircraft (${order})`);
    const paint = JSON.parse(await ev(`JSON.stringify({ w: map.getPaintProperty('vsadsb-range-line','line-width'), c: map.getPaintProperty('vsadsb-range-line','line-color'), f: map.getPaintProperty('vsadsb-range-fill','fill-opacity') })`));
    ok(paint.w >= 2.5 && paint.f < 0.15, `${theme}: a bold edge (${paint.w} px, ${paint.c}) over a faint fill (${paint.f})`);
    ok((await ev(`${btn}.hidden === false && ${btn}.classList.contains('on')`)) === true, `${theme}: the RANGE toggle is there and on`);
    ok(/furthest \d+ NM at \d+°, 8 Nov/.test(s.title), `${theme}: its title says the furthest catch ("${s.title}")`);
    await ev('layer.toggleLegend(true)'); await sleep(300);
    const key = await ev(`document.querySelector('.vsadsb-leg .vsadsb-rg').innerText`);
    ok(/Furthest: \d+ NM at \d+°, 8 Nov/.test(key) && /Furthest ever: 276 NM at 160°, 3 Nov/.test(key) && /last 30 days/.test(key), `${theme}: the key — window, furthest, furthest ever`);
    if (SHOTS) { fs.mkdirSync(SHOTS, { recursive: true }); const p = await send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(path.join(SHOTS, `range-${theme}.png`), Buffer.from(p.result.data, 'base64')); }
    // the all-time line
    await ev(`(() => { const c = document.querySelector('.vsadsb-leg .vsadsb-rg-all'); c.click(); return 1; })()`); await sleep(400);
    s = await shown();
    ok(s.allPoints === heardAll && (await feats()).includes('a:LineString'), `${theme}: ticking "All-time outline" draws the dotted line`);
    ok((await ev('JSON.stringify(saved[saved.length-1])')).includes('"rangeAll":true'), `${theme}: …and the host is told (remembered)`);
    if (SHOTS) { const p = await send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(path.join(SHOTS, `range-${theme}-alltime.png`), Buffer.from(p.result.data, 'base64')); }
    // the toggle off, remembered
    await ev(`${btn}.click()`); await sleep(400);
    s = await shown();
    ok(s.points === 0 && (await feats()).length === 0, `${theme}: RANGE off hides it`);
    const last = await ev('JSON.stringify(saved[saved.length-1])');
    ok(last.includes('"range":false'), `${theme}: …and the host is told: ${last}`);
    await open(theme, JSON.parse(last));
    s = await shown();
    ok(s && s.points === 0 && (await ev(`${btn}.classList.contains('on')`)) === false, `${theme}: made again with the saved prefs, it starts off`);
    await ev(`${btn}.click()`); await sleep(400);
    ok((await shown()).points === heard, `${theme}: …and RANGE turns it back on`);
  }
  // ★★ b96-rangefeed (Stuart 11-27, the Lenovo: "i cant activate the range rings either"): a host with NO receiver
  //    position (a server set up with a locator only, the viewer opened from the front door) — the outline's centre is the
  //    receiver, so RINGS and FIT work, and their titles say the position is approximate; with no outline either, they are
  //    greyed WITH the reason (never dead)
  const rbtn = (t) => `[...document.querySelectorAll('.vsadsb-ctl button')].find(b => b.textContent === '${t}')`;
  const before2 = hits;
  await open('dark', {}, '/old/', true);
  const none = JSON.parse(await ev(`JSON.stringify({ rx: layer.receiverNow(), d: ${rbtn('RINGS')}.disabled, f: ${rbtn('FIT')}.disabled, t: ${rbtn('RINGS')}.title })`));
  ok(none.rx === null && none.d && none.f && /^Not available: .*receiver position/.test(none.t), `no position anywhere: RINGS / FIT greyed, the title says why ("${none.t}")`);
  await open('dark', {}, '/adsb/', true);
  const rr = JSON.parse(await ev(`JSON.stringify({ rx: layer.receiverNow(), d: ${rbtn('RINGS')}.disabled, f: ${rbtn('FIT')}.disabled, t: ${rbtn('RINGS')}.title })`));
  ok(rr.rx && rr.rx.from === 'range' && Math.abs(rr.rx.lat - 52.29) < 1e-9 && Math.abs(rr.rx.lon + 0.85) < 1e-9, `no host position: the receiver is the range outline's centre (${JSON.stringify(rr.rx)})`);
  ok(!rr.d && !rr.f && /approximate/.test(rr.t), `…RINGS and FIT work, their title says approximate ("${rr.t}")`);
  await ev(`${rbtn('RINGS')}.classList.contains('on') || ${rbtn('RINGS')}.click()`); await sleep(500);
  ok((await ev(`map.getSource('vsadsb-rings') ? map.querySourceFeatures('vsadsb-rings').length > 0 || JSON.stringify(map.getStyle().sources['vsadsb-rings'].data).includes('LineString') : false`)) === true, '…and the rings are drawn around it');
  await ev(`layer.setReceiver(52.24, -0.90, 'approximate — the centre of locator square IO92NG')`); await sleep(300);
  const hr = JSON.parse(await ev(`JSON.stringify({ rx: layer.receiverNow(), t: ${rbtn('FIT')}.title })`));
  ok(hr.rx.from === 'host' && hr.rx.lat === 52.24 && /IO92NG/.test(hr.t), `the host's position (its locator square's centre) wins, and the titles name the square ("${hr.t}")`);
  ok(hits === before2 + 1, `…one range fetch for that map (${hits - before2})`);
  // and merged with the server's: a far live catch beyond the fixture on bearing ~95° widens it
  await open('dark', {});
  const pre = await shown();
  const far = (d) => `(() => { const M = VibeAdsbLayer.maths, p = M.destination(52.29, -0.85, 95.5, ${d}); return 'layer.update([{hex:"abc123",lat:' + p.lat + ',lon:' + p.lon + ',gs:450,track:95,alt_baro:38000,seen:0}])'; })()`;
  await ev(await ev(far(330))); await ev('new Promise(r => setTimeout(r, 1100))'); await ev(await ev(far(330.2)));
  await sleep(1900);
  const post = await shown();
  ok(post.live === 1 && post.points >= pre.points && post.ring.some((p) => Math.abs(p[0] - (-0.85)) > 7), `live + server: the far live catch is merged into the server's outline (${post.live} live bearing, ${post.points} points)`);
  ok(hits === 6, `the outline is fetched once per map opened, not per report (${hits} for 6 maps on this server)`);
  ws.close();
} catch (e) { fail++; console.log('   FAIL', e && e.stack || e); }
clearTimeout(watchdog);
console.log(`test-web-adsb-range: ${pass} passed, ${fail} failed`);
done(fail ? 1 : 0);
