// test-server-chat-share.mjs — "share a station" through the REAL server, end to end: two listeners on a
// shared dial, one shares, the other receives the line THE SERVER named.
//
// ★★★ WHY END TO END. test-chat-share.cpp proves the rules in vibe_chat_share.h; scripts/test_chat_share.ts
//     proves the clients never put a label on the wire. This proves the shim wires them together: that the
//     name the room sees comes from THIS receiver's bookmark store, that a label smuggled into the frame is
//     not relayed, that a share outside the radio's range or in a closed mode never reaches the room (and the
//     sender is told why), that flood control covers shares, and that a share moves nobody's dial.
//
// ★ Needs a BUILT vibeserver: VIBESERVER_BIN=/path/to/vibeserver. Without one it reports "not run" (exit 3).
// ★ SILENT: no audio is played; the audio socket is not even opened.
//
//   usage: VIBESERVER_BIN=… node scripts/test-server-chat-share.mjs
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
let pass = 0, fail = 0;
const ok = (c, what, detail = '') => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}${!c && detail ? `\n        ${detail}` : ''}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

/** A minimal WebSocket client recording every text frame (the probes' own — see test-server-decoders.mjs). */
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
  st.said = () => st.txt.filter((t) => t.includes('"type":"said"')).map((t) => JSON.parse(t));
  st.why = () => st.txt.filter((t) => t.includes('"type":"notice"') && t.includes('"why"')).map((t) => JSON.parse(t).why);
  return st;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-chatshare-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
// ★ What this receiver KNOWS — an RDS-learned FM station and a DAB service it has decoded — seeded in the
//   store's own file format, so the server loads it exactly as it would after a night of listening.
const now = Math.floor(Date.now() / 1000);
fs.writeFileSync(path.join(dir, 'data', 'bookmarks.json'), JSON.stringify([
  { frequency: 100000000, name: 'Heart', lastHeard: now, manual: false, mode: 'wfm', pi: 50001, nameSrc: 2 },
  { frequency: 225648000, name: 'Heart DAB', lastHeard: now, manual: false, mode: 'dab', sid: 49362, eid: 52245, nameSrc: 2 },
]));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--wfm', '100.0'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const log = fs.openSync(logPath, 'w');
// ★ --users 4 with no locked centre = ONE SHARED DIAL, which is where the chat exists.
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '100000000',
                        '--mode', 'wfm', '--users', '4', '--idle-grace', '0'], { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}

try {
  console.log('── shared dial, two listeners ──');
  const A = ws(port, '/ws/user-spectrum?user_session_id=aaaaaaaa&bins=256');
  await sleep(500);
  const B = ws(port, '/ws/user-spectrum?user_session_id=bbbbbbbb&bins=256');
  await sleep(1200);
  ok(A.open && B.open, 'both listeners connected');
  const dialMsgs = B.txt.filter((t) => t.includes('"type":"dial"'));
  ok(dialMsgs.length > 0 && !dialMsgs.at(-1).includes('"mode":"exclusive"'), 'the dial is shared (chat exists here)', dialMsgs.at(-1));

  // 1. A bookmark share with a label smuggled in: the room hears THE RECEIVER's name, not the label.
  const n0 = B.said().length;
  A.send({ type: 'say', id: 'check_out', kind: 'bookmark', hz: 100000000, mode: 'wfm', bwLo: -100000, bwHi: 100000,
           name: 'RUDE LABEL', label: 'RUDE LABEL', text: 'RUDE LABEL' });
  await sleep(600);
  const s1 = B.said().slice(n0);
  ok(s1.length === 1, 'B received exactly one line', JSON.stringify(s1));
  const l1 = s1[0] || {};
  ok(!JSON.stringify(B.txt).includes('RUDE'), 'the smuggled label never reached the room');
  ok(l1.id === 'check_out' && l1.kind === 'bookmark' && l1.hz === 100000000 && l1.mode === 'wfm', 'structured share relayed', JSON.stringify(l1));
  ok(l1.name === 'Heart' && l1.nameSrc === 'station', 'named from the RDS-learned station', JSON.stringify(l1));
  ok(l1.text === 'shared 100.000 MHz WFM — Heart', 'fallback text built by the server', l1.text);
  ok(l1.bwLo === -100000 && l1.bwHi === 100000, 'the passband travels');
  ok(A.said().slice(-1)[0]?.from === l1.from, 'the sender sees the same line (no local echo needed)');

  // 2. Flood control covers shares.
  const n1 = B.said().length;
  A.send({ type: 'say', id: 'check_out', kind: 'bookmark', hz: 100000000, mode: 'wfm' });
  await sleep(500);
  ok(B.said().length === n1, 'a second share inside 3 s is dropped (flood control)');
  await sleep(2800);

  // 3. Outside the radio's range: refused, and the sender is told why.
  const w0 = A.why().length, n2 = B.said().length;
  A.send({ type: 'say', id: 'check_out', kind: 'bookmark', hz: 2.4e9, mode: 'nfm' });
  await sleep(600);
  ok(B.said().length === n2, 'a share outside the tuning range never reaches the room');
  ok(A.why().slice(w0).some((w) => /outside/.test(w)), 'the sender is told why', JSON.stringify(A.why()));
  await sleep(3000);

  // 4. A mode outside the closed list (text through the mode field): refused.
  const w1 = A.why().length, n3 = B.said().length;
  A.send({ type: 'say', id: 'check_out', hz: 100000000, mode: 'you are all idiots' });
  await sleep(600);
  ok(B.said().length === n3 && !JSON.stringify(B.txt).includes('idiots'), 'text in the mode field is refused, not relayed');
  ok(A.why().length > w1, '  …and the sender told');

  // 5. An unknown frequency: just the frequency and mode.
  const n4 = A.said().length;
  B.send({ type: 'say', id: 'check_out', kind: 'bookmark', hz: 105000000, mode: 'wfm' });
  await sleep(600);
  const l5 = A.said().slice(n4)[0] || {};
  ok(l5.hz === 105000000 && l5.name === undefined && l5.text === 'shared 105.000 MHz WFM', 'nothing known = the bare frequency', JSON.stringify(l5));
  await sleep(3000);

  // 6. DAB: a service by block + sid, named from the services this receiver has decoded — or, where this
  //    build/radio cannot play DAB, refused with a reason. Either is correct; which one is reported.
  const n5 = B.said().length, w2 = A.why().length;
  A.send({ type: 'say', id: 'check_out', kind: 'dab', hz: 225648000, mode: 'dab', block: '12B', sid: 'C0D2', eid: 52245 });
  await sleep(700);
  const l6 = B.said().slice(n5)[0];
  if (l6) {
    ok(l6.kind === 'dab' && l6.block === '12B' && l6.sid === 49362 && l6.mode === 'dab' && l6.hz === 225648000, 'DAB share relayed with block + sid', JSON.stringify(l6));
    ok(l6.name === 'Heart DAB', 'DAB named from the decoded service list', JSON.stringify(l6));
    ok(/^shared DAB Heart DAB — 12B \(225\.648 MHz\)$/.test(l6.text), 'DAB fallback text', l6.text);
    console.log('   (DAB available on this build: share accepted)');
  } else {
    ok(A.why().slice(w2).some((w) => /DAB/.test(w)), 'DAB refused with a reason where DAB is not available', JSON.stringify(A.why()));
    console.log('   (DAB not available on this build/radio: share refused, as designed)');
  }

  // 7. A share moves nobody: the server log shows no retune since start-up.
  const slog = fs.readFileSync(logPath, 'utf8');
  ok(!/retune -> /.test(slog), 'receiving (and sending) a share retuned nothing');
  ok(/chat share from User \d+ refused/.test(slog), 'refusals are logged with who asked');

  A.close(); B.close();
} finally {
  stop();
  await sleep(200);
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
