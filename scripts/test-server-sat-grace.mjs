// test-server-sat-grace.mjs — b96-satgrace: UNATTENDED SATELLITE PASSES vs A KEPT AIS MAP on a REAL vibeserver (fake rtl_tcp,
// the fake PASS clock VIBESERVER_SAT_TEST_CLOCK, the 2026-11-19 orbit fixture with epochs moved to an hour ago).
//
// ★★★ MAC HA (beta96, background passes on for Meteor, weak WiFi): "the radio keeps jumping to a Meteor pass … AIS ended —
//     The receiver left AIS while you were away … then back to AIS by itself, repeating". The scheduler took the radio the
//     instant no socket was open (a 1-2 s WiFi blink), and a listener coming back got the pass given up but NOT the kept
//     AIS back. ISS's Voice / SSTV pass stands in for Meteor here (any picked pass takes the radio the same way).
//
//   1. AIS kept (landing map + keep running), the clock before AOS; a listener joins the warm AIS map.
//   2. the clock moves INTO the pass with the listener here: nothing happens (a listener has the radio).
//   3. ★ THE BLINK: the listener's socket drops for 2 s — no pass is started, AIS is never switched off; they come back
//      to AIS reports with nothing asked.
//   4. ★ GONE PAST THE GRACE (--idle-grace 12 → 12 s): the pass starts — and NOT before the grace has run.
//   5. ★ GIVE IT BACK: the same listener returns — the pass yields, AIS is back at once (reports with no mode_resume sent,
//      no ais_off), the log says so.
// ★ SILENT: no audio socket is opened.
//   usage: VIBESERVER_BIN=vibeserver/build/vibeserver node scripts/test-server-sat-grace.mjs
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

let issLoc = '', aos = 0;
const clockFile = path.join(os.tmpdir(), `vs-satgrace-clock-${process.pid}`);
const setClock = (sec) => fs.writeFileSync(clockFile, String(Math.round(sec)));
const spec = (S, sid) => ws(S.port, `/ws/user-spectrum?user_session_id=${sid}&bins=256&proto=1`);
const lines = (S, re) => (S.L().match(new RegExp(re.source, 'g')) || []).length;
const satAdmin = async (S) => (await adminGet(S.port, '/vibeserver/admin/status'))?.satPass || null;

/** One radio: AIS kept, ISS Voice / SSTV picked, the owner's idle grace `idle` s. `expectS` = when the pass may take the
 *  radio after the last listener left; `hold` = that wait is the HELD SESSION's (the passes add nothing on top). */
async function scenario(idle, expectS, hold) {
  console.log(`\n══ the owner's idle grace ${idle} s: ${hold ? `the held session (${expectS} s) is the wait` : `no hold — the passes wait modeLeaveGraceS() = ${expectS} s`} ══`);
  console.log(`── AIS kept at ${issLoc}, ISS passes on, the clock 10 min before AOS ──`);
  setClock(aos - Date.now() / 1000 - 600);
  const bg = JSON.stringify({ on: false, waitMin: 5, profiles: [], satPasses: { on: true, minEl: 0, birds: [{ norad: 25544, label: 'Voice / SSTV' }] } });
  const S = await server(`sggrace${idle}`, { loc: issLoc, freq: 96100000, mode: 'wfm', args: ['--idle-grace', String(idle)], env: { VIBESERVER_SAT_TEST_CLOCK: clockFile },
                                            radio: { bgDecode: bg, landingMap: 'avmar', landingModes: 'ais', mapKeep: true, landingFreq: 96100000, freq: 96100000, demodMode: 'wfm' } });
  const SID = crypto.randomUUID();
  try {
    ok(await until(() => /\[MAP\] keep running: nobody is connected \(the server has started\) — entering AIS/.test(S.L()), 10000), 'AIS kept: entered with nobody connected');
    let A = spec(S, SID);
    ok(await until(() => jsons(A, 'ais').length > 0, 5000), `a listener joins the warm AIS map (${jsons(A, 'ais').length} reports)`);

    // ── into the pass, the listener here ──
    console.log('── the clock moves into the pass, the listener here ──');
    setClock(aos - Date.now() / 1000 + 60);
    await sleep(4500);
    ok(!/pass due|unattended pass: ISS/.test(S.L()), 'a listener has the radio: no pass');

    // ── THE BLINK ──
    console.log('── the listener\'s socket drops for 2 s ──');
    const offs0 = lines(S, /\[AIS\] mode OFF/);
    A.close();
    await sleep(2000);
    A = spec(S, SID);
    ok(await until(() => jsons(A, 'ais').length > 0, 5000), `★ back after 2 s: AIS reports again with nothing asked (${jsons(A, 'ais').length})`);
    await sleep(3000);
    ok(!/pass due|unattended pass: ISS/.test(S.L()), '★ no pass started (the WiFi blink keeps the radio)');
    ok(lines(S, /\[AIS\] mode OFF/) === offs0, '★ AIS was never switched off');
    ok(jsons(A, 'ais_off').length === 0 && !jsons(A, 'mode_resume').some((m) => m.ok === 0), 'no ais_off, no refusal — nothing to explain');

    // ── GONE PAST THE GRACE ──
    console.log(`── gone for longer than the grace ──`);
    A.close();
    const tGone = Date.now();
    // ★ timed off the admin line (the log file is written in chunks, so its lines are not a clock)
    let waitWords = '', tPass = 0;
    await until(async () => {
      const sp0 = await satAdmin(S);
      if (sp0 && /^waiting — the last listener left/.test(sp0.now || '')) waitWords = sp0.now;
      if (sp0 && (sp0.keepPaused === 'AIS' || sp0.state === 'running')) { tPass = Date.now(); return true; }
      return false;
    }, (expectS + 15) * 1000);
    const took = (tPass - tGone) / 1000;
    ok(tPass > 0, `the pass takes the radio from kept AIS (${tPass ? took.toFixed(1) + ' s after the last listener left' : 'never'})`);
    ok(tPass > 0 && took >= expectS - 1.5, `★ …and not before ${expectS} s`);
    if (hold) {
      ok(tPass > 0 && took < expectS + 4.5, `★ the held session WAS the wait — no second grace stacked on top (${took.toFixed(1)} s, hold ${expectS} s)`);
    } else {
      ok(!!waitWords, `the admin line said why it waited: "${waitWords}"`);
    }
    ok(await until(() => /\[sat\] ISS Voice \/ SSTV pass due — AIS \(kept running\) pauses for it/.test(S.L()), 8000), 'the log: AIS (kept running) pauses for the pass');
    ok(await until(() => /\[sat\] unattended pass: ISS Voice \/ SSTV \(max/.test(S.L()), 8000), 'the ISS pass runs');

    // ── GIVE IT BACK ──
    console.log('── the same listener returns during the pass ──');
    const offsBefore = lines(S, /\[AIS\] mode OFF/);
    A = spec(S, SID);
    ok(await until(() => /unattended pass: ISS Voice \/ SSTV, max [0-9]+° — ended \(a listener arrived\)/.test(S.L()), 5000), 'the pass yields to the listener');
    ok(await until(() => /\[sat\] a listener arrived — AIS is back at once \(the pass had paused it\)/.test(S.L()), 5000),
       `★ the log: ${(S.L().match(/\[sat\] a listener arrived — AIS[^\n]*/) || ['(nothing)'])[0]}`);
    ok(await until(() => jsons(A, 'ais').length > 0, 6000), `★ AIS reports again, with no mode_resume sent (${jsons(A, 'ais').length})`);
    await sleep(2500);
    ok(jsons(A, 'ais_off').length === 0, '★ no ais_off — the map simply carries on');
    ok(!jsons(A, 'mode_resume').some((m) => m.ok === 0), 'no refusal sent at the arrival');
    ok(lines(S, /\[AIS\] mode OFF/) === offsBefore, 'AIS not left again under the listener');
    const sp1 = await satAdmin(S);
    ok(sp1 && sp1.state !== 'running' && !sp1.keepPaused, `the admin line: the pass is over, nothing paused (${sp1?.state}, "${sp1?.now || ''}")`);
    A.close();
  } finally { if (process.env.KEEP_LOG) console.log(S.L()); S.stop(); }
}

try {
  // ── where ISS is now, and its pass from right under it ──
  {
    console.log('── where ISS is now ──');
    const S = await server('sgfind');
    try {
      const sp = await sock(S, 'sgfind01');
      sp.send({ type: 'sat_list' });
      ok(await until(() => !!lastOf(sp, 'sat_list'), 4000), 'sat_list answered');
      const iss = lastOf(sp, 'sat_list')?.sats?.find((s) => s.id === 'ISS');
      ok(iss && Number.isFinite(iss.lat), `ISS sub-point ${iss?.lat}, ${iss?.lon}`);
      if (iss) issLoc = locator(iss.lat, iss.lon);
      sp.close();
    } finally { S.stop(); }
    const S2 = await server('sgpass', { loc: issLoc });
    try {
      const sp = await sock(S2, 'sgpass01');
      sp.send({ type: 'sat_list' });
      await until(() => !!lastOf(sp, 'sat_list'), 4000);
      const iss = lastOf(sp, 'sat_list')?.sats?.find((s) => s.id === 'ISS');
      const now = Date.now() / 1000;
      const p = iss?.passes?.find((q) => q.aos <= now + 60 && q.los > now + 180);
      ok(!!p, `ISS is up from ${issLoc}: AOS ${p && new Date(p.aos * 1000).toISOString()} LOS ${p && new Date(p.los * 1000).toISOString()}`);
      if (p) aos = p.aos;
      sp.close();
    } finally { S2.stop(); }
  }
  if (!aos) throw new Error('no ISS pass found — cannot go on');
  // ★ Mac Ha's case: no hold (an owner who lets go at once) — the passes' own grace is the only wait, its 5 s floor
  await scenario(0, 5, false);
  // ★ the default shape: a held session (min(idle grace, 3 min)) covers the blink; the pass follows the hold, not hold + grace
  await scenario(12, 12, true);
} catch (e) {
  fail++; console.log(`   FAIL ${e && e.stack || e}`);
} finally { try { fs.unlinkSync(clockFile); } catch {} }
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
