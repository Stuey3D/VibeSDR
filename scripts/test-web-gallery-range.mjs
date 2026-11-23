// test-web-gallery-range.mjs — THE GALLERY'S TIME RANGE, through a real vibeserver and the REAL built web page (V12 beta92,
//   Stuart 2026-11-23: "the gallery needs some sorting last hour last day last week last month buttons").
//   A gallery of NAVTEX items at now −30 min, −5 h, −3 d, −20 d, −60 d plus 30 WEFAX items 6–20 h old:
//   gallery.json?since= narrows the page and its total (count/kinds stay the whole store's), an old request is answered as
//   before (no "since"), since=0 says the server knows the filter; in the page the keys 1 H · 24 H · 7 DAYS · 30 DAYS · ALL
//   show the right items, totals and pages, reset to page 1, the footer names the range, the choice survives a reload
//   (localStorage), a key works from the keyboard, and at 390 px nothing scrolls sideways.
// ★ SILENT: headless Edge with --mute-audio, the fake dongle, nothing played. usage: VIBESERVER_BIN=… node scripts/test-web-gallery-range.mjs
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

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-galrange-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Gallery range test', adminPass: 'gr-admin',
  radios: [{ serial: 'galr', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000 }] }));
const gdir = path.join(dir, 'data', 'gallery');
fs.mkdirSync(gdir);
const now = Math.floor(Date.now() / 1000);
const rows = [];
const NAV = [['30min', 1800], ['5h', 5 * 3600], ['3d', 3 * 86400], ['20d', 20 * 86400], ['60d', 60 * 86400]];
for (const [tag, back] of NAV) rows.push({ kind: 'navtex', t: now - back, title: `navtex ${tag}` });
for (let i = 0; i < 30; i++) rows.push({ kind: 'wefax', t: now - 6 * 3600 - i * 1680, title: `wefax ${i}` });
rows.sort((a, b) => a.t - b.t);   // oldest first, as a gallery fills
let index = '';
rows.forEach((r, i) => {
  const f = `20261123-${String(100000 + i)}-${r.kind}-518k0-${(4096 + i).toString(16)}.txt`;
  fs.writeFileSync(path.join(gdir, f), `${r.title}\n`);
  index += JSON.stringify({ f, k: r.kind, o: 'background', t: r.t, hz: 518000, m: 'usb', d: r.kind, p: 'Profile 1', r: 'Test radio', title: r.title }) + '\n';
});
fs.writeFileSync(path.join(gdir, 'index.jsonl'), index);

const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-galrange-edge-'));
let edge = null;
try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  const J = (q) => fetch(`${H}/vibeserver/gallery.json?${q}`).then((r) => r.json());
  console.log('── gallery.json?since= ──');
  const want = { 3600: [1, 0], 86400: [2, 30], [7 * 86400]: [3, 30], [30 * 86400]: [4, 30], 0: [5, 30] };
  for (const [since, [nav, wef]] of Object.entries(want)) {
    const all = await J(`offset=0&limit=100&since=${since}`);
    const n = await J(`offset=0&limit=100&kind=navtex&since=${since}`);
    const w = await J(`offset=0&limit=100&kind=wefax&since=${since}`);
    ok(all.total === nav + wef && all.items.length === nav + wef && n.total === nav && w.total === wef && all.since === Number(since),
       `since=${since}: ${nav} NAVTEX + ${wef} WEFAX (${all.total}; "since" ${all.since})`);
    ok(all.count === 35 && all.kinds?.navtex?.n === 5 && all.kinds?.wefax?.n === 30, `  …"count" and "kinds" stay the whole store's (${all.count})`);
  }
  const p2 = await J('offset=25&limit=25&since=86400');
  ok(p2.total === 32 && p2.items.length === 7, `a page in a range: 24 h page 2 holds 7 of 32 (${p2.items.length})`);
  const n1 = await J('offset=0&limit=25&kind=navtex&since=3600');
  ok(n1.items.length === 1 && n1.items[0].title === 'navtex 30min', 'the last hour: the 30-minute-old message only');
  const old = await J('limit=300');
  ok(old.items.length === 35 && old.total === undefined && old.since === undefined, '★ an old client (no since, no offset): every item, the old shape');
  const oldPaged = await J('offset=0&limit=25');
  ok(oldPaged.total === 35 && oldPaged.since === undefined, '★ a paged client that does not know since=: unchanged, no "since"');
  ok((await J('offset=0&limit=25&since=999999999')).total === 35, 'past 400 days = all');

  console.log('── the web page ──');
  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1100,700', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await new Promise((r) => { ws.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await send('Page.enable'); await send('Runtime.enable');
  const openGallery = async () => {
    await send('Page.navigate', { url: `${H}/index.html?join` });
    await sleep(4000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
    for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn').onclick`)); i++) await sleep(250);
    await ev(`document.getElementById('galleryBtn').click()`);
  };
  const state = () => ev(`JSON.stringify({ rows: document.querySelectorAll('#galList .sres[role=button]:not(.galNav):not(.galDigi):not(.galDsc)').length,
    titles: [...document.querySelectorAll('#galList .sres[role=button]:not(.galNav) .n')].map(n => n.firstChild?.textContent || ''),
    lab: document.querySelector('#galPager .lab')?.textContent || '', pager: !document.getElementById('galPager').hidden,
    note: document.getElementById('galNote').textContent, empty: document.querySelector('#galList .sres:not([role]) .n')?.textContent || '',
    shown: !document.getElementById('galRange').hidden,
    keys: [...document.querySelectorAll('#galRange button')].map(b => b.textContent + (b.getAttribute('aria-pressed') === 'true' ? '*' : '')).join(' ') })`).then(JSON.parse);
  const until = async (f, ms = 6000) => { for (let t = 0; t < ms; t += 150) { const s = await state(); if (f(s)) return s; await sleep(150); } return state(); };
  const key = (label) => ev(`[...document.querySelectorAll('#galRange button')].find(b => b.textContent === ${JSON.stringify(label)}).click()`);

  await openGallery();
  let s = await until((x) => x.shown && x.rows === 25);
  ok(s.shown && s.keys === '1 H 24 H 7 DAYS 30 DAYS ALL*', `the keys, ALL by default: "${s.keys}"`);
  ok(s.lab === 'Page 1 of 2' && /^35 items on this server/.test(s.note), `ALL: 35 items, 2 pages ("${s.lab}", "${s.note}")`);
  await key('1 H');
  s = await until((x) => x.rows === 1);
  ok(s.rows === 1 && s.titles.includes('navtex 30min') && !s.pager, `1 H: one item, no pager (${s.titles.join('|')})`);
  ok(/^1 item in the last hour — 35 items on this server/.test(s.note), `the footer names the range: "${s.note}"`);
  ok(s.keys === '1 H* 24 H 7 DAYS 30 DAYS ALL', 'the key is lit');
  // page 2 of 24 h, then a range change → page 1
  await key('24 H');
  s = await until((x) => x.lab === 'Page 1 of 2' && x.rows === 25);
  ok(s.lab === 'Page 1 of 2' && /^32 items in the last 24 hours/.test(s.note), `24 H: 32 items, 2 pages ("${s.note}")`);
  await ev(`[...document.querySelectorAll('#galPager button')].find(b => b.textContent === '2').click()`);
  s = await until((x) => x.lab === 'Page 2 of 2');
  ok(s.rows === 7, `24 H page 2: 7 rows (${s.rows})`);
  await key('7 DAYS');
  s = await until((x) => x.lab === 'Page 1 of 2' && x.rows === 25);
  ok(s.lab === 'Page 1 of 2' && /^33 items in the last 7 days/.test(s.note), `★ a range change → page 1 ("${s.lab}", "${s.note}")`);
  await key('30 DAYS');
  s = await until((x) => /^34 items/.test(x.note));
  ok(/^34 items in the last 30 days/.test(s.note), `30 DAYS: 34 ("${s.note}")`);
  // with a kind
  await ev(`document.querySelector('#galFilter [data-kind="navtex"]').click()`);
  s = await until((x) => x.rows === 4);
  ok(s.rows === 4 && /^4 NAVTEX items in the last 30 days — 35 items on this server/.test(s.note), `NAVTEX in 30 DAYS: 4 ("${s.note}")`);
  await key('1 H');
  s = await until((x) => x.rows === 1);
  ok(s.rows === 1 && /^1 NAVTEX item in the last hour/.test(s.note), `NAVTEX in 1 H: 1 ("${s.note}")`);
  await ev(`document.querySelector('#galFilter [data-kind="wefax"]').click()`);
  s = await until((x) => x.rows === 0 && /Nothing/.test(x.empty));
  ok(s.rows === 0 && /Nothing from this decoder kept in the last hour/.test(s.empty), `WEFAX in 1 H: nothing, said so ("${s.empty}")`);
  // keyboard: focus a key, Enter
  await ev(`[...document.querySelectorAll('#galRange button')].find(b => b.textContent === '7 DAYS').focus()`);
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13, text: '\r' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13 });
  s = await until((x) => x.rows === 25 && /WEFAX items in the last 7 days/.test(x.note));
  ok(/^30 WEFAX items in the last 7 days/.test(s.note) && s.keys.includes('7 DAYS*'), `★ a focused key works from the keyboard ("${s.note}")`);
  // opening an item hides the range keys; BACK brings them back
  await ev(`document.querySelectorAll('#galList .sres[role=button]')[0].click()`);
  await sleep(600);
  ok(await ev(`document.getElementById('galRange').hidden && !document.getElementById('galView').hidden`), 'an open item hides the keys');
  await ev(`document.getElementById('galBack').click()`);
  s = await until((x) => x.shown && x.rows === 25);
  ok(s.shown, 'BACK: the keys are back');
  // remembered across a reload
  await openGallery();
  s = await until((x) => x.shown && x.keys.includes('*') && x.rows > 0);
  ok(s.keys === '1 H 24 H 7 DAYS* 30 DAYS ALL' && /^33 items in the last 7 days/.test(s.note), `★ remembered after a reload ("${s.keys}", "${s.note}")`);
  // phone width: no sideways scroll
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 800, deviceScaleFactor: 2, mobile: true });
  await sleep(800);
  const w = JSON.parse(await ev(`JSON.stringify({ doc: document.documentElement.scrollWidth, panel: document.getElementById('galleryPanel').scrollWidth,
    pcw: document.getElementById('galleryPanel').clientWidth, row: document.getElementById('galRange').scrollWidth, rcw: document.getElementById('galRange').clientWidth,
    right: Math.max(...[...document.querySelectorAll('#galRange button')].map(b => b.getBoundingClientRect().right)) })`));
  ok(w.doc <= 390 && w.panel <= w.pcw && w.row <= w.rcw && w.right <= 390, `390 px: no sideways scroll (${JSON.stringify(w)})`);
  await key('ALL');
  s = await until((x) => /^35 items on this server/.test(x.note));
  ok(s.keys === '1 H 24 H 7 DAYS 30 DAYS ALL*', 'ALL again: every item');
  ok(errs.length === 0, `no exception in the page${errs.length ? ': ' + errs[0] : ''}`);
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  try { edge && edge.kill(); } catch {} try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {}
  await sleep(600);
  try { fs.rmSync(prof, { recursive: true, force: true }); } catch {}
  try { fs.rmSync(dir, { recursive: true, force: true }); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
