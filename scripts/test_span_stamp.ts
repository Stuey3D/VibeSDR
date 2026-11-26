/**
 * test_span_stamp.ts — V12 beta83 THE SPAN STAMP, in the clients (NickB, HF+ Dual Port, 2026-11-18: after a keypad jump
 * 7285 → 585 kHz the waterfall's span stayed wrong until he tuned or zoomed; Stuart: "it never seems to recover").
 * A SPEC frame said the CENTRE its bins were built for but never the SPAN, so rows at another span than the last config
 * were drawn at the wrong scale for good. A socket that asks `&span=1` now gets version-2 frames: an f64 Hz per bin at 22,
 * the body at 30 (server half: scripts/test-server-span-stamp.mjs).
 *
 * Proves, on the REAL app client (VibeServerWsClient, bundled with React Native stubbed) and the REAL web client:
 *   • a v2 frame's bins are read from 30 (not 8 bytes of header drawn as signal) and its rows are placed by ITS span at once;
 *   • a stale config — the frames at another span with nothing in flight — is healed after a second: the view settles
 *     onto the frames' span (app: status + view; web: cfg + view);
 *   • NOT while a zoom of ours is in flight (a slow link's old-span frames must not bounce the zoom back), and not for a
 *     stamp that agrees with the config;
 *   • a v1 frame (an older server) is exactly as before: the config's span, the body at 22;
 *   • the app and the web page ask with `span=1`.
 * Run: node --no-warnings scripts/test_span_stamp.ts
 */
import { readFileSync, mkdtempSync, rmSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import * as esbuild from 'esbuild';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const src = (p: string) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');

/** One SPEC frame, full u8: v1 (22-byte header) or v2 (the stamp at 22, body at 30). */
function frame(centre: number, bins: Uint8Array, binHz?: number): ArrayBuffer {
  const h = binHz === undefined ? 22 : 30;
  const b = new ArrayBuffer(h + bins.length), dv = new DataView(b), u = new Uint8Array(b);
  u.set([0x53, 0x50, 0x45, 0x43, binHz === undefined ? 1 : 2, 0x03]);
  dv.setBigUint64(6, 0n, true);
  dv.setBigUint64(14, BigInt(Math.round(centre)), true);
  if (binHz !== undefined) dv.setFloat64(22, binHz, true);
  u.set(bins, h);
  return b;
}
const N = 1024;
const bins = new Uint8Array(N).fill(100); bins[0] = 200;   // DC (wire bin 0) is the one bright bin → display index N/2

const g = globalThis as Record<string, unknown>;
g.window ??= globalThis;
g.location ??= { protocol: 'http:', host: 'x', search: '', href: 'http://x/' };
const tmp = mkdtempSync(path.join(os.tmpdir(), 'span-stamp-'));

// ── 1. the app client ──
{
  const stub: esbuild.Plugin = { name: 'rn-stub', setup(b) {
    b.onResolve({ filter: /^@react-native-async-storage\/async-storage$/ }, (a) => ({ path: a.path, namespace: 'store' }));
    b.onLoad({ filter: /.*/, namespace: 'store' }, () => ({ loader: 'js', contents: `
      const m = new Map();
      const S = { getItem: async (k) => m.has(k) ? m.get(k) : null, setItem: async (k, v) => { m.set(k, v); },
                  removeItem: async (k) => { m.delete(k); }, multiGet: async (ks) => ks.map((k) => [k, m.has(k) ? m.get(k) : null]),
                  multiSet: async (kv) => { for (const [k, v] of kv) m.set(k, v); }, multiRemove: async (ks) => { for (const k of ks) m.delete(k); },
                  getAllKeys: async () => [...m.keys()], clear: async () => m.clear() };
      module.exports = { ...S, default: S };` }));
    b.onResolve({ filter: /^(react-native|@react-native|expo|react$|@expo|react-native-.*)/ }, (a) => ({ path: a.path, namespace: 'stub' }));
    b.onLoad({ filter: /.*/, namespace: 'stub' }, () => ({ loader: 'js', contents: `
      // ★ Everything is a callable proxy; a CALL returns a thenable one (AsyncStorage awaits, a listener's .remove() works).
      const fn = function () {};
      const P = new Proxy(fn, { get: (t, k) => k === 'OS' ? 'ios' : k === '__esModule' || k === 'then' ? undefined
                                  : k === Symbol.toPrimitive ? () => '' : P, apply: () => Q, construct: () => P });
      const Q = new Proxy(fn, { get: (t, k) => k === 'then' ? (res) => res(null) : k === Symbol.toPrimitive ? () => '' : P,
                                apply: () => Q, construct: () => P });
      // ★ esbuild's interop copies the stub's OWN keys (and its default is the whole module), so the names are listed.
      const names = ['NativeModules','Platform','NativeEventEmitter','DeviceEventEmitter','AppState','Dimensions','Linking',
                     'Alert','PixelRatio','StyleSheet','TurboModuleRegistry','Settings','AccessibilityInfo','Vibration','Keyboard'];
      const t = {}; for (const n of names) t[n] = P; t.default = P;
      module.exports = new Proxy(t, { get: (o, k) => k in o ? o[k] : k === '__esModule' || k === 'then' ? undefined : P });` }));
  } };
  await esbuild.build({ entryPoints: [fileURLToPath(new URL('../src/services/VibeServerWsClient.ts', import.meta.url))],
    bundle: true, format: 'esm', platform: 'node', outfile: path.join(tmp, 'app.mjs'), logLevel: 'error', plugins: [stub],
    define: { __DEV__: 'false' } });
  const mod = await import(pathToFileURL(path.join(tmp, 'app.mjs')).href);
  const Cls = mod.VibeServerWsClient ?? mod.default;
  const emits: Array<Record<string, number>> = [];
  const lastBins: Float32Array[] = [];
  const cb = new Proxy({ onSpectrum: (b: Float32Array, st: Record<string, number>) => { emits.push(st); lastBins.push(b.slice()); } } as Record<string, unknown>,
    { get: (t, k) => (k in t ? t[k as string] : () => {}) });
  const c = new Cls('http://127.0.0.1:1', 'span-stamp-test', cb) as Record<string, any>;
  const s = c.status;
  // The state after a config: 1024 bins at 2343.75 Hz (2.4 MHz) round 585 kHz, view settled on it.
  const CFG = 2_400_000 / N, TRUE = 1_200_000 / N;   // the frames are really at HALF that span
  s.binBandwidth = CFG; s.binCount = N; c.bins = new Float32Array(N);
  c.view.centerHz = 585_000; c.view.binBandwidth = CFG;
  const feed = (f: ArrayBuffer) => c._parseBinaryFrame(f);

  feed(frame(585_000, bins));
  let e = emits.at(-1)!;
  ok(!!e && Math.abs(e.trueBwHz - CFG * N) < 1e-6, `v1 frame: the rows take the config's span (${e && e.trueBwHz})`);
  ok(lastBins.at(-1)![N / 2] === 200 - 256 && lastBins.at(-1)![0] === 100 - 256, 'v1 frame: the body from 22 (DC bright, in the middle)');

  feed(frame(585_000, bins, TRUE));
  e = emits.at(-1)!;
  ok(Math.abs(e.trueBwHz - TRUE * N) < 1e-6, `v2 frame: the rows take THE FRAME'S span at once (${e.trueBwHz} = ${TRUE * N})`);
  ok(lastBins.at(-1)![N / 2] === 200 - 256 && lastBins.at(-1)!.every((v: number, i: number) => i === N / 2 || v === 100 - 256),
     'v2 frame: the body from 30 — the stamp is not drawn as signal');
  ok(s.binBandwidth === CFG, 'a fresh disagreement does not move the view yet');
  for (let t = 0; t < 12; t++) { await sleep(100); feed(frame(585_000, bins, TRUE)); }
  ok(Math.abs(s.binBandwidth - TRUE) < 1e-9, `after a second of it, nothing in flight: the config is stale and the frames' span wins (${s.binBandwidth})`);
  await sleep(400); feed(frame(585_000, bins, TRUE));
  ok(Math.abs(c.view.binBandwidth - TRUE) < 1e-9 && Math.abs(emits.at(-1)!.bwHz - TRUE * N) < 1e-3,
     `…and the view settles onto it (view ${c.view.binBandwidth}, drawn span ${emits.at(-1)!.bwHz})`);

  // A zoom of ours in flight: frames at the old span keep coming (a slow link) — no heal, no bounce.
  s.binBandwidth = CFG; c.view.binBandwidth = CFG;
  c.pendingView = { centerHz: 585_000, binBandwidth: CFG };   // as _queueView leaves it while a send is due
  for (let t = 0; t < 15; t++) { await sleep(100); feed(frame(585_000, bins, TRUE)); }
  ok(s.binBandwidth === CFG && c.view.binBandwidth === CFG, 'a zoom of ours in flight: old-span frames for 1.5 s move nothing');
  ok(Math.abs(emits.at(-1)!.trueBwHz - TRUE * N) < 1e-6, '…while each row is still placed by its own span');
  c.pendingView = null;
  // A stamp that agrees (the steady state on a healthy server) changes nothing, ever.
  s.binBandwidth = TRUE; c.view.binBandwidth = TRUE;
  const before = emits.length;
  for (let t = 0; t < 12; t++) { await sleep(100); feed(frame(585_000, bins, TRUE * (1 + 1e-7))); }
  ok(s.binBandwidth === TRUE && c.settleTimer === null && emits.length === before + 12, 'a stamp that agrees: no heal, no settle armed');
  // A short v2 frame is a counted fault, not a crash.
  const n0 = emits.length; feed(frame(585_000, new Uint8Array(0), TRUE).slice(0, 26));
  ok(emits.length === n0, 'a v2 frame shorter than its header is dropped');
  c.destroyed = true;
  if (c.settleTimer) clearTimeout(c.settleTimer);
}

// ── 2. the web client ──
{
  await esbuild.build({ entryPoints: [fileURLToPath(new URL('../web/client/src/spectrum.ts', import.meta.url))],
    bundle: true, format: 'esm', platform: 'neutral', outfile: path.join(tmp, 'spectrum.mjs'), logLevel: 'error' });
  const { SpectrumClient } = await import(pathToFileURL(path.join(tmp, 'spectrum.mjs')).href);
  const got: Array<[Float32Array, number, number]> = [];
  const sc = new SpectrumClient('ws://x/ws', { onBins: (b: Float32Array, c: number, bw: number) => got.push([b.slice(), c, bw]) } as never) as Record<string, any>;
  sc._send = () => {};
  const CFG = 2_400_000 / N, TRUE = 1_200_000 / N;
  sc._handleText(JSON.stringify({ type: 'config', centerFreq: 585_000, binCount: N, binBandwidth: CFG, totalBandwidth: 2_400_000, maxBandwidth: 2_400_000, vfo: 585_000, mode: 'am' }));
  sc._handleBinary(frame(585_000, bins));
  ok(got.length === 1 && Math.abs(got[0][2] - CFG * N) < 1e-6 && got[0][0][N / 2] === 200 - 256, 'web v1 frame: the config\'s span, the body from 22');
  sc._handleBinary(frame(585_000, bins, TRUE));
  ok(Math.abs(got.at(-1)![2] - TRUE * N) < 1e-6 && got.at(-1)![0][N / 2] === 200 - 256 && got.at(-1)![0][0] === 100 - 256,
     'web v2 frame: the frame\'s own span, the body from 30');
  // ★ 1.6 s: the config above sent a view of its own (in flight for 300 ms), and the second only starts after it.
  for (let t = 0; t < 16; t++) { await sleep(100); sc._handleBinary(frame(585_000, bins, TRUE)); }
  ok(Math.abs(sc.cfg.binBandwidth - TRUE) < 1e-9 && Math.abs(sc.view.binBandwidth - TRUE) < 1e-9, `web: a stale config healed after a second (${sc.cfg.binBandwidth})`);
}
rmSync(tmp, { recursive: true, force: true });

// ── 3. the wiring ──
ok(/wideSuffix\(this\.baseUrl\)\}&span=1(&[a-z0-9]+=[0-9]+)*`/.test(   // ★ beta96 more asks may follow (&adsbd=1, ADS-B deltas)
  src('src/services/VibeServerWsClient.ts')), 'the app asks with span=1');
ok(/&wide=1&span=1(&[a-z0-9]+=[0-9]+)*'/.test(src('web/client/src/main.ts')), 'the web page asks with span=1');
ok(/it\.binHz \?\? 0/.test(src('src/services/nativeSpecSocket.ts')) && /"binHz", it\.binHz/.test(src('android/app/src/main/cpp/spectrum/spectrum_jsi.cpp')),
   'Android\'s native socket hands the stamp to the client');
ok(!/span=1/.test(src('handheld-ui/src/engine.h')), 'the R36S does not ask (it reads a 22-byte header)');

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
