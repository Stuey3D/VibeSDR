// beast-sky.mjs — ★ the synthetic dump1090 of scripts/test-server-adsb-delta.ts as a module (our own Mode S encoding from the
// published formats): ~150 aircraft at a time, positions twice a second, velocity, identification, squawks, coming and going.
// feeder(port) listens on 127.0.0.1:port and sends Beast to whoever connects; .kill() stops it. Tests only.
import net from 'node:net';
// ── Mode S encoding (our own, from the published formats — the same as vibeserver/test-adsb-delta.cpp) ──
function crc24(b, n) {   // over the first n-3 bytes
  let c = 0;
  for (let i = 0; i < n - 3; i++) {
    c ^= b[i] << 16;
    for (let k = 0; k < 8; k++) { c <<= 1; if (c & 0x1000000) c ^= 0x1FFF409; }
  }
  return c & 0xFFFFFF;
}
function finish(b, xor = 0) { const p = crc24(b, b.length) ^ xor; const n = b.length; b[n - 3] = p >> 16; b[n - 2] = (p >> 8) & 255; b[n - 1] = p & 255; return b; }
function df17(icao, me) {
  const b = new Uint8Array(14); b[0] = 0x8D; b[1] = icao >> 16; b[2] = (icao >> 8) & 255; b[3] = icao & 255;
  for (let k = 0; k < 7; k++) b[4 + k] = Number((me >> BigInt(48 - 8 * k)) & 0xFFn);
  return finish(b);
}
function NL(lat) {
  if (Math.abs(lat) >= 87) return 1;
  const a = 1 - Math.cos(Math.PI / 30), c = Math.cos(Math.PI / 180 * lat);
  return Math.floor(2 * Math.PI / Math.acos(1 - a / (c * c)));
}
function pos(icao, lat, lon, alt, odd) {
  const mod = (x, y) => x - y * Math.floor(x / y);
  const dLat = 360 / (60 - odd);
  const yz = Math.floor(131072 * mod(lat, dLat) / dLat + 0.5);
  const rLat = dLat * (yz / 131072 + Math.floor(lat / dLat));
  const dLon = 360 / Math.max(1, NL(rLat) - odd);
  const xz = Math.floor(131072 * mod(lon, dLon) / dLon + 0.5);
  const n = Math.floor((alt + 1000) / 25), a = ((n >> 4) << 5) | 0x10 | (n & 0xF);
  return df17(icao, (11n << 51n) | (BigInt(a) << 36n) | (BigInt(odd) << 34n) | (BigInt(yz & 0x1FFFF) << 17n) | BigInt(xz & 0x1FFFF));
}
function vel(icao, gs, trk, vr) {
  const r = trk * Math.PI / 180, ve = gs * Math.sin(r), vn = gs * Math.cos(r);
  let me = 0n;
  const set = (a, b, x) => { const n = b - a + 1; me |= (BigInt(x) & ((1n << BigInt(n)) - 1n)) << BigInt(56 - b); };
  set(1, 5, 19); set(6, 8, 1); set(14, 14, ve < 0 ? 1 : 0); set(15, 24, Math.round(Math.abs(ve)) + 1);
  set(25, 25, vn < 0 ? 1 : 0); set(26, 35, Math.round(Math.abs(vn)) + 1); set(37, 37, vr < 0 ? 1 : 0); set(38, 46, Math.floor(Math.abs(vr) / 64) + 1);
  return df17(icao, me);
}
function ident(icao, cs, cat) {
  const set = '#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######';
  let me = (4n << 51n) | (BigInt(cat) << 48n);
  for (let i = 0; i < 8; i++) { const c = cs[i] ?? ' '; const at = set.indexOf(c); me |= BigInt(at > 0 ? at : 32) << BigInt(42 - 6 * i); }
  return df17(icao, me);
}
function idCode(sq) {
  const A = Math.floor(sq / 1000) % 10, B = Math.floor(sq / 100) % 10, C = Math.floor(sq / 10) % 10, D = sq % 10;
  let id = 0; const put = (i, v) => { if (v) id |= 1 << (12 - i); };
  put(5, A >> 2 & 1); put(3, A >> 1 & 1); put(1, A & 1); put(11, B >> 2 & 1); put(9, B >> 1 & 1); put(7, B & 1);
  put(4, C >> 2 & 1); put(2, C >> 1 & 1); put(0, C & 1); put(12, D >> 2 & 1); put(10, D >> 1 & 1); put(8, D & 1);
  return id;
}
function surv(icao, df, code) {
  const b = new Uint8Array(7); b[0] = df << 3; b[2] = code >> 8; b[3] = code & 255;
  return finish(b, icao);
}
function emerg(icao, state, sq) {
  let me = 0n; const set = (a, b, x) => { const n = b - a + 1; me |= (BigInt(x) & ((1n << BigInt(n)) - 1n)) << BigInt(56 - b); };
  set(1, 5, 28); set(6, 8, 1); set(9, 11, state); set(12, 24, idCode(sq));
  return df17(icao, me);
}
function beast(m, ts, sig) {
  const raw = [];
  for (let i = 5; i >= 0; --i) raw.push(Number((ts >> BigInt(8 * i)) & 0xFFn));
  raw.push(sig, ...m);
  const out = [0x1A, m.length === 7 ? 0x32 : 0x33];
  for (const b of raw) { out.push(b); if (b === 0x1A) out.push(0x1A); }
  return Buffer.from(out);
}

// ── the synthetic sky (deterministic) ──
let seed = 20261126;
const U = (a, b) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return a + (b - a) * seed / 0x7fffffff; };
let nextIcao = 0x400100;
function plane(t) {
  const icao = nextIcao; nextIcao += 0x13;
  const r = U(5, 200) / 60, b = U(0, 2 * Math.PI);
  return { icao, cs: ['BAW', 'EZY', 'RYR', 'KLM', 'DLH', 'TOM'][icao % 6] + (icao % 9000 + 100), cat: icao % 5 + 1,
    lat: 52.29 + r * Math.cos(b), lon: -0.85 + r * Math.sin(b) / Math.cos(52.29 * Math.PI / 180),
    alt: Math.round(U(2000, 39000) / 25) * 25, gs: U(150, 480), trk: U(0, 360), vr: U(0, 1) < 0.3 ? 64 * Math.round(U(-40, 40)) : 0,
    rx: U(0, 1) < 0.6 ? U(0.85, 1) : U(0.15, 0.85), sq: 7000 + Math.floor(U(0, 7)) * 100 + Math.floor(U(0, 7)) * 10 + Math.floor(U(0, 7)), until: t + U(120, 1200) };
}
export function feeder(port) {
  const sky = []; for (let i = 0; i < 150; i++) { const p = plane(0); p.until = U(30, 1200); sky.push(p); }
  let ts = 1000n, sec = 0;
  const socks = new Set();
  const srv = net.createServer((c) => { socks.add(c); c.on('error', () => {}); c.on('close', () => socks.delete(c)); });
  const tick = setInterval(() => {   // every 100 ms: a tenth of the sky's second
    sec += 0.1;
    const bufs = [];
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

