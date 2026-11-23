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
//  10. ★★★ beta92 OPEN IN VFO B: RS-44 transponder → its CW beacon in VFO B (CW on the B host, B's shift = A's × fB/fA,
//      a B socket joins, off / new pick / drop close it quietly); ISS 145.800 → 145.825 APRS only (437.x outside the
//      capture); nothing offered with VFO B switched off; a locked window's listener alone.
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

async function server(tag, { loc = 'IO92ng', radio = {}, orbits = true, freq = 437800000, mode = 'nfm', state = '', env: extraEnv = {}, args = [] } = {}) {
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
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...extraEnv };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(freq), '--mode', mode,
                          '--users', '4', '--admin-pass', PASS, ...args], { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  const L = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };
  return { port, dir, status, L, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} } };
}
const sock = async (S, id) => { const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`); await until(() => sp.open, 3000); await sleep(800); return sp; };

let issLoc = '', rsLoc = '';
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
      // ★★ beta92: a place RS-44 is up from (≈1,250 km off its sub-point at 1,400 km: ~40°) with a Doppler that is not ~0
      const rs = l?.sats?.find((s) => s.norad === 44909);
      if (rs && Number.isFinite(rs.lat) && Number.isFinite(rs.lon)) rsLoc = locator(Math.max(-80, Math.min(80, rs.lat + (rs.lat > 0 ? -8 : 8))), ((rs.lon + 8 + 540) % 360) - 180);
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
      ok(l && l.sats.length === 15 && l.located === true && l.time !== 'none', `the list: ${l?.sats?.length} birds (14 + the six-digit test bird), located ${l?.located}, time ${l?.time}`);
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
      // ★★ SATELLITE MODE: inside a hold, opening it asks NOTHING (the on-open fetch obeys the holds)
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 3000);
      const l = lastOf(sp, 'sat_list');
      ok(l && l.source && l.source.synced === 0 && l.source.fetching === false && /NO SAT DATA/.test(l.why || ''), `an open inside the hold: synced 0, fetching false, ${l?.why}`);
      await sleep(800);
      ok(!/satellite data refreshed|satellite data not refreshed/.test(S.L()), 'and nothing was asked');
      sp.close();
    } finally { S.stop(); }
  }

  // ── 5. ★★ SATELLITE MODE on the shared dial: the list's new fields, pick / doppler / drop ──
  const picked = (sp) => jsons(sp, 'sat').filter((m) => m.state === 'picked');
  const lastPicked = (sp) => picked(sp).slice(-1)[0] || null;
  {
    console.log(`── SATELLITE MODE, shared dial (${issLoc}) ──`);
    const S = await server('satmode', { loc: issLoc });
    try {
      const sp = await sock(S, 'satmd0001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      const l = lastOf(sp, 'sat_list');
      const iss = l?.sats?.find((s) => s.id === 'ISS');
      ok(l?.source && typeof l.source.synced === 'number' && l.source.synced > 0 && l.source.fetching === false, `source.synced ${l?.source?.synced}, fetching ${l?.source?.fetching}`);
      ok(iss && iss.el > 0 && iss.inRangeFor > 0 && iss.inRangeFor < 900, `ISS up: inRangeFor ${iss?.inRangeFor} s`);
      ok(l?.sats?.filter((s) => (s.el ?? -90) < 0).every((s) => s.inRangeFor === undefined), 'no inRangeFor on a bird below the horizon');
      const uses = Object.fromEntries((iss?.tx || []).map((t) => [t.label, `${t.use}${t.sstv ? '+sstv' : ''}`]));
      ok(uses['FM repeater'] === 'voice' && uses['Voice / SSTV'] === 'voice+sstv' && uses['APRS'] === 'packet', `ISS uses: ${JSON.stringify(uses)}`);
      ok(l?.sats?.find((s) => s.id === 'METEOR-M2-3')?.tx?.[0]?.use === 'image', 'Meteor LRPT: image');
      ok(l?.sats?.every((s) => s.tx.every((t) => typeof t.use === 'string' && t.reach === true && t.why === undefined)), 'every tx has use; reachable = no why');

      // pick ISS Voice / SSTV with Doppler: tune + lock + SSTV, one step
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 1, doppler: true });
      ok(await until(() => !!lastPicked(sp), 4000), `picked: ${JSON.stringify(lastPicked(sp))}`);
      let p = lastPicked(sp);
      ok(p && p.norad === 25544 && p.tx === 1 && p.decoder === 'sstv' && p.doppler === true && p.shared === true && p.hz === 145800000 && p.mode === 'nfm' && p.use === 'voice',
         `ISS voice: decoder ${p?.decoder}, doppler ${p?.doppler}, ${p?.hz} ${p?.mode}`);
      ok(await until(() => lastOf(sp, 'config')?.vfo === 145800000, 3000), `the dial on 145.800 (vfo ${lastOf(sp, 'config')?.vfo})`);
      ok(await until(() => jsons(sp, 'sat').some((m) => (m.state === 'TRACK' || m.state === 'COAST') && m.tx === 'Voice / SSTV' && m.nominal === 145800000), 3000),
         'the Doppler lock on exactly that downlink');
      ok(/\[sat\] decoder sstv attached for the shared dial/.test(S.L()), 'the SSTV decoder attached on the server (the same host a decoder socket uses)');
      // the decoder socket sees it: a socket that attaches the SAME decoder joins it (no restart)
      const dx = ws(S.port, '/ws/dxcluster?user_session_id=satmd0001');
      await until(() => dx.open, 3000);
      await sleep(300);
      const attachesBefore = (S.L().match(/decoder attached: sstv/g) || []).length;
      dx.send({ type: 'audio_extension_attach', extension_name: 'sstv' });
      await until(() => dx.txt.some((t) => t.includes('audio_extension_attached')), 3000);
      await sleep(300);
      ok((S.L().match(/decoder attached: sstv/g) || []).length === attachesBefore, 'a client attaching the same decoder JOINS the pick\'s (not restarted)');
      dx.close();
      await sleep(400);

      // AUTO DOPPLER off / on
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'doppler', on: false });
      ok(await until(() => lastPicked(sp)?.doppler === false, 3000) && jsons(sp, 'sat').some((m) => m.state === 'OFF'), 'doppler off: the lock ends, the pick stands');
      sp.send({ type: 'sat', op: 'doppler', on: true });
      ok(await until(() => lastPicked(sp)?.doppler === true && jsons(sp, 'sat').some((m) => m.state === 'TRACK' || m.state === 'COAST'), 3000),
         `doppler on again: TRACK (${jsons(sp, 'sat').slice(-3).map((m) => `${m.state}${m.why ? ' ' + m.why : ''}${m.doppler !== undefined ? ' d=' + m.doppler : ''}`).join(' | ')})`);

      // a new pick replaces the old one — its decoder detached, the new one attached
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'pick', norad: 57166, tx: 0, doppler: false });
      ok(await until(() => lastPicked(sp)?.norad === 57166, 4000), `Meteor-M2-3: ${JSON.stringify(lastPicked(sp))}`);
      p = lastPicked(sp);
      ok(p && p.decoder === 'lrpt' && p.doppler === false && p.use === 'image' && p.hz === 137900000, `Meteor: lrpt, no Doppler (${p?.decoder})`);
      ok(/\[sat\] decoder sstv detached/.test(S.L()) && /\[sat\] decoder lrpt attached/.test(S.L()), 'the SSTV the pick started detached; LRPT attached');
      ok(/decoder attached: lrpt/.test(S.L()) || /lrpt/.test(S.L()), 'the LRPT decoder runs');
      ok(await until(() => lastOf(sp, 'config')?.vfo === 137900000, 3000), 'the dial on 137.900');

      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 2, doppler: true });
      ok(await until(() => lastPicked(sp)?.tx === 2, 4000) && lastPicked(sp).decoder === 'aprs', `ISS APRS: ${lastPicked(sp)?.decoder}`);
      sp.send({ type: 'sat', op: 'pick', norad: 44909, tx: 0, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 44909, 4000), 'RS-44 picked');
      p = lastPicked(sp);
      ok(p && p.decoder === '' && p.mode === 'usb' && p.hz === 435640000, `RS-44 transponder: USB at its centre, no decoder (${p?.mode} ${p?.hz})`);
      ok(/\[sat\] decoder aprs detached/.test(S.L()), 'the APRS decoder the previous pick started detached');
      // ★★ beta90 1.4 THE PASSBAND RULE: a tune 20 kHz across RS-44's passband keeps the lock (the old 15 kHz rule ended it)
      sp.txt.length = 0;
      sp.send({ type: 'tune', frequency: 435660000, mode: 'usb' });
      await sleep(800);
      ok(!jsons(sp, 'sat').some((m) => m.state === 'OFF' && /Tuned away/.test(m.why || '')), 'a tune inside RS-44\'s passband keeps the lock');
      sp.send({ type: 'tune', frequency: 435800000, mode: 'usb' });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'OFF' && /Tuned away/.test(m.why || '')), 3000), 'a tune outside the passband ends it');
      // ★★ beta90 1.1 / 1.2: a CW beacon decodes CW; AO-73's FUNcube telemetry 1200 Hz low in USB with fctlm; MO-122's Fox telemetry none
      sp.send({ type: 'sat', op: 'pick', norad: 44909, tx: 1, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 44909 && lastPicked(sp)?.tx === 1, 4000), 'RS-44 beacon picked');
      p = lastPicked(sp);
      ok(p && p.decoder === 'cw' && p.mode === 'cw' && p.use === 'packet' && p.hz === 435605000, `RS-44 CW beacon: cw decoder (${p?.decoder} ${p?.mode} ${p?.use})`);
      ok(/decoder attached: CW/.test(S.L()), 'the CW decoder runs');
      sp.send({ type: 'sat', op: 'pick', norad: 39444, tx: 1, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 39444, 4000), 'AO-73 telemetry picked');
      p = lastPicked(sp);
      ok(p && p.decoder === 'fctlm' && p.mode === 'usb' && p.hz === 145933800 && p.use === 'packet', `AO-73: fctlm, USB 1200 Hz low (${p?.decoder} ${p?.hz})`);
      ok(/decoder attached: fctlm/.test(S.L()) && /\[sat\] decoder cw detached/.test(S.L()), 'fctlm runs; the CW the previous pick started detached');
      sp.send({ type: 'sat', op: 'pick', norad: 60209, tx: 1, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 60209, 4000) && lastPicked(sp).decoder === '', `MO-122 Fox-format telemetry: no decoder (${lastPicked(sp)?.decoder})`);
      sp.send({ type: 'sat', op: 'pick', norad: 7530, tx: 0, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 7530, 4000), 'AO-7 Mode B picked');
      p = lastPicked(sp);
      ok(p && p.band === 'U/v' && p.modeName === 'Mode B' && p.lo === 145925000 && p.hi === 145975000, `picked carries band / modeName / lo / hi (${p?.band} ${p?.modeName} ${p?.lo}–${p?.hi})`);
      // the list's beta90 fields and the pass's ground track
      sp.send({ type: 'sat_list' });
      await until(() => jsons(sp, 'sat_list').length > 0, 3000);
      const l9 = lastOf(sp, 'sat_list');
      const ao7 = l9?.sats?.find((x) => x.id === 'AO-7');
      ok(ao7 && ao7.tx[0].band === 'U/v' && ao7.tx[0].modeName === 'Mode B' && ao7.tx[3]?.decoder === 'cw' && ao7.tx[3]?.hz === 145977500, 'sat_list: AO-7 band, mode name, the Mode B CW beacon');
      ok(typeof l9?.source?.pickPrivate === 'boolean', `source.pickPrivate ${l9?.source?.pickPrivate}`);
      ok(l9?.sats?.some((x) => x.id === 'TO-108') && l9?.sats?.find((x) => x.id === 'SONATE-2')?.tx?.some((t) => t.hz === 145880000 && t.sstv && t.decoder === 'sstv'), 'TO-108 listed; SONATE-2 SSTV 145.880 with SSTV');
      const ps = l9?.sats?.find((x) => x.id === 'ISS')?.passes?.[0];
      if (ps) {
        sp.send({ type: 'sat_list', op: 'track', norad: 25544, aos: ps.aos, los: ps.los });
        ok(await until(() => !!lastOf(sp, 'sat_track'), 3000), 'sat_track answered');
        const tr = lastOf(sp, 'sat_track');
        ok(tr && Array.isArray(tr.geo) && tr.geo.length > 10 && tr.geo.length <= 120 && Math.abs(tr.geoT0 - (ps.aos - 600)) < 2 && Math.abs(tr.geoT1 - (ps.los + 600)) < 2
           && tr.tca > ps.aos && tr.tca < ps.los && tr.geo.every((g) => g === null || (g.length === 3 && Math.abs(g[0]) <= 90 && g[2] > 200)),
           `sat_track geo: ${tr?.geo?.length} points, ${tr?.geoT0}…${tr?.geoT1}, tca ${tr?.tca}`);
        ok(Array.isArray(tr?.pts) && tr.pts.length > 2, 'pts kept beside it');
      } else ok(false, 'an ISS pass to draw');

      // refusals
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 9 });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'OFF' && m.op === 'pick' && /not in this server's list/.test(m.why || '')), 3000), 'a bad downlink refused in words');
      sp.send({ type: 'sat', op: 'doppler', on: true });
      await sleep(300);

      // drop: unlock + detach, one OFF
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 1, doppler: true });
      await until(() => lastPicked(sp)?.tx === 1 && lastPicked(sp)?.norad === 25544, 4000);
      await sleep(300);
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'drop' });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'OFF'), 3000), `drop: ${JSON.stringify(lastOf(sp, 'sat'))}`);
      await sleep(600);
      ok(jsons(sp, 'sat').filter((m) => m.state === 'OFF').length === 1, 'exactly one OFF');
      ok((S.L().match(/\[sat\] decoder sstv detached/g) || []).length >= 2, 'the pick\'s SSTV detached');
      sp.send({ type: 'sat', op: 'doppler', on: true });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.op === 'doppler' && /Pick a satellite first/.test(m.why || '')), 3000), 'after drop: doppler asks for a pick');
      // the old lock still works for old clients
      sp.send({ type: 'tune', frequency: 437800000, mode: 'nfm' });
      await sleep(500);
      sp.send({ type: 'sat', op: 'lock' });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'TRACK' || m.state === 'COAST'), 3000), 'the old sat lock still locks');
      sp.send({ type: 'sat', op: 'unlock' });
      sp.close();
    } finally { S.stop(); }
  }

  // ── 6. per-listener channels: a pick is the listener's own; reach in words ──
  {
    console.log('── SATELLITE MODE, a locked window on 437.8 MHz ──');
    const S = await server('satmodelk', { loc: issLoc, radio: { mode: 'locked', lockFreq: 437800000, rate: RATE } });
    try {
      const sp = await sock(S, 'satml0001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      const iss = lastOf(sp, 'sat_list')?.sats?.find((s) => s.id === 'ISS');
      const v = iss?.tx?.[1];
      ok(v && v.reach === false && /Outside the range the owner has locked/.test(v.why || ''), `145.800 outside the window: greyed, why "${v?.why}"`);
      ok(iss?.tx?.[0]?.reach === true, '437.800 inside it: reachable');
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 1 });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.op === 'pick' && m.state === 'OFF' && /Outside the range/.test(m.why || '')), 3000), 'picking it is refused with the same words');
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 0 });
      ok(await until(() => lastPicked(sp)?.norad === 25544, 4000), `picked the repeater: ${JSON.stringify(lastPicked(sp))}`);
      const p = lastPicked(sp);
      ok(p && !p.shared && p.decoder === '' && p.doppler === true, 'the listener\'s own pick, voice: no decoder, Doppler on');
      const other = await sock(S, 'satml0002');
      await sleep(1000);
      ok(!picked(other).length && !jsons(other, 'sat').some((m) => m.state === 'TRACK'), 'another listener is not moved and not told');
      other.close();
      sp.send({ type: 'sat', op: 'drop' });
      ok(await until(() => lastOf(sp, 'sat')?.state === 'OFF', 3000), 'dropped');
      sp.close();
    } finally { S.stop(); }
  }

  // ── 7. ★★ THE ON-OPEN FETCH: no orbit data, never synced — a fake vibesdr.net mirror (503, then 200) ──
  {
    console.log('── on-open fetch against a local fake mirror ──');
    const http = await import('node:http');
    const hits = [];
    let answer = 503;
    const mirror = http.createServer((req, res) => {
      hits.push({ url: req.url, ua: req.headers['user-agent'] || '' });
      if (answer === 503) { res.writeHead(503, { 'Retry-After': '900' }); res.end('busy'); return; }
      res.writeHead(200, { 'Content-Type': 'application/json', ETag: '"t1"' }); res.end(freshSats());
    });
    await new Promise((r) => mirror.listen(0, '127.0.0.1', r));
    const mport = mirror.address().port;
    const S = await server('satfetch', { loc: issLoc, orbits: false, state: ' ', env: { VIBESERVER_SATS_TEST_URL: `http://127.0.0.1:${mport}/api/sats` } });
    try {
      const sp = await sock(S, 'satft0001');
      await sleep(500);
      ok(hits.length === 0, 'nothing asked at start-up (its 30–330 s delay)');
      sp.send({ type: 'sat_list' });
      ok(await until(() => !!lastOf(sp, 'sat_list'), 3000), 'sat_list answered');
      let l = lastOf(sp, 'sat_list');
      ok(l?.source?.synced === 0 && l?.source?.fetching === true && /Fetching orbit data/.test(l?.why || ''), `★ the open starts the fetch: synced ${l?.source?.synced}, fetching ${l?.source?.fetching}, "${l?.why}"`);
      // the client re-asks every 3 s while fetching (here faster)
      const reask = async (pred, ms) => until(async () => { sp.txt.length = 0; sp.send({ type: 'sat_list' }); await sleep(250); const x = lastOf(sp, 'sat_list'); return x && pred(x); }, ms);
      ok(await reask((x) => x.source.fetching === false, 8000), 'the fetch ends');
      l = lastOf(sp, 'sat_list');
      ok(hits.length === 1 && hits[0].url === '/api/sats' && /^VibeServer/.test(hits[0].ua), `ONE request to the mirror (${hits.length}), UA ${hits[0]?.ua}`);
      ok(l.source.synced === 0 && /NO SAT DATA — vibesdr\.net's satellite data could not be downloaded \(HTTP 503\)/.test(l.why || ''), `the 503: no data, why "${l.why}"`);
      const st = fs.readFileSync(path.join(S.dir, 'data/sat/sync-state'), 'utf8');
      const until503 = Number((st.match(/\nuntil (\d+)/) || [])[1] || 0) - Math.floor(Date.now() / 1000);
      ok(until503 >= 15 * 60 - 5 && until503 <= 15 * 60 + 5 * 60 + 5, `held 15 min (+ 0–5) — the Retry-After 900 honoured (${until503} s)`);
      sp.send({ type: 'sat_list' });
      await sleep(800);
      ok(hits.length === 1, 'inside the hold: another open asks nothing');
      // ★ 15 minutes pass (the hold rewritten as expired) — the next open fetches, and the mirror answers 200
      const t = Math.floor(Date.now() / 1000);
      fs.writeFileSync(path.join(S.dir, 'data/sat/sync-state'), `checked 0\nattempt ${t - 1000}\nuntil ${t - 10}\nfails 1\ncode 503\nseed 0\n`);
      answer = 200;
      sp.txt.length = 0;
      sp.send({ type: 'sat_list' });
      ok(await until(() => lastOf(sp, 'sat_list')?.source?.fetching === true, 3000), 'the next open: fetching');
      ok(await reask((x) => x.source.fetching === false && x.source.synced > 0, 8000), 'the 200: synced');
      l = lastOf(sp, 'sat_list');
      ok(hits.length === 2 && Math.abs(l.source.synced - Date.now() / 1000) < 30 && !l.why, `synced ${l.source.synced}, no why (${l.why || ''})`);
      ok(l.sats.find((s) => s.id === 'ISS')?.passes?.length > 0, 'the orbits are in: passes listed');
      ok(fs.existsSync(path.join(S.dir, 'data/sat/sats.json')), 'the copy kept on disk (a restart does not refetch)');
      sp.send({ type: 'sat_list' });
      await sleep(800);
      ok(hits.length === 2, 'synced: an open asks nothing more');
      sp.close();
    } finally { S.stop(); mirror.close(); }
  }

  // ── 9. ★★ beta90 UNATTENDED SATELLITE PASSES: nobody here, ISS overhead → the pass is picked (tune, Doppler, SSTV);
  //       a listener arriving gives it up and gets the radio back where the pass found it ──
  {
    console.log(`── beta90 unattended pass, nobody here (${issLoc}) ──`);
    const bg = JSON.stringify({ on: false, waitMin: 5, profiles: [], satPasses: { on: true, minEl: 0, birds: [{ norad: 25544, label: 'Voice / SSTV' }] } });
    const S = await server('satpass', { loc: issLoc, radio: { bgDecode: bg }, freq: 437800000 });
    try {
      ok(await until(() => /\[sat\] unattended pass: ISS Voice \/ SSTV/.test(S.L()), 12000), 'with nobody here the ISS pass is picked by itself');
      ok(/unattended pass: ISS Voice \/ SSTV \(max [0-9]+°, LOS in [0-9]+ s\) — 145\.800 MHz nfm, Doppler (TRACK|COAST|ARMED), decoder sstv/.test(S.L()),
         `tuned to 145.800 NFM, Doppler locked, SSTV running (${(S.L().match(/unattended pass: [^\n]*/) || [''])[0]})`);
      ok(/\[sat\] decoder sstv attached for the shared dial/.test(S.L()), 'the SSTV decoder on the shared host (its gallery copy is the ordinary one)');
      await sleep(2500);
      ok(!/unattended pass: .*ended/.test(S.L()), 'its own decoder does not count as somebody (the pass carries on)');
      const sp = await sock(S, 'satpass01');
      ok(await until(() => /unattended pass: ISS Voice \/ SSTV, max [0-9]+° — ended \(a listener arrived\)/.test(S.L()), 4000), 'a listener arriving gives the pass up');
      ok(await until(() => lastOf(sp, 'config')?.vfo === 437800000, 4000), `…and gets the radio back where the pass found it (vfo ${lastOf(sp, 'config')?.vfo})`);
      sp.close();
    } finally { S.stop(); }
  }

  // ── 10. ★★★ beta92 OPEN IN VFO B (docs/v12/SAT-VFOB.md): another downlink of the PICKED bird on the listener's VFO B,
  //        its decoder on the B host, Doppler-corrected from the same pass (A's shift × fB / fA) ──
  const vfobOf = (sp) => jsons(sp, 'sat_vfob');
  const lastVfob = (sp) => vfobOf(sp).slice(-1)[0] || null;
  const chanB = (sp) => (lastOf(sp, 'watch')?.chans || []).find((c) => c.id === 1) || null;
  {
    console.log(`── OPEN IN VFO B: RS-44 transponder + its CW beacon, shared dial (${rsLoc || 'no RS-44 position'}) ──`);
    const S = await server('satvfob', { loc: rsLoc || issLoc });
    try {
      const sp = await sock(S, 'satvb0001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      const rs = lastOf(sp, 'sat_list')?.sats?.find((s) => s.norad === 44909);
      ok(rs && rs.el > 5, `RS-44 up here (el ${rs?.el})`);
      // ★ the one-at-a-time path: a plain pick creates no VFO B and offers the beacon
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'pick', norad: 44909, tx: 0, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 44909 && lastPicked(sp)?.tx === 0, 4000), 'RS-44 transponder picked');
      let p = lastPicked(sp);
      ok(p && Array.isArray(p.vfob) && p.vfob.length === 1 && p.vfob[0] === 1, `picked offers the CW beacon for VFO B: vfob ${JSON.stringify(p?.vfob)}`);
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'TRACK' && m.tx === 'Transponder'), 3000), 'A locked on the transponder (TRACK)');
      await sleep(600);
      ok(!jsons(sp, 'watch').some((w) => (w.chans || []).length > 0) && !vfobOf(sp).length, 'a plain pick makes no VFO B (one at a time, unchanged)');
      const dialA = lastOf(sp, 'config')?.vfo;
      // open the beacon in VFO B
      sp.txt.length = 0;
      sp.send({ type: 'sat_vfob', norad: 44909, tx: 1 });
      ok(await until(() => lastVfob(sp)?.on === true, 4000), `sat_vfob on: ${JSON.stringify(lastVfob(sp))}`);
      const vb = lastVfob(sp);
      ok(vb && vb.decoder === 'cw' && vb.mode === 'cw' && vb.hz === 435605000 && vb.use === 'packet' && vb.label === 'CW beacon' && typeof vb.params === 'object',
         `B: ${vb?.label} ${vb?.hz} ${vb?.mode}, decoder ${vb?.decoder}`);
      ok(await until(() => chanB(sp)?.freq === 435605000 && chanB(sp)?.mode === 'cw', 3000), `VFO B on 435.605 CW (${JSON.stringify(chanB(sp))?.slice(0, 120)})`);
      ok(/\[sat\] decoder cw attached on VFO B/.test(S.L()) && /decoder attached: CW/.test(S.L()), 'the CW decoder attached on the VFO B host');
      await sleep(500);
      ok(lastOf(sp, 'config')?.vfo === dialA || !jsons(sp, 'config').some((c) => c.vfo !== dialA), `VFO A's dial untouched (${dialA})`);
      // ★ the Doppler: B's shift = A's offset × fB / A's nominal, sampled together (the 25 Hz tracker; the readout at 2 Hz)
      let worst = 0, samples = 0, maxShift = 0;
      for (let i = 0; i < 6; i++) {
        await sleep(700);
        const r = jsons(sp, 'sat').filter((m) => m.state === 'TRACK' || m.state === 'COAST').slice(-1)[0];
        const c = chanB(sp);
        if (!r || !c || !r.nominal) continue;
        const want = r.offset * 435605000 / r.nominal;
        worst = Math.max(worst, Math.abs(c.shift - want) - Math.abs(r.rate || 0) * 0.6);
        maxShift = Math.max(maxShift, Math.abs(c.shift)); samples++;
      }
      ok(samples >= 4 && worst < 6, `B's shift follows A's × fB/fA (${samples} samples, worst ${worst.toFixed(1)} Hz past the rate allowance, |shift| up to ${maxShift.toFixed(0)} Hz)`);
      ok(maxShift > 20, `a real Doppler on B (|shift| ${maxShift.toFixed(0)} Hz)`);
      // the decoder socket of the box's B line JOINS the decoder (no restart)
      const dxb = ws(S.port, '/ws/dxcluster?user_session_id=satvb0001');
      await until(() => dxb.open, 3000);
      await sleep(300);
      const cwStarts = (S.L().match(/decoder attached: CW/g) || []).length;
      dxb.send({ type: 'audio_extension_attach', extension_name: 'cw', ...vb.params, vfo: 'B' });
      ok(await until(() => dxb.txt.some((t) => t.includes('"audio_extension_attached","vfo":"B"')), 3000), 'the B socket attached on VFO B');
      await sleep(300);
      ok((S.L().match(/decoder attached: CW/g) || []).length === cwStarts, 'it joined the open\'s CW decoder (not restarted)');
      // refusals
      sp.txt.length = 0;
      sp.send({ type: 'sat_vfob', norad: 44909, tx: 0 });
      ok(await until(() => lastVfob(sp)?.refused === true && /on VFO A/.test(lastVfob(sp)?.why || ''), 3000), 'the picked downlink itself: refused in words');
      sp.send({ type: 'sat_vfob', norad: 25544, tx: 2 });
      ok(await until(() => vfobOf(sp).some((m) => m.refused && /satellite you picked/.test(m.why || '')), 3000), 'another bird: refused in words');
      ok(await until(() => chanB(sp)?.freq === 435605000, 2000), 'a refusal leaves B where it was');
      // B's usual off ends it: told, the decoder stopped quietly
      sp.txt.length = 0; dxb.txt.length = 0;
      sp.send({ type: 'watch', id: 1, off: true });
      ok(await until(() => lastVfob(sp)?.on === false && !lastVfob(sp)?.refused, 3000), `VFO B off → sat_vfob on:false (${lastVfob(sp)?.why})`);
      ok(/\[sat\] decoder cw on VFO B detached/.test(S.L()), 'its CW decoder stopped');
      await sleep(400);
      ok(!dxb.txt.some((t) => t.includes('decoder_refused')), 'quietly: no "start it again" refusal on the B socket');
      ok(await until(() => jsons(sp, 'sat').some((m) => m.state === 'TRACK' && m.tx === 'Transponder'), 3000) && jsons(sp, 'sat').every((m) => m.state !== 'OFF'), 'A\'s lock on the transponder stands');
      dxb.close();
      // a new pick closes it; so does the drop
      sp.send({ type: 'sat_vfob', norad: 44909, tx: 1 });
      await until(() => lastVfob(sp)?.on === true, 3000);
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'pick', norad: 44909, tx: 1, doppler: true });
      ok(await until(() => lastVfob(sp)?.on === false && /pick changed/.test(lastVfob(sp)?.why || ''), 3000), `a new pick closes B (${lastVfob(sp)?.why})`);
      p = lastPicked(sp);
      ok(await until(() => lastPicked(sp)?.tx === 1, 3000) && JSON.stringify(lastPicked(sp)?.vfob) === '[0]', `beacon picked: the transponder offered the other way (${JSON.stringify(lastPicked(sp)?.vfob)})`);
      sp.send({ type: 'sat_vfob', norad: 44909, tx: 0 });
      ok(await until(() => lastVfob(sp)?.on === true && lastVfob(sp)?.decoder === '' && lastVfob(sp)?.mode === 'usb', 3000), 'the transponder in VFO B: USB, nothing to decode');
      sp.txt.length = 0;
      sp.send({ type: 'sat', op: 'drop' });
      ok(await until(() => lastVfob(sp)?.on === false, 3000), 'the drop closes B');
      ok(await until(() => (chanB(sp) === null), 3000), 'VFO B gone');
      // a single-downlink bird offers nothing
      sp.send({ type: 'sat', op: 'pick', norad: 27607, tx: 0, doppler: false });
      ok(await until(() => lastPicked(sp)?.norad === 27607, 4000) && lastPicked(sp)?.vfob === undefined, 'SO-50 (one downlink): no vfob');
      sp.send({ type: 'sat', op: 'drop' });
      await sleep(300);
      sp.send({ type: 'sat_vfob', norad: 27607, tx: 0 });
      ok(await until(() => vfobOf(sp).some((m) => m.refused && /Pick a satellite first/.test(m.why || '')), 3000), 'no pick: refused in words');
      sp.close();
    } finally { S.stop(); }
  }
  {
    console.log(`── OPEN IN VFO B: ISS 145.800 + 145.825 APRS (${issLoc}) ──`);
    const S = await server('satvfobiss', { loc: issLoc, freq: 145800000 });
    try {
      const sp = await sock(S, 'satvi0001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 1, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 25544 && lastPicked(sp)?.tx === 1, 4000), 'ISS Voice / SSTV picked');
      const p = lastPicked(sp);
      ok(p && JSON.stringify(p.vfob) === '[2]', `only 145.825 offered: 437.800 / 437.550 are outside the capture (vfob ${JSON.stringify(p?.vfob)})`);
      sp.send({ type: 'sat_vfob', norad: 25544, tx: 0 });
      ok(await until(() => vfobOf(sp).some((m) => m.refused && /Outside the radio's window/.test(m.why || '')), 3000), '437.800 asked anyway: refused, outside the window');
      sp.txt.length = 0;
      sp.send({ type: 'sat_vfob', norad: 25544, tx: 2 });
      ok(await until(() => lastVfob(sp)?.on === true, 4000), `APRS in VFO B: ${JSON.stringify(lastVfob(sp))?.slice(0, 200)}`);
      const vb = lastVfob(sp);
      ok(vb && vb.decoder === 'aprs' && vb.hz === 145825000 && vb.mode === 'nfm' && vb.params && vb.params.baud_rate === 1200, 'B: APRS 1200 on 145.825 NFM, the attach parameters said');
      ok(/\[sat\] decoder aprs attached on VFO B/.test(S.L()) && /\[sat\] decoder sstv attached for the shared dial/.test(S.L()), 'SSTV on A and APRS on B, both running');
      ok(await until(() => chanB(sp)?.freq === 145825000, 3000), 'VFO B on 145.825');
      sp.close();
    } finally { S.stop(); }
  }
  {
    console.log('── OPEN IN VFO B: not offered where VFO B is switched off ──');
    const S = await server('satvfoboff', { loc: issLoc, freq: 145800000, radio: { vfoB: 0 } });
    try {
      const sp = await sock(S, 'satvo0001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 1, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 25544, 4000) && lastPicked(sp)?.vfob === undefined, `VFO B off on this radio: no vfob (${JSON.stringify(lastPicked(sp)?.vfob)})`);
      sp.send({ type: 'sat_vfob', norad: 25544, tx: 2 });
      ok(await until(() => vfobOf(sp).some((m) => m.refused && /VFO A\/B is switched off/.test(m.why || '')), 3000), 'asked anyway: refused in words');
      sp.close();
    } finally { S.stop(); }
  }
  {
    console.log('── OPEN IN VFO B: a locked window (each listener their own channel) ──');
    const S = await server('satvfoblk', { loc: issLoc, radio: { mode: 'locked', lockFreq: 437800000, rate: RATE } });
    try {
      const sp = await sock(S, 'satvl0001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 0, doppler: true });
      ok(await until(() => lastPicked(sp)?.norad === 25544 && lastPicked(sp)?.tx === 0, 4000), 'ISS repeater picked on the locked window');
      ok(JSON.stringify(lastPicked(sp)?.vfob) === '[3]', `437.550 (inside the window) offered, 145.8xx not (${JSON.stringify(lastPicked(sp)?.vfob)})`);
      sp.send({ type: 'sat_vfob', norad: 25544, tx: 3 });
      ok(await until(() => lastVfob(sp)?.on === true && lastVfob(sp)?.decoder === 'sstv', 4000), `SSTV events in this listener's VFO B (${lastVfob(sp)?.decoder})`);
      const other = await sock(S, 'satvl0002');
      await sleep(800);
      ok(!vfobOf(other).length && !(lastOf(other, 'watch')?.chans || []).length, 'another listener is not given a VFO B');
      other.close();
      sp.close();
    } finally { S.stop(); }
  }

  // ── 8. ★★★ the R36S handheld: no satellites at all (Stuart, 2026-11-20) ──
  {
    console.log('── the handheld radio: no satellites ──');
    const S = await server('sathh', { loc: issLoc, args: ['--handheld-radio'] });
    try {
      const st = await S.status();
      ok(st && st.doppler === undefined, `vibeserver.json: no doppler (${st && st.doppler})`);
      const sp = await sock(S, 'sathh0001');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      const l = lastOf(sp, 'sat_list');
      ok(l && l.sats.every((s) => s.tx.every((t) => t.reach === false && /not on the handheld/.test(t.why || ''))), 'every downlink greyed: "not on the handheld"');
      ok(l && l.source && l.source.synced === undefined && l.source.fetching === undefined, 'no supply claimed');
      sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 1 });
      ok(await until(() => jsons(sp, 'sat').some((m) => m.op === 'pick' && m.state === 'OFF' && /handheld/.test(m.why || '')), 3000), 'a pick refused in words');
      ok(!/satellite data/.test(S.L()), 'no satellite data asked or loaded');
      sp.close();
    } finally { S.stop(); }
  }
} catch (e) { fail++; console.log(`   FAIL ${e && e.stack || e}`); }

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
