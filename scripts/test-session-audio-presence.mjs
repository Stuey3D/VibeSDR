// test-session-audio-presence.mjs — an AUDIO SOCKET WITH NO WATERFALL keeps its listener's turn (2026-10-28).
//
// ★★★ THE FAULT (Stuart, V12 two servers at once, iPhone): "because it is taking me through the front door each time
//     to switch servers it is resetting the time limit each time so I've found a sneaky time limit bypass here". The
//     server in the BACKGROUND of a pairing plays on its audio socket alone — the waterfall closes with its screen, as
//     it does on a locked phone — and the shared dial's limit walked spectrum sockets only. The turn book read the
//     whole background time as AWAY: on the waterfall's return the clock had not moved, and after the 15-minute break
//     it was a FRESH turn. A hard limit could not end an audio-only listener at all.
// Pinned here on a shared dial with a HARD one-minute limit:
//   A  waterfall closes after 5 s, audio plays on → warned, then ENDED on its audio socket at the end of the ORIGINAL
//      turn, and refused inside the cooldown;
//   B  waterfall closes at 3 s and comes back at ~25 s (a swap back) → the countdown it is given is what is LEFT of the
//      turn (~35 s), not a fresh minute — the time in the background was counted;
//   C  a different listener id from the SAME address arriving at ~25 s → a full minute of its own (two people behind
//      one NAT are two turns, not one).
// ★ The listeners must not be loopback (exempt), so they connect through the LAN address.
//   usage: VIBESERVER_BIN=… node scripts/test-session-audio-presence.mjs   (~80 s)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
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
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = buf.subarray(0, 12).toString().includes(' 101'); buf = buf.subarray(j + 4); }
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
const specPath = (sid, bid) => `/ws/user-spectrum?user_session_id=${sid}&bins=256&bid=${bid}`;
const audPath = (sid, bid) => `/ws/audio?user_session_id=${sid}&codec=opus&bid=${bid}`;
/** The countdown the server hands a spectrum socket at connect (hwinfo sessionSecsLeft). */
const secsLeft = (st) => { const h = msgs(st, 'hwinfo').find((m) => m.sessionSecsLeft != null); return h ? h.sessionSecsLeft : null; };
async function until(pred, ms) { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (pred()) return true; await sleep(250); } return pred(); }

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-audturn-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const log = fs.openSync(path.join(dir, 'server.log'), 'w');
// ★ A SHARED DIAL (several listeners, no locked centre) with a HARD one-minute limit — the Pi 2's shape.
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '7100000',
                        '--mode', 'usb', '--users', '4', '--session-limit', '1', '--idle-grace', '0'],
                  { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, lan); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}

try {
  console.log(`── shared dial, hard 1-minute limit, listeners from ${lan} ──`);
  const t0 = Date.now();
  const bidA = 'aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa', bidB = 'bbbbbbbb-2222-4222-8222-bbbbbbbbbbbb';
  const bidC = 'cccccccc-3333-4333-8333-cccccccccccc';
  // A and B arrive together: waterfall first, then audio (either order is fine; the web's order).
  const As = ws(lan, port, specPath('a1a1a1a1', bidA));
  const Bs = ws(lan, port, specPath('b2b2b2b2', bidB));
  await sleep(300);
  const Aa = ws(lan, port, audPath('a1a1a1a1', bidA));
  const Ba = ws(lan, port, audPath('b2b2b2b2', bidB));
  ok(await until(() => secsLeft(As) != null && secsLeft(Bs) != null, 5000), 'A and B connect with a countdown');
  ok(secsLeft(As) >= 55 && secsLeft(As) <= 60, `A starts a full minute (${secsLeft(As)} s)`);
  await sleep(2500);
  Bs.close();                                 // B's screen goes to the background: audio plays on
  await sleep(2000);
  As.close();                                 // and A's
  await sleep(18_000);
  ok(!Aa.closed && msgs(Aa, 'session_expired').length === 0, 'audio-only A, inside the turn: still playing');

  // ★ B comes back to the front (~25 s in): the countdown is what is LEFT, not a fresh minute.
  const Bs2 = ws(lan, port, specPath('b2b2b2b2', bidB));
  await until(() => secsLeft(Bs2) != null, 5000);
  const atB = Math.round((Date.now() - t0) / 1000);
  const leftB = secsLeft(Bs2);
  ok(leftB != null && leftB > 0 && leftB <= 60 - atB + 6,
     `★ B's waterfall back after ${atB} s: ${leftB} s left — the background time WAS counted (before: a full turn's worth, ~60)`);
  // ★ A different listener behind the SAME address: their own, full turn.
  const Cs = ws(lan, port, specPath('c3c3c3c3', bidC));
  await until(() => secsLeft(Cs) != null, 5000);
  ok(secsLeft(Cs) >= 55, `C, another listener id from the same address, gets a full minute of its own (${secsLeft(Cs)} s)`);

  // ★ A stays audio-only: warned on the audio socket, then ended at the end of the ORIGINAL turn.
  const ended = await until(() => msgs(Aa, 'session_expired').length > 0 || Aa.closed, 60_000);
  const at = Math.round((Date.now() - t0) / 1000);
  ok(msgs(Aa, 'session_warning').length > 0, `audio-only A was WARNED on its audio socket (${msgs(Aa, 'session_warning').map((m) => m.secs).join(', ')} s)`);
  ok(ended && msgs(Aa, 'session_expired').length > 0,
     `★ audio-only A is still ON THE TURN and the hard limit ends it, saying why (${at} s after arrival) — before, it played for ever`);
  ok(at >= 55 && at <= 75, `...at the end of A's ORIGINAL turn, not a fresh one (${at} s)`);
  await until(() => Aa.closed, 5000);
  ok(Aa.closed, '...and the audio socket is closed');
  ok(!Cs.closed && msgs(Cs, 'session_expired').length === 0, 'C, on its own turn, is untouched by A ending');
  // ★ And it cannot simply reconnect for a fresh minute: the cooldown holds.
  const Aa2 = ws(lan, port, audPath('a1a1a1a1', bidA));
  const As2 = ws(lan, port, specPath('a1a1a1a1', bidA));
  await until(() => As2.closed || secsLeft(As2) != null, 4000);
  ok(As2.closed || !As2.open || msgs(As2, 'session_expired').length > 0 || (secsLeft(As2) ?? 99) < 5,
     'A reconnecting inside the cooldown does NOT get a fresh turn');
  for (const s of [Aa2, As2, Cs, Bs2, Ba]) s.close();
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
}
console.log(`   ${pass} passed, ${fail} failed   (server log: ${path.join(dir, 'server.log')})`);
process.exit(fail ? 1 : 0);
