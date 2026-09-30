// test-web-serving.mjs — the REAL server hands out the web client exactly as built: the page and every
// script file, in every encoding a browser may ask for, byte for byte, with the right caching.
//
// ★★★ WHY. Since 2026-09-30 the page is a shell and its code is separate files under /vs/, each
//     pre-compressed at build time (build-web.mjs) and compiled into vibe_web_page.h. Four things can
//     now go wrong that could not before, and each is silent in a browser until someone listens:
//       • a compressed copy that does not decode to the built file (the WASM Opus decoder inside it
//         is CRC-checked, so one flipped byte is no audio for that class of browser only);
//       • a script served without `immutable`, so every visit pays for it again — the whole point;
//       • an HTML page cached by something between us and the listener (it must stay no-store: the
//         directory injects a tunnel host into it that changes);
//       • a /r/<id>/ page whose scripts 404 because the prefix was not stripped for /vs/.
// ★ Compares against web/dist — so the binary must be built from the SAME build-web.mjs run. A stale
//   binary fails here, which is the point: it would serve yesterday's client.
// ★ Needs a BUILT vibeserver: VIBESERVER_BIN=/path/to/vibeserver. Without one it reports "not run"
//   (exit 3) — a result, not a pass (see run-tests.sh). SILENT: no browser, no audio.
//
//   usage: node scripts/build-web.mjs && cmake --build vibeserver/build && \
//          VIBESERVER_BIN=vibeserver/build/vibeserver node scripts/test-web-serving.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** One raw HTTP/1.1 GET — no client library, so nothing decompresses or caches behind our back. */
function get(port, pathq, acceptEncoding) {
  return new Promise((res, rej) => {
    const s = net.connect(port, '127.0.0.1');
    const chunks = [];
    s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\n`
      + (acceptEncoding === null ? '' : `Accept-Encoding: ${acceptEncoding}\r\n`) + '\r\n'));
    s.on('data', (d) => chunks.push(d));
    s.on('error', rej);
    s.on('end', () => {
      const b = Buffer.concat(chunks);
      const i = b.indexOf('\r\n\r\n');
      const lines = b.subarray(0, i).toString().split('\r\n');
      const h = {};
      for (const l of lines.slice(1)) { const c = l.indexOf(':'); h[l.slice(0, c).toLowerCase()] = l.slice(c + 1).trim(); }
      res({ status: Number(lines[0].split(' ')[1]), h, body: b.subarray(i + 4) });
    });
  });
}
const decode = (r) => r.h['content-encoding'] === 'gzip' ? zlib.gunzipSync(r.body)
  : r.h['content-encoding'] === 'br' ? zlib.brotliDecompressSync(r.body) : r.body;

const DIST = path.join(root, 'web/dist');
const page = fs.readFileSync(path.join(DIST, 'vibesdr.html'));
const scripts = fs.readdirSync(path.join(DIST, 'vs')).filter((n) => n.endsWith('.js'));
ok(scripts.some((n) => n.startsWith('app-')), `web/dist/vs holds the entry (${scripts.length} script file(s))`);

// ── a single-radio server on the fake dongle ────────────────────────────────────────────────────
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-web-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, adminPass: 'testadmin123', name: 'Web test', mode: 'single', sharing: 'local', users: 4, web: true }));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000'], {
  env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
         VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') }, stdio: ['ignore', log, log] });
try {
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  // Browsers: none (curl), plain-http (no br), https (br), and a refusal of br by q=0.
  const CASES = [[null, undefined], ['identity', undefined], ['gzip, deflate', 'gzip'],
                 ['gzip, deflate, br, zstd', 'br'], ['br;q=0, gzip', 'gzip']];
  for (const prefix of ['', '/r/abc12345']) {
    console.log(`── ${prefix || '/'} ──`);
    for (const [ae, want] of CASES) {
      const r = await get(port, prefix + '/', ae);
      ok(r.status === 200 && r.h['content-encoding'] === want, `page [${ae}] → ${r.status}, ${want || 'identity'} (${r.body.length} B on the wire)`);
      ok(Buffer.compare(decode(r), page) === 0, `page [${ae}] decodes to web/dist/vibesdr.html byte for byte`);
      ok(r.h['cache-control'] === 'no-store' && r.h['vary'] === 'Accept-Encoding', 'page: no-store, Vary: Accept-Encoding');
      ok(!/'unsafe-eval'/.test(r.h['content-security-policy'] || '') && /wasm-unsafe-eval/.test(r.h['content-security-policy'] || ''),
         "page CSP: no 'unsafe-eval' (nothing evals any more), 'wasm-unsafe-eval' kept for the Opus decoder");
      for (const n of scripts) {
        const s = await get(port, `${prefix}/vs/${n}`, ae);
        const built = fs.readFileSync(path.join(DIST, 'vs', n));
        ok(s.status === 200 && s.h['content-encoding'] === want && Buffer.compare(decode(s), built) === 0,
           `/vs/${n} [${ae}] → ${s.status}, byte for byte (${s.body.length} B on the wire)`);
        ok(s.h['cache-control'] === 'public, max-age=31536000, immutable' && s.h['vary'] === 'Accept-Encoding'
           && /^text\/javascript; charset=utf-8$/.test(s.h['content-type'] || ''), `/vs/${n}: immutable, Vary, text/javascript`);
      }
    }
    // ★ The NUL bytes the Opus decoder's CRC depends on reach the client as they were built.
    const withNul = scripts.find((n) => fs.readFileSync(path.join(DIST, 'vs', n)).includes(0));
    ok(!!withNul, `a script carries the WASM module's raw NUL bytes (${withNul})`);
    const q = await get(port, `${prefix}/vs/${scripts[0]}?v=1`, 'gzip');
    ok(q.status === 200, 'a query string on a script URL is ignored');
    const nf = await get(port, `${prefix}/vs/app-NOSUCHHASH.js`, 'gzip');
    ok(nf.status === 404, `an unknown script is a 404 (${nf.status}), never the page`);
  }
} finally {
  srv.kill('SIGTERM'); rtl.kill('SIGTERM');
  await sleep(300);
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch { /* best effort */ }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
