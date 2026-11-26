// test-server-leave-count.mjs — beta96 THE COUNT AND THE LOCK AGREE WHEN A LISTENER LEAVES (11-26: another agent's
// headless run, a real vibeserver + fake rtl_tcp, a shared dial, 4 places, ask before tuning on: after the second listener
// closed its sockets, `listeners` (vibeserver.json / hwinfo / the dial) stayed at 2/4 and the bar read "ASK TO TUNE · 2/4",
// although the ask LOCK had lifted — the one listener present was told two things).
//
// Against the REAL server (shared dial, --users 4, --ask-before-tuning, --idle-grace 12 so a hold is 12 s, not 180),
// listener A stays; listener B arrives the way the web client does (spectrum + audio + /ws/dxcluster, the same session,
// "presence: foreground" after its first hwinfo) and leaves in each way a client can:
//   1. a tab closed: "bye" on the spectrum socket, then every socket closed with 1001 — released AT ONCE;
//   2. the same with the "bye" lost (pagehide never ran): 1001 on every socket — released at once;
//   3. audio closed first, then spectrum, both 1001 — released at once (only the LAST socket's close decides);
//   4. a dropped connection (no close frame — a suspended phone): HELD. The held place stays counted (Stuart's rule: "their
//      listener slot stays counted"), but A is alone in the room: the lock lifts, and the dial and `users` must SAY the place
//      is a held one (`away`), so the banner reads FREE TO TUNE, never ASK TO TUNE · 2/4; when the hold ends, everything says 1.
// Checked on every case: the dial's `listeners` (and `away`), A's askFirst, the `users` message's n, /vibeserver.json `listeners`.
// ★ SILENT: audio sockets are opened only so the session has them; frames are counted, never played.
//   usage: VIBESERVER_BIN=…/vibeserver node --max-old-space-size=512 scripts/test-server-leave-count.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   not run — VIBESERVER_BIN needed'); process.exit(3); }

let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const waitFor = async (fn, ms) => { const t = Date.now(); while (Date.now() - t < ms) { if (await fn()) return true; await sleep(150); } return !!(await fn()); };
const children = new Set();
const killAll = () => { for (const c of children) { try { c.kill('SIGTERM'); } catch {} } children.clear(); };
process.on('exit', killAll);
const deadline = setTimeout(() => { console.log('   FAIL deadline — the run took over 3 minutes'); killAll(); process.exit(1); }, 3 * 60_000);
deadline.unref();

/** A minimal WebSocket client: text frames kept, binary counted; drop() = vanish, close(code) = a clean close frame. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0, open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  st.opened = new Promise((r) => { st._r = r; });
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; st._r(); buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x2) st.bin++;
      if (op === 0x1) { st.txt.push(p.toString()); if (st.txt.length > 4000) st.txt.splice(0, 2000); }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.drop = () => { s.destroy(); };
  st.close = (code) => {
    try { s.write(Buffer.from([0x88, 0x82, 0, 0, 0, 0, (code >> 8) & 0xff, code & 0xff])); } catch {}
    setTimeout(() => s.end(), 50);
  };
  st.last = (type) => { for (let i = st.txt.length - 1; i >= 0; i--) if (st.txt[i].includes(`"type":"${type}"`)) { try { return JSON.parse(st.txt[i]); } catch { return null; } } return null; };
  return st;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-leavecount-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Leave count test', adminPass: 'leave-count-admin',
  radios: [{ serial: 'leave-count', label: 'Test radio', enabled: true, configured: true, users: 4, askBeforeTuning: true }] }));
const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
children.add(rtl);
await sleep(400);
const HOLD_S = 12;
const env = { ...process.env, VIBE_ASK_GONE_GRACE_S: '6', VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logFd = fs.openSync(path.join(dir, 'server.log'), 'w');
const srv = spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm',
                           '--users', '4', '--ask-before-tuning', '--idle-grace', String(HOLD_S)], { env, stdio: ['ignore', logFd, logFd] });
children.add(srv);
const slog = () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } };

const jsonListeners = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); const j = await r.json(); return Number(j.listeners); } catch { return NaN; } };
/** A web-client-shaped listener: spectrum + audio + decoders, one session; presence after the first hwinfo. */
async function listener(tag) {
  const sid = `lc${tag}${crypto.randomBytes(4).toString('hex')}`;
  const L = { sid };
  L.spec = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=128&proto=1`);
  await L.spec.opened;
  L.audio = ws(port, `/ws/audio?user_session_id=${sid}&proto=1&codec=opus`);
  L.dx = ws(port, `/ws/dxcluster?user_session_id=${sid}`);
  await Promise.all([L.audio.opened, L.dx.opened]);
  await waitFor(() => !!L.spec.last('hwinfo'), 4000);
  L.spec.send({ type: 'presence', state: 'foreground' });
  L.beat = setInterval(() => { try { L.spec.send({ type: 'ping' }); L.audio.send({ type: 'ping' }); } catch {} }, 5000);
  return L;
}
const stopBeat = (L) => clearInterval(L.beat);
/** What A is told, everywhere it is told it. */
const view = async (A) => {
  const d = A.spec.last('dial') || {};
  return { dial: d.listeners, held: d.away || 0, uaway: A.spec.last('users')?.away || 0, ask: d.askFirst === true, hw: A.spec.last('users')?.n, json: await jsonListeners() };
};
const fmt = (v) => `dial ${v.dial}${v.held ? ` (away ${v.held})` : ''} · askFirst ${v.ask} · users ${v.hw}${v.uaway ? ` (away ${v.uaway})` : ''} · vibeserver.json ${v.json}`;
/** A is alone: every count says 1 and the lock is off. */
const alone = (v) => v.dial === 1 && !v.ask && v.hw === 1 && v.json === 1 && !v.held && !v.uaway;

try {
  for (let i = 0; i < 150; i++) { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`); if (r.ok) break; } catch {} await sleep(100); }
  const A = await listener('a');
  await sleep(600);
  ok(await waitFor(async () => alone(await view(A)), 4000), `A alone: ${fmt(await view(A))}`);

  const cases = [
    ['a tab closed: bye, then 1001 on every socket', (B) => { B.spec.send({ type: 'bye' }); setTimeout(() => { B.spec.close(1001); B.audio.close(1001); B.dx.close(1001); }, 50); }],
    ['bye lost: 1001 on every socket', (B) => { B.spec.close(1001); B.audio.close(1001); B.dx.close(1001); }],
    ['audio first, then spectrum, 1001', async (B) => { B.audio.close(1001); B.dx.close(1001); await sleep(400); B.spec.close(1001); }],
  ];
  for (const [what, leave] of cases) {
    console.log(`── ${what} ──`);
    const B = await listener('b');
    ok(await waitFor(async () => { const v = await view(A); return v.dial === 2 && v.ask && v.hw === 2 && v.json === 2; }, 5000),
       `B with A: two counted everywhere and the lock on (${fmt(await view(A))})`);
    stopBeat(B);
    await leave(B);
    ok(await waitFor(async () => alone(await view(A)), 3500), `B gone: A told 1 everywhere within 3.5 s, lock off (${fmt(await view(A))})`);
    await sleep(500);
  }

  console.log('── a dropped connection: held — counted as a held place, and the dial says so ──');
  const B = await listener('b');
  ok(await waitFor(async () => { const v = await view(A); return v.dial === 2 && v.ask; }, 5000), `B with A (${fmt(await view(A))})`);
  stopBeat(B);
  B.spec.drop(); B.audio.drop(); B.dx.drop();
  ok(await waitFor(() => /sockets have all gone — their session is HELD/.test(slog()), 4000), 'B is held (the server log says so)');
  // past the ask grace (6 s here, + the 2 s housekeeping tick): the held listener is not company
  await waitFor(async () => !(await view(A)).ask, 10000);
  let v = await view(A);
  ok(!v.ask, `held: the lock is off — A is the only one present (${fmt(v)})`);
  ok(v.dial === 2 && v.hw === 2 && v.json === 2, `held: the place is still counted everywhere (Stuart's rule) (${fmt(v)})`);
  ok(v.held === 1 && v.uaway === 1, `★ held: the dial and the users message say one of the two is away (a held place), so the banner can say FREE TO TUNE (dial away ${v.held}, users away ${v.uaway})`);
  ok(v.dial - v.held <= 1 && !v.ask, '★ the count of people present and the lock agree (present 1, no lock)');
  ok(await waitFor(async () => alone(await view(A)), (HOLD_S + 6) * 1000), `the hold ends: A told 1 everywhere (${fmt(await view(A))})`);
  v = await view(A);
  ok(!v.held, `…and held is gone from the dial (${v.held})`);
  // ★★ THE INVARIANT, over every dial A was sent all run: the lock is on exactly when more than one person is present
  //    (listeners − away > 1) — never "ASK TO TUNE · 2/4" unlocked, never locked while the count says alone.
  const dials = A.spec.txt.filter((t) => t.includes('"type":"dial"')).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
  const bad = dials.filter((d) => (d.askFirst === true) !== ((d.listeners - (d.away || 0)) > 1));
  ok(dials.length >= 8 && bad.length === 0, `every dial A was sent (${dials.length}) has the lock and the count agree${bad.length ? ` — ${bad.length} do not, e.g. ${JSON.stringify(bad[0])}` : ''}`);
  stopBeat(A);
} catch (e) { ok(false, `driver: ${e && e.stack || e}`); }
finally {
  clearTimeout(deadline);
  killAll();
  await sleep(400);
  if (!fail) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
}
console.log(`   ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
