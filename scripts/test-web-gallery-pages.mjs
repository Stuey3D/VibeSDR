// test-web-gallery-pages.mjs — THE GALLERY IN PAGES, through a real vibeserver and the REAL built web page (V12 beta 4).
//   A gallery of 60 items (40 NAVTEX, 20 FT8): gallery.json pages with offset/limit and the filter's total, and an old
//   request (no offset) is answered exactly as before; in the page, 25 a page with "Page 1 of 3", Older/Newer and the
//   numbered keys, the filter kept across pages and page 1 on a filter change, and BACK from an item lands on the same
//   page, the same scroll and the same row (focused — keyboard, D-pad and controller carry on from it).
// ★ SILENT: headless Edge with --mute-audio, the fake dongle, nothing played. usage: VIBESERVER_BIN=… node scripts/test-web-gallery-pages.mjs
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

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-galpages-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Gallery pages test', adminPass: 'gp-admin',
  radios: [{ serial: 'galp', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000 }] }));
const gdir = path.join(dir, 'data', 'gallery');
fs.mkdirSync(gdir);
let index = '';
for (let i = 0; i < 60; i++) {
  const ft8 = i % 3 === 0;
  const f = `20261022-${String(100000 + i)}-${ft8 ? 'ft8-1840k0' : 'navtex-518k0'}-${(1000 + i).toString(16)}.txt`;
  fs.writeFileSync(path.join(gdir, f), `item ${i}\n`);
  index += JSON.stringify({ f, k: ft8 ? 'ft8' : 'navtex', o: 'background', t: 1792700000 + i, hz: ft8 ? 1840000 : 518000, m: 'usb',
                            d: ft8 ? 'ft8' : 'navtex', p: 'Profile 1', r: 'Test radio', title: `item ${i}` }) + '\n';
}
fs.writeFileSync(path.join(gdir, 'index.jsonl'), index);

const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-galpages-edge-'));
let edge = null;
try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  console.log('── gallery.json: pages, totals per filter, the old request unchanged ──');
  const j1 = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=25`).then((r) => r.json());
  ok(j1.items.length === 25 && j1.total === 60 && j1.offset === 0 && j1.items[0].title === 'item 59', `page 1: 25, total 60, newest first (${j1.items[0]?.title})`);
  const j3 = await fetch(`${H}/vibeserver/gallery.json?offset=50&limit=25`).then((r) => r.json());
  ok(j3.items.length === 10 && j3.items[9].title === 'item 0', 'page 3: the last 10');
  const f8 = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=25&kind=ft8`).then((r) => r.json());
  ok(f8.total === 20 && f8.items.length === 20 && f8.items.every((x) => x.k === 'ft8'), `FT8: total 20 for the filter (${f8.total})`);
  const nv = await fetch(`${H}/vibeserver/gallery.json?offset=25&limit=25&kind=navtex`).then((r) => r.json());
  ok(nv.total === 40 && nv.items.length === 15, `NAVTEX page 2: 15 of 40 (${nv.items.length})`);
  const big = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=500`).then((r) => r.json());
  ok(big.items.length === 60 && big.total === 60, 'a page is capped at 100 (60 here, all of them)');
  const capped = await fetch(`${H}/vibeserver/gallery.json?offset=0&limit=1000`).then((r) => r.json());
  ok(capped.items.length <= 100, 'limit=1000 with offset → at most 100');
  const old = await fetch(`${H}/vibeserver/gallery.json?limit=300`).then((r) => r.json());
  ok(old.items.length === 60 && old.total === undefined && old.offset === undefined && old.count === 60, '★ an old client (no offset): every item, the old shape — no total, no offset');

  console.log('── the web page: 25 a page, the pager, the filter, BACK to the same page and scroll ──');
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
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  // ★ The splash may want START (a no-PIN server); the GALLERY key is wired once the page has joined (initDecoders).
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn').onclick`)); i++) await sleep(250);
  const state = () => ev(`JSON.stringify({ rows: document.querySelectorAll('#galList .sres[role=button]').length,
    first: document.querySelector('#galList .sres .n')?.firstChild?.textContent,
    lab: document.querySelector('#galPager .lab')?.textContent || '', pager: !document.getElementById('galPager').hidden,
    view: !document.getElementById('galView').hidden, scroll: document.getElementById('galleryPanel').scrollTop,
    focus: document.activeElement?.closest?.('#galList .sres') ? [...document.querySelectorAll('#galList .sres')].indexOf(document.activeElement) : -1,
    keys: [...document.querySelectorAll('#galPager button')].map(b => b.textContent + (b.classList.contains('on') ? '*' : '') + (b.disabled ? '-' : '')).join(' ') })`).then(JSON.parse);
  const until = async (f, ms = 6000) => { for (let t = 0; t < ms; t += 150) { const s = await state(); if (f(s)) return s; await sleep(150); } return state(); };
  await ev(`(() => { const b = document.getElementById('galleryBtn'); b && b.click(); return !!b; })()`);
  let s = await until((x) => x.rows === 25 && x.pager);
  ok(s.rows === 25 && s.first === 'item 59' && s.lab === 'Page 1 of 3', `opened: 25 rows, newest first, "${s.lab}"`);
  ok(/^‹ Newer- 1\* 2 3 Older ›$/.test(s.keys), `the keys: "${s.keys}" (Newer off on page 1)`);
  await ev(`[...document.querySelectorAll('#galPager button')].find(b => /Older/.test(b.textContent)).click()`);
  s = await until((x) => x.lab === 'Page 2 of 3' && x.first === 'item 34');
  ok(s.lab === 'Page 2 of 3' && s.first === 'item 34' && s.rows === 25, `Older ›: "${s.lab}", from item 34`);
  // scroll down, open the 11th row, come BACK
  await ev(`(() => { const p = document.getElementById('galleryPanel'); p.scrollTop = 0; const r = document.querySelectorAll('#galList .sres')[10]; p.scrollTop = r.offsetTop - 40; return p.scrollTop; })()`);
  const before = (await state()).scroll;
  await ev(`document.querySelectorAll('#galList .sres')[10].click()`);
  s = await until((x) => x.view);
  ok(s.view && !s.pager, 'an item opens (the pager is hidden under it)');
  await ev(`document.getElementById('galBack').click()`);
  s = await until((x) => !x.view && x.rows === 25);
  ok(s.lab === 'Page 2 of 3' && s.first === 'item 34', `★ BACK: the same page ("${s.lab}")`);
  ok(before > 0 && Math.abs(s.scroll - before) <= 2, `★ ...the same scroll (${before} → ${s.scroll})`);
  ok(s.focus === 10, `★ ...and the row that was opened has the focus (${s.focus})`);
  // keyboard: Enter on a focused row opens it
  await ev(`(() => { const r = document.querySelectorAll('#galList .sres')[3]; r.focus(); r.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); })()`);
  s = await until((x) => x.view);
  ok(s.view, 'Enter on a focused row opens it (keyboard / D-pad)');
  await ev(`document.getElementById('galBack').click()`);
  await until((x) => !x.view);
  // the filter: page 1, kept across pages
  await ev(`document.querySelector('#galFilter [data-kind="navtex"]').click()`);
  s = await until((x) => x.lab === 'Page 1 of 2');
  ok(s.lab === 'Page 1 of 2' && s.first === 'item 59', `★ a filter change → page 1 of ITS pages ("${s.lab}", ${s.first})`);
  await ev(`[...document.querySelectorAll('#galPager button')].find(b => b.textContent === '2').click()`);
  s = await until((x) => x.lab === 'Page 2 of 2');
  ok(s.rows === 15 && s.lab === 'Page 2 of 2', `the filter kept across pages: NAVTEX page 2, 15 rows (${s.rows})`);
  ok(/Older ›-$/.test(s.keys.split(' ').slice(-2).join(' ')), `Older off on the last page ("${s.keys}")`);
  await ev(`document.querySelector('#galFilter [data-kind="ft8"]').click()`);
  s = await until((x) => x.rows === 20);
  ok(s.rows === 20 && !s.pager, '★ FT8: 20 items, one page — no pager');
  ok(errs.length === 0, `no exception in the page${errs.length ? ': ' + errs[0] : ''}`);
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  try { edge && edge.kill(); } catch {} try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {}
  await sleep(600);
  try { fs.rmSync(prof, { recursive: true, force: true }); } catch {}
}
console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
