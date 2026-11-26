// test-setup-paul-tabs.mjs — ★★★ beta96 (Stuart 11-26, a tester's multi-radio server): "when I went back in all modes were
// ticked again". A made-up config shaped like his (never his real one): SEVEN radio entries, two of them present, the
// ADS-B radio fifth — an RTL locked at 1090 MHz, landing in ADS-B and keeping it running, every switch off except ADS-B,
// saved by an older beta (its list names nothing newer than generation 80, and carries "+"-option tokens).
// Drives the REAL setup page in headless MUTED Edge against a real vibeserver front door built from this tree:
//   • open an all-ticked radio's tab FIRST, then the ADS-B radio's: every box but ADS-B unticked (the bug: a LOCKED radio's
//     Modes card was never redrawn, so it showed the previous tab's ticks);
//   • the new switches (SATELLITE, VOR MAP, DV signalling …) are marked NEW and unticked; the one-line notice says so;
//   • the only-mode note says ADS-B only; "When an update adds new modes" is fixed at off (single-mode);
//   • All on / All off change the boxes only (config.json untouched until Save); All off keeps ADS-B (its landing);
//   • Save writes "seen@N" and the same enforced list; the page reopened shows the ADS-B tab exactly the same;
//   • the other radios' lists are untouched by the save.
// ★ SILENT: no audio (--mute-audio), no radio process at all (VIBESERVER_NO_RADIO_SPAWN). Exit 3 = not run.
//   usage: VIBESERVER_BIN=… node --max-old-space-size=512 scripts/test-setup-paul-tabs.mjs
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

// ── the made-up config ──
const h = fs.readFileSync(path.join(root, 'android/app/src/main/cpp/vibe_decoder_reach.h'), 'utf8');
const GENS = {};
for (const m of h.matchAll(/\{"([a-z0-9+-]+)",(\d+),(true|false)\}/g)) if (m[3] === 'false') GENS[m[1]] = Number(m[2]);
const NOW = Number((h.match(/constexpr int kModeGenNow = (\d+);/) || [])[1]);
const OLD = Object.keys(GENS).filter((id) => GENS[id] <= 80);
const ADSB_LIST = [...OLD.filter((id) => id !== 'adsb'), 'spots-ft8', 'spots-wspr', 'spotsband-ft4'].join(',');   // + Band words
const NEWER = Object.keys(GENS).filter((id) => GENS[id] > 80);
const ADMIN = 'paul-shape-admin', ADSB = 'ADSB0001';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-paultabs-'));
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const P = await freePort();
const cfgPath = path.join(dir, 'config.json');
const base = { enabled: true, configured: true, users: 1, rate: 2400000, freq: 100000000, demodMode: 'wfm' };
const RADIOS = [
  { ...base, serial: '0000000001', driver: 'sdrplay', label: '3,5m FM Pirates', mode: 'locked', lockFreq: 85000000, users: 10,
    blockedModes: 'dab,drm,adsb,ais,ism,ndb,usb,lsb,cwu,cwl', order: 0 },
  { ...base, serial: 'ICOM-A', driver: 'icom', label: 'Icom one', blockedModes: '', order: 1 },
  { ...base, serial: 'ICOM-B', driver: 'icom', label: 'Icom two', blockedModes: '', order: 2 },
  { ...base, serial: '31A0B7C', driver: 'usrp', label: 'USRP', blockedModes: '', order: 3 },
  { ...base, serial: ADSB, driver: 'rtlsdr', label: 'ADS-B', mode: 'locked', lockFreq: 1090000000, freq: 1090000000, rate: 2560000,
    landingAdsb: true, adsbKeepRunning: true, adsb256: true, rateLock: true, blockedModes: ADSB_LIST + ',avdata+text,lora+all', order: 4 },
  { ...base, serial: 'HACKRF1', driver: 'hackrf', label: 'HackRF', blockedModes: '', order: 5 },
  { ...base, serial: '0000000001_3-9', driver: 'sdrplay', label: 'RSP second', blockedModes: '', order: 6 },
];
fs.writeFileSync(cfgPath, JSON.stringify({ configured: true, fullMode: true, name: 'Seven radios', adminPass: ADMIN, port: P, radios: RADIOS }));
const saved = () => JSON.parse(fs.readFileSync(cfgPath, 'utf8')).radios;
const savedOf = (serial) => saved().find((r) => r.serial === serial) || {};

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
// ★ "ticked" = live and ticked: a box greyed under a switched-off parent (each digital-spots mode under Digital spots) is off
const ticked = () => js(`[...document.querySelectorAll('#modeBlockList input[data-mode]:not([data-on])')].filter(b => b.checked && !b.disabled).map(b => b.dataset.mode).sort().join(",")`);
const drawn = () => js(`[...document.querySelectorAll('#modeBlockList input[data-mode]:not([data-on])')].map(b => b.dataset.mode)`);
async function openSetup() {
  await call('Page.navigate', { url: `http://127.0.0.1:${P}/setup` });
  ok(await until(async () => await js(`(() => { const c = document.getElementById("signinCard"); return !!c && c.offsetParent !== null; })()`), 10000), 'the setup page asks to sign in');
  await js(`document.getElementById("pass").value = ${JSON.stringify(ADMIN)}; document.getElementById("signinBtn").click(); 1`);
  ok(await until(async () => (await js(`document.querySelectorAll("#radioTabs button").length`)) === 8, 10000), 'eight tabs: SERVER and the seven radios');
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

  console.log('── an all-ticked radio first, then the ADS-B radio ──');
  ok(await tab('HackRF'), 'the HackRF tab (nothing blocked)');
  ok(await until(async () => (await ticked()).split(',').includes('wfm')), 'HackRF: WFM ticked (its list is empty)');
  ok(await tab('ADS-B'), 'the ADS-B tab (the fifth entry)');
  ok(await until(async () => (await js(`radio().serial`)) === ADSB), 'the page is on the ADS-B radio (by serial)');
  ok(await until(async () => (await ticked()) === 'adsb', 10000), `★★★ every box but ADS-B is unticked (ticked: ${await ticked()})`);
  const shown = await drawn() || [];
  ok(shown.includes('wfm') && shown.includes('dab') && shown.length > 20, `the list is drawn for the locked radio (${shown.length} switches)`);
  for (const nid of NEWER.filter((x) => shown.includes(x)))
    ok(await js(`(() => { const b = document.querySelector('input[data-mode="${nid}"]'); const w = b && b.parentElement.querySelector('[data-region-why]'); return !!b && !b.checked && !!w && /NEW \\(off until you tick it\\)/.test(w.textContent); })()`),
       `new switch ${nid}: unticked and marked NEW`);
  ok(await js(`!document.getElementById("newModesNote").classList.contains("hide") && /This update added/.test(document.getElementById("newModesNote").textContent)`),
     'the one-line notice: "This update added …"');
  ok(/ADS-B only/.test(await js(`document.getElementById("dabOnlyModesNote").textContent`) || '')
     && await js(`!document.getElementById("dabOnlyModesNote").classList.contains("hide")`), 'the only-mode note: ADS-B only');
  ok(await js(`document.getElementById("newModesPolOff").checked && document.getElementById("newModesPolOn").disabled`),
     '"When an update adds new modes": off, and fixed (a single-mode radio)');

  console.log('── All on / All off change the boxes only ──');
  const m0 = fs.statSync(cfgPath).mtimeMs, before = radio0 => radio0;
  await js(`document.getElementById("modeAllOn").click(); 1`);
  ok(await until(async () => (await js(`[...document.querySelectorAll('#modeBlockList input[data-mode]:not([data-on])')].every(b => b.checked)`))), 'All on: every drawn switch ticked');
  ok(await js(`!document.querySelector('input[data-mode="avdata+text"]') || document.querySelector('input[data-mode="avdata+text"]').checked === true`),
     'All on leaves the "+" opt-in (message text) exactly as it was');
  await js(`document.getElementById("modeAllOff").click(); 1`);
  ok(await until(async () => (await ticked()) === 'adsb'), `All off: only ADS-B (where new listeners start) left (ticked: ${await ticked()})`);
  ok(/except ADS-B/.test(await js(`document.getElementById("modeAllNote").textContent`) || ''), '…and it says so');
  ok(fs.statSync(cfgPath).mtimeMs === m0 && savedOf(ADSB).blockedModes === RADIOS[4].blockedModes, 'nothing saved by All on / All off');

  console.log('── Save, then reopen ──');
  await js(`document.getElementById("saveRadioBtn").click(); 1`);
  ok(await until(() => fs.statSync(cfgPath).mtimeMs > m0, 10000), 'saved through the page');
  const s1 = String(savedOf(ADSB).blockedModes || '').split(',');
  ok(s1.includes('seen@' + NOW), `the save records seen@${NOW}`);
  ok(NEWER.every((x) => s1.includes(x)) && !s1.includes('adsb'), 'every new switch saved OFF by name, ADS-B on');
  ok(s1.includes('avdata+text') && s1.includes('spots-ft8') && s1.includes('spotsband-ft4'), 'option and Band words kept');
  ok(savedOf('ICOM-A').blockedModes === '' && savedOf('USRP' ) && savedOf('31A0B7C').blockedModes === '', 'radios never opened: their lists untouched by the save');
  ok(savedOf('HACKRF1').blockedModes === 'seen@' + NOW, 'the HackRF (opened, nothing blocked): only "seen@" added — nothing switched off');
  await openSetup();
  ok(await tab('HackRF') && await tab('ADS-B'), 'reopened: HackRF, then ADS-B');
  ok(await until(async () => (await ticked()) === 'adsb', 10000), `reopened: still ADS-B only (ticked: ${await ticked()})`);
  ok(await js(`document.getElementById("newModesNote").classList.contains("hide")`), 'reopened: no notice (everything seen now)');

  // ★★★ beta96 (b96-keeprun, docs/v12/KEEP-RUNNING.md): Paul's ADS-B radio is SINGLE MODE (worked out from its ticks on
  //     first open, saved by the save above). Single mode decides, the ticks are underneath — the server enforces "everything
  //     but ADS-B" whatever they say — so every tick off there is NOT "no modes": it saves, and stays ADS-B single mode.
  //     The "no modes" refusal is driven on a General radio (the HackRF) instead.
  console.log('── never a radio with no modes ──');
  ok(String(savedOf(ADSB).singleMode || '') === 'adsb', `the ADS-B radio was saved as single mode: ADS-B (singleMode ${JSON.stringify(savedOf(ADSB).singleMode)})`);
  await js(`radio().blockedModes = Object.keys(MODE_GENS).join(","); modeBlockRender(); modeBlockSummary(); 1`);
  const m1 = fs.statSync(cfgPath).mtimeMs;
  await js(`document.getElementById("saveRadioBtn").click(); 1`);
  ok(await until(() => fs.statSync(cfgPath).mtimeMs > m1, 10000) && savedOf(ADSB).singleMode === 'adsb',
     'single mode (ADS-B) with every tick off: saved, still single mode ADS-B (the ticks are underneath)');
  ok(await tab('HackRF') && await until(async () => (await js(`radio().serial`)) === 'HACKRF1'), 'the HackRF tab (General)');
  await js(`radio().blockedModes = Object.keys(MODE_GENS).join(","); modeBlockRender(); modeBlockSummary(); 1`);
  const m2 = fs.statSync(cfgPath).mtimeMs;
  await js(`document.getElementById("saveRadioBtn").click(); 1`);
  await sleep(1200);
  ok(fs.statSync(cfgPath).mtimeMs === m2 && /every mode switched off/.test(await js(`document.getElementById("saveErr").textContent`) || ''),
     'a General radio with every mode off is not saved, and the page says why');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  clearInterval(guard); clearTimeout(deadline);
  stopAll();
  await sleep(500);
}
console.log(`\n${fail ? '✗' : '✓'} test-setup-paul-tabs: ${pass} passed, ${fail} failed`);
if (fail) console.log(`   (logs in ${dir})`); else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
