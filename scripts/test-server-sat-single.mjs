// test-server-sat-single.mjs — ★★★ beta96 SINGLE MODE: SATELLITES through the REAL server (docs/v12/SAT-SINGLE.md).
// Synthetic only: a fake rtl_tcp (no satellite on it — the Doppler lock is the MODEL's), CelesTrak's 2026-11-19 fixture with
// its epochs moved to an hour ago (as test-server-sat.mjs), and a FAKE PASS CLOCK (VIBESERVER_SAT_TEST_CLOCK: a file of
// seconds the satellite engine's clock is moved by — AOS and LOS are file writes).
//   1. where RS-44 is now, and its pass from a place under it (the AOS / LOS the clock is moved to).
//   2. a SATELLITES radio (singleMode "sat", the owner's pick: RS-44 Transponder), two listeners BEFORE AOS: hwinfo says
//      sat, sat_single carries the timetable with RS-44 to come and no pass now; a listener's tune is refused (watch only).
//   3. AOS → the HYBRID: the log, `sat_hybrid` on to both, their sockets recycled; reconnected, each has their own VFO at the
//      passband centre with their own pick + AUTO DOPPLER; they tune independently (each keeps their own lock and dial);
//      a tune outside the passband is refused in words; a JOINER mid-pass lands at the centre with their own lock.
//   4. CPU of the server process: watch-only vs the hybrid, three listeners, 15 s each (Mac, fake rtl_tcp at 2.4 MS/s).
//   5. LOS → back to watch-only: `sat_hybrid` off, sockets recycled, a tune refused again, sat_single with no hybrid.
// Silent: no audio socket is opened.   VIBESERVER_BIN=…/vibeserver node scripts/test-server-sat-single.mjs
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
  let isC = false;
  s.on('close', () => { isC = true; clearInterval(keep); });
  st.isClosed = () => isC;
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
 *  ERMINAZ-2V's real elements under a test-only downlink) with every EPOCH moved to an hour ago.
 *  ★★★ beta94 + TEST-139444: AO-73's elements with ONLY a FUNcube telemetry downlink (145.700, no transponder within
 *  reach) — AO-73's own telemetry now opens its transponder's finder, so the telemetry-only pick is checked on this one.
 *  `file` (beta94 passprio): another fixture in vibeserver/testdata-sat (one without AO-73 gets no TEST-139444). */
function freshSats(file = 'sats-6digit-2026-11-19.json') {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat', file), 'utf8'));
  const ao = d.satellites.find((s) => s.norad === 39444);
  if (ao) d.satellites.push({ id: 'TEST-139444', name: 'TEST telemetry only (AO-73 elements)', norad: 139444, group: 'amateur', note: 'TEST ONLY \u2014 not a real downlink',
    transmitters: [{ label: 'Telemetry', downlink: 145700000, mode: 'usb', loop: 'none', consumer: 'fc-bpsk', baud: 1200 }],
    omm: { ...ao.omm, NORAD_CAT_ID: 139444, OBJECT_NAME: 'TEST-139444' } });
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  for (const g of Object.keys(d.labelOrbits || {})) d.labelOrbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
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

async function server(tag, { loc = 'IO92ng', radio = {}, orbits = true, satsFile = undefined, freq = 437800000, mode = 'nfm', state = '', env: extraEnv = {}, args = [] } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run', 'data/sat']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  if (orbits) fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats(satsFile));
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
  return { port, dir, status, L, pid: srv.pid, stop: () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} try { fs.rmSync(dir, { recursive: true, force: true }); } catch {} } };
}
const sock = async (S, id) => { const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`); await until(() => sp.open, 3000); await sleep(800); return sp; };


const cpuSecs = (pid) => { try { const t = String(require_ps(pid)).trim(); const p = t.split(':').map(Number); return p.length === 3 ? p[0] * 3600 + p[1] * 60 + p[2] : p[0] * 60 + p[1]; } catch { return NaN; } };
import { execFileSync } from 'node:child_process';
function require_ps(pid) { return execFileSync('ps', ['-o', 'time=', '-p', String(pid)]).toString(); }

let rsLoc = '', aos = 0, los = 0;
const clockFile = path.join(os.tmpdir(), `vs-satclock-${process.pid}`);
const setClock = (sec) => fs.writeFileSync(clockFile, String(Math.round(sec)));
const S_SESS = ['satsgl0a', 'satsgl0b', 'satsgl0c'];
const conn = async (S, id) => { const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${id}0000000000&bins=256&proto=1`); await until(() => sp.open, 3000); await sleep(600); return sp; };
const closed = (sp) => new Promise((res) => { const t = setInterval(() => { if (sp.isClosed()) { clearInterval(t); res(true); } }, 100); setTimeout(() => { clearInterval(t); res(false); }, 8000); });
try {
  // ── 1. where RS-44 is now ──
  {
    console.log('── where RS-44 is now ──');
    const S = await server('sgfind');
    try {
      const sp = await sock(S, 'sgfind01');
      sp.send({ type: 'sat_list' });
      ok(await until(() => !!lastOf(sp, 'sat_list'), 4000), 'sat_list answered');
      const rs = lastOf(sp, 'sat_list')?.sats?.find((s) => s.norad === 44909);
      ok(rs && Number.isFinite(rs.lat), `RS-44 sub-point ${rs?.lat}, ${rs?.lon}`);
      if (rs) rsLoc = locator(Math.max(-80, Math.min(80, rs.lat + (rs.lat > 0 ? -8 : 8))), ((rs.lon + 8 + 540) % 360) - 180);
      sp.close();
    } finally { S.stop(); }
    const S2 = await server('sgpass', { loc: rsLoc });
    try {
      const sp = await sock(S2, 'sgpass01');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      const rs = lastOf(sp, 'sat_list')?.sats?.find((s) => s.norad === 44909);
      const now = Date.now() / 1000;
      const p = rs?.passes?.find((q) => q.aos <= now + 60 && q.los > now + 120);
      ok(!!p, `RS-44 is up from ${rsLoc}: AOS ${p && new Date(p.aos * 1000).toISOString()} LOS ${p && new Date(p.los * 1000).toISOString()}`);
      if (p) { aos = p.aos; los = p.los; }
      sp.close();
    } finally { S2.stop(); }
  }
  if (!aos) throw new Error('no RS-44 pass found — cannot go on');

  // ── 2. a SATELLITES radio before AOS ──
  console.log(`── a SATELLITES radio at ${rsLoc}, the clock 5 min before RS-44's AOS ──`);
  setClock(aos - Date.now() / 1000 - 300);
  const bg = JSON.stringify({ on: false, waitMin: 5, profiles: [], satPasses: { on: true, minEl: 0, birds: [{ norad: 44909, label: 'Transponder' }] } });
  const S = await server('sgsingle', { loc: rsLoc, radio: { bgDecode: bg, singleMode: 'sat' }, freq: 145800000, mode: 'nfm', env: { VIBESERVER_SAT_TEST_CLOCK: clockFile } });
  let A, B, C;
  try {
    A = await conn(S, S_SESS[0]); B = await conn(S, S_SESS[1]);
    ok(await until(() => lastOf(A, 'hwinfo')?.singleMode === 'sat', 3000), `hwinfo singleMode ${lastOf(A, 'hwinfo')?.singleMode}`);
    ok(await until(() => !!lastOf(A, 'sat_single'), 3000), 'sat_single sent on arrival');
    const s0 = lastOf(A, 'sat_single');
    const rsRow = s0?.plan?.find((r) => r.norad === 44909);
    ok(s0 && s0.on === true && !s0.now && rsRow && rsRow.label === 'Transponder' && rsRow.aos > s0.t, `the timetable: RS-44 Transponder to come (AOS in ${rsRow ? Math.round(rsRow.aos - s0.t) : '?'} s), no pass now`);
    A.txt.length = 0;
    A.send({ type: 'tune', frequency: 145500000, mode: 'nfm' });
    ok(await until(() => jsons(A, 'notice').some((n) => n.single === 'sat'), 3000), `watch only: a tune refused in words (${jsons(A, 'notice')[0]?.why?.slice(0, 60)})`);
    A.send({ type: 'sat_list' });
    ok(await until(() => jsons(A, 'sat_single').length > 0, 3000), 'sat_list is answered with sat_single too');

    // ── 3. AOS → the hybrid ──
    console.log('── AOS: the hybrid ──');
    A.txt.length = 0; B.txt.length = 0;
    setClock(aos - Date.now() / 1000 + 90);
    ok(await until(() => /\[sat\] hybrid ON: RS-44 Transponder/.test(S.L()), 15000), `the log: ${(S.L().match(/\[sat\] hybrid ON[^\n]*/) || [''])[0].slice(0, 200)}`);
    ok(await until(() => lastOf(A, 'sat_hybrid')?.on === true && lastOf(B, 'sat_hybrid')?.on === true, 3000), 'sat_hybrid on to both listeners');
    const h = lastOf(A, 'sat_hybrid');
    ok(h && h.passLo === 435610000 && h.passHi === 435670000 && h.lo < 435610000 && h.hi > 435670000 && h.centre === 435640000,
       `the window ${h?.lo}–${h?.hi} (passband ${h?.passLo}–${h?.passHi}, centre ${h?.centre})`);
    ok(await closed(A) && await closed(B), 'both listeners\' sockets recycled by the server');
    A = await conn(S, S_SESS[0]); B = await conn(S, S_SESS[1]);
    ok(await until(() => (S.L().match(/hybrid: a listener joins/g) || []).length >= 2, 4000), 'both reconnect into their own channel + pick');
    const cfgVfo = (sp) => lastOf(sp, 'config')?.vfo;
    const sat = (sp) => jsons(sp, 'sat').filter((m) => m.state === 'TRACK' || m.state === 'COAST' || m.state === 'ARMED');
    ok(await until(() => jsons(A, 'sat').some((m) => m.state === 'picked') && jsons(B, 'sat').some((m) => m.state === 'picked'), 4000), 'each has their own pick (state picked)');
    ok(jsons(A, 'sat').find((m) => m.state === 'picked')?.shared !== true, 'the pick is the listener\'s own (not the room\'s)');
    ok(await until(() => sat(A).length > 0 && sat(B).length > 0, 4000), `each has their own Doppler lock: A ${sat(A).slice(-1)[0]?.state} ${sat(A).slice(-1)[0]?.offset} Hz, B ${sat(B).slice(-1)[0]?.state}`);
    // ★ a transponder lock keeps the listener where they HEAR (the received centre); its nominal is the satellite frame
    const heard = (m) => (m ? m.nominal + m.offset : 0);
    ok(Math.abs(heard(sat(A)[0]) - 435640000) < 150 && Math.abs(heard(sat(B)[0]) - 435640000) < 150,
       `both hear the passband centre (A ${heard(sat(A)[0]).toFixed(0)}, B ${heard(sat(B)[0]).toFixed(0)} = satellite-frame dial + Doppler)`);
    ok(/\[sat\] listener satsgl0a: Doppler lock on RS-44/.test(S.L()) && /\[sat\] listener satsgl0b: Doppler lock on RS-44/.test(S.L()), 'the log: a lock per listener');
    // independent tuning
    A.send({ type: 'tune', frequency: 435620000, mode: 'usb' });
    B.send({ type: 'tune', frequency: 435662000, mode: 'usb' });
    await sleep(1500);
    const la = sat(A).slice(-1)[0], lb = sat(B).slice(-1)[0];
    ok(la && lb && la.nominal === 435620000 && lb.nominal === 435662000, `each lock follows its own dial: A nominal ${la?.nominal}, B nominal ${lb?.nominal}`);
    ok(la && lb && Math.abs(la.offset) > 0 && Math.abs(lb.offset) > 0 && la.offset !== lb.offset, `Doppler applied per listener (A ${la?.offset} Hz, B ${lb?.offset} Hz — proportional to each frequency)`);
    // outside the passband
    A.txt.length = 0;
    A.send({ type: 'tune', frequency: 435800000, mode: 'usb' });
    ok(await until(() => jsons(A, 'notice').some((n) => /Outside this pass's transponder/.test(n.why || '')), 3000), `outside the window refused: "${jsons(A, 'notice').slice(-1)[0]?.why}"`);
    await sleep(500);
    ok(sat(A).slice(-1)[0]?.nominal === 435620000 && Math.abs((cfgVfo(A) ?? 0) - 435800000) > 1000, `…and A's dial did not move there (nominal ${sat(A).slice(-1)[0]?.nominal}, config ${cfgVfo(A)})`);
    A.send({ type: 'gain', gain: 100 });
    await sleep(400);
    ok(/\[SINGLE\] satellites: a listener's "gain" refused/.test(S.L()), 'the radio itself (gain) is still refused');
    // ★★★ b96-satgrace a returning listener's mode_resume (the app was in AIS) is ANSWERED with the pass, never left to time
    //     out into the app's generic "the receiver left AIS while you were away"
    A.send({ type: 'mode_resume', mode: 'ais' });
    ok(await until(() => jsons(A, 'mode_resume').length > 0, 3000), 'a mode_resume refused by single mode is still answered');
    const mr = lastOf(A, 'mode_resume');
    ok(mr && mr.ok === 0 && /^A scheduled RS-44 pass has the receiver until \d\d:\d\d UTC\.$/.test(mr.why || ''), `…with the pass and its end: "${mr?.why}"`);
    const losHm = new Date(los * 1000).toISOString().slice(11, 16);
    ok(mr && (mr.why || '').includes(losHm), `…the end is the pass's LOS (${losHm} UTC)`);
    // a joiner mid-pass
    C = await conn(S, S_SESS[2]);
    ok(await until(() => lastOf(C, 'sat_hybrid')?.on === true, 3000), 'a joiner is told the hybrid (sendFullState)');
    ok(await until(() => jsons(C, 'sat').some((m) => m.state === 'picked') && sat(C).length > 0, 4000) && Math.abs(heard(sat(C)[0]) - 435640000) < 150,
       `a joiner mid-pass lands at the centre with their own pick + Doppler (hears ${heard(sat(C)[0]).toFixed(0)}, ${sat(C)[0]?.state} ${sat(C)[0]?.offset} Hz)`);

    // ── 4. CPU ──
    console.log('── CPU (server process, 15 s) ──');
    let c0 = cpuSecs(S.pid); await sleep(15000); let c1 = cpuSecs(S.pid);
    const hybCpu = (c1 - c0) / 15 * 100;
    ok(Number.isFinite(hybCpu), `hybrid, 3 listeners (own VFO + Doppler + finder each): ${hybCpu.toFixed(1)} % of one core`);

    // ── 5. LOS → watch only ──
    console.log('── LOS: back to watch-only ──');
    for (const x of [A, B, C]) x.txt.length = 0;
    setClock(los - Date.now() / 1000 + 30);
    ok(await until(() => /\[sat\] hybrid OFF \(LOS\)/.test(S.L()), 15000), 'the log: hybrid OFF (LOS)');
    ok(await until(() => [A, B, C].every((x) => lastOf(x, 'sat_hybrid')?.on === false), 3000), 'sat_hybrid off to everyone');
    ok(await until(() => [A, B, C].every((x) => jsons(x, 'sat').some((m) => m.pickGone && m.single)), 3000), 'the pass\'s pick goes (pickGone, with its words)');
    ok(await closed(A) && await closed(B) && await closed(C), 'sockets recycled at LOS');
    A = await conn(S, S_SESS[0]); B = await conn(S, S_SESS[1]); C = await conn(S, S_SESS[2]);
    const s2 = lastOf(A, 'sat_single');
    ok(s2 && !s2.hybrid && !s2.now && /RS-44 Transponder, max/.test(s2.last || ''), `watch-only again: no hybrid, no pass now, the last pass "${s2?.last}"`);
    A.txt.length = 0;
    A.send({ type: 'tune', frequency: 435640000, mode: 'usb' });
    ok(await until(() => jsons(A, 'notice').some((n) => n.single === 'sat' && /follows the satellite passes/.test(n.why || '')), 3000), 'a tune is refused again (watch only)');
    c0 = cpuSecs(S.pid); await sleep(15000); c1 = cpuSecs(S.pid);
    const watchCpu = (c1 - c0) / 15 * 100;
    ok(Number.isFinite(watchCpu), `watch-only, 3 listeners on the one dial: ${watchCpu.toFixed(1)} % of one core (hybrid ${hybCpu.toFixed(1)} %)`);
    for (const x of [A, B, C]) x?.close();
  } finally { S.stop(); }
} catch (e) { ok(false, `threw: ${e && e.stack || e}`); }
finally { try { fs.rmSync(clockFile, { force: true }); } catch {} }
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
