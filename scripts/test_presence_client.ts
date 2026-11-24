/**
 * test_presence_client.ts — V12 PRESENCE, the clients' half (Stuart, 2026-11-24: a closed app held a slot on the XCover
 * for three minutes; "we need the app to identify to the server when it has been backgrounded"). The server's half is
 * scripts/test-server-presence.mjs.
 *
 * Proves, on the REAL app client (VibeServerClient, bundled with React Native stubbed) and the REAL web client:
 *   • setPresence('background') goes out on the open spectrum socket, once (a repeat says nothing);
 *   • after the spectrum is paused (closed) nothing is sent — and the state is said again on the NEXT socket's first
 *     hwinfo, so a reconnect never loses it (and a second hwinfo on the same socket does not repeat it);
 *   • destroy() still says `leave` (the goodbye an older V12 server knows too);
 *   • the web page's presence / bye never raise "not connected — reconnecting" or wake a closed socket (a hidden tab
 *     must not reopen its socket to say it is hidden).
 * Run: node --no-warnings scripts/test_presence_client.ts
 */
import { mkdtempSync, rmSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import * as esbuild from 'esbuild';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };

const g = globalThis as Record<string, unknown>;
g.window ??= globalThis;
g.location ??= { protocol: 'http:', host: 'x', search: '', href: 'http://x/' };
const tmp = mkdtempSync(path.join(os.tmpdir(), 'presence-'));

/** A spectrum socket that only records what is sent. */
const fakeWs = () => {
  const w = { readyState: 1, sent: [] as string[], send(x: string) { w.sent.push(x); }, close() { w.readyState = 3; } };
  return w;
};
const presences = (w: { sent: string[] }) => w.sent.map((x) => JSON.parse(x)).filter((m) => m.type === 'presence');

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
      const fn = function () {};
      const P = new Proxy(fn, { get: (t, k) => k === 'OS' ? 'ios' : k === '__esModule' || k === 'then' ? undefined
                                  : k === Symbol.toPrimitive ? () => '' : P, apply: () => Q, construct: () => P });
      const Q = new Proxy(fn, { get: (t, k) => k === 'then' ? (res) => res(null) : k === Symbol.toPrimitive ? () => '' : P,
                                apply: () => Q, construct: () => P });
      const names = ['NativeModules','Platform','NativeEventEmitter','DeviceEventEmitter','AppState','Dimensions','Linking',
                     'Alert','PixelRatio','StyleSheet','TurboModuleRegistry','Settings','AccessibilityInfo','Vibration','Keyboard'];
      const t = {}; for (const n of names) t[n] = P; t.default = P;
      module.exports = new Proxy(t, { get: (o, k) => k in o ? o[k] : k === '__esModule' || k === 'then' ? undefined : P });` }));
  } };
  await esbuild.build({ entryPoints: [fileURLToPath(new URL('../src/services/VibeServerClient.ts', import.meta.url))],
    bundle: true, format: 'esm', platform: 'node', outfile: path.join(tmp, 'app.mjs'), logLevel: 'error', plugins: [stub],
    define: { __DEV__: 'false' } });
  const mod = await import(pathToFileURL(path.join(tmp, 'app.mjs')).href);
  const Cls = mod.VibeServerClient ?? mod.default;
  const cb = new Proxy({} as Record<string, unknown>, { get: (t, k) => (k in t ? t[k as string] : () => {}) });
  const c = new Cls('http://127.0.0.1:1', 'presence-test', cb) as Record<string, any>;
  ok(c.isVibe === true, 'the client is a VibeServer client');

  let w = fakeWs();
  c.spectrumWs = w;
  c.setPresence('background');
  ok(presences(w).length === 1 && presences(w)[0].state === 'background', 'background → {"type":"presence","state":"background"} on the open socket');
  c.setPresence('background');
  ok(presences(w).length === 1, 'the same state again says nothing');

  c.pauseSpectrum();
  ok(w.readyState === 3 && c.spectrumWs === null, 'the spectrum paused (its socket closed)');
  c.setPresence('foreground');
  ok(presences(w).length === 1, 'foreground with no socket open: nothing sent (and nothing thrown)');

  // ★ the next socket: its first hwinfo says where the app is
  w = fakeWs();
  c.spectrumWs = w; c.pausedByApp = false; c.hwinfoThisSocket = false;
  try { c._handleSpectrumMessage({ type: 'hwinfo' }); } catch (e) { console.log('   (hwinfo handler threw after presence:', String(e).slice(0, 120), ')'); }
  ok(presences(w).length === 1 && presences(w)[0].state === 'foreground', '★ the next socket\'s first hwinfo says "foreground"');
  try { c._handleSpectrumMessage({ type: 'hwinfo' }); } catch { /* the rest of the handler is not under test */ }
  ok(presences(w).length === 1, 'a second hwinfo on the same socket does not repeat it');

  c.destroy();
  const sent = w.sent.map((x) => JSON.parse(x));
  ok(sent.some((m) => m.type === 'leave'), 'destroy() says `leave` — the goodbye every V12 server knows');
  ok(!sent.some((m) => m.type === 'bye'), '…and not "bye" (only this server version knows that name)');
}

// ── 2. the web client ──
{
  await esbuild.build({ entryPoints: [fileURLToPath(new URL('../web/client/src/spectrum.ts', import.meta.url))],
    bundle: true, format: 'esm', platform: 'neutral', outfile: path.join(tmp, 'spectrum.mjs'), logLevel: 'error' });
  const { SpectrumClient } = await import(pathToFileURL(path.join(tmp, 'spectrum.mjs')).href);
  const statuses: string[] = [];
  const sc = new SpectrumClient('ws://x/ws', { onStatus: (s: string, m: string) => statuses.push(`${s}: ${m}`) } as never) as Record<string, any>;
  sc.ws = null;
  sc.send({ type: 'presence', state: 'background' });
  sc.send({ type: 'bye' });
  ok(statuses.length === 0 && !sc.reconnectTimer, 'web: presence / bye with no socket raise no "not connected" and wake nothing');
  const w = fakeWs();
  sc.ws = w;
  sc.send({ type: 'presence', state: 'foreground' });
  ok(presences(w).length === 1, 'web: presence goes out on an open socket');
  if (sc.reconnectTimer) clearTimeout(sc.reconnectTimer);
}
rmSync(tmp, { recursive: true, force: true });

console.log(`\n${fail ? '✗' : '✓'} test_presence_client: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
