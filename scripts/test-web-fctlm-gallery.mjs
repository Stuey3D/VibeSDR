// test-web-fctlm-gallery.mjs — ★★ V12 beta91 TELEMETRY CHARTS in the GALLERY, through a real vibeserver and the REAL built
// web page. The gallery holds what vibe_fctlm_gallery.h writes for three AO-73 passes and their day log
// (scripts/fixtures/fctlm/ — the server's own writer on SYNTHETIC readings; nothing here was heard on air):
//   · the server serves a pass .csv as text/csv, its download name from the index ("dn");
//   · the FUNCUBE filter lists the passes and the day log;
//   · a pass opens as its CHARTS (not a wall of CSV): this pass, and the passes up to it from the gallery; + MORE; DOWNLOAD
//     still saves the CSV;
//   · the day log opens as text; BACK and another pass work; desktop and 390 px, no sideways scroll.
// ★ SILENT: headless Edge with --mute-audio, the fake dongle. usage: VIBESERVER_BIN=… node scripts/test-web-fctlm-gallery.mjs
//   Screenshots to $SAT_SHOTS (default $TMPDIR).
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
const SHOTS = process.env.SAT_SHOTS || os.tmpdir();
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
if (!fs.existsSync(EDGE)) { console.log('   (not run — no Edge for the headless page)'); process.exit(3); }

const FIX = path.join(root, 'scripts/fixtures/fctlm');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-fcgal-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'FUNcube gallery test', adminPass: 'fg-admin',
  radios: [{ serial: 'fcg', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000 }] }));
const gdir = path.join(dir, 'data', 'gallery');
fs.mkdirSync(gdir);
// the index lines exactly as vibe_fctlm_gallery.h's passMeta / dayMeta make them (titles, dn)
const firstUnix = (csv) => Number(csv.split('\n').find((l) => /^\d{4}-/.test(l)).split(',')[1]);
const hm = (t) => new Date(t * 1000).toISOString().slice(11, 16);
let index = '';
const items = [];
for (const n of [1, 2, 3]) {
  const body = fs.readFileSync(path.join(FIX, `pass-${n}.csv`), 'utf8');
  const t = firstUnix(body), frames = body.split('\n').filter((l) => /^\d{4}-/.test(l)).length;
  const f = `${new Date(t * 1000).toISOString().replace(/[-:]/g, '').slice(0, 15).replace('T', '-')}-fctlm-145935k0-${(0xa00 + n).toString(16)}.csv`;
  fs.writeFileSync(path.join(gdir, f), body);
  // ★ pass 2 was a LISTENER's pick (another origin, no profile): the history is still one series (Stuart 11-22)
  const it = { f, k: 'fctlm', o: n === 2 ? 'live' : 'background', t, hz: 145935000, m: 'usb', d: 'fctlm', p: n === 2 ? '' : 'Profile 1', r: 'Test radio',
               title: `AO-73 telemetry · pass ${hm(t)} UTC · ${frames} frames · max el 45°`, dn: `AO-73 telemetry ${new Date(t * 1000).toISOString().slice(0, 10)} ${hm(t).replace(':', '')} UTC.csv` };
  items.push(it); index += JSON.stringify(it) + '\n';
}
{
  const body = fs.readFileSync(path.join(FIX, 'day.txt'), 'utf8');
  const t = Math.floor(items[0].t / 86400) * 86400;
  const it = { f: '20261122-000000-fctlm-145935k0-0d01.txt', k: 'fctlm', o: 'background', t, hz: 145935000, m: 'usb', d: 'fctlm', p: 'Profile 1',
               r: 'Test radio', title: 'AO-73 telemetry log · 22 Nov · 66 frames', s: 'day-20261122-AO-73', dn: 'AO-73 telemetry 2026-11-22.txt' };
  fs.writeFileSync(path.join(gdir, it.f), body);
  index += JSON.stringify(it) + '\n';
}
fs.writeFileSync(path.join(gdir, 'index.jsonl'), index);

const rtlPort = await freePort(), port = await freePort(), cdp = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--users', '4'],
                  { env, stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
const prof = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-fcgal-edge-'));
let edge = null;
const deadline = setTimeout(() => { console.log('   FAIL deadline (120 s)'); try { edge?.kill('SIGKILL'); srv.kill('SIGKILL'); rtl.kill('SIGKILL'); } catch {} process.exit(1); }, 120000);
try {
  for (let i = 0; i < 100; i++) {
    if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) break;
    await sleep(100);
  }
  const H = `http://127.0.0.1:${port}`;
  console.log('── the server: the pass CSV served ──');
  const r = await fetch(`${H}/vibeserver/gallery/file/${encodeURIComponent(items[0].f)}`);
  const txt = await r.text();
  ok(r.ok && /^text\/csv/.test(r.headers.get('content-type') || ''), `a pass .csv is served as text/csv (${r.headers.get('content-type')})`);
  ok(txt.startsWith('# FUNcube telemetry · AO-73') && txt === fs.readFileSync(path.join(FIX, 'pass-1.csv'), 'utf8'), 'byte for byte the file the server wrote');
  const rd = await fetch(`${H}/vibeserver/gallery/file/${encodeURIComponent(items[0].f)}?download=1`);
  ok(/AO-73 telemetry 2026-11-22 1402 UTC\.csv/.test(rd.headers.get('content-disposition') || ''), `DOWNLOAD names it from the index (${rd.headers.get('content-disposition')})`);
  const gl = await fetch(`${H}/vibeserver/gallery.json?kind=fctlm&limit=300`).then((x) => x.json());
  ok(gl.items?.length === 4, `gallery.json?kind=fctlm: 3 passes + the day log (${gl.items?.length})`);

  console.log('── the web page: FUNCUBE, a pass as its charts ──');
  edge = spawn(EDGE, ['--headless=new', `--remote-debugging-port=${cdp}`, '--no-first-run', '--no-default-browser-check', '--mute-audio',
    '--disable-audio-output', `--user-data-dir=${prof}`, '--window-size=1100,800', 'about:blank'], { stdio: 'ignore' });
  let page = null;
  for (let i = 0; i < 50 && !page; i++) { await sleep(200); page = await fetch(`http://127.0.0.1:${cdp}/json/list`).then((x) => x.json()).then((l) => l.find((t) => t.type === 'page')).catch(() => null); }
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  let id = 0; const pend = new Map(); const errs = [];
  ws.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
                          if (m.method === 'Runtime.exceptionThrown') errs.push(m.params.exceptionDetails?.exception?.description || 'exception'); };
  await new Promise((res) => { ws.onopen = res; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => (await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true })).result?.result?.value;
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, `fctlm-gallery-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
  const until = async (x, ms = 6000) => { for (let t = 0; t < ms; t += 150) { if (await ev(x)) return true; await sleep(150); } return false; };
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 1100, height: 800, deviceScaleFactor: 1, mobile: false });
  await send('Page.navigate', { url: `${H}/index.html?join` });
  await sleep(4000);
  await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()) && x.offsetParent); if (b) b.click(); return !!b; })()`);
  for (let i = 0; i < 40 && !(await ev(`!!document.getElementById('galleryBtn').onclick`)); i++) await sleep(250);
  // ★ the screenshots show the gallery, not the connect screen over it (no audio is started: tests are silent)
  await ev(`(() => { for (const id of ['splash', 'audioGate']) { const s = document.getElementById(id); if (s) s.style.display = 'none'; } return true; })()`);
  await ev(`document.getElementById('galleryBtn').click(); true`);
  ok(await until(`!!document.querySelector('#galFilter [data-kind="fctlm"]')`), 'a FUNCUBE filter key');
  await ev(`document.querySelector('#galFilter [data-kind="fctlm"]').click(); true`);
  ok(await until(`document.querySelectorAll('#galList .sres[role=button]').length === 4`), 'FUNCUBE: the passes and the day log');
  // open the MIDDLE pass (15:42): its charts, and the passes up to it (2)
  await ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /pass 15:42/.test(r.textContent)).click(); true`);
  ok(await until(`!!document.getElementById('galCharts') && !document.getElementById('galCharts').hidden && /LAST 2 PASSES/.test(document.getElementById('galCharts').textContent)`, 8000),
     'a pass opens as its charts, with the passes up to it from the gallery (LAST 2 PASSES — a background one and a listener\'s)');
  const st = JSON.parse(await ev(`JSON.stringify({ text: document.getElementById('galText').hidden, n: document.querySelectorAll('#galCharts .tlm-c').length,
    head: document.querySelector('#galCharts .tlm-h')?.textContent, dl: document.getElementById('galDl').getAttribute('download'),
    lines: [...document.querySelectorAll('#galCharts path.tlm-l')].map(p => (p.getAttribute('d').match(/M/g) || []).length) })`));
  ok(st.text === true && st.n === 16, `the CSV text hidden, 8 + 8 charts (${st.n})`);
  ok(st.head === 'THE PASS · 18 frames · 15:42–15:44 UTC', `the pass's head (${st.head})`);
  ok(st.lines[0] === 2, `the fade in this pass breaks its line: two runs (${st.lines[0]})`);
  ok(st.dl === items[1].dn, `DOWNLOAD saves the CSV under its name (${st.dl})`);
  await shot('pass-desktop');
  await ev(`document.querySelector('#galCharts [data-act="tlmall"]').click(); true`);
  ok(await ev(`document.querySelectorAll('#galCharts .tlm-c').length`) > 16, '+ MORE CHANNELS: every analogue channel');
  await ev(`document.getElementById('galBack').click(); true`);
  await until(`document.getElementById('galView').hidden`);
  await ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /telemetry log/.test(r.textContent)).click(); true`);
  ok(await until(`!document.getElementById('galText').hidden && /14:02:30  AO-73  WO1/.test(document.getElementById('galText').textContent) && document.getElementById('galCharts').hidden`),
     'the day log opens as text (no charts)');
  await ev(`document.getElementById('galBack').click(); true`);
  await until(`document.getElementById('galView').hidden`);
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true }); await sleep(400);
  await ev(`[...document.querySelectorAll('#galList .sres[role=button]')].find(r => /pass 17:22/.test(r.textContent)).click(); true`);
  ok(await until(`/LAST 3 PASSES/.test(document.getElementById('galCharts').textContent)`, 8000), 'the newest pass: LAST 3 PASSES');
  const fit = JSON.parse(await ev(`JSON.stringify({ sw: document.documentElement.scrollWidth, w: innerWidth, cw: document.getElementById('galCharts').scrollWidth, cc: document.getElementById('galCharts').clientWidth })`));
  ok(fit.sw <= fit.w + 1 && fit.cw <= fit.cc + 1, `390 px: no sideways scroll (${JSON.stringify(fit)})`);
  await shot('pass-phone');
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
console.log(`\n${pass} passed, ${fail} failed · screenshots: ${SHOTS}/fctlm-gallery-*.png`);
process.exit(fail ? 1 : 0);
