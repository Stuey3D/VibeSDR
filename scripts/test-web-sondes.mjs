// test-web-sondes.mjs — V12 the web client's SONDES mode (RS41 radiosondes, docs/v12/RS41.md) against the REAL server
// (fake rtl_tcp replaying `test-rs41 --write-clip`: three sondes from our own transmitter), in headless Edge with
// --mute-audio (tests are silent), at desktop and 390 px: the demodulator menu's SONDES entry, the full-screen view with its
// list (serial · height · climb, "not yet calibrated" — never a guessed temperature), the sondes on the map, a chosen
// sonde's card, the band control, the list fitting a phone, EXIT giving the dial back. Needs the page BUILT INTO the
// server (node scripts/build-web.mjs, then the server rebuilt). ★ Synthetic sondes: nothing here was heard on air.
// Screenshots to $TMPDIR/vibesdr-sondes-*.png.
//   VIBESERVER_BIN=…/vibeserver VIBE_RS41_CLIP=…/r.cu8 node scripts/test-web-sondes.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_RS41_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_RS41_CLIP and Edge needed'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });


async function session(tag, clip, centre, freq, mode, run) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-web${tag}-`));
  // ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
  //   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
  process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Sondes web test', locator: 'IO70jf', adminPass: 'sondes-web-admin',
    radios: [{ serial: `${tag}-web`, label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
  const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--hf', clip, '--hf-centre', String(centre)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', String(freq), '--mode', mode, '--users', '1'],
                    { env, stdio: ['ignore', fs.openSync(path.join(dir, 'server.log'), 'w'), 'ignore'] });
  const edge = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', `--remote-debugging-port=${dbg}`, '--no-first-run', '--no-default-browser-check',
    `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
  let wsC; const failBefore = fail;
  try {
    await sleep(3000);
    const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
    wsC = new WebSocket(list.find((t) => t.type === 'page').webSocketDebuggerUrl);
    let id = 0; const pend = new Map();
    wsC.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
    await new Promise((r) => { wsC.onopen = r; });
    const send = (method, params = {}) => new Promise((res) => { const i = ++id; pend.set(i, res); wsC.send(JSON.stringify({ id: i, method, params })); });
    const ev = async (x) => { const r = await send('Runtime.evaluate', { expression: x, awaitPromise: true, returnByValue: true }); return r.result?.result?.value ?? r.result?.exceptionDetails?.text; };
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-sondes-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
    const until = async (x, ms) => { for (let i = 0; i < ms / 250; i++) { if (await ev(x)) return true; await sleep(250); } return false; };
    await send('Page.enable'); await send('Runtime.enable');
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
    await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
    await until(`!!document.querySelector('#audioGate button')`, 15000);
    {
      const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('#audioGate button').getBoundingClientRect())`));
      const x = r.x + r.width / 2, y = r.y + r.height / 2;
      await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
      await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
    }
    ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000), `${tag}: START RADIO pressed`);
    await sleep(2500);
    await run({ ev, send, shot, until });
  } catch (e) { ok(false, `${tag} driver: ${e}`); }
  finally {
    try { wsC?.close(); } catch {}
    edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(600);
    if (fail === failBefore) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
  }
}

console.log('── radiosondes in the web client ──');
await session('snd', CLIP, 403005000, 96100000, 'wfm', async ({ ev, send, shot, until }) => {
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(700);
  const opts = await ev(`[...document.querySelectorAll('.mModeOpt')].map(b => b.textContent.trim()).join(',')`);
  // ★★★ beta96 ONE AIR & SEA LIVE key in the mode menu (menu consolidation, b96-mapchat): last used = SONDES → it lands there
  ok(/AIR & SEA LIVE/.test(opts || ''), `the demodulator menu offers AIR & SEA LIVE (SONDES on its TUNE list): ${opts}`);
  await ev(`localStorage.setItem('vibesdr.airSeaLast', 'sd'); document.getElementById('mModeAirSea')?.click(); true`);
  ok(await until(`[...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 6000), 'the sondes view opens');
  ok(await until(`/AVIATION & MARITIME · RADIOSONDES/.test(document.querySelector('.trkView:not([hidden]) .ttl')?.textContent || '')`, 3000), '★ beta96 stage 8: the AIR & SEA map, SONDES live (its title says RADIOSONDES)');
  ok(await until(`document.querySelector('.trkView:not([hidden]) .exit')?.textContent === 'EXIT SONDES'`, 3000), 'EXIT SONDES on the map');
  ok(await until(`document.querySelector('.trkView:not([hidden]) .tab[aria-selected="true"]')?.dataset.tab === 'sondes' || !document.querySelector('.trkView:not([hidden]) .tabs:not([hidden])')`, 3000), 'on the SONDES tab');
  const bands = await ev(`[...document.querySelectorAll('.trkView:not([hidden]) [data-rng]')].map(b => b.textContent.trim()).join(',')`);
  ok(bands === '400–402,402–404,404–406', `the band control: ${bands} (the whole 400–406 is not offered at 2.4 MS/s)`);
  ok(await until(`document.querySelector('.trkView:not([hidden]) [data-rng="mid"]')?.getAttribute('aria-pressed') === 'true'`, 8000), 'the band the radio is on is the lit one (402–404)');
  ok(await until(`document.querySelectorAll('.trkView:not([hidden]) .trkRow').length >= 3`, 30000), `sondes listed: ${await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow b')].map(x => x.textContent).join(' | ')`)}`);
  ok(await until(`document.querySelectorAll('.trkView:not([hidden]) .trkRow.nopos').length === 0`, 15000), 'every sonde has a position');
  const rows = await ev(`document.querySelector('.trkView:not([hidden]) .rows')?.textContent`) || '';
  ok(/S3310411 · 3,\d{3} m · \+5\.2 m\/s/.test(rows), 'a row: serial · height · climb');
  ok(/U0912345 · 2\d,\d{3} m · −14\.0 m\/s/.test(rows), 'the descending one, its climb negative');
  ok(/403\.300 MHz/.test(rows) && /402\.700 MHz/.test(rows), 'where each was heard');
  ok(/not yet calibrated \(\d+ of 51\)/.test(rows) && !/°C/.test(rows), '★ "not yet calibrated" — no temperature before the calibration is whole');
  ok(/402–404 MHz · 3 sondes/.test(await ev(`document.querySelector('.trkView:not([hidden]) .st')?.textContent`) || ''), `the status: ${await ev(`document.querySelector('.trkView:not([hidden]) .st')?.textContent`)}`);
  // ★ the balloon glyph (sondes.ts sondeIcon) is the only ellipse on the map: one per sonde
  ok(await until(`document.querySelectorAll('.trkView:not([hidden]) .trkMapHost ellipse').length === 3`, 15000),
     `the three sondes are on the map (${await ev(`document.querySelectorAll('.trkView:not([hidden]) .trkMapHost ellipse').length`)} balloons)`);
  await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow')].find(r => r.dataset.key === 'sd:S3310411')?.click()`);
  ok(await until(`/S3310411/.test(document.querySelector('.vtl-card')?.textContent || '')`, 6000), `a chosen sonde's card: ${(await ev(`document.querySelector('.vtl-card')?.textContent`) || '').slice(0, 120)}`);
  console.log('   screenshot', await shot('desktop'));
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('.trkView:not([hidden]) .trkList').getBoundingClientRect())`));
  ok(r.left >= 5 && r.right <= 390 - 5 && r.bottom <= 844, `the list fits a phone: ${Math.round(r.left)}–${Math.round(r.right)}, bottom ${Math.round(r.bottom)}`);
  const sw = await ev(`document.documentElement.scrollWidth`);
  ok(sw <= 390, `no sideways scroll at 390 px (${sw})`);
  console.log('   screenshot', await shot('phone'));
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(600);
  // ★ the band control moves the radio (the server owns the band: the next report lights the new one)
  await ev(`document.querySelector('.trkView:not([hidden]) [data-rng="hi"]')?.click()`);
  ok(await until(`document.querySelector('.trkView:not([hidden]) [data-rng="hi"]')?.getAttribute('aria-pressed') === 'true' && /404–406 MHz/.test(document.querySelector('.trkView:not([hidden]) .st')?.textContent || '')`, 8000), 'a band pick: 404–406 MHz');
  // ★★★ beta96 maps2: the server feeds the day's sondes card every 20 s while SONDES runs (vibe_posgal.h) — make sure one
  //     tick has seen positions before leaving, so the card is written when the mode stops
  await sleep(22000);
  await ev(`document.querySelector('.trkView:not([hidden]) .exit').click()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 6000), 'EXIT SONDES closes the view');
  await sleep(1500);
  const after = await ev(`document.getElementById('mFreq')?.textContent`);
  ok(/96\.100/.test(after || ''), `the dial is back: ${after}`);
  /* ★★★ beta96 maps2 STORED SONDES: the mode's end writes the day's card (gallery kind sondes, slot sondes-<date>); the AIR & SEA
   *  map, opened from VIEWS · AVIATION with SONDES off, draws them from it — faded by age, keyed sds:, in the SONDES tab as
   *  "remembered", the KEY's remembered row, the status. */
  const cardUp = await until(`fetch('/vibeserver/gallery.json?kind=sondes&limit=10', { cache: 'no-store' }).then(r => r.json()).then(j => (j.items || []).some(i => /^sondes-\\d{4}-/.test(i.s || ''))).catch(() => false)`, 40000);
  ok(cardUp, 'the day\'s sondes card is in the gallery once SONDES stops (kind sondes, slot sondes-<date>)');
  const card = await ev(`fetch('/vibeserver/gallery.json?kind=sondes&limit=10', { cache: 'no-store' }).then(r => r.json()).then(j => { const it = (j.items || []).find(i => /^sondes-/.test(i.s || '')); return it ? fetch('/vibeserver/gallery/file/' + encodeURIComponent(it.f)).then(r => r.text()) : ''; })`) || '';
  ok(/^Radiosondes heard · \d{4}-\d\d-\d\d \(UTC\)/.test(card) && /S3310411 · - · /.test(card) && /U0912345 · - · /.test(card), `the card lists the sondes heard: ${card.split('\n').slice(5, 8).join(' | ')}`);
  await ev(`document.getElementById('avMapBtn')?.click()`);
  ok(await until(`[...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 6000), 'VIEWS · AVIATION opens the AIR & SEA map with SONDES off');
  ok(await until(`!!document.querySelector('.trkView:not([hidden]) .tab[data-tab="sondes"]')`, 20000),
     `the SONDES tab is there from the store: ${await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .tab')].map(b => b.textContent.trim()).join(',')`)}`);
  const chips = await ev(`[...document.querySelectorAll('.trkView:not([hidden]) [data-tgl]')].map(b => b.textContent.trim()).join(',')`) || '';
  ok(!chips || /SONDES/.test(chips), `its chip where there is a chip row (one layer alone draws none): ${chips || '(none)'}`);
  ok(!/RADIOSONDES/.test(await ev(`document.querySelector('.trkView:not([hidden]) .ttl')?.textContent`) || ''), 'not live: the title is not SONDES\'s');
  await ev(`document.querySelector('.trkView:not([hidden]) .tab[data-tab="sondes"]')?.click()`);
  ok(await until(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow')].filter(r => (r.dataset.key || '').startsWith('sds:')).length >= 3`, 8000),
     `the SONDES tab lists them as remembered: ${await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow')].map(r => r.dataset.key + ' ' + (r.querySelector('small')?.textContent || '')).join(' | ')`)}`);
  ok(/remembered/.test(await ev(`document.querySelector('.trkView:not([hidden]) .trkRow[data-key^="sds:"] small')?.textContent`) || ''), 'a row says remembered');
  ok(await until(`[...document.querySelectorAll('.trkView:not([hidden]) .trkMapHost ellipse')].filter(e => e.getAttribute('fill') === '#9cc3d8').length >= 3`, 10000),
     `the remembered balloons are on the map (${await ev(`document.querySelectorAll('.trkView:not([hidden]) .trkMapHost ellipse').length`)})`);
  ok(/sondes remembered \(24 h\)/.test(await ev(`document.querySelector('.trkView:not([hidden]) .st')?.textContent`) || ''), `the status says so: ${await ev(`document.querySelector('.trkView:not([hidden]) .st')?.textContent`)}`);
  await ev(`document.querySelector('.trkView:not([hidden]) .trkRow[data-key="sds:S3310411"]')?.click()`);
  ok(await until(`/Remembered by this server/.test(document.querySelector('.vtl-card')?.textContent || '')`, 6000), `its card: ${(await ev(`document.querySelector('.vtl-card')?.textContent`) || '').slice(0, 140)}`);
  console.log('   screenshot', await shot('stored'));
  await ev(`document.querySelector('.trkView:not([hidden]) .exit').click()`);
});
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
