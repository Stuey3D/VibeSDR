// ★★★ DOES AN IDLE ADMIN SESSION ACTUALLY RE-LOCK — AND KEEP STREAMING?
//
// Two halves, and the second is the one that is easy to get wrong:
//   1. after the idle period the CONTROLS lock and the client is told why;
//   2. the SESSION, its audio and any decoder carry on regardless.
//
// (2) is the whole design. An admin leaves a session running ON PURPOSE — a decode, a recording,
// a long listen — so a re-lock that dropped the connection would punish the intended use in
// order to defend against the careless one. A test that only checked (1) would pass just as
// happily on an implementation that hung up.
//
// ★ Run the server with a short window, e.g.
//     vibeserver --tcp 127.0.0.1:9999 --admin-pass secret --port 48111 --admin-idle 1
//   ...and note the server checks every 2 s, so allow slack.
//
//   usage:  node admin-idle-relock.mjs <port> [admin-password] [idle-minutes]
import net from 'node:net';
import crypto from 'node:crypto';

const PORT = Number(process.argv[2]), PASS = process.argv[3] ?? 'secret';
const IDLE_MIN = Number(process.argv[4] ?? 1);
const HOST = '127.0.0.1';
let pass = 0, fail = 0;
const ok = (c, what, extra = '') => { c ? (pass++, console.log(`   ok   ${what}`))
                                        : (fail++, console.log(`   FAIL ${what} ${extra}`)); };

const hmac = (s, n) => crypto.createHmac('sha256', s).update(n).digest('hex');
const nonce = async () => (await (await fetch(`http://${HOST}:${PORT}/vibeserver/auth`)).json()).nonce;

/** A spectrum client that records the admin verdicts it is told and counts the frames it gets. */
function connect(session, adminQ) {
  const st = { frames: 0, msgs: [], closed: false, sock: null };
  const key = crypto.randomBytes(16).toString('base64');
  const sock = net.connect(PORT, HOST);
  st.sock = sock;
  let hs = false, buf = Buffer.alloc(0);
  sock.on('connect', () => sock.write(
    `GET /ws/user-spectrum?user_session_id=${session}${adminQ ? '&' + adminQ : ''} HTTP/1.1\r\n` +
    `Host: ${HOST}:${PORT}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
    `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  sock.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const i = buf.indexOf('\r\n\r\n'); if (i < 0) return; hs = true; buf = buf.subarray(i + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f;
      let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const payload = buf.subarray(off, off + len);
      buf = buf.subarray(off + len);
      if (op === 0x9) {                                  // ping -> pong, or we get dropped
        const p = Buffer.alloc(6 + payload.length);
        p[0] = 0x8a; p[1] = 0x80 | payload.length; p.writeUInt32BE(0, 2); payload.copy(p, 6);
        sock.write(p); continue;
      }
      if (op === 0x2) st.frames++;
      if (op === 0x1) { try { st.msgs.push(JSON.parse(payload.toString())); } catch {} }
    }
  });
  sock.on('close', () => { st.closed = true; });
  sock.on('error', () => { st.closed = true; });
  return st;
}

console.log(`\nProbing ${HOST}:${PORT} — admin idle window is ${IDLE_MIN} min`);

const n = await nonce();
const c = connect('idleprobe', `vs_admin_nonce=${n}&vs_admin_auth=${hmac(PASS, n)}`);
await new Promise((r) => setTimeout(r, 2500));

console.log('\n1. THE ADMIN ARRIVES UNLOCKED');
{
  const hw = c.msgs.find((m) => m.type === 'hwinfo');
  ok(!!hw, 'the server sent hwinfo');
  // ★ This is the fix for "connecting as admin from the splash granted nothing" — the credential
  //   is now verified even on an idle receiver, and adminOk rides hwinfo so the UI can show it.
  ok(hw?.adminOk === true, 'connecting WITH admin credentials lands already unlocked',
     `adminOk=${hw?.adminOk}`);
  ok(c.frames > 0, 'and is being served spectrum', `(${c.frames} frames)`);
}

console.log(`\n2. WAIT OUT THE IDLE WINDOW (${IDLE_MIN} min + slack), touching nothing`);
{
  const framesBefore = c.frames;
  const waitMs = IDLE_MIN * 60000 + 6000;
  const t0 = Date.now();
  while (Date.now() - t0 < waitMs && !c.msgs.some((m) => m.type === 'admin' && m.relocked)) {
    await new Promise((r) => setTimeout(r, 500));
  }
  const relock = c.msgs.find((m) => m.type === 'admin' && m.relocked);
  ok(!!relock, 'the controls were re-locked', `after ${((Date.now() - t0) / 1000).toFixed(0)}s`);
  ok(relock?.ok === false, 'and the client is told they are locked');
  ok(relock?.idleMin === IDLE_MIN, 'and told how long it waited', `idleMin=${relock?.idleMin}`);

  // ★★★ THE HALF THAT MATTERS MOST.
  ok(!c.closed, 'the SESSION IS STILL OPEN — a re-lock must not disconnect anybody');
  ok(c.frames > framesBefore, 'and the spectrum is STILL STREAMING',
     `(${framesBefore} -> ${c.frames} frames)`);
}

console.log('\n3. THE MENU PASSWORD BOX RESTORES IT');
{
  const n2 = await nonce();
  const msg = JSON.stringify({ type: 'admin_unlock', nonce: n2, token: hmac(PASS, n2) });
  // ★★ EXTENDED LENGTH. An admin_unlock carries a 32-hex nonce AND a 64-hex token, so the JSON
  //    is ~130 bytes — over the 125-byte limit of the 7-bit length field. The first cut of this
  //    probe wrote the length into those 7 bits anyway, producing a frame the server could not
  //    parse, and the probe then reported that unlocking was BROKEN. The server was fine.
  //    ★ A probe is code too: this is the third time in this project a bad probe has accused
  //      working code (see the memory note on wrong frame headers and text-only reads).
  const b = Buffer.from(msg);
  const ext = b.length >= 126;
  const f = Buffer.alloc((ext ? 8 : 6) + b.length);
  f[0] = 0x81;
  if (ext) { f[1] = 0x80 | 126; f.writeUInt16BE(b.length, 2); f.writeUInt32BE(0, 4); b.copy(f, 8); }
  else     { f[1] = 0x80 | b.length; f.writeUInt32BE(0, 2); b.copy(f, 6); }
  c.sock.write(f);
  const t0 = Date.now();
  while (Date.now() - t0 < 4000
         && !c.msgs.some((m) => m.type === 'admin' && m.ok === true && !m.relocked)) {
    await new Promise((r) => setTimeout(r, 200));
  }
  ok(c.msgs.some((m) => m.type === 'admin' && m.ok === true && !m.relocked),
     'entering the password again unlocks the controls');
  ok(!c.closed, 'still on the same session throughout');
}

console.log('\n4. AN ADMIN WHO IS ACTUALLY USING THE RECEIVER IS NOT RE-LOCKED');
{
  // ★★★ THE COUNTERPART, AND THE ONE THAT DECIDES WHETHER THIS FEATURE IS USABLE OR HATED.
  //     A re-lock that fires while somebody is plainly working — tuning, changing mode, zooming
  //     — is not a safety feature, it is a fault. The idle clock is stamped on every CONTROL
  //     message for exactly this reason, and nothing else in this file proves that it is.
  const before = c.msgs.filter((m) => m.type === 'admin' && m.relocked).length;
  const send = (obj) => {
    const b = Buffer.from(JSON.stringify(obj));
    const ext = b.length >= 126;
    const f = Buffer.alloc((ext ? 8 : 6) + b.length);
    f[0] = 0x81;
    if (ext) { f[1] = 0x80 | 126; f.writeUInt16BE(b.length, 2); f.writeUInt32BE(0, 4); b.copy(f, 8); }
    else     { f[1] = 0x80 | b.length; f.writeUInt32BE(0, 2); b.copy(f, 6); }
    c.sock.write(f);
  };
  // Tune gently, well inside the locked window, for longer than the whole idle period.
  const t0 = Date.now(), span = IDLE_MIN * 60000 + 8000;
  let hz = 6400000;
  while (Date.now() - t0 < span) {
    send({ type: 'tune', frequency: (hz += 1000) });
    await new Promise((r) => setTimeout(r, 4000));
  }
  const after = c.msgs.filter((m) => m.type === 'admin' && m.relocked).length;
  ok(after === before, 'no re-lock while the admin keeps using the receiver',
     `(${after - before} re-locks during ${(span / 1000).toFixed(0)}s of activity)`);
  ok(!c.closed, 'and still connected');
}

c.sock.destroy();
console.log(`\n${fail ? `${fail} FAILURES, ` : ''}${pass} passed`);
process.exit(fail ? 1 : 0);
