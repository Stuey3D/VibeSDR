// test-web-vfo-ab.mjs — V12 VFO A/B in the WEB client against the REAL server (vibe_watch.h; web/client/src/vfoAB.ts), in
// headless Edge with --mute-audio (tests are silent), at desktop and 390 px:
//   • the TUNING menu (the step button's menu) carries the VFO A/B toggle — on sends {watch,on}, off sends {watch,off};
//   • while on, the A/B KEY sits between the tune buttons (the radio glyph steps aside), labelled with the selected VFO,
//     lit in its colour; pressing it switches; V on the keyboard switches; a tap ON the other needle switches;
//   • with B selected the tune buttons move B (watch messages) and not A; B is drawn as a second needle, the active one
//     full brightness and the other at 75 % after a ~200 ms cross-fade; the needle labels carry L / R by frequency;
//   • the AUDIO panel's STEREO switch sends stereo:false and the L/R tags go;
//   • no toggle and no stereo row on a server whose owner switched VFO A/B off (hwinfo watch:0);
//   • ROUND 3: with B on the AUDIO panel has a squelch row for EACH VFO (labelled, coloured) — B's row hidden with B off;
//     each row's MUTE sends its own watch message and the server says so; B's bar sets B's squelch (a watch sql) and
//     never A's; B's AUTO SQUELCH sends B's threshold; the A row's bar stays A's whichever VFO is selected; starting a
//     decoder with B selected switches the controls to A (the decoders are VFO A's).
// Screenshots to $VFOAB_SHOTS (default $TMPDIR) as web-*.png.
//   VIBESERVER_BIN=…/vibeserver node scripts/test-web-vfo-ab.mjs
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const EDGE = '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge';
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(EDGE)) { console.log('   not run — VIBESERVER_BIN and Edge needed'); process.exit(3); }
const SHOTS = process.env.VFOAB_SHOTS || os.tmpdir();
fs.mkdirSync(SHOTS, { recursive: true });
const CENTRE = 7000000;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// ★ Every message the page sends on any socket, recorded before the page's own scripts run.
const SNIFF = `(() => { window.__sent = []; const o = WebSocket.prototype.send;
  WebSocket.prototype.send = function (d) { try { if (typeof d === 'string') window.__sent.push(d); } catch {} return o.apply(this, arguments); }; })();`;

async function session(tag, radioExtra, run) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), `vs-webab-${tag}-`));
  for (const d of ['data', 'run', 'edge']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'VFO A/B web test', locator: 'IO92nh', adminPass: 'vfoab-web-admin',
    radios: [{ serial: `${tag}-web`, label: 'Test radio', enabled: true, configured: true, users: 1, ...radioExtra }] }));
  const rtlPort = await freePort(), port = await freePort(), dbg = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--tones', '3', '--tones-at', String(CENTRE + 30000)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', String(CENTRE - 90000), '--mode', 'am', '--users', '1'],
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
    const shot = async (name) => { const s = await send('Page.captureScreenshot', { format: 'png' }); const f = path.join(SHOTS, `web-${name}.png`); fs.writeFileSync(f, Buffer.from(s.result.data, 'base64')); return f; };
    const until = async (x, ms) => { for (let i = 0; i < ms / 200; i++) { if (await ev(x)) return true; await sleep(200); } return false; };
    const click = async (sel) => {
      const r = JSON.parse(await ev(`JSON.stringify(document.querySelector(${JSON.stringify(sel)})?.getBoundingClientRect() || null)`) || 'null');
      if (!r) return false;
      const x = r.x + r.width / 2, y = r.y + r.height / 2;
      await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', clickCount: 1 });
      await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', clickCount: 1 });
      return true;
    };
    await send('Page.enable'); await send('Runtime.enable');
    await send('Page.addScriptToEvaluateOnNewDocument', { source: SNIFF });
    await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
    await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
    await until(`!![...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim()))`, 15000);
    await ev(`(() => { const b = [...document.querySelectorAll('button')].find(x => /^START$/i.test(x.textContent.trim())); b && b.click(); return !!b; })()`);
    await until(`!!document.querySelector('#audioGate button')`, 15000);
    await click('#audioGate button');
    ok(await until(`!document.getElementById('audioGate') || document.getElementById('audioGate').style.display === 'none' || !document.getElementById('audioGate').isConnected`, 8000), `${tag}: START RADIO pressed`);
    await sleep(2500);
    await run({ ev, send, shot, until, click });
  } catch (e) { ok(false, `${tag} driver: ${e && e.stack || e}`); }
  finally {
    try { wsC?.close(); } catch {}
    edge.kill(); srv.kill('SIGTERM'); rtl.kill('SIGTERM');
    await sleep(600);
    if (fail === failBefore) fs.rmSync(dir, { recursive: true, force: true }); else console.log(`   (kept ${dir})`);
  }
}
const AB = `window.__vibeAB()`;
const sentWatch = `window.__sent.filter(s => s.includes('"type":"watch"')).map(s => JSON.parse(s))`;

console.log('── VFO A/B in the web client ──');
await session('on', {}, async ({ ev, send, shot, until, click }) => {
  ok(await until(`${AB} && ${AB}.max === 1`, 8000), `hwinfo offers VFO A/B (max ${await ev(`${AB}?.max`)})`);
  ok(await ev(`document.getElementById('mAbKey').hidden && !document.getElementById('mTuneGlyph').hidden`), 'off: no A/B key, the radio glyph between the tune buttons');
  // ── on, from the tuning menu
  await click('#mStep');
  ok(await until(`document.getElementById('abToggle')?.textContent === 'OFF'`, 3000), 'the tuning (step) menu carries VFO A/B: OFF');
  await shot('step-menu');
  await click('#abToggle');
  ok(await until(`${sentWatch}.some(m => m.on === true && m.id === 1)`, 3000), `ON sends {watch, id:1, on:true} (${JSON.stringify(await ev(`${sentWatch}[0]`))})`);
  ok(await until(`${AB}.state && ${AB}.state.chans.length === 1`, 4000), 'the server reports VFO B');
  ok(await ev(`!document.getElementById('mAbKey').hidden && document.getElementById('mAbKey').textContent === 'A' && document.getElementById('mTuneGlyph').hidden`),
     'the A/B key appears between the tune buttons, labelled A; the glyph steps aside');
  const aHz = await ev(`${AB}.a.frequency`);
  // ── B selected: the key, the colour, the brightness
  await click('#mAbKey');
  ok(await until(`${AB}.sel === 'B' && document.getElementById('mAbKey').textContent === 'B'`, 2000), 'the A/B key switches to B and says so');
  // ★ Read after a painted frame (headless Edge paints only when asked): part-way through the 200 ms fade.
  const mid = await ev(`new Promise(r => setTimeout(() => requestAnimationFrame(() => requestAnimationFrame(() => r(${AB}.liveAlpha))), 50))`);
  await sleep(400);
  const end = await ev(`new Promise(r => requestAnimationFrame(() => requestAnimationFrame(() => r([${AB}.liveAlpha, ${AB}.other.alpha]))))`);
  ok(mid < 1 && mid >= 0.75 && end[0] === 1 && Math.abs(end[1] - 0.75) < 1e-6, `brightness cross-fades: active ${mid.toFixed(2)} → 1, the other → ${end[1]}`);
  const keyCol = await ev(`getComputedStyle(document.getElementById('mAbKey')).getPropertyValue('--abcol').trim()`);
  ok(keyCol === (await ev(`${AB}.liveColor`)) && keyCol !== '', `the key is lit in the selected VFO's colour (${keyCol})`);
  // ── tuning moves B, not A
  for (let i = 0; i < 5; i++) { await click('#mVfoUp'); await sleep(120); }
  await sleep(500);
  const ab = JSON.parse(await ev(`JSON.stringify(${AB})`));
  ok(ab.b.frequency > aHz && ab.a.frequency === aHz, `the tune buttons move B (${ab.b.frequency}) and leave A (${ab.a.frequency})`);
  ok(await until(`${sentWatch}.some(m => m.frequency === ${ab.b.frequency})`, 2000), 'as watch messages');
  ok(await until(`${AB}.state.chans[0].freq === ${ab.b.frequency}`, 3000), 'and the server has B there');
  ok(!(await ev(`window.__sent.some(s => s.includes('"type":"tune"') && s.includes('"frequency":${ab.b.frequency}'))`)), 'no tune message moved the dial');
  ok(/^B · R /.test(ab.liveLabel) && /^A · L /.test(ab.other.label), `needle labels by frequency: "${ab.liveLabel}" / "${ab.other.label}"`);
  ok(ab.other.col !== ab.liveColor, `two colours (A ${ab.other.col}, B ${ab.liveColor})`);
  await shot('ab-on-desktop');
  // ── a tap on the other needle selects it
  const ox = await ev(`${AB}.otherX`);
  const cr = JSON.parse(await ev(`JSON.stringify(document.getElementById('wf').getBoundingClientRect())`));
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x: cr.x + ox, y: cr.y + cr.height * 0.6, button: 'left', clickCount: 1 });
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: cr.x + ox, y: cr.y + cr.height * 0.6, button: 'left', clickCount: 1 });
  ok(await until(`${AB}.sel === 'A'`, 2000), 'a tap on A\'s needle selects A');
  ok((await ev(`${AB}.a.frequency`)) === aHz, '…without tuning anything');
  // ── V on the keyboard
  await send('Input.dispatchKeyEvent', { type: 'keyDown', key: 'v', text: 'v' });
  await send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'v' });
  ok(await until(`${AB}.sel === 'B'`, 2000), 'V swaps to B');
  // ── the audio panel's STEREO switch
  await click('#mAudio');
  ok(await until(`document.getElementById('abStereoRow') && !document.getElementById('abStereoRow').hidden && document.getElementById('abStereoBtn').textContent === 'ON'`, 3000),
     'the AUDIO panel has VFO A/B STEREO: ON');
  await shot('audio-stereo');
  await click('#abStereoBtn');
  ok(await until(`${sentWatch}.some(m => m.stereo === false)`, 2000), 'STEREO off sends {watch, stereo:false}');
  ok(await until(`${AB}.state.stereo === false && !/ · [LR]/.test(${AB}.liveLabel)`, 3000), `the server mixes both into both ears; the L/R tags go ("${await ev(`${AB}.liveLabel`)}")`);
  ok(await ev(`(() => { try { return JSON.parse(localStorage.getItem(Object.keys(localStorage).find(k => /pref/i.test(k)) || '{}').abStereo === false; } catch { return 'n/a'; } })()`) !== false,
     'remembered on this device');
  await click('#abStereoBtn');
  // ── ★★ ROUND 3: a squelch row for each VFO, each with its MUTE (B is selected here)
  ok(await ev(`!document.getElementById('sqlBlockB').hidden && !document.getElementById('sqlVfoA').hidden && !document.getElementById('sqlMuteA').hidden`),
     'with B on, the AUDIO panel has VFO A\'s squelch row AND VFO B\'s, each with a MUTE');
  ok(await ev(`getComputedStyle(document.getElementById('sqlVfoB')).getPropertyValue('--abcol').trim() === ${AB}.liveColor`), 'B\'s row is labelled in B\'s colour');
  await shot('audio-two-squelch');
  await click('#sqlMuteB');
  ok(await until(`${sentWatch}.some(m => m.id === 1 && m.mute === true)`, 2000), 'B\'s MUTE sends {watch, id:1, mute:true}');
  ok(await until(`${AB}.state.chans[0].mute === true && document.getElementById('sqlMuteB').textContent === 'MUTED'`, 3000), 'the server mutes B, and the key says MUTED');
  await click('#sqlMuteB');
  ok(await until(`${AB}.state.chans[0].mute === false`, 3000), 'pressed again: B unmuted');
  await click('#sqlMuteA');
  ok(await until(`${sentWatch}.some(m => m.muteA === true) && ${AB}.state.muteA === true`, 3000), 'A\'s MUTE sends {watch, muteA:true}; the server mutes A');
  await click('#sqlMuteA');
  ok(await until(`${AB}.state.muteA === false`, 3000), 'pressed again: A unmuted');
  const sqlA0 = await ev(`window.__sent.filter(s => s.includes('"type":"squelch"')).length`);
  await click('#sqlBarB');
  ok(await until(`${sentWatch}.some(m => m.id === 1 && typeof m.sql === 'number' && m.sqlOn === true)`, 2000), 'B\'s bar sets B\'s squelch (a watch sql)');
  ok(await until(`${AB}.state.chans[0].sqlOn === true`, 3000), 'the server has B\'s squelch on');
  ok((await ev(`window.__sent.filter(s => s.includes('"type":"squelch"')).length`)) === sqlA0, '…and A\'s squelch was not touched');
  await click('#sqlAutoB');
  ok(await until(`document.getElementById('sqlAutoB').classList.contains('on') && document.getElementById('sqlBarB').classList.contains('auto')`, 2000), 'B\'s AUTO SQUELCH is on, its bar in auto');
  const nB = await ev(`${sentWatch}.filter(m => m.id === 1 && 'sql' in m).length`);
  ok(await until(`${sentWatch}.filter(m => m.id === 1 && 'sql' in m).length > ${nB} || ${sentWatch}.some(m => m.id === 1 && m.sqlOn === false)`, 4000),
     'B\'s auto squelch drives B\'s threshold from B\'s own level');
  await click('#sqlAutoB');
  ok(await until(`${AB}.state.chans[0].sqlOn === false`, 3000), 'B\'s auto off: B\'s squelch back off');
  await click('#sqlBar');
  ok(await until(`window.__sent.filter(s => s.includes('"type":"squelch"')).length > ${sqlA0}`, 2000), 'A\'s bar sets A\'s squelch while B is the selected VFO');
  ok((await ev(`${AB}.state.chans[0].sqlOn`)) === false, '…and leaves B\'s alone');
  await ev(`document.getElementById('audioClose')?.click()`);
  // ── ★★ ROUND 3: the decoders are VFO A's — starting one with B selected switches the controls to A
  await click('#mAbKey');
  if ((await ev(`${AB}.sel`)) !== 'B') await click('#mAbKey');
  ok((await ev(`${AB}.sel`)) === 'B', 'B selected before the decoder');
  await ev(`document.querySelector('[data-dec="wefax"]')?.click()`);
  ok(await until(`${AB}.sel === 'A'`, 2000), 'starting a decoder (WEFAX) with B selected switches the controls to VFO A');
  await ev(`document.querySelector('[data-dec="wefax"]')?.click()`);
  await click('#mAbKey');
  ok(await until(`${AB}.sel === 'B'`, 2000), 'and the A/B key goes back to B to listen');
  // ── control colours: each background swatch, and solid
  const nSw = await ev(`document.querySelectorAll('#ctlBgSw > *').length`);
  for (let k = 0; k < Math.min(3, nSw || 0); k++) {
    await ev(`document.querySelectorAll('#ctlBgSw > *')[${k}].click()`);
    await sleep(500);
    await shot(`ab-on-skin${k}`);
  }
  await ev(`(() => { const s = document.getElementById('ctlSolid'); s.value = '100'; s.dispatchEvent(new Event('input', { bubbles: true })); })()`);
  await sleep(400);
  await shot('ab-on-solid');
  await ev(`(() => { const s = document.getElementById('ctlSolid'); s.value = '0'; s.dispatchEvent(new Event('input', { bubbles: true })); document.querySelectorAll('#ctlBgSw > *')[0]?.click(); })()`);
  // ── a phone
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await sleep(1200);
  const kr = JSON.parse(await ev(`JSON.stringify(document.getElementById('mAbKey').getBoundingClientRect())`));
  const dn = JSON.parse(await ev(`JSON.stringify(document.getElementById('mVfoDown').getBoundingClientRect())`));
  const up = JSON.parse(await ev(`JSON.stringify(document.getElementById('mVfoUp').getBoundingClientRect())`));
  ok(kr.width > 20 && kr.left >= dn.right - 1 && kr.right <= up.left + 1, `390 px: the key sits between ‹ and › (${Math.round(dn.right)} | ${Math.round(kr.left)}–${Math.round(kr.right)} | ${Math.round(up.left)})`);
  ok((await ev(`document.documentElement.scrollWidth`)) <= 390, 'no sideways scroll at 390 px');
  await shot('ab-on-phone');
  await send('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
  await sleep(600);
  // ── off, from the tuning menu
  await click('#mStep');
  ok(await until(`document.getElementById('abToggle')?.textContent === 'ON'`, 3000), 'the toggle reads ON');
  await click('#abToggle');
  ok(await until(`${sentWatch}.some(m => m.off === true)`, 2000), 'OFF sends {watch, id:1, off:true}');
  ok(await until(`!${AB}.on && ${AB}.sel === 'A' && document.getElementById('mAbKey').hidden && !document.getElementById('mTuneGlyph').hidden`, 3000), 'back to one VFO: A selected, key gone, glyph back');
  ok((await ev(`${AB}.a.frequency`)) === aHz, `A is where it was (${aHz})`);
  ok(await until(`${AB}.state && ${AB}.state.chans.length === 0`, 3000), 'the server has no VFO B');
  ok(await ev(`document.getElementById('sqlBlockB').hidden && document.getElementById('sqlMuteA').hidden && document.getElementById('sqlVfoA').hidden`),
     'one VFO: B\'s squelch row and the MUTE keys are gone — the squelch block is exactly as before');
});

console.log('── a server whose owner switched VFO A/B off ──');
await session('off', { vfoB: 0 }, async ({ ev, until, click }) => {
  await until(`${AB} && ${AB}.max === 0`, 5000);
  await sleep(800);
  ok((await ev(`${AB}.max`)) === 0, 'hwinfo says watch:0');
  await click('#mStep');
  await sleep(500);
  ok(await ev(`!!document.getElementById('stepMenu') && !document.getElementById('abToggle')`), 'no VFO A/B toggle in the tuning menu');
  ok(await ev(`document.getElementById('abStereoRow').hidden`), 'no STEREO row in the audio panel');
});
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
