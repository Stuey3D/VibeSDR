// test-server-map-water-nl.mjs — THE NETHERLANDS' WATER on a SERVER's GPU map, with AND without High Detail Maps, in a
// real (headless, MUTED) Edge against a real vibeserver.
//   ★★ beta96 (Stuart 11-26, "the Netherlands now doesn't have enough water"): the basic pack's lakes/rivers/islands/
//      reefs/playas are its tier1 data, z4–5 only, and the style's basic source runs to z6 — so with no detail pack
//      the IJsselmeer, the Markermeer and every lake drew as LAND from z6 up. Fixed in the SHARED renderer
//      (vibemapgl.js prepareStyle, vibesdr:basicOnlyZ5 → source vs-basic-z5) — the server web map, the app and the
//      directory all run it. The directory's own check is directory/scripts/test-map-water-nl.mjs (main).
//   ★★ beta96 too (Stuart 11-26, a tester's 600 AIS ships at Rotterdam all "on land"): the RIVERS >= 200 m now in the
//      basic pack (gen-river-areas.mjs --basic, layer riverareas, z5–8; style riverareas-b on source vs-basic-rivers,
//      stopping at z8 where the detail pack's riverareas-d starts). Points checked against the Overture polygons first
//      (DuckDB ST_Contains; mean width in brackets):
//      water: Rotterdam Erasmusbrug (266 m) · Nieuwe Waterweg (468) · Thames at Tower Bridge (208) and London Bridge (208)
//             · Antwerp Het Steen (273) and south (272) · Saigon Bach Dang (372) and Thanh Da (230)
//      land:  Rotterdam Coolsingel, Rotterdam Zuid, Rozenburg, the City of London, Southwark, Antwerp centre, Saigon D1
//      at z8–11 (z7 too for the Nieuwe Waterweg, wide enough for z7; Thanh Da from z9 — at z8 its 230 m is 0.77 px at
//      10°N, drawn but sub-pixel, so no pixel is blue, with or without the detail pack), in all three phases.
//   Samples the rendered PIXEL at fixed points, z5–10, as the server page builds its map (mapgl.ts probeMapGL:
//   the style and the files from the server's /mapgl/, HEAD vibemap-detail.pmtiles decides the detail pack):
//      water: IJsselmeer 52.75N 5.35E · Markermeer 52.55N 5.20E · Wadden Sea 53.20N 5.30E
//      land:  Lelystad 52.50N 5.47E · Emmeloord 52.71N 5.75E
//   WATER = blue clearly beats green (lakes #1b4a6b, sea #123049); LAND otherwise.
//   Phase 1 runs with no detail pack; phase 2 links VIBE_DETAIL_PACK into the server's data dir (the pack the server
//   would download, 250,319,197 B — any other size is replaced by the server after 120 s, so only that one).
//   Phase 3 is the APP's path (no rangeBase: each bundled pack read whole and served from memory, mapglPack.ts → the
//   WebView), no detail pack — the second source shares the one in-memory 'basic' pack (the whole-read fallback).
//   Phases 4–5 (beta96) are the app's RANGE path, which the app now uses: packRange (+ detailRange in phase 5) instead
//   of whole reads, each read round-tripped through base64 as the bridge does. Before the phases, a fetch test of the
//   server's /mapgl/vibemap-basic.pmtiles?r=<basicRev>. MEASURE_MEM=1 instead measures the WebView's RSS, whole vs range.
//   The server's /mapgl/ is THIS checkout's directory/public/mapgl + web/mapkit style (as CMake installs them), so a
//   prebuilt vibeserver is enough: the map files are read off disk, not compiled in.
//   VIBESERVER_BIN=… [VIBE_DETAIL_PACK=…/vibemap-detail.pmtiles] node scripts/test-server-map-water-nl.mjs [screenshotDir]
//   (exit 3 = not run)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const DETAIL = process.env.VIBE_DETAIL_PACK || '';
const SHOTS = process.argv[2] || '';
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (![BIN, EDGE].every((f) => f && fs.existsSync(f))) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
if (SHOTS) fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const procs = [];
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-mapwater-'));
const cleanup = () => { for (const p of procs) { try { p.kill('SIGKILL'); } catch {} } try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} };
process.on('exit', cleanup);
setTimeout(() => { console.log('   FAIL deadline (900 s)'); process.exit(1); }, 900000).unref();

// ── the server: this checkout's /mapgl/, a data dir whose mapgl/ may hold the detail pack ──
for (const d of ['data/mapgl', 'run', 'edge']) fs.mkdirSync(path.join(dir, d), { recursive: true });
const MAPGL = path.join(dir, 'mapgl'); fs.mkdirSync(MAPGL);
for (const n of fs.readdirSync(path.join(root, 'directory/public/mapgl'))) fs.symlinkSync(path.join(root, 'directory/public/mapgl', n), path.join(MAPGL, n));
fs.copyFileSync(path.join(root, 'web/mapkit/vibemap-style.json'), path.join(MAPGL, 'vibemap-style.json'));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Map water test', locator: 'JO22oj', adminPass: 'map-water-admin',
  radios: [{ serial: 'map-water', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
procs.push(spawn('nice', ['-n', '10', process.execPath, path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' }));
await sleep(400);
const env = { ...process.env, VIBESERVER_MAPGL_DIR: MAPGL, VIBESERVER_MAPDATA_DIR: path.join(root, 'assets/mapdata'), VIBESERVER_CONFIG: path.join(dir, 'config.json'),
  VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
procs.push(spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '100000000', '--mode', 'wfm', '--users', '1'],
  { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] }));
const H = `http://127.0.0.1:${port}`;
for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }

// ── a 1-pixel PNG reader (Page.captureScreenshot with a 1×1 clip) ──
function pngPixel(buf) {
  let o = 8; const idat = [];
  while (o < buf.length) {
    const len = buf.readUInt32BE(o), type = buf.toString('ascii', o + 4, o + 8);
    if (type === 'IDAT') idat.push(buf.subarray(o + 8, o + 8 + len));
    o += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  return [raw[1], raw[2], raw[3]];
}

// ── the browser (★★ TESTS MUST BE SILENT: --mute-audio) ──
procs.push(spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--no-default-browser-check',
  '--disable-extensions', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist',
  `--remote-debugging-port=${dbg}`, `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' }));
let target;
for (let i = 0; i < 60 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${dbg}/json`)).json()).find((t) => t.type === 'page'); } catch {} if (!target) await sleep(250); }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0; const pending = new Map();
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const js = async (expr) => { const r = await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.exception?.description; };
const until = async (expr, ms = 20000) => { const end = Date.now() + ms; while (Date.now() < end) { if ((await js(expr)) === true) return true; await sleep(200); } return false; };
await call('Runtime.enable'); await call('Page.enable');

const NL = [5, 6, 7, 8, 9, 10], RIV = [8, 9, 10, 11];
const POINTS = [
  ['IJsselmeer', 52.75, 5.35, 'water', NL], ['Markermeer', 52.55, 5.20, 'water', NL], ['Wadden Sea', 53.20, 5.30, 'water', NL],
  ['Lelystad', 52.50, 5.47, 'land', NL], ['Emmeloord', 52.71, 5.75, 'land', NL],
  ['Rotterdam Erasmusbrug', 51.9078, 4.4868, 'water', RIV], ['Nieuwe Waterweg', 51.9460, 4.1800, 'water', [7, ...RIV]],
  ['Thames Tower Bridge', 51.5055, -0.0754, 'water', RIV], ['Thames London Bridge', 51.5080, -0.0870, 'water', RIV],
  ['Antwerp Het Steen', 51.2240, 4.3930, 'water', RIV], ['Antwerp Scheldt south', 51.2150, 4.3900, 'water', RIV],
  ['Saigon Bach Dang', 10.7745, 106.7075, 'water', RIV], ['Saigon Thanh Da', 10.8160, 106.7260, 'water', [9, 10, 11]],
  ['Rotterdam Coolsingel', 51.9200, 4.4790, 'land', RIV], ['Rotterdam Zuid', 51.8950, 4.4950, 'land', RIV],
  ['Rozenburg', 51.9050, 4.2500, 'land', RIV], ['City of London', 51.5140, -0.0900, 'land', RIV],
  ['Southwark', 51.5000, -0.0800, 'land', RIV], ['Antwerp centre', 51.2170, 4.4100, 'land', RIV],
  ['Saigon D1', 10.7760, 106.7000, 'land', RIV],
];
const ZOOMS = [...new Set(POINTS.flatMap((p) => p[4]))].sort((a, b) => a - b);
const detailLink = path.join(dir, 'data/mapgl/vibemap-detail.pmtiles');

// ★ the map as the server page makes it (mapgl.ts probeMapGL + loadMapGLScripts + VibeMapGL.create), on a bare page
//   of the server's own origin (a 404 under /mapgl/ — nothing else on it, no radio UI over the sample points)
/* ★★ beta96 THE APP'S RANGE PATH (mapglPack.ts readPackRange → MapOverlay's packRange): vibemapgl is handed packRange
 *  (and detailRange when the detail pack is in) instead of reading the bundled packs whole. Here the bridge is played
 *  by an HTTP range read from the server plus the same base64 round trip the WebView does (__mgDeliver), so the bytes
 *  vibemapgl sees are exactly what the app's bridge would hand it. app = 'whole' | 'range' | false (the server page). */
const OPEN = (app, withDetail = false) => `(async () => {
  document.documentElement.innerHTML = '<head></head><body style="margin:0"><div id="m" style="position:fixed;inset:0"></div></body>';
  const base = location.origin + '/mapgl/';
  for (const f of ['maplibre-gl.js', 'pmtiles.js', 'vibemapgl.js']) {
    await new Promise((res, rej) => { const s = document.createElement('script'); s.src = base + 'vendor/' + f; s.onload = res; s.onerror = () => rej(new Error(f)); document.head.appendChild(s); });
  }
  const style = await (await fetch(base + 'vibemap-style.json', { cache: 'no-cache' })).json();
  const detail = ${app && !withDetail ? 'false' : "(await fetch(base + 'vibemap-detail.pmtiles', { method: 'HEAD', cache: 'no-store' })).status === 200"};
  const load = (p, kind) => fetch(p).then((r) => r.ok ? (kind === 'text' ? r.text() : r.arrayBuffer()) : null).catch(() => null);
  const bridged = (name) => (off, len) => fetch(base + name, { headers: { Range: 'bytes=' + off + '-' + (off + len - 1) }, cache: 'no-store' })
    .then((r) => (r.status === 206 || r.status === 200) ? r.arrayBuffer() : null)
    .then((b) => { if (!b) return null; const u = new Uint8Array(b); let bin = ''; for (let i = 0; i < u.length; i += 32768) bin += String.fromCharCode.apply(null, u.subarray(i, i + 32768));
      const s = atob(btoa(bin)), o = new Uint8Array(s.length); for (let i = 0; i < s.length; i++) o[i] = s.charCodeAt(i); window.__rangeReads = (window.__rangeReads || 0) + 1; window.__rangeBytes = (window.__rangeBytes || 0) + o.length; return o.buffer; })
    .catch(() => null);
  const packRange = ${app === 'range' ? '(name, off, len) => bridged(name)(off, len)' : 'undefined'};
  const detailRange = ${app && withDetail ? "detail ? bridged('vibemap-detail.pmtiles') : undefined" : 'undefined'};
  window.VM = await VibeMapGL.create(document.getElementById('m'), { style, load, base, rangeBase: ${app ? 'undefined' : 'base'}, packRange, detailRange, detail, profile: 'directory', center: [5.4, 52.7], zoom: 5 });
  window.VM_DETAIL = detail;
  return true;
})()`;

/* ★★ beta96 THE BUILT-IN SERVER SERVES THE ONE BASIC PACK at /mapgl/vibemap-basic.pmtiles?r=<basicRev> (the APK's server reads
 *  the same file the app does, mapgl-bundled/): a ranged GET with the revision query answers 206, the PMTiles magic and the
 *  whole file's size; a HEAD answers the size. */
async function serverServesPack() {
  console.log('── the server serves the basic pack ──');
  const style = JSON.parse(fs.readFileSync(path.join(root, 'web/mapkit/vibemap-style.json'), 'utf8'));
  const rev = style.metadata && style.metadata['vibesdr:basicRev'];
  const size = fs.statSync(path.join(root, 'directory/public/mapgl/vibemap-basic.pmtiles')).size;
  ok(!!rev, `the style carries vibesdr:basicRev (${rev})`);
  const r = await fetch(`${H}/mapgl/vibemap-basic.pmtiles?r=${encodeURIComponent(rev)}`, { headers: { Range: 'bytes=0-6' } });
  const body = Buffer.from(await r.arrayBuffer());
  ok(r.status === 206 && body.toString('ascii') === 'PMTiles', `ranged GET ?r=${rev}: ${r.status}, "${body.toString('ascii')}"`);
  ok(r.headers.get('content-range') === `bytes 0-6/${size}`, `Content-Range ${r.headers.get('content-range')} (file ${size} B)`);
  const h = await fetch(`${H}/mapgl/vibemap-basic.pmtiles?r=${encodeURIComponent(rev)}`, { method: 'HEAD' });
  ok(h.status === 200 && Number(h.headers.get('content-length')) === size, `HEAD: ${h.status}, ${h.headers.get('content-length')} B`);
}

/* ★★ MEASURE_MEM=1: the map WebView's memory, the app's WHOLE-read path against its RANGE path (beta96). A fresh page per run,
 *  the map opened over the Netherlands at z5 and then the same 6 views, idle, a forced GC, then the RSS of every renderer process
 *  (ps, this Edge's --user-data-dir). Alternated whole/range 3 times so drift shows. Chromium's renderer = the Android
 *  System WebView's engine. The JUDGED figure is V8's ArrayBuffer backing stores (Runtime.getHeapUsage), where
 *  the whole path's packs live; renderer RSS is printed too but swings by hundreds of MB under SwiftShader (11-26 runs). */
async function measureMemory() {
  const views = [[5.4, 52.7, 5], [4.48, 51.91, 8], [-0.08, 51.505, 9], [106.7, 10.78, 8], [0, 30, 1], [5.4, 52.7, 7]];
  const rss = async () => {
    // ★ this test's own Edge only (its private --user-data-dir), renderer processes only (the page's JS + its ArrayBuffers)
    let kb = 0;
    for (const line of execFileSync('ps', ['-ax', '-ww', '-o', 'rss=,command=']).toString().split('\n')) {
      if (line.includes('--type=renderer') && line.includes(dir)) kb += Number(line.trim().split(/\s+/)[0]) || 0;
    }
    return kb / 1024;
  };
  const res = { whole: [], range: [] };
  for (let round = 0; round < 3; round++) {
    for (const mode of ['whole', 'range']) {
      await call('Page.navigate', { url: `${H}/mapgl/none-${Date.now()}` });
      await sleep(800);
      if ((await js(OPEN(mode))) !== true || !(await until(`!!window.VM && VM.map.loaded()`, 30000))) { ok(false, `memory ${mode}: map up`); continue; }
      for (const [lon, lat, z] of views) {
        await js(`VM.map.jumpTo({ center: [${lon}, ${lat}], zoom: ${z} }), true`);
        await js(`new Promise((r) => { VM.map.once('idle', () => r(true)); VM.map.triggerRepaint(); setTimeout(() => r(false), 8000); })`);
      }
      await call('HeapProfiler.collectGarbage'); await sleep(1500); await call('HeapProfiler.collectGarbage'); await sleep(500);
      const mb = await rss();
      // ★ the deterministic figure: V8's ArrayBuffer backing stores + JS heap of the page (Runtime.getHeapUsage)
      const hu = (await call('Runtime.getHeapUsage')).result || {};
      const ab = (hu.backingStorageSize || 0) / 1048576, heap = (hu.usedSize || 0) / 1048576;
      res[mode + 'Ab'] = (res[mode + 'Ab'] || []).concat(ab);
      const rr = await js(`JSON.stringify({ n: window.__rangeReads || 0, b: window.__rangeBytes || 0 })`);
      res[mode].push(mb);
      console.log(`   ${mode.padEnd(5)} round ${round + 1}: ArrayBuffers ${ab.toFixed(1)} MB, JS heap ${heap.toFixed(1)} MB, renderer RSS ${mb.toFixed(1)} MB (noisy); range reads ${rr}`);
    }
  }
  const med = (a) => a.slice().sort((x, y) => x - y)[Math.floor(a.length / 2)];
  console.log(`   median ArrayBuffers: whole ${med(res.wholeAb).toFixed(1)} MB, range ${med(res.rangeAb).toFixed(1)} MB, saved ${(med(res.wholeAb) - med(res.rangeAb)).toFixed(1)} MB`);
  ok(med(res.wholeAb) - med(res.rangeAb) > 25, 'the range path holds at least 25 MB less in ArrayBuffers than the whole read (basic+relief+runways = 30.5 MB)');
}

try {
  await serverServesPack();
  if (process.env.MEASURE_MEM) { await measureMemory(); throw 'done'; }
  for (const phase of ['no detail pack', 'detail pack', 'app path', 'app range path', 'app range path + detail']) {
    console.log(`── the server map, ${phase} ──`);
    if (phase === 'detail pack' || phase === 'app range path + detail') {
      if (!DETAIL || !fs.existsSync(DETAIL)) { console.log('   (skipped — set VIBE_DETAIL_PACK to the 250,319,197-byte mapgl-detail-v4 pack)'); continue; }
      try { fs.symlinkSync(DETAIL, detailLink); } catch {}
    }
    if (phase === 'app path' || phase === 'app range path') { try { fs.unlinkSync(detailLink); } catch {} }
    await call('Page.navigate', { url: `${H}/mapgl/none-${Date.now()}` });
    await sleep(800);
    const app = phase === 'app path' ? 'whole' : phase.startsWith('app range') ? 'range' : false;
    const opened = await js(OPEN(app, phase === 'app range path + detail'));
    if (opened !== true || !(await until(`!!window.VM && VM.map.loaded()`, 30000))) { ok(false, `${phase}: the GPU map came up (${opened})`); continue; }
    const want = phase === 'detail pack' || phase === 'app range path + detail';
    if (app === 'range') {
      const srcs = await js(`JSON.stringify(['basic', 'relief', 'runways', 'detail'].map((k) => [k, (VM.map.getStyle().sources[k] || {}).url || null]))`);
      ok(/\["basic","pmtiles:\/\/basic"\]/.test(srcs) && /\["relief","pmtiles:\/\/relief"\]/.test(srcs), `${phase}: packs read through packRange (${srcs})`);
    }
    const st = await js(`JSON.stringify({ d: VM_DETAIL, z5: !!VM.map.getSource('vs-basic-z5'), lakes: VM.map.getLayer('lakes-b') ? VM.map.getLayer('lakes-b').source : null,
      riv: VM.map.getLayer('riverareas-b') ? VM.map.getLayer('riverareas-b').source : null, rivMax: VM.map.getLayer('riverareas-b') ? VM.map.getLayer('riverareas-b').maxzoom : null,
      rivSrcMax: VM.map.getSource('vs-basic-rivers') ? VM.map.getSource('vs-basic-rivers').maxzoom : null })`);
    const S = JSON.parse(st);
    ok(S.d === want, `the server says detail pack ${S.d ? 'installed' : 'not installed'}`);
    ok(want ? (!S.z5 && S.lakes === 'basic') : (S.z5 && S.lakes === 'vs-basic-z5'),
       want ? `with it, no vs-basic-z5 source (lakes-b reads ${S.lakes})` : `without it, lakes-b reads ${S.lakes} (capped at z5)`);
    ok(S.riv === 'vs-basic-rivers' && S.rivSrcMax === 8, `riverareas-b reads ${S.riv} (source maxzoom ${S.rivSrcMax})`);
    ok(want ? S.rivMax === 8 : S.rivMax === 24 || S.rivMax == null,
       want ? `with it, riverareas-b stops at z${S.rivMax} where riverareas-d starts` : `without it, riverareas-b over-zooms (maxzoom ${S.rivMax})`);
    // ★ labels off (a country name sits on the Markermeer at z5): this tests the ground, not the lettering
    await js(`VM.map.getStyle().layers.filter(l => l.type === 'symbol').forEach(l => VM.map.setLayoutProperty(l.id, 'visibility', 'none')), true`);
    for (const z of ZOOMS) {
      for (const [pn, lat, lon, wantKind, zs] of POINTS) {
        if (!zs.includes(z)) continue;
        await js(`VM.map.jumpTo({ center: [${lon}, ${lat}], zoom: ${z} }), true`);
        await sleep(200);
        await until(`VM.map.loaded() && VM.map.areTilesLoaded() && !VM.map.isMoving()`, 20000);
        await js(`new Promise((r) => { VM.map.once('idle', () => r(true)); VM.map.triggerRepaint(); setTimeout(() => r(false), 8000); })`);
        const pt = await js(`(() => { const p = VM.map.project([${lon}, ${lat}]); const r = VM.map.getCanvas().getBoundingClientRect(); return [p.x + r.left, p.y + r.top]; })()`);
        const s = await call('Page.captureScreenshot', { format: 'png', clip: { x: Math.round(pt[0]), y: Math.round(pt[1]), width: 1, height: 1, scale: 1 } });
        const [r, g, b] = pngPixel(Buffer.from(s.result.data, 'base64'));
        const is = b > g + 12 ? 'water' : 'land';
        const fills = await js(`VM.map.queryRenderedFeatures(VM.map.project([${lon}, ${lat}])).filter(f => f.layer.type === 'fill').map(f => f.layer.id).slice(0, 4).join('>')`);
        ok(is === wantKind, `${phase} z${z} ${pn}: ${is} (rgb ${r},${g},${b}; top fills ${fills || 'none'})`);
      }
      for (const [name, lon, lat, zz] of [['nl', 5.4, 52.75, 7], ['rotterdam', 4.35, 51.92, 9], ['london', -0.08, 51.505, 9]]) {
        if (!SHOTS || z !== zz) continue;
        await js(`VM.map.jumpTo({ center: [${lon}, ${lat}], zoom: ${zz} }), true`);
        await js(`new Promise((r) => { VM.map.once('idle', () => r(true)); VM.map.triggerRepaint(); setTimeout(() => r(false), 8000); })`);
        const s = await call('Page.captureScreenshot', { format: 'png' });
        fs.writeFileSync(path.join(SHOTS, `server-map-${phase.replace(/ /g, '-')}-${name}-z${zz}.png`), Buffer.from(s.result.data, 'base64'));
      }
    }
    if (app === 'range') console.log(`   range reads over the "bridge": ${await js(`JSON.stringify({ n: window.__rangeReads || 0, bytes: window.__rangeBytes || 0 })`)}`);
  }
} catch (e) { if (e !== 'done') throw e;
} finally {
  console.log(`\n${pass} passed, ${fail} failed`);
  try { ws.close(); } catch {}
  process.exit(fail ? 1 : 0);
}
