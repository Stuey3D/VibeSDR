// test-server-orbcomm-pick.mjs — ★★★ beta94 ORBCOMM: ANOTHER BIRD KEEPS THE DECODER, and THE ROOM'S PICK FOR A JOINER, through the
//   REAL server (Stuart 11-25: "i left an expired pass open and the next pass started decoding as expected but when i
//   selected the correct pass to see the half globe correctly it reset the pass that was decoding"). Synthetic: the fake
//   rtl_tcp (no ORBCOMM signal — what is checked is the decoder's LIFE, not its packets: vibeserver/test-orbcomm-run.cpp §1b
//   has the follow on a synthetic pass) and the ORBCOMM fixture with its epochs moved to an hour ago.
//   1. pick ORBCOMM bird A → "orbcomm" attached once; a decoder socket JOINS it; orbcomm_status carries ended / follow.
//   2. pick bird B from the list → "picked" B with the SAME parameters; the log says the decoder was KEPT; no second
//      "decoder attached: ORBCOMM", no detach; the status keeps counting (the IQ fed goes on rising, not reset).
//   3. a listener who joins the shared dial and opens SATELLITE (sat_list) is told the room's pick (B) — once; after their
//      drop… (not sent: a joiner's drop ends the room's pick); a non-ORBCOMM pick then DOES detach the decoder.
// Silent: no audio socket is opened.   VIBESERVER_BIN=…/vibeserver node scripts/test-server-orbcomm-pick.mjs
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

/** vibesdr.net's sats.json fixture WITH the ORBCOMM birds (sats-orbcomm-SYNTHETIC-2026-11-22.json — the mirror's own code),
 *  every EPOCH moved to an hour ago (as test-web-sat-labels.mjs). */
function freshSats() {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-orbcomm-SYNTHETIC-2026-11-22.json'), 'utf8'));
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

async function server(tag, { loc = 'IO92ng', radio = {}, orbits = true, freq = 437800000, mode = 'nfm', state = '', env: extraEnv = {}, args = [], rtlArgs = [] } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-${tag}-`));
  for (const d of ['data', 'run', 'data/sat']) fs.mkdirSync(path.join(dir, d), { recursive: true });
  if (orbits) fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
  fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), state || syncState());
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Satellite test', locator: loc, adminPass: PASS,
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users: 4, ...radio }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE), ...rtlArgs], { stdio: 'ignore' });
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


const picked = (sp) => jsons(sp, 'sat').filter((m) => m.state === 'picked');
const lastPicked = (sp) => picked(sp).slice(-1)[0] || null;
const ostat = (dx) => jsons(dx, 'orbcomm_status').map((m) => m.data).filter(Boolean);
const lastOstat = (dx) => ostat(dx).slice(-1)[0] || null;
const count = (S, re) => (S.L().match(re) || []).length;

try {
  console.log('── ORBCOMM picks on a shared dial ──');
  const S = await server('orbpick', { freq: 137500000, mode: 'nfm' });
  try {
    const sp = await sock(S, 'orbpk0001');
    sp.send({ type: 'sat_list' });
    ok(await until(() => !!lastOf(sp, 'sat_list'), 4000), 'sat_list answered');
    const l = lastOf(sp, 'sat_list');
    const orb = (l?.sats || []).filter((s) => /^ORBCOMM/.test(s.name) && s.tx.some((t) => t.decoder === 'orbcomm' && t.reach));
    ok(orb.length >= 2, `ORBCOMM birds offered with the orbcomm decoder (${orb.map((s) => s.name).join(', ')})`);
    const [A, B] = orb;
    const ia = A.tx.findIndex((t) => t.decoder === 'orbcomm'), ib = B.tx.findIndex((t) => t.decoder === 'orbcomm');
    // ── 1. A ──
    sp.send({ type: 'sat', op: 'pick', norad: A.norad, tx: ia, doppler: true });
    ok(await until(() => lastPicked(sp)?.norad === A.norad, 5000), `${A.name} picked`);
    const pa = lastPicked(sp);
    ok(pa?.decoder === 'orbcomm' && pa?.params?.width === 640000, `the whole-band decoder (${JSON.stringify(pa?.params)})`);
    ok(await until(() => count(S, /decoder attached: ORBCOMM/g) === 1, 4000), 'decoder attached: ORBCOMM — once');
    const dx = ws(S.port, '/ws/dxcluster?user_session_id=orbpk0001');
    await until(() => dx.open, 3000); await sleep(300);
    dx.send({ type: 'audio_extension_attach', extension_name: 'orbcomm', ...pa.params });
    ok(await until(() => dx.txt.some((t) => t.includes('"audio_extension_attached"')), 3000), 'the decoder socket attached');
    ok(await until(() => ostat(dx).length >= 3, 8000), `orbcomm_status once a second (${ostat(dx).length})`);
    ok(count(S, /decoder attached: ORBCOMM/g) === 1, '…and JOINED the pick\'s run (not restarted)');
    const s1 = lastOstat(dx);
    ok(s1 && s1.ended === false && s1.follow === 0, `the status carries ended / follow (${s1 && JSON.stringify({ ended: s1.ended, follow: s1.follow, pick: s1.pick, up: s1.up })})`);
    const fed1 = s1?.slice?.fedSec ?? 0, blocks1 = s1?.slice?.blocks ?? 0;
    ok(fed1 > 0 && blocks1 > 0, `IQ is fed (${fed1} s, ${blocks1} blocks)`);
    // ── 2. B from the list ──
    sp.send({ type: 'sat', op: 'pick', norad: B.norad, tx: ib, doppler: true });
    ok(await until(() => lastPicked(sp)?.norad === B.norad, 5000), `${B.name} picked from the list`);
    const pb = lastPicked(sp);
    ok(pb?.decoder === 'orbcomm' && JSON.stringify(pb?.params) === JSON.stringify(pa.params), `the same decoder, the same parameters (${JSON.stringify(pb?.params)})`);
    ok(/\[sat\] decoder orbcomm kept for the new ORBCOMM pick/.test(S.L()), '★ the log: the decoder was KEPT');
    await sleep(2500);
    ok(count(S, /decoder attached: ORBCOMM/g) === 1, `★ no second "decoder attached: ORBCOMM" (${count(S, /decoder attached: ORBCOMM/g)})`);
    ok(!/decoder orbcomm detached/.test(S.L()), '★ never detached');
    const s2 = lastOstat(dx);
    ok(s2 && s2.slice && s2.slice.fedSec > fed1 && s2.slice.blocks > blocks1, `★ the same run counts on — IQ fed ${fed1} → ${s2?.slice?.fedSec} s, blocks ${blocks1} → ${s2?.slice?.blocks} (not reset)`);
    ok(s2 && s2.good >= s1.good && s2.bad >= s1.bad, `packet counts not reset (good ${s1?.good} → ${s2?.good}, bad ${s1?.bad} → ${s2?.bad})`);
    ok(await until(() => lastOstat(dx)?.pick === B.norad || lastOstat(dx)?.pick === 0, 4000), `the status's pick follows the lock (${lastOstat(dx)?.pick})`);
    // ── 3. a joiner ──
    const j = await sock(S, 'orbjn0001');
    ok(!picked(j).length, 'a joiner hears no pick before they open SATELLITE');
    j.send({ type: 'sat_list' });
    ok(await until(() => lastPicked(j)?.norad === B.norad, 4000), `★ a joiner opening SATELLITE is told the room's pick (${lastPicked(j)?.norad}, shared ${lastPicked(j)?.shared})`);
    ok(lastPicked(j)?.decoder === 'orbcomm' && JSON.stringify(lastPicked(j)?.params) === JSON.stringify(pa.params), '…with the decoder\'s parameters, so their page JOINS it');
    j.send({ type: 'sat_list' });
    await until(() => jsons(j, 'sat_list').length >= 2, 4000); await sleep(300);
    ok(picked(j).length === 1, `…once (a second sat_list does not repeat it: ${picked(j).length})`);
    const jdx = ws(S.port, '/ws/dxcluster?user_session_id=orbjn0001');
    await until(() => jdx.open, 3000); await sleep(300);
    jdx.send({ type: 'audio_extension_attach', extension_name: 'orbcomm', ...lastPicked(j).params });
    ok(await until(() => ostat(jdx).length >= 2, 6000), 'the joiner\'s decoder socket gets the same status (the room\'s followed bird with it)');
    ok(count(S, /decoder attached: ORBCOMM/g) === 1, '…the joiner JOINED, nothing restarted');
    // ── a non-ORBCOMM pick ends it ──
    const other = (l?.sats || []).find((s) => !/^ORBCOMM/.test(s.name) && s.tx.some((t) => t.reach && t.decoder !== 'orbcomm'));
    if (other) {
      const io = other.tx.findIndex((t) => t.reach && t.decoder !== 'orbcomm');
      sp.send({ type: 'sat', op: 'pick', norad: other.norad, tx: io, doppler: false });
      ok(await until(() => lastPicked(sp)?.norad === other.norad, 5000), `${other.name} picked`);
      ok(await until(() => /decoder orbcomm detached \(SATELLITE drop: a new pick\)/.test(S.L()), 3000), '★ a non-ORBCOMM pick detaches the ORBCOMM decoder (the ordinary new pick)');
    } else ok(false, 'a non-ORBCOMM bird in the fixture');
    jdx.close(); j.close(); dx.close(); sp.close();
  } finally { S.stop(); }
} catch (e) { console.log('   FAIL threw:', e?.stack || e); fail++; }
console.log(`\n${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
