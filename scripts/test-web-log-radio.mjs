// test-web-log-radio.mjs — V12 beta92 THE LOGS' RADIO FILTER, through a real vibeserver and the REAL built web page
// (docs/v12/LOG-RADIO-FILTER.md). Stuart, 2026-11-22: the logs stay ONE PER SERVER; "in the logs themselves we could
// filter by sdr if multiple picked up the same signal".
//   A machine with TWO radios in its config ("HF loop" = this process, "VHF discone"). A gallery written by both — NAVTEX
//   from each, a FUNcube day log both wrote ("ri" lists both), FT8 cards from the HF radio only, and one OLD line with no
//   radio — and a station-store day file whose rows carry both radios' bits (one station both heard, one each, one old).
//   · gallery.json / stations.json: "radios" names both as the machine does; radio= filters (a view); the untagged show
//     under every radio; per-station "radios".
//   · the page: the gallery's radio chip row appears with 2+ radios and filters; it is NOT drawn for a kind only one radio
//     wrote (FT8); the DIGI MODES MAP window offers ALL RADIOS / each radio and filters its stations.
// ★ SILENT: headless Edge with --mute-audio, the fake dongle, nothing played. usage: VIBESERVER_BIN=… node scripts/test-web-log-radio.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }

/** vsconfig::radioId — FNV-1a 64 of the serial, folded to 32 bits, 8 hex. */
function radioId(serial) {
  let h = 1469598103934665603n;
  for (const c of Buffer.from(serial)) { h ^= BigInt(c); h = (h * 1099511628211n) & 0xffffffffffffffffn; }
  return Number((h ^ (h >> 32n)) & 0xffffffffn).toString(16).padStart(8, '0');
}
const A = radioId('lra'), B = radioId('lrb');

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-logradio-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Log radio test', adminPass: 'lr-admin',
  radios: [{ serial: 'lra', label: 'HF loop', enabled: true, configured: true, users: 4, rate: 2048000 },
           // ★ the second radio: named in the config (its process is not run here — its entries are on disk already)
           { serial: 'lrb', label: 'VHF discone', enabled: false, configured: true, users: 4, rate: 2048000 }] }));
const gdir = path.join(dir, 'data', 'gallery');
fs.mkdirSync(gdir);
let index = '';
const item = (i, k, ri, title) => {
  const f = `20261122-${String(100000 + i)}-${k}-${(1000 + i).toString(16)}.txt`;
  fs.writeFileSync(path.join(gdir, f), `item ${i}\n`);
  const o = { f, k, o: 'background', t: 1795000000 + i, hz: k === 'navtex' ? 518000 : 14074000, m: 'usb', d: k, p: 'Profile 1', r: 'x', title };
  if (ri) o.ri = ri;
  index += JSON.stringify(o) + '\n';
};
let n = 0;
for (let i = 0; i < 6; i++) item(n++, 'navtex', A, `navtex A${i}`);
for (let i = 0; i < 4; i++) item(n++, 'navtex', B, `navtex B${i}`);
item(n++, 'navtex', '', 'navtex OLD');                 // ★ an older build's line: no radio
item(n++, 'fctlm', `${A},${B}`, 'funcube day both');  // ★ the shared day log both radios wrote
for (let i = 0; i < 3; i++) item(n++, 'ft8', A, `ft8 A${i}`);
fs.writeFileSync(path.join(gdir, 'index.jsonl'), index);

// ── the station store: today's day file (vibe_station_store.h Hdr/Rec), bits from <gallery>/radios.idx ──
fs.writeFileSync(path.join(gdir, 'radios.idx'), `${A}\n${B}\n`);
const now = Math.floor(Date.now() / 1000), day = Math.floor(now / 86400) * 86400;
const dt = new Date(day * 1000);
const dayName = `${dt.getUTCFullYear()}${String(dt.getUTCMonth() + 1).padStart(2, '0')}${String(dt.getUTCDate()).padStart(2, '0')}.dat`;
const rows = [['G4BOTH', 'IO91', 3], ['G4AONE', 'IO92', 1], ['M0BONE', 'IO93', 2], ['OH2OLD', 'KP20', 0]];
const buf = Buffer.alloc(64 + 64 * rows.length);
buf.writeUInt32LE(0x31545356, 0); buf.writeUInt32LE(64, 4); buf.writeUInt32LE(rows.length, 8); buf.writeUInt32LE(0, 12);
buf.writeBigInt64LE(BigInt(day), 16);
rows.forEach(([call, grid, mask], i) => {
  const o = 64 + 64 * i;
  buf.write(call, o, 'latin1'); buf.write(grid, o + 16, 'latin1'); buf.write('FT8', o + 24, 'latin1'); buf.write('20m', o + 32, 'latin1');
  buf.writeUInt32LE(now - 600 + i, o + 40); buf.writeUInt32LE(now - 300 + i, o + 44); buf.writeUInt32LE(5, o + 48); buf.writeUInt32LE(14074000, o + 52);
  buf.writeInt8(-5, o + 56); buf.writeInt8(-7, o + 57); buf.writeUInt8(1, o + 58); buf.writeUInt8(1, o + 59); buf.writeUInt32LE(mask, o + 60);
});
fs.mkdirSync(path.join(gdir, 'stations'));
fs.writeFileSync(path.join(gdir, 'stations', dayName), buf);

const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-logradio-edge-'));
let edge = null;
const deadline = setTimeout(() => { console.log('   FAIL the test ran past its 120 s deadline'); try { edge && edge.kill(); srv.kill('SIGKILL'); rtl.kill(); } catch {} process.exit(1); }, 120000);
try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  console.log(`── the API (radios ${A} "HF loop", ${B} "VHF discone") ──`);
  const all = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=25`).then((r) => r.json());
  const names = (all.radios || []).map((r) => `${r.name}:${r.n}`).join(' ');
  ok(all.total === 15 && names === 'HF loop:10 VHF discone:5' && all.radioUnknown === 1, `gallery.json: 15 items, radios named with counts (${names}), 1 unknown`);
  const fb = all.items.find((x) => x.k === 'fctlm');
  ok(fb && fb.ri === `${A},${B}`, `the shared FUNcube log carries both radios (${fb && fb.ri})`);
  const onlyB = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=25&radio=${B}`).then((r) => r.json());
  ok(onlyB.total === 6 && onlyB.items.every((x) => !x.ri || x.ri.includes(B)) && onlyB.radio === B,
     `radio=VHF: its 4 NAVTEX + the shared log + the old one (${onlyB.total})`);
  const nvA = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=25&kind=navtex&radio=${A}`).then((r) => r.json());
  ok(nvA.total === 7, `NAVTEX + radio=HF: 6 + the old one (${nvA.total})`);
  const ft8 = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=25&kind=ft8`).then((r) => r.json());
  ok((ft8.radios || []).length === 1, 'FT8 (one radio wrote it): one radio listed');
  const junk = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=25&radio=%3Cscript%3E`).then((r) => r.json());
  ok(junk.total === 15 && junk.radio === undefined, 'a junk radio= is dropped (every item)');
  const st = await fetch(`${H}/vibeserver/stations.json?window=24h`).then((r) => r.json());
  const by = Object.fromEntries((st.stations || []).map((s) => [s.call, (s.radios || []).join(',')]));
  ok(by.G4BOTH === `${A},${B}` && by.G4AONE === A && by.M0BONE === B && by.OH2OLD === '', `stations.json: who heard each (${JSON.stringify(by)})`);
  ok((st.radios || []).map((r) => `${r.name}:${r.n}`).join(' ') === 'HF loop:2 VHF discone:2' && st.radioUnknown === 1, `stations.json radios (${JSON.stringify(st.radios)})`);
  const stB = await fetch(`${H}/vibeserver/stations.json?window=24h&radio=${B}`).then((r) => r.json());
  ok(stB.stations.map((s) => s.call).sort().join(' ') === 'G4BOTH M0BONE OH2OLD', `stations.json radio=VHF (${stB.stations.map((s) => s.call).join(' ')})`);

  console.log('── the web page: the gallery\'s radio chips ──');
  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1100,700', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => Promise.race([new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); }), sleep(15000).then(() => ({}))]);
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await send('Page.enable'); await send('Runtime.enable');
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn').onclick`)); i++) await sleep(250);
  const state = () => ev(`JSON.stringify({ rows: document.querySelectorAll('#galList .sres[role=button]:not(.galDigi):not(.galNav)').length,
    chips: document.getElementById('galRadio').hidden ? null : [...document.querySelectorAll('#galRadio button')].map(b => b.textContent + (b.classList.contains('on') ? '*' : '')).join(' | '),
    titles: [...document.querySelectorAll('#galList .sres[role=button]:not(.galDigi):not(.galNav) .n')].map(n => n.firstChild?.textContent).join(','),
    srcs: [...document.querySelectorAll('#galList .sres .src')].map(n => n.textContent).join(' || ') })`).then(JSON.parse);
  const until = async (f, ms = 6000) => { for (let t = 0; t < ms; t += 150) { const s = await state(); if (f(s)) return s; await sleep(150); } return state(); };
  await ev(`document.getElementById('galleryBtn').click()`);
  let s = await until((x) => x.rows === 15 && x.chips);
  ok(s.chips === 'ALL RADIOS* | HF loop · 10 | VHF discone · 5', `two radios: the chip row (${s.chips})`);
  ok(/Heard by HF loop and VHF discone/.test(s.srcs), 'the shared log says who heard it');
  await ev(`document.querySelector('#galRadio [data-radio="${B}"]').click()`);
  s = await until((x) => x.rows === 6);
  ok(s.rows === 6 && !/navtex A/.test(s.titles) && /navtex OLD/.test(s.titles) && /funcube day both/.test(s.titles), `VHF discone: 6 rows, none of HF's (${s.titles})`);
  ok(/VHF discone · 5\*/.test(s.chips || ''), 'its chip is on');
  await ev(`document.querySelector('#galFilter [data-kind="ft8"]').click()`);
  s = await until((x) => x.chips === null && x.rows === 3);
  ok(s.chips === null && s.rows === 3, `★ FT8 (only HF wrote it): no radio row, and the pick fell back to ALL (${s.rows} rows)`);
  await ev(`document.querySelector('#galFilter [data-kind=""]').click()`);
  s = await until((x) => x.rows === 15);
  ok(s.rows === 15 && /ALL RADIOS\*/.test(s.chips || ''), 'ALL again: every item, ALL RADIOS on');

  console.log('── the DIGI MODES MAP window: ALL RADIOS / each radio ──');
  for (let i = 0; i < 30 && !(await ev(`!!document.getElementById('galDigiMap')`)); i++) await sleep(200);
  const before = new Set((await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).map((t) => t.id));
  await send('Runtime.evaluate', { expression: `document.getElementById('galDigiMap').click()`, userGesture: true });
  let mapT = null;
  for (let i = 0; i < 40 && !mapT; i++) { await sleep(250); mapT = (await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json())).find((t) => t.type === 'page' && !before.has(t.id)); }
  ok(!!mapT, 'the map window opened');
  if (mapT) {
    const mws = new WebSocket(mapT.webSocketDebuggerUrl);
    let mid = 0; const mp = new Map();
    mws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && mp.has(m.id)) { mp.get(m.id)(m); mp.delete(m.id); } };
    await Promise.race([new Promise((r) => { mws.onopen = r; }), sleep(5000)]);
    const msend = (method, params = {}) => Promise.race([new Promise((res) => { const i = ++mid; mp.set(i, res); mws.send(JSON.stringify({ id: i, method, params })); }), sleep(10000).then(() => ({}))]);
    const mev = async (x) => (await msend('Runtime.evaluate', { expression: x, returnByValue: true })).result?.result?.value;
    const mstate = () => mev(`JSON.stringify({ rads: document.getElementById('rads').hidden ? null : [...document.querySelectorAll('#rads button')].map(b => b.textContent + (b.getAttribute('aria-pressed') === 'true' ? '*' : '')).join(' | '),
      calls: (typeof spots === 'undefined' ? [] : spots.map(s => s.callsign)).sort().join(' ') })`).then((v) => JSON.parse(v || '{}'));
    let m = {};
    for (let i = 0; i < 40; i++) { m = await mstate(); if (m.rads) break; await sleep(250); }
    ok(m.rads === 'ALL RADIOS* | HF loop · 2 | VHF discone · 2', `the radio keys (${m.rads})`);
    ok(m.calls === 'G4AONE G4BOTH M0BONE OH2OLD', `ALL: every station (${m.calls})`);
    await mev(`document.querySelector('#rads [data-radio="${A}"]').click()`);
    for (let i = 0; i < 20; i++) { m = await mstate(); if (!/M0BONE/.test(m.calls)) break; await sleep(150); }
    ok(m.calls === 'G4AONE G4BOTH OH2OLD' && /HF loop · 2\*/.test(m.rads || ''), `HF loop: its two + the old one (${m.calls})`);
    await mev(`document.querySelector('#rads [data-radio=""]').click()`);
    for (let i = 0; i < 20; i++) { m = await mstate(); if (/M0BONE/.test(m.calls)) break; await sleep(150); }
    ok(m.calls === 'G4AONE G4BOTH M0BONE OH2OLD', 'ALL RADIOS again: every station');
    try { mws.close(); } catch {}
  }
  ok(errs.length === 0, `no exception in the page${errs.length ? ': ' + errs[0] : ''}`);
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  clearTimeout(deadline);
  try { edge && edge.kill(); } catch {} try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {}
  await sleep(600);
  try { fs.rmSync(prof, { recursive: true, force: true }); } catch {}
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
