#!/usr/bin/env node
// test-web-direct-data.mjs — ★★★ beta93: THE PAGE READS THE RECEIVER DIRECTLY, NOT THROUGH THE DIRECTORY'S WORKER.
//
// Stuart, 2026-11-24 (the Workers allowance gone in minutes to one page's DX Log poll, every tunnel down with it): "the
// gallery etc should all be handled via the free tunnel not the main workers — they should only establish the
// connection." So: the page at <slug>.vibeserver.vibesdr.net gets its HTML from the Worker (the stable origin keeps
// localStorage), and EVERY HTTP request after that — scripts, vibeserver.json, gallery, images, telemetry charts, the
// DX Log, logos, map data — goes to the receiver's own tunnel host, exactly like the WebSocket always has.
//
// The set-up, all on this Mac, all real except the two Cloudflare pieces it stands in for:
//   · a vibeserver BUILT FROM THIS TREE (its own page, node scripts/build-web.mjs first), the fake dongle, a seeded
//     gallery (an image, a text item, three FUNcube passes → telemetry charts) and a seeded DX Log;
//   · a "TUNNEL": TLS on rx-test.trycloudflare.com:<T> in front of it, adding X-Forwarded-For as cloudflared does;
//   · a "WORKER": TLS on stuey-test.vibeserver.vibesdr.net:<W> doing what directory/src/index.js serveBySlug does to a
//     page — x-vibesdr-via / x-vibesdr-direct-host out, the metas injected at the top of the head (vibe-direct-assets
//     only when the receiver's CSP names the host), directHost on vibeserver.json — and COUNTING every request;
//   · headless MUTED Edge, its DNS pinned to 127.0.0.1 for those names and NOTHING else (--host-resolver-rules).
// Asserts:
//   A. CORS straight from the receiver: a slug origin echoed (+ Vary: Origin), a stranger with a credential gets no
//      Access-Control-Allow-Origin, the public read keeps its `*`, never `*` with a credential, preflight 204 only for
//      a slug origin; the page's CSP names the tunnel host only for a request the directory proxied.
//   B. ★★★ the page loaded through the Worker, used for real (gallery list, an image, a pass's telemetry charts, the DX
//      Log, the DX dial labels): ZERO requests reach the Worker after the document, bar the browser's own same-origin
//      furniture (service worker, manifest, icons) — counted and listed;
//   C. the tunnel host ROTATES (the server "restarted"): a failed direct read re-learns the host with ONE read through the
//      Worker, the page reloads onto the new host and carries on direct there.
// ★ SILENT: --mute-audio, no START of audio. usage: VIBESERVER_BIN=… node scripts/test-web-direct-data.mjs  (exit 3 = not run)
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import tls from 'node:tls';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — Edge needed)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const procs = [];
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-direct-'));
process.on('exit', () => { for (const p of procs) { try { p.kill('SIGTERM'); } catch {} } try { fs.rmSync(T, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
const deadline = setTimeout(() => { console.log('   FAIL deadline (300 s)'); process.exit(1); }, 300000);

const SLUG = 'stuey-test.vibeserver.vibesdr.net', TUN1 = 'rx-test.trycloudflare.com', TUN2 = 'rx-test2.trycloudflare.com';

// ── a throwaway certificate for the three names (the browser is told to ignore its issuer) ──
execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', path.join(T, 'k.pem'), '-out', path.join(T, 'c.pem'),
  '-days', '1', '-subj', '/CN=vibesdr-direct-test', '-addext', `subjectAltName=DNS:*.vibeserver.vibesdr.net,DNS:${TUN1},DNS:${TUN2}`], { stdio: 'ignore' });
const KEY = fs.readFileSync(path.join(T, 'k.pem')), CERT = fs.readFileSync(path.join(T, 'c.pem'));

// ── the receiver ──
const now = Math.floor(Date.now() / 1000), DAY = 86400;
const dir = path.join(T, 'srv');
for (const d of ['data', 'run', 'data/gallery']) fs.mkdirSync(path.join(dir, d), { recursive: true });
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Direct data test', locator: 'IO92nh', adminPass: 'dd-admin',
  radios: [{ serial: 'dx-test', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2400000,
             dxScan: JSON.stringify({ on: false, bands: 'fm', waitMin: 5, viewAdmin: false }) }] }));
// a week of DX Log, as the server writes it (test-web-dx-dial.mjs's shape)
const fmE = (id, pi, khz, ps, cls, heard) => ({ id, band: 'FM', key: `FM:${pi.toString(16).toUpperCase().padStart(4, '0')}:${khz}`, class: cls, khz, piN: pi, ps,
  psVariants: [ps], firstHeard: now - 3 * DAY, lastHeard: heard, timesHeard: 12, presence: 0.05, lastSweep: 1 });
fs.writeFileSync(path.join(dir, 'data', 'dxlog-dx-test.json'), JSON.stringify({ v: 1, created: now - 30 * DAY, nextId: 4, sweepsFm: 5, sweepsDab: 5,
  alertSeq: 0, fmCursorKhz: 87500, dabCursor: 0, nextDab: false, bootDone: true, alerts: [],
  entries: [fmE(1, 0xC201, 92500, 'BBC R4', 'local', now - 300), fmE(2, 0x5201, 93500, 'RAI R1', 'dx', now - 7200), fmE(3, 0x5202, 98800, 'RNE 1', 'dx', now - 9000)] }));
// the gallery: an image, a text item, three FUNcube passes (their charts) — the fixtures test-web-fctlm-gallery uses
const gdir = path.join(dir, 'data', 'gallery');
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEklEQVR4nGP4z8CAFWEXHbQSACj/P8Fu7N9hAAAAAElFTkSuQmCC', 'base64');   // an 8×8 grey PNG
let index = '';
const add = (it, body) => { fs.writeFileSync(path.join(gdir, it.f), body); index += JSON.stringify(it) + '\n'; };
add({ f: '20261124-101500-wefax-4610k0-0a01.png', k: 'wefax', o: 'background', t: now - 4000, hz: 4610000, m: 'usb', d: 'wefax', p: 'Profile 1', r: 'Test radio', title: 'WEFAX test chart' }, PNG);
add({ f: '20261124-101600-navtex-518k0-0a02.txt', k: 'navtex', o: 'background', t: now - 3900, hz: 518000, m: 'usb', d: 'navtex', p: 'Profile 1', r: 'Test radio', title: 'NAVTEX test message' }, 'ZCZC GA01\nTEST MESSAGE\nNNNN\n');
const FIX = path.join(root, 'scripts/fixtures/fctlm');
const firstUnix = (csv) => Number(csv.split('\n').find((l) => /^\d{4}-/.test(l)).split(',')[1]);
for (const n of [1, 2, 3]) {
  const body = fs.readFileSync(path.join(FIX, `pass-${n}.csv`), 'utf8');
  const t = firstUnix(body), hm = new Date(t * 1000).toISOString().slice(11, 16);
  add({ f: `${new Date(t * 1000).toISOString().replace(/[-:]/g, '').slice(0, 15).replace('T', '-')}-fctlm-145935k0-${(0xb00 + n).toString(16)}.csv`, k: 'fctlm', o: 'background',
        t, hz: 145935000, m: 'usb', d: 'fctlm', p: 'Profile 1', r: 'Test radio', title: `AO-73 telemetry · pass ${hm} UTC · 20 frames · max el 45°`,
        dn: `AO-73 telemetry ${hm.replace(':', '')} UTC.csv` }, body);
}
fs.writeFileSync(path.join(gdir, 'index.jsonl'), index);

const rtlPort = await freePort(), SP = await freePort();
procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' }));
await sleep(400);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const slog = fs.openSync(path.join(T, 'server.log'), 'w');
// ★ --trusted-proxies 127.0.0.1: what the daemon sets for itself while it runs its own tunnel (setTunnelLoopbackTrust)
procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(SP), '--rate', '2400000', '--freq', '93200000', '--mode', 'wfm', '--users', '4',
                       '--trusted-proxies', '127.0.0.1'], { env, stdio: ['ignore', slog, slog] }));
for (let i = 0; i < 100; i++) {
  if (await new Promise((r) => { const c = net.connect(SP, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
  await sleep(100);
}
const S = `http://127.0.0.1:${SP}`;

// ── the "tunnel": TLS in front of the receiver, X-Forwarded-For added to the request (cloudflared's) ──
const tunnelHits = { [TUN1]: 0, [TUN2]: 0 };
function startTunnel(name) {
  const socks = new Set();
  const srv = tls.createServer({ key: KEY, cert: CERT }, (c) => {
    socks.add(c); c.on('close', () => socks.delete(c));
    let buf = Buffer.alloc(0), up = null;
    c.on('data', function first(d) {
      buf = Buffer.concat([buf, d]);
      const nl = buf.indexOf('\r\n');
      if (nl < 0) return;
      c.off('data', first);
      tunnelHits[name]++;
      up = net.connect(SP, '127.0.0.1', () => {
        up.write(Buffer.concat([buf.subarray(0, nl + 2), Buffer.from('X-Forwarded-For: 198.51.100.7\r\n'), buf.subarray(nl + 2)]));
        c.pipe(up); up.pipe(c);
      });
      socks.add(up); up.on('close', () => { socks.delete(up); c.destroy(); }); up.on('error', () => c.destroy());
    });
    c.on('error', () => up && up.destroy());
  });
  return new Promise((res) => srv.listen(0, '127.0.0.1', () => res({ port: srv.address().port,
    close: () => new Promise((r) => { srv.close(() => r()); for (const s of socks) s.destroy(); }) })));
}
const tun1 = await startTunnel(TUN1);
let direct = `${TUN1}:${tun1.port}`;

// ── the "Worker": the slug address — what serveBySlug does to a page, and a COUNT of everything that reaches it ──
const workerHits = [];
const worker = https.createServer({ key: KEY, cert: CERT }, (req, res) => {
  const u = new URL(req.url, 'https://x');
  workerHits.push({ method: req.method, path: u.pathname, q: u.search, t: Date.now() });
  const h = { ...req.headers };
  delete h.cookie; delete h.authorization; delete h['accept-encoding'];
  h['x-forwarded-for'] = '198.51.100.7'; h['x-real-ip'] = '198.51.100.7';
  h['x-vibesdr-via'] = 'directory';
  h['x-vibesdr-direct-host'] = direct;
  h.host = direct;                                     // ★ the tunnel delivers the request under its own name
  const statusRead = (req.method === 'GET' || req.method === 'HEAD') && /^\/(r\/[^/]+\/)?vibeserver\.json$/.test(u.pathname);
  if (statusRead) { delete h.origin; delete h.referer; }
  const up = http.request({ host: '127.0.0.1', port: SP, method: req.method, path: req.url, headers: h }, (r) => {
    const type = String(r.headers['content-type'] || '');
    const chunks = [];
    r.on('data', (d) => chunks.push(d));
    r.on('end', () => {
      let body = Buffer.concat(chunks);
      const hdr = { ...r.headers }; delete hdr['set-cookie']; delete hdr['content-length'];
      if (statusRead && type.includes('application/json')) {
        const j = JSON.parse(body.toString('utf8')); j.directHost = direct; j.directUrl = `https://${direct}`;
        res.writeHead(r.statusCode, { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store', 'access-control-allow-origin': '*' });
        return res.end(JSON.stringify(j));
      }
      if (type.includes('text/html')) {
        let html = body.toString('utf8');
        const csp = String(r.headers['content-security-policy'] || '');
        const scriptSrc = (/(?:^|;)\s*script-src([^;]*)/i.exec(csp) || [])[1] || '';
        const assetsOk = scriptSrc.toLowerCase().split(/\s+/).includes(`https://${direct}`.toLowerCase());
        const inject = `<meta name="vibe-direct-host" content="${direct}">` + (assetsOk ? `<meta name="vibe-direct-assets" content="${direct}">` : '')
                     + `<script>window.__VIBE_DIRECT_HOST__=${JSON.stringify(direct)};</script>`;
        const CS = '<meta charset="utf-8">', at = html.indexOf(CS);
        html = at !== -1 && at < 512 ? html.slice(0, at + CS.length) + inject + html.slice(at + CS.length) : html.replace('</head>', inject + '</head>');
        hdr['cache-control'] = 'no-store'; delete hdr['content-encoding'];
        workerHits[workerHits.length - 1].doc = true; workerHits[workerHits.length - 1].assetsOk = assetsOk;
        res.writeHead(r.statusCode, hdr);
        return res.end(html);
      }
      res.writeHead(r.statusCode, hdr);
      res.end(body);
    });
  });
  up.on('error', () => { res.writeHead(503); res.end(); });
  req.pipe(up);
});
worker.on('upgrade', (req, sock) => { workerHits.push({ method: 'UPGRADE', path: req.url, t: Date.now() }); sock.end('HTTP/1.1 426 Upgrade Required\r\n\r\n'); });
const WP = await new Promise((r) => worker.listen(0, '127.0.0.1', () => r(worker.address().port)));
const PAGE = `https://${SLUG}:${WP}`;

try {
  // ═══ A. CORS and the policy, straight from the receiver ═══
  console.log('── A. CORS from the receiver itself ──');
  // ★ as the tunnel delivers it: cloudflared's X-Forwarded-For (a loopback request with a foreign Origin is refused outright)
  const XFF = { 'x-forwarded-for': '198.51.100.7' };
  const get = (p, origin, extra = {}) => fetch(`${S}${p}`, { headers: { ...XFF, ...(origin ? { origin } : {}), ...extra } });
  const adminQ = async () => {
    const nonce = (await fetch(`${S}/vibeserver/auth`).then((r) => r.json())).nonce;
    const { createHmac } = await import('node:crypto');
    return `vs_admin_nonce=${nonce}&vs_admin_auth=${createHmac('sha256', 'dd-admin').update(nonce).digest('hex')}`;
  };
  const SO = PAGE;
  for (const p of ['/vibeserver.json', '/vibeserver/gallery.json?limit=5', '/vibeserver/dxlog.json?dial=1', `/vibeserver/gallery/file/20261124-101500-wefax-4610k0-0a01.png`]) {
    const r = await get(p, SO);
    ok(r.headers.get('access-control-allow-origin') === SO && /origin/i.test(r.headers.get('vary') || ''),
       `a slug origin is echoed on ${p.split('?')[0]} (${r.status}, ACAO ${r.headers.get('access-control-allow-origin')}, Vary ${r.headers.get('vary')})`);
  }
  {
    const q = await adminQ();
    const r1 = await get(`/vibeserver/gallery.json?limit=5&${q}`, SO);
    ok(r1.headers.get('access-control-allow-origin') === SO, `★★ an authed read from a slug origin: its own origin, not * (${r1.headers.get('access-control-allow-origin')})`);
    const q2 = await adminQ();
    const r2 = await get(`/vibeserver/gallery.json?limit=5&${q2}`, 'https://evil.example');
    ok(r2.headers.get('access-control-allow-origin') === null, `★★★ an authed read from a stranger: NO Access-Control-Allow-Origin (${r2.headers.get('access-control-allow-origin')})`);
    const r3 = await get(`/vibeserver/dxlog.json?vs_admin_ticket=bogus`, 'https://evil.example');
    ok(r3.headers.get('access-control-allow-origin') === null, `★★ a stranger carrying an admin ticket, even a refused one: none (${r3.status})`);
    const r4 = await get('/vibeserver.json', 'https://evil.example');
    ok(r4.headers.get('access-control-allow-origin') === '*', `the public status read from a stranger keeps its * (${r4.headers.get('access-control-allow-origin')})`);
    const r5 = await get('/vibeserver.json');
    ok(r5.headers.get('access-control-allow-origin') === '*' && !(r5.headers.get('vary') || '').includes('Origin'), 'no Origin: the answer exactly as before');
    const r6 = await get(`/vibeserver/admin/status?${await adminQ()}`, SO);
    ok(r6.headers.get('access-control-allow-origin') === SO, `an admin read from the slug page: echoed (${r6.status})`);
    const r7 = await get(`/vibeserver/admin/status?${await adminQ()}`, 'https://evil.example');
    ok(r7.headers.get('access-control-allow-origin') === null, `★★★ an admin read from a stranger: none — it was * before (${r7.status})`);
    const r8 = await get('/vibeserver/admin/status', 'https://evil.example');
    ok(r8.status === 401 && r8.headers.get('access-control-allow-origin') === null, `the admin 401 to a stranger: none (${r8.status})`);
  }
  {
    const pf = await fetch(`${S}/mapgl/vibemap-style.json`, { method: 'OPTIONS', headers: { ...XFF, origin: SO, 'access-control-request-method': 'GET', 'access-control-request-headers': 'range' } });
    ok(pf.status === 204 && pf.headers.get('access-control-allow-origin') === SO && /range/i.test(pf.headers.get('access-control-allow-headers') || ''),
       `a preflight from the slug page: 204, its origin, Range allowed (${pf.status})`);
    const pf2 = await fetch(`${S}/mapgl/vibemap-style.json`, { method: 'OPTIONS', headers: { ...XFF, origin: 'https://evil.example', 'access-control-request-method': 'GET' } });
    ok(pf2.status !== 204 && pf2.headers.get('access-control-allow-origin') === null, `…from a stranger: no preflight answer (${pf2.status})`);
  }
  {
    const viaDir = await fetch(`${S}/`, { headers: { 'x-vibesdr-via': 'directory', 'x-vibesdr-direct-host': direct, host: direct } });
    const csp = viaDir.headers.get('content-security-policy') || '';
    ok(new RegExp(`script-src 'self' https://${direct.replace(/\./g, '\\.')} `).test(csp), `the page's policy names the tunnel host when the directory proxied it`);
    const plain = await fetch(`${S}/`);
    ok(!/trycloudflare/.test(plain.headers.get('content-security-policy') || ''), 'a direct visit: the policy is unchanged');
  }

  // ═══ B. the page, through the Worker ═══
  console.log('── B. the page through the stand-in Worker: nothing but the document reaches it ──');
  const cdp = await freePort();
  procs.push(spawn('nice', ['-n', '10', EDGE, '--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', '--no-default-browser-check',
    '--disable-extensions', '--ignore-certificate-errors', `--host-resolver-rules=MAP ${SLUG} 127.0.0.1, MAP ${TUN1} 127.0.0.1, MAP ${TUN2} 127.0.0.1, MAP * ~NOTFOUND`,
    `--remote-debugging-port=${cdp}`, `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=1200,850', 'about:blank'], { stdio: 'ignore' }));
  let target;
  for (let i = 0; i < 50 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdp}/json`)).json()).find((t) => t.type === 'page'); } catch {} if (!target) await sleep(250); }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => (ws.onopen = r));
  let id = 0; const pending = new Map(); const errors = []; const reqs = [];
  ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') errors.push(m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
    if (m.method === 'Network.requestWillBeSent') reqs.push(m.params.request.url); };
  const call = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
  const js = async (expr) => (await call('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const until = async (expr, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (await js(expr)) return true; await sleep(200); } return false; };
  await call('Runtime.enable'); await call('Page.enable'); await call('Network.enable');

  await call('Page.navigate', { url: `${PAGE}/?join` });
  ok(await until(`document.readyState === 'complete' && !!document.getElementById('galleryBtn')`, 20000), 'the page loads through the Worker');
  const docHit = workerHits.find((h) => h.doc);
  ok(!!docHit && docHit.assetsOk, `the Worker saw the receiver's policy allow its host, and said so (vibe-direct-assets)`);
  ok(await js(`window.__vsJsOrigin`) === `https://${direct}`, `★★ the page's scripts come from the receiver (${await js('window.__vsJsOrigin')})`);
  await js(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  ok(await until(`!!document.getElementById('galleryBtn').onclick`, 20000), 'the radio joined (the socket went direct)');
  await js(`(() => { for (const id of ['splash', 'audioGate']) { const s = document.getElementById(id); if (s) s.style.display = 'none'; } return true; })()`);
  // the gallery: the list, the image, a pass's telemetry charts
  await js(`document.getElementById('galleryBtn').click(); true`);
  ok(await until(`document.querySelectorAll('#galList .sres[role=button]').length >= 5`, 10000), 'the gallery lists its items');
  await js(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /WEFAX test chart/.test(r.textContent)).click(); true`);
  ok(await until(`(() => { const i = document.getElementById('galImg') || document.querySelector('#galView img'); return !!i && i.complete && i.naturalWidth > 0; })()`, 8000),
     'an IMAGE opens and draws (from the receiver directly)');
  await js(`document.getElementById('galBack').click(); true`);
  await until(`document.getElementById('galView').hidden`, 4000);
  await js(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /pass/.test(r.textContent)).click(); true`);
  ok(await until(`!!document.getElementById('galCharts') && !document.getElementById('galCharts').hidden && document.querySelectorAll('#galCharts .tlm-c').length > 0`, 10000),
     'a FUNcube pass opens as TELEMETRY CHARTS (the passes read from the gallery)');
  await js(`document.getElementById('galBack').click(); true`);
  await js(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /NAVTEX test/.test(r.textContent)).click(); true`);
  ok(await until(`/TEST MESSAGE/.test(document.getElementById('galText').textContent)`, 8000), 'a text item opens');
  // the DX Log
  ok(await until(`!document.getElementById('dxLogBtn').hidden`, 20000), 'the DX Log key is offered');
  await js(`document.getElementById('dxLogBtn').click(); true`);
  ok(await until(`/RAI R1/.test(document.getElementById('dxLogPanel').textContent)`, 10000), 'the DX Log lists its catches');
  await sleep(2500);    // ★ let anything periodic (conditions, logos, dial labels) have its go

  const after = workerHits.filter((h) => !h.doc);
  const FURNITURE = /^\/(sw\.js|manifest\.webmanifest|favicon\.png|apple-touch-icon[^/]*|icon-(192|512)\.png)$/;
  const stray = after.filter((h) => !FURNITURE.test(h.path));
  const furniture = after.filter((h) => FURNITURE.test(h.path));
  console.log(`      Worker: ${workerHits.filter((h) => h.doc).length} document, ${furniture.length} browser furniture (${[...new Set(furniture.map((h) => h.path))].join(' ')}), ${stray.length} other`);
  console.log(`      the page's re-learns so far: ${await js('JSON.stringify(window.__vibeDirect && window.__vibeDirect())')}`);
  const directReqs = reqs.filter((u) => u.startsWith(`https://${direct}/`));
  console.log(`      direct to the tunnel: ${directReqs.length} requests (${[...new Set(directReqs.map((u) => new URL(u).pathname.replace(/\/(c|app)-[A-Z0-9]+\.js$/, '/$1-….js').replace(/\/file\/.*/, '/file/…')))].slice(0, 14).join(' ')} …)`);
  ok(stray.length === 0, `★★★ ZERO requests reach the Worker after the document${stray.length ? ': ' + stray.map((h) => `${h.method} ${h.path}`).join(', ') : ''}`);
  ok(!after.some((h) => h.path.startsWith('/vs/')), '★★ no script through the Worker');
  ok(!after.some((h) => h.method === 'UPGRADE'), 'no socket through the Worker (it refuses them)');
  ok(['/vibeserver/gallery.json', '/vibeserver/gallery/file/', '/vibeserver/dxlog.json', '/vs/app-'].every((p) => directReqs.some((u) => new URL(u).pathname.startsWith(p))),
     'the gallery list, its files, the DX Log and the scripts all went to the receiver directly');
  ok(errors.length === 0, `no exception in the page${errors.length ? ': ' + errors[0] : ''}`);

  // ═══ C. the tunnel host rotates ═══
  console.log('── C. the tunnel restarts under a new name: one re-learn through the Worker, then direct again ──');
  const tun2 = await startTunnel(TUN2);
  const oldDirect = direct;
  direct = `${TUN2}:${tun2.port}`;              // ★ what the directory now says (the server pinged its new address)
  await tun1.close();
  const before = workerHits.length;
  // a direct read now fails (the old host is gone) → re-learn → the page reloads onto the new host
  await js(`fetch('https://${oldDirect}/vibeserver.json', { cache: 'no-store' }).catch(() => 0); true`);
  ok(await until(`window.__VIBE_DIRECT_HOST__ === ${JSON.stringify(direct)} && document.readyState === 'complete'`, 15000), `the page reloaded onto the new host (${direct})`);
  const relearn = workerHits.slice(before).filter((h) => !h.doc && !FURNITURE.test(h.path));
  ok(relearn.length === 1 && relearn[0].path === '/vibeserver.json', `★★ exactly ONE read through the Worker to re-learn it (${relearn.map((h) => h.path).join(', ')})`);
  await js(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  ok(await until(`!!document.getElementById('galleryBtn').onclick`, 20000), 'the radio joined again, on the new host');
  const h2 = tunnelHits[TUN2];
  await js(`document.getElementById('galleryBtn').click(); true`);
  ok(await until(`document.querySelectorAll('#galList .sres[role=button]').length >= 5`, 10000) && tunnelHits[TUN2] > h2, 'the gallery reads from the new host');
  const after2 = workerHits.slice(before).filter((h) => !h.doc && !FURNITURE.test(h.path));
  ok(after2.length === 1, `still nothing else through the Worker (${after2.map((h) => h.path).join(', ')})`);
  await tun2.close();
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  clearTimeout(deadline);
  try { worker.close(); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
