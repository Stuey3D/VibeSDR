// test-setup-mode-tree.mjs — THE MODES LIST AS A TREE (V12, 2026-10-31). Stuart: "all the decoders need to be able to be
// toggled and any options such as LoRA all or All station name scanning for DAB need to be options under the enable toggle
// for the main decoder itself". This drives the REAL setup page (in headless MUTED Edge) against a real vibeserver on a fake
// rtl_tcp and checks:
//   • the nesting: Advanced RDS under WFM, tone ID under NFM, the DAB label scan under DAB, every digital-spots mode and
//     Band under Digital spots, Band's mode-by-mode list under Band, LoRa under ISM, "every channel" under LoRa;
//   • a parent off greys its options WITH the reason, and never hides them;
//   • "LoRa: listen on every channel": ticking writes "lora+all"; unticking removes that token and nothing else; greyed
//     while "lora" is blocked; the hint says what unticked means;
//   • each new switch (drm, tone, adsb, spots-<mode>) round-trips through the CSV — page → config.json → page — and the
//     server enforces it (vibeserver.json);
//   • benchApplyDefaults with a fake BENCH: dec_lora_all GREEN ticks "every channel" on with a note; not green leaves it
//     off with "measured X %"; the advice says "Can be enabled" when green.
// ★ SILENT: no audio is played (--mute-audio, fake radio, no listener). Exit 3 = not run.
//   usage: VIBESERVER_BIN=… node scripts/test-setup-mode-tree.mjs
import net from 'node:net';
import crypto from 'node:crypto';
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

const ADMIN = 'tree-admin-pass', SERIAL = 'tree-rtl';
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-modetree-'));
// ★ The browser profile (~0.5 GB) never outlives the run — a failed or timed-out run keeps its logs, not Edge's cache
//   (53 kept waterfall-history runs held 25 GB of /var/folders, 2026-10-31).
process.on('exit', () => { try { fs.rmSync(path.join(dir, 'edge'), { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); } catch {} });
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
let P = 0;
for (let i = 0; i < 20 && !P; i++) {
  const p = await freePort();
  const free = await Promise.all([p + 1].map((q) => new Promise((r) => { const s = net.createServer(); s.once('error', () => r(false)); s.listen(q, '127.0.0.1', () => s.close(() => r(true))); })));
  if (free.every(Boolean)) P = p;
}
const cfgPath = path.join(dir, 'config.json');
fs.writeFileSync(cfgPath, JSON.stringify({
  configured: true, fullMode: true, name: 'Modes tree test', adminPass: ADMIN, port: P,
  radios: [{ serial: SERIAL, driver: 'rtlsdr', label: 'RTL', enabled: true, configured: true, users: 1, rate: 2400000,
             freq: 100000000, demodMode: 'wfm', blockedModes: 'spotsband+wspr' }],
}));
// ★★ EVERY PORT DISTINCT. freePort() can hand back P+1 (the radio's own port) — and a radio told to read rtl_tcp from its
//    own HTTP port feeds on itself (the first run of this test grew to 17 GB before it was killed).
const taken = new Set([P, P + 1]);
const distinctPort = async () => { for (;;) { const q = await freePort(); if (!taken.has(q)) { taken.add(q); return q; } } };
const rtlPort = await distinctPort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const env = { ...process.env, VIBESERVER_NO_RADIO_SPAWN: '1', VIBESERVER_CONFIG: cfgPath,
              VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), HOME: dir };
const doorLog = path.join(dir, 'door.log');
// ★ ONE descriptor for stdout and stderr: two (a 'w' and an 'a') overwrite each other's start-up lines
const doorFd = fs.openSync(doorLog, 'w');
const door = spawn(BIN, [], { env, stdio: ['ignore', doorFd, doorFd] });
let radio = null, radioN = 0, radioLog = '';
const startRadio = () => {
  radioLog = path.join(dir, `radio-${++radioN}.log`);
  const fd = fs.openSync(radioLog, 'w');
  radio = spawn(BIN, ['--radio-serial', SERIAL, '--tcp', `127.0.0.1:${rtlPort}`], { env, stdio: ['ignore', fd, fd] });
};
const stopRadio = async () => { if (!radio) return; radio.kill('SIGTERM'); for (let i = 0; i < 100 && radio.exitCode === null && radio.signalCode === null; i++) await sleep(50); if (radio.exitCode === null && radio.signalCode === null) radio.kill('SIGKILL'); radio = null; };
/** ★ The radio's log, BOUNDED: its first 64 KB (start-up lines) and last 192 KB — never the whole of a growing file. */
const R = () => {
  try {
    const fd = fs.openSync(radioLog, 'r'); const size = fs.fstatSync(fd).size;
    const head = Buffer.alloc(Math.min(size, 65536)); fs.readSync(fd, head, 0, head.length, 0);
    const tl = Math.min(Math.max(0, size - head.length), 196608); const tail = Buffer.alloc(tl); if (tl) fs.readSync(fd, tail, 0, tl, size - tl);
    fs.closeSync(fd); return head.toString() + '\n' + tail.toString();
  } catch { return ''; }
};
const up = async (port) => { for (let i = 0; i < 100; i++) { if (await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); })) return true; await sleep(100); } return false; };
/** The radio's blocked-modes CSV as saved in config.json, as a sorted list. */
const savedCsv = () => { const r = JSON.parse(fs.readFileSync(cfgPath, 'utf8')).radios.find((x) => x.serial === SERIAL) || {};
                         return String(r.blockedModes || '').split(',').filter(Boolean).sort().join(','); };

const CDP = await distinctPort();
const br = spawn(EDGE, ['--headless=new', '--mute-audio', '--disable-audio-output', '--no-first-run', `--remote-debugging-port=${CDP}`,
                        `--user-data-dir=${path.join(dir, 'edge')}`, '--window-size=1280,900', 'about:blank'], { stdio: 'ignore' });
const stopAll = () => { for (const p of [door, radio, rtl, br]) { try { p && p.kill('SIGKILL'); } catch {} } };
process.on('exit', stopAll);
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => { stopAll(); process.exit(1); });
/* ★★★ BOUNDED, WHATEVER GOES WRONG: a hard deadline for the whole test, and a memory watch over THIS process and every
 *  child (server, fake radio, browser) — over the limit, everything is killed and the test fails. */
let peakSelf = 0, peakChild = 0;
const abort = (why) => { console.log(`   FAIL aborted — ${why}`); stopAll(); process.exit(1); };
const guard = setInterval(() => {
  const rss = process.memoryUsage().rss; peakSelf = Math.max(peakSelf, rss);
  if (rss > (1 << 30)) abort(`the test itself reached ${(rss / 1048576).toFixed(0)} MB`);
  const pids = [door, radio, rtl, br].filter((p) => p && p.pid).map((p) => p.pid).join(',');
  const ps = spawn('ps', ['-o', 'rss=', '-p', pids]); let out = '';
  ps.stdout.on('data', (d) => { out += d; });
  ps.on('close', () => { const kb = Math.max(0, ...out.split(/\s+/).filter(Boolean).map(Number)); peakChild = Math.max(peakChild, kb * 1024);
                         if (kb > 2 * 1048576) abort(`a child process reached ${(kb / 1024).toFixed(0)} MB`); });
}, 1000);
const deadline = setTimeout(() => abort('the 240 s deadline passed'), 240000);

let call, js;
const csv = async () => (await js(`[...modeBlockSet()].sort().join(",")`)) ?? '';
const box = (id) => `document.querySelector('input[data-mode="${id}"]')`;
const click = (id) => js(`(() => { const b = ${box(id)}; if (!b || b.disabled) return false; b.click(); return true; })()`);
async function openSetup() {
  await call('Page.navigate', { url: `http://127.0.0.1:${P}/setup` });
  ok(await until(async () => await js(`(() => { const c = document.getElementById("signinCard"); return !!c && c.offsetParent !== null; })()`), 10000), 'the setup page asks to sign in');
  await js(`document.getElementById("pass").value = ${JSON.stringify(ADMIN)}; document.getElementById("signinBtn").click(); 1`);
  await until(async () => await js(`document.querySelectorAll("#radioTabs button").length > 0`), 10000);
  await js(`(() => { const b = [...document.querySelectorAll("#radioTabs button")].find(x => /RTL/.test(x.textContent)); b && b.click(); return 1; })()`);
  ok(await until(async () => await js(`!!${box('lora+all')}`), 15000), 'signed in: the modes list is drawn');
}

try {
  ok(await up(P), `the front door is up (${P})`);
  startRadio();
  ok(await up(P + 1), `the radio's own process is up (${P + 1})`);
  let t; for (let i = 0; i < 60 && !t; i++) { try { t = (await (await fetch(`http://127.0.0.1:${CDP}/json`)).json()).find((x) => x.type === 'page'); } catch { await sleep(250); } }
  const cws = new WebSocket(t.webSocketDebuggerUrl); await new Promise((r) => { cws.onopen = r; });
  let id = 0; const pend = new Map(); cws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id && pend.has(m.id)) { pend.get(m.id)(m); pend.delete(m.id); } };
  call = (method, params = {}) => new Promise((r) => { const n = ++id; pend.set(n, r); cws.send(JSON.stringify({ id: n, method, params })); setTimeout(() => { if (pend.delete(n)) r({}); }, 10000); });
  js = async (e) => (await call('Runtime.evaluate', { expression: e, awaitPromise: true, returnByValue: true })).result?.result?.value;
  await call('Runtime.enable');
  await openSetup();

  console.log('── the nesting ──');
  const under = async (parent, sel) => js(`!!document.querySelector('[data-kids-of="${parent}"] ${sel}')`);
  for (const [p, sel, what] of [['wfm', 'input[data-mode="rds"]', 'Advanced RDS under WFM'], ['nfm', 'input[data-mode="tone"]', 'CTCSS / DCS tone ID under NFM'],
                                ['dab', '#dabScanRow', "DAB's station-label scan under DAB (moved from the hardware card)"],
                                ['spots', 'input[data-mode="spotsband"]', 'Band under Digital spots'], ['spotsband', '#bandModeWrap', "Band's mode-by-mode list under Band"],
                                ['spots', 'input[data-mode="spots-q65"]', 'each digital-spots mode under Digital spots (Q65 …)'],
                                ['ism', 'input[data-mode="lora"]', 'LoRa under ISM'], ['lora', 'input[data-mode="lora+all"]', '"every channel" under LoRa']])
    ok(await under(p, sel), what);
  ok(await js(`document.querySelectorAll('#radioHwCard #dabScanRow').length === 0`), 'the label scan is no longer on the hardware card (one place only)');
  const spotModes = await js(`SPOT_MODES.slice()`) || [];
  ok(spotModes.length >= 8 && await js(`SPOT_MODES.every(m => !!document.querySelector('[data-kids-of="spots"] input[data-mode="spots-' + m.toLowerCase() + '"]'))`),
     `every mode the server runs has its switch (${spotModes.join(', ')})`);
  for (const sw of ['drm', 'adsb', 'cw', 'psk', 'same', 'selcal', 'mfsk', 'hell', 'jtty', 'ais', 'aprs', 'ndb', 'dsc', 'sitora', 'time'])
    ok(await js(`!!${box(sw)}`), `"${sw}" has its own switch`);
  ok(/default channels/.test(await js(`document.getElementById("loraAllHint").textContent`) || ''), 'the hint says what unticked means (the default channels still decode)');

  console.log('── LoRa: listen on every channel ──');
  ok((await csv()) === 'spotsband+wspr' && await js(`${box('lora+all')}.checked === false`), 'starts unticked; the CSV holds only the Band word it was given');
  await click('lora+all');
  ok((await csv()) === 'lora+all,spotsband+wspr', `ticking writes "lora+all" (${await csv()})`);
  await click('nfm');   // another token beside it
  await click('lora+all');
  ok((await csv()) === 'nfm,spotsband+wspr', `unticking removes "lora+all" and nothing else (${await csv()})`);
  await click('nfm');
  await click('lora+all');
  await click('lora');   // LoRa off
  ok((await csv()) === 'lora,lora+all,spotsband+wspr', 'LoRa off: "lora" joins the list, "lora+all" stays as the owner left it');
  ok(await js(`${box('lora+all')}.disabled && ${box('lora+all')}.offsetParent !== null`), '…and "every channel" is GREYED, not hidden');
  ok(/Off while LoRa \/ Meshtastic \/ MeshCore is off/.test(await js(`document.querySelector('[data-kids-of="lora"] .modeKidsWhy').textContent`) || ''),
     '…with the reason beside it');
  await click('lora');
  await click('ism');
  ok(await js(`${box('lora')}.disabled && ${box('lora+all')}.disabled`), 'ISM off greys LoRa and "every channel" with it');
  ok(await js(`document.querySelector('[data-kids-of="lora"] .modeKidsWhy').classList.contains("hide")
              && !document.querySelector('[data-kids-of="ism"] .modeKidsWhy').classList.contains("hide")`), '…the reason said once, at ISM');
  await click('ism'); await click('lora+all');
  ok(await js(`!${box('lora+all')}.disabled`) && (await csv()) === 'spotsband+wspr', 'switched back: everything live again, the CSV as it started');

  console.log('── a parent off greys the rest the same way ──');
  await click('spots');
  ok(await js(`${box('spotsband')}.disabled && ${box('spots-q65')}.disabled && [...document.querySelectorAll("#bandModeList select")].every(s => s.disabled)
              && document.getElementById("bandModeWrap").offsetParent !== null`), 'Digital spots off: Band, every mode and Band\'s per-mode list greyed, still shown');
  await click('spots');
  await click('wfm');
  ok(await js(`${box('rds')}.disabled && ${box('rds')}.offsetParent !== null`), 'WFM off: Advanced RDS greyed, still shown');
  await click('wfm');
  await click('dab');
  ok(await js(`[...document.querySelectorAll("#dabScanRow select")].every(s => s.disabled)`), 'DAB off: its label scan greyed');
  await click('dab');

  console.log('── every new switch round-trips: page → config.json → page, and the server enforces it ──');
  const NEW = ['adsb', 'drm', 'spots-fst4', 'spots-msk144', 'spots-q65', 'tone'];
  for (const sw of NEW) await click(sw);
  ok((await csv()) === [...NEW, 'spotsband+wspr'].sort().join(','), `unticked, each joins the CSV (${await csv()})`);
  const before = fs.statSync(cfgPath).mtimeMs;
  const visible = await js(`(() => { const b = document.getElementById("saveRadioBtn"); return !!b && !b.classList.contains("hide") && !b.disabled; })()`);
  await js(`document.getElementById(${JSON.stringify(visible ? 'saveRadioBtn' : 'saveBtn')}).click(); 1`);
  ok(await until(() => fs.statSync(cfgPath).mtimeMs > before, 10000), 'saved through the page');
  ok(await until(() => savedCsv() === [...NEW, 'spotsband+wspr'].sort().join(','), 5000), `config.json holds them (${savedCsv()})`);
  await stopRadio(); startRadio(); ok(await up(P + 1), 'the radio restarts on the saved list');
  await sleep(1500);
  const vj = await (await fetch(`http://127.0.0.1:${P + 1}/vibeserver.json`)).json().catch(() => ({}));
  ok(Array.isArray(vj.modes) && !vj.modes.includes('drm') && vj.adsb !== true, `the server enforces them: no "drm" in its modes, ADS-B not offered (adsb ${vj.adsb})`);
  ok(Array.isArray(vj.loraChannels) && vj.loraChannels.some((c) => c.band === '868' && c.plan === 16 && c.fit > c.defaults),
     `vibeserver.json says how many LoRa channels run (${JSON.stringify(vj.loraChannels)})`);
  await openSetup();
  ok(await js(`${JSON.stringify(NEW)}.every(id => ${'document.querySelector(\'input[data-mode="\' + id + \'"]\')'}.checked === false)`), 'reloaded: each shows unticked');
  for (const sw of NEW) await click(sw);
  ok((await csv()) === 'spotsband+wspr', 'ticked again: each leaves the CSV');

  console.log('── the benchmark sets the default (a fake BENCH, a fresh setup) ──');
  const bench = (grade, pct) => JSON.stringify({ v: 1, at: 1, rows: [{ id: 'dec_lora_all', label: 'Decoder: LoRa (868, every channel)', pct, grade, rate: 48000, threads: {} }] });
  await js(`cfg.configured = false; renderBench(${bench('green', 41)}); 1`);
  ok(await js(`${box('lora+all')}.checked`) && (await csv()).includes('lora+all'), 'GREEN: "every channel" is ticked on');
  ok(/measured 41% of a core here at its busiest thread, so it was turned on/.test(await js(`${box('lora+all')}.parentElement.textContent`) || ''), '…with the note beside it');
  ok(/Can be enabled: LoRa on every channel/.test(await js(`document.getElementById("benchAdvice").textContent`) || ''), '…and the advice says it can be enabled');
  await openSetup();   // a fresh page, the token as the green run left it (not saved): start again from the saved list
  await js(`cfg.configured = false; renderBench(${bench('amber', 82)}); 1`);
  ok(await js(`!${box('lora+all')}.checked`) && !(await csv()).includes('lora+all'), 'AMBER: "every channel" stays off');
  ok(/measured 82% of a core here at its busiest thread\. Tick it to use it anyway/.test(await js(`${box('lora+all')}.parentElement.textContent`) || ''), '…with "measured 82 %" and how to use it anyway');
  ok(await js(`!${box('lora+all')}.disabled`), '…and the box is still there to tick (never hidden)');
  await js(`cfg.configured = true; renderBench(${bench('green', 41)}); 1`);
  ok(!(await csv()).includes('lora+all'), 'an already set-up box is never changed by a re-run');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  clearInterval(guard); clearTimeout(deadline);
  stopAll();
  await sleep(500);
}
console.log(`\n${fail ? '✗' : '✓'} test-setup-mode-tree: ${pass} passed, ${fail} failed (peak RSS: test ${(peakSelf / 1048576).toFixed(0)} MB, children ${(peakChild / 1048576).toFixed(0)} MB)`);
if (fail) console.log(`   (logs in ${dir})`); else fs.rmSync(dir, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
