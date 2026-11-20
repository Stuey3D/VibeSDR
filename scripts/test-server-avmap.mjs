// test-server-avmap.mjs — V12 2026-11-16 THE SHARED AIRCRAFT TABLE + THE AVIATION MAP's data, through TWO REAL radio
// processes (docs/v12/AVIATION.md §The shared table). Stuart: "a server with 2 SDRs — one on ADS-B, one on ACARS/VDL2 —
// must not show duplicate aircraft". Radio A replays a synthetic 1090 MHz clip (`test-adsb --write-clip`: 4CA97B RYR12AB,
// 406B90 EZY45KL, 400F11 BAW117) in ADS-B mode; radio B a synthetic VDL2 clip (`test-vdl2 --write-clip`: 4CA97B EI-EST,
// 406B90 G-EZWK, 3C6548 D-AIJH with an ADS-C position, …) in the DATALINK mode. Both processes share one runtime directory,
// as a multi-radio box's radios do. Checked: GET /vibeserver/aircraft.json on EITHER radio lists 4CA97B and 406B90 ONCE each
// with BOTH sources, 400F11 with ADS-B only, 3C6548 with VDL2 + ADS-C and its position; never a message body; the file in
// the runtime dir is its fixed size; the rows outlive the modes (kept an hour). {type:'ping'} keepalives (30 s silent drop).
// Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_VDL2_CLIP=…/vdl2.cu8 VIBE_ADSB_SYN_CLIP=…/adsb.u8 node scripts/test-server-avmap.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const VDL2 = process.env.VIBE_VDL2_CLIP || '', ADSB = process.env.VIBE_ADSB_SYN_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !VDL2 || !fs.existsSync(VDL2) || !ADSB || !fs.existsSync(ADSB)) {
  console.log('   not run — set VIBESERVER_BIN, VIBE_VDL2_CLIP (test-vdl2 --write-clip) and VIBE_ADSB_SYN_CLIP (test-adsb --write-clip)');
  process.exit(3);
}
const RATE = 2400000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 250; i++) { if (await f()) return true; await sleep(250); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

const top = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-avmap-'));
const RUN = path.join(top, 'run');   // ★ the runtime directory BOTH radio processes share (a multi-radio box's /run/vibeserver)
fs.mkdirSync(RUN);
async function server(tag, clipArgs) {
  const dir = path.join(top, tag);
  fs.mkdirSync(path.join(dir, 'data'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Aviation map test', locator: 'IO92nh', adminPass: 'avmap-test-admin',
    radios: [{ serial: `${tag}-test`, label: `Radio ${tag}`, enabled: true, configured: true, users: 4 }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE), ...clipArgs], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: RUN };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', '96100000', '--mode', 'wfm',
                             '--users', '4', '--idle-grace', '0', '--admin-pass', 'avmap-test-admin'], { env, stdio: ['ignore', log, log] });
  const getJson = async (p) => { try { const r = await fetch(`http://127.0.0.1:${port}${p}`, { signal: AbortSignal.timeout(3000) }); return { status: r.status, cors: r.headers.get('access-control-allow-origin'), text: await r.text() }; } catch { return null; } };
  await until(async () => (await getJson('/vibeserver.json'))?.status === 200, 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, getJson, L, stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
const table = async (S) => { const r = await S.getJson('/vibeserver/aircraft.json'); try { return r ? { ...r, j: JSON.parse(r.text) } : null; } catch { return null; } };
const rows = (t, key) => (t?.j?.aircraft || []).filter((a) => a.key === key);

console.log('── two radio processes, one aircraft table ──');
const A = await server('adsbA', ['--adsb', ADSB]);
const B = await server('vdl2B', ['--hf', VDL2, '--hf-centre', '136850000']);
const socks = [];
const keep = setInterval(() => { for (const s of socks) try { s.send({ type: 'ping' }); } catch {} }, 10000);
try {
  const a = ws(A.port, '/ws/user-spectrum?user_session_id=avmapA01&bins=256&proto=1'); socks.push(a);
  const b = ws(B.port, '/ws/user-spectrum?user_session_id=avmapB01&bins=256&proto=1'); socks.push(b);
  await until(() => a.open && b.open, 4000);
  await sleep(1200);
  a.send({ type: 'adsb', on: 1 });
  b.send({ type: 'avdata', on: 1, band: 'vdl2' });
  ok(await until(() => (lastOf(a, 'adsb')?.aircraft || []).length >= 3, 30000), `radio A: ADS-B hears the three clip aircraft (${(lastOf(a, 'adsb')?.aircraft || []).map((x) => x.hex || x.icao).join(' ')})`);
  ok(await until(() => (lastOf(b, 'avdata')?.aircraft || []).filter((x) => x.icao).length >= 5, 30000), `radio B: the datalink hears the VDL2 clip's aircraft (${(lastOf(b, 'avdata')?.aircraft || []).map((x) => x.icao).join(' ')})`);
  const both = (t) => { const r = rows(t, '4CA97B'); return r.length === 1 && r[0].src.includes('ADS-B') && r[0].src.includes('VDL2'); };
  ok(await until(async () => both(await table(A)), 20000), 'A\'s /vibeserver/aircraft.json: 4CA97B once, heard on ADS-B AND VDL2');
  // ★ each radio writes every 2 s and the VDL2 clip's aircraft come round once a loop: wait for the second one too
  const both2 = (t) => { const r = rows(t, '406B90'); return r.length === 1 && r[0].src.includes('ADS-B') && r[0].src.includes('VDL2'); };
  await until(async () => both2(await table(A)) && rows(await table(A), '3C6548').length === 1, 30000);
  const tA = await table(A), tB = await table(B);
  ok(tA && tA.status === 200 && tA.cors === '*', `200, CORS open (${tA && tA.status} ${tA && tA.cors})`);
  ok(both(tB), 'B\'s endpoint (the other process) says the same');
  const ei = rows(tA, '4CA97B')[0];
  ok(ei && ei.reg === 'EI-EST' && ei.posSrc === 'ADS-B' && Math.abs(ei.lat - 52.6) < 0.01 && ei.alt === 36000 && ei.srcAge && ei.srcAge['ADS-B'] != null && ei.srcAge.VDL2 != null,
     `one row, ADS-B's position + VDL2's registration: ${JSON.stringify(ei)}`);
  const ez = rows(tA, '406B90');
  ok(ez.length === 1 && ez[0].src.includes('ADS-B') && ez[0].src.includes('VDL2') && ez[0].reg === 'G-EZWK', `406B90 once, both sources: ${JSON.stringify(ez)} — B's row ${JSON.stringify((lastOf(b, 'avdata')?.aircraft || []).find((x) => x.icao === '406B90'))}`);
  const ba = rows(tA, '400F11');
  ok(ba.length === 1 && ba[0].src.join() === 'ADS-B' && ba[0].flight === 'BAW117', `400F11 (ADS-B only): ${JSON.stringify(ba)}`);
  const dl = rows(tA, '3C6548');
  ok(dl.length === 1 && dl[0].src.includes('VDL2') && dl[0].src.includes('ADS-C') && dl[0].posSrc === 'ADS-C' && Math.abs(dl[0].lat - 51.75) < 1e-3,
     `3C6548 (VDL2 only, an ADS-C report): ${JSON.stringify(dl)}`);
  // ★ beta88 the ADS-B state: radio A is decoding ADS-B now
  ok(tA?.j?.adsbLive === 1 && tA.j.adsbNow >= 3 && tA.j.adsbWin === 60 && tB?.j?.adsbLive === 1,
     `beta88: adsbLive 1 on both radios' endpoints, adsbNow ${tA?.j?.adsbNow} (≥ 3), window ${tA?.j?.adsbWin} s`);
  const keys = (tA?.j?.aircraft || []).map((x) => x.key);
  ok(keys.length === new Set(keys).size, `no aircraft twice (${keys.length} rows, ${new Set(keys).size} keys)`);
  ok(!/TEST MESSAGE BODY|EGLLXYA|#M1BPOS/.test(tA?.text || ''), '★ never a message body in the table');
  const f = path.join(RUN, 'aircraft-live.dat');
  ok(fs.existsSync(f) && fs.statSync(f).size === 264256, `the file in the runtime dir: ${fs.existsSync(f) ? fs.statSync(f).size : 'missing'} bytes`);
  ok(/aircraft table: .*aircraft-live\.dat/.test(A.L()) && /aircraft table: .*aircraft-live\.dat/.test(B.L()), 'both processes say where the table is');
  // ★ the rows outlive the modes (the hour the table keeps): leave both modes
  a.send({ type: 'adsb', on: 0 }); b.send({ type: 'avdata', on: 0 });
  await sleep(3000);
  ok(both(await table(A)), 'after both modes end the aircraft is still on the map (kept an hour)');
  // ★★ beta88 Stuart's case: no radio decoding ADS-B any more → within kAdsbLiveSec (10 s) adsbLive 0, adsbNow 0 — while the
  //    hour's rows are still served (the datalink's history; the clients drop the ADS-B-only ones)
  let off = null;
  ok(await until(async () => { off = await table(B); return off?.j?.adsbLive === 0; }, 20000),
     `beta88: ADS-B mode ended → adsbLive 0 on the other radio's endpoint (adsbLast ${off?.j?.adsbLast} s, adsbNow ${off?.j?.adsbNow})`);
  ok(off?.j?.adsbNow === 0 && off.j.adsbLast >= 10 && rows(off, '400F11').length === 1, '…ADS-B now 0, adsbLast ≥ 10 s, the rows still kept for the hour');
  const s1 = await table(A);
  const since = await A.getJson('/vibeserver/aircraft.json?since=1');
  ok(s1 && since && JSON.parse(since.text).aircraft.length <= s1.j.aircraft.length, '?since narrows the list');
  for (const s of socks) s.close();
} finally {
  clearInterval(keep);
  A.stop(); B.stop();
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
