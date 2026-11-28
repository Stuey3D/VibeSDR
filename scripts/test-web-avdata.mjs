// test-web-avdata.mjs — V12 the web client's AIRCRAFT DATALINK (docs/v12/AVIATION.md) against the REAL server (fake
// rtl_tcp replaying `test-vdl2 --write-clip`), in headless Edge with --mute-audio (tests are silent), at desktop and 390 px:
//   the demodulator menu's DATALINK entry, the full-screen view with its list (five aircraft, flight · registration · ICAO
//   address), no message text (no opt-in), the band control (ACARS ↔ VDL2), the list fitting a phone, EXIT giving the dial
//   back. Needs the page BUILT INTO the server (node scripts/build-web.mjs, then the server rebuilt).
// Screenshots to $TMPDIR/vibesdr-avdata-*.png.
//   VIBESERVER_BIN=…/vibeserver VIBE_VDL2_CLIP=…/v.cu8 node scripts/test-web-avdata.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '', CLIP = process.env.VIBE_VDL2_CLIP || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN, VIBE_VDL2_CLIP and Edge needed'); process.exit(3); }
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
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'Datalink web test', locator: 'IO90bs', adminPass: 'avdata-web-admin',
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
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(os.tmpdir(), `vibesdr-avdata-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
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

console.log('── the aircraft datalink in the web client ──');
await session('avd', CLIP, 136850000, 96100000, 'wfm', async ({ ev, send, shot, until }) => {
  await ev(`document.getElementById('mMode')?.click()`);
  await sleep(700);
  const opts = await ev(`[...document.querySelectorAll('.mModeOpt')].map(b => b.textContent.trim()).join(',')`);
  // ★★★ beta96 ONE AIR & SEA LIVE key in the mode menu (menu consolidation, b96-mapchat): last used = DATALINK → it lands there
  ok(/AIR & SEA LIVE/.test(opts || ''), `the demodulator menu offers AIR & SEA LIVE (DATALINK on its TUNE list): ${opts}`);
  await ev(`localStorage.setItem('vibesdr.airSeaLast', 'av'); document.getElementById('mModeAirSea')?.click(); true`);
  ok(await until(`[...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 6000), 'the datalink view opens');
  // ★ the server lands on the first band (ACARS) — pick VDL2, where the clip's aircraft are
  // ★★★ beta96 (b96-mapchat) the band is a row of the map's TUNE list (data-tune="av:vdl2"), as test-web-avmap picks it
  ok(await until(`!!document.querySelector('.trkView:not([hidden]) .tunePop [data-tune="av:vdl2"]')`, 8000), 'the TUNE list offers VDL2');
  await ev(`(() => { const r = document.querySelector('.trkView:not([hidden]) .tunePop [data-tune="av:vdl2"]'); if (r && r.getAttribute('aria-checked') !== 'true') r.click(); return !!r; })()`);
  // ★★ beta96 the AIR & SEA map opens on MESSAGES for the DATALINK; the aircraft are its AIRCRAFT tab
  ok(await until(`document.querySelectorAll('.trkView:not([hidden]) .trkRow.trkMsg').length >= 3`, 30000), 'MESSAGES: the datalink feed');
  await ev(`document.querySelector('.trkView:not([hidden]) .trkList .hd .tab[data-tab="aircraft"]')?.click()`);
  ok(await until(`document.querySelectorAll('.trkView:not([hidden]) .trkRow').length >= 5`, 30000), `aircraft listed: ${await ev(`[...document.querySelectorAll('.trkView:not([hidden]) .trkRow b')].map(x => x.textContent).join(' | ')`)}`);
  const rows = await ev(`document.querySelector('.trkView:not([hidden]) .rows')?.textContent`) || '';
  ok(/FR1234[\s\S]{0,40}?EI-EST · 4CA97B · VDL2/.test(rows), `a row (its FL | NM pill after the flight, Stuart 13:10): the flight, then registration · ICAO address · the source (the AVIATION MAP's list) — ${rows.slice(0, 200)}`);
  ok(/^AVIATION & MARITIME · DATALINK$/.test(await ev(`document.querySelector('.trkView:not([hidden]) .ttl')?.textContent`) || ''), 'the view is the AIR & SEA map, DATALINK live');
  ok(!/TEST MESSAGE BODY/.test(rows), '★ no message text without the owner\'s opt-in');
  ok(/VDL2 136\.\d+ MHz · \d+ aircraft/.test(await ev(`document.querySelector('.trkView:not([hidden])')?.textContent`) || ''), 'the status names the band and the count');
  // ★ V12 2026-11-16 the ACARS applications: three of the clip's aircraft report a position (FMS, label 16, ADS-C) — on the
  //   map, with the altitude and the fix in the row; G-EZWK's OOOI OUT with its airports; still no message text
  // ★ beta90 (c31cc4566): the aviation map's aircraft are drawn by the ADS-B layer (window.__vibeAcLayer), not as track-layer
  //   markers (.vtl-mk is the HFDL ground stations now) — counted the way test-web-avmap does
  const AC = `(window.__vibeAcLayer ? window.__vibeAcLayer.shown().length : -1)`;
  ok(await until(`${AC} >= 3`, 15000), `aircraft on the map: ${await ev(AC)}`);
  const rows2 = await ev(`document.querySelector('.trkView:not([hidden]) .rows')?.textContent`) || '';
  ok(/37,000 ft · fix 21:20:00 UTC · FMS/.test(rows2), 'the FMS row: altitude, the fix time, the source');
  ok(/36,000 ft · fix at :20:34 past the hour · ADS-C/.test(rows2), 'the ADS-C row: the time past the hour');
  ok(/EGLL → LEMD · OUT 10:15 UTC/.test(rows2), 'the OOOI row: the route and OUT');
  ok(!/EGLLXYA|#M1BPOS/.test(await ev(`document.body.innerHTML`) || ''), '★ the messages themselves never on the page without the opt-in');
  ok(/position messages/.test(await ev(`document.querySelector('.trkView:not([hidden])')?.textContent`) || ''), 'the status counts the position messages');
  /* ★★★ beta97 hotfix THE MESSAGES LIST FROM THE SERVER'S LOG (src/services/avMsgs.ts): backfilled from
   *  /vibeserver/avmsgs.json, the tab saying what it shows, polled no faster than every 5 s while the map is open */
  ok(await until(`(() => { const l = window.__vibeAvLog && window.__vibeAvLog(); return !!l && l.ok === true && l.n >= 3 && l.polling; })()`, 15000), `MESSAGES from the server's log (${await ev(`JSON.stringify(window.__vibeAvLog && window.__vibeAvLog())`)})`);
  const tabTxt = await ev(`document.querySelector('.trkView:not([hidden]) .trkList .hd .tab[data-tab="messages"]')?.textContent || ''`) || '';
  ok(/MESSAGES\s*·\s*(last \d+ (h|min)|last 24 h|latest \d+|this session)/i.test(tabTxt), `the tab says what it shows: "${tabTxt.trim()}"`);
  { const n0 = await ev(`performance.getEntriesByType('resource').filter((e) => /avmsgs\.json/.test(e.name)).length`);
    await sleep(11000);
    const n1 = await ev(`performance.getEntriesByType('resource').filter((e) => /avmsgs\.json/.test(e.name)).length`);
    const direct = await ev(`performance.getEntriesByType('resource').filter((e) => /avmsgs\.json/.test(e.name)).every((e) => new URL(e.name).host === location.host)`);
    ok(n1 - n0 >= 1 && n1 - n0 <= 3 && direct, `polled at the floor: ${n1 - n0} calls in 11 s (5 s floor), this server's own host`); }
  /* ★★★ beta97 hotfix ON THE MAP / HEARD (Stuart: ✈ 28 with nothing drawn): fewer aircraft placed than heard → the LAYERS chip
   *  and the VDL2 row say "n/m", the LIST's line "m aircraft · n with position" */
  { const st = JSON.parse(await ev(`JSON.stringify((() => { const v = document.querySelector('.trkView:not([hidden])');
      return { sum: [...v.querySelectorAll('.mDock[data-dock="layers"] .lsum .ln')].map((e) => e.textContent), row: v.querySelector('.tgl [data-tgl="vdl2"] .n')?.textContent || '', text: v.textContent }; })())`));
    const ac = await ev(`window.__vibeAcLayer ? window.__vibeAcLayer.shown().length : -1`);
    const m = /(\d+) aircraft · (\d+) with position/.exec(st.text);
    ok(st.sum.some((t) => /^\d+\/\d+$/.test(t)) && /^\d+\/\d+$/.test(st.row) && !!m && Number(m[2]) < Number(m[1]),
       `on the map / heard: LAYERS ${JSON.stringify(st.sum)}, the VDL2 row "${st.row}", the LIST "${m ? m[0] : '(none)'}" (${ac} drawn)`); }
  console.log('   screenshot', await shot('desktop'));
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  // ★ map cards: a phone opens with no card — the LIST drawer opened from its chip first
  await ev(`(() => { const b = document.querySelector('.trkView:not([hidden]) [data-cbtn="list"]'); if (b && b.getAttribute('aria-expanded') !== 'true') b.click(); return 1; })()`); await sleep(900);
  const r = JSON.parse(await ev(`JSON.stringify(document.querySelector('.trkView:not([hidden]) .trkList').getBoundingClientRect())`));
  ok(r.left >= -2 && r.right <= 390 + 1 && r.bottom <= 844, `the list fits a phone (the LIST drawer joins the left edge, its border just past it — map cards): ${Math.round(r.left)}–${Math.round(r.right)}, bottom ${Math.round(r.bottom)}`);
  const sw = await ev(`document.documentElement.scrollWidth`);
  ok(sw <= 390, `no sideways scroll at 390 px (${sw})`);
  console.log('   screenshot', await shot('phone'));
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(600);
  await ev(`document.querySelector('.trkView:not([hidden]) .exit').click()`);
  ok(await until(`![...document.querySelectorAll('.trkView')].some(v => !v.hidden)`, 6000), 'EXIT DATALINK closes the view');
  ok(await until(`!(window.__vibeAvLog && window.__vibeAvLog().polling)`, 8000), 'the map shut: the message log is no longer polled');
  await sleep(1500);
  const after = await ev(`document.getElementById('mFreq')?.textContent`);
  ok(/96\.100/.test(after || ''), `the dial is back: ${after}`);
  // ★★ HFDL: a listener's decoder on the dial — offered where vibeserver.json says "hfdl": true (an RTL reaches HF here)
  ok(await ev(`(() => { const b = document.querySelector('#decodersPanel [data-dec="hfdl"]'); return !!b && !b.hidden; })()`), 'the DECODERS panel offers HFDL');
  await ev(`document.querySelector('#decodersPanel [data-dec="hfdl"]').click()`);
  ok(await until(`document.getElementById('decTitle')?.textContent === 'HFDL'`, 4000), `the box is HFDL (${await ev(`document.getElementById('decTitle')?.textContent`)})`);
  ok(/listening for HFDL/.test(await ev(`document.getElementById('decStatus')?.textContent`) || ''), `its status: ${await ev(`document.getElementById('decStatus')?.textContent`)}`);
});
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
