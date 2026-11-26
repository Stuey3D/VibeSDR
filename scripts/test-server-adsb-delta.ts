/* test-server-adsb-delta.ts — ★★ V12 beta96 the ADS-B DELTA protocol through the REAL server, and its MEASUREMENT.
 *
 * A synthetic dump1090 here (a local TCP port sending Beast: ~150 aircraft at a time, positions twice a second, velocity,
 * identification, Mode S squawks and altitudes, aircraft arriving and leaving, one squawking 7700) feeds a VibeServer
 * whose only radio is an ADS-B Beast feed (docs/v12/ADSB-FEED.md). Then:
 *   1. an OLDER client (no adsbd) and a DELTA client (adsbd=1) listen side by side for MEASURE_S seconds (default 60),
 *      counting bytes per message type: KB/s per listener before vs after;
 *   2. the older client gets `adsb` exactly as before and never adsb_k / adsb_d; the delta client gets a keyframe on
 *      joining, then deltas, never `adsb`; its rebuilt table (src/services/adsbDelta.ts) has the same aircraft as the
 *      older client's report of the same tick;
 *   3. a resync request is answered with a keyframe;
 *   4. CPU: the server process's CPU over 30 s with 1 delta listener, then with 100 delta listeners, then 100 older ones.
 * Silent (no audio device). Exit 3 = not run (no VIBESERVER_BIN). Never touches a real receiver or aggregator.
 *   VIBESERVER_BIN=vibeserver/build/vibeserver node --no-warnings scripts/test-server-adsb-delta.ts */
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
const { AdsbDeltaStore } = await import('../src/services/adsbDelta.ts');

const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
const MEASURE_S = Number(process.env.MEASURE_S || 60);
const CPU_S = Number(process.env.CPU_S || 30);
let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const freePort = (): Promise<number> => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = (s.address() as net.AddressInfo).port; s.close(() => res(p)); }); });
const until = async (f: () => boolean, ms = 10000) => { for (let t = 0; t < ms; t += 100) { if (f()) return true; await sleep(100); } return f(); };

// ── Mode S encoding (our own, from the published formats — the same as vibeserver/test-adsb-delta.cpp) ──
function crc24(b: Uint8Array, n: number): number {   // over the first n-3 bytes
  let c = 0;
  for (let i = 0; i < n - 3; i++) {
    c ^= b[i] << 16;
    for (let k = 0; k < 8; k++) { c <<= 1; if (c & 0x1000000) c ^= 0x1FFF409; }
  }
  return c & 0xFFFFFF;
}
function finish(b: Uint8Array, xor = 0): Uint8Array { const p = crc24(b, b.length) ^ xor; const n = b.length; b[n - 3] = p >> 16; b[n - 2] = (p >> 8) & 255; b[n - 1] = p & 255; return b; }
function df17(icao: number, me: bigint): Uint8Array {
  const b = new Uint8Array(14); b[0] = 0x8D; b[1] = icao >> 16; b[2] = (icao >> 8) & 255; b[3] = icao & 255;
  for (let k = 0; k < 7; k++) b[4 + k] = Number((me >> BigInt(48 - 8 * k)) & 0xFFn);
  return finish(b);
}
function NL(lat: number): number {
  if (Math.abs(lat) >= 87) return 1;
  const a = 1 - Math.cos(Math.PI / 30), c = Math.cos(Math.PI / 180 * lat);
  return Math.floor(2 * Math.PI / Math.acos(1 - a / (c * c)));
}
function pos(icao: number, lat: number, lon: number, alt: number, odd: number): Uint8Array {
  const mod = (x: number, y: number) => x - y * Math.floor(x / y);
  const dLat = 360 / (60 - odd);
  const yz = Math.floor(131072 * mod(lat, dLat) / dLat + 0.5);
  const rLat = dLat * (yz / 131072 + Math.floor(lat / dLat));
  const dLon = 360 / Math.max(1, NL(rLat) - odd);
  const xz = Math.floor(131072 * mod(lon, dLon) / dLon + 0.5);
  const n = Math.floor((alt + 1000) / 25), a = ((n >> 4) << 5) | 0x10 | (n & 0xF);
  return df17(icao, (11n << 51n) | (BigInt(a) << 36n) | (BigInt(odd) << 34n) | (BigInt(yz & 0x1FFFF) << 17n) | BigInt(xz & 0x1FFFF));
}
function vel(icao: number, gs: number, trk: number, vr: number): Uint8Array {
  const r = trk * Math.PI / 180, ve = gs * Math.sin(r), vn = gs * Math.cos(r);
  let me = 0n;
  const set = (a: number, b: number, x: number) => { const n = b - a + 1; me |= (BigInt(x) & ((1n << BigInt(n)) - 1n)) << BigInt(56 - b); };
  set(1, 5, 19); set(6, 8, 1); set(14, 14, ve < 0 ? 1 : 0); set(15, 24, Math.round(Math.abs(ve)) + 1);
  set(25, 25, vn < 0 ? 1 : 0); set(26, 35, Math.round(Math.abs(vn)) + 1); set(37, 37, vr < 0 ? 1 : 0); set(38, 46, Math.floor(Math.abs(vr) / 64) + 1);
  return df17(icao, me);
}
function ident(icao: number, cs: string, cat: number): Uint8Array {
  const set = '#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######';
  let me = (4n << 51n) | (BigInt(cat) << 48n);
  for (let i = 0; i < 8; i++) { const c = cs[i] ?? ' '; const at = set.indexOf(c); me |= BigInt(at > 0 ? at : 32) << BigInt(42 - 6 * i); }
  return df17(icao, me);
}
function idCode(sq: number): number {
  const A = Math.floor(sq / 1000) % 10, B = Math.floor(sq / 100) % 10, C = Math.floor(sq / 10) % 10, D = sq % 10;
  let id = 0; const put = (i: number, v: number) => { if (v) id |= 1 << (12 - i); };
  put(5, A >> 2 & 1); put(3, A >> 1 & 1); put(1, A & 1); put(11, B >> 2 & 1); put(9, B >> 1 & 1); put(7, B & 1);
  put(4, C >> 2 & 1); put(2, C >> 1 & 1); put(0, C & 1); put(12, D >> 2 & 1); put(10, D >> 1 & 1); put(8, D & 1);
  return id;
}
function surv(icao: number, df: number, code: number): Uint8Array {
  const b = new Uint8Array(7); b[0] = df << 3; b[2] = code >> 8; b[3] = code & 255;
  return finish(b, icao);
}
function emerg(icao: number, state: number, sq: number): Uint8Array {
  let me = 0n; const set = (a: number, b: number, x: number) => { const n = b - a + 1; me |= (BigInt(x) & ((1n << BigInt(n)) - 1n)) << BigInt(56 - b); };
  set(1, 5, 28); set(6, 8, 1); set(9, 11, state); set(12, 24, idCode(sq));
  return df17(icao, me);
}
function beast(m: Uint8Array, ts: bigint, sig: number): Buffer {
  const raw: number[] = [];
  for (let i = 5; i >= 0; --i) raw.push(Number((ts >> BigInt(8 * i)) & 0xFFn));
  raw.push(sig, ...m);
  const out = [0x1A, m.length === 7 ? 0x32 : 0x33];
  for (const b of raw) { out.push(b); if (b === 0x1A) out.push(0x1A); }
  return Buffer.from(out);
}

// ── the synthetic sky (deterministic) ──
let seed = 20261126;
const U = (a: number, b: number) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return a + (b - a) * seed / 0x7fffffff; };
interface Plane { icao: number; cs: string; cat: number; lat: number; lon: number; alt: number; gs: number; trk: number; vr: number; rx: number; sq: number; until: number }
let nextIcao = 0x400100;
function plane(t: number): Plane {
  const icao = nextIcao; nextIcao += 0x13;
  const r = U(5, 200) / 60, b = U(0, 2 * Math.PI);
  return { icao, cs: ['BAW', 'EZY', 'RYR', 'KLM', 'DLH', 'TOM'][icao % 6] + (icao % 9000 + 100), cat: icao % 5 + 1,
    lat: 52.29 + r * Math.cos(b), lon: -0.85 + r * Math.sin(b) / Math.cos(52.29 * Math.PI / 180),
    alt: Math.round(U(2000, 39000) / 25) * 25, gs: U(150, 480), trk: U(0, 360), vr: U(0, 1) < 0.3 ? 64 * Math.round(U(-40, 40)) : 0,
    rx: U(0, 1) < 0.6 ? U(0.85, 1) : U(0.15, 0.85), sq: 7000 + Math.floor(U(0, 7)) * 100 + Math.floor(U(0, 7)) * 10 + Math.floor(U(0, 7)), until: t + U(120, 1200) };
}
function feeder(port: number) {
  const sky: Plane[] = []; for (let i = 0; i < 150; i++) { const p = plane(0); p.until = U(30, 1200); sky.push(p); }
  let ts = 1000n, sec = 0;
  const socks = new Set<net.Socket>();
  const srv = net.createServer((c) => { socks.add(c); c.on('error', () => {}); c.on('close', () => socks.delete(c)); });
  const tick = setInterval(() => {   // every 100 ms: a tenth of the sky's second
    sec += 0.1;
    const bufs: Buffer[] = [];
    for (let i = 0; i < sky.length; i++) {
      const q = sky[i];
      if (sec >= q.until) { sky[i] = plane(sec); continue; }
      const deg = q.gs / 3600 / 60 * 0.1;
      q.lat += deg * Math.cos(q.trk * Math.PI / 180); q.lon += deg * Math.sin(q.trk * Math.PI / 180) / Math.cos(q.lat * Math.PI / 180);
      q.alt = Math.max(1000, q.alt + q.vr / 600);
      if (U(0, 1) < 0.002) q.trk = (q.trk + U(-20, 20) + 360) % 360;
      if (U(0, 1) > q.rx * 0.1 * 2.5) continue;   // heard: ~2.5 replies a second × reception
      const sig = Math.max(1, Math.min(255, Math.round(255 * Math.pow(10, (-30 + 25 * q.rx + U(-2, 2)) / 20))));
      const alt = Math.round(q.alt / 25) * 25;
      const k = U(0, 1);
      const m = k < 0.5 ? pos(q.icao, q.lat, q.lon, alt, U(0, 1) < 0.5 ? 0 : 1)
        : k < 0.75 ? vel(q.icao, q.gs, q.trk, q.vr)
        : k < 0.82 ? ident(q.icao, q.cs, q.cat)
        : k < 0.91 ? (i === 10 && sec > 60 && sec < 120 ? emerg(q.icao, 1, 7700) : surv(q.icao, 5, idCode(q.sq)))
        : surv(q.icao, 4, (() => { const n = Math.floor((alt + 1000) / 25); return ((n >> 5) << 7) | (((n >> 4) & 1) << 5) | 0x10 | (n & 0xF); })());
      ts += 12000n * 7n;
      bufs.push(beast(m, ts, sig));
    }
    const all = Buffer.concat(bufs);
    for (const c of socks) c.write(all);
  }, 100);
  srv.listen(port, '127.0.0.1');
  return { kill: () => { clearInterval(tick); srv.close(); for (const c of socks) c.destroy(); } };
}

// ── a raw WebSocket client that counts bytes per message type ──
interface Ws { bytes: Map<string, number>; count: Map<string, number>; last: Map<string, any>; msgs: any[]; keep: boolean; send(o: unknown): void; close(): void; reset(): void; onMsg?: (m: any) => void }
function ws(port: number, q: string, keep = true): Ws {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st: Ws = { bytes: new Map(), count: new Map(), last: new Map(), msgs: [], keep,
    send(o) { const b = Buffer.from(JSON.stringify(o)); const h = Buffer.alloc(6); h[0] = 0x81; h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); s.write(Buffer.concat([h, b])); },
    close() { s.destroy(); }, reset() { this.bytes.clear(); this.count.clear(); } };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET /ws/user-spectrum?user_session_id=${crypto.randomUUID()}&mode=binary8&bins=256&proto=1${q} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      let type = op === 0x2 ? '(binary)' : '(text)';
      let m: any = null;
      if (op === 0x1) { const t = p.toString(); const mm = /"type":"([a-z0-9_]+)"/.exec(t.slice(0, 40)); if (mm) type = mm[1]; if (st.keep) { try { m = JSON.parse(t); } catch {} } }
      st.bytes.set(type, (st.bytes.get(type) || 0) + off + len);   // ★ the frame as it crosses the wire, header included
      st.count.set(type, (st.count.get(type) || 0) + 1);
      if (m) { st.last.set(type, m); st.onMsg?.(m); }
    }
  });
  return st;
}
const kb = (w: Ws, secs: number, types?: string[]) => [...w.bytes].filter(([t]) => !types || types.includes(t)).reduce((a, [, b]) => a + b, 0) / 1024 / secs;
const kidsOf = (pid: number): number[] => {
  let out: string[] = [];
  try { out = execFileSync('pgrep', ['-P', String(pid)]).toString().trim().split('\n').filter(Boolean); } catch { /* none */ }
  return out.map(Number).flatMap((k) => [k, ...kidsOf(k)]);
};
/** ★ The server and every process it started (a radio runs in its own process behind the front door). */
const cpuSecs = (pid: number) => [pid, ...kidsOf(pid)].reduce((a, p) => a + cpuOne(p), 0);
const cpuOne = (pid: number) => {   // ps TIME: [[dd-]hh:]mm:ss.cc
  let t = '';
  try { t = execFileSync('ps', ['-o', 'time=', '-p', String(pid)]).toString().trim(); } catch { return 0; }
  const [a, b] = t.split('-').length > 1 ? [Number(t.split('-')[0]), t.split('-')[1]] : [0, t];
  const p = String(b).split(':').map(Number);
  return a * 86400 + (p.length === 3 ? p[0] * 3600 + p[1] * 60 + p[2] : p[0] * 60 + p[1]);
};

const fport = await freePort();
const F = feeder(fport);
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-adsbd-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const port = await freePort(), ADMIN = 'adsb-delta-test';
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, adminPass: ADMIN, port,
  radios: [{ serial: 'beast-1', driver: 'beast', label: 'Synthetic sky', beastIn: `127.0.0.1:${fport}`, enabled: true, configured: true, users: 250 }] }));
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logf = path.join(dir, 'server.log');
const srv = spawn('nice', ['-n', '10', BIN, '--port', String(port), '--admin-pass', ADMIN], { env, stdio: ['ignore', fs.openSync(logf, 'w'), fs.openSync(logf, 'a')] });
const kids: Ws[] = [];
const results: string[] = [];
try {
  const up = await until(() => { try { return /Beast feed radio started/.test(fs.readFileSync(logf, 'utf8')); } catch { return false; } }, 20000);
  ok(up, 'the server starts as a Beast feed radio fed by the synthetic sky');
  await sleep(70000);   // ★ let the sky fill (aircraft are placed after an even/odd pair; the 60 s expiry settles)
  // ── 1 + 2: side by side ──
  const OLD = ws(port, ''), NEW = ws(port, '&adsbd=1');
  kids.push(OLD, NEW);
  const store = new AdsbDeltaStore();
  let rebuilt: any = null, resyncs = 0, applied = 0, matched = 0, compared = 0;
  NEW.onMsg = (m) => {
    if (m.type === 'adsb_k' || m.type === 'adsb_d') {
      const r = store.take(m);
      if (r.resync) { resyncs++; NEW.send({ type: 'adsb_resync' }); }
      if (r.report) { rebuilt = r.report; applied++; }
    }
  };
  OLD.onMsg = (m) => {
    if (m.type !== 'adsb' || !rebuilt) return;
    // the same tick reaches both sockets within a few ms; compare the address sets
    const a = new Set((m.aircraft || []).map((x: any) => x.hex)), b = new Set((rebuilt.aircraft || []).map((x: any) => x.hex));
    compared++;
    if (a.size === b.size && [...a].every((h) => b.has(h))) matched++;
  };
  await until(() => OLD.count.has('adsb') && NEW.count.has('adsb_k'), 8000);
  ok((OLD.last.get('adsb')?.aircraft || []).length >= 100, `the sky is up: ${(OLD.last.get('adsb')?.aircraft || []).length} aircraft in the report`);
  OLD.reset(); NEW.reset();
  const t0 = Date.now();
  await sleep(MEASURE_S * 1000);
  const secs = (Date.now() - t0) / 1000;
  const ac = (OLD.last.get('adsb')?.aircraft || []).length;
  const row = (w: Ws) => [...w.bytes].sort((x, y) => y[1] - x[1]).map(([t, b]) => `${t} ${(b / 1024 / secs).toFixed(2)}`).join(', ');
  results.push(`${secs.toFixed(0)} s, ${ac} aircraft at the end`);
  results.push(`older client: ${kb(OLD, secs).toFixed(2)} KB/s total (${row(OLD)})`);
  results.push(`delta client: ${kb(NEW, secs).toFixed(2)} KB/s total (${row(NEW)})`);
  for (const r of results) console.log('   ' + r);
  ok(!OLD.count.has('adsb_k') && !OLD.count.has('adsb_d') && (OLD.count.get('adsb') || 0) >= MEASURE_S - 2, `the older client gets adsb every second, never a delta (${OLD.count.get('adsb')} reports)`);
  ok(!NEW.count.has('adsb') && (NEW.count.get('adsb_d') || 0) + (NEW.count.get('adsb_k') || 0) >= MEASURE_S - 2, `the delta client gets adsb_d / adsb_k, never adsb (${NEW.count.get('adsb_d')} deltas, ${NEW.count.get('adsb_k') || 0} keyframes)`);
  ok((NEW.count.get('adsb_k') || 0) >= Math.floor(MEASURE_S / 60), 'a keyframe for everyone every 60 s');
  ok(resyncs === 0, `no gaps on a healthy socket (${resyncs} resyncs)`);
  ok(compared >= MEASURE_S - 3 && matched >= compared - 2, `the rebuilt table has the same aircraft as the full report (${matched}/${compared} ticks; a tick may straddle)`);
  const adsbOld = kb(OLD, secs, ['adsb']), adsbNew = kb(NEW, secs, ['adsb_d', 'adsb_k']);
  ok(adsbNew <= 3.0, `the delta listener's aircraft traffic is ≤ 3 KB/s at ~150 aircraft (${adsbNew.toFixed(2)}; full list ${adsbOld.toFixed(2)})`);
  // ── 3: resync answered ──
  const k0 = NEW.count.get('adsb_k') || 0;
  NEW.send({ type: 'adsb_resync' });
  ok(await until(() => (NEW.count.get('adsb_k') || 0) > k0, 3000), 'adsb_resync is answered with a keyframe');
  // ── 4: CPU ──
  const measureCpu = async (label: string) => {
    const c0 = cpuSecs(srv.pid!), w0 = Date.now();
    await sleep(CPU_S * 1000);
    const pct = (cpuSecs(srv.pid!) - c0) / ((Date.now() - w0) / 1000) * 100;
    const line = `server CPU, ${label}: ${pct.toFixed(1)} % of one core`;
    console.log('   ' + line); results.push(line);
    return pct;
  };
  OLD.close(); NEW.close();
  const one = [ws(port, '&adsbd=1', false)]; kids.push(...one);
  await sleep(3000);
  const c1 = await measureCpu('1 delta listener');
  const many: Ws[] = []; for (let i = 0; i < 99; i++) { many.push(ws(port, '&adsbd=1', false)); await sleep(20); }
  kids.push(...many);
  await sleep(5000);
  const open100 = [...one, ...many].filter((w) => (w.count.get('adsb_d') || 0) + (w.count.get('adsb_k') || 0) > 0).length;
  ok(open100 >= 95, `100 delta listeners connected and fed (${open100})`);
  const c100 = await measureCpu('100 delta listeners');
  for (const w of [...one, ...many]) w.close();
  await sleep(3000);
  const olds: Ws[] = []; for (let i = 0; i < 100; i++) { olds.push(ws(port, '', false)); await sleep(20); }
  kids.push(...olds);
  await sleep(5000);
  const c100o = await measureCpu('100 OLDER listeners (the full list)');
  const kbOld100 = olds.reduce((a, w) => a + kb(w, 1, ['adsb']), 0);
  results.push(`(the older listeners received ${(kbOld100 / 1024).toFixed(1)} MB of adsb in ~${CPU_S + 5} s between them)`);
  // ★ on a fast machine both are within noise (the tick is built once either way); the number that matters is the
  //   upload. Held only to a ceiling, so a per-socket rebuild (100× the encode) would show.
  ok(c100 < 5, `100 delta listeners cost under 5 % of a core (${c100.toFixed(1)} %; 100 older: ${c100o.toFixed(1)} %)`);
  void c1;
} finally {
  for (const w of kids) { try { w.close(); } catch {} }
  try { srv.kill('SIGTERM'); } catch {}
  F.kill();
  await sleep(500);
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log('\n   RESULTS'); for (const r of results) console.log('   ' + r);
console.log(`\ntest-server-adsb-delta: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
