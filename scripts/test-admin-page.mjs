// ★★★ THE ADMIN PAGE, RENDERED FOR REAL, IN A REAL DOM.
//
// The API probes (tools/vibeserver-probes/admin-*.mjs) prove the SERVER is right. They cannot see
// the failure mode that has bitten this repo hardest: a control that is drawn, enabled and inert,
// or a field the server sends under one name and the page reads under another. Both render as a
// perfectly healthy-looking page showing nothing, or zeroes.
//
// So this loads the actual index.html into jsdom, runs the actual bundled admin module against
// the actual JSON shapes the server emits, and asserts what a person would SEE.
//
//   usage:  node scripts/test-admin-page.mjs
import { JSDOM } from 'jsdom';
import * as esbuild from 'esbuild';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what, extra = '') => { c ? (pass++, console.log(`   ok   ${what}`))
                                        : (fail++, console.log(`   FAIL ${what} ${extra}`)); };

// ── Build the admin module on its own, so a failure here is about THIS page ──────────────────
const built = await esbuild.build({
  entryPoints: [path.join(root, 'web/client/src/admin.ts')],
  bundle: true, write: false, format: 'esm', platform: 'browser', target: 'es2020',
  logLevel: 'silent',
}).catch((e) => { console.error('admin.ts does not build:\n', e.message); process.exit(1); });
const code = built.outputFiles[0].text;

// ── A DOM with the real markup ────────────────────────────────────────────────────────────────
const html = fs.readFileSync(path.join(root, 'web/client/index.html'), 'utf8');
const dom = new JSDOM(html, { runScripts: 'outside-only', url: 'http://127.0.0.1:48111/' });
const { window } = dom;
global.window = window; global.document = window.document;
global.fetch = async () => { throw new Error('no network in this test'); };

const mod = await import('data:text/javascript;base64,' + Buffer.from(code).toString('base64'));

// ── The fixtures are the SERVER'S OWN SHAPES, copied from a live run ─────────────────────────
const STATUS = {
  sys: { cores: 4, load1: 1.2, load5: 1.1, load15: 0.9, loadStatus: 'ok', cpuPct: 37.4,
         tempC: 72.4, tempStatus: 'warning', memTotalKB: 8000000, memAvailKB: 2000000,
         uptimeSec: 93600, governor: 'performance' },
  listeners: 3, maxUsers: 20, waiting: 1,
  // ★ frontEnd is nested INSIDE radio — the fixture mirrors the server's real shape exactly,
  //   because a fixture that flattens it would have hidden the bug it was written to catch.
  radio: { present: true, driver: 'sdrplay', centreHz: 6500000, spanHz: 8000000, lockedCentre: 6500000,
           frontEnd: { sysGainDb: 18.8, lna: 3, lnaStates: 10, ifGrDb: 43, overload: false, ifAgc: true } },
  txBytes: 12345678, txKbps: 4210, uniqueDay: 41, uniqueHour: 7,

  countries: [{ cc: 'GB', n: 22 }, { cc: 'DE', n: 9 }, { cc: 'US', n: 4 }],
  bans: [
    { cidr: '192.0.2.0/24', reason: 'scraper', at: 1786000000, until: 0, valid: true, asn: 0 },
    { cidr: 'AS16509', reason: 'hosting', at: 1786000050, until: 0, valid: true, asn: 16509 },
    { cidr: 'rubbish',      reason: 'typo',    at: 1786000100, until: 0, valid: false, asn: 0 },
  ],
  adminIdleMin: 30, sessionLimitMin: 0, terminal: false,
  publicSharing: true,   // the fixture is a PUBLIC receiver; Simple mode is tested separately
  maintenance: "restart,reboot,shutdown,update-check,update,update-all",  // a Linux server
};
const SESSIONS = { sessions: [
  { session: 'abc', ip: '192.168.1.9', vfoHz: 7100000, mode: 'lsb', bwHz: 2700,
    audio: true, opus: true, dropped: 0, zoomed: false, occupant: true, secs: 4327, cc: 'GB', net: 'AS5089 Virgin Media' },
  { session: 'def', ip: '10.0.0.5', vfoHz: 14074000, mode: 'usb', bwHz: 3000,
    audio: false, opus: false, dropped: 12, zoomed: true, occupant: false, cc: '', net: '' },
], adminOk: true };
const CONNS = { connections: [
  { at: 1786040000, end: 0,          ip: '192.168.1.9', session: 'abc', agent: 'Mozilla/5.0', reason: '',        bytes: 0 },
  { at: 1786030000, end: 1786031000, ip: '203.0.113.4', session: 'x',   agent: 'python-requests/2.31', reason: 'banned', bytes: 10, cc: 'NL' },
  { at: 1786020000, end: 1786021000, ip: '10.0.0.5',    session: 'y',   agent: '', reason: 'timeout', bytes: 99 },
] };
const HIST = { fields: ['at','load1','tempC','listeners','kbps'],
               rows: Array.from({ length: 40 }, (_, i) => [1786040000 + i, 0.5 + i * 0.02, 60 + i * 0.3, i % 5, 100 * i]) };

// ★ The module polls the network, which this test does not have. Drive the renderers directly
//   through the same entry point the poll uses, by stubbing fetch per call.
const responses = { status: STATUS, sessions: SESSIONS, connections: CONNS, history: HIST };
global.fetch = async (url) => {
  if (String(url).includes('/vibeserver/auth')) return { ok: true, json: async () => ({ nonce: 'deadbeef' }) };
  const m = String(url).match(/\/vibeserver\/admin\/(\w+)/);
  const body = responses[m?.[1]];
  if (!body) return { ok: false, status: 404, json: async () => ({}) };
  return { ok: true, status: 200, json: async () => body };
};
window.fetch = global.fetch;

mod.openAdmin('127.0.0.1:48111', 'secret');
await new Promise((r) => setTimeout(r, 200));   // let the first refresh settle

const $ = (id) => window.document.getElementById(id);
const text = (id) => ($(id)?.textContent ?? '').replace(/\s+/g, ' ').trim();

console.log('\n1. THE PANEL OPENS');
ok($('adminPanel').hidden === false, 'the admin panel is visible');
ok(text('adminHost').includes('127.0.0.1'), 'it names the server', text('adminHost'));

console.log('\n2. HEALTH CARDS — the numbers a person actually reads');
{
  const h = text('adminHealth');
  ok(/37%/.test(h), 'shows CPU USAGE as a percentage — the number Stuart reads', h.slice(0, 150));
  ok(/load 1\.20/.test(h), 'and keeps the load average beside it', h.slice(0, 150));
  ok(/4 cores/.test(h), 'shows the core count');
  ok(/72\.4°C/.test(h), 'shows the CPU temperature');
  ok(/3 \/ 20/.test(h), 'shows listeners against the cap');
  ok(/1 waiting/.test(h), 'shows the queue');
  ok(/4210 kbps/.test(h), 'shows the uplink rate');
  ok(/SDRPLAY/.test(h), 'names the radio');
  ok(/6\.500 MHz/.test(h), 'shows the centre frequency');
  ok(/18\.8 dB/.test(h), 'shows the front end system gain', h.slice(0,220));
  ok(/IF GR 43 dB/.test(h), 'and the IF gain reduction');
  ok(/AGC auto/.test(h), 'and whether the AGC is in charge');
  ok(/1d 2h/.test(h), 'shows uptime as a duration, not seconds', h);
  ok(/75%/.test(h), 'shows memory used as a percentage');
  // ★ Status must reach the DOM as a class, or the colour coding is decorative.
  const warnCards = $('adminHealth').querySelectorAll('.aCard.warn');
  ok(warnCards.length >= 1, 'the warm CPU card carries the warning class', `(${warnCards.length})`);
  ok($('adminHealth').querySelectorAll('.aCard.crit').length === 0, 'nothing is falsely critical');
}

console.log('\n3. ABSENT IS ABSENT — never a fake zero');
{
  // Re-render with the shape a Mac / container actually returns.
  responses.status = { ...STATUS, sys: { cores: 10, loadStatus: 'unknown', tempStatus: 'unknown' } };
  mod.openAdmin('127.0.0.1:48111', 'secret');
  await new Promise((r) => setTimeout(r, 150));
  const h = text('adminHealth');
  ok(/not available/.test(h), 'a missing sensor reads "not available"', h.slice(0, 160));
  ok(!/0\.0°C/.test(h) && !/0°C/.test(h), 'and NEVER as 0 °C', h.slice(0, 160));
  ok($('adminHealth').querySelectorAll('.aCard.none').length >= 2, 'both unknowns are styled as absent');
  responses.status = STATUS;
  mod.openAdmin('127.0.0.1:48111', 'secret');
  await new Promise((r) => setTimeout(r, 150));
}

console.log('\n3b. WHERE THEY CONNECT FROM');
{
  const rows = $('adminCountries').querySelectorAll('.ccRow');
  ok(rows.length === 3, 'a bar per country', `(${rows.length})`);
  const t = text('adminCountries');
  ok(/\u{1F1EC}\u{1F1E7}/u.test($('adminCountries').innerHTML), 'the GB flag is rendered');
  ok(/22/.test(t), 'and the count');
  // ★ The widest bar must be the biggest country, or the chart is decoration.
  const widths = [...rows].map((r) => parseFloat(r.querySelector('.ccBar i').style.width));
  ok(widths[0] === 100 && widths[0] > widths[1] && widths[1] > widths[2],
     'bars are scaled to the largest', widths.join(','));
  ok($('adminNoCountries').hidden, 'and the "no data" note is hidden');
}

console.log('\n4. LISTENERS');
{
  const rows = $('adminSessions').querySelectorAll('tbody tr');
  ok(rows.length === 2, 'one row per listener', `(${rows.length})`);
  const t = text('adminSessions');
  ok(/192\.168\.1\.9/.test(t), 'shows the address');
  ok(/\u{1F1EC}\u{1F1E7}/u.test($('adminSessions').innerHTML), 'shows a flag for a known country');
  // ★★ AND NOTHING AT ALL for an unknown one — never a placeholder the user reads as an answer.
  const cells = [...$('adminSessions').querySelectorAll('tbody tr')][1];
  ok(!cells.querySelector('.cc'), 'and NO flag when the country is unknown',
     cells.innerHTML.slice(0, 80));
  ok(/7\.100 MHz/.test(t), 'shows the tuned frequency');
  ok(/LSB/.test(t), 'shows the mode');
  ok(/1h 12m/.test(t), 'shows how long they have been on, as a duration', t);
  ok(/zoom/.test(t), 'flags the listener using their own zoom');
  ok(/12/.test(t), 'shows dropped blocks');
  // ★★ THE BUTTONS MUST BE THERE AND MUST CARRY THEIR TARGET. This is the "drawn but inert"
  //    check: a DISCONNECT with no session id renders identically and does nothing.
  const kick = $('adminSessions').querySelector('button[data-kick]');
  ok(!!kick && kick.getAttribute('data-kick') === 'abc', 'DISCONNECT carries the session id',
     kick?.getAttribute('data-kick'));
  const ban = $('adminSessions').querySelector('button[data-ban]');
  ok(!!ban && ban.getAttribute('data-ban') === '192.168.1.9', 'BLOCK carries the address',
     ban?.getAttribute('data-ban'));
  ok(text('adminListenerCount').includes('2'), 'the heading counts them');
}

console.log('\n5. BLOCKED ADDRESSES');
{
  const t = text('adminBans');
  ok(/192\.0\.2\.0\/24/.test(t), 'shows the range');
  ok(/scraper/.test(t), 'shows the reason');
  ok(/forever/.test(t), 'a permanent block says so rather than showing an epoch');
  // ★ A malformed entry must be VISIBLE and marked, not silently dropped — otherwise the owner
  //   believes a block is in force that can never match anything.
  ok(/never matches/.test(t), 'an invalid entry is shown AND marked as never matching', t.slice(0, 200));
  const un = $('adminBans').querySelector('button[data-unban]');
  ok(un?.getAttribute('data-unban') === '192.0.2.0/24', 'REMOVE carries the range');
}

console.log('\n5b. NETWORKS (ASN)');
{
  const t = text('adminSessions');
  ok(/AS5089 Virgin Media/.test(t), 'the listener\'s network is named', t.slice(0, 200));
  ok(/unknown/.test(t), 'and an unresolvable one says "unknown" rather than being blank');
  // ★★ The button must carry the AS NUMBER (what the server bans on) AND the full name (what the
  //    confirmation shows the human). Carrying only one of the two is how you get either a
  //    ban that does nothing or a dialogue that says "block AS5089?" and means nothing.
  const b = $('adminSessions').querySelector('button[data-banasn]');
  ok(b?.getAttribute('data-banasn') === 'AS5089', 'BLOCK NETWORK carries the AS number',
     b?.getAttribute('data-banasn'));
  ok(b?.getAttribute('data-netname') === 'AS5089 Virgin Media', 'and the readable name');
  // ★ No network known = no button. Offering one that cannot work is worse than none.
  const rows = [...$('adminSessions').querySelectorAll('tbody tr')];
  ok(!rows[1].querySelector('button[data-banasn]'),
     'and NO block-network button when the network is unknown');
  // The ban list distinguishes a network rule from an address rule.
  ok(/AS16509/.test(text('adminBans')), 'an ASN ban appears in the list');
  ok(/whole network/.test(text('adminBans')), 'and is labelled as a whole network');
}

console.log('\n6. CONNECTION HISTORY — the "ended" column is the point');
{
  const t = text('adminConns');
  ok(/banned/.test(t), 'shows a banned refusal');
  ok(/timeout/.test(t), 'shows a session timeout');
  ok(/connected/.test(t), 'shows a still-open connection as connected, not as a 0s one', t.slice(0, 200));
  ok(/python-requests/.test(t), 'shows the client, which is what separates a person from a scraper');
  const cells = [...$('adminConns').querySelectorAll('td')].map((c) => c.className);
  ok(cells.includes('why-banned'), 'the reason is class-tagged so it can be coloured', cells.join(','));
}

console.log('\n7. GRAPHS');
{
  const svgs = $('adminGraphs').querySelectorAll('svg polyline');
  ok(svgs.length >= 3, 'a line is drawn for each series', `(${svgs.length})`);
  ok([...svgs].every((s) => (s.getAttribute('points') ?? '').split(' ').length > 10),
     'each line has real points, not a stub');
  ok(text('adminGraphs').includes('CPU TEMP'), 'temperature is graphed when the machine has a sensor');
}

console.log('\n8. THE PAGE STATES ITS OWN LIMITS');
{
  const body = text('adminPanelBody');
  // ★ The VPN/bot-detection note was REMOVED at Stuart's request (2026-08-06): naming a feature
  //   we do not have only plants the idea that it is missing. What must survive is the warning
  //   with a real consequence — blocking an ISP takes out everyone behind it.
  ok(/blocking one blocks everybody behind it/i.test(body),
     'it warns that blocking an ISP hits everyone behind it');
  ok(!/VPN|bot detection/i.test(body), 'and does NOT raise VPN or bot detection at all');
  ok(/no web terminal/i.test(body), 'and that there is deliberately no web terminal');
  ok(/Tailscale|WireGuard/.test(body), 'and points at the right answer for a real shell');
}

console.log('\n8b. SIMPLE MODE HIDES THE STRANGER-MANAGEMENT PANELS');
{
  responses.status = { ...STATUS, publicSharing: false };
  mod.openAdmin('127.0.0.1:48111', 'secret');
  await new Promise((r) => setTimeout(r, 200));
  for (const id of ['secListeners', 'secBlocking', 'secHistory', 'secCountries']) {
    ok($(id).hidden === true, `${id} is hidden`, `(hidden=${$(id).hidden})`);
  }
  // ★★ HEALTH AND MAINTENANCE STAY. They are useful on a household receiver too — "your Pi is at
  //    82 °C" does not care how many people are listening — and removing the maintenance buttons
  //    would take away the easiest way for a non-technical owner to update.
  ok(!$('adminHealth').hidden && text('adminHealth').length > 0, 'health is still shown');
  ok(text('adminPanelBody').includes('CHECK FOR UPDATES'), 'maintenance is still shown');
  // ★ And it EXPLAINS the shorter page, so nothing reads as missing or broken.
  ok($('adminSimpleNote').hidden === false, 'the "local sharing" note is shown');
  ok(/local sharing/i.test(text('adminSimpleNote')), 'and says why');
  // ★ The header must still update — an early return past it would leave it stale.
  ok(text('adminHost').includes('127.0.0.1'), 'the header still names the server');

  responses.status = STATUS;
  mod.openAdmin('127.0.0.1:48111', 'secret');
  await new Promise((r) => setTimeout(r, 200));
  ok($('secListeners').hidden === false, 'and Full mode brings them back');
}

console.log('\n8c. A PLATFORM THAT CANNOT DO MAINTENANCE IS NOT OFFERED IT');
{
  // ★ macOS and Android: a reboot needs someone physically present (FileVault / replugging the
  //   radio), and neither updates through apt. Buttons there would strand the receiver.
  responses.status = { ...STATUS, maintenance: '' };
  mod.openAdmin('127.0.0.1:48111', 'secret');
  await new Promise((r) => setTimeout(r, 200));
  ok($('secMaintenance').hidden === true, 'the whole maintenance section is hidden');

  // ★ And a platform offering only SOME actions gets only those — per button, not all-or-nothing.
  responses.status = { ...STATUS, maintenance: 'restart' };
  mod.openAdmin('127.0.0.1:48111', 'secret');
  await new Promise((r) => setTimeout(r, 200));
  ok($('secMaintenance').hidden === false, 'a restart-only platform still gets the section');
  ok($('actRestart').hidden === false, 'and the restart button');
  ok($('actReboot').hidden === true, 'but NOT reboot');
  ok($('actUpdate').hidden === true, 'and NOT update');
  ok($('updSchedRow').hidden === true, 'and no scheduler, with nothing to schedule');

  responses.status = STATUS;
  mod.openAdmin('127.0.0.1:48111', 'secret');
  await new Promise((r) => setTimeout(r, 200));
  ok($('actReboot').hidden === false, 'a Linux server gets them all back');
}

console.log('\n9. CLOSING STOPS THE POLLING');
{
  mod.closeAdmin();
  ok($('adminPanel').hidden === true, 'the panel hides');
  ok(mod.isAdminOpen() === false, 'and reports itself closed');
  let calls = 0;
  const real = global.fetch;
  global.fetch = window.fetch = async (...a) => { calls++; return real(...a); };
  await new Promise((r) => setTimeout(r, 300));
  ok(calls === 0, 'and makes no further requests — a closed page must not poll the machine',
     `(${calls} calls)`);

  // ★★★ AND RE-OPENING MUST NOT STACK ANOTHER TIMER ON TOP.
  //     The check above passes even if the cleanup is deleted, because refresh() returns early
  //     while closed — so it proves the GUARD, not the cleanup. THIS is what the cleanup
  //     prevents: an owner who opens and closes the page five times polling the machine they are
  //     worried about five times as often, which is the one thing this page must not do.
  //     ★ Mutation-checked. The load-bearing clear is the one in OPENADMIN, not the one in
  //       closeAdmin — deleting closeAdmin's leaves a single idle timer that the next open
  //       replaces anyway, and all assertions stay green. Deleting openAdmin's fails this one.
  //       Do not "simplify" by trusting closeAdmin to be the only cleanup.
  for (let i = 0; i < 4; i++) { mod.openAdmin('127.0.0.1:48111', 'secret'); mod.closeAdmin(); }
  mod.openAdmin('127.0.0.1:48111', 'secret');
  await new Promise((r) => setTimeout(r, 120));   // let the immediate refresh finish
  calls = 0;
  await new Promise((r) => setTimeout(r, 2400));  // ~1 poll at the 2 s interval
  const perPoll = 4;                              // status + sessions + connections + history
  ok(calls <= perPoll * 2, 'repeated open/close does not multiply the polling rate',
     `(${calls} requests in one interval; one poll is ${perPoll})`);
  mod.closeAdmin();
}

console.log(`\n${fail ? `${fail} FAILURES, ` : ''}${pass} passed`);
process.exit(fail ? 1 : 0);
