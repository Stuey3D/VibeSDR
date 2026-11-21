/**
 * test_whole_mode_resync.ts — WHOLE-RADIO MODES HAND THE CLIENT BACK THE SERVER'S GEOMETRY (Stuart, 2026-11-21, beta89).
 *
 *   • app 622 / iOS 577 on his VibeServer: EXIT ADS-B, open SATELLITE and pick a satellite — audio fine, the spectrum and
 *     waterfall BLANK until one tune click ("can trigger it every time");
 *   • Mac app, V4L server: after ADS-B, DAB on 11A — the spectrum scale right (216.0–217.8 MHz) but the readout still
 *     1090000 kHz and the waterfall's scale 1.09G.
 *
 * Drives the app's REAL client (VibeServerClient, bundled with React Native stubbed) against a REAL vibeserver on a fake
 * rtl_tcp, through every pair of whole-radio modes, and after each step asserts what the screen would draw:
 *   • the readout (status.frequency) is the server's dial (its config's vfo);
 *   • the frames are accepted for drawing: their own centre/span overlap the VIEW the client emits (WaterfallView places a
 *     row by trueCenterHz/trueBwHz under centerHz/bwHz — utils/wfRowPlace.ts rowViewOffset; no overlap = a blank screen);
 *   • the view is the server's (no stale centre held for ever).
 * Silent: no audio socket is opened (the native audio module is a stub).
 * Run: VIBESERVER_BIN=…/vibeserver node --no-warnings scripts/test_whole_mode_resync.ts
 */
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { spawn, type ChildProcess } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import * as esbuild from 'esbuild';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const RATE = 2_400_000, PASS = 'resync-test-admin';
let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const until = async (f: () => boolean, ms: number) => { const end = Date.now() + ms; while (Date.now() < end) { if (f()) return true; await sleep(50); } return f(); };
const freePort = () => new Promise<number>((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = (s.address() as net.AddressInfo).port; s.close(() => res(p)); }); });

// ── the app client, bundled with React Native stubbed (test_span_stamp.ts's stub) ──
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'whole-resync-'));
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
  // ★ NativeModules is EMPTY here (no VibePowerModule, no native spectrum socket) — the JS WebSocket path, as on iOS.
  b.onLoad({ filter: /.*/, namespace: 'stub' }, () => ({ loader: 'js', contents: `
    const fn = function () {};
    const P = new Proxy(fn, { get: (t, k) => k === 'OS' ? 'ios' : k === '__esModule' || k === 'then' ? undefined
                                : k === Symbol.toPrimitive ? () => '' : P, apply: () => Q, construct: () => P });
    const Q = new Proxy(fn, { get: (t, k) => k === 'then' ? (res) => res(null) : k === Symbol.toPrimitive ? () => '' : P,
                              apply: () => Q, construct: () => P });
    const names = ['Platform','NativeEventEmitter','DeviceEventEmitter','AppState','Dimensions','Linking',
                   'Alert','PixelRatio','StyleSheet','TurboModuleRegistry','Settings','AccessibilityInfo','Vibration','Keyboard'];
    const t = {}; for (const n of names) t[n] = P; t.NativeModules = {}; t.default = P;
    module.exports = new Proxy(t, { get: (o, k) => k in o ? o[k] : k === '__esModule' || k === 'then' ? undefined : P });` }));
} };
await esbuild.build({ entryPoints: [path.join(root, 'src/services/VibeServerClient.ts')], bundle: true, format: 'esm', platform: 'node',
  outfile: path.join(tmp, 'app.mjs'), logLevel: 'error', plugins: [stub], define: { __DEV__: 'false' } });
if (process.env.RESYNC_DBG) {
  // ★ debug only: every config the server sends, as the client receives it
  const W = globalThis.WebSocket;
  (globalThis as Record<string, unknown>).WebSocket = class extends W {
    constructor(u: string, p?: string | string[]) {
      super(u, p);
      this.addEventListener('message', (ev: MessageEvent) => {
        if (typeof ev.data !== 'string' || !ev.data.includes('"type":"config"')) return;
        try { const m = JSON.parse(ev.data); console.log(`      [cfg ${Date.now() % 100000}] centre ${m.centerFreq} vfo ${m.vfo} bb ${m.binBandwidth} shared ${m.shared}`); } catch {}
      });
    }
    send(d: string) { if (!/"type":"(ping|set_rate|sat_list)"/.test(String(d))) console.log(`      [send ${Date.now() % 100000}] ${String(d).slice(0, 160)}`); super.send(d); }
  };
}
// ── the WEB client (web/client/src/spectrum.ts): a whole-radio request inside a tune's echo window ──
{
  console.log('── the web client ──');
  const g = globalThis as Record<string, unknown>;
  g.window ??= globalThis;
  g.location ??= { protocol: 'http:', host: 'x', search: '', href: 'http://x/' };
  await esbuild.build({ entryPoints: [path.join(root, 'web/client/src/spectrum.ts')], bundle: true, format: 'esm', platform: 'neutral',
    outfile: path.join(tmp, 'spectrum.mjs'), logLevel: 'error' });
  const { SpectrumClient } = await import(pathToFileURL(path.join(tmp, 'spectrum.mjs')).href);
  const sent: Array<Record<string, unknown>> = [];
  const sc = new SpectrumClient('ws://x/ws', {} as never) as Record<string, any>;
  sc._send = (m: Record<string, unknown>) => { sent.push(m); };
  const cfg = (centre: number, vfo: number, mode: string) => sc._handleText(JSON.stringify({ type: 'config', centerFreq: centre, binCount: 1024,
    binBandwidth: 2343.75, totalBandwidth: 2_400_000, maxBandwidth: 2_400_000, vfo, mode, shared: false }));
  cfg(96_000_000, 96_000_000, 'wfm');
  await sleep(1700);
  for (const [what, enter, hz, mode] of [
    ['DAB 11A', () => sc.dab(true, 26), 216_928_000, 'wfm'],
    ['ADS-B', () => sc.adsb(true), 1_090_000_000, 'wfm'],
    ['a SATELLITE pick', () => sc.satPick(25544, 0, true), 437_800_000, 'nfm'],
  ] as Array<[string, () => void, number, string]>) {
    if (what === 'ADS-B') sc.dab(false);
    if (what === 'a SATELLITE pick') sc.adsb(false);
    await sleep(1700);
    sc.tune(97_000_000, 'wfm');       // ★ a person's tune …
    enter();                           // ★ … and at once a request that hands the dial to the server
    cfg(hz, hz, mode);                 // the server's config, sent once, inside the tune's 1.5 s echo window
    ok(Math.abs(sc.frequency - hz) < 1, `web: a tune then ${what} at once — the READOUT is the server's dial (${sc.frequency})`);
    await sleep(400);
    ok(Math.abs(sc.view.centerHz - hz) < 1, `web: …and the view is the server's capture (${sc.view.centerHz})`);
  }
}
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   the server half not run — set VIBESERVER_BIN');
  console.log(`\n${pass} passed, ${fail} failed`);
  fs.rmSync(tmp, { recursive: true, force: true });
  process.exit(fail ? 1 : 3);
}

const mod = await import(pathToFileURL(path.join(tmp, 'app.mjs')).href);
const Cls = mod.VibeServerClient ?? mod.default;

// ── a real server on a fake rtl_tcp, satellites in its cache (test-server-sat.mjs's fixture, epochs an hour ago) ──
function freshSats(): string {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-6digit-2026-11-19.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  return JSON.stringify(d);
}
async function server(tag: string, radio: Record<string, unknown>) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run', 'data/sat']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
  const t = Math.floor(Date.now() / 1000);
  fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), `checked ${t}\nattempt ${t}\nseed 0\n`);   // ★ no network
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Resync test', locator: 'IO92ng', adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', driver: 'rtlsdr', enabled: true, configured: true, users: 4, ...radio }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const procs: ChildProcess[] = [];
  procs.push(spawn('nice', ['-n', '10', process.execPath, '--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' }));
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  procs.push(spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '96000000', '--mode', 'wfm',
                           '--users', String(radio.users ?? 4), '--admin-pass', PASS], { env, stdio: ['ignore', log, log] }));
  const up = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(2000) }); return r.ok; } catch { return false; } };
  for (let i = 0; i < 150 && !(await up()); i++) await sleep(100);
  return { port, dir, log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } },
           stop: () => { for (const p of procs) { try { p.kill('SIGTERM'); } catch {} } try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} } };
}

type Emit = { centerHz: number; bwHz: number; trueCenterHz?: number; trueBwHz?: number; frequency: number; at: number; n: number };
/** One app client + what the screen would have drawn. */
function appClient(port: number, id: string) {
  const emits: Emit[] = [];
  const texts: Record<string, unknown>[] = [];
  let sat: Record<string, unknown> | null = null;
  const cb = new Proxy({
    onSpectrum: (b: Float32Array, st: Record<string, number>) => emits.push({ centerHz: st.centerHz, bwHz: st.bwHz, trueCenterHz: st.trueCenterHz,
      trueBwHz: st.trueBwHz, frequency: st.frequency, at: Date.now(), n: b.length }),
    onSatList: (l: unknown) => { sat = l as Record<string, unknown>; },
    onDbg: (m: string) => { if (process.env.RESYNC_DBG && !/^(config:|sig|adc)/.test(m)) console.log(`      [dbg ${Date.now() % 100000}] ${m.slice(0, 200)}`); },
  } as Record<string, unknown>, { get: (t, k) => (k in t ? t[k as string] : () => {}) });
  const c = new Cls(`http://127.0.0.1:${port}`, id, cb) as Record<string, any>;
  // ★ every JSON the server sent, beside the client's own handling (the server's config is the truth we compare with)
  const orig = c.handleServerMessage.bind(c);
  c.handleServerMessage = (m: Record<string, unknown>) => { texts.push(m); return orig(m); };
  return { c, emits, texts, satList: () => sat };
}

/** What the screen draws from the last second of frames: each row placed under the view it was emitted with. */
function drawn(e: Emit[], since: number) {
  const rows = e.filter((x) => x.at >= since);
  const placed = rows.filter((x) => {
    const rc = x.trueCenterHz ?? x.centerHz, rbw = (x.trueBwHz && x.trueBwHz > 0) ? x.trueBwHz : x.bwHz;
    if (!(rc > 0 && rbw > 0 && x.centerHz > 0 && x.bwHz > 0)) return false;
    // ★ rowViewOffset: same Hz a bin, and an overlap (|offset| < n) — else the row has no place in the view
    if (Math.abs(rbw - x.bwHz) > x.bwHz * 1e-6) return false;
    return Math.abs(rc - x.centerHz) < x.bwHz;
  });
  return { rows: rows.length, placed: placed.length, last: rows.at(-1) };
}

async function check(label: string, A: ReturnType<typeof appClient>, srvFreq: () => number, wantHz: number, tolHz: number) {
  // ★ 3 s: long enough for every settle and the stamp heal (1 s), short of anything a person would call "after a while".
  //   ★ ADS-B runs the waterfall at a crawl behind the map (recomputeEngineRate): its rows are read over 2 s, at least one.
  const adsb = Math.abs(wantHz - 1090e6) < 1e6;
  await sleep(adsb ? 4000 : 3000);
  const since = Date.now() - (adsb ? 2000 : 1000);
  const d = drawn(A.emits, since);
  const st = A.c.status;
  const vfo = srvFreq();
  ok(Math.abs(vfo - wantHz) < tolHz, `${label}: the server's dial is where the mode put it (${(vfo / 1e6).toFixed(3)} MHz)`);
  ok(Math.abs(Number(st.frequency) - vfo) < 2000, `${label}: the READOUT is the server's dial (${(Number(st.frequency) / 1e6).toFixed(3)} vs ${(vfo / 1e6).toFixed(3)} MHz)`);
  ok(d.rows >= (adsb ? 1 : 4) && d.placed === d.rows, `${label}: rows arrive and every one is drawn in the view (${d.placed}/${d.rows})`);
  const l = d.last;
  ok(!!l && Math.abs(l.centerHz - Number(st.centerHz)) < 1 && Math.abs((l.trueCenterHz ?? 0) - Number(st.centerHz)) < 1,
     `${label}: the view is the server's capture (view ${(Number(l?.centerHz) / 1e6).toFixed(3)}, rows ${(Number(l?.trueCenterHz) / 1e6).toFixed(3)}, server ${(Number(st.centerHz) / 1e6).toFixed(3)} MHz)`);
  ok(!!l && Math.abs(Number(l.centerHz) - vfo) < Number(l.bwHz) / 2, `${label}: the dial is inside the drawn view`);
}

const procsToStop: Array<() => void> = [];
let A: ReturnType<typeof appClient> | null = null;
try {
  for (const shared of (process.env.RESYNC_ONLY === 'own' ? [false] : process.env.RESYNC_ONLY === 'shared' ? [true] : [true, false])) {
    console.log(`── ${shared ? 'a shared dial (4 listeners)' : 'a one-listener radio'} ──`);
    const S = await server(shared ? 'rsshared' : 'rsown', { users: shared ? 4 : 1 });
    procsToStop.push(S.stop);
    A = appClient(S.port, `resync${shared ? 's' : 'o'}000000001`);
    await A.c.connect(96_000_000, 'wfm');
    ok(await until(() => A!.emits.length > 5, 8000), 'connected: frames arrive');
    const srvVfo = () => Number(A!.c.status.serverDial?.frequency ?? 0);

    // where ISS is in the list (the pick needs norad + transmitter index)
    A.c.satList();
    await until(() => !!A!.satList(), 4000);
    const L = A.satList() as { sats?: Array<{ id?: string; norad?: number; tx?: Array<{ hz?: number }> }> } | null;
    const iss = L?.sats?.find((s) => s.id === 'ISS');
    const issTx = iss?.tx?.findIndex((t) => Math.abs(Number(t.hz) - 437.8e6) < 50e3) ?? -1;
    ok(!!iss && issTx >= 0, `ISS in the satellite list (norad ${iss?.norad}, tx ${issTx})`);

    const steps: Array<[string, () => void, number]> = [
      ['ADS-B', () => A!.c.adsb(true), 1090e6],
      ['ADS-B → normal', () => A!.c.adsb(false), 96e6],
      ['ADS-B again', () => A!.c.adsb(true), 1090e6],
      ['ADS-B → (exit) SATELLITE pick', () => { A!.c.adsb(false); setTimeout(() => A!.c.satPick(iss!.norad, issTx, true), 400); }, 437.8e6],
      ['SATELLITE → ADS-B', () => { A!.c.satDrop(); A!.c.adsb(true); }, 1090e6],
      ['ADS-B → DAB 11A', () => A!.c.dab(true, 26), 216.928e6],
      ['DAB → ADS-B', () => A!.c.adsb(true), 1090e6],
      ['ADS-B → AIS', () => A!.c.ais(true), 162e6],
      ['AIS → DAB 11A', () => A!.c.dab(true, 26), 216.928e6],
      ['DAB → normal', () => A!.c.dab(false), 0],
      // ★ a person's tune and, inside its 1.5 s echo window, a whole-radio mode: the mode's dial must still be adopted
      ['a tune, then DAB 11A 300 ms later', () => { A!.c.tune(97_000_000, 'wfm'); setTimeout(() => A!.c.dab(true, 26), 300); }, 216.928e6],
      ['DAB → normal', () => A!.c.dab(false), 0],
      ['ISM', () => A!.c.ism(true), 0],
      ['ISM → (exit) SATELLITE pick', () => { A!.c.ism(false); setTimeout(() => A!.c.satPick(iss!.norad, issTx, true), 400); }, 437.8e6],
      ['SATELLITE → normal', () => A!.c.satDrop(), 0],
    ];
    for (const [label, act, hz] of steps.slice(0, Number(process.env.RESYNC_STEPS) || steps.length)) {
      if (process.env.RESYNC_DBG) console.log(`   >> ${label}`);
      act();
      // ★ 0 = "wherever the server put it" — the readout and the view are still checked against the server's own dial
      await check(label, A, srvVfo, hz || srvVfo(), hz ? 1.5e6 : 1e12);
    }
    A.c.disconnect?.();
    A.c.destroyed = true;
    S.stop();
  }
} catch (e) {
  ok(false, `harness threw: ${(e as Error).stack}`);
} finally {
  for (const s of procsToStop) s();
  fs.rmSync(tmp, { recursive: true, force: true });
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
