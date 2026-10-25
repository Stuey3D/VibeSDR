// test-server-ism-gallery.mjs — ISM IN THE GALLERY through the REAL server (V12, 2026-10-25). A fake rtl_tcp replays the
// synthetic 433 MHz clip (`test-ism --write-clip`: an Oregon THGR810, a WH1080, a TPMS twice, a fixed-code remote).
//   1. A LISTENER'S SESSION: ISM on 433 → the devices listed → ISM off. Then the gallery lists ISM (kind=ism): the day's
//      log and its summary card, labelled a listener's; the log's lines are "HH:MM:SS  433.xxx  <the list's line>",
//      exactly the lines the ISM list showed; nothing was written while the session ran (the RAM buffer); the download
//      is named "ISM 433.92 <date>.txt"; and NO TPMS ID or remote code is in any file, the index or the gallery JSON.
//   2. BACKGROUND DECODING with an ISM profile (433, all day, wait 0): nobody here → the radio goes onto the band in
//      ISM mode by itself; the admin line says "running" with the band and the day so far; a listener ARRIVES → it
//      stops, the listener lands on the radio's own dial (96.1 MHz WFM, not the band), and the gallery's 433 log and
//      card for the day are the SAME items as the listener's, now marked "both".
// Silent: no audio socket is opened.
//   VIBESERVER_BIN=…/vibeserver VIBE_ISM_CLIP=…/clip.u8 node scripts/test-server-ism-gallery.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const CLIP = process.env.VIBE_ISM_CLIP || '';
if (!BIN || !fs.existsSync(BIN) || !CLIP || !fs.existsSync(CLIP)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_ISM_CLIP (make one: test-ism --write-clip clip.u8 434130000 2400000)');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { if (await f()) return true; await sleep(100); } return !!(await f()); };
const ADMIN = 'ismgal-admin';
// ★ the clip's secrets, every way they could be written: the TPMS ID 0x11223344 and the remote's code 0xA5C3E8
const SECRETS = ['11223344', '287454020', 'A5C3E8', 'a5c3e8', 'A5C3E', 'a5c3e', '10863592', '678974'];

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-ismgal-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const gdir = path.join(dir, 'data', 'gallery');
const writeCfg = (radio) => fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'ISM gallery test', locator: 'IO92nh', adminPass: ADMIN,
  radios: [{ serial: 'ismgal', label: 'Test radio', enabled: true, configured: true, users: 1, rate: 2400000, ...radio }],
}));
const rtlPort = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2400000', '--ism', CLIP], { stdio: 'ignore' });
await sleep(400);
let srv = null, port = 0, logPath = '';
async function startServer(tag, envExtra = {}) {
  port = await freePort();
  logPath = path.join(dir, `server-${tag}.log`);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...envExtra };
  const log = fs.openSync(logPath, 'w');
  srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2400000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
              { env, stdio: ['ignore', log, log] });
  return until(async () => { try { return (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(2000) })).ok; } catch { return false; } }, 15000);
}
async function stopServer() {
  if (!srv) return;
  const p = srv; srv = null;
  const gone = new Promise((r) => p.once('exit', r));
  p.kill('SIGTERM');
  await Promise.race([gone, sleep(8000)]);
}
const L = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
const gallery = async (q = '') => (await fetch(`http://127.0.0.1:${port}/vibeserver/gallery.json?offset=0&limit=100${q}`)).json();
const fileOf = async (f, dl = false) => fetch(`http://127.0.0.1:${port}/vibeserver/gallery/file/${encodeURIComponent(f)}${dl ? '?download=1' : ''}`);
const adminQ = async () => { const n = (await fetch(`http://127.0.0.1:${port}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${n}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(n).digest('hex')}`; };
const bgStatus = async () => { try { return (await fetch(`http://127.0.0.1:${port}/vibeserver/admin/status?${await adminQ()}`).then((r) => r.json())).bgDecode; } catch { return null; } };
const everythingOnDisk = () => fs.existsSync(gdir) ? fs.readdirSync(gdir).map((f) => fs.readFileSync(path.join(gdir, f), 'utf8')).join('\n') : '';
function spectrum(id) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=${id}&bins=256&proto=1`);
  ws.binaryType = 'arraybuffer';
  const st = { last: null, cfg: null, off: false, msgs: [], ws };
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    st.msgs.push(e.data);
    const j = JSON.parse(e.data);
    if (j.type === 'ism') st.last = j;
    if (j.type === 'ism_off') st.off = true;
    if (j.type === 'config') st.cfg = j;
  };
  st.open = new Promise((r) => { ws.onopen = r; });
  return st;
}
const today = new Date().toISOString().slice(0, 10);

try {
  // ── 1. a listener's session ─────────────────────────────────────────────────────────────────────────────────────
  console.log('── a listener in ISM mode ──');
  writeCfg({});
  ok(await startServer('live'), 'the server is up');
  const g0 = await gallery();
  ok(g0.enabled === true && (g0.items || []).length === 0, `the gallery is on and empty (${(g0.items || []).length} items)`);
  const a = spectrum('ismgal01');
  await a.open;
  await sleep(1000);
  a.ws.send(JSON.stringify({ type: 'ism', on: 1, band: '433' }));
  const want = ['Oregon Scientific THGR810', 'Fine Offset WH1080', 'TPMS #1', 'Doorbell / remote #1'];
  const has = (s) => !!a.last && a.last.devices.some((d) => d.line.startsWith(s));
  ok(await until(() => want.every(has), 20000), `the list shows all four devices: ${a.last ? a.last.devices.map((d) => d.line).join(' | ') : '-'}`);
  const listLines = new Set();
  for (const m of a.msgs) { try { const j = JSON.parse(m); if (j.type === 'ism') for (const d of j.devices) listLines.add(d.line); } catch {} }
  const midG = await gallery('&kind=ism');
  ok((midG.items || []).length === 0, 'while the session runs, nothing is written yet (the readings are held in RAM)');
  await sleep(4000);    // ★ the clip loops every 3.4 s: more readings of the same devices
  a.ws.send(JSON.stringify({ type: 'ism', on: 0 }));
  ok(await until(() => a.off, 5000), 'ISM off');
  await sleep(800);
  const g1 = await gallery('&kind=ism');
  const items = g1.items || [];
  const log = items.find((i) => (i.s || '').startsWith('ismlog-433-')), card = items.find((i) => (i.s || '').startsWith('ismsum-433-'));
  ok(items.length === 2 && !!log && !!card, `the gallery lists ISM (kind=ism): ${items.map((i) => i.title).join(' | ')}`);
  ok(!!log && log.o === 'live' && log.k === 'ism' && Math.abs(log.hz - 433.92e6) < 1, `the log: a listener's, kind ism, 433.92 MHz (${log && log.o})`);
  ok(!!log && log.title === `ISM 433.92 MHz · ${today} · every reading`, `its title: ${log && log.title}`);
  const all = await gallery();
  ok((all.items || []).length === 2 && all.total === 2, 'ALL shows the same two items');
  const body = log ? await (await fileOf(log.f)).text() : '';
  const lines = body.split('\n').filter((l) => /^\d\d:\d\d:\d\d  /.test(l));
  ok(lines.length >= 4, `the log holds ${lines.length} reading lines`);
  ok(lines.every((l) => /^\d\d:\d\d:\d\d  43[34]\.\d{3}  \S/.test(l)), `every line is "HH:MM:SS  433.xxx  …": ${lines[0]}`);
  const oregonLine = lines.find((l) => l.includes('Oregon'));
  ok(!!oregonLine && /^\d\d:\d\d:\d\d  433\.9\d\d  Oregon Scientific THGR810 \/ THGN801 · 18\.4 °C · 62 % RH · battery OK · ch 2$/.test(oregonLine), `the Oregon line exactly as the list: ${oregonLine}`);
  ok(lines.every((l) => listLines.has(l.slice(19))), 'every logged reading is a line the ISM list showed');
  const oregonCount = lines.filter((l) => l.includes('Oregon')).length;
  ok(oregonCount >= 2, `the clip looped: the Oregon is logged ${oregonCount} times (each loop is a new reading, the repeats inside it are not)`);
  const dl = log ? await fileOf(log.f, true) : null;
  ok(!!dl && dl.headers.get('content-disposition') === `attachment; filename="ISM 433.92 ${today}.txt"`, `downloads as: ${dl && dl.headers.get('content-disposition')}`);
  const cardText = card ? await (await fileOf(card.f)).text() : '';
  ok(/4 devices: 2 weather, 1 TPMS, 1 doorbell \/ remote/.test(cardText), `the card: ${(cardText.match(/^\d+ devices?:.*$/m) || ['-'])[0]}`);
  ok(cardText.includes('Heard by a listener in ISM mode.') && /Most heard: {3}\S/.test(cardText), 'the card says who heard it, and the most heard');
  ok(/ · TPMS · TPMS #1 · type B · 32 PSI · 18 °C$/m.test(cardText), 'the card\'s last reading of the TPMS, by its nickname');
  const wire = JSON.stringify(g1) + JSON.stringify(all) + body + cardText + everythingOnDisk();
  const leaked = SECRETS.filter((x) => wire.includes(x));
  ok(leaked.length === 0, `no TPMS ID or remote code in the files, the index or the gallery JSON${leaked.length ? ' — found ' + leaked.join(',') : ''}`);
  a.ws.close();
  await stopServer();

  // ── 2. background decoding with an ISM profile ──────────────────────────────────────────────────────────────────
  console.log('── background decoding: an ISM profile ──');
  writeCfg({ bgDecode: JSON.stringify({ on: true, waitMin: 0, profiles: [{ centre: 0, from: '00:00', to: '00:00', vfos: [], ism: '433' }] }) });
  ok(await startServer('bg', { VIBESERVER_BG_WAIT_SEC: '1' }), 'the server is up again, with an ISM profile');
  ok(await until(() => /profile 1 puts the radio on the 433\.92 MHz ISM band/.test(L()), 15000), 'nobody here: background decoding puts the radio on the 433.92 band');
  ok(await until(() => /\[ISM\] mode ON 433\.92 MHz ISM/.test(L()), 5000), '…through ISM mode itself ("[ISM] mode ON")');
  let st = null;
  ok(await until(async () => { st = await bgStatus(); return !!st && st.state === 'running' && st.ism && st.ism.devices >= 4; }, 20000),
     `the admin line: ${st && st.state}, ISM ${st && st.ism && st.ism.name}, ${st && st.ism && st.ism.devices} devices, ${st && st.ism && st.ism.today}`);
  ok(!!st && !st.runner, 'no VFO runner for an ISM profile');
  await sleep(3500);
  const b = spectrum('ismgal02');
  await b.open;
  ok(await until(() => /background decoding stopped — a listener arrived \(ISM: the radio is back where it was\)/.test(L()), 5000), 'a listener arrives: background ISM stops');
  ok(await until(() => !!b.cfg, 5000) && Math.abs(b.cfg.vfo - 96.1e6) < 1 && b.cfg.mode === 'wfm', `…and the listener lands on the radio's own dial: ${b.cfg && b.cfg.vfo} ${b.cfg && b.cfg.mode}`);
  await sleep(1500);
  ok(!b.msgs.some((m) => m.includes('"type":"ism"')), 'the listener is not sent any ISM report (they are not in ISM)');
  const g2 = await gallery('&kind=ism');
  const it2 = g2.items || [];
  const log2 = it2.find((i) => (i.s || '').startsWith('ismlog-433-')), card2 = it2.find((i) => (i.s || '').startsWith('ismsum-433-'));
  ok(it2.length === 2 && log2 && log && log2.f === log.f && card2 && card && card2.f === card.f, 'the SAME day\'s log and card (one file per UTC day per band)');
  ok(log2 && log2.o === 'both' && card2 && card2.o === 'both', `both now marked "both" (${log2 && log2.o}, ${card2 && card2.o})`);
  const body2 = log2 ? await (await fileOf(log2.f)).text() : '';
  const lines2 = body2.split('\n').filter((l) => /^\d\d:\d\d:\d\d  /.test(l));
  ok(lines2.length > lines.length, `background added readings to it: ${lines.length} → ${lines2.length} lines`);
  const card2Text = card2 ? await (await fileOf(card2.f)).text() : '';
  ok(card2Text.includes('Heard by background decoding and listeners in ISM mode.'), 'the card says both heard it');
  ok(/ · TPMS · TPMS #1 · /.test(card2Text) && / · TPMS · TPMS #2 · /.test(card2Text),
     'the restarted server numbered its TPMS above the day\'s: #1 (the listener\'s) and #2 (a new salt cannot know it is the same car)');
  const leaked2 = SECRETS.filter((x) => (JSON.stringify(g2) + body2 + card2Text + everythingOnDisk()).includes(x));
  ok(leaked2.length === 0, `still no ID or code anywhere${leaked2.length ? ' — found ' + leaked2.join(',') : ''}`);
  b.ws.close();
} finally {
  await stopServer();
  rtl.kill('SIGTERM');
  await sleep(300);
  if (fail) console.log(`   (server logs and gallery: ${dir})`);
  else fs.rmSync(dir, { recursive: true, force: true });
}
console.log(`${fail ? 'FAILED' : 'ok'}: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
