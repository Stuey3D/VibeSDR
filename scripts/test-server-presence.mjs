// test-server-presence.mjs — V12 PRESENCE (Stuart, 2026-11-24: "i also closed down the app a minute ago but its still
// holding a slot on the xcover … we need the app to identify to the server when it has been backgrounded then it holds
// the slot rather than wait 3 mins after it has closed").
//
// The server HOLDS a session whose sockets have all gone (THE HELD SESSION) because it cannot tell a closed app from a
// suspended one or a network blip. Now the app says which: {"type":"presence","state":"background"|"foreground"} and
// {"type":"bye"} (= the older "leave"). On a REAL vibeserver (fake rtl_tcp, a shared dial, --users 3, idle grace 12 s so
// every hold is 12 s rather than 180 — sessionHoldS is min(180, grace)), observer B reading the dial's listener count:
//   1. an OLDER client (says nothing) drops → held, as today; the count stays until the hold runs out;
//   2. "bye" → released AT ONCE: no hold, the count drops within a second or two;
//   3. "background" then the sockets drop → held ("its app said it went to the background"); back, "foreground", then a
//      clean 1001 close → released at once;
//   4. "foreground" then an ABNORMAL drop (no close frame — a Wi-Fi blip) → held;
//   5. pauses are not goodbyes: spectrum + audio both closed with 1000 → held; spectrum 1001 while audio is still open,
//      then the audio drops → held (only the LAST socket's close decides); "background" + 1001 → held;
//   6. a FULL radio (--users 2): A says bye, C arrives and gets in with no held place to release.
// ★ SILENT: audio sockets are opened only so a session has two sockets; frames are counted, never played.
//   usage: VIBESERVER_BIN=…/vibeserver node scripts/test-server-presence.mjs
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
const waitFor = async (fn, ms) => { const t = Date.now(); while (Date.now() - t < ms) { if (fn()) return true; await sleep(150); } return fn(); };
const children = new Set();
const killAll = () => { for (const c of children) { try { c.kill('SIGTERM'); } catch {} } children.clear(); };
process.on('exit', killAll);
const deadline = setTimeout(() => { console.log('   FAIL deadline — the run took over 4 minutes'); killAll(); process.exit(1); }, 4 * 60_000);
deadline.unref();

/** A minimal WebSocket client: text frames kept, binary counted; drop() = vanish, close(code) = a clean close frame. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0, open: false, closed: false };
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
      if (op === 0x2) st.bin++;
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
  st.drop = () => { s.destroy(); };   // ★ a suspended phone / a blip: no close frame
  st.close = (code) => {              // ★ a clean close frame carrying `code` (masked with a zero key)
    try { s.write(Buffer.from([0x88, 0x82, 0, 0, 0, 0, (code >> 8) & 0xff, code & 0xff])); } catch {}
    setTimeout(() => s.end(), 50);
  };
  return st;
}
const parsed = (st, type) => st.txt.filter((x) => x.includes(`"type":"${type}"`)).map((x) => { try { return JSON.parse(x); } catch { return {}; } });

async function server(tag, users) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-presence-${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Presence test', adminPass: 'presence-test-admin',
    radios: [{ serial: 'presence-test', label: 'Test radio', enabled: true, configured: true, users }] }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, ['--max-old-space-size=256', path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000'], { stdio: 'ignore' });
  children.add(rtl);
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const log = fs.openSync(path.join(dir, 'server.log'), 'w');
  const srv = spawn('nice', ['-n', '10', BIN, '--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '1000000', '--mode', 'am',
                             '--users', String(users), '--idle-grace', '12'], { env, stdio: ['ignore', log, log] });
  children.add(srv);
  for (let i = 0; i < 150; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  return { port, dir, log: () => { try { return fs.readFileSync(path.join(dir, 'server.log'), 'utf8'); } catch { return ''; } },
           stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); children.delete(srv); children.delete(rtl); } };
}
const spectrum = (port, sid) => ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=128&proto=1`);
const audio = (port, sid) => ws(port, `/ws/audio?user_session_id=${sid}&codec=opus`);
const count = (S, re) => (S.log().match(re) || []).length;
const HELD = /sockets have all gone — their session is HELD/g;

try {
  const S = await server('main', 3);
  try {
    // ★ observer B: the dial's listener count is what the banners ("2/3") read
    const B = spectrum(S.port, crypto.randomUUID());
    await waitFor(() => parsed(B, 'dial').length > 0, 5000);
    const lastN = () => { const d = parsed(B, 'dial').filter((m) => typeof m.listeners === 'number'); return d.length ? d.at(-1).listeners : -1; };
    /** A listener with both sockets, ready (hwinfo on the spectrum, audio flowing). */
    const arrive = async () => {
      const sid = crypto.randomUUID();
      const L = { sid, spec: spectrum(S.port, sid) };
      await waitFor(() => parsed(L.spec, 'hwinfo').length > 0, 5000);
      L.aud = audio(S.port, sid);
      await waitFor(() => L.aud.bin > 0, 5000);
      await waitFor(() => lastN() === 2, 4000);
      return L;
    };
    const settledTo1 = (ms) => waitFor(() => lastN() === 1, ms);

    console.log('\n1. An OLDER client (says nothing) — held, as today');
    {
      const L = await arrive();
      ok(lastN() === 2, `two listeners (${lastN()})`);
      const h0 = count(S, HELD);
      L.spec.drop(); L.aud.drop();
      await waitFor(() => count(S, HELD) > h0, 5000);
      ok(count(S, HELD) === h0 + 1 && /HELD for 12 s[^\n]*no word from the app/.test(S.log()), '★ held 12 s — "no word from the app"');
      await sleep(3000);
      ok(lastN() === 2, `…its place still counted 3 s on (${lastN()})`);
      const t0 = Date.now();
      await settledTo1(16_000);
      ok(lastN() === 1 && /hold ran out/.test(S.log()), `the count drops when the hold runs out (${((Date.now() - t0) / 1000 + 3).toFixed(0)} s after the drop)`);
    }

    console.log('\n2. "bye" — released at once');
    {
      const L = await arrive();
      L.spec.send({ type: 'presence', state: 'foreground' });
      await waitFor(() => /app is now in the foreground/.test(S.log()), 3000);
      ok(/app is now in the foreground/.test(S.log()), 'the presence notice is logged in words');
      const h0 = count(S, HELD);
      L.spec.send({ type: 'bye' });
      await sleep(200);
      const t0 = Date.now();
      L.spec.drop(); L.aud.drop();   // ★ even an ungraceful close after the bye is a goodbye
      await settledTo1(4000);
      const el = (Date.now() - t0) / 1000;
      ok(lastN() === 1 && el < 3, `★ the count drops to 1 at once (${el.toFixed(1)} s), not after the hold`);
      await sleep(500);
      ok(count(S, HELD) === h0, '★ never held');
      ok(/the app said goodbye — it has closed/.test(S.log()), '…"the app said goodbye" logged');
    }

    console.log('\n3. "background", then the phone suspends it — held; back, then a clean close — released');
    {
      const L = await arrive();
      L.spec.send({ type: 'presence', state: 'background' });
      await waitFor(() => /app is now in the background/.test(S.log()), 3000);
      L.spec.close(1000);   // ★ the app pauses its spectrum in the background (1000) — the audio carries on
      await sleep(800);
      ok(lastN() === 2 && L.aud.bin > 0 && !L.aud.closed, `the spectrum paused, the audio still open: still 2 (${lastN()})`);
      const h0 = count(S, HELD);
      L.aud.drop();          // ★ …then iOS suspends it
      await waitFor(() => count(S, HELD) > h0, 5000);
      ok(/HELD for 12 s[^\n]*its app said it went to the background/.test(S.log()), '★ held — "its app said it went to the background"');
      await sleep(1500);
      ok(lastN() === 2, `its place still counted (${lastN()})`);
      // ★ back: the same session, foreground again
      const R = spectrum(S.port, L.sid);
      await waitFor(() => parsed(R, 'hwinfo').length > 0, 5000);
      R.send({ type: 'presence', state: 'foreground' });
      await waitFor(() => /held session \S+ returned/.test(S.log()), 3000);
      ok(/held session \S+ returned/.test(S.log()), 'the same session came back — the hold carried on');
      await sleep(400);
      const h1 = count(S, HELD);
      const t0 = Date.now();
      R.close(1001);         // ★ the app closed without managing a bye: a clean "going away"
      await settledTo1(4000);
      ok(lastN() === 1 && (Date.now() - t0) < 3000, `★ a clean 1001 close in the foreground: released at once (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
      await sleep(500);
      ok(count(S, HELD) === h1 && /closed its connection cleanly in the foreground/.test(S.log()), '…never held, and logged in words');
    }

    console.log('\n4. "foreground", then an ABNORMAL drop (a blip) — held');
    {
      const L = await arrive();
      L.spec.send({ type: 'presence', state: 'foreground' });
      await sleep(300);
      const h0 = count(S, HELD);
      L.spec.drop(); L.aud.drop();
      await waitFor(() => count(S, HELD) > h0, 5000);
      ok(count(S, HELD) === h0 + 1 && /HELD for 12 s[^\n]*dropped with no goodbye/.test(S.log()), '★ held — "the connection dropped with no goodbye"');
      await sleep(1500);
      ok(lastN() === 2, `its place still counted (${lastN()})`);
      await settledTo1(16_000);
    }

    console.log('\n5. Pauses are not goodbyes');
    {
      const L = await arrive();
      L.spec.send({ type: 'presence', state: 'foreground' });
      await sleep(300);
      let h0 = count(S, HELD);
      L.spec.close(1000); await sleep(300); L.aud.close(1000);
      await waitFor(() => count(S, HELD) > h0, 5000);
      ok(count(S, HELD) === h0 + 1, '★ both sockets closed with 1000 (the app\'s own pauses) — held');
      await settledTo1(16_000);

      const M = await arrive();
      M.spec.send({ type: 'presence', state: 'foreground' });
      await sleep(300);
      h0 = count(S, HELD);
      M.spec.close(1001);
      await sleep(800);
      ok(lastN() === 2 && count(S, HELD) === h0, `spectrum 1001 while the audio is still open — nothing ends (${lastN()})`);
      M.aud.drop();
      await waitFor(() => count(S, HELD) > h0, 5000);
      ok(count(S, HELD) === h0 + 1, '★ …then the audio drops: held (only the LAST socket\'s close decides)');
      await settledTo1(16_000);

      const N = await arrive();
      N.spec.send({ type: 'presence', state: 'background' });
      await sleep(300);
      h0 = count(S, HELD);
      N.aud.close(1001); await sleep(200); N.spec.close(1001);
      await waitFor(() => count(S, HELD) > h0, 5000);
      ok(count(S, HELD) === h0 + 1, '★ "background" then 1001 closes — held (the background notice wins)');
      await settledTo1(16_000);
    }
    B.drop();
  } finally { S.stop(); }

  console.log('\n6. A full radio (--users 2): "bye" leaves no held place to give way');
  {
    const S2 = await server('full', 2);
    try {
      const a = crypto.randomUUID();
      const A = spectrum(S2.port, a);
      await waitFor(() => parsed(A, 'hwinfo').length > 0, 5000);
      const B = spectrum(S2.port, crypto.randomUUID());
      await waitFor(() => parsed(B, 'hwinfo').length > 0, 5000);
      A.send({ type: 'presence', state: 'foreground' });
      A.send({ type: 'bye' });
      await sleep(200);
      A.drop();
      await sleep(800);
      const C = spectrum(S2.port, crypto.randomUUID());
      await waitFor(() => parsed(C, 'hwinfo').length > 0 || C.closed, 5000);
      ok(parsed(C, 'hwinfo').length > 0 && !C.closed, '★ C gets in at once');
      ok(!/the held place is theirs/.test(S2.log()) && !/sockets have all gone — their session is HELD/.test(S2.log()),
         '…with no held place to release (A was never held)');
      B.drop(); C.drop();
    } finally { S2.stop(); }
  }
} finally {
  killAll();
}

console.log(`\n${fail ? '✗' : '✓'} test-server-presence: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
