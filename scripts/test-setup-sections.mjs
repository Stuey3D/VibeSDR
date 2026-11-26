// test-setup-sections.mjs — ★★★ beta96 b96-setupux (Stuart 11-26): the Setup page's COLLAPSIBLE SECTIONS and the RED "!".
// "Made for the Sony TV: scrolling the long screen with a remote took ages." Drives the REAL page in headless MUTED Edge
// against a real vibeserver front door built from this tree (no radio process). A made-up two-radio machine with no
// location: one radio never set up, one set up with its own 5 fps.
//   • every card of the SERVER tab and a radio tab has a header (title + one-line summary), closed by default;
//   • ★★★ beta96 (Stuart 11-26) THREE STATES: green (set up), AMBER (worth setting; calm — a mark, the reason, NOT opened),
//     RED "!" only after a SAVE it stopped — on its own tab, opened, refused with a line, red until fixed:
//     no location → amber, a locator → green live; PSK Reporter with no name → amber, a typed name that is not one → amber
//     until Save, then RED + "Not saved"; every mode off on a radio → amber, its Save → RED; the SERVER tab's Save with that
//     radio still all-off → its tab selected, RED; a radio never set up → amber, red after the master save's gate;
//   • a header click toggles and is remembered across a reload (localStorage); OPEN ALL / CLOSE ALL;
//   • keys: ↓ / ↑ step header to header, Enter opens and moves focus inside, Escape goes back to the header;
//   • the frame rate: the Server tab's Frame rate, the new radio follows it, the old one keeps its 5 — saved so.
// ★ SILENT: --mute-audio, no radio process (VIBESERVER_NO_RADIO_SPAWN). Exit 3 = not run.
//   usage: VIBESERVER_BIN=… node --max-old-space-size=512 scripts/test-setup-sections.mjs
import net from 'node:net';
import fs from 'node:fs';
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

const ADMIN = 'sections-admin';
const base = { enabled: true, users: 1, rate: 2400000, freq: 100000000, demodMode: 'wfm', blockedModes: '' };
const RADIOS = [
  { ...base, serial: 'NEW00001', driver: 'rtlsdr', label: 'New dongle', configured: false, order: 0 },
  { ...base, serial: 'OLD00002', driver: 'rtlsdr', label: 'Old dongle', configured: true, maxFps: 5, fftRate: 5, order: 1 },
];
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-sections-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const P = await freePort();
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({ configured: true, fullMode: true, name: 'Two radios', adminPass: ADMIN, port: P, radios: RADIOS }));
const savedAll = () => JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
const savedOf = (serial) => savedAll().radios.find((r) => r.serial === serial) || {};
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: cfgPath,
              VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir };
const doorFd = fs.openSync(path.join(dir, 'door.log'), 'w');
const door = spawn(BIN, [], { env, stdio: ['ignore', doorFd, doorFd] });
let CDP = await freePort(); while (CDP === P || CDP === P + 1) CDP = await freePort();
const br = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', `--remote-debugging-port=${CDP}`,
                        `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
const stopAll = () => { for (const p of [door, br]) { try { p && p.kill('SIGKILL'); } catch {} } };
process.on('exit', stopAll);
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => { stopAll(); process.exit(1); });
const abort = (why) => { console.log(`   FAIL aborted — ${why}`); stopAll(); process.exit(1); };
const guard = setInterval(() => {
  if (process.memoryUsage().rss > (1 << 30)) abort('the test itself passed 1 GB');
  const ps = spawn('ps', ['-o', 'rss=', '-p', [door.pid, br.pid].join(',')]); let out = '';
  ps.stdout.on('data', (d) => { out += d; });
  ps.on('close', () => { if (Math.max(0, ...out.split(/\s+/).filter(Boolean).map(Number)) > 2 * 1048576) abort('a child passed 2 GB'); });
}, 1000);
const deadline = setTimeout(() => abort('the 180 s deadline passed'), 180000);
const up = async (port) => { for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };

let call, js;
const tab = (label) => js(`(() => { const b = [...document.querySelectorAll("#radioTabs button")].find(x => x.textContent.trim().startsWith(${JSON.stringify(label)})); if (!b) return false; b.click(); return true; })()`);
const head = (pane, title) => `[...document.querySelectorAll("#${pane} .card.sec")].find(c => c.querySelector(".secTitle").textContent === ${JSON.stringify(title)})`;
const state = (pane, title) => js(`(() => { const c = ${head(pane, title)}; if (!c) return null; return { open: c.classList.contains("secOpen"), must: c.classList.contains("secMust"), tone: c.dataset.secState, sum: c.querySelector(".secSum").textContent }; })()`);
const key = async (k, code, vk, text) => { await call('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk, ...(text ? { text } : {}) }); await call('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk }); await sleep(60); };
const focusTitle = () => js(`(() => { const a = document.activeElement; if (!a) return ""; if (a.classList.contains("secHead")) return "H:" + a.querySelector(".secTitle").textContent; const c = a.closest(".card.sec"); return c ? "IN:" + c.querySelector(".secTitle").textContent : a.tagName; })()`);
const type = (id, v) => js(`(() => { const e = document.getElementById(${JSON.stringify(id)}); e.value = ${JSON.stringify(v)}; e.dispatchEvent(new Event("input", { bubbles: true })); e.dispatchEvent(new Event("change", { bubbles: true })); return 1; })()`);
async function openSetup() {
  await call('Page.navigate', { url: `http://127.0.0.1:${P}/setup` });
  ok(await until(async () => await js(`(() => { const c = document.getElementById("signinCard"); return !!c && c.offsetParent !== null; })()`), 10000), 'the setup page asks to sign in');
  await js(`document.getElementById("pass").value = ${JSON.stringify(ADMIN)}; document.getElementById("signinBtn").click(); 1`);
  ok(await until(async () => (await js(`document.querySelectorAll("#radioTabs button").length`)) === 3, 10000), 'three tabs: SERVER and the two radios');
  await sleep(400);
}

try {
  ok(await up(P), `the front door is up (${P})`);
  let t; for (let i = 0; i < 60 && !t; i++) { try { t = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find((x) => x.type === 'page'); } catch { await sleep(250); } }
  const cws = new WebSocket(t.webSocketDebuggerUrl); await new Promise((r) => { cws.onopen = r; });
  let id = 0; const pend = new Map(); cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  call = (method, params = {}) => new Promise((r) => { const n = ++id; pend.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); setTimeout(() => { if (pend.delete(n)) r({}); }, 10000); });
  js = async (e) => (await call('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await call('Runtime.enable');
  await openSetup();

  console.log('── the SERVER tab: a header on every card, closed by default ──');
  ok(await js(`(() => { const t = [...document.querySelectorAll("#radioTabs button")].find(b => b.dataset.i === "-1"); t.click(); return 1; })()`), 'the SERVER tab');
  await sleep(300);
  const srv = await js(`[...document.querySelectorAll("#serverPane .card")].filter(c => c.offsetParent !== null).map(c => ({ t: c.querySelector(".secTitle") ? c.querySelector(".secTitle").textContent : "(none) " + (c.querySelector("h2") || {}).textContent, open: c.classList.contains("secOpen"), must: c.classList.contains("secMust") }))`) || [];
  ok(srv.length >= 8 && srv.every((c) => !c.t.startsWith('(none)')), `every shown server card has a header (${srv.map((c) => c.t).join(' · ')})`);
  ok(srv.filter((c) => !c.must).every((c) => !c.open), 'every section without a "!" starts closed');
  ok(await js(`[...document.querySelectorAll("#serverPane .card.sec:not(.secOpen)")].every(c => [...c.children].slice(1).every(x => getComputedStyle(x).display === "none"))`), 'a closed section shows its header only');
  let s = await state('serverPane', 'Where this receiver is');
  ok(s && !s.must && s.tone === 'amber' && !s.open && /^Location not set: satellites and maps need it/.test(s.sum),
     `★ no location: AMBER, calm (closed, no "!"), "${s && s.sum}"`);
  s = await state('serverPane', 'Audio');
  ok(s && !s.open && /Opus/.test(s.sum), `a closed section's one line says what it is set to (Audio · "${s && s.sum}")`);
  s = await state('serverPane', 'Frame rate');
  ok(s && /10 fps/.test(s.sum), `Frame rate · "${s && s.sum}"`);
  await type('locator', 'IO91wm');
  await sleep(150);
  s = await state('serverPane', 'Where this receiver is');
  ok(s && !s.must && s.tone === 'green' && /IO91wm/.test(s.sum), `★ a locator typed: GREEN live ("${s && s.sum}")`);
  await type('locator', '');
  await sleep(150);
  ok((await state('serverPane', 'Where this receiver is')).tone === 'amber', '…and amber again when it is emptied');
  await type('locator', 'IO91wm');

  // ★ SECTIONS_SHOT=<dir>: pictures to LOOK at (no visible polish gaps) — a wide window and a phone's width
  if (process.env.SECTIONS_SHOT) {
    for (const [w, h] of [[1280, 900], [390, 844]]) {
      await call('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: w < 500 });
      await sleep(300);
      const shot = await call('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false });
      if (shot.result) fs.writeFileSync(path.join(process.env.SECTIONS_SHOT, `server-${w}.png`), Buffer.from(shot.result.data, 'base64'));
    }
    await call('Emulation.clearDeviceMetricsOverride');
  }
  console.log('── PSK Reporter: the save path\'s own refusal ──');
  await js(`(() => { const e = document.getElementById("pskrOn"); e.checked = true; e.dispatchEvent(new Event("change", { bubbles: true })); return 1; })()`);
  await type('pskrCall', '');
  await sleep(150);
  s = await state('serverPane', 'PSK Reporter');
  ok(s && !s.must && s.tone === 'amber' && /^PSK Reporter: no callsign, so no spots are sent$/.test(s.sum), `PSK Reporter on, no name: AMBER, "no spots are sent" (the uploader refuses without one) ("${s && s.sum}")`);
  await type('pskrCall', 'X');
  await sleep(150);
  s = await state('serverPane', 'PSK Reporter');
  ok(s && !s.must && s.tone === 'amber', `a name typed that is not one: still calm before a save ("${s && s.sum}")`);
  const m0s = fs.statSync(cfgPath).mtimeMs;
  await js(`document.getElementById("saveBtn").click(); 1`);
  await sleep(400);
  s = await state('serverPane', 'PSK Reporter');
  ok(s && s.must && s.open && /^! PSK Reporter: that receiver name is not valid/.test(s.sum), `★ SAVE pressed: RED "!", opened ("${s && s.sum}")`);
  ok(/^Not saved/.test(await js(`document.getElementById("saveErr").textContent`) || '') && fs.statSync(cfgPath).mtimeMs === m0s,
     `…the save refused with its line: "${await js(`document.getElementById("saveErr").textContent`)}"`);
  await type('pskrCall', 'G4ABC-SDR');
  await sleep(150);
  s = await state('serverPane', 'PSK Reporter');
  ok(s && !s.must && s.tone === 'green' && /On · G4ABC-SDR/.test(s.sum), `a valid name typed: GREEN, the red gone ("${s && s.sum}")`);
  await type('pskrCall', 'X');
  await sleep(150);
  ok(!(await state('serverPane', 'PSK Reporter')).must, 'broken again WITHOUT a new save: calm (red needs a save attempt)');
  await type('pskrCall', 'G4ABC-SDR');
  s = await state('serverPane', 'Audio');
  ok(s && s.tone === 'green', `a section with nothing to set: GREEN (Audio: ${s && s.tone})`);
  await js(`(() => { const e = document.getElementById("pskrOn"); e.checked = false; e.dispatchEvent(new Event("change", { bubbles: true })); return 1; })()`);

  console.log('── toggle, remember, OPEN ALL / CLOSE ALL ──');
  await js(`${head('serverPane', 'Audio')}.querySelector(".secHead").click(); 1`);
  ok((await state('serverPane', 'Audio')).open, 'a click on the header opens it');
  ok(await js(`JSON.parse(localStorage.getItem("vibeSetupSections"))["server:audio"] === 1`), 'remembered on this device (localStorage)');
  await js(`document.getElementById("secOpenAll").click(); 1`);
  ok(await js(`[...document.querySelectorAll("#serverPane .card.sec")].filter(c => c.offsetParent !== null).every(c => c.classList.contains("secOpen"))`), 'OPEN ALL: every section of this tab open');
  await js(`document.getElementById("secCloseAll").click(); 1`);
  ok(await js(`[...document.querySelectorAll("#serverPane .card.sec")].filter(c => c.offsetParent !== null).every(c => !c.classList.contains("secOpen"))`), 'CLOSE ALL: every section closed');
  await js(`${head('serverPane', 'Audio')}.querySelector(".secHead").click(); 1`);

  console.log('── keys: header to header, Enter in, Escape back ──');
  await js(`[...document.querySelectorAll("#serverPane .card.sec")].filter(c => c.offsetParent !== null)[0].querySelector(".secHead").focus(); 1`);
  const order = await js(`[...document.querySelectorAll("#serverPane .card.sec")].filter(c => c.offsetParent !== null).map(c => c.querySelector(".secTitle").textContent)`);
  const f0 = await focusTitle();
  ok(f0 === 'H:' + order[0], `focus on the first header (${f0})`);
  await key('ArrowDown', 'ArrowDown', 40);
  ok((await focusTitle()) === 'H:' + order[1], `↓ → the next header (${await focusTitle()})`);
  await key('s', 'KeyS', 83, 's');
  ok((await focusTitle()) === 'H:' + order[2], `S (WASD) → the next one (${await focusTitle()})`);
  await key('ArrowUp', 'ArrowUp', 38);
  ok((await focusTitle()) === 'H:' + order[1], `↑ → back one (${await focusTitle()})`);
  // walk to Audio (it is open from above: close it first so ↓ steps over closed ones only)
  await js(`(() => { const c = ${head('serverPane', 'Audio')}; if (c.classList.contains("secOpen")) c.querySelector(".secHead").click(); c.querySelector(".secHead").focus(); return 1; })()`);
  await key('Enter', 'Enter', 13, '\r');
  ok((await state('serverPane', 'Audio')).open && (await focusTitle()) === 'IN:Audio', `Enter (OK) opens it and moves focus inside (${await focusTitle()})`);
  await key('Escape', 'Escape', 27);
  ok((await focusTitle()) === 'H:Audio', `Escape (BACK) → its header (${await focusTitle()})`);
  await key('ArrowDown', 'ArrowDown', 40);
  ok((await focusTitle()) === 'IN:Audio', `↓ on an OPEN section's header goes into it (${await focusTitle()})`);

  console.log('── a reload remembers ──');
  await openSetup();
  await js(`(() => { const t = [...document.querySelectorAll("#radioTabs button")].find(b => b.dataset.i === "-1"); t.click(); return 1; })()`);
  await sleep(300);
  ok((await state('serverPane', 'Audio')).open && !(await state('serverPane', 'Processor')).open, 'after a reload: Audio open (left open), Processor closed');

  console.log('── a radio tab ──');
  ok(await tab('New dongle'), 'the new radio\'s tab');
  await sleep(400);
  const rad = await js(`[...document.querySelectorAll("#radioPane .card")].filter(c => c.offsetParent !== null).map(c => c.querySelector(".secTitle") ? c.querySelector(".secTitle").textContent : "(none)")`) || [];
  ok(rad.length >= 8 && !rad.includes('(none)'), `every shown radio card has a header (${rad.join(' · ')})`);
  s = await state('radioPane', 'SDR display name');
  ok(s && !s.must && s.tone === 'amber' && /^Not set up yet/.test(s.sum), `a radio never set up: AMBER before any save ("${s && s.sum}")`);
  s = await state('radioPane', 'Operating mode');
  ok(s && /^Unlocked|^Locked|^Single use/.test(s.sum), `Operating mode · "${s && s.sum}"`);
  s = await state('radioPane', "This radio's hardware");
  ok(s && /server’s 10 fps/.test(s.sum), `★ the new radio follows the server's frame rate ("${s && s.sum}")`);
  ok(await js(`document.getElementById("maxFps").value`) === 'server', 'its rate menu says "Use the server\'s frame rate"');
  await js(`radio().blockedModes = Object.keys(MODE_GENS).join(","); modeBlockRender(); modeBlockSummary(); 1`);
  await sleep(200);
  s = await state('radioPane', 'Modes and decoders');
  ok(s && !s.must && s.tone === 'amber' && /^Every mode is off/.test(s.sum), `every mode off: AMBER until a save ("${s && s.sum}")`);
  await js(`document.getElementById("saveRadioBtn").click(); 1`);
  await sleep(400);
  s = await state('radioPane', 'Modes and decoders');
  ok(s && s.must && s.open && /^! Every mode is off/.test(s.sum), `★ Save radio pressed: RED "!", opened ("${s && s.sum}")`);
  ok(/^Not saved/.test(await js(`document.getElementById("saveErr").textContent`) || ''), '…refused with its line');
  // ★ the SERVER tab's Save with this radio still all-off: its tab is SELECTED and the section shown red
  await js(`(() => { const t = [...document.querySelectorAll("#radioTabs button")].find(b => b.dataset.i === "-1"); t.click(); return 1; })()`);
  await sleep(300);
  ok(await js(`curRadio`) === -1, 'on the SERVER tab');
  await js(`document.getElementById("saveBtn").click(); 1`);
  await sleep(500);
  ok(await js(`radio() && radio().serial`) === 'NEW00001' && await js(`curRadio`) >= 0, `★ the master Save selected the offending radio's tab (${await js(`radio() && radio().serial`)})`);
  s = await state('radioPane', 'Modes and decoders');
  ok(s && s.must && s.open, `…its Modes and decoders RED and open ("${s && s.sum}")`);
  await js(`radio().blockedModes = ""; modeBlockRender(); modeBlockSummary(); 1`);
  await sleep(200);
  s = await state('radioPane', 'Modes and decoders');
  ok(s && !s.must && s.tone === 'green', `…a mode back on: GREEN, the red gone (${s && s.tone})`);
  // ★ the master save's gate marks a radio never set up red (it will not be on air) — without refusing
  ok(await js(`secSaveStops(true)`) === false, 'the master gate does not refuse for a radio never set up…');
  await sleep(150);
  s = await state('radioPane', 'SDR display name');
  ok(s && s.must && /^! Not set up yet/.test(s.sum), `…but shows it RED ("${s && s.sum}")`);
  ok(await js(`SEC_RULES.block.join() + "|" + SEC_RULES.advise.join()`) === 'dir,modes,flight-trackers,beast,pskr-invalid,radio-never-set-up|location,pskr-no-callsign,radio-not-detected',
     'the rule names (the app holds the same list)');
  ok(await tab('Old dongle'), 'the old radio\'s tab');
  await sleep(400);
  ok(await js(`document.getElementById("maxFps").value`) === '5', 'the old radio keeps its own 5 fps (saved before beta96: identical behaviour)');
  ok(!(await state('radioPane', 'SDR display name')).must, 'a radio that is set up: no "!" on its name');
  ok(!(await state('radioPane', 'SDR display name')).open, 'switching from a radio with a "!" to one without does not leave the section open');
  await js(`document.getElementById("modeLocked").click(); 1`);
  await sleep(300);
  ok(await js(`[...document.querySelectorAll("#radioPane .card.sec")].every(c => [...c.children].filter(x => x.classList.contains("secHead")).length === 1 && c.firstElementChild.classList.contains("secHead"))`),
     'a locked radio (rows moved between cards): every card still has ONE header, first');
  s = await state('radioPane', 'Operating mode');
  ok(s && /^Locked/.test(s.sum), `Operating mode · "${s && s.sum}"`);
  await js(`document.getElementById("modeSingle").click(); 1`);
  await sleep(200);
  ok(await js(`[...document.querySelectorAll("#radioPane .card.sec")].every(c => [...c.children].filter(x => x.classList.contains("secHead")).length === 1 && c.firstElementChild.classList.contains("secHead"))`),
     'unlocked again (rows moved home, inserted at the top of the hardware card): still ONE header, first');
  ok(!!(await state('radioPane', 'Spectrum detail')), 'the zoom switch has its own section (Spectrum detail), not a loose line between cards');

  if (process.env.SECTIONS_SHOT) {
    await js(`radio().blockedModes = Object.keys(MODE_GENS).join(","); modeBlockRender(); modeBlockSummary(); 1`); await sleep(200);
    await js(`window.scrollTo(0, 0); 1`);
    for (const [w, h] of [[1280, 1400], [390, 1400]]) {
      await call('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: w < 500 });
      await sleep(300);
      const shot = await call('Page.captureScreenshot', { format: 'png' });
      if (shot.result) fs.writeFileSync(path.join(process.env.SECTIONS_SHOT, `radio-${w}.png`), Buffer.from(shot.result.data, 'base64'));
    }
    await call('Emulation.clearDeviceMetricsOverride');
    await js(`radio().blockedModes = ""; modeBlockRender(); modeBlockSummary(); 1`);
  }
  console.log('── the frame rate is saved ──');
  await js(`(() => { const t = [...document.querySelectorAll("#radioTabs button")].find(b => b.dataset.i === "-1"); t.click(); return 1; })()`);
  await sleep(200);
  await js(`(() => { const e = document.getElementById("fpsDefault"); e.value = "20"; e.dispatchEvent(new Event("change", { bubbles: true })); return 1; })()`);
  ok(/Used by: New dongle/.test(await js(`document.getElementById("fpsDefaultUse").textContent`) || ''), `the card says who uses it: "${await js(`document.getElementById("fpsDefaultUse").textContent`)}"`);
  await tab('New dongle'); await sleep(400);
  const m0 = fs.statSync(cfgPath).mtimeMs;
  await js(`document.getElementById("saveRadioBtn").click(); 1`);
  ok(await until(() => fs.statSync(cfgPath).mtimeMs > m0, 10000), 'saved through the page');
  const all = savedAll();
  ok(all.fpsDefault === 20, `the server's frame rate saved: ${all.fpsDefault}`);
  ok(savedOf('NEW00001').fpsFollow === true, 'the new radio saved following it');
  ok(savedOf('OLD00002').fpsFollow === false && savedOf('OLD00002').fftRate === 5 && savedOf('OLD00002').maxFps === 5, 'the old radio saved with its own 5');
  await sleep(300);
  ok(!(await state('radioPane', 'SDR display name')).must, 'saved: the "Not set up yet" "!" clears');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  clearInterval(guard); clearTimeout(deadline);
  stopAll();
  await sleep(500);
}
console.log(`\n${fail ? '✗' : '✓'} test-setup-sections: ${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs in ${dir})`); else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
