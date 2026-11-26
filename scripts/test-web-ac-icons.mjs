#!/usr/bin/env node
// test-web-ac-icons.mjs — V12 beta96 THE RIGHT GLYPH FOR EVERY AIRCRAFT on the AIR & SEA / aviation map (Stuart 11-26: "the
// correct icon for the aircraft type like the ADS-B map … I hope it's not just a generic plane icon"). Before beta96 the
// layer chose the glyph from the ADS-B emitter category ONLY, so every VDL2 / ACARS / HFDL / ADS-C aircraft and every
// remembered one (none of them carries a category) drew the generic jet.
// web/mapkit/adsblayer.js on the real MapLibre (directory/public/mapgl/vendor), fed exactly what the AIR & SEA map feeds it:
// live ADS-B aircraft (the decoder's own objects, with a category) and the aviation table's rows through the app's and web
// client's own converter (src/services/aviationMap.ts avPlaneAdsb — bundled here with esbuild). Headless Edge with
// --mute-audio (tests are silent). No server, no network.
//   node scripts/test-web-ac-icons.mjs
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { build } from 'esbuild';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const av = (await build({ entryPoints: [path.join(root, 'src/services/aviationMap.ts')], bundle: true, format: 'iife', globalName: 'AV',
  write: false, platform: 'browser', target: 'es2019', logLevel: 'warning' })).outputFiles[0].text;

const PAGE = '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
  + '<link rel="stylesheet" href="/v/maplibre-gl.css"><style>html,body,#m{margin:0;height:100%;background:#000}</style></head><body><div id="m"></div>'
  + '<script src="/v/maplibre-gl.js"></script><script src="/v/adsblayer.js"></script><script src="/v/av.js"></script><script>'
  + 'window.map=new maplibregl.Map({container:"m",style:{version:8,sources:{},layers:[{id:"bg",type:"background",paint:{"background-color":"#000"}}]},center:[-0.8,52.3],zoom:7});'
  + 'map.on("load",function(){window.layer=VibeAdsbLayer.create(map,{receiver:{lat:52.29,lon:-0.85},theme:"dark",prefs:{autoZoom:false,fit:false}});window.ready=1;});'
  + '</script></body></html>';
const FILES = { '/v/maplibre-gl.css': 'directory/public/mapgl/vendor/maplibre-gl.css', '/v/maplibre-gl.js': 'directory/public/mapgl/vendor/maplibre-gl.js',
                '/v/adsblayer.js': 'web/mapkit/adsblayer.js' };
const srv = http.createServer((q, r) => {
  if (q.url === '/') { r.setHeader('content-type', 'text/html'); return r.end(PAGE); }
  if (q.url === '/v/av.js') { r.setHeader('content-type', 'text/javascript'); return r.end(av); }
  const f = FILES[q.url];
  if (!f) { r.statusCode = 404; return r.end(); }
  r.setHeader('content-type', f.endsWith('.css') ? 'text/css' : 'text/javascript'); r.end(fs.readFileSync(path.join(root, f)));
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${srv.address().port}/`;
const dbg = await freePort();
const ud = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-acicons-'));
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  '--enable-unsafe-swiftshader', `--user-data-dir=${path.join(ud, 'edge')}`, '--window-size=1280,900', url], { stdio: 'ignore' });
const done = (code) => { try { edge.kill(); } catch {} srv.close(); setTimeout(() => { try { fs.rmSync(ud, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }); } catch {} process.exit(code); }, 500); };
const watchdog = setTimeout(() => { console.log('   FAIL timed out'); done(1); }, 90000);

try {
  let page = null;
  for (let i = 0; i < 40 && !page; i++) { await sleep(250); try { page = (await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json()).find((t) => t.type === 'page' && t.url.startsWith(url)); } catch {} }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? ('ERR ' + r.result?.exceptionDetails?.exception?.description); };
  for (let i = 0; i < 60 && (await ev('window.ready')) !== 1; i++) await sleep(250);
  ok((await ev('window.ready')) === 1, 'the map and the layer are up');

  // ── the AIR & SEA map's mixed list: live ADS-B objects (category) + the aviation table's rows through avPlaneAdsb ──
  const want = await ev(`(function () {
    var y = 52.0, step = 0.08, out = [], want = {};
    function at() { y += step; return { lat: y, lon: -0.9 + (out.length % 4) * 0.1 }; }
    function live(hex, extra, w) { var p = at(); out.push(Object.assign({ hex: hex, lat: p.lat, lon: p.lon, gs: 300, track: 45, alt_baro: 20000, seen: 0, seen_pos: 0, messages: 50 }, extra)); want[hex] = w; }
    function row(key, plane, w) { var p = at(); var a = AV.avPlaneAdsb(Object.assign({ key: key, icao: key, srcAge: {}, age: 20, n: 3, lat: p.lat, lon: p.lon, trk: 90, posAge: 20 }, plane)); out.push(a); want[a.hex] = w; }
    // live ADS-B: the emitter category wins, even over the aircraft data
    live('400001', { category: 'A7', t: 'A320', wtc: 'M', eng: '2J' }, 'heli');
    live('400002', { category: 'A5', t: 'B77W' }, 'heavy');
    live('400003', { category: 'A1', t: 'C172' }, 'light');
    live('400004', { category: 'A3', t: 'A320' }, 'jet');
    live('400005', { category: 'A0', t: 'EC35', species: 'H', wtc: 'L', eng: '2T' }, 'heli');   // A0 says nothing: the data decides
    // datalink / remembered rows: no category — the aircraft data decides (before beta96 every one of these was the jet)
    row('4CA001', { src: ['VDL2'], posSrc: 'VDL2', type: 'B744', info: { wtc: 'H', eng: '4J', species: 'L' } }, 'heavy');
    row('4CA002', { src: ['HFDL'], posSrc: 'HFDL', type: 'EC35', info: { wtc: 'L', eng: '2T', species: 'H' } }, 'heli');
    row('4CA003', { src: ['ACARS'], posSrc: 'ACARS', type: 'C172', info: { wtc: 'L', eng: '1P', species: 'L' } }, 'light');
    row('4CA004', { src: ['VDL2'], posSrc: 'ADS-B', lastSeen: '10:42', posAge: 900, type: 'A320', info: { wtc: 'M', eng: '2J', species: 'L' } }, 'jet');   // remembered ADS-B
    row('4CA005', { src: ['ADS-C'], posSrc: 'ADS-C', type: 'B77W' }, 'heavy');                                     // the type alone (an older server)
    row('4CA006', { src: ['VDL2'], posSrc: 'VDL2', type: 'AT76', info: { wtc: 'M', eng: '2T', species: 'L' } }, 'regional');
    row('4CA007', { src: ['HFDL'], posSrc: 'HFDL' }, 'jet');                                                        // nothing known
    window.__list = out;
    layer.update(out);
    return want;
  })()`);
  ok(want && typeof want === 'object' && Object.keys(want).length === 12, `twelve aircraft fed (${typeof want === 'string' ? want : Object.keys(want).length})`);
  await sleep(2600);   // past the glide-in
  const got = JSON.parse(await ev(`JSON.stringify((function () {
    var m = {};
    map.querySourceFeatures('vsadsb-ac').forEach(function (f) { var p = f.properties; if (p && p.h && p.img) m[p.h] = p.img; });
    return m;
  })())`));
  for (const [hex, kind] of Object.entries(want)) {
    const img = got[hex] || '';
    ok(new RegExp('^vsadsb-dark-' + kind + '-').test(img), `${hex} draws the ${kind} glyph (image ${img || 'none'})`);
  }
  // ★ the images really exist on the map (drawn from the same geometry), one per class used
  const classes = [...new Set(Object.values(want))];
  ok(await ev(`(${JSON.stringify(classes)}).every(function (k) { return Object.keys(map.style.imageManager.images || {}).some(function (i) { return i.indexOf('vsadsb-dark-' + k + '-') === 0; }) || map.listImages().some(function (i) { return i.indexOf('vsadsb-dark-' + k + '-') === 0; }); })`) === true,
     `every class's image is on the map (${classes.join(', ')})`);
  // ★ the aircraft data arriving LATER (the server's first poll goes without it) changes the glyph on the next update
  await ev(`(function () { var l = window.__list.map(function (a) { return a.hex === '4ca007' ? Object.assign({}, a, { t: 'GLID' }) : a; }); layer.update(l); return 1; })()`);
  await sleep(600);
  const later = JSON.parse(await ev(`JSON.stringify(map.querySourceFeatures('vsadsb-ac').filter(function (f) { return f.properties.h === '4ca007'; }).map(function (f) { return f.properties.img; }))`));
  ok(later.length && /^vsadsb-dark-glider-/.test(later[0]), `an answer that arrives later re-picks the glyph (4CA007 → glider: ${later[0]})`);
  ws.close();
} catch (e) { fail++; console.log('   FAIL', e && e.stack || e); }
clearTimeout(watchdog);
console.log(`test-web-ac-icons: ${pass} passed, ${fail} failed`);
done(fail ? 1 : 0);
