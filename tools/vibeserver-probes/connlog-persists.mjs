// ★★★ DOES THE CONNECTION LOG SURVIVE A RESTART?
//
// It did not. The log lived only in memory, and I had written down that surviving a restart was
// "explicitly NOT promised" — reasoning about SD-card wear. That was wrong twice over: VibeServer
// restarts on EVERY UPDATE, so the history vanished exactly when an owner would want it; and a
// closed connection is ~150 bytes, against a spectrogram that writes 3 MB on a timer.
//
// ★★ A log that forgets on restart is a live view, not a log. Its whole value is noticing a
//    pattern ACROSS time — "41 refusals from one range overnight" is the question it exists to
//    answer, and that question spans restarts by definition.
//
//   usage:  HOST=<pi> node connlog-persists.mjs <port> <admin-password> <restart-cmd>
//   e.g.    HOST=192.168.86.88 node connlog-persists.mjs 48000 pass "ssh pi sudo systemctl restart vibeserver"
import net from 'node:net';
import crypto from 'node:crypto';
import { execSync } from 'node:child_process';

const PORT = Number(process.argv[2]), PASS = process.argv[3], RESTART = process.argv[4];
const HOST = process.env.HOST ?? '127.0.0.1';
let pass = 0, fail = 0;
const ok = (c, what, extra = '') => { c ? (pass++, console.log(`   ok   ${what}`))
                                        : (fail++, console.log(`   FAIL ${what} ${extra}`)); };
const hmac = (s, n) => crypto.createHmac('sha256', s).update(n).digest('hex');
async function conns() {
  const n = (await (await fetch(`http://${HOST}:${PORT}/vibeserver/auth`)).json()).nonce;
  const q = `vs_admin_nonce=${n}&vs_admin_auth=${hmac(PASS, n)}`;
  const j = await (await fetch(`http://${HOST}:${PORT}/vibeserver/admin/connections?${q}`)).json();
  return j.connections ?? [];
}

/** Connect a spectrum socket, then drop it — one closed record in the log. */
function blip(session) {
  return new Promise((resolve) => {
    const key = crypto.randomBytes(16).toString('base64');
    const sock = net.connect(PORT, HOST);
    sock.on('connect', () => sock.write(
      `GET /ws/user-spectrum?user_session_id=${session} HTTP/1.1\r\nHost: ${HOST}\r\n` +
      `Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\n` +
      `Sec-WebSocket-Version: 13\r\n\r\n`));
    sock.on('data', () => {});
    setTimeout(() => { sock.destroy(); resolve(); }, 1800);
    sock.on('error', () => resolve());
  });
}

const MARK = 'persist-' + crypto.randomBytes(4).toString('hex');
console.log(`\nleaving a marked connection in the log (${MARK})`);
await blip(MARK);
await new Promise((r) => setTimeout(r, 2500));   // let the 1 Hz flush run

const before = await conns();
ok(before.some((c) => c.session === MARK), 'the connection is in the log before the restart',
   `(${before.length} entries)`);

console.log('\nrestarting the server');
execSync(RESTART, { stdio: 'ignore' });
await new Promise((r) => setTimeout(r, 12000));

const after = await conns();
ok(after.length > 0, 'the log is not empty after a restart', `(${after.length} entries)`);
// ★ THE ASSERTION THAT MATTERS. Anything else can pass on a log that merely refilled itself with
//   the probe's own reconnect.
ok(after.some((c) => c.session === MARK),
   '★ the SAME connection is still there — history survived the restart',
   `(looked for ${MARK} among ${after.length})`);

console.log(`\n${fail ? `${fail} FAILURES, ` : ''}${pass} passed`);
process.exit(fail ? 1 : 0);
