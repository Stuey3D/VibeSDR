// ★★★ CAN A LISTENER CHANGE THE RECEIVER'S BOOKMARKS?
//
// They could. `POST /bookmarks` was gated on vsAuthOk — the PIN — with a comment promising "when
// public servers arrive this becomes the admin credential instead". They arrived; it never moved.
//
// ★★ AND THE GAP IS NOT THEORETICAL. The PIN is ACCESS (may you listen) and the admin password is
//    CONTROL (may you change this receiver), and they are independent on purpose: the
//    configuration a PUBLIC receiver wants is NO PIN plus an admin password. In exactly that
//    configuration vsAuthOk returns true for EVERYONE — `if (secret.empty()) return true`. So on
//    every public server, any listener could add or delete the receiver's bookmarks.
//
// ★ MUST BE RUN FROM A NON-LOOPBACK ADDRESS. Loopback is deliberately exempt (the host is the
//   operator), so a local run passes no matter what the gate does — the same trap as the ban-list
//   probe.
//
//   usage:  HOST=<lan-ip-of-server> node bookmark-write-gate.mjs <port> <admin-password>
import crypto from 'node:crypto';

const PORT = Number(process.argv[2]), PASS = process.argv[3];
const HOST = process.env.HOST ?? '127.0.0.1';
if (HOST === '127.0.0.1' || HOST === 'localhost') {
  console.error('Run this against the LAN address — loopback is exempt and would pass regardless.');
  process.exit(2);
}
let pass = 0, fail = 0;
const ok = (c, what, extra = '') => { c ? (pass++, console.log(`   ok   ${what}`))
                                        : (fail++, console.log(`   FAIL ${what} ${extra}`)); };
const hmac = (s, n) => crypto.createHmac('sha256', s).update(n).digest('hex');
const base = `http://${HOST}:${PORT}`;
async function adminQ() {
  const n = (await (await fetch(`${base}/vibeserver/auth`)).json()).nonce;
  return `vs_admin_nonce=${n}&vs_admin_auth=${hmac(PASS, n)}`;
}
const list = async () => (await (await fetch(`${base}/bookmarks`)).json());

const HZ = 4321000, NAME = 'probe-' + crypto.randomBytes(3).toString('hex');

console.log(`\nProbing ${base} as an ordinary listener (no admin credential)`);
{
  const before = await list();
  const r = await fetch(`${base}/bookmarks?frequency=${HZ}&name=${NAME}&mode=am`, { method: 'POST' });
  ok(r.status === 401, 'a listener CANNOT add a bookmark to the receiver', `(got ${r.status})`);
  const after = await list();
  // ★ The status code is not enough on its own: assert the list did not actually change.
  ok(!after.some?.((b) => b?.name === NAME), 'and nothing was written', JSON.stringify(after).slice(0, 120));
  ok(Array.isArray(after) && after.length === (before?.length ?? 0), 'the list is the same length');
}

console.log('\nReading is still open to everyone (it is what the client shows)');
{
  const r = await fetch(`${base}/bookmarks`);
  ok(r.status === 200, 'GET /bookmarks -> 200', `(got ${r.status})`);
}

console.log('\nDeleting is a write too, and must be refused the same way');
{
  const r = await fetch(`${base}/bookmarks?frequency=${HZ}`, { method: 'DELETE' });
  ok(r.status === 401, 'a listener CANNOT delete a bookmark', `(got ${r.status})`);
}

console.log('\nWith the admin credential it works');
{
  const r = await fetch(`${base}/bookmarks?${await adminQ()}&frequency=${HZ}&name=${NAME}&mode=am`,
                        { method: 'POST' });
  ok(r.status === 200, 'admin CAN add', `(got ${r.status})`);
  ok((await list()).some?.((b) => b?.name === NAME), 'and it is really there');
  const d = await fetch(`${base}/bookmarks?${await adminQ()}&frequency=${HZ}`, { method: 'DELETE' });
  ok(d.status === 200, 'admin CAN delete', `(got ${d.status})`);
  ok(!(await list()).some?.((b) => b?.name === NAME), 'and it is gone again');
}

console.log(`\n${fail ? `${fail} FAILURES, ` : ''}${pass} passed`);
process.exit(fail ? 1 : 0);
