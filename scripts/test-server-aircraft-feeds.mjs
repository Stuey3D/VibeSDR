// ★★ V12 beta96 SERVER-LEVEL AIRCRAFT FEEDS through the REAL server (docs/v12/ADSB-FEED.md — "feeds are NOT radios").
// A synthetic dump1090 (Beast, a local port — never Stuart's Pi) and a synthetic dumphfdl (BaseStation text, connecting
// IN, as dumphfdl's tcp output does), on a FULL server (the front door owns the feeds) with a fake RTL radio:
//   A. a b96-adsbfeed Beast feed RADIO in the config is migrated to a server-level source (and its process, started as
//      vibeserver@beast-1 would be, leaves cleanly); the door's admin status lists both sources; one aircraft heard both
//      ways is ONE row with ADS-B and HFDL; an HFDL-only aircraft is HFDL only; aircraft.json carries the feeds (labels,
//      no addresses) through the door AND through the radio behind it; the Beast receiver going away → the banner names it
//      and says HFDL is still live; the flight-tracker hub forwards the Beast replies byte-faithfully to a destination,
//      never anything HFDL, and skips a destination on a source's host; addbeast adds a source with no restart.
// Silent: no audio device. Exit 3 = not run (no VIBESERVER_BIN).
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const up = async (port) => { for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return f(); };
const kids = [];
process.on('exit', () => { for (const k of kids) { try { k.kill('SIGTERM'); } catch {} } });

// ── the synthetic dump1090: the Riddle's worked examples (CRC-good), re-sent with a rising 12 MHz counter ──
const FRAMES = ['8D4840D6202CC371C32CE0576098',                // KLM1023 identification (4840D6)
                '8D40621D58C382D690C8AC2863A7', '8D40621D58C386435CC412692AD6',   // 40621D even + odd: 52.2572, 3.9194
                '5D484FDEA248F5'];                              // a DF11 all-call
const BAD = '8D40621D58C382D690C8AC2863A8';                     // one bit off in the parity… plus a second: never believed
function beast(hex, ts, sig) {
  const m = Buffer.from(hex, 'hex');
  const t = m.length === 7 ? 0x32 : 0x33;
  const raw = [];
  for (let i = 5; i >= 0; --i) raw.push(Number((BigInt(ts) >> BigInt(8 * i)) & 0xffn));
  raw.push(sig, ...m);
  const out = [0x1a, t];
  for (const b of raw) { out.push(b); if (b === 0x1a) out.push(0x1a); }
  return Buffer.from(out);
}
function feeder(port) {
  let ts = 1000, k = 0;
  const socks = new Set();
  const srv = net.createServer((c) => {
    socks.add(c);
    c.on('error', () => {});
    const t = setInterval(() => {
      const bufs = [];
      for (let i = 0; i < 20; i++, k++) {
        ts += 12000 * 25;
        bufs.push(beast(FRAMES[k % FRAMES.length], ts, 0x1a));
        if (k % 37 === 0) bufs.push(Buffer.from([0x55, 0x1a, 0x99, 0x00]));        // line noise
        if (k % 41 === 0) { const b = Buffer.from(BAD, 'hex'); b[8] ^= 0x40; bufs.push(beast(b.toString('hex'), ts, 30)); }
      }
      c.write(Buffer.concat(bufs));
    }, 100);
    c.on('close', () => clearInterval(t));
  });
  srv.listen(port, '127.0.0.1');
  srv.kill = () => { srv.close(); for (const c of socks) c.destroy(); };
  return srv;
}

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0 };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
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
      if (op === 0x2) st.bin++;
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(6); h[0] = 0x81; h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2);
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => s.destroy();
  return st;
}
const parsed = (st, type) => st.txt.filter((x) => x.includes(`"type":"${type}"`)).map((x) => { try { return JSON.parse(x); } catch { return {}; } });
const adminQ = async (H, pass) => {
  const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
  const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', pass).update(nonce).digest('hex')}`;
  // ★ a radio behind the door takes the door's admin ticket (as admin.ts qCached does)
  const t = await (await fetch(`${H}/vibeserver/admin-ticket?${q}`)).json();
  return `vs_admin_ticket=${encodeURIComponent(t.ticket)}`;
};
function mkdir(tag) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-feeds-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  return dir;
}


// ── A. the Lenovo case: PiAware (Beast) + UberSDR's dumphfdl (SBS) beside an SDR ──
console.log('\nA. a full server: a Beast source (migrated from a feed radio) + a BaseStation HFDL source, beside a fake RTL');
{
  const fport = await freePort();
  const fsrv = feeder(fport);
  const sbsPort = await freePort();
  // a flight-tracker destination (listens on every address: the hub dials "localhost", which may be ::1 first)
  const dest = { bytes: Buffer.alloc(0), conns: 0 };
  const dsrv = net.createServer((c) => { dest.conns++; c.on('data', (d) => { dest.bytes = Buffer.concat([dest.bytes, d]).subarray(-200000); }); c.on('error', () => {}); });
  await new Promise((r) => dsrv.listen(0, '::', r));
  const dport = dsrv.address().port;
  const loopPort = await freePort();   // a destination on the source's host — must be skipped
  let P = 0;
  for (let i = 0; i < 20 && !P; i++) {
    const p = await freePort();
    const free = await Promise.all([p + 1].map((q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); })));
    if (free.every(Boolean)) P = p;
  }
  const ADMIN = 'feeds-test', dir = mkdir('feeds');
  const cfgPath = path.join(dir, 'config.json');
  fs.writeFileSync(cfgPath, JSON.stringify({
    configured: true, name: 'Feeds beside an SDR', adminPass: ADMIN, port: P, fullMode: true,
    locator: 'IO92nh', lat: '', lon: '',   // ★ b96-rangefeed the Lenovo's own: a locator, lat / lon empty
    aircraftFeeds: JSON.stringify([{ fmt: 'sbs', dir: 'listen', port: sbsPort, label: 'hfdl', name: 'UberSDR HFDL' }]),
    adsbShare: JSON.stringify({ push: true, feeds: [{ host: 'localhost', port: dport, on: true }, { host: '127.0.0.1', port: loopPort, on: true }] }),
    radios: [
      { serial: 'afx-rtl', driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true, users: 1, rate: 2048000 },
      { serial: 'beast-1', driver: 'beast', label: 'PiAware', beastIn: `127.0.0.1:${fport}`, enabled: true, configured: true, users: 4 },
    ],
  }));
  let rtlPort = 0;
  while (!rtlPort || (rtlPort >= P && rtlPort <= P + 2)) rtlPort = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
  kids.push(rtl);
  await sleep(300);
  const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: cfgPath,
                VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logs = ['door', 'rtl', 'beast'].map((n) => path.join(dir, `${n}.log`));
  const out = (i) => ['ignore', fs.openSync(logs[i], 'w'), fs.openSync(logs[i], 'a')];
  const door = spawn(BIN, [], { env, stdio: out(0) });
  kids.push(door);
  const LG = (i) => { try { return fs.readFileSync(logs[i], 'utf8'); } catch { return ''; } };
  let hfdl = null;
  try {
    ok(await up(P), `the door is up (${P})`);
    const r1 = spawn(BIN, ['--radio-serial', 'afx-rtl', '--tcp', `127.0.0.1:${rtlPort}`, '--freq', '96100000', '--mode', 'wfm'], { env, stdio: out(1) });
    kids.push(r1);
    // ★ the old feed radio's process, exactly as a left-over vibeserver@beast-1 starts it: it leaves cleanly
    const r2 = spawn(BIN, ['--radio-serial', 'beast-1'], { env, stdio: out(2) });
    const code = await new Promise((res) => { const t = setTimeout(() => res('still running'), 15000); r2.on('exit', (c) => { clearTimeout(t); res(c); }); });
    ok(code === 0 && /was an ADS-B feed radio; it is now a server-level aircraft feed/.test(LG(2)), `a left-over beast-1 process says why and exits 0 (${code})`);
    ok(await up(P + 1), 'the RTL radio process is up');
    const H = `http://127.0.0.1:${P}`;
    const q = await adminQ(H, ADMIN);
    const st0 = await until(async () => {
      try { const j = await (await fetch(`${H}/vibeserver/admin/adsbfeed?${q}`)).json(); return (j.sources || []).length === 2 && j.sources.some((s) => s.title === 'PiAware' && s.state === 'connected'); } catch { return false; }
    }, 10000);
    const st = await (await fetch(`${H}/vibeserver/admin/adsbfeed?${q}`)).json();
    if (!st0) console.log('   status was: ' + JSON.stringify(st).slice(0, 600));
    ok(st0, 'the door owns two sources: PiAware (migrated from the feed radio) connected, and the HFDL listener');
    ok((st.sources || []).some((s) => s.title === 'UberSDR HFDL' && /listening on port/.test(s.line || '')), 'the HFDL source is listening');
    // dumphfdl connects IN and sends its MSG,3 lines (the formatter's exact shape)
    hfdl = net.connect(sbsPort, '127.0.0.1');
    hfdl.on('error', () => {});
    await new Promise((r) => hfdl.on('connect', r));
    const t = new Date(); const d = t.toISOString().slice(0, 10).replace(/-/g, '/'), hm = t.toISOString().slice(11, 19) + '.000';
    hfdl.write(`MSG,3,1,1,40621D,1,${d},${hm},${d},${hm},KLM10,,,,52.250000,3.900000,,0,,,,0\n`);
    hfdl.write(`MSG,3,1,1,AB12CD,1,${d},${hm},${d},${hm},BAW123,,,,10.500000,-40.100000,,8977,,,,0\n`);
    hfdl.write('garbage line\n');
    const acOf = async (base) => { try { return await (await fetch(`${base}/vibeserver/aircraft.json`)).json(); } catch { return {}; } };
    const both = await until(async () => { const j = await acOf(H); const a = (j.aircraft || []).find((x) => x.icao === '40621D'); return a && a.src.includes('ADS-B') && a.src.includes('HFDL'); }, 12000);
    const j = await acOf(H);
    const a1 = (j.aircraft || []).find((x) => x.icao === '40621D') || {};
    ok(both, `ONE row for 40621D heard both ways, sources ${JSON.stringify(a1.src)}`);
    const a2 = (j.aircraft || []).find((x) => x.icao === 'AB12CD') || {};
    ok(JSON.stringify(a2.src) === '["HFDL"]' && a2.posSrc === 'HFDL' && a2.flight === 'BAW123', `the HFDL-only aircraft: HFDL only, an HFDL position (${JSON.stringify(a2.src)} ${a2.posSrc})`);
    ok(a2.via === 'HFDL 8977', `…its channel from dumphfdl's --freq-as-squawk (${a2.via})`);
    ok(Array.isArray(j.feeds) && j.feeds.length === 2 && j.feeds.some((f) => f.label === 'HFDL') && !JSON.stringify(j.feeds).includes('127.0.0.1'),
       'aircraft.json lists the feeds by label, with no addresses');
    const jr = await acOf(`${H}/r/afx-rtl`);
    ok(Array.isArray(jr.feeds) && jr.feeds.length === 2 && (jr.aircraft || []).some((x) => x.icao === 'AB12CD'), 'the radio behind the door shows the same feeds and aircraft (the status file + shared table)');
    // ★★ b96-rangefeed (Stuart 11-27, the Lenovo: /adsb/range all 0 with the feed's aircraft on the map): the RADIO behind
    //    the door (no ADS-B of its own) counts the door's Beast feed into ITS outline, around the server's location — the
    //    locator's centre, as its /location says
    const loc = await (await fetch(`${H}/r/afx-rtl/location`)).json().catch(() => ({}));
    ok(Math.abs(loc.lat - 52.3125) < 1e-6 && Math.abs(loc.lon + 0.875) < 1e-6 && /IO92/i.test(loc.grid || ''), `the radio's /location: the locator's centre (${JSON.stringify(loc)})`);
    let rg = {};
    const filled = await until(async () => { try { rg = await (await fetch(`${H}/r/afx-rtl/adsb/range`)).json(); } catch { rg = {}; } return Array.isArray(rg.km) && rg.km.some((v) => v > 0); }, 15000);
    const fb = Array.isArray(rg.km) ? rg.km.findIndex((v) => v > 0) : -1;
    ok(filled && rg.lat === 52.3125 && rg.lon === -0.875 && fb >= 88 && fb <= 90 && rg.km[fb] > 300 && rg.km[fb] < 340,
       `the radio's /adsb/range fills from the door's Beast feed: 40621D at bearing ${fb}°, ${fb >= 0 ? rg.km[fb] : '—'} km, around ${rg.lat}, ${rg.lon}`);
    ok(filled && rg.km.filter((v) => v > 0).length <= 2, '…and only where the feed heard it (no HFDL position counted: AB12CD over the Atlantic)');
    // the hub: the destination got the Beast replies byte-faithfully (signal byte 0x1a as sent, escaped), nothing else
    await until(() => dest.bytes.length > 200, 8000);
    const hexs = []; { let i = 0; const b = dest.bytes; while (i < b.length - 1) { if (b[i] === 0x1a && (b[i + 1] === 0x32 || b[i + 1] === 0x33)) { const n = b[i + 1] === 0x32 ? 7 : 14; const raw = []; let k = i + 2; while (raw.length < 7 + n && k < b.length) { if (b[k] === 0x1a) k++; raw.push(b[k]); k++; } if (raw.length === 7 + n) hexs.push({ sig: raw[6], m: Buffer.from(raw.slice(7)).toString('hex').toUpperCase() }); i = k; } else i++; } }
    ok(hexs.length > 5 && hexs.every((h) => FRAMES.includes(h.m)), `the flight-tracker destination gets the receiver's replies (${hexs.length}) and nothing else — no HFDL, no noise`);
    ok(hexs.every((h) => h.sig === 0x1a), 'byte-faithful: the upstream signal byte kept');
    ok((st.feeds || []).some((f) => f.port === loopPort && f.state === 'skipped'), 'a destination on the source\'s host is skipped (the loop guard)');
    // the Beast receiver goes away
    fsrv.kill();
    const lost = await until(async () => /PiAware lost .* · UberSDR HFDL still live/.test((await acOf(H)).feedWarn || ''), 10000);
    ok(lost, `the banner names the lost source and the live one: ${(await acOf(H)).feedWarn}`);
    // addbeast from an older page: a server-level source, no restart
    const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
    const aq = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
    const r = await fetch(`${H}/vibeserver/sdr-change?${aq}`, { method: 'POST', body: JSON.stringify({ action: 'addbeast', beastIn: '192.168.86.99:30005', label: 'Another Pi' }) });
    const rj = await r.json().catch(() => ({}));
    const c2 = JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
    const srcs = JSON.parse(c2.aircraftFeeds || '[]');
    ok(r.status === 200 && rj.restart === false && srcs.some((s) => s.host === '192.168.86.99' && s.name === 'Another Pi'), `addbeast adds a server-level source, no restart (${srcs.length} sources)`);
    ok(!(c2.radios || []).some((x) => x.driver === 'beast') && srcs.some((s) => s.name === 'PiAware'), 'the saved config: no feed radio, PiAware a source');
    ok(c2.adsbShare && !(c2.radios || []).some((x) => x.adsbFeed), 'the flight-tracker settings are server-level');
    ok((LG(0).match(/PiAware .*lost:/g) || []).length === 1, 'the loss logged once');
  } finally { try { hfdl && hfdl.destroy(); } catch {} for (const k of kids) { try { k.kill('SIGTERM'); } catch {} } fsrv.kill(); dsrv.close(); }
}
console.log(`\ntest-server-aircraft-feeds: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
