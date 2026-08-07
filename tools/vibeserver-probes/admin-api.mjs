// Exercise /vibeserver/admin/* — the gate, the monitor, the ban list, and the actions.
//
// ★ Run it against a BROKEN build first. Every probe in this directory that was trusted without
//   doing that has, at least once, reported a pass on code that did not work.
//
//   usage:  node admin-api.mjs <port> [admin-password]
import crypto from 'node:crypto';
// ★ HOST is an env var so this can be pointed at the Pi. The ban tests use TEST-NET-2/3
//   (192.0.2.0/24, 203.0.113.0/24) precisely so running it against a LIVE receiver cannot
//   block a real listener — never put a real address in a probe that runs unattended.
const PORT = process.argv[2], HOST = process.env.HOST ?? '127.0.0.1', PASS = process.argv[3] ?? 'secret';
const base = `http://${HOST}:${PORT}`;

let pass = 0, fail = 0;
const ok = (cond, what, extra = '') => {
  if (cond) { pass++; console.log(`   ok   ${what}`); }
  else      { fail++; console.log(`   FAIL ${what} ${extra}`); }
};

async function nonce() { const r = await fetch(`${base}/vibeserver/auth`); return (await r.json()).nonce; }
const hmac = (secret, n) => crypto.createHmac('sha256', secret).update(n).digest('hex');
async function authQ(p = PASS) { const n = await nonce(); return `vs_admin_nonce=${n}&vs_admin_auth=${hmac(p, n)}`; }
const get  = async (path)       => fetch(`${base}/vibeserver/admin/${path}?${await authQ()}`);
const post = async (path, body) => fetch(`${base}/vibeserver/admin/${path}?${await authQ()}`,
                                         { method: 'POST', body: JSON.stringify(body) });

console.log('\n1. THE GATE — every endpoint must refuse without the password');
for (const p of ['status', 'sessions', 'bans', 'connections', 'history']) {
  const r = await fetch(`${base}/vibeserver/admin/${p}`);
  ok(r.status === 401, `GET ${p} with no credentials -> 401`, `(got ${r.status})`);
}
{
  const n = await nonce();
  const r = await fetch(`${base}/vibeserver/admin/status?vs_admin_nonce=${n}&vs_admin_auth=${hmac('wrong', n)}`);
  ok(r.status === 401, 'wrong password -> 401', `(got ${r.status})`);
  // ★ A WRONG guess now counts toward the brute-force backoff (it should — that is the whole
  //   point of the fix). One is harmless, but running this probe twice in quick succession can
  //   leave the NEXT request locked out and looking like a failure. Pause so the rest of the run
  //   measures the server rather than our own deliberate bad password.
  await new Promise((r2) => setTimeout(r2, 1500));
}

console.log('\n2. STATUS — the hardware monitor');
{
  const r = await get('status');
  ok(r.status === 200, 'status -> 200', `(got ${r.status})`);
  const j = r.ok ? await r.json() : {};
  const sys = j.sys ?? {};
  ok(typeof sys.cores === 'number', 'reports CPU cores', JSON.stringify(sys));
  ok(typeof sys.loadStatus === 'string', `load status = ${sys.loadStatus}`);
  ok(typeof sys.tempStatus === 'string', `temp status = ${sys.tempStatus}`);
  ok(typeof j.listeners === 'number', `listeners = ${j.listeners}`);
  ok(typeof j.txKbps === 'number', `uplink = ${j.txKbps} kbps`);
  ok(j.radio && typeof j.radio.present === 'boolean', `radio present = ${j.radio?.present}, driver = ${j.radio?.driver}`);
  ok(j.terminal === false, 'declares that it has no web terminal');
  // ★ Absent, not zero. A CPU temperature of 0 C is indistinguishable from a very cold Pi, and
  //   an owner who believes it will not investigate the fan.
  ok(!('tempC' in j.sys) || j.sys.tempC > 0, 'temperature is absent rather than a fake zero', JSON.stringify(j.sys));
}

console.log('\n3. SESSIONS / CONNECTIONS / HISTORY');
for (const [p, key] of [['sessions', 'sessions'], ['connections', 'connections']]) {
  const r = await get(p);
  const j = await r.json();
  ok(Array.isArray(j[key]), `${p} returns an array`, JSON.stringify(j).slice(0, 80));
}
{
  const j = await (await get('history')).json();
  ok(Array.isArray(j.fields) && Array.isArray(j.rows), 'history is fields + rows', JSON.stringify(j).slice(0, 80));
}

console.log('\n4. THE BAN LIST');
{
  // start clean
  await post('unban', { cidr: '198.51.100.0/24' });
  await post('unban', { cidr: '203.0.113.9' });

  let r = await post('ban', { cidr: '198.51.100.0/24', reason: 'probe: a range' });
  ok(r.status === 200, 'ban a CIDR range -> 200', `(got ${r.status})`);

  r = await post('ban', { cidr: '203.0.113.9', reason: 'probe: one address', minutes: 60 });
  ok(r.status === 200, 'ban a single address with an expiry -> 200', `(got ${r.status})`);

  r = await post('ban', { cidr: 'not-an-address', reason: 'probe' });
  ok(r.status === 400, 'a malformed address is REJECTED, not silently ignored', `(got ${r.status})`);

  const j = await (await get('bans')).json();
  const byCidr = Object.fromEntries(j.bans.map(b => [b.cidr, b]));
  ok(!!byCidr['198.51.100.0/24'], 'the range is in the list');
  ok(byCidr['198.51.100.0/24']?.reason === 'probe: a range', 'the reason was kept');
  ok(byCidr['203.0.113.9']?.until > 0, 'the expiry was recorded', JSON.stringify(byCidr['203.0.113.9']));
  ok(byCidr['198.51.100.0/24']?.until === 0, 'a ban with no expiry is permanent');

  // ★★ THE PREFLIGHT MUST AGREE WITH THE BAN LIST. This is the half that is easy to ship
  //    broken: the list shows the ban, and the listener carries on connecting.
  //    Only checkable from a non-loopback address, which a local probe does not have — so this
  //    asserts the SHAPE and the log, and the Pi run is what proves the refusal.
  //    ★★ ONLY MEANINGFUL WHEN WE *ARE* THE LOOPBACK. Run with HOST pointed at the Pi, this
  //       request arrives from a LAN address, so a busy receiver correctly answers "in-use" —
  //       and the assertion failed while nothing was wrong. A test whose premise depends on
  //       where it is run from must check that premise, not assume it.
  if (HOST === '127.0.0.1' || HOST === 'localhost') {
    r = await fetch(`${base}/connection?user_session_id=probe`);
    const pf = await r.json();
    ok(pf.allowed === true, 'loopback is exempt from the ban list (it must never lock the host out)',
       JSON.stringify(pf));
  } else {
    console.log('   --   loopback exemption not checkable from a remote host (see admin-ban-enforce.mjs)');
  }

  r = await post('unban', { cidr: '198.51.100.0/24' });
  ok(r.status === 200, 'unban -> 200');
  r = await post('unban', { cidr: '198.51.100.0/24' });
  ok(r.status === 404, 'unbanning something not banned -> 404', `(got ${r.status})`);
  await post('unban', { cidr: '203.0.113.9' });

  const after = await (await get('bans')).json();
  ok(after.bans.every(b => b.cidr !== '198.51.100.0/24'), 'the unbanned range is gone');
}

console.log('\n5. KICK');
{
  const r = await post('kick', { session: 'nobody-by-that-name' });
  ok(r.status === 200, 'kicking an absent session is not an error', `(got ${r.status})`);
  ok((await r.json()).kicked === 0, 'and reports 0 kicked');
}

console.log('\n6. ACTIONS — a FIXED list, and nothing else gets through');
{
  let r = await post('action', { action: 'rm -rf /' });
  ok(r.status === 400, 'an arbitrary command is refused', `(got ${r.status})`);
  r = await post('action', { action: 'update-check' });
  // Not running as root in a dev shell, so the honest answer is a refusal with a reason.
  ok(r.status === 200 || r.status === 400, `a known action is understood (${r.status})`);
  const j = await r.json();
  ok(r.status === 200 || /privileges/.test(j.error ?? ''),
     'a non-root server says WHY rather than pretending it worked', JSON.stringify(j));
}

console.log('\n7. UNKNOWN ENDPOINT');
{
  const r = await get('no-such-thing');
  ok(r.status === 404, 'unknown admin endpoint -> 404', `(got ${r.status})`);
}

console.log(`\n${fail ? `${fail} FAILURES, ` : ''}${pass} passed`);
process.exit(fail ? 1 : 0);
