// test-map-water-nl.mjs — THE NETHERLANDS' WATER on the directory's GPU maps, in a real (headless, MUTED) Edge.
//   ★★ Two faults, opposite ways round, on the same few square miles:
//      11-18  Flevoland, Lelystad and Almere drawn as WATER (lake islands lost — map_lake_islands_fix).
//      11-26  the IJsselmeer, the Markermeer and the Wadden Sea drawn as LAND (Stuart's community-map screenshot, z≈7).
//   So this samples the rendered PIXEL at fixed points, on BOTH the community map and the directory map, at z5–10:
//      water: IJsselmeer 52.75N 5.35E · Markermeer 52.55N 5.20E · Wadden Sea 53.20N 5.30E
//      land:  Lelystad 52.50N 5.47E · Emmeloord 52.71N 5.75E
//   A pixel is WATER when blue clearly beats green (lakes #1b4a6b, sea #123049) and LAND otherwise (land #3c5a3f,
//   urban #5a5d62, wetland #33523f) — robust to the greyline's shading. No servers are listed (empty API answers), so
//   no marker sits on a sample point.
//   node directory/scripts/test-map-water-nl.mjs [screenshotDir]      (exit 3 = not run: no Edge)
import http from 'node:http';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import zlib from 'node:zlib';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

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
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'mapwater-'));
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGKILL'); } catch {} } try { fs.rmSync(T, { recursive: true, force: true }); } catch {} });
setTimeout(() => { console.log('   FAIL deadline (240 s)'); process.exit(1); }, 240000).unref();
const listen = (srv) => new Promise((r) => srv.listen(0, '127.0.0.1', () => r(srv.address().port)));

// ── the directory stand-in: public/ as the Worker serves it (Range for the packs); every API answers empty ──
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.png': 'image/png' };
const dirSrv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  if (u.pathname.startsWith('/api/')) {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(u.pathname === '/api/community' ? '{"servers":[]}' : u.pathname === '/api/directory' ? '{"servers":[]}' : '{"rows":[]}');
    return;
  }
  let p = decodeURIComponent(u.pathname);
  if (p === '/') p = '/index.html';
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

// ── a 1-pixel PNG reader (Page.captureScreenshot with a 1×1 clip) ──
function pngPixel(buf) {
  let o = 8; const idat = []; let ct = 6;
  while (o < buf.length) {
    const len = buf.readUInt32BE(o), type = buf.toString('ascii', o + 4, o + 8);
    if (type === 'IHDR') ct = buf[o + 8 + 9];
    if (type === 'IDAT') idat.push(buf.subarray(o + 8, o + 8 + len));
    o += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));   // one row: filter byte + RGB(A); filter 0/1/2 all leave a 1-px row's first pixel as is (no left/up)
  return [raw[1], raw[2], raw[3]];
}

// ── the browser ──
const cdpPort = await new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
procs.push(spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--no-default-browser-check',
  '--disable-extensions', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist',
  `--remote-debugging-port=${cdpPort}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1280,800', 'about:blank'], { stdio: 'ignore' }));
let target;
for (let i = 0; i < 60 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdpPort}/json`)).json()).find((t) => t.type === 'page'); } catch {} if (!target) await sleep(250); }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((r) => (ws.onopen = r));
let id = 0; const pending = new Map();
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
const until = async (expr, ms = 20000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(200); } return false; };
await call('Runtime.enable'); await call('Page.enable');

const POINTS = [
  ['IJsselmeer', 52.75, 5.35, 'water'], ['Markermeer', 52.55, 5.20, 'water'], ['Wadden Sea', 53.20, 5.30, 'water'],
  ['Lelystad', 52.50, 5.47, 'land'], ['Emmeloord', 52.71, 5.75, 'land'],
];
const ZOOMS = [5, 6, 7, 8, 9, 10];
const PAGES = [['community map', '/community-map'], ['directory map', '/']];

try {
  for (const [name, url] of PAGES) {
    console.log(`── ${name} (${url}) ──`);
    await call('Page.navigate', { url: `http://127.0.0.1:${DP}${url}` });
    if (!(await until(`typeof GLVM !== 'undefined' && GLVM && GLVM.map && GLVM.map.loaded() && !!GLVM.map.getLayer('vs-water')`, 30000))) {
      ok(false, `${name}: the GPU map and its water overlay came up`); continue;
    }
    // ★ labels off (a country name sits on the Markermeer at z5): this tests the ground, not the lettering
    await js(`GLVM.map.getStyle().layers.filter(l => l.type === 'symbol').forEach(l => GLVM.map.setLayoutProperty(l.id, 'visibility', 'none')), true`);
    for (const z of ZOOMS) {
      const got = [];
      for (const [pn, lat, lon, want] of POINTS) {
        // ★ each point at the centre of the view (at z9–10 they are further apart than the screen)
        await js(`GLVM.map.jumpTo({ center: [${lon}, ${lat}], zoom: ${z} }), true`);
        await sleep(200);
        await until(`GLVM.map.loaded() && GLVM.map.areTilesLoaded() && !GLVM.map.isMoving()`, 20000);
        await js(`new Promise((r) => { GLVM.map.once('idle', () => r(true)); GLVM.map.triggerRepaint(); setTimeout(() => r(false), 8000); })`);
        // ★ the point on screen; any page chrome over it (the empty-list notice, a control) is hidden, so the pixel is the map's
        const pt = await js(`(() => { const p = GLVM.map.project([${lon}, ${lat}]); const c = GLVM.map.getCanvas(), r = c.getBoundingClientRect();
          const x = p.x + r.left, y = p.y + r.top;
          for (let i = 0, e; i < 20 && (e = document.elementFromPoint(x, y)) && e !== c; i++) e.style.visibility = 'hidden';
          return [x, y]; })()`);
        await sleep(50);
        const s = await call('Page.captureScreenshot', { format: 'png', clip: { x: Math.round(pt[0]), y: Math.round(pt[1]), width: 1, height: 1, scale: 1 } });
        const [r, g, b] = pngPixel(Buffer.from(s.result.data, 'base64'));
        const is = b > g + 12 ? 'water' : 'land';
        const layers = await js(`GLVM.map.queryRenderedFeatures(GLVM.map.project([${lon}, ${lat}])).filter(f => f.layer.type === 'fill').map(f => f.layer.id).slice(0, 4).join('>')`);
        got.push(is === want);
        ok(is === want, `${name} z${z} ${pn}: ${is} (rgb ${r},${g},${b}; top fills ${layers || 'none'})`);
      }
      if (SHOTS && (got.includes(false) || z === 7)) {   // z7 always: the zoom of Stuart's screenshot
        await js(`GLVM.map.jumpTo({ center: [5.4, 52.75], zoom: ${z} }), true`);
        await js(`new Promise((r) => { GLVM.map.once('idle', () => r(true)); GLVM.map.triggerRepaint(); setTimeout(() => r(false), 8000); })`);
        const s = await call('Page.captureScreenshot', { format: 'png' });
        fs.writeFileSync(path.join(SHOTS, `${name.replace(/ /g, '-')}-z${z}.png`), Buffer.from(s.result.data, 'base64'));
      }
    }
  }
} finally {
  console.log(`\n${pass} passed, ${fail} failed`);
  ws.close();
  process.exit(fail ? 1 : 0);
}
