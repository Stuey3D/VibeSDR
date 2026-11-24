// test-web-lrpt-outline.mjs — ★★ V12 METEOR LRPT's MAP OUTLINE in the GALLERY, through a real vibeserver and the REAL built
// web page (docs/v12/LRPT-MAP-OVERLAY.md). The gallery holds what vibe_bgdecode.h's LrptGalleryTap writes for two passes:
//   A — a picture with its outline item ("geo"), B — a picture whose pass failed the checks ("gw"), and C — a picture
//   from before the feature (neither). The pictures and the outline are SYNTHETIC (made here), unless LRPT_REAL=<prefix>
//   names a tool-lrpt-geo output (<prefix>-plain.png + <prefix>-geo.json) — then pass A is that real pass.
//   · the server: the outline item is served; ?outline=1 = a burnt-in copy (same size, different pixels, "… map outline.png");
//     a picture with no outline is served plain with ?outline=1;
//   · the page: the outline item is never a row; A: MAP OUTLINE: OFF by default, no SVG; ON = the SVG over the picture
//     with its paths, DOWNLOAD = ?outline=1; the choice is remembered (reload); B: no key, "No map outline: …"; C: neither;
//     390 px: no sideways scroll.
// ★ SILENT: headless Edge with --mute-audio, the fake dongle. usage: VIBESERVER_BIN=… node scripts/test-web-lrpt-outline.mjs
//   Screenshots to $SAT_SHOTS (default $TMPDIR).
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const SHOTS = process.env.SAT_SHOTS || os.tmpdir();
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }

// ── a PNG (RGB, filter None) and a synthetic outline ──
const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc = (b) => { let c = -1; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([l, td, c]); };
function png(w, h, px) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; px.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3); }
  const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const W = 1568, Hs = 400;
const px = Buffer.alloc(W * Hs * 3);
for (let y = 0; y < Hs; y++) for (let x = 0; x < W; x++) { const v = (x > 700 && x < 1000 && y > 120 && y < 300) ? 170 : 30; px.fill(v, (y * W + x) * 3, (y * W + x) * 3 + 3); }
let picA = png(W, Hs, px), geoA = JSON.stringify({ v: 1, w: W, h: Hs, scale: 1, sat: 'Meteor-M2-4', norad: 59051, start: 0, end: 0,
  corners: [[50, 10], [45, -20], [58, 12], [52, -25]], centre: [54, -3],
  coast: [[700, 120, 1000, 120, 1000, 300, 700, 300, 700, 120]], lake: [], border: [[850, 120, 850, 300]], grid: [[0, 200, 1568, 205]], labels: [[2, 200, '55°N']] });
if (process.env.LRPT_REAL) { picA = fs.readFileSync(`${process.env.LRPT_REAL}-plain.png`); geoA = fs.readFileSync(`${process.env.LRPT_REAL}-geo.json`, 'utf8'); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-lrptgal-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'METEOR outline test', adminPass: 'lo-admin',
  radios: [{ serial: 'lot', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000 }] }));
const gdir = path.join(dir, 'data', 'gallery');
fs.mkdirSync(gdir);
const t0 = Math.floor(Date.now() / 1000) - 3600;
const base = { o: 'live', hz: 137900000, m: 'iq', d: 'lrpt', p: '', r: 'Test radio' };
const geoItem = { f: '20261123-140645-lrptgeo-137900k0-0a01.json', k: 'lrptgeo', t: t0, ...base, title: 'METEOR LRPT · map outline' };
const A = { f: '20261123-140645-lrpt-137900k0-0a02.png', k: 'lrpt', t: t0, ...base, title: 'METEOR LRPT · RGB 221 false colour · 1568×400', geo: geoItem.f };
const B = { f: '20261123-150000-lrpt-137900k0-0a03.png', k: 'lrpt', t: t0 + 600, ...base, title: 'METEOR LRPT · channel 1 visible · 1568×400', gw: "the satellite's clock reads 3 h 0 min ahead of ours" };
const C = { f: '20261122-120000-lrpt-137900k0-0a04.png', k: 'lrpt', t: t0 - 86400, ...base, title: 'METEOR LRPT · channel 2 near-IR · 1568×400' };
fs.writeFileSync(path.join(gdir, geoItem.f), geoA);
fs.writeFileSync(path.join(gdir, A.f), picA);
fs.writeFileSync(path.join(gdir, B.f), png(W, Hs, px));
fs.writeFileSync(path.join(gdir, C.f), png(W, Hs, px));
fs.writeFileSync(path.join(gdir, 'index.jsonl'), [C, geoItem, A, B].map((x) => JSON.stringify(x)).join('\n') + '\n');

const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-lrptgal-edge-'));
let edge = null;
const deadline = setTimeout(() => { console.log('   FAIL deadline (120 s)'); try { edge?.kill('SIGKILL'); srv.kill('SIGKILL'); rtl.kill('SIGKILL'); } catch {} process.exit(1); }, 120000);
try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  console.log('── the server: the outline item, and a burnt-in copy on request ──');
  const g = await fetch(`${H}/vibeserver/gallery/file/${geoItem.f}`);
  ok(g.ok && (await g.text()) === geoA, 'the outline item is served as written');
  const plain = Buffer.from(await (await fetch(`${H}/vibeserver/gallery/file/${A.f}`)).arrayBuffer());
  ok(plain.equals(picA), 'the picture itself is the plain PNG, byte for byte');
  const rb = await fetch(`${H}/vibeserver/gallery/file/${A.f}?download=1&outline=1`);
  const burnt = Buffer.from(await rb.arrayBuffer());
  ok(rb.ok && burnt.subarray(1, 4).toString() === 'PNG' && !burnt.equals(picA) && burnt.readUInt32BE(16) === picA.readUInt32BE(16) && burnt.readUInt32BE(20) === picA.readUInt32BE(20),
     `?outline=1: a different PNG of the same size (${burnt.length} bytes)`);
  ok(/map outline\.png/.test(rb.headers.get('content-disposition') || ''), `its download name (${rb.headers.get('content-disposition')})`);
  fs.writeFileSync(path.join(SHOTS, 'lrpt-outline-burnt.png'), burnt);
  const rc = Buffer.from(await (await fetch(`${H}/vibeserver/gallery/file/${C.f}?outline=1`)).arrayBuffer());
  ok(rc.equals(fs.readFileSync(path.join(gdir, C.f))), 'a picture with no outline is served plain with ?outline=1');

  console.log('── the web page: the switch, OFF by default ──');
  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1100,800', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((x) => x.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await new Promise((res) => { ws.onopen = res; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, `lrpt-outline-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms = 6000) => { for (let t = 0; t < ms; t += 150) { if (await ev(x)) return true; await sleep(150); } return false; };
  const openGallery = async () => {
    await send('Page.navigate', { url: `${H}/index.html?join` });
    await sleep(4000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
    for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn').onclick`)); i++) await sleep(250);
    await ev(`(() => { for (const id of ['splash', 'audioGate']) { const s = document.getElementById(id); if (s) s.style.display = 'none'; } return true; })()`);
    await ev(`document.getElementById('galleryBtn').click(); true`);
    await until(`document.querySelectorAll('#galList .sres[role=button]').length >= 3`);
  };
  const openRow = async (re) => { await ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => ${re}.test(r.textContent)).click(); true`); };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1100, height: 800, deviceScaleFactor: 1, mobile: false });
  await openGallery();
  ok(await ev(`[...document.querySelectorAll('#galList .sres[role=button]')].length === 3 && !/map outline/.test(document.getElementById('galList').textContent)`),
     'the outline item is not a row (3 pictures listed)');
  await openRow('/RGB 221/');
  ok(await until(`!document.getElementById('galOutline').hidden`), 'A: the MAP OUTLINE key appears');
  const off = JSON.parse(await ev(`JSON.stringify({ t: document.getElementById('galOutline').textContent, p: document.getElementById('galOutline').getAttribute('aria-pressed'),
    svg: document.getElementById('galGeo').hasAttribute('hidden'), dl: document.getElementById('galDl').getAttribute('href') })`));
  ok(off.t === 'MAP OUTLINE: OFF' && off.p === 'false' && off.svg === true, `OFF by default, no outline drawn (${off.t})`);
  ok(!/outline=1/.test(off.dl), 'DOWNLOAD (OFF) = the plain picture');
  await shot('off');
  await ev(`document.getElementById('galOutline').click(); true`);
  ok(await until(`!document.getElementById('galGeo').hasAttribute('hidden') && document.querySelectorAll('#galGeo path').length >= 2`), 'ON: the outline is drawn over the picture');
  const on = JSON.parse(await ev(`JSON.stringify({ t: document.getElementById('galOutline').textContent, dl: document.getElementById('galDl').getAttribute('href'),
    vb: document.getElementById('galGeo').getAttribute('viewBox'), ir: document.getElementById('galImg').getBoundingClientRect().toJSON(), sr: document.getElementById('galGeo').getBoundingClientRect().toJSON(),
    lab: document.querySelector('#galGeo text')?.textContent || '' })`));
  ok(on.t === 'MAP OUTLINE: ON' && /outline=1/.test(on.dl), `the key says ON; DOWNLOAD = the burnt-in copy (${on.dl})`);
  ok(Math.abs(on.ir.x - on.sr.x) < 1 && Math.abs(on.ir.y - on.sr.y) < 1 && Math.abs(on.ir.width - on.sr.width) < 1 && Math.abs(on.ir.height - on.sr.height) < 1,
     `the SVG lies exactly on the picture (${Math.round(on.sr.width)}×${Math.round(on.sr.height)})`);
  ok(/^0 0 1568 /.test(on.vb) && /°/.test(on.lab), `viewBox = the picture's pixels (${on.vb}), a grid label (${on.lab})`);
  await shot('on');
  await ev(`document.getElementById('galBack').click(); true`);
  await until(`document.getElementById('galView').hidden`);
  await openRow('/channel 1 visible/');
  ok(await until(`!document.getElementById('galOutlineWhy').hidden`), 'B: the reason line');
  ok(await ev(`document.getElementById('galOutline').hidden && document.getElementById('galOutlineWhy').textContent === "No map outline: the satellite's clock reads 3 h 0 min ahead of ours"`), 'B: no key, the reason in words');
  await ev(`document.getElementById('galBack').click(); true`);
  await until(`document.getElementById('galView').hidden`);
  await openRow('/channel 2 near-IR/');
  await sleep(800);
  ok(await ev(`document.getElementById('galOutline').hidden && document.getElementById('galOutlineWhy').hidden && !/outline=1/.test(document.getElementById('galDl').getAttribute('href'))`), 'C (before the feature): plain, nothing said');
  console.log('── remembered, and on a phone ──');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await openGallery();
  await openRow('/RGB 221/');
  ok(await until(`document.getElementById('galOutline').textContent === 'MAP OUTLINE: ON' && !document.getElementById('galGeo').hasAttribute('hidden')`), 'after a reload the switch is still ON (this browser)');
  await sleep(500);
  const fit = JSON.parse(await ev(`JSON.stringify({ sw: document.documentElement.scrollWidth, w: innerWidth })`));
  ok(fit.sw <= fit.w + 1, `390 px: no sideways scroll (${JSON.stringify(fit)})`);
  await shot('phone');
  ok(errs.length === 0, `no exception in the page${errs.length ? ': ' + errs[0] : ''}`);
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  clearTimeout(deadline);
  try { edge && edge.kill(); } catch {} try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {}
  await sleep(600);
  try { fs.rmSync(prof, { recursive: true, force: true }); } catch {}
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed · screenshots: ${SHOTS}/lrpt-outline-*.png`);
process.exit(fail ? 1 : 0);
