// test-web-chunk-cache.mjs — the SHARED CHUNK CACHE in a REAL browser, across REAL servers.
//
// ★★★ WHAT IT PROVES (web/client/src/chunkCache.ts, directory/public/store.html):
//   1. Server A opens the map: the chunk is fetched from A once and lands in the directory frame's
//      IndexedDB under its SHA-256.
//   2. Server B (another origin, another vibeserver process) opens the map: ZERO requests for the
//      chunk reach B, and the map window is written all the same.
//   3. The admin panel on B: fetched once, stored; its imports of the page's shared chunk reach the
//      SAME module the page already has (no second request for it) and the panel opens.
//   4. A TAMPERED store entry (malicious code of the right length, written straight into the frame's
//      IndexedDB) is NOT run on server C: C fetches the chunk itself, the map works, and the entry is
//      replaced with the genuine bytes.
//   5. A hostile page posting poisoned bytes under the genuine key is refused by the store.
// ★ How: a local https front (self-signed, trusted by SPKI for this browser only) answers for
//   a/b/c.vibeserver.vibesdr.net and vibeserver.vibesdr.net, mapped to 127.0.0.1 by the browser's
//   host resolver rules. a → server A; b and c → server B. vibeserver.vibesdr.net/store → this tree's
//   directory/public/store.html. Nothing leaves the machine.
// ★ SILENT: headless Edge with --mute-audio, a throwaway profile, deleted at the end.
// ★ Needs VIBESERVER_BIN = a vibeserver built from THIS tree after `node scripts/build-web.mjs` (its
//   embedded page must carry the manifest), and Microsoft Edge + openssl. Otherwise: not run (exit 3).
//
//   usage: VIBESERVER_BIN=/path/to/vibeserver node scripts/test-web-chunk-cache.mjs
import { spawn, execFileSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) {
  console.log('   not run — needs VIBESERVER_BIN (built from this tree) and Microsoft Edge');
  process.exit(3);
}
const html = fs.readFileSync(path.join(root, 'web/dist/vibesdr.html'), 'utf8');
const man = JSON.parse(/<script type="application\/json" id="vs-chunks">([^<]*)<\/script>/.exec(html)?.[1] || '{}');
if (!man.vibemap || !man.admin) { console.log('   FAIL web/dist/vibesdr.html has no chunk manifest — run node scripts/build-web.mjs'); process.exit(1); }
const sharedChunk = (/<link rel="modulepreload" href="\/vs\/(c-[A-Z0-9]+\.js)">/.exec(html.replace(/<link rel="modulepreload" href="\/vs\/app-[^"]+">/, '')) || [])[1];

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const tmp = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'vs-chunkcache-'));
const procs = [];

// ── two real servers on fake dongles ──────────────────────────────────────────────────────────────
async function server(tag) {
  const dir = path.join(tmp, tag);
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, adminPass: 'testadmin123', name: 'Chunk cache ' + tag, mode: 'single', sharing: 'local', users: 4, web: true }));
  const rtlPort = await freePort(), port = await freePort();
  procs.push(spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' }));
  await sleep(300);
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  procs.push(spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000'], {
    env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
           VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') }, stdio: ['ignore', log, log] }));
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  return port;
}

// ── the https front ────────────────────────────────────────────────────────────────────────────
execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=vibeserver.vibesdr.net',
  '-addext', 'subjectAltName=DNS:vibeserver.vibesdr.net,DNS:*.vibeserver.vibesdr.net',
  '-keyout', path.join(tmp, 'key.pem'), '-out', path.join(tmp, 'cert.pem')], { stdio: 'ignore' });
const cert = fs.readFileSync(path.join(tmp, 'cert.pem'));
const spki = crypto.createHash('sha256').update(new crypto.X509Certificate(cert).publicKey.export({ type: 'spki', format: 'der' })).digest('base64');

const hits = {};   // host → path → count
const count = (host, p) => { (hits[host] ??= {})[p] = (hits[host][p] || 0) + 1; };
const n = (host, file) => hits[host]?.['/vs/' + file] || 0;
let backends = {};
const front = https.createServer({ key: fs.readFileSync(path.join(tmp, 'key.pem')), cert }, (req, res) => {
  const host = (req.headers.host || '').split(':')[0];
  const p = (req.url || '').split('?')[0];
  count(host, p);
  if (host === 'vibeserver.vibesdr.net') {
    if (p === '/store') {
      res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' });
      res.end(fs.readFileSync(path.join(root, 'directory/public/store.html')));
    } else { res.writeHead(404); res.end(); }
    return;
  }
  if (host === 'evil.vibeserver.vibesdr.net') {
    // ★ A hostile server's page: it tries to plant code under the genuine map's key.
    res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
    res.end(`<!doctype html><meta charset="utf-8"><body><script>
      window.result = new Promise((done) => {
        const f = document.createElement('iframe'); f.src = 'https://vibeserver.vibesdr.net/store'; f.style.display = 'none';
        addEventListener('message', (e) => { if (e.origin === 'https://vibeserver.vibesdr.net' && e.data.id === 7) done(e.data.ok); });
        f.onload = () => { const evil = new TextEncoder().encode('window.PWNED=1;'.padEnd(${man.vibemap.size}, ' ')).buffer;
          f.contentWindow.postMessage({ op: 'chunkPut', id: 7, sha: '${man.vibemap.sha256}', name: 'vibemap', bytes: evil }, 'https://vibeserver.vibesdr.net', [evil]); };
        document.body.appendChild(f);
        setTimeout(() => done('no answer'), 3000);
      });
    </script>`);
    return;
  }
  const port = backends[host.split('.')[0]];
  if (!port) { res.writeHead(404); res.end(); return; }
  const up = http.request({ host: '127.0.0.1', port, method: req.method, path: req.url, headers: req.headers }, (r) => {
    res.writeHead(r.statusCode || 502, r.headers); r.pipe(res);
  });
  up.on('error', () => { try { res.writeHead(502); res.end(); } catch { /* gone */ } });
  req.pipe(up);
});
front.on('upgrade', (req, sock, head) => {   // the radio's WebSockets, passed straight through
  const port = backends[(req.headers.host || '').split('.')[0]];
  if (!port) return sock.destroy();
  const up = net.connect(port, '127.0.0.1', () => {
    up.write(`${req.method} ${req.url} HTTP/1.1\r\n` + Object.entries(req.headers).map(([k, v]) => `${k}: ${v}`).join('\r\n') + '\r\n\r\n');
    if (head?.length) up.write(head);
    sock.pipe(up).pipe(sock);
  });
  up.on('error', () => sock.destroy()); sock.on('error', () => up.destroy());
});

let edge, prof;
try {
  const A = await server('A'), B = await server('B');
  backends = { a: A, b: B, c: B };
  const frontPort = await freePort();
  await new Promise((r) => front.listen(frontPort, '127.0.0.1', r));
  const CDP = await freePort();
  prof = fs.mkdtempSync(path.join(tmp, 'edge-'));
  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${CDP}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    `--user-data-dir=${prof}`, `--host-resolver-rules=MAP *.vibesdr.net 127.0.0.1:${frontPort},MAP vibesdr.net 127.0.0.1:${frontPort}`,
    `--ignore-certificate-errors-spki-list=${spki}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  let target = null;
  for (let i = 0; i < 50 && !target; i++) {
    await sleep(200);
    try { target = (await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).find((t) => t.type === 'page'); } catch { /* not up yet */ }
  }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const consoleLines = []; const contexts = [];
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); return; }
    if (m.method === 'Runtime.consoleAPICalled') consoleLines.push((m.params.args || []).map((a) => a.value ?? a.description ?? '').join(' '));
    if (m.method === 'Runtime.executionContextCreated') contexts.push(m.params.context);
    if (m.method === 'Runtime.executionContextsCleared') contexts.length = 0;
  };
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  await new Promise((r) => { ws.onopen = r; });
  await send('Page.enable'); await send('Runtime.enable');
  const ev = async (expression, extra = {}) => {
    const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true, userGesture: true, ...extra });
    if (r.result?.exceptionDetails) return { error: r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text };
    return r.result?.result?.value;
  };
  const until = async (expr, ms = 10000) => { for (let t = 0; t < ms; t += 200) { if (await ev(expr) === true) return true; await sleep(200); } return false; };
  /** An expression run INSIDE the store frame (vibeserver.vibesdr.net) on the current page. */
  const inStore = async (expr) => {
    const ctx = contexts.find((c) => c.origin === 'https://vibeserver.vibesdr.net');
    if (!ctx) return { error: 'no store frame context' };
    return ev(expr, { contextId: ctx.id });
  };
  const IDB = `(async (fn) => { const db = await new Promise((r, j) => { const q = indexedDB.open('vsChunks', 1); q.onupgradeneeded = () => q.result.createObjectStore('chunks', { keyPath: 'sha' }); q.onsuccess = () => r(q.result); q.onerror = () => j(q.error); });
     return fn(db.transaction('chunks', 'readwrite').objectStore('chunks')); })`;
  const shaOf = `async (b) => [...new Uint8Array(await crypto.subtle.digest('SHA-256', b))].map((x) => x.toString(16).padStart(2, '0')).join('')`;
  const storedSha = (sha) => inStore(`${IDB}((st) => new Promise((r) => { const q = st.get('${sha}'); q.onsuccess = async () => r(q.result ? await (${shaOf})(q.result.bytes) : null); }))`);

  /** Open server `h`'s page, start it, and wait for the store frame. */
  const visit = async (h) => {
    await send('Page.navigate', { url: `https://${h}.vibeserver.vibesdr.net/` });
    await until(`document.readyState === 'complete' && !!document.getElementById('mapBtn')`);
    await sleep(1500);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
    const framed = await until(`!!document.querySelector('iframe[src^="https://vibeserver.vibesdr.net"]')`, 8000);
    await sleep(800);
    return framed;
  };
  const openMap = async () => {
    const before = (await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).filter((t) => t.type === 'page').length;
    await ev(`document.getElementById('mapBtn').click()`);
    await sleep(3000);
    const pages = (await (await fetch(`http://127.0.0.1:${CDP}/json/list`)).json()).filter((t) => t.type === 'page');
    const pop = pages.find((t) => t.id !== target.id && t.url === 'about:blank') || pages.find((t) => t.id !== target.id);
    let len = 0;
    if (pop) {
      const pws = new WebSocket(pop.webSocketDebuggerUrl);
      await new Promise((r) => { pws.onopen = r; });
      len = await new Promise((r) => { pws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id === 1) r(m.result?.result?.value || 0); };
        pws.send(JSON.stringify({ id: 1, method: 'Runtime.evaluate', params: { expression: 'document.documentElement.outerHTML.length', returnByValue: true } })); });
      await new Promise((r) => { pws.onmessage = r; pws.send(JSON.stringify({ id: 2, method: 'Page.close' })); setTimeout(r, 500); });
      pws.close();
    }
    return { opened: pages.length > before, len };
  };

  console.log('── server A: first visit, empty store ──');
  ok(await visit('a'), 'A: the page loaded the directory store frame');
  let m = await openMap();
  ok(m.opened && m.len > man.vibemap.size * 0.9, `A: the map window opened and carries the renderer (${m.len} chars)`);
  ok(n('a.vibeserver.vibesdr.net', man.vibemap.file) === 1, `A: the map chunk was fetched from A once (${n('a.vibeserver.vibesdr.net', man.vibemap.file)})`);
  await sleep(500);
  ok(await storedSha(man.vibemap.sha256) === man.vibemap.sha256, 'A: the store now holds it, and its bytes hash to the key');

  console.log('── server B: another origin, another process ──');
  ok(await visit('b'), 'B: the page loaded the store frame');
  m = await openMap();
  ok(m.opened && m.len > man.vibemap.size * 0.9, `B: the map window opened and carries the renderer (${m.len} chars)`);
  ok(n('b.vibeserver.vibesdr.net', man.vibemap.file) === 0, `B: ZERO requests for the map chunk reached B (${n('b.vibeserver.vibesdr.net', man.vibemap.file)})`);

  console.log('── admin panel on B, then C ──');
  await ev(`(() => { const i = document.getElementById('adminPwInput'); i.value = 'testadmin123'; document.getElementById('adminPwGo').click(); })()`);
  await sleep(1500);
  await ev(`document.getElementById('btnServerAdmin').click()`);
  ok(await until(`document.getElementById('adminPanel') && !document.getElementById('adminPanel').hidden`, 6000), 'B: the admin panel opened (loaded via the cache path)');
  ok(n('b.vibeserver.vibesdr.net', man.admin.file) === 1, 'B: the admin chunk was fetched from B once');
  ok(!sharedChunk || n('b.vibeserver.vibesdr.net', sharedChunk) === 1,
     `B: the panel's imports reached the page's OWN shared module — ${sharedChunk} requested once, not twice`);
  await sleep(500);
  ok(await storedSha(man.admin.sha256) === man.admin.sha256, 'B: the admin chunk is in the store under its hash');
  ok(await visit('c'), 'C: the page loaded the store frame');
  await ev(`(() => { const i = document.getElementById('adminPwInput'); i.value = 'testadmin123'; document.getElementById('adminPwGo').click(); })()`);
  await sleep(1500);
  await ev(`document.getElementById('btnServerAdmin').click()`);
  ok(await until(`document.getElementById('adminPanel') && !document.getElementById('adminPanel').hidden`, 6000), 'C: the admin panel opened from the store');
  ok(n('c.vibeserver.vibesdr.net', man.admin.file) === 0, 'C: ZERO requests for the admin chunk');

  console.log('── a hostile server ──');
  await send('Page.navigate', { url: 'https://evil.vibeserver.vibesdr.net/' });
  await sleep(1000);
  ok(await ev('window.result') === false, 'a hostile page posting poisoned bytes under the genuine key is refused by the store');
  ok(await storedSha(man.vibemap.sha256) === man.vibemap.sha256, '... and the genuine entry is untouched');

  console.log('── a tampered entry (written straight into IndexedDB) ──');
  ok(await visit('c'), 'C: the page loaded the store frame');
  const wrote = await inStore(`${IDB}((st) => new Promise((r) => { const evil = new TextEncoder().encode('window.PWNED=1;export const VIBEMAP_JS="evil";'.padEnd(${man.vibemap.size}, ' ')).buffer;
    const q = st.put({ sha: '${man.vibemap.sha256}', name: 'vibemap', size: ${man.vibemap.size}, used: Date.now(), bytes: evil }); q.onsuccess = () => r(true); q.onerror = () => r(false); }))`);
  ok(wrote === true && await storedSha(man.vibemap.sha256) !== man.vibemap.sha256, 'the store entry now holds malicious code of the right length');
  consoleLines.length = 0;
  m = await openMap();
  ok(await ev('window.PWNED === undefined') === true, 'C: the malicious code did NOT run');
  ok(m.opened && m.len > man.vibemap.size * 0.9, `C: the map window opened with the genuine renderer (${m.len} chars)`);
  ok(n('c.vibeserver.vibesdr.net', man.vibemap.file) === 1, 'C: the chunk was refetched from C');
  ok(consoleLines.some((l) => /failed its integrity check/.test(l)), 'C: the refusal is said in the console');
  await sleep(500);
  ok(await storedSha(man.vibemap.sha256) === man.vibemap.sha256, 'C: the entry was replaced with the genuine bytes');
  try { await send('Browser.close'); } catch { /* closing */ }
  ws.close();
} catch (e) {
  console.error(e); fail++;
} finally {
  try { edge?.kill(); } catch { /* gone */ }
  for (const p of procs) try { p.kill(); } catch { /* gone */ }
  front.close();
  await sleep(800);
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch (e) { console.error('cleanup', e); }
}
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
