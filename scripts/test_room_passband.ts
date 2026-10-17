/**
 * THE ROOM'S PASSBAND ON A SHARED DIAL (src/services/roomPassband.ts, 2026-10-17).
 *
 * Stuart: "on shared VFO, if a user changes the bandwidth sliders I am hearing them but they aren't
 * synchronising with mine." The server applied the passband to the one shared demodulator and `config`
 * never carried it, and both clients adopted the room's mode only with a move of the dial.
 *
 * Proves:
 *   • the rule: adopt the room's edges / mode on a shared dial only; never inside our own echo (1.5 s);
 *     never one mode's edges drawn on another; nothing when the server says nothing;
 *   • the WEB client end to end: a real SpectrumClient fed the server's config adopts the edges and the
 *     mode, tells the page, and SENDS NOTHING (the contract: adopting is never a user action); a drag of
 *     its own is not undone by the configs answering it;
 *   • the app and Jr read the same fields and never adopt through their sending setters.
 * The server half (vibe_room_passband.h) is proved by vibeserver/test-room-passband.cpp.
 *
 * Run: node --no-warnings scripts/test_room_passband.ts
 */
import { readFileSync, mkdtempSync, rmSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import * as esbuild from 'esbuild';
import { roomMode, roomPassband, ROOM_SETTLE_MS } from '../src/services/roomPassband.ts';

let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const src = (p: string) => readFileSync(new URL(`../${p}`, import.meta.url), 'utf8');

// ── 1. the rule ──
const ctx = { shared: true, mode: 'usb', low: 50, high: 2700, sinceLocalMs: 60_000 };
const cfg = (o: Record<string, unknown>) => ({ type: 'config', mode: 'usb', vfo: 7_074_000, shared: true, ...o });

ok(JSON.stringify(roomPassband(cfg({ bandwidthLow: 100, bandwidthHigh: 3000 }), ctx)) === '[100,3000]',
   'another listener widened USB: the edges are adopted');
ok(roomPassband(cfg({ bandwidthLow: 50, bandwidthHigh: 2700 }), ctx) === null, 'already showing them: nothing to do');
ok(roomPassband(cfg({}), ctx) === null, 'the server says nothing (nobody chose one): keep the default we draw');
ok(roomPassband(cfg({ bandwidthLow: 100, bandwidthHigh: 3000 }), { ...ctx, shared: false }) === null,
   'not a shared dial: our own passband is the answer');
ok(roomPassband(cfg({ bandwidthLow: 100, bandwidthHigh: 3000 }), { ...ctx, sinceLocalMs: ROOM_SETTLE_MS - 1 }) === null,
   'inside our own echo window: not adopted (a drag is not yanked back)');
ok(roomPassband(cfg({ mode: 'lsb', bandwidthLow: -3000, bandwidthHigh: -100 }), ctx) === null,
   "another mode's edges are never drawn on ours");
ok(roomPassband(cfg({ bandwidthLow: 3000, bandwidthHigh: 100 }), ctx) === null, 'an inverted passband is refused');
ok(roomPassband(cfg({ bandwidthLow: '100', bandwidthHigh: 3000 }), ctx) === null, 'a non-number edge is refused');
ok(roomMode(cfg({ mode: 'lsb' }), ctx) === 'lsb', 'the room switched USB -> LSB on the same frequency: adopted');
ok(roomMode(cfg({ mode: 'USB' }), ctx) === null, 'the same mode (any case) is not a change');
ok(roomMode(cfg({ mode: 'lsb' }), { ...ctx, sinceLocalMs: 100 }) === null, 'a mode inside our own echo window is not adopted');
ok(roomMode(cfg({ mode: 'lsb' }), { ...ctx, shared: false }) === null, 'not a shared dial: our own mode stands');

// ── 2. the web client, end to end ──
// SpectrumClient is a browser module; give it just enough of a browser to construct (no socket is opened).
const g = globalThis as Record<string, unknown>;
g.window ??= globalThis;
g.location ??= { protocol: 'http:', host: 'x', search: '', href: 'http://x/' };
// ★ Bundled first: the web client imports without extensions (it is built by esbuild), which node cannot resolve.
const tmp = mkdtempSync(path.join(os.tmpdir(), 'room-pb-'));
await esbuild.build({ entryPoints: [fileURLToPath(new URL('../web/client/src/spectrum.ts', import.meta.url))],
  bundle: true, format: 'esm', platform: 'neutral', outfile: path.join(tmp, 'spectrum.mjs'), logLevel: 'error' });
const { SpectrumClient } = await import(pathToFileURL(path.join(tmp, 'spectrum.mjs')).href);
const sent: unknown[] = [];
let told = 0, toldMode: string | undefined;
const sc = new SpectrumClient('ws://x/ws', {
  onRoomPassband: (m?: string) => { told++; toldMode = m; },
} as never) as unknown as Record<string, any>;
sc._send = (m: unknown) => { sent.push(m); };
const feed = (o: Record<string, unknown>) => sc._handleText(JSON.stringify({
  type: 'config', centerFreq: 7_074_000, binCount: 1024, binBandwidth: 100, totalBandwidth: 102_400,
  maxBandwidth: 2_400_000, vfo: 7_074_000, locked: false, shared: true, mode: 'usb', ...o }));
sc.frequency = 7_074_000; sc.mode = 'usb'; sc.bandwidthLow = 50; sc.bandwidthHigh = 2700;

feed({ bandwidthLow: 100, bandwidthHigh: 3000 });
ok(sc.bandwidthLow === 100 && sc.bandwidthHigh === 3000, 'web: another listener moved the passband — ours follows');
ok(told === 1 && toldMode === undefined, 'web: the page is told to redraw its sliders (no mode change)');
ok(sent.length === 0, 'web: adopting sent NOTHING back to the server');

feed({ mode: 'lsb' });
ok(sc.mode === 'lsb' && sc.bandwidthLow === -2700 && sc.bandwidthHigh === -50,
   "web: the room switched to LSB on the same frequency — mode and LSB's default follow");
ok(told === 2 && toldMode === 'lsb', 'web: the page is told the mode changed');
feed({ mode: 'lsb', bandwidthLow: -3000, bandwidthHigh: -200 });
ok(sc.bandwidthLow === -3000 && sc.bandwidthHigh === -200, "web: then the room's LSB edges");
ok(sent.length === 0, 'web: still nothing sent');

sc.setBandwidth(-2400, -300);   // our own drag
const afterDrag = sent.length;
feed({ mode: 'lsb', bandwidthLow: -3000, bandwidthHigh: -200 });   // a config answering something else, from before our drag landed
ok(sc.bandwidthLow === -2400 && sc.bandwidthHigh === -300, 'web: our own drag is not undone by a config already on its way');
ok(sent.length === afterDrag, 'web: and that config sent nothing either');

// not shared: nothing
sc.lastLocalBwAt = -Infinity; sc.lastLocalTuneAt = -Infinity;
feed({ shared: false, mode: 'lsb', bandwidthLow: -1000, bandwidthHigh: -100 });
ok(sc.bandwidthLow === -2400, 'web: a radio of our own — the server never overrides our passband');

// ── 3. the app and Jr read the same fields, and adopt without sending ──
const app = src('src/services/VibeServerWsClient.ts');
const adoptApp = app.slice(app.indexOf('const rm = roomMode(msg, ctx)'), app.indexOf('if (this.wantTune) {', app.indexOf('const rm = roomMode(msg, ctx)')));
ok(adoptApp.length > 0 && /roomPassband\(msg/.test(adoptApp), 'app: the config handler applies roomMode + roomPassband');
ok(!/this\.setBandwidth\(|this\.setMode\(|_sendCtl\(|sendBandwidth\(/.test(adoptApp), 'app: the adopt never goes through a sending setter');
ok(/this\.lastLocalBwAt = Date\.now\(\)/.test(app), 'app: setBandwidth stamps its own echo window');
const jr = src('spike/WristSDR/WristSDR/UberClient.swift');
const adoptJr = jr.slice(jr.indexOf('private func adoptRoomPassband'), jr.indexOf('\n  }\n', jr.indexOf('private func adoptRoomPassband')));
ok(/j\["bandwidthLow"\]/.test(adoptJr) && /j\["bandwidthHigh"\]/.test(adoptJr), 'Jr: reads the same config fields');
ok(!/setBandwidth\(|setMode\(|sendTune\(|\.send\(/.test(adoptJr), 'Jr: the adopt never goes through a sending setter');
ok(/lastLocalBwAt = ProcessInfo/.test(jr), 'Jr: setBandwidth stamps its own echo window');

rmSync(tmp, { recursive: true, force: true });
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
