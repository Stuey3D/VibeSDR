// test-navtex-warm.mjs — V12 THE SHIPPING MAP KEPT BUILT IN THE BACKGROUND (vibe_navtex_warm.h, 2026-11-20), from a REAL
// vibeserver whose gallery holds a large NAVTEX set (the fixtures repeated) — HTTP only, no browser, no audio:
//   1. WARM: nobody opens the map. Once the warm-up is idle, the FIRST navtex-warnings.json answers whole (pending 0,
//      every message indexed — no "Building … %"), navtex-met.json likewise, and the stations map answers from its cache.
//      A message saved later is indexed and the caches re-warmed, again with no request.
//   2. NO SPARE CPU (the gate held shut by VIBESERVER_NAVWARM_TEST_BUSY): the warm-up does nothing, and opening the map
//      backfills EXACTLY as before — 400 a request, "pending" while there are more ("if there has been no spare cpu time
//      to build the map then we revert to building on open like we do now", Stuart). Then the gate opens mid-backfill
//      while requests keep coming: the two share one index — every message once, never twice.
//   3. OFF (VIBESERVER_NAVWARM=0): state "off", the on-request build alone.
//   VIBESERVER_BIN=… [VIBE_NAVWARM_N=2000] node scripts/test-navtex-warm.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', SET = path.join(root, 'scripts/navtex-fixtures');
const N = Math.max(500, Number(process.env.VIBE_NAVWARM_N) || 2000);
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — VIBESERVER_BIN needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const watchdog = setTimeout(() => { console.log('   FAIL watchdog: 600 s'); process.exit(1); }, 600_000);

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-navwarm-'));
// ★ the fixtures, repeated to N: each copy its own name, an hour older per round, its warning number moved
const src = fs.readFileSync(path.join(SET, 'index.jsonl'), 'utf8').split('\n').filter((l) => l.startsWith('{'))
  .map((l) => JSON.parse(l)).filter((j) => fs.existsSync(path.join(SET, 'files', j.f)))
  .map((j) => ({ j, text: fs.readFileSync(path.join(SET, 'files', j.f), 'latin1') }));
const nowS = Math.floor(Date.now() / 1000), tMax = Math.max(...src.map((s) => s.j.t));
const copy = (i) => {
  const s = src[i % src.length], round = Math.floor(i / src.length);
  const f = `${s.j.f.slice(0, 22)}w${String(i).padStart(6, '0')}.txt`;
  const text = s.text.replace(/ZCZC (\w\w)\d\d/, (m, b) => `ZCZC ${b}${String(round % 100).padStart(2, '0')}`);
  return { line: JSON.stringify({ ...s.j, f, t: s.j.t - tMax + nowS - 3600 - round * 3600, bytes: Buffer.byteLength(text, 'latin1') }), f, text };
};
function makeGallery(name, n) {
  const d = path.join(dir, name);
  for (const s of ['data/gallery', 'run']) fs.mkdirSync(path.join(d, s), { recursive: true });
  const gal = path.join(d, 'data/gallery'), lines = [];
  for (let i = 0; i < n; i++) { const c = copy(i); fs.writeFileSync(path.join(gal, c.f), c.text, 'latin1'); lines.push(c.line); }
  fs.writeFileSync(path.join(gal, 'index.jsonl'), lines.join('\n') + '\n');
  fs.writeFileSync(path.join(d, 'config.json'), JSON.stringify({ configured: true, name: 'NAVTEX warm-up test', locator: 'IO92nh', adminPass: 'nav-warm-admin',
    galleryMb: 200, radios: [{ serial: 'nav-warm', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
  return d;
}
const procs = [];
async function startServer(d, extraEnv) {
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
  procs.push(rtl);
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(d, 'config.json'), VIBESERVER_DATA_DIR: path.join(d, 'data'), VIBESERVER_RUNTIME_DIR: path.join(d, 'run'),
    VIBESERVER_NAVWARM_GRACE_SEC: '1', ...extraEnv };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '518000', '--mode', 'usb', '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(path.join(d, 'server.log'), 'w'), 'ignore'] });
  procs.push(srv);
  for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break; await sleep(100); }
  return { H: `http://127.0.0.1:${port}`, srv, rtl };
}
const stop = async (s) => { s.srv.kill('SIGTERM'); s.rtl.kill('SIGTERM'); await sleep(500); };
const getJ = async (url) => { const t0 = performance.now(); const r = await fetch(url); const j = await r.json(); return { j, ms: performance.now() - t0, h: r.headers }; };
const warm = (H) => fetch(`${H}/vibeserver/gallery/navtex-warm.json`).then((r) => r.json());
async function until(fn, ms, every = 250) { const end = Date.now() + ms; while (Date.now() < end) { const v = await fn(); if (v) return v; await sleep(every); } return null; }
const idxLines = (d) => { try { return fs.readFileSync(path.join(d, 'data/gallery/navtex-warn.idx'), 'utf8').split('\n').filter((l) => l.startsWith('v')); } catch { return []; } };

try {
  // ── 1. WARM ─────────────────────────────────────────────────────────────────────────────────────────────────────────
  console.log(`── 1. the warm-up: a gallery of ${N} NAVTEX messages, nobody opens the map ──`);
  const dA = makeGallery('a', N);
  const A = await startServer(dA, {});
  const t0 = Date.now();
  const w1 = await until(async () => { const w = await warm(A.H); return w.state === 'idle' && w.parsed >= N ? w : null; }, 400_000, 500);
  const wall = (Date.now() - t0) / 1000;
  ok(!!w1, `the warm-up built the index alone: ${w1 ? `${w1.parsed} messages in ${w1.passes} passes, ${wall.toFixed(1)} s wall` : 'never finished'}`);
  if (w1) console.log(`      cost: ${(w1.busyMs / 1000).toFixed(1)} s of work at idle priority (${(100 * w1.busyMs / 1000 / wall).toFixed(0)} % of one core over the wall time), ` +
                      `longest index slice ${w1.maxSliceMs.toFixed(1)} ms CPU, longest pass (the whole-gallery scan, not sliced) ${w1.maxMs.toFixed(1)} ms CPU (${w1.maxWallMs.toFixed(0)} ms by the clock, at idle priority), slice settled at ${w1.budget}`);
  ok(w1 && w1.maxSliceMs < 250, `no index slice hogs a core (longest ${w1?.maxSliceMs?.toFixed(1)} ms of CPU)`);
  const W = await getJ(`${A.H}/vibeserver/gallery/navtex-warnings.json`);
  ok(W.j.pending === 0 && W.j.indexed === N, `the FIRST warnings request: pending ${W.j.pending}, indexed ${W.j.indexed}/${N} — no "Building" (${W.ms.toFixed(0)} ms)`);
  ok(Array.isArray(W.j.warnings) && W.j.warnings.length > 0, `…with ${W.j.warnings?.length} warnings drawn`);
  const M = await getJ(`${A.H}/vibeserver/gallery/navtex-met.json`);
  ok(M.j.pending === 0 && M.j.indexed === N, `navtex-met.json: pending ${M.j.pending} (${M.ms.toFixed(0)} ms)`);
  const S = await getJ(`${A.H}/vibeserver/gallery/navtex-stations.json?since=0`);
  ok(S.h.get('x-navtex-cache') === 'hit' && S.j.stations?.length > 0, `the stations map from the warm cache: ${S.h.get('x-navtex-cache')}, ${S.j.stations?.length} stations (${S.ms.toFixed(0)} ms)`);
  const S1 = await getJ(`${A.H}/vibeserver/gallery/navtex-stations.json?since=86400`);
  ok(S1.h.get('x-navtex-cache') === 'miss', '24 H, never asked for: scanned on request (a miss), as before');
  const S2 = await getJ(`${A.H}/vibeserver/gallery/navtex-stations.json?since=86400`);
  ok(S2.h.get('x-navtex-cache') === 'hit' && JSON.stringify(S2.j.stations) === JSON.stringify(S1.j.stations), '…then from the cache, the same answer');
  ok(idxLines(dA).length === N, `the index: ${idxLines(dA).length} lines, one a message`);
  // ★ a message saved later: indexed and the caches re-warmed, still with no map request
  const before = (await warm(A.H)).parsed;
  const c = copy(N);
  fs.writeFileSync(path.join(dA, 'data/gallery', c.f), c.text, 'latin1');
  fs.appendFileSync(path.join(dA, 'data/gallery/index.jsonl'), c.line + '\n');
  const w2 = await until(async () => { const w = await warm(A.H); return w.state === 'idle' && w.parsed === before + 1 ? w : null; }, 30_000, 300);
  ok(!!w2, 'a message saved later: indexed by the warm-up alone (one more parsed)');
  const S3 = await getJ(`${A.H}/vibeserver/gallery/navtex-stations.json?since=0`);
  ok(S3.h.get('x-navtex-cache') === 'hit' && S3.j.messages === S.j.messages + 1, `…the stations map re-warmed: a hit with ${S3.j.messages} messages`);
  const S4 = await getJ(`${A.H}/vibeserver/gallery/navtex-stations.json?since=86400`);
  ok(S4.h.get('x-navtex-cache') === 'hit', '…and the 24 H range a client asked for, re-warmed too');
  const W2 = await getJ(`${A.H}/vibeserver/gallery/navtex-warnings.json`);
  ok(W2.j.pending === 0 && W2.j.indexed === N + 1, `…the warnings whole at once: indexed ${W2.j.indexed}`);
  await stop(A);

  // ── 2. NO SPARE CPU: the on-request build, exactly as before; then both together ─────────────────────────────────────
  console.log('── 2. no spare CPU (the gate held shut): opening the map builds it, as before ──');
  const dB = makeGallery('b', N);
  const busy = path.join(dir, 'busy');
  fs.writeFileSync(busy, '1');
  const B = await startServer(dB, { VIBESERVER_NAVWARM_TEST_BUSY: busy });
  await sleep(5000);
  const wb = await warm(B.H);
  ok(wb.state === 'paused-test' && wb.passes === 0 && idxLines(dB).length === 0, `the warm-up held: ${wb.state}, ${wb.passes} passes, nothing indexed`);
  const r1 = (await getJ(`${B.H}/vibeserver/gallery/navtex-warnings.json`)).j;
  ok(r1.indexed === 400 && r1.pending === N - 400, `request 1: indexed ${r1.indexed}, pending ${r1.pending} — the map says "Building … ${Math.round(100 * r1.indexed / N)} %"`);
  const r2 = (await getJ(`${B.H}/vibeserver/gallery/navtex-warnings.json`)).j;
  ok(r2.indexed === 800 && r2.pending === N - 800, `request 2: indexed ${r2.indexed}, pending ${r2.pending}`);
  // ★ the gate opens mid-backfill and the map keeps asking: both build the one index
  fs.rmSync(busy);
  let reqs = 2, last = r2;
  const both = await until(async () => {
    last = (await getJ(`${B.H}/vibeserver/gallery/navtex-warnings.json`)).j; reqs++;
    return last.pending === 0 ? last : null;
  }, 400_000, 300);
  ok(!!both && both.indexed === N, `the gate opened: warm-up + requests together finished (${reqs} requests, indexed ${last.indexed})`);
  const lines = idxLines(dB), names = new Set(lines.map((l) => l.split('\t')[1]));
  ok(lines.length === N && names.size === N, `every message indexed ONCE: ${lines.length} lines, ${names.size} names`);
  const wb2 = await until(async () => { const w = await warm(B.H); return w.state === 'idle' ? w : null; }, 30_000, 300);
  ok(!!wb2 && wb2.parsed > 0 && wb2.parsed < N - 800, `…the warm-up did its share and stopped (${wb2?.parsed} of ${N - 800})`);
  await stop(B);

  // ── 3. OFF ──────────────────────────────────────────────────────────────────────────────────────────────────────────
  console.log('── 3. the warm-up switched off ──');
  const dC = makeGallery('c', 500);
  const C = await startServer(dC, { VIBESERVER_NAVWARM: '0' });
  await sleep(4000);
  const wc = await warm(C.H);
  ok(wc.state === 'off' && wc.passes === 0, `state ${wc.state}, ${wc.passes} passes`);
  const rc = (await getJ(`${C.H}/vibeserver/gallery/navtex-warnings.json`)).j;
  const rc2 = (await getJ(`${C.H}/vibeserver/gallery/navtex-warnings.json`)).j;
  ok(rc.indexed === 400 && rc.pending === 100 && rc2.pending === 0 && rc2.indexed === 500, 'the on-request build alone: 400, then the last 100');
  await stop(C);
} catch (e) { fail++; console.log('   FAIL', e?.stack || e); }
finally {
  clearTimeout(watchdog);
  for (const p of procs) { try { p.kill('SIGKILL'); } catch { /* gone */ } }
  await sleep(300);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
