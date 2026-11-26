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
//   WebView), no detail pack — the second source shares the one in-memory 'basic' pack.
//   The server's /mapgl/ is THIS checkout's directory/public/mapgl + web/mapkit style (as CMake installs them), so a
//   prebuilt vibeserver is enough: the map files are read off disk, not compiled in.
//   VIBESERVER_BIN=… [VIBE_DETAIL_PACK=…/vibemap-detail.pmtiles] node scripts/test-server-map-water-nl.mjs [screenshotDir]
//   (exit 3 = not run)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawn } from 'node:child_process';
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
const OPEN = (app) => `(async () => {
  document.documentElement.innerHTML = '<head></head><body style="margin:0"><div id="m" style="position:fixed;inset:0"></div></body>';
  const base = location.origin + '/mapgl/';
  for (const f of ['maplibre-gl.js', 'pmtiles.js', 'vibemapgl.js']) {
    await new Promise((res, rej) => { const s = document.createElement('script'); s.src = base + 'vendor/' + f; s.onload = res; s.onerror = () => rej(new Error(f)); document.head.appendChild(s); });
  }
  const style = await (await fetch(base + 'vibemap-style.json', { cache: 'no-cache' })).json();
  const detail = ${app ? 'false' : "(await fetch(base + 'vibemap-detail.pmtiles', { method: 'HEAD', cache: 'no-store' })).status === 200"};
  const load = (p, kind) => fetch(p).then((r) => r.ok ? (kind === 'text' ? r.text() : r.arrayBuffer()) : null).catch(() => null);
  window.VM = await VibeMapGL.create(document.getElementById('m'), { style, load, base, rangeBase: ${app ? 'undefined' : 'base'}, detail, profile: 'directory', center: [5.4, 52.7], zoom: 5 });
  window.VM_DETAIL = detail;
  return true;
})()`;

try {
  for (const phase of ['no detail pack', 'detail pack', 'app path']) {
    console.log(`── the server map, ${phase} ──`);
    if (phase === 'detail pack') {
      if (!DETAIL || !fs.existsSync(DETAIL)) { console.log('   (skipped — set VIBE_DETAIL_PACK to the 250,319,197-byte mapgl-detail-v4 pack)'); continue; }
      fs.symlinkSync(DETAIL, detailLink);
    }
    if (phase === 'app path') { try { fs.unlinkSync(detailLink); } catch {} }
    await call('Page.navigate', { url: `${H}/mapgl/none-${Date.now()}` });
    await sleep(800);
    const opened = await js(OPEN(phase === 'app path'));
    if (opened !== true || !(await until(`!!window.VM && VM.map.loaded()`, 30000))) { ok(false, `${phase}: the GPU map came up (${opened})`); continue; }
    const want = phase === 'detail pack';
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
  }
} finally {
  console.log(`\n${pass} passed, ${fail} failed`);
  try { ws.close(); } catch {}
  process.exit(fail ? 1 : 0);
}
