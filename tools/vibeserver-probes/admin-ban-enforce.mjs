// ★★★ DOES A BAN ACTUALLY REFUSE A LISTENER?
//
// The admin-api probe proves the ban list STORES what it is told. That is the easy half, and it
// is the half that can pass while the feature does nothing: the owner sees the ban in the list
// and watches the banned listener carry on listening.
//
// ★★ It has to run over the LAN ADDRESS, not 127.0.0.1 — loopback is deliberately exempt from
//    the ban list (the host must never be able to lock itself out), so a loopback probe would
//    report a pass no matter what the enforcement code did. That exemption is exactly the thing
//    that would hide this bug.
//
//   usage:  node admin-ban-enforce.mjs <port> <lan-ip> [admin-password]
import crypto from 'node:crypto';
import net from 'node:net';

const PORT = process.argv[2], IP = process.argv[3], PASS = process.argv[4] ?? 'secret';
if (!PORT || !IP) { console.error('usage: admin-ban-enforce.mjs <port> <lan-ip> [password]'); process.exit(2); }
const base = `http://${IP}:${PORT}`;

let pass = 0, fail = 0;
const ok = (c, what, extra = '') => { c ? (pass++, console.log(`   ok   ${what}`))
                                        : (fail++, console.log(`   FAIL ${what} ${extra}`)); };

const hmac = (s, n) => crypto.createHmac('sha256', s).update(n).digest('hex');
async function authQ() {
  const n = (await (await fetch(`${base}/vibeserver/auth`)).json()).nonce;
  return `vs_admin_nonce=${n}&vs_admin_auth=${hmac(PASS, n)}`;
}
const post = async (p, b) => fetch(`${base}/vibeserver/admin/${p}?${await authQ()}`,
                                   { method: 'POST', body: JSON.stringify(b) });
const get  = async (p)    => fetch(`${base}/vibeserver/admin/${p}?${await authQ()}`);

/** Open a spectrum socket and report what the server said.
 *  ★ Raw net + hand-rolled framing, like every other probe in this directory (see
 *    two-listeners.mjs). Server->client frames are never masked, so the parser is short. */
function tryConnect(session) {
  return new Promise((resolve) => {
    const key = crypto.randomBytes(16).toString('base64');
    const sock = net.connect(Number(PORT), IP);
    let handshook = false, buf = Buffer.alloc(0), settled = false;
    const done = (r) => { if (settled) return; settled = true; clearTimeout(timer);
                          try { sock.destroy(); } catch {} resolve(r); };
    // No verdict and no frames within the window means we are simply being served.
    const timer = setTimeout(() => done('streaming'), 2500);
    sock.on('connect', () => sock.write(
      `GET /ws/user-spectrum?user_session_id=${session} HTTP/1.1\r\nHost: ${IP}:${PORT}\r\n` +
      `Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\n` +
      `Sec-WebSocket-Version: 13\r\n\r\n`));
    sock.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      if (!handshook) {
        const i = buf.indexOf('\r\n\r\n');
        if (i < 0) return;
        handshook = true;
        buf = buf.subarray(i + 4);
      }
      for (;;) {
        if (buf.length < 2) break;
        const op = buf[0] & 0x0f;
        let len = buf[1] & 0x7f, off = 2;
        if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
        else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
        if (buf.length < off + len) break;
        const payload = buf.subarray(off, off + len);
        buf = buf.subarray(off + len);
        if (op === 0x2) { done('streaming'); return; }        // binary = we are being served
        if (op === 0x1) {
          let j; try { j = JSON.parse(payload.toString()); } catch { continue; }
          if (j.type === 'banned' || j.type === 'busy' || j.type === 'cooldown') { done(j.type); return; }
        }
      }
    });
    sock.on('error', () => done('error'));
    sock.on('close', () => done(handshook ? 'closed' : 'refused'));
  });
}

console.log(`\nProbing ${base} (a NON-loopback address, so the ban list applies)`);

await post('unban', { cidr: IP });

console.log('\n1. BEFORE the ban — a listener must get in');
{
  const r = await fetch(`${base}/connection?user_session_id=pre`);
  ok((await r.json()).allowed === true, 'preflight allows');
  ok(await tryConnect('pre') === 'streaming', 'the spectrum socket streams');
}

console.log('\n2. BAN THIS ADDRESS');
{
  const r = await post('ban', { cidr: IP, reason: 'probe: enforcement' });
  ok(r.status === 200, 'ban accepted');
}

console.log('\n3. AFTER the ban — the same listener must be refused');
{
  const r = await fetch(`${base}/connection?user_session_id=post`);
  const j = await r.json();
  ok(j.allowed === false && j.reason === 'banned', 'preflight refuses with reason "banned"', JSON.stringify(j));
  const verdict = await tryConnect('post');
  ok(verdict === 'banned', 'the spectrum socket is refused and TOLD why', `(got "${verdict}")`);
}

console.log('\n4. THE LOG RECORDED IT');
{
  const j = await (await get('connections')).json();
  ok(j.connections.some(c => c.reason === 'banned'), 'a "banned" entry is in the connection log',
     JSON.stringify(j.connections.slice(0, 2)));
}

console.log('\n5. LOOPBACK IS STILL EXEMPT — the host must never lock itself out');
{
  const r = await fetch(`http://127.0.0.1:${PORT}/connection?user_session_id=loop`);
  ok((await r.json()).allowed === true, 'loopback still allowed while its LAN address is banned');
  // ★ And the admin API itself must stay reachable, or an owner who mistypes a range into the
  //   ban box has no way back in except physically visiting the machine.
  const a = await get('status');
  ok(a.status === 200, 'the ADMIN API is still reachable from the banned address', `(got ${a.status})`);
}

console.log('\n6. UNBAN restores access');
{
  await post('unban', { cidr: IP });
  const r = await fetch(`${base}/connection?user_session_id=after`);
  ok((await r.json()).allowed === true, 'preflight allows again');
  ok(await tryConnect('after') === 'streaming', 'the spectrum socket streams again');
}

console.log(`\n${fail ? `${fail} FAILURES, ` : ''}${pass} passed`);
process.exit(fail ? 1 : 0);
