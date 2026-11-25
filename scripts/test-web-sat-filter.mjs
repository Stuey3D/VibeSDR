// test-web-sat-filter.mjs — ★★ beta94 SHOW on the web's SATELLITE window (Stuart 11-25: "on the satellite list can we have a
// dropdown to show only passes from certain satellites"), against a FAKE sat_list inside the BUILT page (web/dist — the real
// CSS and the control looks), headless Edge with --mute-audio (tests are silent; no server, no network):
//   no drop-down while the list is empty; "SHOW: All" by default with every pass listed; unticking a satellite hides its
//   passes; a group shortcut (Weather) shows only its birds; a tick adds one ("2 satellites"); it is kept in localStorage
//   by NORAD number and is the same after a reload; the satellite picked stays listed when SHOW leaves it out; a reminder
//   for a satellite SHOW hides does not fire, one it shows does; default / silver / black, desktop and 390 px: legible,
//   nothing off the side. Needs web/dist (node scripts/build-web.mjs). Screenshots to $SAT_SHOTS (default $TMPDIR).
//     node scripts/test-web-sat-filter.mjs
import * as esbuild from 'esbuild';
import http from 'node:http';
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
const DIST = path.join(root, 'web/dist');
if (!fs.existsSync(EDGE) || !fs.existsSync(path.join(DIST, 'vibesdr.html'))) { console.log('   not run — Edge and web/dist needed'); process.exit(3); }
const SHOTS = process.env.SAT_SHOTS || os.tmpdir();
fs.mkdirSync(SHOTS, { recursive: true });
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const bundle = (await esbuild.build({ stdin: { contents: "export { SatPanel } from './web/client/src/satellites.ts'; export { parseSatList } from './src/services/satellites.ts'; export { applyControlLook, readLook } from './web/client/src/controlColours.ts';", resolveDir: root, loader: 'ts' },
  bundle: true, write: false, format: 'iife', globalName: '__Sat', platform: 'browser', target: 'es2020', logLevel: 'silent' })).outputFiles[0].text;

const port = await freePort();
const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.webp': 'image/webp', '.png': 'image/png', '.svg': 'image/svg+xml', '.json': 'application/json' };
const srv = http.createServer((q, r) => {
  const u = decodeURIComponent((q.url || '/').split('?')[0]);
  const f = u === '/' ? path.join(DIST, 'vibesdr.html') : path.join(DIST, path.normalize(u).replace(/^(\.\.[/\\])+/, ''));
  if (!f.startsWith(DIST) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { r.writeHead(404); r.end(); return; }
  r.writeHead(200, { 'content-type': types[path.extname(f)] || 'application/octet-stream' }); fs.createReadStream(f).pipe(r);
}).listen(port, '127.0.0.1');

const now = Math.floor(Date.now() / 1000);
const pas = (inMin, maxEl = 40) => [{ aos: now + inMin * 60, tca: now + inMin * 60 + 300, los: now + inMin * 60 + 600, maxEl, aosAz: 0, losAz: 180 },
                                    { aos: now + (inMin + 100) * 60, tca: now + (inMin + 105) * 60, los: now + (inMin + 110) * 60, maxEl: 20, aosAz: 0, losAz: 180 }];
const SO50_AOS = now + 38 * 60, SON2_AOS = now + 70 * 60;
const list = (sats) => ({
  type: 'sat_list', time: 'ok', located: true, now,
  source: { orbits: 'Orbits: CelesTrak (celestrak.org) GP data', via: 'vibesdr.net', fetched: now - 7200, synced: now - 7200, fetching: false, pickPrivate: true },
  sats: sats ?? [
    { id: 'METEOR-M2-3', name: 'METEOR-M2 3', norad: 57166, note: '', age: 0.4, ageState: 'fresh', el: -20, passes: pas(20, 62),
      tx: [{ label: 'LRPT', hz: 137900000, mode: 'lrpt', loop: 'costas', consumer: 'lrpt', reach: true, use: 'image', decoder: 'lrpt' }] },
    { id: 'ISS', name: 'ISS', norad: 25544, note: '', age: 0.2, ageState: 'fresh', el: -10, passes: pas(25),
      tx: [{ label: 'FM repeater', hz: 437800000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice', upHz: 145990000, ctcss: 67 },
           { label: 'APRS', hz: 145825000, mode: 'nfm', loop: 'fll', consumer: 'ax25-1200', reach: true, use: 'packet', decoder: 'aprs' }] },
    { id: 'RS-44', name: 'RS-44', norad: 44909, note: '', age: 1, ageState: 'fresh', el: -30, passes: pas(30),
      tx: [{ label: 'Transponder', hz: 435610000, hiHz: 435670000, mode: 'usb', loop: 'none', reach: true, use: 'voice', decoder: 'xpdr' },
           { label: 'CW beacon', hz: 435605000, mode: 'cw', loop: 'none', reach: true, use: 'voice', decoder: 'xpdr' }] },
    { id: 'SO-50', name: 'SO-50 (SaudiSat-1C)', norad: 27607, note: '', age: 1, ageState: 'fresh', el: -9, passes: [{ aos: SO50_AOS, tca: SO50_AOS + 300, los: SO50_AOS + 600, maxEl: 56, aosAz: 10, losAz: 170 }],
      tx: [{ label: 'FM repeater', hz: 436795000, mode: 'nfm', loop: 'fll', reach: true, use: 'voice' }] },
    { id: 'SONATE-2', name: 'SONATE-2', norad: 59112, note: '', age: 1, ageState: 'fresh', el: -30, passes: [{ aos: SON2_AOS, tca: SON2_AOS + 300, los: SON2_AOS + 600, maxEl: 22, aosAz: 10, losAz: 170 }],
      tx: [{ label: 'APRS', hz: 145825000, mode: 'nfm', loop: 'fll', consumer: 'ax25-1200', reach: true, use: 'packet', decoder: 'aprs' }] },
  ],
});

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-satshow-'));
const dbg = await freePort();
const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
  `--user-data-dir=${dir}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
const deadline = setTimeout(() => { console.log('   FAIL deadline (150 s)'); try { edge.kill('SIGKILL'); } catch {} process.exit(1); }, 150000);
let wsC;
const errors = [];
try {
  let tabs = [];
  for (let i = 0; i < 40 && !tabs.some((t) => t.type === 'page'); i++) { await sleep(250); try { tabs = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json(); } catch {} }
  wsC = new WebSocket(tabs.find((t) => t.type === 'page').webSocketDebuggerUrl);
  let id = 0; const pend = new Map();
  wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); }
    if (m.method === 'Runtime.exceptionThrown') errors.push(m.params?.exceptionDetails?.exception?.description || m.params?.exceptionDetails?.text); };
  await new Promise((r) => { wsC.onopen = r; });
  const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
  const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.exception?.description ?? r.result?.exceptionDetails?.text; };
  const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); fs.writeFileSync(path.join(SHOTS, `sat-show-${name}.png`), Buffer.from(s.result.data, 'base64')); };
  const until = async (x, ms) => { for (let i = 0; i < ms / 200; i++) { if (await ev(x)) return true; await sleep(200); } return false; };
  const desk = () => send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  const phone = () => send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await send('Page.enable'); await send('Runtime.enable');
  await desk();
  await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: 'dark' }] });

  /** the built page (re)loaded, and a fresh SATELLITE window in it — as a reload builds it */
  const boot = async () => {
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await sleep(300);
    if (!await until(`document.readyState === 'complete' && !!document.getElementById('decImgWrap')`, 15000)) throw new Error('the page did not load');
    await sleep(600);
    await ev(`(() => { const s = document.getElementById('splash'); if (s) s.style.display = 'none'; return true; })()`);
    await ev(`(() => { ${bundle}; window.__Sat = __Sat; return true; })()`);
    await ev(`(() => {
      window.__sent = []; window.__fake = null;
      const home = document.getElementById('decImgWrap').parentNode;
      window.__p = new __Sat.SatPanel({
        list: () => {}, pick: (norad, tx, doppler) => { window.__sent.push({ op: 'pick', norad, tx }); return true; },
        doppler: () => {}, drop: () => {}, rxPos: () => ({ lat: 52.24, lon: -0.9 }), follow: () => {}, left: () => {},
        image: (slot) => { const w = document.getElementById('decImgWrap'); if (slot) slot.appendChild(w); else home.appendChild(w); },
        track: () => {}, serverKey: () => 'test-server', connected: () => true, now: () => window.__fake ?? Date.now(),
      });
      window.__p.setCap({ ok: true, why: '' });
      window.__p.show();
      return true; })()`);
  };
  const setList = (sats) => ev(`window.__p.onList(__Sat.parseSatList(${JSON.stringify(list(sats))})); document.querySelector('#satPanel details.sat-list').open = true; true`);
  const state = async () => JSON.parse(await ev(`JSON.stringify({
    hidden: document.querySelector('#satPanel details.sat-show').hidden,
    sum: document.querySelector('#satPanel details.sat-show .sat-shv').textContent,
    names: [...new Set([...document.querySelectorAll('#satPanel .sat-row .sat-n')].map((e) => e.textContent.split(' · ')[0]))],
    rows: document.querySelectorAll('#satPanel .sat-row').length,
    ticks: [...document.querySelectorAll('#satPanel .sat-sht button')].map((b) => b.textContent + '=' + b.getAttribute('aria-checked')),
    groups: [...document.querySelectorAll('#satPanel .sat-shg button')].map((b) => b.textContent + (b.classList.contains('on') ? '*' : '')),
    stored: (() => { try { return localStorage.getItem('vibe.satShow'); } catch { return 'threw'; } })(),
  })`));
  const tick = (norad) => ev(`document.querySelector('#satPanel .sat-sht button[data-norad="${norad}"]').click(); true`);
  const group = (g) => ev(`document.querySelector('#satPanel .sat-shg button[data-g="${g}"]').click(); true`);

  await boot();
  await ev(`(() => { try { localStorage.removeItem('vibe.satShow'); } catch {} return true; })()`);
  await boot();

  console.log('— the drop-down: none while the list is empty; All by default');
  await setList([]);
  let s = await state();
  ok(s.hidden === true, 'an empty list: no SHOW drop-down at all');
  await setList();
  s = await state();
  ok(s.hidden === false && s.sum === 'SHOW: All', `the list arrives: "${s.sum}"`);
  ok(s.rows === 12 && s.names.length === 5, `every pass listed (${s.rows} rows, ${s.names.join(' / ')})`);
  ok(s.ticks.every((t) => t.endsWith('=true')) && s.ticks.length === 5, `a tick box per satellite, all ticked: ${s.ticks.join(', ')}`);
  ok(JSON.stringify(s.groups) === JSON.stringify(['All*', 'Weather', 'Transponders', 'FM voice', 'Telemetry']), `the shortcuts, All lit: ${s.groups.join(' ')}`);
  ok(s.stored === null, 'nothing stored until it is touched');
  ok(await ev(`document.querySelector('#satPanel details.sat-show').open`) === false, 'it opens closed (the list is what you see)');
  ok(await ev(`(() => { const a = getComputedStyle(document.querySelector('#satPanel details.sat-show summary')).fontFamily, b = getComputedStyle(document.querySelector('#satPanel .sat-row')).fontFamily; return a === b; })()`),
     'the drop-down keeps the menu font (the rows\' own, not the display font)');
  await ev(`document.querySelector('#satPanel details.sat-show summary').click(); true`);
  await sleep(150);
  ok(await ev(`document.querySelector('#satPanel details.sat-show').open`) === true, 'a tap on "SHOW: All" opens it');
  await shot('open-desktop');

  console.log('— a tick hides that satellite\'s passes');
  await tick(57166);
  s = await state();
  ok(!s.names.includes('METEOR-M2 3') && s.names.length === 4, `METEOR unticked: its passes gone (${s.names.join(' / ')})`);
  ok(s.sum === 'SHOW: 4 satellites', `header: "${s.sum}"`);
  ok(await ev(`document.querySelector('#satPanel details.sat-show').open`) === true, 'the drop-down stays open while ticking');
  await tick(57166);
  s = await state();
  ok(s.sum === 'SHOW: All' && s.rows === 12 && s.stored === null, 'ticked again: back to All (and nothing stored)');

  console.log('— a group shortcut');
  await group('weather');
  s = await state();
  ok(JSON.stringify(s.names) === '["METEOR-M2 3"]', `Weather: only METEOR's passes (${s.names.join(' / ')}, ${s.rows} rows)`);
  ok(s.sum === 'SHOW: Weather' && s.groups.includes('Weather*'), `header "${s.sum}", the key lit`);
  ok(s.stored === '{"group":"weather"}', `kept as the group: ${s.stored}`);
  await group('tlm');
  s = await state();
  ok(JSON.stringify(s.names.sort()) === JSON.stringify(['ISS', 'RS-44', 'SONATE-2']), `Telemetry: ISS (APRS), RS-44 (beacon), SONATE-2 — ${s.names.join(' / ')}`);
  await group('weather');
  await tick(25544);
  s = await state();
  ok(JSON.stringify(s.names.sort()) === JSON.stringify(['ISS', 'METEOR-M2 3']) && s.sum === 'SHOW: 2 satellites', `Weather + a tick on ISS: "${s.sum}" (${s.names.join(' / ')})`);
  ok(JSON.parse(s.stored).norads.sort().join() === '25544,57166', `stored by NORAD: ${s.stored}`);

  console.log('— remembered across a reload');
  await boot();
  await setList();
  s = await state();
  ok(s.sum === 'SHOW: 2 satellites' && JSON.stringify(s.names.sort()) === JSON.stringify(['ISS', 'METEOR-M2 3']), `after a reload: "${s.sum}" (${s.names.join(' / ')})`);
  // another server: a list without METEOR — its tick is ignored, not lost
  await setList(list().sats.filter((x) => x.norad !== 57166));
  s = await state();
  ok(s.sum === 'SHOW: ISS' && JSON.stringify(s.names) === '["ISS"]', `a server without METEOR: "${s.sum}" — the missing one ignored`);
  ok(JSON.parse(s.stored).norads.includes(57166), '…and still kept for a server that has it');
  await setList();

  console.log('— the picked satellite always stays');
  await ev(`window.__p.onPicked({ norad: 44909, tx: 0, decoder: 'xpdr' }); document.querySelector('#satPanel details.sat-list').open = true; true`);
  s = await state();
  ok(s.names.includes('RS-44'), `RS-44 picked, not ticked: still listed (${s.names.join(' / ')})`);
  ok(s.ticks.includes('RS-44=false'), '…its tick box still unticked (the pick does not change SHOW)');
  ok(await ev(`!!document.querySelector('#satPanel .sat-row.on[data-norad="44909"]')`), '…and lit as the pick');
  await ev(`(() => { const b = document.querySelector('#satPanel button[data-act="exit"]'); b.click(); return true; })()`);
  await ev(`window.__p.show(); true`);
  await setList();
  s = await state();
  ok(!s.names.includes('RS-44'), 'out of the mode (no pick): RS-44 is hidden again');

  console.log('— reminders follow SHOW');
  await group('all');
  await ev(`window.__fake = ${now * 1000}; document.querySelector('#satPanel .sat-rem[data-norad="27607"]').click(); document.querySelector('#satPanel .sat-rem[data-norad="59112"]').click(); true`);
  ok(await ev(`window.__p.reminders().length`) === 2, 'REMIND on SO-50 and SONATE-2');
  await group('tlm');   // SONATE-2 (APRS) shown, SO-50 (FM voice) hidden
  s = await state();
  ok(!s.names.includes('SO-50 (SaudiSat-1C)') && s.names.includes('SONATE-2'), 'Telemetry: SO-50 hidden, SONATE-2 shown');
  await ev(`window.__fake = ${(SO50_AOS - 119) * 1000}; window.__p.remTick(); true`);
  ok(await ev(`document.querySelectorAll('#satRemind .sat-toast').length`) === 0, 'SO-50\'s time comes: hidden by SHOW, no pop-up');
  await ev(`window.__fake = ${(SON2_AOS - 119) * 1000}; window.__p.remTick(); true`);
  const toast = await ev(`document.querySelector('#satRemind .sat-toast')?.textContent || ''`);
  ok(/^SONATE-2 rises in 1:5\d/.test(toast), `SONATE-2's time comes: its pop-up — "${toast.slice(0, 40)}"`);
  ok(await ev(`window.__p.reminders().length`) === 0, 'both reminders taken out of the book');
  await ev(`document.querySelector('#satRemind button[data-act="dismiss"]')?.click(); window.__fake = null; true`);

  console.log('— the looks, desktop and 390 px');
  const CONTRAST = `(() => { const out = [];
    const rgb = (s) => (s.match(/[\\d.]+/g) || []).map(Number);
    const lum = ([r, g, b]) => { const f = (c) => { c /= 255; return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4; }; return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b); };
    for (const sel of ['#satPanel details.sat-show summary', '#satPanel .sat-sht button', '#satPanel .sat-shg button:not(.on)']) {
      const b = document.querySelector(sel);
      let bg = null; for (let e = b; e; e = e.parentElement) { const c = rgb(getComputedStyle(e).backgroundColor); if (c.length >= 3 && (c.length < 4 || c[3] > 0.5)) { bg = c; break; } }
      if (!bg) bg = [0, 0, 0];
      const fg = rgb(getComputedStyle(b).color); const a = lum(fg), z = lum(bg);
      out.push(Math.round(((Math.max(a, z) + 0.05) / (Math.min(a, z) + 0.05)) * 10) / 10);
    } return out; })()`;
  const fits = async () => JSON.parse(await ev(`JSON.stringify((() => { const p = document.getElementById('satPanel'), r = p.getBoundingClientRect(), sh = p.querySelector('details.sat-show').getBoundingClientRect();
    return { l: Math.round(r.left), r: Math.round(r.right), w: innerWidth, sw: document.documentElement.scrollWidth, pw: p.scrollWidth, pcw: p.clientWidth,
             shl: Math.round(sh.left), shr: Math.round(sh.right), bw: p.querySelector('.sat-body').scrollWidth, bcw: p.querySelector('.sat-body').clientWidth }; })())`));
  const fitOk = (f) => f.l >= 0 && f.r <= f.w && f.sw <= f.w + 1 && f.pw <= f.pcw + 1 && f.bw <= f.bcw + 1 && f.shl >= f.l && f.shr <= f.r;
  /* the page's control looks, put on as the page does (applyControlLook — the harness never connects, so #mcard is not
   *  there: what reaches the window is the root overlays and their transparency, exactly as on a live page, where the
   *  satellite window sits outside the card and the decoder box) — and the light colour scheme */
  const looks = [['default', {}, 'dark'], ['silver', { ctlBg: '#c8c8c8', ctlBtn: '#c8c8c8' }, 'dark'], ['black', { ctlBg: '#111111', ctlBtn: '#111111' }, 'dark'],
                 ['transparency off', { ctlSolid: 100 }, 'dark'], ['transparency full', { ctlSolid: 0 }, 'dark'], ['light scheme', {}, 'light']];
  await boot();
  for (const [name, look, scheme] of looks) {
    await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: scheme }] });
    await ev(`(() => { __Sat.applyControlLook(__Sat.readLook(${JSON.stringify(look)})); return 1; })()`);
    await setList();
    await ev(`document.querySelector('#satPanel details.sat-show').open = true; true`);
    await sleep(200);
    const btnBg = await ev(`getComputedStyle(document.getElementById('satPanel')).backgroundColor + ' · ov ' + getComputedStyle(document.documentElement).getPropertyValue('--ov-panel96').trim() + ' · card ' + (document.getElementById('mcard')?.style.getPropertyValue('--btn-bg') || '-')`);
    const c = await ev(CONTRAST);
    ok(Array.isArray(c) && c.every((x) => x >= 3), `${name} (panel ${btnBg}): header, tick boxes and keys legible (${c.join(' / ')} :1)`);
    let f = await fits();
    ok(fitOk(f), `${name}, desktop: the drop-down inside the box, nothing off the side (${JSON.stringify(f)})`);
    await shot(`${name}-desktop`);
    await phone(); await sleep(300);
    f = await fits();
    ok(fitOk(f), `${name}, 390 px: fits, no sideways scroll (${JSON.stringify(f)})`);
    await shot(`${name}-phone`);
    await desk(); await sleep(200);
  }
  await ev(`(() => { try { localStorage.removeItem("vibesdr_web_prefs_v1"); localStorage.removeItem("vibe.satShow"); } catch {} return 1; })()`);
  ok(errors.length === 0, `no page exceptions${errors.length ? ': ' + errors.slice(0, 3).join(' | ') : ''}`);
} catch (e) { ok(false, `driver: ${e?.stack || e}`); }
finally {
  clearTimeout(deadline);
  try { wsC?.close(); } catch {}
  try { edge.kill(); } catch {}
  srv.close();
  await sleep(600);
  try { fs.rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {}
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed · screenshots: ${SHOTS}/sat-show-*.png`);
process.exit(fail ? 1 : 0);
