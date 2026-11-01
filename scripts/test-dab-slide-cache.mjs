// ★★★ AN OFF-AIR PICTURE LOADS ONCE, THEN COMES FROM CACHE (2026-10-19). Stuart: "light everywhere … light on data".
// /vibeserver/dabslide sent the live slide `no-cache` with nothing to revalidate against and `?sid=` re-sent in full every
// 60 s; the web station list is rebuilt twice a second; the app asked for `?seq=`, which the server ignored and which
// restarts from 0 with the server. Now every off-air picture carries a strong ETag (FNV-1a of the bytes) and a matching
// If-None-Match gets a 304; `?h=<that hash>` is a URL unique to the picture, sent immutable.
//   1. ?sid= → 200 with an ETag; the same request with If-None-Match → 304, no body;
//   2. a DIFFERENT picture (another service) → 200, its own ETag, even when sent the first one's tag;
//   3. ?h=<the ETag> → 200 immutable; an unknown ?h= is never answered immutable;
//   4. dablogoair → 304 on revalidation too;
//   5. the legacy report carries no `h` (byte-for-byte rule) and src/services/dabSlide.ts builds ?h= only from a hash;
//   6. in a REAL (headless, MUTED) browser on the served page, the station list — rebuilt twice a second — fetches the
//      picture ONCE (the page's keep-a-copy fetch beside the <img> is now a 304); after a reload it is a 304, straight
//      from the cache, or not asked for at all — never the bytes again.
// ★ Real server + the DAB bench clip; the pictures are seeded into the server's slide store (data/dab-slides), because
//   the bench multiplex signals slideshows but carries none. Without the clip or VIBESERVER_BIN: NOT RUN (exit 3); the
//   browser part needs Microsoft Edge (skipped, said so, when absent). Loopback only — every other URL is blocked.
import * as esbuild from 'esbuild';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = path.join(root, 'vibeserver/bench-clip/dab-bench-clean.vbu8');
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN)'); process.exit(3); }
if (!fs.existsSync(CLIP)) { console.log('   (not run — no bench clip; vibeserver/bench-clip/make-clip.sh builds it)'); process.exit(3); }

// A tiny valid PNG of one colour — two different pictures.
function png(r, g, b) {
  const crc = (buf) => { let c = ~0; for (const x of buf) { c ^= x; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); } return ~c >>> 0; };
  const chunk = (t, d) => { const len = Buffer.alloc(4); len.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]);
    const c = Buffer.alloc(4); c.writeUInt32BE(crc(td)); return Buffer.concat([len, td, c]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(8, 0); ihdr.writeUInt32BE(8, 4); ihdr[8] = 8; ihdr[9] = 2;
  const rows = Buffer.concat(Array.from({ length: 8 }, () => Buffer.from([0, ...Array.from({ length: 8 }, () => [r, g, b]).flat()])));
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(rows)), chunk('IEND', Buffer.alloc(0))]);
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-slide-'));
for (const d of ['data', 'run', 'data/dab-slides']) fs.mkdirSync(path.join(dir, d));
// The bench ensemble: ECC 0xE1, EId 0x4FFE; services 49153 (0xC001) and 49154 (0xC002).
fs.writeFileSync(path.join(dir, 'data/dab-slides/e1-4ffe-0000c001.png'), png(200, 30, 30));
fs.writeFileSync(path.join(dir, 'data/dab-slides/e1-4ffe-0000c002.png'), png(30, 30, 200));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Slide cache', locator: 'IO92nh',
  adminPass: 'slide-admin', radios: [{ serial: 'slide', label: 'T', enabled: true, configured: true, users: 3 }] }));
const raw = path.join(dir, 'clip.u8');
fs.writeFileSync(raw, fs.readFileSync(CLIP).subarray(28));
const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn('nice', ['-n', '10', process.execPath, path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000', '--dab', raw], { stdio: 'ignore' });
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000',
  '--mode', 'wfm', '--users', '3'], { env, stdio: ['ignore', log, log] });
let br = null;
const base = `http://127.0.0.1:${port}`;
const get = async (p, headers = {}) => { const r = await fetch(base + p, { headers, cache: 'no-store' }); const b = Buffer.from(await r.arrayBuffer());
  return { status: r.status, etag: r.headers.get('etag'), cc: r.headers.get('cache-control') || '', len: b.length, body: b }; };

try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const s = net.connect(port, '127.0.0.1'); s.on('connect', () => { s.end(); r(true); }); s.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  // Put the radio on the bench multiplex and wait until the list says it holds the pictures.
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=sl${Date.now()}&bins=256&proto=1`);
  let legacy = null;
  ws.onmessage = (e) => { if (typeof e.data === 'string' && e.data.startsWith('{"type":"dab"')) legacy = e.data; };
  await new Promise((r) => { ws.onopen = r; });
  ws.send(JSON.stringify({ type: 'dab', on: 1, channel: 34 }));   // 12D
  let listed = false;
  for (let i = 0; i < 250 && !listed; i++) { await sleep(100); listed = !!legacy && /"sid":49153,"logoAir":(true|false),"logoSlide":true/.test(legacy); }
  ok(listed, 'the bench multiplex locks and its list says the seeded picture is held (logoSlide)');

  console.log('── ETag and 304 ──');
  const a1 = await get('/vibeserver/dabslide?sid=49153');
  ok(a1.status === 200 && /^"[0-9a-f]{16}"$/.test(a1.etag ?? '') && a1.len > 50, `?sid= → 200, ${a1.len} B, ETag ${a1.etag}`);
  const a2 = await get('/vibeserver/dabslide?sid=49153', { 'If-None-Match': a1.etag });
  ok(a2.status === 304 && a2.len === 0 && a2.etag === a1.etag, `…asked again with If-None-Match → ${a2.status}, ${a2.len} B`);
  const b1 = await get('/vibeserver/dabslide?sid=49154', { 'If-None-Match': a1.etag });
  ok(b1.status === 200 && b1.etag && b1.etag !== a1.etag && b1.len > 50, `a different picture is fetched, even sent the first one's tag (${b1.etag})`);
  const h = (a1.etag ?? '').replace(/"/g, '');
  const c1 = await get(`/vibeserver/dabslide?h=${h}`);
  ok(c1.status === 200 && c1.etag === a1.etag && /immutable/.test(c1.cc), `?h=<its hash> → 200, Cache-Control: ${c1.cc}`);
  const c2 = await get('/vibeserver/dabslide?h=0123456789abcdef');
  ok(!/immutable/.test(c2.cc), `an unknown ?h= is never answered immutable (${c2.status})`);
  const c3 = await get('/vibeserver/dabslide?sid=49153');
  ok(!/immutable/.test(c3.cc) && /max-age=60/.test(c3.cc), `?sid= keeps its own policy, now revalidated (${c3.cc})`);

  console.log('── the wire ──');
  ok(legacy && !/"slide":\{[^}]*"h":/.test(legacy), 'the legacy report carries no `h` (unchanged)');
  const built = await esbuild.build({ entryPoints: [path.join(root, 'src/services/dabSlide.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral', logLevel: 'silent' });
  const { dabSlideQuery } = await import('data:text/javascript;base64,' + Buffer.from(built.outputFiles[0].text).toString('base64'));
  ok(dabSlideQuery({ seq: 3, h: 'a1b2c3d4e5f60718' }) === 'h=a1b2c3d4e5f60718' && dabSlideQuery({ seq: 3 }) === 'seq=3'
     && dabSlideQuery({ seq: 3, h: 'x"><img' }) === 'seq=3' && dabSlideQuery({ seq: 0, h: 'a1b2c3d4e5f60718' }) === '' && dabSlideQuery(undefined) === '',
     'dabSlideQuery: ?h= from a hash, ?seq= from an older server, nothing unsafe in the URL');

  console.log('── a real browser on the served page ──');
  if (!fs.existsSync(EDGE)) { console.log('   (browser part skipped — no Microsoft Edge)'); }
  else {
    br = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--disable-extensions',
      `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(dir, 'browser')}`, '--window-size=1200,900', 'about:blank'], { stdio: 'ignore' });
    let target;
    for (let i = 0; i < 60 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch { await sleep(250); } }
    const cws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((r) => (cws.onopen = r));
    let id = 0; const pending = new Map(); const reqs = new Map(); const seen = [];
    cws.onmessage = (ev) => { const m = JSON.parse(ev.data);
      if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
      if (m.method === 'Network.requestWillBeSent' && m.params.request.url.includes('/vibeserver/dabslide')) reqs.set(m.params.requestId, { url: m.params.request.url, status: 0, cache: false });
      if (m.method === 'Network.requestServedFromCache' && reqs.has(m.params.requestId)) reqs.get(m.params.requestId).cache = true;
      if (m.method === 'Network.responseReceived' && reqs.has(m.params.requestId)) { const r = reqs.get(m.params.requestId); r.status = m.params.response.status; r.cache ||= !!m.params.response.fromDiskCache || !!m.params.response.fromMemoryCache; seen.push(r); } };
    const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); });
    const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
    await call('Network.enable');
    // ★ Nothing leaves the machine: the name-search fallback for logos would go to the internet.
    await call('Network.setBlockedURLs', { urls: ['https://*'] });   // the page itself is plain http on 127.0.0.1
    await call('Page.enable');
    const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(150); } return false; };
    const tap = async (x, y) => { for (const type of ['mousePressed', 'mouseReleased']) await call('Input.dispatchMouseEvent', { type, x, y, button: 'left', clickCount: 1 }); };
    // CONNECT, then START RADIO with a real tap (audio wants a gesture) — as test-web-gamepad-dab.mjs does.
    const connect = async () => {
      await until(`!!document.getElementById("btnConnect")`, 20000);
      await js(`document.getElementById("btnConnect").click(); 1`);
      await until(`!!document.getElementById("audioGate")`, 20000);
      for (let i = 0; i < 6; i++) {
        const at = await js(`(() => { const b = document.querySelector("#audioGate button"); if (!b) return null;
          const r = b.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; })()`);
        if (!at) break;
        await tap(at[0], at[1]);
        if (await until(`!document.getElementById("audioGate")`, 3000)) break;
      }
    };
    await call('Page.navigate', { url: `${base}/` });
    await connect();
    // The page joins the multiplex the radio is already on and draws the list; the seeded row falls back to its slide.
    // ★ 40e0e52a1 (the persistent logo cache): once the keep-a-copy fetch lands, the row draws the SAME picture from the
    //   data: copy it filed (logoStoreWeb `show`) — within moments of the first draw. Either is this station's own picture.
    const seeded = (await get('/vibeserver/dabslide?sid=49153')).body.toString('base64');
    let drawn = false;
    for (let i = 0; i < 200 && !drawn; i++) { await sleep(150); drawn = !!(await js(`[...document.querySelectorAll('img.dabLogo')].some((i) => {
      const s = i.getAttribute('src') || ''; return s.includes('dabslide?sid=49153') || (s.startsWith('data:image/') && s.endsWith(',${seeded}')); })`)); }
    if (!drawn) console.log('      (page: ' + await js(`JSON.stringify({ gate: !!document.getElementById('audioGate'), imgs: [...document.querySelectorAll('img.dabLogo')].map((i) => (i.getAttribute('src') || '').slice(0, 60)).slice(0, 8), rows: document.querySelectorAll('.dabRow, .dabSvc, [data-sid]').length, dabOn: document.body.className })`) + ')');
    ok(drawn, 'the station list draws the seeded row from its off-air picture');
    const mark = seen.length;
    await sleep(10000);
    const net1 = seen.filter((r) => r.url.includes('sid=49153'));
    const fetched = net1.filter((r) => r.status === 200 && !r.cache).length;
    ok(fetched === 1, `the list rebuilding twice a second for ten seconds: ${fetched} full fetch of the picture, then ${net1.slice(1).map((r) => r.cache ? 'cache' : r.status).join(', ') || 'nothing'}`);
    ok(seen.slice(mark).filter((r) => r.url.includes('sid=49153') && r.status === 200 && !r.cache).length === 0, '…and none after the first draw');
    const mark2 = seen.length;
    await call('Page.reload', {});
    await connect();
    let back = false;
    // ★ The row may now draw from the copy the page kept (dabKeepAirLogo → a data: URL in localStorage) — no request at all.
    for (let i = 0; i < 200 && !back; i++) { await sleep(150); back = !!(await js(`document.querySelectorAll('#dabStations .dabSvc').length >= 5`)); }
    await sleep(2000);
    const after = seen.slice(mark2).filter((r) => r.url.includes('sid=49153'));
    ok(back && after.every((r) => r.status === 304 || r.cache), `after a reload: ${after.map((r) => r.cache ? 'cache' : r.status).join(', ') || 'no request at all'} — never the bytes again`);
    cws.close();
  }
  ws.close();
} finally {
  for (const p of [br, srv, rtl]) { try { p?.kill('SIGTERM'); } catch {} }
  await sleep(300);
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`test-dab-slide-cache: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
