// test-ask-before-tuning.mjs — ASK BEFORE TUNING, end to end through a real vibeserver (docs/TUNING-ETIQUETTE.md).
//
// ★★★ WHAT THE UNIT TESTS CANNOT SHOW: that the SERVER refuses a locked listener's tune at the socket (an OLD client
//     that knows nothing of the lock simply does not tune, and is told why), that the dial message carries the lock to
//     each listener, and that a chat line moves it — on the wire, with real sockets. The rule itself is pinned by
//     vibeserver/test-tune-etiquette.cpp; the clients' reader by scripts/test_ask_before_tuning.ts.
// ★ SILENT: no browser, no audio device — the fake dongle's samples go to sockets nobody plays.
// usage: VIBESERVER_BIN=… node scripts/test-ask-before-tuning.mjs   (~75 s)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)');
  process.exit(3);
}

/** A minimal WebSocket client (test-chat-phrases.mjs's), text frames only. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
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
  /** The last message of a type, parsed. */
  st.last = (type) => { for (let i = st.txt.length - 1; i >= 0; i--) if (st.txt[i].includes(`"type":"${type}"`)) return JSON.parse(st.txt[i]); return null; };
  st.count = (type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).length;
  return st;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ask-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
// ★ TEST-ONLY: the grant's idle lapse (kGrantIdleS, 3 minutes) shortened to GRANT_IDLE_S so the suite can watch it.
const GRANT_IDLE_S = 10;
// ★ TEST-ONLY: the place in the order is kept across a reconnect for 2 minutes (kGoneForgetS); shortened to RANK_FORGET_S.
const RANK_FORGET_S = 4;
const env = { ...process.env, VIBE_ASK_GRANT_IDLE_S: String(GRANT_IDLE_S), VIBE_ASK_RANK_FORGET_S: String(RANK_FORGET_S), VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const log = fs.openSync(logPath, 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000',
                        '--mode', 'wfm', '--users', '4', '--ask-before-tuning'], { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
const join = async (sid, bid) => { const s = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256${bid ? `&bid=${bid}` : ''}`); await sleep(900); return s; };
const vfo = (st) => { const c = st.last('config'); return c ? Number(c.vfo) : NaN; };
const until = async (f, ms = 4000) => { for (let t = 0; t < ms; t += 100) { if (f()) return true; await sleep(100); } return f(); };

try {
  console.log('── alone, nothing is locked ──');
  const a = await join('aaaa0001');
  const da = a.last('dial');
  ok(!!da && da.askFirst !== true && da.tuneLocked !== true, 'a lone listener is told no lock (askFirst absent: one listener is not company)');

  console.log('── a newcomer is locked; the one who was here first is not ──');
  const b = await join('bbbb0002');
  await until(() => a.last('dial')?.askFirst === true);
  ok(b.last('dial')?.askFirst === true && b.last('dial')?.tuneLocked === true, '★ the newcomer\'s dial says askFirst + tuneLocked');
  ok(a.last('dial')?.askFirst === true && a.last('dial')?.tuneLocked === false, '★ the longest-connected listener is in force but free');

  console.log('── the SERVER refuses a locked tune, however the client was written ──');
  const before = vfo(a);
  b.send({ type: 'tune', frequency: 97000000, mode: 'wfm' });   // ★ what an OLD client does: just send it
  await sleep(1200);
  ok(vfo(a) === before && before === 96100000, `★ the radio did not move for anybody (still ${before})`);
  const n = b.last('notice');
  ok(!!n && n.ask === 1 && /ask in chat before tuning/i.test(n.why || ''), `★ the old client is told why: "${n && n.why}"`);
  ok(vfo(b) === 96100000, '…and handed the room\'s state back, so it re-adopts where the radio is');
  const notices = b.count('notice');
  b.send({ type: 'tune', frequency: 97100000 }); await sleep(400);
  b.send({ type: 'tune', frequency: 97200000 }); await sleep(600);
  ok(b.count('notice') === notices, 'a stream of refused tunes is ONE notice per 10 s, not a pill each');
  const n2 = b.count('notice');
  b.send({ type: 'tune', frequency: 96100000, mode: 'wfm' }); await sleep(600);
  ok(b.count('notice') === n2, 'a re-assert of where the radio already is is dropped in silence (recovery is not a user action)');

  console.log('── and the room\'s receiver settings are locked too (Stuart, 2026-10-21: "lock gain too") ──');
  const refusedLines = (type, sid) => fs.readFileSync(logPath, 'utf8').split('\n')
    .filter((l) => l.includes(`${type} from session [${sid}] refused`)).length;
  for (const m of [{ type: 'gain', value: 200 }, { type: 'agc', on: true }, { type: 'stereo', on: false },
                   { type: 'nr', on: true, strength: 1 }, { type: 'deemph', tau: 75e-6 }, { type: 'rsp_control', lna: 3 },
                   { type: 'tunerbw', value: 1000000 }]) {
    b.send(m); await sleep(250);
    ok(refusedLines(m.type, 'bbbb0002') >= 1, `★ a locked listener's ${m.type} is refused at the socket`);
  }
  const zoomBefore = refusedLines('zoom', 'bbbb0002');
  b.send({ type: 'zoom', frequency: 96100000, binBandwidth: 500 }); await sleep(300);
  ok(refusedLines('zoom', 'bbbb0002') === zoomBefore, 'zoom / pan stays free ("that shouldn\'t move the tuning or audio")');
  console.log('── …but a held-back listener\'s zoom is a VIEW, never a tune (Lenovo, 2026-10-30: zoom + free:1 moved the RSP) ──');
  // ★ rfCentre from a full-state reply (`state` is not gated): where the HARDWARE is, not where anybody's view is.
  const rfNow = async (s) => { const k = s.count('hwinfo'); s.send({ type: 'state' }); await until(() => s.count('hwinfo') > k, 2000);
                               return Number(s.last('hwinfo')?.rfCentre); };
  const viewOnlyLines = () => fs.readFileSync(logPath, 'utf8').split('\n')
    .filter((l) => l.includes('zoom from a listener ask-before-tuning is holding back')).length;
  const rf0 = await rfNow(a), vo0 = viewOnlyLines();
  b.send({ type: 'zoom', frequency: 94975000, binBandwidth: 500, free: 1 }); await sleep(500);   // ★ the Lenovo's frame
  b.send({ type: 'zoom', frequency: 98500000, binBandwidth: 2000, free: 1 }); await sleep(500);
  b.send({ type: 'zoom', frequency: 93000000, binBandwidth: 500 }); await sleep(500);              // ★ a LOCKED pan, too
  const rf1 = await rfNow(a);
  ok(Number.isFinite(rf0) && rf1 === rf0, `★ a refused listener's zoom/free off the capture did not move the hardware centre (${rf0} -> ${rf1})`);
  ok(viewOnlyLines() - vo0 >= 3, '…each one kept inside the capture as a view, and the log says so');
  ok(vfo(a) === 96100000, '…and nobody\'s dial moved');
  const sqlBefore = refusedLines('squelch', 'bbbb0002');
  b.send({ type: 'squelch', db: -60, on: true }); await sleep(300);
  ok(refusedLines('squelch', 'bbbb0002') === sqlBefore, 'squelch stays free — it is this listener\'s own gate');
  a.send({ type: 'stereo', on: true }); await sleep(300);
  ok(refusedLines('stereo', 'aaaa0001') === 0, 'the longest-connected listener may still change the receiver');

  console.log('── the free listener tunes as ever ──');
  a.send({ type: 'tune', frequency: 96600000, mode: 'wfm' });
  ok(await until(() => vfo(b) === 96600000), '★ the longest-connected listener moved the dial, and the newcomer followed');

  console.log('── asking: a NO holds, the sayer\'s YES lets them in ──');
  b.send({ type: 'say', id: 'ask_tune' });
  ok(await until(() => b.last('dial')?.askState === 'open'), '"Can I tune?" opens the ask — the newcomer is told it is waiting');
  ok((b.last('dial')?.askSecs ?? 0) > 10, `…with the seconds left (${b.last('dial')?.askSecs})`);
  a.send({ type: 'say', id: 'listening_few_min' });
  ok(await until(() => b.last('dial')?.askState === 'no'), '★ "I\'m listening to this one — a few more minutes, please?" is a NO');
  ok((b.last('dial')?.askSecs ?? 0) > 280, `…held for the five-minute limit (${b.last('dial')?.askSecs} s left)`);
  b.send({ type: 'tune', frequency: 98000000 }); await sleep(800);
  ok(vfo(a) === 96600000, 'still refused while the NO stands');
  await sleep(3200);                                               // flood control: one line per 3 s per session
  a.send({ type: 'say', id: 'go_ahead' });
  ok(await until(() => b.last('dial')?.tuneLocked === false), '★ "Go ahead, tune" from the one who said no: let in at once');
  b.send({ type: 'tune', frequency: 98000000, mode: 'wfm' });
  ok(await until(() => vfo(a) === 98000000), '★ …and now their tune moves the radio for everybody');
  // ★ Let in, the same frame is a tune and moves the centre: the permission, not the frame, decides.
  const rfG0 = await rfNow(a), voG0 = viewOnlyLines();
  b.send({ type: 'zoom', frequency: 100500000, binBandwidth: 500, free: 1 }); await sleep(800);
  const rfG1 = await rfNow(a);
  ok(Number.isFinite(rfG1) && Math.abs(rfG1 - rfG0) > 100000 && viewOnlyLines() === voG0,
     `★ a permitted listener's zoom/free still moves the hardware centre (${rfG0} -> ${rfG1})`);
  b.send({ type: 'zoom', frequency: 98000000, binBandwidth: 500 }); await sleep(500);   // back over the dial for what follows
  const gainBefore = refusedLines('gain', 'bbbb0002');
  b.send({ type: 'gain', value: 300 }); await sleep(400);
  ok(refusedLines('gain', 'bbbb0002') === gainBefore, '…and their gain is accepted too');

  console.log(`── a grant lapses when left on a station (Stuart, 2026-10-22; idle lapse shortened to ${GRANT_IDLE_S} s) ──`);
  const gs = b.last('dial')?.askGrantSecs;
  ok(typeof gs === 'number' && gs > 0 && gs <= GRANT_IDLE_S, `★ the granted listener's dial carries askGrantSecs (${gs})`);
  ok(a.last('dial')?.askGrantSecs === undefined, '…the top listener\'s does not (no grant to lose)');
  // ★ Actively tuning: an accepted change every 3 s for 1.6× the lapse — never locked.
  const tActive = Date.now();
  let f = 98000000, stayedUp = true, tLastTune = 0;
  while (Date.now() - tActive < GRANT_IDLE_S * 1600) {
    f += 100000; b.send({ type: 'tune', frequency: f, mode: 'wfm' }); tLastTune = Date.now(); await sleep(3000);
    if (b.last('dial')?.tuneLocked === true) stayedUp = false;
  }
  ok(stayedUp && vfo(a) === f, `★ actively tuning for ${((Date.now() - tActive) / 1000).toFixed(0)} s: the grant stays up (radio at ${vfo(a)})`);
  // ★ Left on a station: zoom and squelch are free and do not count — the grant lapses all the same.
  const zoomer = setInterval(() => { b.send({ type: 'zoom', frequency: f, binBandwidth: 500 }); b.send({ type: 'squelch', db: -60, on: false }); }, 1500);
  const lapsed = await until(() => b.last('dial')?.tuneLocked === true, (GRANT_IDLE_S + 6) * 1000);
  clearInterval(zoomer);
  const idleS = (Date.now() - tLastTune) / 1000;   // ★ from their LAST accepted change; the 2 s tick adds up to 2 s
  ok(lapsed && idleS >= GRANT_IDLE_S - 0.5, `★ left on a station (zooming only): locked again ${idleS.toFixed(1)} s after their last tune`);
  ok(b.last('dial')?.askFirst === true && !b.last('dial')?.askState && b.last('dial')?.askGrantSecs === undefined,
     '…exactly like a newcomer: no ask open, no NO, no grant countdown');
  b.send({ type: 'tune', frequency: 99000000, mode: 'wfm' }); await sleep(1000);
  ok(vfo(a) === f, '…and their tune is refused again');
  // ★ They ask again: the 15 s rule is unchanged. a YES lets them back in at once.
  await sleep(3200);
  b.send({ type: 'say', id: 'ask_tune' });
  ok(await until(() => b.last('dial')?.askState === 'open'), 'asking again opens a fresh window');
  a.send({ type: 'say', id: 'go_ahead' });
  ok(await until(() => b.last('dial')?.tuneLocked === false), '…and a YES lets them back in');

  console.log('── no answer in 15 s lets a newcomer in ──');
  const c = await join('cccc0003');
  ok(await until(() => c.last('dial')?.tuneLocked === true), 'a third listener arrives locked');
  c.send({ type: 'say', id: 'anyone_using' });
  ok(await until(() => c.last('dial')?.askState === 'open'), '"Anyone using this?" asks too');
  const t0 = Date.now();
  ok(await until(() => c.last('dial')?.tuneLocked === false, 20000), `★ nobody answered: let in after ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  ok(Date.now() - t0 >= 13000, '…and not before the 15 s were up');

  console.log('── signing in as admin is the way out, at once (Stuart: "Jr needs to be able to sign in as admin too") ──');
  // ★ This server has no password, so an unlock is granted as it is on any server without one.
  const d = await join('dddd0004');
  ok(await until(() => d.last('dial')?.tuneLocked === true), 'a fourth listener arrives locked');
  const tAdmin = Date.now();
  d.send({ type: 'admin_unlock', nonce: '', token: '' });
  ok(await until(() => d.last('dial')?.tuneLocked === false, 1500), `★ signed in: unlocked from the next dial state (${Date.now() - tAdmin} ms, not the 2 s tick)`);
  ok(await until(() => a.last('dial')?.tuneLocked === true, 1500), '★ …and the admin now outranks the longest-connected listener, who is locked');
  d.send({ type: 'gain', value: 250 }); await sleep(400);
  ok(refusedLines('gain', 'dddd0004') === 0, 'an admin\'s gain is never refused');
  // ★★ Stuart, 2026-10-21: "admin mode must have full control at all time … including tuning etc".
  //    An admin outranks the longest-connected listener (locked just above), and still tunes at once, no asking.
  d.send({ type: 'tune', frequency: 99900000, mode: 'wfm' });
  ok(await until(() => vfo(a) === 99900000), '★ the admin tunes at once, without asking, and everyone follows');
  d.send({ type: 'stereo', on: false }); await sleep(400);
  for (const t of ['tune', 'stereo'])
    ok(refusedLines(t, 'dddd0004') === 0, `an admin's ${t} is never refused`);
  d.close();
  ok(await until(() => a.last('dial')?.tuneLocked === false, 6000), 'the admin leaves: the longest-connected listener is top again');

  console.log('── when the room empties, the lock goes ──');
  b.close(); c.close();
  ok(await until(() => a.last('dial')?.askFirst !== true, 6000), 'alone again: the dial says no lock');
  a.close();
  await sleep(1500);

  // ★★★ Stuart, 2026-10-22: his app left and came back 12 s later with full control over a friend who had joined
  //     after him — the place was the session limit's, remembered for 15 minutes away. "It would be a bit unfair for
  //     a user to disconnect for 14 mins then come back with full control." Now a blip keeps the place; longer than
  //     2 minutes away (here RANK_FORGET_S) and the returner is the newcomer. Each has its own browser id, as the
  //     web client and the app send, so they are two people on one address.
  console.log(`── leaving keeps your place for a blip, not longer (place kept ${RANK_FORGET_S} s here, 2 minutes on a server) ──`);
  let e1 = await join('eeee0001', 'browserE');
  const f1 = await join('ffff0001', 'browserF');
  ok(await until(() => f1.last('dial')?.tuneLocked === true) && e1.last('dial')?.tuneLocked === false, 'the first arrival is free, the second locked');
  e1.close(); await sleep(1000);
  e1 = await join('eeee0002', 'browserE');   // ★ a new session id, as the app makes on a reconnect
  ok(await until(() => e1.last('dial')?.askFirst === true) && await until(() => e1.last('dial')?.tuneLocked === false),
     '★ back after a 1 s blip with a new session: still first, still free');
  ok(f1.last('dial')?.tuneLocked === true, '…and the friend is still the one who asks');
  e1.close();
  await sleep((RANK_FORGET_S + 2) * 1000);
  e1 = await join('eeee0003', 'browserE');
  ok(await until(() => e1.last('dial')?.tuneLocked === true), `★ back after ${RANK_FORGET_S + 2} s away: the returner is the newcomer and is locked`);
  ok(await until(() => f1.last('dial')?.tuneLocked === false), '★ …and the friend who stayed is now the one in charge');
  e1.close(); f1.close();
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
}
const refused = fs.readFileSync(logPath, 'utf8').split('\n').filter((l) => l.includes('ask before tuning is on')).length;
console.log(`   .. the server logged ${refused} refusal(s)`);
console.log(`   ${pass} passed, ${fail} failed   (server log: ${logPath})`);
process.exit(fail ? 1 : 0);
