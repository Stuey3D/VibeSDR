// test-setup-feed-test.mjs — ★★ beta96 follow-up (docs/v12/FEED-TEST.md): TEST A FEED BEFORE SAVING IT, on the REAL setup
// page in headless Edge against a real server (one radio on a fake rtl_tcp). Stuart 11-27: "we need to test the connection
// before saving" and "the easiest way possible please".
//   · FIND: type an address → the server finds a fake UberSDR's HFDL add-on there → one line + ADD (name suggested).
//   · Advanced ADD tests first: a fake Beast server passes and is added; a closed port fails with the reason and
//     ADD ANYWAY, which adds it.
//   · TEST beside a source in the list says how it is now.
//   · Save: config.json holds the three sources the server's own parser accepts.
//   · ★★ b96-2 "Not at this site": the tick in the add form marks the ADS-B feed ("offsite":true) and its row in the list
//     ("off-site"); not the HFDL one (the outline is ADS-B only); it saves, and the page signed in again shows it.
// Fakes on 127.0.0.1 only (never Stuart's receivers). Needs VIBESERVER_BIN and Microsoft Edge; SILENT (--mute-audio).
import net from 'node:net';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Microsoft Edge)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (await f()) return true; await sleep(100); } return !!(await f()); };

// ── the fakes ──
const FRAMES = ['8D4840D6202CC371C32CE0576098', '8D40621D58C382D690C8AC2863A7', '8D40621D58C386435CC412692AD6'];
function beast(hex, ts) {
  const m = Buffer.from(hex, 'hex'); const raw = [];
  for (let i = 5; i >= 0; --i) raw.push(Number((BigInt(ts) >> BigInt(8 * i)) & 0xffn));
  raw.push(0x40, ...m);
  const out = [0x1a, 0x33]; for (const b of raw) { out.push(b); if (b === 0x1a) out.push(0x1a); }
  return Buffer.from(out);
}
const socks = new Set();
const beastSrv = net.createServer((c) => {
  socks.add(c); c.on('error', () => {});
  let ts = 1000, k = 0;
  const t = setInterval(() => { const b = []; for (let i = 0; i < 10; i++, k++) b.push(beast(FRAMES[k % 3], ts += 300000)); c.write(Buffer.concat(b)); }, 50);
  c.on('close', () => clearInterval(t));
});
await new Promise((r) => beastSrv.listen(0, '127.0.0.1', r));
const fixture = fs.readFileSync(path.join(root, 'test/fixtures/uberhfdl/aircraft-2026-11-27.json'), 'utf8');
const delta = Math.floor(Date.now() / 1000) - 1795769725;
const body = fixture.replace(/"last_seen":(\d+)/g, (_, v) => `"last_seen":${Number(v) ? Number(v) + delta : 0}`);
let uberHits = 0;
const uber = http.createServer((q, s) => {
  if (q.url === '/addon/hfdl/aircraft') { uberHits++; s.writeHead(200, { 'Content-Type': 'application/json' }); s.end(body); }
  else { s.writeHead(404); s.end('404 page not found'); }
});
await new Promise((r) => uber.listen(0, '127.0.0.1', r));
const closed = await freePort();

const ADMIN = 'feedtest-admin-pass';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-feedtest-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
let P = 0;
for (let i = 0; i < 20 && !P; i++) { const p = await freePort(); if (await new Promise((r) => { const t = net.createServer(); t.once('error', () => r(false)); t.listen(p + 1, '127.0.0.1', () => t.close(() => r(true))); })) P = p; }
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({
  configured: true, fullMode: true, name: 'Feed test page', adminPass: ADMIN, port: P,
  radios: [{ serial: 'ft-rtl', driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true, users: 1, rate: 2048000, freq: 100000000, demodMode: 'wfm' }],
}));
let rtlPort = 0; while (!rtlPort || rtlPort === P || rtlPort === P + 1) rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: cfgPath, VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir };
const logPath = path.join(dir, 'server.log'); const lfd = fs.openSync(logPath, 'w');
// ★ the Lenovo's layout: a front door (it owns the feeds and answers the page) and the radio's own process
const srv = spawn(BIN, [], { env, stdio: ['ignore', lfd, lfd] });
const rfd = fs.openSync(path.join(dir, 'radio.log'), 'w');
const radio = spawn(BIN, ['--radio-serial', 'ft-rtl', '--tcp', `127.0.0.1:${rtlPort}`], { env, stdio: ['ignore', rfd, rfd] });
let CDP = 0; while (!CDP || CDP === P || CDP === P + 1 || CDP === rtlPort) CDP = await freePort();
const br = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', `--remote-debugging-port=${CDP}`,
                        `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
const stopAll = () => { for (const p of [srv, radio, rtl, br]) { try { p && p.kill('SIGKILL'); } catch {} } for (const c of socks) c.destroy(); beastSrv.close(); uber.close(); };
process.on('exit', stopAll);
const deadline = setTimeout(() => { console.log('   FAIL aborted — the 180 s deadline passed'); stopAll(); process.exit(1); }, 180000);
const up = async (port) => { for (let i = 0; i < 150; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };

let call, js;
const val = (id, v) => js(`(() => { const e = document.getElementById(${JSON.stringify(id)}); e.value = ${JSON.stringify(v)}; e.dispatchEvent(new Event("change")); return 1; })()`);
const click = (id) => js(`(() => { const e = document.getElementById(${JSON.stringify(id)}); if (!e) return false; e.click(); return true; })()`);
const text = (id) => js(`(document.getElementById(${JSON.stringify(id)}) || {}).textContent || ""`);
const model = () => js(`JSON.stringify(feedsModel)`).then((s) => JSON.parse(s || '[]'));
try {
  ok(await up(P) && await up(P + 1), `the front door (${P}) and the radio (${P + 1}) are up`);
  let t; for (let i = 0; i < 60 && !t; i++) { try { t = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find((x) => x.type === 'page'); } catch { await sleep(250); } }
  const cws = new WebSocket(t.webSocketDebuggerUrl); await new Promise((r) => { cws.onopen = r; });
  let id = 0; const pend = new Map(); cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  call = (method, params = {}) => new Promise((r) => { const n = ++id; pend.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); setTimeout(() => { if (pend.delete(n)) r({}); }, 15000); });
  js = async (e) => (await call('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await call('Runtime.enable');
  await call('Page.navigate', { url: `http://127.0.0.1:${P}/setup` });
  ok(await until(async () => await js(`(() => { const c = document.getElementById("signinCard"); return !!c && c.offsetParent !== null; })()`), 10000), 'the setup page asks to sign in');
  await js(`document.getElementById("pass").value = ${JSON.stringify(ADMIN)}; document.getElementById("signinBtn").click(); 1`);
  ok(await until(async () => !!(await js(`!!document.getElementById("feedFindGo") && document.getElementById("feedFindGo").__wired !== 0 && typeof feedsModel !== "undefined" && !!document.getElementById("feedFmt").__wired`)), 15000), 'signed in: the feeds card is wired');
  await js(`document.getElementById("feedsAdd").open = true; 1`);

  console.log('── FIND ──');
  await val('feedFind', `127.0.0.1:${uber.address().port}`);
  await click('feedFindGo');
  ok(await until(async () => /^Found: UberSDR HFDL \(port \d+\) — 253 aircraft \(24\d with positions\)/.test(await text('feedFindMsg')), 12000), `★ found: ${await text('feedFindMsg')}`);
  ok(await js(`document.querySelector('#feedFound input[data-foundname="0"]').value`) === 'UberSDR HFDL', 'the name is suggested');
  await js(`document.querySelector('#feedFound button[data-foundadd="0"]').click(); 1`);
  let m = await model();
  ok(m.length === 1 && m[0].fmt === 'uberhfdl' && m[0].port === uber.address().port && m[0].name === 'UberSDR HFDL' && m[0].label === 'hfdl', `★ ADD: ${JSON.stringify(m[0])}`);
  ok(/Added — press Save/.test(await text('feedFindMsg')), 'and it says to Save');

  console.log('── Advanced: ADD tests first ──');
  await js(`document.getElementById("feedAdv").open = true; 1`);
  await val('feedFmt', 'beast'); await val('feedDir', 'connect');
  await val('feedAddr', `127.0.0.1:${beastSrv.address().port}`); await val('feedName', 'PiAware');
  // ★★ b96-2 the tick: "Not at this site"
  ok(await js(`(() => { const e = document.getElementById("feedOffsite"); return !!e && e.type === "checkbox" && !e.checked && !!e.closest("#feedsAdd") && !e.closest(".hide"); })()`) === true, '"Not at this site" is a tick in the add form (not hidden), unticked');
  ok(/Not at this site — its aircraft are shown but not counted in this server.s range outline/.test(await js(`document.getElementById("feedOffsite").closest("label").textContent.replace(/\\s+/g, " ")`)), '…with its words');
  await js(`document.getElementById("feedOffsite").checked = true; 1`);
  await click('feedAddGo');
  ok(await until(async () => (await model()).length === 2, 12000), '★ a passing test: added');
  m = await model();
  ok(m[1].offsite === true && m[0].offsite === undefined, `★ the ticked ADS-B feed is off-site, the HFDL one found before is not: ${JSON.stringify(m.map((f) => f.offsite))}`);
  ok(await js(`(() => { const t = [...document.querySelectorAll("#feedsList [data-offsitetag]")]; return t.length === 1 && t[0].dataset.offsitetag === "1" && /off-site/.test(t[0].textContent); })()`) === true, '★ the list tags it "off-site" (that row only)');
  ok(await js(`document.getElementById("feedOffsite").checked`) === false, '…and the tick is cleared for the next feed');
  ok(/^✓ Connected — [\d,]+ Beast messages in \d s, \d+ aircraft Added — press Save/.test(await text('feedAddMsg')), `…with what it found: ${await text('feedAddMsg')}`);
  await val('feedAddr', `127.0.0.1:${closed}`); await val('feedName', 'Pi off');
  await click('feedAddGo');
  ok(await until(async () => /✗ Nothing is listening on 127\.0\.0\.1:\d+/.test(await text('feedAddMsg')), 12000), `★ a failing test says why: ${await text('feedAddMsg')}`);
  ok((await model()).length === 2 && await js(`!document.getElementById("feedAddAnyway").classList.contains("hide")`), '…adds nothing, and offers ADD ANYWAY');
  await click('feedAddAnyway');
  m = await model();
  ok(m.length === 3 && m[2].port === closed && m[2].name === 'Pi off' && await js(`document.getElementById("feedAddAnyway").classList.contains("hide")`), '★ ADD ANYWAY adds it');

  console.log('── TEST beside a source ──');
  await js(`document.querySelector('#feedsList button[data-feedtest="1"]').click(); 1`);
  ok(await until(async () => /^✓ Connected — /.test(await js(`(document.querySelector('#feedsList [data-feedtested="1"]') || {}).textContent || ""`)), 10000),
     `the PiAware row: ${await js(`(document.querySelector('#feedsList [data-feedtested="1"]') || {}).textContent || ""`)}`);
  const hitsBefore = uberHits;
  await js(`document.querySelector('#feedsList button[data-feedtest="0"]').click(); 1`);
  ok(await until(async () => /UberSDR HFDL, 253 aircraft/.test(await js(`(document.querySelector('#feedsList [data-feedtested="0"]') || {}).textContent || ""`)), 10000), 'the UberSDR row');
  ok(uberHits - hitsBefore >= 1, 'one request per TEST press');

  console.log('── Save ──');
  await click('saveBtn');
  ok(await until(() => { try { return JSON.parse(JSON.parse(fs.readFileSync(cfgPath, 'utf8')).aircraftFeeds || '[]').length === 3; } catch { return false; } }, 10000), '★ saved: config.json holds the three feeds');
  const saved = JSON.parse(JSON.parse(fs.readFileSync(cfgPath, 'utf8')).aircraftFeeds || '[]');
  ok(saved[0].fmt === 'uberhfdl' && saved[1].fmt === 'beast' && saved[2].port === closed, `…as added: ${saved.map((f) => f.fmt + ':' + f.port).join(', ')}`);
  ok(saved[1].offsite === true && saved[0].offsite === undefined && saved[2].offsite === undefined, `★★ b96-2 saved: only the PiAware is off-site (${saved.map((f) => f.offsite ? 'off-site' : 'here').join(', ')})`);
  // …and LOADS: the page opened again reads it back from the server
  await call('Page.navigate', { url: `http://127.0.0.1:${P}/setup` });
  await until(async () => await js(`(() => { const c = document.getElementById("signinCard"); return !!c && c.offsetParent !== null; })()`), 10000);
  await js(`document.getElementById("pass").value = ${JSON.stringify(ADMIN)}; document.getElementById("signinBtn").click(); 1`);
  ok(await until(async () => (await model()).length === 3, 15000) && (await model())[1].offsite === true
     && await js(`(() => { const t = [...document.querySelectorAll("#feedsList [data-offsitetag]")]; return t.length === 1 && t[0].dataset.offsitetag === "1"; })()`) === true,
     '★★ b96-2 loaded: the page signed in again shows the PiAware off-site');
  ok(await up(P) && await until(async () => { try { const j = await (await fetch(`http://127.0.0.1:${P}/vibeserver/aircraft.json`)).json(); return (j.aircraft || []).some((a) => a.icao === '4521BF' && (a.src || []).includes('HFDL')); } catch { return false; } }, 25000),
     '★★ the server reads the UberSDR: its aircraft are on the map as HFDL');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  clearTimeout(deadline);
  stopAll();
  await sleep(400);
}
console.log(`\n${fail ? '✗' : '✓'} test-setup-feed-test: ${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs in ${dir})`); else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
