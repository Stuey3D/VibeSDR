#!/usr/bin/env node
// test-web-adsb-card.mjs — V12 beta94 THE AIRCRAFT CARD'S x (Stuart, 2026-11-25: "sometimes the x button to close it greys
// out. work around is to minimise it and then press x"). The card used to scroll in MapLibre's popup content, the element
// MapLibre puts its x in: an overlay scrollbar (macOS, a trackpad) lay over the x and took the press; a scrolled card took
// the x with it; and every full refresh (setHTML) made a NEW x, so a press across a refresh was lost. Folded, the card did not
// scroll — so its x worked. web/mapkit/adsblayer.js itself, on the real MapLibre (directory/public/mapgl/vendor), in headless
// Edge with --mute-audio (tests are silent), at desktop and 390 px. No server, no network.
//   node scripts/test-web-adsb-card.mjs
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   not run — Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const PAGE = '<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
  + '<link rel="stylesheet" href="/v/maplibre-gl.css"><style>html,body,#m{margin:0;height:100%;background:#000}</style></head><body><div id="m"></div>'
  + '<script src="/v/maplibre-gl.js"></script><script src="/v/adsblayer.js"></script><script>'
  + 'window.sel=[];window.map=new maplibregl.Map({container:"m",style:{version:8,sources:{},layers:[{id:"bg",type:"background",paint:{"background-color":"#000"}}]},center:[-0.8,52.3],zoom:7});'
  + 'map.on("load",function(){window.layer=VibeAdsbLayer.create(map,{receiver:{lat:52.29,lon:-0.85},theme:"dark",prefs:{autoZoom:false},onSelect:function(h){sel.push(h)}});window.ready=1;});'
  + '</script></body></html>';
const FILES = { '/v/maplibre-gl.css': 'directory/public/mapgl/vendor/maplibre-gl.css', '/v/maplibre-gl.js': 'directory/public/mapgl/vendor/maplibre-gl.js',
                '/v/adsblayer.js': 'web/mapkit/adsblayer.js' };
const srv = http.createServer((q, r) => {
  if (q.url === '/') { r.setHeader('content-type', 'text/html'); return r.end(PAGE); }
  const f = FILES[q.url];
  if (!f) { r.statusCode = 404; return r.end(); }
  r.setHeader('content-type', f.endsWith('.css') ? 'text/css' : 'text/javascript'); r.end(fs.readFileSync(path.join(root, f)));
});
await new Promise((r) => srv.listen(0, '127.0.0.1', r));
const url = `http://127.0.0.1:${srv.address().port}/`;
const dbg = await freePort();
const ud = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-adsbcard-'));
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  '--enable-unsafe-swiftshader', `--user-data-dir=${path.join(ud, 'edge')}`, '--window-size=1280,900', url], { stdio: 'ignore' });
const done = (code) => { try { edge.kill(); } catch {} srv.close(); setTimeout(() => { try { fs.rmSync(ud, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 }); } catch {} process.exit(code); }, 500); };
const watchdog = setTimeout(() => { console.log('   FAIL timed out'); done(1); }, 120000);

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

  // A card taller than its box: the aviation map's ten source lines, Comm-B-sized. seen = seconds since the last message.
  await ev(`window.mk = function (seen, route) { return [{ hex: '4ca97b', lat: 52.4, lon: -0.7, gs: 420, track: 90, alt_baro: 35000, alt_geom: 35400, baro_rate: -640,
    flight: 'RYR123', r: 'EI-ABC', t: 'B738', squawk: '7243', category: 'A3', seen: seen, rssi: -12.3, messages: 100 + seen,
    route: route ? { from: 'EGSS', to: 'LEMG', fromName: 'London Stansted Airport', toName: 'Malaga Airport' } : null,
    rowsExtra: [['Sources','ADS-B'],['Last','one'],['Two','2'],['Three','3'],['Four','4'],['Five','5'],['Six','6'],['Seven','7'],['Eight','8'],['Nine','9']] }]; }; 1`);
  const X = `document.querySelector('.vsadsb-pop-dark .maplibregl-popup-close-button')`;
  const geo = async () => JSON.parse(await ev(`(() => { const b = ${X}; if (!b) return 'null';
    const r = b.getBoundingClientRect(), cx = r.x + r.width / 2, cy = r.y + r.height / 2, hit = document.elementFromPoint(cx, cy);
    const sc = document.querySelector('.vsadsb-pop-dark .vsadsb-sc'), cs = getComputedStyle(b);
    let scrolling = null; for (let e = b.parentElement; e && !e.classList.contains('maplibregl-popup'); e = e.parentElement) {
      const o = getComputedStyle(e).overflowY; if ((o === 'auto' || o === 'scroll') && e.scrollHeight > e.clientHeight) scrolling = e.className; }
    return JSON.stringify({ x: r.x, y: r.y, w: r.width, h: r.height, cx, cy, hitX: hit === b, same: b === window.__x, inSc: !!(sc && sc.contains(b)), scrolling,
      scTall: sc ? sc.scrollHeight > sc.clientHeight + 4 : false, disabled: b.disabled, op: cs.opacity, pe: cs.pointerEvents, col: cs.color, vis: cs.visibility }); })()`));
  const press = async (x, y, mid) => {
    await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y }); await sleep(150);
    await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
    if (mid) { await ev(mid); await sleep(100); }
    await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 }); await sleep(250);
  };
  const open = () => ev(`document.querySelectorAll('.vsadsb-pop-dark').length`);

  for (const vp of [null, { width: 390, height: 700 }]) {
    const tag = vp ? '390 px' : 'desktop';
    if (vp) { await send('Emulation.setDeviceMetricsOverride', { width: vp.width, height: vp.height, deviceScaleFactor: 2, mobile: true }); await sleep(400); }
    await ev(`map.resize(); map.jumpTo({ center: [-0.7, 52.25], zoom: 7 }); 1`); await sleep(300);   // ★ the card on screen, below the top
    await ev(`layer.update(mk(0)); layer.focus('4ca97b'); window.__x = ${X}; 1`);
    await sleep(500);
    let g = await geo();
    ok(g && g.scTall, `${tag}: the card is taller than its box — it scrolls`);
    ok(g && !g.inSc && g.scrolling === null, `${tag}: the x is not inside anything that scrolls (overlay scrollbar never lies on it) [${g && g.scrolling}]`);
    ok(g && g.hitX, `${tag}: the x is what a press at its centre lands on`);
    const col0 = g.col, y0 = g.y;
    // ── a card updated every second for 6 s (a route arrives at 3 s — a refresh of the WHOLE card), then going stale ──
    for (let s = 1; s <= 6; s++) { await ev(`layer.update(mk(${s}, ${s >= 3})); 1`); await sleep(1000); }
    g = await geo();
    ok(g && g.same, `${tag}: after 6 s of updates and a whole-card refresh the x is the SAME node (never replaced under a press)`);
    ok(g && !!(await ev(`!!document.querySelector('.vsadsb-pop-dark .rt')`)), `${tag}: …and the whole-card refresh did land (the route line)`);
    for (const s of [15, 35, 50]) { await ev(`layer.update(mk(${s}, true)); 1`); await sleep(400); }
    g = await geo();
    ok(g && !g.disabled && g.op === '1' && g.pe !== 'none' && g.vis === 'visible' && g.col === col0, `${tag}: a stale aircraft's x is not greyed or disabled (${g && [g.disabled, g.op, g.pe, g.col].join(' ')})`);
    // ★ scrolled to the bottom (headless emulation sends no scroll event for a scrollTop set by script, so one is sent — the
    //   layer's own capture-phase listener judges the cue)
    await ev(`(() => { const sc = document.querySelector('.vsadsb-pop-dark .vsadsb-sc'); sc.scrollTop = sc.scrollHeight; sc.dispatchEvent(new Event('scroll')); return 1; })()`); await sleep(200);
    g = await geo();
    ok(g && Math.abs(g.y - y0) < 1 && g.hitX, `${tag}: scrolled to the bottom, the x has not moved and is still on top (y ${g && g.y} vs ${y0})`);
    ok((await ev(`document.querySelector('.vsadsb-pop-dark .vsadsb-sc').classList.contains('vsadsb-more-up')`)) === true, `${tag}: …and the "more above" cue shows`);
    // ── a press that straddles a whole-card refresh still closes it, on the first press ──
    await ev('window.sel.length = 0; 1');
    await press(g.cx, g.cy, `layer.update(mk(51, false)); 1`);
    ok((await open()) === 0, `${tag}: the first press on the x closes the card — even with a whole-card refresh between press and release`);
    ok((await ev('JSON.stringify(window.sel)')) === '[null]', `${tag}: …and the host is told the selection cleared`);
    // ── fold, unfold, close: the same x throughout ──
    await ev(`layer.update(mk(0)); layer.focus('4ca97b'); window.__x = ${X}; 1`); await sleep(400);
    const fold = JSON.parse(await ev(`JSON.stringify((() => { const b = document.querySelector('.vsadsb-pop-dark .vsadsb-min').getBoundingClientRect(); return { x: b.x + b.width / 2, y: b.y + b.height / 2 }; })())`));
    await press(fold.x, fold.y);
    ok(!!(await ev(`!!document.querySelector('.vsadsb-pop-dark .t.mn')`)), `${tag}: the fold key folds the card`);
    g = await geo();
    ok(g && g.same && g.hitX, `${tag}: folded, the x is the same node and on top`);
    const unf = JSON.parse(await ev(`JSON.stringify((() => { const b = document.querySelector('.vsadsb-pop-dark .vsadsb-min').getBoundingClientRect(); return { x: b.x + b.width / 2, y: b.y + b.height / 2 }; })())`));
    await press(unf.x, unf.y);
    ok(!(await ev(`!!document.querySelector('.vsadsb-pop-dark .t.mn')`)), `${tag}: …and opens it again`);
    g = await geo();
    await press(g.cx, g.cy);
    ok((await open()) === 0, `${tag}: unfolded again, the x closes it on the first press`);
  }
  ws.close();
} catch (e) { fail++; console.log('   FAIL', e && e.stack || e); }
clearTimeout(watchdog);
console.log(`test-web-adsb-card: ${pass} passed, ${fail} failed`);
done(fail ? 1 : 0);
