// test-chat-phrases.mjs — ONE CANNED VOCABULARY, EVERYWHERE, AND THE SERVER CARRIES EVERY WORD OF IT (2026-10-08).
//
// ★★★ THE IDS ARE THE CONTRACT. The shared-dial chat sends phrase IDS, never text: the server's allow-list
//     (chatPhrases() in local_sdr_shim.cpp) drops any id it does not know, and each client drops any id it cannot
//     draw. So a phrase added to one list and not another vanishes in silence — a button that sends nothing, or a
//     message nobody sees. Stuart, 2026-10-08, adding nine social phrases ("make it a little more social without
//     opening it up to full text chat"): five lists to keep in step and no test that they were.
// ★ Two halves: (1) the app, web, both watch apps and the server list the SAME ids; (2) a live shared-dial server
//   carries every id from one listener to the other, and drops one it does not know.
// ★ SILENT: nothing plays audio.   usage: VIBESERVER_BIN=… node scripts/test-chat-phrases.mjs   (~45 s)
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

// ── (1) the five lists ──
const read = (f) => fs.readFileSync(path.join(root, f), 'utf8');
const idsTs = (f) => [...read(f).matchAll(/\{ id: '([a-z0-9_]+)',\s*text:/g)].map((m) => m[1]);
const idsSwift = (f) => { const t = read(f); const s = t.indexOf('static let all', t.indexOf('enum CannedDial')); const e = t.indexOf('\n  ]', s);   // ★ not the first ']' — the type [(id: String, text: String)] has one
  return [...t.slice(s, e).matchAll(/\("([a-z0-9_]+)",/g)].map((m) => m[1]); };
const idsServer = () => { const t = read('android/app/src/main/cpp/local_sdr_shim.cpp');
  const s = t.indexOf('chatPhrases() {'); const e = t.indexOf('};', s);
  return [...t.slice(s, e).matchAll(/^\s*"([a-z0-9_]+)",/gm)].map((m) => m[1]).filter((i) => i !== 'check_out'); };
const lists = {
  server: idsServer(),
  app: idsTs('src/services/dialChat.ts'),
  web: idsTs('web/client/src/chat.ts'),
  'watch (Buddy)': idsSwift('ios/VibeSDRWatch/Chat.swift'),
  'watch (Jr)': idsSwift('spike/WristSDR/WristSDR/Chat.swift'),
};
console.log('── one vocabulary in five places ──');
const ref = lists.server.join(',');
ok(lists.server.length >= 24, `the server allows ${lists.server.length} phrases (the 15 + the 9 social ones of 2026-10-08)`);
for (const [k, v] of Object.entries(lists)) if (k !== 'server')
  ok(v.join(',') === ref, `${k} lists exactly the server's ids, in order (${v.length})${v.join(',') === ref ? '' : ' — differs: ' + v.filter((x) => !lists.server.includes(x)).concat(lists.server.filter((x) => !v.includes(x))).join(' ')}`);
for (const id of ['hello', 'just_scanning', 'what_is_this', 'sounds_awesome', 'nice_catch', 'not_my_music', 'good_conditions', 'poor_conditions', 'off_73'])
  if (!lists.server.includes(id)) ok(false, `social phrase ${id} missing from the server`);

// ── (2) a live shared dial ──
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   (live half not run — set VIBESERVER_BIN to a vibeserver built from this tree)');
  console.log(`   ${pass} passed, ${fail} failed`); process.exit(fail ? 1 : 3);
}
/** A minimal WebSocket client (the probes' own), recording binary frames with their arrival time. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { bin: [], txt: [], open: false, closed: false };
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
      if (op === 0x2) st.bin.push({ t: Date.now(), b: Buffer.from(p) });
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));   // zero mask: payload unchanged
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-chat-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_HANDLE_HOLD_SEC: '6', VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const log = fs.openSync(logPath, 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000',
                        '--mode', 'wfm', '--users', '3'], { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
try {
  console.log('── a shared dial carries every phrase, and drops one it does not know ──');
  const a = ws(port, `/ws/user-spectrum?user_session_id=aaaaaaaa&bins=256`);
  const b = ws(port, `/ws/user-spectrum?user_session_id=bbbbbbbb&bins=256`);
  await sleep(800);
  const heard = () => b.txt.filter((t) => t.includes('"type":"said"')).map((t) => JSON.parse(t).id);
  let missing = [];
  for (const id of lists.server) {
    const before = heard().length;
    a.send({ type: 'say', id });
    for (let i = 0; i < 20 && heard().length === before; i++) await sleep(50);
    if (heard().length === before || heard()[heard().length - 1] !== id) missing.push(id);
    await sleep(3100);                                           // the server's flood control: one line per 3 s
  }
  ok(missing.length === 0, `★ every one of the ${lists.server.length} phrases reached the other listener${missing.length ? ' — not: ' + missing.join(' ') : ''}`);
  const before = heard().length;
  a.send({ type: 'say', id: 'free text <b>hi</b>' });
  await sleep(1200);
  ok(heard().length === before, 'an id the server does not know is dropped, never passed on');
  a.close(); b.close();

  // ★★ USER NUMBERS: the lowest not in use, held 2 minutes for a reconnect (Stuart: "user 10 on a 3 user server").
  console.log('── user numbers: the lowest free one, kept across a reconnect ──');
  await sleep(500);
  const you = (st) => { const d = st.txt.filter((x) => x.includes('"type":"dial"')).map((x) => JSON.parse(x).you); return d[d.length - 1]; };
  const join = async (sid) => { const s = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=256`); await sleep(700); return s; };
  const c1 = await join('cccc0001'), c2 = await join('cccc0002');
  const n1 = you(c1), n2 = you(c2);
  c2.close(); await sleep(600);
  const c3 = await join('cccc0003');
  const n3 = you(c3);
  const c2b = await join('cccc0002');
  const n2b = you(c2b);
  console.log(`   .. numbers: first ${n1}, second ${n2}, a newcomer while the second is away ${n3}, the second back ${n2b}`);
  ok(n3 !== n2 && n2b === n2, '★ a listener who drops and comes back within 2 minutes gets THEIR number back; a newcomer meanwhile does not take it');
  // ★ The two listeners of the half above left moments ago and are HELD (1, 2) — so the lowest free numbers now are
  //   3, 4, 5. Under the old running count this run's sessions would be numbered 3, 4, 5, 6 (the reconnect a NEW one).
  ok(n1 === 3 && n2 === 4 && n3 === 5, `★ the lowest numbers nobody holds (1 and 2 are held for the two who just left): ${n1}, ${n2}, ${n3}`);
  // ★★ and once a hold runs out the number is REUSED — the running count would hand out 6 here (hold shortened to 6 s
  //    for this test by VIBESERVER_HANDLE_HOLD_SEC; it is 2 minutes in service).
  c3.close(); await sleep(7500);
  const c4 = await join('cccc0004');
  const n4 = you(c4);
  console.log(`   .. after the hold ran out, a newcomer gets ${n4}`);
  ok(n4 <= 3, `★ a number whose holder has been gone longer than the hold is given out again (${n4}, not a running count)`);
  c1.close(); c2b.close(); c4.close();
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
}
console.log(`   ${pass} passed, ${fail} failed   (server log: ${logPath})`);
process.exit(fail ? 1 : 0);
