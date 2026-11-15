#!/usr/bin/env node
// ★★★ A NEW CARD'S ONE UPDATE CHECK, AS THE SETUP PAGE SHOWS IT (Stuart, 2026-10-28). The page's own functions, run against
// a fake server that walks the root helper's record through each outcome:
//   online + newer → "Checking…" → "Downloading…" → "Installing…" → the restart → "Updated to VibeServer 11.0.16."
//   online + same → "up to date" · offline → "cannot check for updates — proceeding with setup" · failure → said, and a
//   later visit NEVER asks again · Cancel → the cancel is sent, "unchanged" · Cancel too late (installing) → said so, and
//   the install finishes · still on the open setup hotspot → not asked yet · not a new card → nothing drawn.
// Silent, no browser. The helper's half (real apt + dpkg) is vibeserver/linux/test_first_update_docker.sh.
//   node scripts/test-setup-first-update.mjs
import { readFileSync } from 'node:fs';

const src = readFileSync(new URL('../android/app/src/main/cpp/vibe_setup_page.h', import.meta.url), 'utf8');
const html = src.split('R"HTML(')[1].split(')HTML"')[0];
const grab = name => {
  const i = html.search(new RegExp('(async )?function ' + name + '\\('));
  if (i < 0) throw new Error(name + ' is missing from the setup page');
  let d = 0;
  for (let k = html.indexOf('{', html.indexOf(')', i)); k < html.length; k++) {
    if (html[k] === '{') d++;
    else if (html[k] === '}' && --d === 0) return html.slice(i, k + 1);
  }
  throw new Error(name + ' never closes');
};
const cancelHandler = (() => {
  const i = html.indexOf('$("firstUpdCancel").onclick = ');
  const j = html.indexOf('\n};', i);
  return html.slice(i, j + 3);
})();
const constLine = html.match(/const FIRST_UPD_RUNNING = .*\n/)[0];

let fails = 0;
const ok = (c, what) => { console.log(`  ${c ? '\x1b[32mok\x1b[0m  ' : '\x1b[31mFAIL\x1b[0m'} ${what}`); if (!c) fails++; };

/** One page, one fake server. `script(n, srv)` is called on every status read and returns the record (or null = down). */
function page({ wifi = { mode: 'joined', wired: false }, offered = true, script, pocket = {} }) {
  const els = {};
  const el = id => els[id] || (els[id] = {
    id, textContent: '', disabled: false, onclick: null, cls: new Set(id === 'firstUpdCard' ? ['hide'] : []),
    get classList() { const c = this.cls; return { add: x => c.add(x), remove: x => c.delete(x), contains: x => c.has(x),
      toggle: (x, on) => { if (on === undefined ? !c.has(x) : on) c.add(x); else c.delete(x); } }; },
  });
  const body = el('body');
  const srv = { actions: [], reads: 0, instance: 'old', said: [], reloaded: false };
  const fetch = async (url, opt = {}) => {
    const u = String(url);
    const J = (o, status = 200) => ({ ok: status < 400, status, json: async () => o });
    if (u.startsWith('/vibeserver/admin/action')) { srv.actions.push(JSON.parse(opt.body).action); return J({ ok: true }); }
    if (u.startsWith('/vibeserver/admin/status')) {
      const f = script(srv.reads++, srv);
      if (f === null) throw new Error('down');
      if (f === 401) return J({}, 401);
      return J({ maintenance: 'update,update-check' + (offered ? ',first-update,first-update-cancel' : ''), updates: { first: f } });
    }
    if (u.startsWith('/vibeserver/admin/maintenance-log')) return J({ running: srv.running !== false });
    if (u.startsWith('/vibeserver.json')) { if (srv.down) throw new Error('down'); return J({ instance: srv.instance }); }
    throw new Error('unexpected fetch ' + u);
  };
  const env = {
    $: el, fetch, document: { body }, POCKET: { ...pocket }, WIFI: wifi, BOOT_ID: 'old',
    authQuery: async () => 'a=1', wifiLoad: () => {}, location: { reload: () => { srv.reloaded = true; } },
    pocketWifiFirst: (mode, wired) => mode === 'setup-ap' && !wired,
    setTimeout: (f) => Promise.resolve().then(f),
  };
  const fn = new Function(...Object.keys(env), `
    let BOOT = BOOT_ID;
    ${constLine}
    let FIRST_UPD_ASKED = false; let FIRST_UPD_CANCEL = false;
    ${grab('firstUpdVer')}
    ${grab('firstUpdWords')}
    ${grab('firstUpdShow')}
    ${grab('firstUpdStatus')}
    ${grab('firstUpdMaybe')}
    ${grab('firstUpdRun').replace(/BOOT_ID/g, 'BOOT')}
    ${cancelHandler}
    return { maybe: firstUpdMaybe, boot: () => BOOT };`);
  const api = fn(...Object.values(env));
  // ★ The page's 1.5 s poll, without the wait.
  return { ...api, els, el, srv, body };
}

// The helper's record, step by step. Each array entry is what one status read returns.
const R = (state, extra = {}) => ({ state, from: '11.0.15-1', to: '11.0.16-1', why: '', ...extra });
const steps = (list, after) => (n, srv) => { const x = n < list.length ? list[n] : list[list.length - 1]; if (after) after(n, srv); return typeof x === 'function' ? x(srv) : x; };
const run = async p => { const t = setTimeout(() => {}, 0); await p.maybe(); clearTimeout(t); };
const text = p => p.el('firstUpdText').textContent;

// The poll waits with setTimeout(r, 1500) — make every wait instant for the test.
const realST = globalThis.setTimeout;
globalThis.setTimeout = (f, ms) => realST(f, 0);

console.log('setup page — a new card\'s one update check');
{
  const seen = [];
  const p = page({ script: steps([
    R(''), R('') /* firstUpdMaybe's read, then the request waiting */, R('checking'), R('downloading'), R('installing'),
    R('updated'), null, null, (srv) => { srv.instance = 'new'; return R('updated'); },
  ], (n, srv) => { seen.push(n); }) });
  const shown = [];
  const orig = p.el('firstUpdText');
  let v = '';
  Object.defineProperty(orig, 'textContent', { get: () => v, set: x => { v = x; shown.push(x); } });
  await run(p);
  ok(p.srv.actions.join() === 'first-update', 'online + newer: asks the helper ONCE  [' + p.srv.actions.join() + ']');
  ok(shown.includes('Checking for a VibeServer update…'), '"Checking for a VibeServer update…"');
  ok(shown.includes('Downloading VibeServer 11.0.16…'), '"Downloading VibeServer 11.0.16…"');
  ok(shown.includes('Installing VibeServer 11.0.16…'), '"Installing VibeServer 11.0.16…"');
  ok(shown.some(s => /restarting/.test(s)), 'waits through the restart ("…reconnects by itself")');
  ok(text(p) === 'Updated to VibeServer 11.0.16.', 'ends "Updated to VibeServer 11.0.16."  [' + text(p) + ']');
  ok(!p.body.classList.contains('firstUpdBusy'), 'setup carries on: the rest of the page is back');
  ok(p.boot() === 'new', 'the NEW instance is the baseline for the next Save');
  ok(p.el('firstUpdCancel').classList.contains('hide'), 'no Cancel once finished');
}
{
  const p = page({ script: steps([R(''), R('checking'), R('uptodate', { to: '' })]) });
  await run(p);
  ok(text(p) === 'VibeServer is up to date (11.0.15).', 'online + same: "VibeServer is up to date (11.0.15)."  [' + text(p) + ']');
}
{
  const p = page({ script: steps([R(''), R('checking'), R('offline', { to: '' })]) });
  await run(p);
  ok(text(p) === 'VibeServer cannot check for updates — proceeding with setup.', 'offline: "…cannot check for updates — proceeding with setup."');
  ok(!p.body.classList.contains('firstUpdBusy'), '…and setup continues');
}
{
  const p = page({ script: steps([R(''), R('downloading'), R('failed', { why: 'download' })]) });
  await run(p);
  ok(/did not finish/.test(text(p)) && /11\.0\.15 is unchanged/.test(text(p)) && /Admin › Maintenance/.test(text(p)),
     'failure: said, old version unchanged, points to Admin › Maintenance');
  ok(!p.body.classList.contains('firstUpdBusy'), '…and setup continues');
  const again = page({ script: steps([R('failed', { why: 'download' })]) });
  await run(again);
  ok(again.srv.actions.length === 0, '★ a later visit NEVER asks again after a failure');
  ok(again.el('firstUpdCard').classList.contains('hide'), '…and draws nothing');
}
{
  let p;
  p = page({ script: steps([R(''), R('checking'),
    (srv) => { p.el('firstUpdCancel').onclick(); return R('downloading'); },
    (srv) => R(srv.actions.includes('first-update-cancel') ? 'cancelled' : 'downloading')]) });
  await run(p);
  ok(p.srv.actions.join() === 'first-update,first-update-cancel', 'Cancel sends first-update-cancel  [' + p.srv.actions.join() + ']');
  ok(text(p) === 'Update cancelled — VibeServer 11.0.15 is unchanged.', 'Cancel: "Update cancelled — VibeServer 11.0.15 is unchanged."');
  ok(!p.body.classList.contains('firstUpdBusy'), '…and setup continues');
}
{
  let p;
  const more = [];
  p = page({ script: steps([R(''), R('installing'),
    () => { p.el('firstUpdCancel').onclick(); return R('installing'); }, R('installing'),
    R('updated'), null, (srv) => { srv.instance = 'new'; return R('updated'); }]) });
  const m = p.el('firstUpdMore'); let v = '';
  Object.defineProperty(m, 'textContent', { get: () => v, set: x => { v = x; more.push(x); } });
  await run(p);
  ok(more.some(s => /Too late to cancel/.test(s)), 'Cancel while installing: "Too late to cancel — … finishing safely"');
  ok(text(p) === 'Updated to VibeServer 11.0.16.', '…and the install finishes: updated');
}
{
  const p = page({ wifi: { mode: 'setup-ap', wired: false }, script: steps([R('')]) });
  await run(p);
  ok(p.srv.actions.length === 0 && p.srv.reads === 0, 'still on the open setup hotspot (step 2 not done): not asked yet');
}
{
  const p = page({ offered: false, script: steps([R('')]) });
  await run(p);
  ok(p.srv.actions.length === 0 && p.el('firstUpdCard').classList.contains('hide'), 'not a new card (action not offered): nothing');
}
{
  const p = page({ script: steps([R('downloading'), R('downloading'), R('uptodate')]) });
  await run(p);
  ok(p.srv.actions.length === 0 && text(p) === 'VibeServer is up to date (11.0.15).', 'a reload mid-run follows it — never starts a second one');
}
{
  const p = page({ script: steps([R(''), R('downloading')]) });
  p.srv.running = false;   // ★ the helper died (power cut): the record stays on its last step, nothing runs
  await run(p);
  ok(/did not finish/.test(text(p)) && p.srv.actions.length === 1, 'a run that died is reported as not finished, not restarted');
}
ok(/id="firstUpdCard"/.test(html) && /id="firstUpdCancel"/.test(html), 'the card and its Cancel are on the page');
ok(/firstUpdMaybe\(\);\n\}/.test(html.slice(html.indexOf('async function wifiLoad'), html.indexOf('async function wifiLoad') + 4000)),
   'wifiLoad asks for it once the network state is known');
globalThis.setTimeout = realST;
process.exit(fails ? 1 : 0);
