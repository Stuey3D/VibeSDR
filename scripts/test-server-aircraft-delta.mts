/* test-server-aircraft-delta.ts — ★★ V12 beta96 THE SHARED AIRCRAFT TABLE AS DELTAS and THE STANDALONE MAP's TICKETS through the
 * REAL server, and the MEASUREMENT for the Setup page's "Map viewers" hint (docs/v12/ADSB-FEED.md).
 *
 * The synthetic dump1090 of test-server-adsb-delta (~150 aircraft, positions twice a second, coming and going) feeds a server in
 * the --beast-in diagnostic mode (one process: the shared table, aircraft.json, the spectrum socket). Then, from node:
 *   1. MEASURE_S seconds (default 60) of the standalone viewer's poll every 3 s, four ways side by side: aircraft.json whole,
 *      whole + gzip, the delta (?d=<cursor>&vt=<ticket>), delta + gzip — bytes on the wire per second; the delta's rebuilt table
 *      (src/services/aircraftDelta.ts) holds the same aircraft as the whole table at the end;
 *   2. the socket's acs (acd=1 — a server capability; the clients POLL the delta instead, because WebSocket frames are not
 *      compressed: measured here): {"type":"acs"} about every 3 s, its bytes, the same aircraft; a socket that did not ask gets
 *      none; acs_resync brings the whole table again;
 *   3. the tickets (mapViewers 3): three join, the fourth is told "the map is full (3/3)"; leave makes room; a poll with an
 *      unknown ticket answers viewer:"unknown"; a poll without one still works (an older client, a radio's map);
 *      admin/adsbfeed counts them;
 *   4. the time limit (mapTimeMin 1): polling past a minute answers viewer:"ended"; joining again at once is refused.
 * Silent. Exit 3 = not run (no VIBESERVER_BIN). Never touches a real receiver.
 *   VIBESERVER_BIN=vibeserver/build/vibeserver npx tsx scripts/test-server-aircraft-delta.mts */
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawn } from 'node:child_process';
import { AircraftDeltaStore } from '../src/services/aircraftDelta';
import { sharedSanitise } from '../src/services/aviationMap';

const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
const MEASURE_S = Number(process.env.MEASURE_S || 60);
let pass = 0, fail = 0;
const ok = (c: boolean, what: string) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));
const freePort = (): Promise<number> => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = (s.address() as net.AddressInfo).port; s.close(() => res(p)); }); });
const until = async (f: () => boolean | Promise<boolean>, ms = 10000) => { for (let t = 0; t < ms; t += 250) { if (await f()) return true; await sleep(250); } return !!(await f()); };

/** One GET with raw byte counting (the body as it crossed the wire), gunzipped when the server compressed it. */
function get(port: number, pathq: string, gzip = false): Promise<{ status: number; wire: number; enc: string; json: any }> {
  return new Promise((res) => {
    const s = net.connect(port, '127.0.0.1');
    const chunks: Buffer[] = [];
    s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\n${gzip ? 'Accept-Encoding: gzip\r\n' : ''}Connection: close\r\n\r\n`));
    s.on('data', (d) => chunks.push(d));
    s.on('error', () => res({ status: 0, wire: 0, enc: '', json: null }));
    s.on('close', () => {
      const all = Buffer.concat(chunks);
      const he = all.indexOf('\r\n\r\n');
      if (he < 0) { res({ status: 0, wire: all.length, enc: '', json: null }); return; }
      const head = all.subarray(0, he).toString();
      const status = Number(/HTTP\/1\.1 (\d+)/.exec(head)?.[1] || 0);
      const enc = /content-encoding:\s*(\S+)/i.exec(head)?.[1] || '';
      let body = all.subarray(he + 4);
      try { if (enc === 'gzip') body = zlib.gunzipSync(body); } catch { /* bad gzip */ }
      let json: any = null;
      try { json = JSON.parse(body.toString()); } catch { /* not json */ }
      res({ status, wire: all.length, enc, json });
    });
  });
}

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
  let ping: ReturnType<typeof setInterval> | undefined;
  const st: Ws = { bytes: new Map(), count: new Map(), last: new Map(), msgs: [], keep,
    send(o) { const b = Buffer.from(JSON.stringify(o)); const h = Buffer.alloc(6); h[0] = 0x81; h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); s.write(Buffer.concat([h, b])); },
    close() { clearInterval(ping); s.destroy(); }, reset() { this.bytes.clear(); this.count.clear(); } };
  let hs = false, buf = Buffer.alloc(0);
  // ★ the session's life: the server releases a proto ≥ 1 socket that sends nothing for 30 s (as the clients' heartbeat)
  ping = setInterval(() => { if (hs) st.send({ type: 'ping', ka: 1 }); }, 5000);
  s.on('close', () => clearInterval(ping));
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

const fport = await freePort();
const F = feeder(fport);
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-acd-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const port = await freePort(), ADMIN = 'acd-test-admin';
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, adminPass: ADMIN, port, mapViewers: 3, mapTimeMin: 1, radios: [] }));
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logf = path.join(dir, 'server.log');
const srv = spawn('nice', ['-n', '10', BIN, '--port', String(port), '--admin-pass', ADMIN, '--beast-in', `127.0.0.1:${fport}`, '--users', '50'], { env, stdio: ['ignore', fs.openSync(logf, 'w'), fs.openSync(logf, 'a')] });
const socks: Ws[] = [];
const kbs = (b: number, s: number) => (b / 1024 / s).toFixed(2);
const adminQ = async () => {
  const nonce = (await get(port, '/vibeserver/auth')).json?.nonce;
  const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(String(nonce)).digest('hex')}`;
  const t = (await get(port, `/vibeserver/admin-ticket?${q}`)).json;
  return `vs_admin_ticket=${encodeURIComponent(String(t?.ticket || ''))}`;
};
try {
  ok(await until(() => { try { return /Beast feed radio started/.test(fs.readFileSync(logf, 'utf8')); } catch { return false; } }, 20000), 'the server starts, fed by the synthetic sky');
  ok(await until(async () => ((await get(port, '/vibeserver/aircraft.json')).json?.aircraft?.length ?? 0) >= 100, 90000), 'the sky fills the shared table (≥ 100 aircraft)');

  // ── 3 + 4 first (the time limit needs a minute; it runs beside the measurement) ──
  const j1 = (await get(port, '/vibeserver/airsea/join')).json, j2 = (await get(port, '/vibeserver/airsea/join')).json, j3 = (await get(port, '/vibeserver/airsea/join')).json;
  const j4 = (await get(port, '/vibeserver/airsea/join')).json;
  ok(j1?.ok && j2?.ok && j3?.ok && j1.vt !== j2.vt, `three viewers join (tickets ${String(j1?.vt).slice(0, 6)}…, limit ${j1?.max}, ${j1?.limit} s each)`);
  ok(j4?.ok === false && j4.full === true && j4.n === 3 && j4.max === 3, 'the fourth is told the map is full (3/3)');
  const adm = (await get(port, `/vibeserver/admin/adsbfeed?${await adminQ()}`)).json;
  ok(adm?.mapViewers?.now === 3 && adm.mapViewers.max === 3, `admin/adsbfeed counts them (${adm?.mapViewers?.now} of ${adm?.mapViewers?.max})`);
  await get(port, `/vibeserver/airsea/leave?vt=${j3.vt}`);
  const j5 = (await get(port, '/vibeserver/airsea/join')).json;
  ok(j5?.ok === true, 'leave makes room: a new viewer joins');
  ok((await get(port, '/vibeserver/aircraft.json?d=&vt=nonesuch000000000000')).json?.viewer === 'unknown', 'an unknown ticket: viewer "unknown" (join again)');
  ok(Array.isArray((await get(port, '/vibeserver/aircraft.json')).json?.aircraft), 'a poll without a ticket still works (an older client, a radio\'s map)');
  // ★ j1 is kept polling for the time limit (1 minute) below; the measuring viewers are the owner's (no time limit)
  let j1Ended = false;
  const keep = setInterval(() => { void get(port, `/vibeserver/aircraft.json?vt=${j1.vt}`).then((r) => { if (r.json?.viewer === 'ended') j1Ended = true; }); }, 5000);
  const m1 = (await get(port, `/vibeserver/airsea/join?${await adminQ()}`)).json, m2 = (await get(port, `/vibeserver/airsea/join?${await adminQ()}`)).json;
  ok(m1?.ok && m2?.ok && m1.left === -1, 'the owner joins past the full map, with no time limit (the measuring viewers)');

  // ── 2. the socket ──
  const A = ws(port, '&acd=1'), N = ws(port, '');
  socks.push(A, N);
  const sockStore = new AircraftDeltaStore();
  let sockRebuilt: any = null, sockAcs = 0, sockFull = 0;
  A.onMsg = (m) => { if (m.type === 'acs') { sockAcs++; if (m.full === 1) sockFull++; const r = sockStore.apply(m); if (r.json) sockRebuilt = r.json; } };

  // ── 1. the four polls side by side ──
  const st = new AircraftDeltaStore();
  let wFull = 0, wGz = 0, wD = 0, wDgz = 0, polls = 0, cur = '', cur2 = '', ended = false;
  const st2 = new AircraftDeltaStore();
  A.reset(); N.reset();
  const t0 = Date.now();
  while (Date.now() - t0 < MEASURE_S * 1000) {
    const [a, b, c, d] = await Promise.all([
      get(port, '/vibeserver/aircraft.json'), get(port, '/vibeserver/aircraft.json', true),
      get(port, `/vibeserver/aircraft.json?d=${encodeURIComponent(cur)}&vt=${m1.vt}`), get(port, `/vibeserver/aircraft.json?d=${encodeURIComponent(cur2)}&vt=${m2.vt}`, true)]);
    if (c.json?.viewer === 'ended') { ended = true; break; }
    wFull += a.wire; wGz += b.wire; wD += c.wire; wDgz += d.wire; polls++;
    const r = st.apply(c.json); cur = r.cursor; const r2 = st2.apply(d.json); cur2 = r2.cursor;
    await sleep(3000);
  }
  const secs = (Date.now() - t0) / 1000;
  ok(polls >= Math.floor(MEASURE_S / 3.5) && !ended, `${polls} polls in ${secs.toFixed(0)} s`);
  const whole = sharedSanitise((await get(port, '/vibeserver/aircraft.json')).json)!;
  const mine = sharedSanitise(st.apply((await get(port, `/vibeserver/aircraft.json?d=${encodeURIComponent(cur)}&vt=${m1.vt}`)).json).json)!;
  const keysW = new Set(whole.aircraft.map((x) => x.key)), keysM = new Set(mine.aircraft.map((x) => x.key));
  const same = [...keysW].filter((k) => keysM.has(k)).length;
  ok(keysW.size >= 100 && same >= keysW.size - 3 && Math.abs(keysW.size - keysM.size) <= 3, `the delta viewer holds the same aircraft as the whole table (${same} of ${keysW.size}; it holds ${keysM.size})`);
  const pw = whole.aircraft.find((x) => x.lat != null && keysM.has(x.key))!, pm = mine.aircraft.find((x) => x.key === pw?.key);
  ok(!!pw && !!pm && Math.abs((pw.lat ?? 0) - (pm!.lat ?? 0)) < 0.05 && Math.abs((pw.posAge ?? 0) - (pm!.posAge ?? 0)) <= 4, `…with the same position and age (${pw?.key}: ${pm?.lat} vs ${pw?.lat})`);
  const results = [
    `aircraft.json every 3 s: ${kbs(wFull, secs)} KB/s`, `…gzipped: ${kbs(wGz, secs)} KB/s`,
    `delta every 3 s: ${kbs(wD, secs)} KB/s`, `…gzipped: ${kbs(wDgz, secs)} KB/s`,
    `the socket's acs (acd=1): ${kbs([...A.bytes].filter(([t]) => t === 'acs').reduce((x, [, y]) => x + y, 0), secs)} KB/s`,
  ];
  for (const l of results) console.log(`   ${l}`);
  ok(wGz < wFull * 0.4, `gzip takes the whole table under 40 % (${kbs(wGz, secs)} vs ${kbs(wFull, secs)} KB/s)`);
  ok(wD < wFull && wDgz < wGz, `the delta beats the whole table, gzipped or not (${kbs(wD, secs)} / ${kbs(wDgz, secs)} KB/s)`);
  ok(sockAcs >= Math.floor(secs / 3) - 2 && sockFull === 1, `the socket that asked gets acs about every 3 s (${sockAcs} in ${secs.toFixed(0)} s, ${sockFull} whole)`);
  ok(!N.count.has('acs'), 'a socket that did not ask gets none');
  const ks = new Set(((sharedSanitise(sockRebuilt)?.aircraft) ?? []).map((x) => x.key));
  ok(ks.size >= keysW.size - 5 && [...keysW].filter((k) => ks.has(k)).length >= keysW.size - 5, `the socket's rebuilt table has the same aircraft (${ks.size} vs ${keysW.size})`);
  const before = sockFull;
  A.send({ type: 'acs_resync' });
  ok(await until(() => sockFull > before, 8000), 'acs_resync brings the whole table again');

  // ── 4. the time limit: j1 joined over a minute ago ──
  await until(() => j1Ended, 30000);
  clearInterval(keep);
  ok(j1Ended, 'past the 1-minute limit the poll answers viewer "ended"');
  const again = (await get(port, '/vibeserver/airsea/join')).json;
  ok(again?.ok === false && again.ended === true && again.back > 0, `joining again at once is refused: time used (back in ${again?.back} s)`);
  fs.writeFileSync(path.join(os.tmpdir(), 'vs-acd-results.txt'), results.join('\n') + '\n');
} finally {
  for (const s of socks) s.close();
  try { srv.kill('SIGTERM'); } catch {}
  F.kill();
  await sleep(500);
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
}
console.log(`\ntest-server-aircraft-delta: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
