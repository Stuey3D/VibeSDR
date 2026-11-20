// test-server-sat.mjs — V12 SATELLITE ENGINE through the REAL server (docs/v12/SATELLITE-ENGINE.md). Synthetic only:
//   a fake rtl_tcp (no satellite on it — the lock is the MODEL's, which is what this checks), and CelesTrak's 2026-11-19
//   amateur + weather groups (vibeserver/testdata-sat) placed in the server's orbit cache with their epochs moved to an
//   hour ago, so the sets are always "fresh" whatever day the test runs (the orbits stay self-consistent, only shifted
//   in time). No network: a fresh cache is never refreshed (satcat.cpp).
//   1. discover where ISS is now (sat_list) and start the server UNDER it (a 6-character locator), so it is up.
//   2. shared dial: doppler in vibeserver.json, the picker list, LOCK → TRACK on the nominal 437.800 with the dial snapped
//      and the shift smooth, a nudge keeps it, UNLOCK freezes the dial where the channel is, a big tune ends it, the
//      sky track for a pass.
//   3. a locked window (per-listener channels): the listener's channel is built ONCE at the lock and never rebuilt by
//      the tracker (no AGC reset), and the lock is the listener's own.
//   4. no orbit data: doppler advertised GREYED with the reason, a lock refused in words.
// Silent: no audio socket is opened.   VIBESERVER_BIN=…/vibeserver node scripts/test-server-sat.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — set VIBESERVER_BIN'); process.exit(3); }
const RATE = 2400000, PASS = 'sat-test-admin';
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };

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
  const keep = setInterval(() => { if (st.open) st.send({ type: 'ping' }); }, 8000);
  s.on('close', () => clearInterval(keep));
  st.close = () => { clearInterval(keep); try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
const lastOf = (st, type) => { const a = jsons(st, type); return a[a.length - 1] || null; };

/** Maidenhead (6 characters) for a point. */
function locator(lat, lon) {
  const L = 'ABCDEFGHIJKLMNOPQRSTUVWX';
  let x = lon + 180, y = lat + 90;
  const a = L[Math.floor(x / 20)], b = L[Math.floor(y / 10)];
  x %= 20; y %= 10;
  const c = Math.floor(x / 2), d = Math.floor(y);
  x -= c * 2; y -= d;
  return `${a}${b}${c}${d}${L[Math.floor(x * 12)].toLowerCase()}${L[Math.floor(y * 24)].toLowerCase()}`;
}

/** vibesdr.net's sats.json fixture (built by the mirror's own code from CelesTrak's 2026-11-19 data, plus TEST-101095:
 *  ERMINAZ-2V's real elements under a test-only downlink) with every EPOCH moved to an hour ago. */
function freshSats() {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-6digit-2026-11-19.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  return JSON.stringify(d);
}
async function adminGet(port, p) {
  try {
    const nonce = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`, { signal: AbortSignal.timeout(3000) }).then((r) => r.json())).nonce;
    const q = `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', PASS).update(nonce).digest('hex')}`;
    return await fetch(`http://127.0.0.1:${port}${p}?${q}`, { signal: AbortSignal.timeout(3000) }).then((r) => r.json());
  } catch { return null; }
}
/** ★ The sync's shared state: just checked — so the server under test asks vibesdr.net NOTHING (no network in tests). */
function syncState(extra = '') {
  const t = Math.floor(Date.now() / 1000);
  return `checked ${t}\nattempt ${t}\nseed 0\n${extra}`;
}

async function server(tag, { loc = 'IO92ng', radio = {}, orbits = true, freq = 437800000, mode = 'nfm', state = '' } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run', 'data/sat']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  if (orbits) fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
  fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), state || syncState());
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Satellite test', locator: loc, adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, ...radio }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE)], { stdio: 'ignore' });
  await sleep(300);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(freq), '--mode', mode,
                          '--users', '4', '--admin-pass', PASS], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, status, L, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} } };
}
const sock = async (S, id) => { const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`); await until(() => sp.open, 3000); await sleep(800); return sp; };

let issLoc = '';
try {
  // ── 1. where is ISS now? ──
  {
    console.log('── where ISS is now ──');
    const S = await server('satfind');
    try {
      const sp = await sock(S, 'satfind01');
      sp.send({ type: 'sat_list' });
      ok(await until(() => !!lastOf(sp, 'sat_list'), 4000), 'sat_list answered');
      const l = lastOf(sp, 'sat_list');
      const iss = l?.sats?.find((s) => s.id === 'ISS');
      ok(iss && Number.isFinite(iss.lat) && Number.isFinite(iss.lon), `ISS sub-point ${iss?.lat}, ${iss?.lon}`);
      // ★ 20 s ahead along its track is near enough (7.7 km/s): the next server starts in a few seconds
      issLoc = iss ? locator(iss.lat, iss.lon) : 'IO92ng';
      ok(/^[A-R]{2}\d\d[a-x]{2}$/.test(issLoc), `the locator under it: ${issLoc}`);
      sp.close();
    } finally { S.stop(); }
  }

  // ── 2. the shared dial ──
  {
    console.log(`── shared dial, the server under ISS (${issLoc}) ──`);
    const S = await server('satshared', { loc: issLoc });
    try {
      const st = await S.status();
      ok(st && st.doppler === 1 && !st.dopplerWhy, `vibeserver.json doppler ${st && st.doppler} ${st && st.dopplerWhy || ''}`);
      const sp = await sock(S, 'satsh0001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      const l = lastOf(sp, 'sat_list');
      ok(l && l.sats.length === 14 && l.located === true && l.time !== 'none', `the list: ${l?.sats?.length} birds (13 + the six-digit test bird), located ${l?.located}, time ${l?.time}`);
      ok(l?.source && /CelesTrak/.test(l.source.orbits) && l.source.via === 'vibesdr.net' && Math.abs(Date.now() / 1000 - 2 * 3600 - l.source.fetched) < 120
         && /SatNOGS/.test(l.source.transmitters || ''), `the credit + the data's age: ${JSON.stringify(l?.source)}`);
      const six = l?.sats?.find((s) => s.norad === 101095);
      ok(six && six.id === 'TEST-101095' && six.ageState === 'fresh' && Array.isArray(six.passes), `★ six digits in sat_list: ${six?.norad} ${six?.ageState}`);
      const iss = l?.sats?.find((s) => s.id === 'ISS');
      ok(iss && iss.el > 45, `ISS overhead (el ${iss?.el})`);
      ok(l?.sats?.every((s) => s.tx.every((t) => t.reach === true)), 'an RTL reaches every listed downlink');
      ok(l?.sats?.some((s) => s.passes.length > 0), 'passes listed');
      sp.send({ type: 'tune', frequency: 437795000, mode: 'nfm' });
      await sleep(600);
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'lock' });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'TRACK' || m.state === 'COAST'), 4000), `locked: ${JSON.stringify(lastOf(sp, 'sat'))?.slice(0, 160)}`);
      const m0 = lastOf(sp, 'sat');
      ok(m0 && m0.shared === true && m0.name === 'ISS' && m0.nominal === 437800000, `ISS on the nominal 437.800 (shared ${m0?.shared})`);
      ok(m0 && Math.abs(m0.offset) < 11000 && Math.abs(m0.offset) > 0, `a Doppler shift ${m0?.offset} Hz`);
      ok(await until(() => lastOf(sp, 'config')?.vfo === 437800000, 3000), `the dial snapped to the nominal (vfo ${lastOf(sp, 'config')?.vfo})`);
      await sleep(3200);
      const seq = jsons(sp, 'sat').filter((m) => m.state === 'TRACK' || m.state === 'COAST');
      const steps = seq.slice(1).map((m, i) => Math.abs(m.offset - seq[i].offset));
      ok(seq.length >= 6, `the readout twice a second (${seq.length} in ~3.5 s)`);
      ok(steps.length > 0 && Math.max(...steps) < 160, `the shift moves smoothly: biggest change between readouts ${Math.max(...steps).toFixed(1)} Hz (≤ 215 Hz/s × 0.5 s)`);
      ok(new Set(seq.map((m) => m.offset)).size > 2, 'the shift follows the pass (it changes)');
      ok(/\[sat\] shared dial: Doppler lock on ISS FM repeater/.test(S.L()), 'the log says who and what');
      sp.send({ type: 'tune', frequency: 437800500, mode: 'nfm' });
      await sleep(900);
      const m1 = lastOf(sp, 'sat');
      ok(m1 && (m1.state === 'TRACK' || m1.state === 'COAST') && m1.nominal === 437800500, `a 500 Hz nudge keeps the lock (nominal ${m1?.nominal})`);
      const off1 = m1?.offset ?? 0;
      sp.send({ type: 'sat', op: 'unlock' });
      ok(await until(() => lastOf(sp, 'sat')?.state === 'OFF', 3000), 'unlocked');
      await until(() => Math.abs((lastOf(sp, 'config')?.vfo ?? 0) - 437800500) > 50, 2000);
      const v = lastOf(sp, 'config')?.vfo ?? 0;
      ok(Math.abs(v - (437800500 + off1)) < 400, `★ the dial frozen where the channel was, not snapped back (vfo ${v}, nominal + shift ≈ ${Math.round(437800500 + off1)})`);
      sp.send({ type: 'sat', op: 'lock', norad: 25544 });
      await until(() => jsons(sp, 'sat').slice(-1)[0]?.state === 'TRACK', 3000);
      sp.send({ type: 'tune', frequency: 145500000, mode: 'nfm' });
      ok(await until(() => { const m = lastOf(sp, 'sat'); return m && m.state === 'OFF' && /Tuned away/.test(m.why || ''); }, 3000), `a deliberate tune ends it: ${lastOf(sp, 'sat')?.why}`);
      await sleep(600);
      ok(Math.abs((lastOf(sp, 'config')?.vfo ?? 0) - 145500000) < 1, `…and the dial goes where they asked (${lastOf(sp, 'config')?.vfo})`);
      const p = iss?.passes?.[0];
      if (p) {
        sp.send({ type: 'sat_list', op: 'track', norad: 25544, aos: p.aos, los: p.los });
        ok(await until(() => !!lastOf(sp, 'sat_track'), 3000) && lastOf(sp, 'sat_track').pts.length > 5, `the sky track for a pass: ${lastOf(sp, 'sat_track')?.pts?.length} points`);
      }
      // ★★ six digits end to end: the lock by catalogue number 101095 (ARMED for its next pass, or TRACK if it is up)
      sp.send({ type: 'tune', frequency: 435500000, mode: 'nfm' });
      await sleep(600);
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'lock', norad: 101095 });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.norad === 101095 && ['ARMED', 'TRACK', 'COAST'].includes(m.state)), 3000),
         `★ locked by 101095: ${JSON.stringify(lastOf(sp, 'sat'))?.slice(0, 140)}`);
      ok(/"norad":101095[,}]/.test(sp.txt.find((t) => t.includes('"type":"sat"')) || ''), 'the readout carries 101095 in full');
      sp.send({ type: 'sat', op: 'unlock' });
      await until(() => lastOf(sp, 'sat')?.state === 'OFF', 2000);
      sp.send({ type: 'tune', frequency: 100000000, mode: 'wfm' });
      await sleep(500);
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'lock' });
      ok(await until(() => { const m = lastOf(sp, 'sat'); return m && m.state === 'OFF' && /NO SAT|No listed/.test(m.why || ''); }, 3000), `at 100 MHz: refused in words (${lastOf(sp, 'sat')?.why})`);
      sp.close();
    } finally { S.stop(); }
  }

  // ── 3. a locked window: per-listener channels ──
  {
    console.log('── locked window on 437.8 MHz: the listener\'s own channel ──');
    const S = await server('satlocked', { loc: issLoc, radio: { mode: 'locked', lockFreq: 437800000, rate: RATE } });
    try {
      const sp = await sock(S, 'satlk0001');
      sp.send({ type: 'tune', frequency: 437800000, mode: 'nfm' });
      await sleep(1000);
      sp.txt.length = 0;
      const before = (S.L().match(/client channel: /g) || []).length;
      sp.send({ type: 'sat', op: 'lock', norad: 25544 });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'TRACK' || m.state === 'COAST'), 4000), `locked: ${JSON.stringify(lastOf(sp, 'sat'))?.slice(0, 140)}`);
      const m0 = lastOf(sp, 'sat');
      ok(m0 && !m0.shared, 'the lock is this listener\'s own (not the shared dial\'s)');
      ok(/\[sat\] listener satlk000: Doppler lock on ISS/.test(S.L()), 'the log names the listener');
      await sleep(400);
      const atLock = (S.L().match(/client channel: /g) || []).length;
      ok(atLock - before <= 1, `the channel built at most once at the lock (${atLock - before}) — widened for the pass`);
      await sleep(3000);
      const after = (S.L().match(/client channel: /g) || []).length;
      ok(after === atLock, `★ never rebuilt by the tracker: no AGC reset, no audio gap (${after - atLock} rebuilds in 3 s of tracking)`);
      const seq = jsons(sp, 'sat').filter((m) => m.state === 'TRACK' || m.state === 'COAST');
      ok(seq.length >= 6 && new Set(seq.map((m) => m.offset)).size > 2, `the shift follows (${seq.length} readouts)`);
      const other = await sock(S, 'satlk0002');
      await sleep(1200);
      ok(!jsons(other, 'sat').some((m) => m.state === 'TRACK'), 'another listener on the same radio is not moved');
      other.close();
      sp.send({ type: 'tune', frequency: 437790000, mode: 'usb' });
      ok(await until(() => { const m = lastOf(sp, 'sat'); return m && m.state === 'OFF' && /Mode changed/.test(m.why || ''); }, 3000), `a mode change ends it: ${lastOf(sp, 'sat')?.why}`);
      sp.close();
    } finally { S.stop(); }
  }

  // ── 4. no orbit data ──
  {
    console.log('── no orbit data yet ──');
    const until0 = Math.floor(Date.now() / 1000) + 5400;
    const S = await server('satnodata', { orbits: false, state: syncState(`until ${until0}\nfails 1\ncode 0\nmsg vibesdr.net could not be reached for the satellite data (curl: (6) Could not resolve host) \u2014 VibeServer has stopped asking and will try again after 2026-11-20 03:00 UTC\n`) });
    try {
      const st = await S.status();
      ok(st && st.doppler === 1 && /NO SAT DATA — this server could not reach vibesdr\.net/.test(st.dopplerWhy || ''), `advertised greyed with the reason: ${st && st.dopplerWhy}`);
      // ★ the owner's words on the setup page (GET /vibeserver/config's satStatus)
      const cfgr = await adminGet(S.port, '/vibeserver/config');
      ok(cfgr && cfgr.satStatus && /could not be reached .* will try again after/.test(cfgr.satStatus.message) && cfgr.satStatus.code === 0
         && cfgr.satStatus.nextTry === until0 && cfgr.satStatus.have === false, `setup page: ${JSON.stringify(cfgr && cfgr.satStatus)}`);
      ok(!/vibesdr\.net\/api\/sats|satellite data refreshed/.test(S.L()), 'inside the hold: nothing asked');
      const sp = await sock(S, 'satnd0001');
      sp.send({ type: 'sat', op: 'lock' });
      ok(await until(() => { const m = lastOf(sp, 'sat'); return m && m.state === 'OFF' && /NO SAT DATA/.test(m.why || ''); }, 3000), `refused: ${lastOf(sp, 'sat')?.why}`);
      sp.close();
    } finally { S.stop(); }
  }
} catch (e) { fail++; console.log(`   FAIL ${e && e.stack || e}`); }

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
