// test-server-shared-decoders.mjs — ★★★ A DECODER ON A SHARED DIAL IS THE ROOM'S, through the REAL server (Stuart 2026-11-24):
//   "if the aprs decoder is open for user 1 then user 2 connecting it should be open for, same with all decoders. then if
//    user 2 closes it then it closes for them only"
// A fake rtl_tcp plays keyed APRS packets on 144.800 (test-aprs-onair --write-clip, as test-server-aprs-shared.mjs). Rows:
//   A. a SHARED DIAL (users 4): listener 1 opens APRS → told {dec_shared} (held, mine); listener 2 ARRIVES and is told what
//      runs with its parameters; listener 2 attaches EXACTLY them → JOINS (nothing restarts) and gets packets; listener 2
//      closes → listener 1 still decoding, listener 2's socket gets nothing more; listener 1 closes → the decoder stopped,
//      the room told (an empty list).
//   B. the starter LEAVES (every socket closed) while listener 2 holds it: it runs on for listener 2, whose view says it is
//      now theirs (the admin credit moved).
//   C. the spots run (subscribe_digital_spots): held by two, one unsubscribes → still running, the last → gone.
//   D. a SATELLITE pick's decoder on the shared dial: never listed (the pick tells the room), and a listener who joined it
//      and closes their box does not stop it under the pick.
//   E. a listener's own VFO (users 4, range-locked): no dec_shared at all (each listener has their own host).
// Silent: an audio socket is opened only so a listener has audio; its frames are dropped unread.
//   VIBESERVER_BIN=…/vibeserver VIBE_APRS_ONAIR_BIN=…/test-aprs-onair node scripts/test-server-shared-decoders.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const GEN = process.env.VIBE_APRS_ONAIR_BIN || '';
if (!BIN || !fs.existsSync(BIN) || !GEN || !fs.existsSync(GEN)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_APRS_ONAIR_BIN (the test-aprs-onair binary, which writes the clip)');
  process.exit(3);
}
const RATE = 1200000, DIAL = 144800000, N = 10;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.open = false; });
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
      if (op === 0x1 && st.txt.length < 20000) st.txt.push(p.toString());
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
const packets = (dx) => jsons(dx, 'aprs_packet').filter((p) => /on-air test #\d+/.test(p.data?.raw || '')).length;
/** The room's view as this spectrum socket last heard it (null = never told). */
const room = (sp) => { const l = jsons(sp, 'dec_shared'); return l.length ? l[l.length - 1].list : null; };
const roomDec = (sp) => (room(sp) || []).find((e) => e.slot === 'decoder') || null;
const roomSpots = (sp) => (room(sp) || []).find((e) => e.slot === 'spots') || null;

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-shared-dec-'));
const clip = (() => {
  const f = path.join(tmp, 'clean.cu8');
  const r = spawnSync(GEN, ['--write-clip', f, String(RATE), '15', String(N), '0', '0', '3000', '0', '5'], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`test-aprs-onair --write-clip failed: ${r.stderr}`);
  return f;
})();
function freshSats() {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-6digit-2026-11-19.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  return JSON.stringify(d);
}
async function server(tag, { users = 4, locked = false, sats = false } = {}) {
  const dir = fs.mkdtempSync(path.join(tmp, `${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  if (sats) {
    fs.mkdirSync(path.join(dir, 'data/sat'));
    fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
    const t = Math.floor(Date.now() / 1000);
    fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), `checked ${t}\nattempt ${t}\nseed 0\n`);
  }
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'Shared decoders test', locator: 'IO92nh', adminPass: 'shared-decoders-test-admin',
    radios: [{ serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users }],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clip, '--hf-centre', String(DIAL)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const logPath = path.join(dir, 'server.log');
  const log = fs.openSync(logPath, 'w');
  const args = ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'nfm', '--users', String(users)];
  if (locked) args.push('--lock-freq', String(DIAL));
  const srv = spawn(BIN, args, { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, logText: () => fs.readFileSync(logPath, 'utf8'), stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}
async function listener(S, sid) {
  const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`);
  await sleep(300);
  const au = ws(S.port, `/ws/audio?user_session_id=${sid}&proto=1`);
  const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(600);
  return { sp, au, dx, close: () => { sp.close(); au.close(); dx.close(); } };
}
// ★ the web page's shape (params spread at the top level — src/services/packet.ts packetParams('auto'))
const ATTACH = { type: 'audio_extension_attach', extension_name: 'aprs', baud_rate: 1200, packet: 'auto' };
/** Join what the room says runs — the app's shape (params nested), as SDRScreen's follow sends it. */
const joinMsg = (e) => ({ type: 'audio_extension_attach', extension_name: e.ext, params: e.params });

{
  console.log('── A. shared dial: 1 opens APRS, 2 arrives + joins, 2 closes, 1 closes ──');
  const S = await server('shared');
  try {
    const L1 = await listener(S, 'shdec01');
    ok(Array.isArray(room(L1.sp)) && room(L1.sp).length === 0, 'an arrival on a quiet radio is told: nothing runs (an empty list)');
    L1.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L1.dx.send(ATTACH);
    ok(await until(() => roomDec(L1.sp)?.ext === 'aprs', 4000), 'listener 1 is told the room runs APRS');
    const e1 = roomDec(L1.sp);
    ok(e1?.held === true && e1?.mine === true && e1?.n === 1, `…held and theirs (${JSON.stringify(e1)})`);
    ok(await until(() => packets(L1.dx) >= 2, 30000), `listener 1 decodes (${packets(L1.dx)} packets)`);

    const L2 = await listener(S, 'shdec02');
    const e2 = roomDec(L2.sp);
    ok(e2?.ext === 'aprs' && e2.held === false && e2.mine === false, `listener 2 ARRIVING is told what runs (${JSON.stringify(e2)})`);
    ok(e2?.params?.baud_rate === 1200 && e2?.params?.packet === 'auto', 'with the parameters that join it');
    L2.dx.send(joinMsg(e2));
    ok(await until(() => roomDec(L2.sp)?.held === true, 4000), 'listener 2 joined — now holds it');
    ok(await until(() => roomDec(L1.sp)?.n === 2, 4000), 'the room is told: two hold it');
    ok(roomDec(L2.sp)?.key === e1?.key, 'the SAME run (the key did not change — nothing restarted)');
    ok(await until(() => packets(L2.dx) >= 2, 30000), `listener 2 gets packets (${packets(L2.dx)})`);

    // ★ listener 2 closes their box
    L2.dx.send({ type: 'audio_extension_detach' });
    ok(await until(() => roomDec(L2.sp)?.held === false && roomDec(L1.sp)?.n === 1, 4000), 'listener 2 closed: no longer holds it, still listed for them');
    await sleep(1500);
    const p1 = packets(L1.dx), p2 = packets(L2.dx);
    ok(await until(() => packets(L1.dx) >= p1 + 2, 30000), `listener 1 STILL decoding after listener 2 closed (+${packets(L1.dx) - p1})`);
    ok(packets(L2.dx) === p2, `listener 2's socket gets nothing more (its box is shut): +${packets(L2.dx) - p2}`);
    ok(/one listener let go — kept for the others/.test(S.logText()), 'the server log says: kept for the others');

    // ★ listener 1 closes: the last
    L1.dx.send({ type: 'audio_extension_detach' });
    ok(await until(() => Array.isArray(room(L2.sp)) && room(L2.sp).length === 0 && room(L1.sp).length === 0, 4000), 'listener 1 closed: the room is told nothing runs');
    ok(/the last listener holding it let go — stopped/.test(S.logText()), 'the server log says: the last let go — stopped');
    await sleep(1500);
    const q1 = packets(L1.dx);
    await sleep(8000);
    ok(packets(L1.dx) === q1, 'no packets after the decoder stopped');
    L1.close(); L2.close();
  } finally { S.stop(); }
}
{
  console.log('── B. the starter leaves while another holds it ──');
  const S = await server('leave');
  try {
    const L1 = await listener(S, 'shdec11');
    L1.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L1.dx.send(ATTACH);
    ok(await until(() => roomDec(L1.sp)?.ext === 'aprs', 4000), 'listener 1 opened APRS');
    const L2 = await listener(S, 'shdec12');
    L2.dx.send(joinMsg(roomDec(L2.sp)));
    ok(await until(() => roomDec(L2.sp)?.held === true, 4000), 'listener 2 joined');
    L1.close();
    ok(await until(() => roomDec(L2.sp)?.n === 1 && roomDec(L2.sp)?.mine === true, 6000), `listener 1 gone: it runs on for listener 2, now theirs (${JSON.stringify(roomDec(L2.sp))})`);
    const p = packets(L2.dx);
    ok(await until(() => packets(L2.dx) >= p + 2, 30000), `listener 2 still decoding (+${packets(L2.dx) - p})`);
    L2.close();
  } finally { S.stop(); }
}
{
  console.log('── C. the spots run is the room\'s too ──');
  const S = await server('spots');
  try {
    const L1 = await listener(S, 'shdec21'), L2 = await listener(S, 'shdec22');
    L1.dx.send({ type: 'subscribe_digital_spots' });
    ok(await until(() => roomSpots(L2.sp)?.held === false && roomSpots(L1.sp)?.held === true, 4000), 'listener 1 subscribes: listener 2 told the spots run');
    L2.dx.send({ type: 'subscribe_digital_spots', ...roomSpots(L2.sp).params });
    ok(await until(() => roomSpots(L1.sp)?.n === 2, 4000), 'listener 2 joins it (2 hold it)');
    L2.dx.send({ type: 'unsubscribe_digital_spots' });
    ok(await until(() => roomSpots(L1.sp)?.n === 1 && roomSpots(L2.sp)?.held === false, 4000), 'listener 2 unsubscribes: still running for listener 1');
    L1.dx.send({ type: 'unsubscribe_digital_spots' });
    ok(await until(() => !roomSpots(L1.sp) && !roomSpots(L2.sp), 4000), 'listener 1 unsubscribes: gone');
    L1.close(); L2.close();
  } finally { S.stop(); }
}
{
  console.log('── D. a SATELLITE pick\'s decoder on the shared dial ──');
  const S = await server('sat', { sats: true });
  try {
    const L1 = await listener(S, 'shdec31'), L2 = await listener(S, 'shdec32');
    const picked = () => jsons(L2.sp, 'sat').filter((m) => m.state === 'picked');
    L1.sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 2, doppler: true });   // ★ ISS APRS 145.825 (test-server-sat.mjs)
    ok(await until(() => picked().length > 0, 6000), 'ISS APRS picked — the room told by the pick ("shared")');
    await sleep(800);
    ok(/\[sat\] decoder aprs attached for the shared dial/.test(S.logText()), 'the pick attached its decoder');
    ok(!roomDec(L1.sp) && !roomDec(L2.sp), 'not listed in dec_shared (the pick tells the room — no double follow)');
    const pk = picked().pop();
    L2.dx.send({ type: 'audio_extension_attach', extension_name: pk.decoder, ...(pk.params || {}) });   // ★ the web page's satFollow
    await sleep(800);
    L2.dx.send({ type: 'audio_extension_detach' });
    await sleep(800);
    ok(!/the last listener holding it let go — stopped/.test(S.logText()), 'listener 2 closing the pick\'s box does not stop the pick\'s decoder');
    L1.sp.send({ type: 'sat', op: 'drop' });
    ok(await until(() => /\[sat\] decoder aprs detached/.test(S.logText()), 4000), 'the drop ends it');
    L1.close(); L2.close();
  } finally { S.stop(); }
}
{
  console.log('── E. a listener\'s own VFO (range-locked): no dec_shared ──');
  const S = await server('own', { locked: true });
  try {
    const L1 = await listener(S, 'shdec41'), L2 = await listener(S, 'shdec42');
    L1.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L1.dx.send(ATTACH);
    await sleep(2500);
    ok(room(L1.sp) === null && room(L2.sp) === null, 'nobody is told a room decoder (each listener has their own host)');
    L1.close(); L2.close();
  } finally { S.stop(); }
}

fs.rmSync(tmp, { recursive: true, force: true });
console.log(`\ntest-server-shared-decoders: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
