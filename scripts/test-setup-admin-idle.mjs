// test-setup-admin-idle.mjs — ★★★ beta98 THE SETUP PAGE'S ADMIN LOGIN IS AN IDLE TIMEOUT (Stuart 11-28, the Pi 500: a save at
// 14:39:21 worked; from 14:40:23 every config/admin call was refused 401 and the page said nothing — "later changes didn't
// save"). The page's REAL code (vibe_setup_page.h, `let TICKET` … authQuery) against a fake server that mints and checks
// tickets the way local_sdr_shim.cpp /vibeserver/admin-ticket does (a 10-minute lease, renewed only from a VALID ticket,
// origin kept, 12 h from the password at most), on a SIMULATED clock:
//   • active editing for 30 minutes → never lapses, every save accepted;
//   • idle past the limit → lapses: the inline prompt, NOTHING sent with the dead ticket;
//   • background polls alone (a tab left open) → lapses — polls never renew;
//   • a 401 from the server (the ticket refused) → the prompt at once, no 401 loop; callers held, a few, the rest refused;
//   • the password in the prompt → a fresh ticket, the refused SAVE sent again with the same edits; a wrong password says so;
//   • the 12-hour chain is kept: renewing never outlives it.
// Silent, no browser.   node scripts/test-setup-admin-idle.mjs
import fs from 'node:fs';
const src = fs.readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
let pass = 0, fail = 0;
const ok = (c, w) => { console.log(`   ${c ? 'ok  ' : 'FAIL'} ${w}`); c ? pass++ : fail++; };
const a = src.indexOf('let TICKET = (() => {'), b = src.indexOf('\n}\n', src.indexOf('async function authQuery() {')) + 3;
if (a < 0 || b < a) { console.log('   FAIL the admin login code was not found'); process.exit(1); }
let block = src.slice(a, b);
ok(/function adminClock\(\) \{ return Date\.now\(\); \}/.test(block), 'one clock (adminClock) — the test replaces it');
block = block.replace('function adminClock() { return Date.now(); }', 'function adminClock() { return CLOCK.now; }');
ok(/hmacSha256\(bytesOf\(PASS\), bytesOf\(nonce\)\)/.test(block), 'the password handshake path is still there');

// ── a fake server: tickets "T<n>.<origin>.<expiry>", 600 s lease, 12 h chain ──
const CLOCK = { now: 1_000_000_000 };
const PW = 'correct horse';
const S = { tickets: new Map(), n: 0, calls: [], saves: [], stale: 0 };
const mint = (origin) => { const exp = Math.min(CLOCK.now + 600e3, origin + 12 * 3600e3); const t = `T${++S.n}`; S.tickets.set(t, { origin, exp }); return { t, ttl: Math.max(0, Math.round((exp - CLOCK.now) / 1000)) }; };
const valid = (t) => { const x = S.tickets.get(t); return x && CLOCK.now < x.exp ? x : null; };
const resp = (status, body) => ({ status, ok: status >= 200 && status < 300, json: async () => body ?? {} });
const fakeFetch = async (u, o) => {
  const url = new URL(String(u), 'http://box');
  const q = url.searchParams, t = q.get('vs_admin_ticket'), n = q.get('vs_admin_nonce'), au = q.get('vs_admin_auth');
  S.calls.push({ path: url.pathname, t, at: CLOCK.now });
  if (url.pathname === '/vibeserver/auth') return resp(200, { nonce: 'N' + S.calls.length });
  const pwOk = n && au === 'H(' + PW + '|' + n + ')';
  const tk = t ? valid(t) : null;
  if (t && !tk) S.stale++;
  if (url.pathname === '/vibeserver/admin-ticket') {
    if (pwOk) { const m = mint(CLOCK.now); return resp(200, { ticket: m.t, ttl: m.ttl }); }
    if (tk && CLOCK.now - tk.origin < 12 * 3600e3) { const m = mint(tk.origin); return resp(200, { ticket: m.t, ttl: m.ttl }); }
    return resp(401);
  }
  if (!(pwOk || tk)) return resp(401);
  if (o && o.method === 'POST') S.saves.push({ body: o.body, at: CLOCK.now });
  return resp(200, {});
};
// ── the page around it ──
const listeners = {};
const el = (id) => ({ id, value: '', textContent: '', classList: { s: new Set(['hide']), add(c) { this.s.add(c); }, remove(c) { this.s.delete(c); }, contains(c) { return this.s.has(c); } }, focus() {} });
const els = { adminLapse: el('adminLapse'), adminLapsePass: el('adminLapsePass'), adminLapseErr: el('adminLapseErr') };
const win = { fetch: fakeFetch };
const env = {
  $: (id) => els[id], window: win, location: { search: '', pathname: '/setup' }, history: { replaceState() {} },
  document: { addEventListener: (ev, f) => { (listeners[ev] = listeners[ev] || []).push(f); } },
  fetch: (...x) => win.fetch(...x),   // ★ a bare fetch in the page IS window.fetch (which the page wraps)
  hmacSha256: (k, m) => 'H(' + k + '|' + m + ')', bytesOf: (s) => s, toHex: (x) => x, CLOCK,
};
const api = new Function(...Object.keys(env), `let PASS = "";\n${block}\nreturn { authQuery, adminSave, adminSignedIn, adminUnlock, adminActivity, ADMIN,
  get TICKET() { return TICKET; }, setPass: (p) => { PASS = p; }, get PASS() { return PASS; }, ADMIN_IDLE_MS, ADMIN_RENEW_MS };`)(...Object.values(env));
const fetch = (...x) => win.fetch(...x);   // the page's watched fetch (it replaced window.fetch)
const act = (ev = 'input') => (listeners[ev] || []).forEach((f) => f({ isTrusted: true }));
const tick = (ms) => { CLOCK.now += ms; };
const lapsed = () => !els.adminLapse.classList.contains('hide');
const save = async (body) => { const r = await api.adminSave('/vibeserver/config', { method: 'POST', body }); return r.status; };
const poll = async () => { try { const r = await fetch('/vibeserver/sdr-changes?' + await api.authQuery()); return r.status; } catch (e) { return 'refused'; } };
const settle = () => new Promise((r) => setTimeout(r, 0));

// ── 1. signed in with the password: exchanged for a ticket, the password not kept ──
api.setPass(PW);
await api.adminSignedIn();
ok(/^T\d+$/.test(api.TICKET) && api.PASS === '', `a password sign-in is traded for a ticket and the password dropped (${api.TICKET})`);
ok(['pointerdown', 'keydown', 'input', 'change', 'wheel', 'scroll', 'touchstart'].every((e) => listeners[e]), 'activity = edits, clicks, keys, scroll, touch');

// ── 2. thirty minutes of editing, a save every five: never lapses ──
{
  let refused = 0;
  for (let m = 1; m <= 30; m++) {
    tick(60e3); act('input'); await settle(); await settle();
    if (m % 5 === 0) { act('pointerdown'); if ((await save('{"edit":' + m + '}')) !== 200) refused++; }
  }
  ok(!lapsed() && refused === 0 && S.saves.length === 6, `30 minutes of editing (simulated): no lapse, all 6 saves accepted (refused ${refused})`);
  ok(S.stale === 0, `…and not one call carried a dead ticket (${S.stale})`);
  const renews = S.calls.filter((c) => c.path === '/vibeserver/admin-ticket').length;
  ok(renews >= 25 && renews <= 33, `renewed on activity, at most once a minute (${renews} renewals in 30 min)`);
}

// ── 3. a tab left open with background polls only: lapses, and polls never renewed it ──
{
  const before = S.calls.filter((c) => c.path === '/vibeserver/admin-ticket').length;
  let codes = [];
  for (let s = 0; s < 13 * 60; s += 10) { tick(10e3); codes.push(await Promise.race([poll(), new Promise((r) => setTimeout(() => r('held'), 5))])); }
  const after = S.calls.filter((c) => c.path === '/vibeserver/admin-ticket').length;
  ok(after === before, `background polls never renew the login (${after - before} renewals while idle)`);
  ok(lapsed(), 'idle past the limit with only polls → the login lapses: the inline prompt');
  ok(S.stale === 0, `…no call was sent with the dead ticket — the page knew its lease (${S.stale} stale)`);
  const firstHeld = codes.indexOf('held');
  ok(firstHeld > 50 && firstHeld <= 62, `polls ran for the ten minutes, then were held (${firstHeld} polls before)`);
  ok(codes.includes('refused'), 'past a few held calls the rest are refused at once — no queue to flood back');
}

// ── 4. a save while lapsed: held, then sent AGAIN with the same edits once the password is in ──
{
  const n0 = S.saves.length;
  const p = save('{"the":"unsaved edits"}');
  await settle();
  els.adminLapsePass.value = 'wrong';
  await api.adminUnlock();
  ok(lapsed() && /not accepted/.test(els.adminLapseErr.textContent), `a wrong password: still locked, said so ("${els.adminLapseErr.textContent}")`);
  els.adminLapsePass.value = PW;
  await api.adminUnlock();
  const st = await Promise.race([p, new Promise((r) => setTimeout(() => r('still held'), 50))]);
  ok(!lapsed() && els.adminLapsePass.value === '', 'the right password: unlocked, the field cleared');
  ok(st === 200 && S.saves.length === n0 + 1 && S.saves[S.saves.length - 1].body === '{"the":"unsaved edits"}', `the held save went through with the SAME edits (${st})`);
}

// ── 5. the server refuses the ticket (restarted with a new secret): the prompt at once, no 401 loop ──
{
  S.tickets.clear();
  const c0 = S.calls.length;
  const st = await Promise.race([save('{"x":1}'), new Promise((r) => setTimeout(() => r('held'), 20))]);
  for (let i = 0; i < 10; i++) { tick(5e3); await Promise.race([poll(), new Promise((r) => setTimeout(r, 2))]); }
  const sent401 = S.calls.slice(c0).filter((c) => c.t && !valid(c.t)).length;
  ok(lapsed() && st === 'held', 'a 401 → the inline prompt, and the save is held for the password, not dropped');
  ok(sent401 <= 1, `one refused call, then silence — no polling storm (${sent401} sent with the refused ticket)`);
  els.adminLapsePass.value = PW; await api.adminUnlock(); await settle();
  ok(!lapsed() && S.saves[S.saves.length - 1].body === '{"x":1}', 'unlocked: the refused save is sent again');
}

// ── 6. the 12-hour chain: renewing on activity never outlives the password ──
{
  let lapsedAt = 0;
  for (let m = 1; m <= 13 * 60; m++) { tick(60e3); act('keydown'); await settle(); await settle(); if (!lapsedAt) { await Promise.race([poll(), new Promise((r) => setTimeout(r, 5))]); if (lapsed()) lapsedAt = m; } }
  ok(lapsedAt >= 11.9 * 60 && lapsedAt <= 12.1 * 60, `busy all day: the login ends 12 h after the password, never later (${(lapsedAt / 60).toFixed(2)} h)`);
}

// ── the source: both saves go through adminSave; a password sign-in trades for a ticket ──
ok((src.match(/await adminSave\("\/vibeserver\/config", \{method:"POST"/g) || []).length === 2, 'both config saves (radio, and apply + restart) re-send after an unlock');
ok(/cfg = await r\.json\(\);\n    await adminSignedIn\(\);/.test(src), 'signIn → adminSignedIn');
ok(/<div id="adminLapse" class="hide" role="alert">/.test(src) && /Your admin login has lapsed: enter the password to save\./.test(src), 'the inline box, in Stuart\'s words');
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
