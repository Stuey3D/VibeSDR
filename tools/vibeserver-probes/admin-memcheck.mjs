// "Check this radio's memory" through the real admin API (2026-11-03) — the gate and the serial checks.
//
//   usage:  node admin-memcheck.mjs <port> [admin-password] [known-serial]
//
// ★ What it proves everywhere: memcheck is refused without the password, refused with a serial outside the serial
//   alphabet (a path, an option, empty) BEFORE anything else looks at it, and another action cannot smuggle an argument.
// ★★ Where the server offers memcheck (a Linux package carrying diag/vibeserver-asan): a serial not in the settings is
//    refused by the daemon, and [known-serial] — a radio in the settings — is accepted (the request file then names it).
//    Elsewhere (macOS, Android, armhf, i386) the action is not offered and the answer says so; that is checked too.
// ★ Never point this at a server someone is listening to with a known serial: an accepted check stops that radio for a
//   minute. The made-up serials here (31A0B7C, 9999) match nothing real.
import crypto from 'node:crypto';
const PORT = process.argv[2], HOST = process.env.HOST ?? '127.0.0.1', PASS = process.argv[3] ?? 'secret';
const KNOWN = process.argv[4] ?? '';
const base = `http://${HOST}:${PORT}`;

let pass = 0, fail = 0;
const ok = (cond, what, extra = '') => {
  if (cond) { pass++; console.log(`   ok   ${what}`); }
  else      { fail++; console.log(`   FAIL ${what} ${extra}`); }
};
async function nonce() { const r = await fetch(`${base}/vibeserver/auth`); return (await r.json()).nonce; }
const hmac = (secret, n) => crypto.createHmac('sha256', secret).update(n).digest('hex');
async function authQ(p = PASS) { const n = await nonce(); return `vs_admin_nonce=${n}&vs_admin_auth=${hmac(p, n)}`; }
const act = async (body, auth = true) => {
  const r = await fetch(`${base}/vibeserver/admin/action${auth ? '?' + await authQ() : ''}`, { method: 'POST', body: JSON.stringify(body) });
  return { status: r.status, j: await r.json().catch(() => ({})) };
};

const st = await (await fetch(`${base}/vibeserver/admin/status?${await authQ()}`)).json();
const offered = String(st.maintenance ?? '').split(',').includes('memcheck');
console.log(`memcheck offered here: ${offered}`);

console.log('\n1. THE GATE');
{
  const r = await act({ action: 'memcheck', serial: '31A0B7C' }, false);
  ok(r.status === 401, 'memcheck with no credentials -> 401', `(got ${r.status})`);
}
console.log('\n2. THE SERIAL ALPHABET — refused at the endpoint, whatever the platform');
for (const s of ['../../etc/passwd', '-rf', '', 'a b', 'x;reboot', 'vibeserver@31A0B7C', 'A'.repeat(65)]) {
  const r = await act({ action: 'memcheck', serial: s });
  ok(r.status === 400 && /needs the serial/.test(r.j.error ?? ''), `serial ${JSON.stringify(s.slice(0, 20))} -> 400`, `(${r.status} ${r.j.error})`);
}
{
  const r = await act({ action: 'memcheck' });
  ok(r.status === 400, 'no serial at all -> 400', `(${r.status} ${r.j.error})`);
  const r2 = await act({ action: 'restart 31A0B7C' });
  ok(r2.status === 400, 'another action with an argument smuggled in -> 400', `(${r2.status} ${r2.j.error})`);
}
console.log('\n3. A WELL-FORMED SERIAL');
{
  const r = await act({ action: 'memcheck', serial: '9999' });
  if (offered) ok(r.status === 400 && /settings/.test(r.j.error ?? ''), 'a serial not in the settings -> 400 (the daemon)', `(${r.status} ${r.j.error})`);
  else ok(r.status === 400 && /does not offer/.test(r.j.error ?? ''), 'not offered on this server -> 400, and says so', `(${r.status} ${r.j.error})`);
  ok(!offered || Array.isArray(st.memcheck?.radios), 'where offered, the status lists the radios for the picker');
  ok(offered || st.memcheck === undefined, 'where not offered, the status carries no memcheck list (no button)');
  if (offered && KNOWN) {
    const k = await act({ action: 'memcheck', serial: KNOWN });
    ok(k.status === 200 && k.j.ok === true, `a radio in the settings (${KNOWN}) -> 200`, `(${k.status} ${k.j.error})`);
  }
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
