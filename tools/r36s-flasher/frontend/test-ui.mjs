// test-ui.mjs — the flasher's window, driven in headless Edge with the Go side mocked: which JOB each flow sends.
// ★★★ Why: on the Update screen the backup choices were still visible ([hidden] lost to `.big { display:block }`) and
//     pressing one sent a MAKE job — Stuart's first Update test rebuilt his card from scratch (2026-11-14).
//   node tools/r36s-flasher/frontend/test-ui.mjs        (silent; no Wails, no card)
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const PAGE = new URL('./dist/index.html', import.meta.url).pathname;
const BROWSER = process.env.EDGE || '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!fs.existsSync(BROWSER)) { console.log('   not run — no Edge at ' + BROWSER); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const T = fs.mkdtempSync(path.join(os.tmpdir(), 'flasher-ui-'));
const cdpPort = await freePort();
const br = spawn(BROWSER, ['--headless=new', '--mute-audio', '--no-first-run', '--disable-extensions', `--remote-debugging-port=${cdpPort}`,
  `--user-data-dir=${path.join(T, 'browser')}`, '--window-size=800,900', 'about:blank'], { stdio: 'ignore' });

// The Go side, mocked: one VibeSDR card; Start records what it was asked.
const MOCK = `
  window.__starts = []; window.__h = {}; window.__phase = 'source';
  const card = { id: 'disk4', name: 'Built In SDXC Reader', size: 15.9e9, device: '/dev/disk4', looks: 'VibeSDR card', bus: 'SD', large: false };
  // ★ the new card in the SAME reader gets the same disk id — the window must wait for the original to go first
  const fresh = { id: 'disk4', name: 'Built In SDXC Reader', size: 31.9e9, device: '/dev/disk4', looks: 'other', volumes: ['EASYROMS'], bus: 'SD', large: false };
  const blankC = { id: 'disk5', name: 'USB Reader', size: 31.9e9, device: '/dev/disk5', looks: 'No readable volumes', volumes: [], bus: 'USB', large: false };
  const bks = [{ path: '/b/2026-11-15 1031 SD/2026-11-15 1031 SD.img.gz', name: '2026-11-15 1031 SD', kind: 'whole', bytes: 63.9e9, written: '12.0.0-beta78' }];
  window.go = { main: { App: {
    Backups: async () => bks,
    CheckDrivers: async (d) => { if (d.kind === 'own') throw 'my.dtb is not for an RK3326 (it is for GA36)'; return d.kind === 'generic' ? 'the generic ROCKNIX drivers built into VibeSDR (Generic EE clone)' : 'the R36S (EE clone) drivers from ' + d.file; },
    ChooseImageFile: async () => '/x/other.img.xz',
    ChooseDTB: async () => '/x/my.dtb',
    ChooseOverlays: async () => [],
    PreviewRestore: async (p) => ({ name: p.split('/').pop(), bytes: 63.9e9, whole: true, checked: true }),
    StartAdvanced: async (id, kind, folder, large, d) => { window.__starts.push('adv:' + id + ':' + kind + ':' + d.kind + (d.file ? ':' + d.file : '')); },
    StartRestore: async (id, kind, folder, large, p) => { window.__starts.push('restore:' + id + ':' + kind + ':' + p); },
    Rescan: async () => ({ flasher: '0.2.1-localtest', localTest: true, imageOk: true, version: '12.0.0-beta77+handheld73', imageErr: '' }),
    Disks: async () => window.__phase === 'source' ? [card] : window.__phase === 'out' ? [] : window.__phase === 'blank' ? [blankC] : [fresh],
    DefaultBackupDir: async () => '/Users/x/Documents/VibeSDR Backups/test',
    ChooseFolder: async (d) => d,
    Start: async (id, kind, folder, large) => { window.__starts.push(kind); },
    StartNew: async (id, kind, folder, large) => { window.__starts.push('new:' + kind); },
    WriteToNew: async (id, large) => { window.__starts.push('write:' + id); },
    CancelNew: async () => { window.__starts.push('cancel'); },
    RevealBackup: async () => {},
  } } };
  window.runtime = { EventsOn: (n, f) => { window.__h[n] = f; } };`;

let id = 0; const waiters = new Map(); let ws;
const send = (method, params = {}) => new Promise((res) => { const i = ++id; waiters.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
const ev = async (expr) => {
  const r = (await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })).result;
  if (r?.exceptionDetails) console.log('   (page error: ' + (r.exceptionDetails.exception?.description || r.exceptionDetails.text).split('\n')[0] + ')');
  return r?.result?.value;
};
try {
  let target;
  for (let i = 0; i < 40 && !target; i++) { try { target = (await (await fetch(`http://127.0.0.1:${cdpPort}/json`)).json()).find((t) => t.type === 'page'); } catch {} if (!target) await sleep(250); }
  ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => { ws.onopen = r; });
  ws.onmessage = (m) => { const d = JSON.parse(m.data); if (d.id && waiters.has(d.id)) { waiters.get(d.id)(d); waiters.delete(d.id); } };
  await send('Page.enable');
  await send('Page.addScriptToEvaluateOnNewDocument', { source: MOCK });

  const run = async (flow, press) => {
    await send('Page.navigate', { url: 'file://' + PAGE });
    await sleep(700);
    await ev(`document.getElementById('${flow === 'update' ? 'bUpdate' : 'bMake'}').click()`);
    await sleep(2300);                                                   // the card list polls every 2 s
    await ev(`document.querySelector('#diskList .big').click()`); await sleep(100);
    await ev(`document.getElementById('toBackup').click()`); await sleep(200);
    if (flow !== 'update') { await ev(`document.getElementById('whereNext').click()`); await sleep(200); }   // "This card"
    const visible = await ev(`[document.getElementById('kWhole'), document.getElementById('kBoot')].map((b) => b.offsetParent !== null)`);
    for (const k of press) await ev(`document.getElementById('${k}').click()`);
    await ev(`document.getElementById('start').click()`); await sleep(100);
    const text = await ev(`document.getElementById('confirmText').textContent + ' | ' + document.getElementById('cYes').textContent`);
    await ev(`document.getElementById('cYes').click()`); await sleep(200);
    return { visible, text, starts: await ev('window.__starts') };
  };

  let r = await run('update', ['kWhole', 'kBoot']);
  ok(r.visible && !r.visible[0] && !r.visible[1], `★ UPDATE: the backup choices are not on screen (visible: ${JSON.stringify(r.visible)})`);
  ok(JSON.stringify(r.starts) === '["update"]', `★★ UPDATE sends an update job even if the hidden choices are pressed (sent ${JSON.stringify(r.starts)})`);
  ok(/update my card/.test(r.text) && !/already a VibeSDR card/.test(r.text), 'UPDATE confirm: "update my card", no Make warning');

  r = await run('make', []);
  ok(r.visible && r.visible[0] && r.visible[1], 'MAKE: both backup choices are on screen');
  ok(JSON.stringify(r.starts) === '["whole"]', `MAKE sends "whole" by default (sent ${JSON.stringify(r.starts)})`);
  ok(/already a VibeSDR card/.test(r.text) && /whole-card backup/.test(r.text), '★ MAKE on a VibeSDR card warns, and says the whole-card backup keeps the recordings');

  // ★★ MAKE ON A NEW CARD — the original is only read; the window waits for it to come out; the new card is warned about
  await send('Page.navigate', { url: 'file://' + PAGE }); await sleep(700);
  await ev(`document.getElementById('bMake').click()`); await sleep(2300);
  ok(/current R36S system/.test(await ev(`document.getElementById('pickTitle').textContent`)), 'MAKE asks for the card with your current R36S system');
  await ev(`document.querySelector('#diskList .big').click()`); await sleep(100);
  await ev(`document.getElementById('toBackup').click()`); await sleep(200);
  ok(await ev(`document.getElementById('where').classList.contains('on')`), 'then asks where VibeSDR should go');
  await ev(`document.getElementById('wNew').click()`); await ev(`document.getElementById('whereNext').click()`); await sleep(200);
  await ev(`document.getElementById('kBoot').click()`); await ev(`document.getElementById('start').click()`); await sleep(100);
  let t = await ev(`document.getElementById('confirmText').textContent + ' | ' + document.getElementById('cYes').textContent`);
  ok(/It is NOT changed/.test(t) && /back up and read it/.test(t) && !/already a VibeSDR card/.test(t), 'new-card confirm: the original is backed up and read, NOT changed');
  await ev(`document.getElementById('cYes').click()`); await sleep(200);
  ok(JSON.stringify(await ev('window.__starts')) === '["new:boot"]', `★ the original goes to StartNew (read only) with its backup choice (${JSON.stringify(await ev('window.__starts'))})`);
  await ev(`window.__h.needTarget({ backup: '/b/x', summary: 'the card own device tree', sourceId: 'disk4' })`); await sleep(1800);
  ok(await ev(`document.getElementById('swap').classList.contains('on')`), 'then the swap screen');
  ok(/Take your original card out/.test(await ev(`document.getElementById('swapList').textContent`)), '★ it waits for the original to be taken out (same disk id stays the original)');
  await ev(`window.__phase = 'out'`); await sleep(1800);
  await ev(`window.__phase = 'new'`); await sleep(1800);
  const offered = await ev(`document.querySelectorAll('#swapList .big').length`);
  ok(offered === 1, 'then offers the card put in its place');
  await ev(`document.querySelector('#swapList .big').click()`); await sleep(100);
  await ev(`document.getElementById('swapNext').click()`); await sleep(100);
  t = await ev(`document.getElementById('confirmText').textContent + ' | ' + document.getElementById('cYes').textContent`);
  ok(/not empty/.test(t) && /EASYROMS/.test(t) && /NOT backed up/.test(t) && /write VibeSDR/.test(t), '★ warn, then write: names what is on the new card and that it is not backed up');
  await ev(`document.getElementById('cYes').click()`); await sleep(200);
  ok(JSON.stringify(await ev('window.__starts')) === '["new:boot","write:disk4"]', `★★ the write goes to the NEW card through WriteToNew (${JSON.stringify(await ev('window.__starts'))})`);

  // ★★ ADVANCED — generic drivers onto a BLANK card, no backup by default for a blank card
  const fresh2 = async () => { await send('Page.navigate', { url: 'file://' + PAGE }); await sleep(700); };
  const conf = () => ev(`document.getElementById('confirmText').textContent + ' | ' + document.getElementById('cYes').textContent`);
  await fresh2(); await ev(`window.__phase = 'blank'`);
  await ev(`document.getElementById('bAdv').click()`); await ev(`document.getElementById('aMake').click()`); await sleep(100);
  ok(await ev(`document.getElementById('advSrcNext').disabled`), 'ADVANCED: Next waits for a choice of drivers');
  await ev(`document.getElementById('dGeneric').click()`); await sleep(200);
  ok(!(await ev(`document.getElementById('advSrcNext').disabled`)) && /generic ROCKNIX/.test(await ev(`document.getElementById('dCheck').textContent`)), 'ADVANCED generic: checked, says what it uses, Next on');
  await ev(`document.getElementById('advSrcNext').click()`); await sleep(2300);
  await ev(`document.querySelector('#diskList .big').click()`); await sleep(100);
  await ev(`document.getElementById('toBackup').click()`); await sleep(200);
  ok(await ev(`document.getElementById('backup').classList.contains('on')`), 'ADVANCED goes straight from the card to the backup choice (no "where")');
  ok(await ev(`document.getElementById('kNone').offsetParent !== null && document.getElementById('kNone').classList.contains('sel')`), '★ a BLANK card: "Don\'t back up" offered and chosen');
  await ev(`document.getElementById('start').click()`); await sleep(100);
  t = await conf();
  ok(/Write VibeSDR/.test(await ev(`document.getElementById('confirmTitle').textContent`)) && /generic ROCKNIX/.test(t) && /not backed up first/.test(t) && !/NOT backed up/.test(t), 'ADVANCED confirm names the drivers; a blank card is not backed up, calmly');
  await ev(`document.getElementById('cYes').click()`); await sleep(200);
  ok(JSON.stringify(await ev('window.__starts')) === '["adv:disk5:none:generic"]', `★★ ADVANCED sends StartAdvanced with no backup and the generic drivers (${JSON.stringify(await ev('window.__starts'))})`);

  // ADVANCED — drivers from one of your backups, onto a VibeSDR card: whole backup by default, the VibeSDR warning
  await fresh2();
  await ev(`document.getElementById('bAdv').click()`); await ev(`document.getElementById('aMake').click()`); await sleep(100);
  await ev(`document.getElementById('dFile').click()`); await sleep(200);
  await ev(`document.querySelector('#dBackups .big').click()`); await sleep(300);
  ok(/EE clone/.test(await ev(`document.getElementById('dCheck').textContent`)), 'ADVANCED file: one of your backups, its drivers read');
  await ev(`document.getElementById('advSrcNext').click()`); await sleep(2300);
  await ev(`document.querySelector('#diskList .big').click()`); await sleep(100);
  await ev(`document.getElementById('toBackup').click()`); await sleep(200);
  ok(await ev(`document.getElementById('kWhole').classList.contains('sel')`), 'a card with something on it: Whole card backup by default');
  await ev(`document.getElementById('start').click()`); await sleep(100);
  t = await conf();
  ok(/VibeSDR card/.test(t) && /Update a VibeSDR card/.test(t) && /whole card is backed up/.test(t), 'ADVANCED onto a VibeSDR card: warns, points at Update, says it is backed up');
  await ev(`document.getElementById('cYes').click()`); await sleep(200);
  ok((await ev('window.__starts'))[0].startsWith('adv:disk4:whole:file:/b/'), `ADVANCED file → StartAdvanced with the backup's path (${JSON.stringify(await ev('window.__starts'))})`);

  // ADVANCED — own device tree refused: the reason shown, Next stays off
  await fresh2();
  await ev(`document.getElementById('bAdv').click()`); await ev(`document.getElementById('aMake').click()`); await sleep(100);
  await ev(`document.getElementById('dOwn').click()`); await ev(`document.getElementById('dPickDtb').click()`); await sleep(300);
  ok(/not for an RK3326/.test(await ev(`document.getElementById('dCheck').textContent`)) && await ev(`document.getElementById('advSrcNext').disabled`), '★ own .dtb refused: the reason shown, Next stays off');

  // RESTORE — a backup back on a card, with this card's boot partition backed up first
  await fresh2();
  await ev(`document.getElementById('bAdv').click()`); await ev(`document.getElementById('aRestore').click()`); await sleep(200);
  await ev(`document.querySelector('#rBackups .big').click()`); await sleep(300);
  ok(/a whole card/.test(await ev(`document.getElementById('rCheck').textContent`)) && !(await ev(`document.getElementById('advRestoreNext').disabled`)), 'RESTORE: your backup chosen and described');
  await ev(`document.getElementById('advRestoreNext').click()`); await sleep(2300);
  await ev(`document.querySelector('#diskList .big').click()`); await sleep(100);
  await ev(`document.getElementById('toBackup').click()`); await sleep(200);
  ok(/Put it back/.test(await ev(`document.getElementById('start').textContent`)), 'RESTORE: the button says Put it back');
  await ev(`document.getElementById('kBoot').click()`); await ev(`document.getElementById('start').click()`); await sleep(100);
  t = await conf();
  ok(/replaced by the backup/.test(t) && /put it back/.test(t), 'RESTORE confirm: the card is replaced by the backup');
  await ev(`document.getElementById('cYes').click()`); await sleep(200);
  ok((await ev('window.__starts'))[0].startsWith('restore:disk4:boot:/b/'), `★★ RESTORE sends StartRestore with the card's own backup choice (${JSON.stringify(await ev('window.__starts'))})`);

  // ★ SIMPLE MAKE never offers "Don't back up"
  r = await run('make', ['kNone']);
  ok(JSON.stringify(r.starts) === '["whole"]', `★ MAKE: "Don't back up" is Advanced only — pressing it changes nothing (sent ${JSON.stringify(r.starts)})`);

  r = await run('make', ['kBoot']);
  ok(JSON.stringify(r.starts) === '["boot"]', `MAKE with Boot partition only sends "boot" (sent ${JSON.stringify(r.starts)})`);
  ok(/does NOT keep them/.test(r.text) && /Update a VibeSDR card/.test(r.text), '★ MAKE + boot backup on a VibeSDR card: says the recordings are NOT kept, points at Update');
} finally {
  try { ws?.close(); } catch {}
  br.kill(); await sleep(300);
  fs.rmSync(T, { recursive: true, force: true });
}
console.log(fail ? `   ${fail} FAILED, ${pass} passed` : `   ${pass} passed, 0 failed`);
process.exit(fail ? 1 : 0);
