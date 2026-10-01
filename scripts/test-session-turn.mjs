// test-session-turn.mjs — a listener whose turn ran out comes back, END TO END, through the LAN.
//
// ★★★ THE FAULT (B10, Stuart on Kiko's server, 2026-10-01). A single-user receiver with a 15-minute
//     limit ended his turn and the TIME UP card said "You can try again in about 2 minutes". He was
//     still refused well after that. The cooldown (2 min) did end — but the TURN did not: a listener
//     back within the turn-break window resumed the SAME turn with no time left, was ended again on
//     the next tick (another session_expired, another 2-minute cooldown), and every such attempt
//     refreshed the turn's "last seen" so the fresh turn the card promised never came while he kept
//     trying. The card quoted a door the server had already shut.
//
// ★★ WHAT THIS PINS, on a REAL vibeserver (fake rtl_tcp, --users 1 --session-limit 1):
//     1. the turn ends with session_expired carrying cooldown, fresh, and borrow:true;
//     2. inside the cooldown the address is refused with `cooldown` — the limit is still real;
//     3. after the cooldown, on a FREE receiver with nobody waiting, the listener is ADMITTED and
//        STAYS (hwinfo says borrowed:true), rather than being ended on the spot;
//     4. the moment somebody else wants the radio, the borrowed listener gets the handover notice
//        and is then ended — so borrowed time never keeps anybody else out.
//
// ★ THE LISTENER MUST NOT BE LOOPBACK — loopback is exempt from the limit. So listener A connects
//   through this Mac's LAN address (en0); the "somebody else" (B) is loopback, which is queued like
//   anyone when the radio is full. Without a LAN address it reports NOT RUN.
// ★ SLOW: a one-minute turn plus the two-minute cooldown plus the 30 s notice, ~4 minutes. So
//   run-tests.sh runs it only with VIBESERVER_SLOW=1 and otherwise reports it NOT RUN.
// ★ SILENT: audio frames are received and discarded, never played.
//
//   usage: VIBESERVER_BIN=… node scripts/test-session-turn.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
let lan = process.env.LAN_IP || '';
if (!lan) {
  for (const list of Object.values(os.networkInterfaces())) {
    for (const a of list || []) if (a.family === 'IPv4' && !a.internal && !lan) lan = a.address;
  }
}
if (!lan) { console.log('   not run — no non-loopback IPv4 address to connect from'); process.exit(3); }

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** A minimal WebSocket client, recording every text frame (binary frames are counted, not kept). */
function ws(host, port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, host);
  const st = { txt: [], bin: 0, open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: ${host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
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
      if (op === 0x2) st.bin++;
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}
const msgs = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => JSON.parse(t));
async function listen(host, port, sid) {
  const L = { spec: ws(host, port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`) };
  await sleep(300);
  L.aud = ws(host, port, `/ws/audio?user_session_id=${sid}&codec=opus`);
  L.all = () => [...L.spec.txt, ...L.aud.txt];
  L.got = (type) => [...msgs(L.spec, type), ...msgs(L.aud, type)];
  L.close = () => { L.spec.close(); L.aud.close(); };
  return L;
}
/** Wait up to `ms` for `pred`, polling. */
async function until(pred, ms) { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (pred()) return true; await sleep(250); } return pred(); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-turn-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '100000000',
                        '--mode', 'wfm', '--users', '1', '--session-limit', '1', '--idle-grace', '0'],
                  { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, lan); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}

try {
  console.log(`── single-user receiver, 1-minute turn, listener A from ${lan} ──`);
  // 1. A takes a turn and runs it out.
  let A = await listen(lan, port, 'aaaaaaaa');
  await until(() => msgs(A.spec, 'hwinfo').length > 0, 5000);
  const hw0 = msgs(A.spec, 'hwinfo')[0] || {};
  ok(hw0.sessionSecsLeft > 0 && hw0.sessionSecsLeft <= 60 && hw0.borrowed !== true,
     `A's first turn has a clock (${hw0.sessionSecsLeft}s) and is not borrowed`);
  const ended = await until(() => A.got('session_expired').length > 0, 75_000);
  const exp = A.got('session_expired')[0] || {};
  ok(ended, `A's turn ends with session_expired (${JSON.stringify(exp)})`);
  ok(exp.cooldown > 0, `...which states the cooldown (${exp.cooldown}s)`);
  ok(exp.fresh === 900, `...and states when a FULL turn returns — the real 15-minute turn break, not the limit (${exp.fresh}s)`);
  ok(exp.borrow === true, '...and that coming back after the cooldown is allowed when the radio is free (borrow:true)');
  const tEnd = Date.now();
  A.close();
  await sleep(1500);

  // 2. Inside the cooldown: refused — the limit is still real.
  A = await listen(lan, port, 'aaaaaaaa');
  await until(() => A.got('cooldown').length > 0 || A.got('hwinfo').length > 0, 5000);
  const cd = A.got('cooldown')[0];
  ok(!!cd && A.got('hwinfo').length === 0, `inside the cooldown A is refused (${JSON.stringify(cd)})`);
  A.close();

  // 3. After the cooldown, radio free, nobody waiting: A is let back in, and STAYS.
  const wait = (exp.cooldown || 120) * 1000 - (Date.now() - tEnd) + 3000;
  console.log(`   … waiting ${Math.round(wait / 1000)} s for the cooldown to end`);
  await sleep(Math.max(0, wait));
  A = await listen(lan, port, 'aaaaaaaa');
  await until(() => A.got('hwinfo').length > 0 || A.got('cooldown').length > 0 || A.got('session_expired').length > 0, 5000);
  const hw1 = A.got('hwinfo')[0];
  ok(!!hw1 && A.got('cooldown').length === 0, 'after the cooldown A is ADMITTED to the free receiver');
  ok(!!hw1 && hw1.borrowed === true, `...on borrowed time, and told so (borrowed:${hw1 && hw1.borrowed}, secsLeft ${hw1 && hw1.sessionSecsLeft})`);
  await sleep(12_000);
  ok(A.got('session_expired').length === 0 && !A.spec.closed,
     'A is NOT ended while nobody else wants the radio (12 s on borrowed time)');
  ok(A.spec.bin > 20, `...and is being served the spectrum all the while (${A.spec.bin} frames)`);

  // 4. Somebody else arrives: A gets notice, then goes; B gets the radio.
  const B = await listen('127.0.0.1', port, 'bbbbbbbb');
  const notice = await until(() => A.got('session_handover').length > 0, 8000);
  ok(notice, `B waiting: A is given the handover notice (${JSON.stringify(A.got('session_handover')[0] || null)})`);
  ok(notice && A.got('session_expired').length === 0, '...BEFORE the ending — the notice is not an obituary');
  const gone = await until(() => A.got('session_expired').length > 0, 45_000);
  const exp2 = A.got('session_expired')[0] || {};
  ok(gone, `...and is then ended (${JSON.stringify(exp2)})`);
  // ★ A waiter is HELD, then told `your_turn` and reconnects into its reservation — as the clients do.
  const turn = await until(() => B.got('your_turn').length > 0, 15_000);
  ok(turn, 'B, who was waiting, is told it is their turn');
  B.close(); A.close();
  await sleep(300);
  const B2 = await listen('127.0.0.1', port, 'bbbbbbbb');
  const bIn = await until(() => B2.got('hwinfo').length > 0, 8000);
  ok(bIn, '…and gets the radio');
  B2.close();
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
}
console.log(`   ${pass} passed, ${fail} failed   (server log: ${path.join(dir, 'server.log')})`);
process.exit(fail ? 1 : 0);
