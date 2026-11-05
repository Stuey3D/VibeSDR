// test-handheld.mjs — `vibeserver --handheld` end to end: the R36S handheld's supervisor (vibeserver/handheld.cpp)
// against fake radios (vibeserver/fake-rtl-tcp.mjs) and a fake hardware service (scripts/fake-vibe-hwd.mjs, the §0.2
// contract of docs/v12/R36S-HANDHELD-IMAGE.md).
//
//  1. no radio: the page loads from the supervisor, /vibeserver.json says so, /handheld/session state "none"
//  2. a radio plugged in is adopted with no setup step; its own /vibeserver.json arrives through the hand-off and says
//     "handheld":true; a spectrum socket opens; it listens on 127.0.0.1 only
//  3. per-radio memory: the page's prefs go in, are written (temp + rename) on unplug, and come back when THAT radio
//     returns — a second radio keeps its own; a late post for a pulled radio is refused
//  4. hot swap: radio A out, radio B in, B's process; the events stream says so
//  5. Start+Select: the flag is spent on the next radio (its memory gone, a reset sequence for the toast), then removed
//  6. the card: a recording uploaded, listed, fetched with a Range, deleted; names "YYYY-MM-DD HHMM - <kHz> kHz <MODE>.wav";
//     no ".part" left; import/ listed and a result moves the file to import/done/ with a .result.txt; export/; the
//     diagnostics file in "Diagnostic logs/"
//  7. the hardware service: state relayed; brightness clamped 1..255; no SSH requests relayed (none on the handheld); a clock set (deltaMs) reaches the service; events (volume) reach a
//     long-poll; the shutdown event saves memory and is ACKed inside 5 s
//
// usage: VIBESERVER_BIN=… node scripts/test-handheld.mjs   (~1 min; exit 3 = not run)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { startFakeHwd } from './fake-vibe-hwd.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 100; i++) { try { if (await f()) return true; } catch { /* not yet */ } await sleep(100); } try { return !!(await f()); } catch { return false; } };

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-handheld-'));
const data = path.join(dir, 'data'), card = path.join(dir, 'card'), radios = path.join(dir, 'radios.txt');
fs.mkdirSync(data); fs.mkdirSync(card); fs.writeFileSync(radios, '');
const hwdSock = path.join(dir, 'hwd.sock'), resetFlag = path.join(dir, 'reset-radio');
const diag = path.join(dir, 'diag.py');
fs.writeFileSync(diag, 'print("== fake diagnostics ==")\n');
const procs = [];
const fakeRtl = async () => {
  const p = await freePort();
  const child = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(p), '--rate', '2048000'], { stdio: 'ignore' });
  procs.push(child);
  return p;
};
const hwd = await startFakeHwd(hwdSock);
const rtlA = await fakeRtl(), rtlB = await fakeRtl();
await sleep(400);
const port = await freePort();
const logPath = path.join(dir, 'handheld.log');
let sup = null;
function startSupervisor() {
  const log = fs.openSync(logPath, 'a');
  sup = spawn(BIN, ['--handheld', '--port', String(port)], {
    env: { ...process.env, VIBESERVER_DATA_DIR: data, VIBESERVER_CARD_DIR: card, VIBE_HWD_SOCK: hwdSock,
           VIBE_HANDHELD_FAKE_RADIOS: radios, VIBE_HANDHELD_RESET_FLAG: resetFlag, VIBESERVER_DIAG_SCRIPT: diag },
    stdio: ['ignore', log, log],
  });
}
async function stopSupervisor() {
  if (!sup) return;
  const p = sup; sup = null;
  const gone = new Promise((r) => p.once('exit', r));
  p.kill('SIGTERM');
  await Promise.race([gone, sleep(10000)]);
}
const L = () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } };
const U = (p) => `http://127.0.0.1:${port}${p}`;
const getJ = async (p) => (await fetch(U(p), { signal: AbortSignal.timeout(30000) })).json();
const postJ = async (p, body, method = 'POST') => {
  const r = await fetch(U(p), { method, body: (typeof body === 'string' || Buffer.isBuffer(body)) ? body : JSON.stringify(body), signal: AbortSignal.timeout(30000) });
  let j = null; try { j = await r.json(); } catch { /* not JSON */ }
  return { status: r.status, j };
};
const session = () => getJ('/handheld/session');
const plug = (serial, rtl) => fs.writeFileSync(radios, rtl ? `${serial} rtlsdr 127.0.0.1:${rtl}\n` : '');
const running = (serial) => until(async () => { const s = await session(); return s.state === 'running' && s.radio && s.radio.serial === serial; }, 30000);
const memFile = (serial) => path.join(data, 'handheld', `radio-${serial}.json`);
const walk = (d) => fs.existsSync(d) ? fs.readdirSync(d, { withFileTypes: true }).flatMap((e) => e.isDirectory() ? walk(path.join(d, e.name)) : [path.join(d, e.name)]) : [];

try {
  startSupervisor();
  // ── 1 ──
  console.log('── no radio ──');
  ok(await until(async () => (await session()).handheld === true, 15000), 'the supervisor answers /handheld/session');
  let s = await session();
  ok(s.state === 'none' && s.radio === null, `state "none", no radio (${s.state})`);
  const page = await fetch(U('/'));
  const html = await page.text();
  ok(page.status === 200 && html.includes('<title>VibeSDR</title>'), `the page itself loads with no radio (${page.status}, ${html.length} bytes)`);
  const vs0 = await getJ('/vibeserver.json');
  ok(vs0.handheld === true && vs0.radio === null, '/vibeserver.json says handheld, no radio');
  const ws0 = await fetch(U('/ws/user-spectrum'));
  ok(ws0.status === 503, `a radio route with no radio is a plain 503 (${ws0.status})`);
  ok(await until(() => hwd.subscribed() > 0, 5000), 'the supervisor subscribed to the hardware service');

  // ── 2 ──
  console.log('── a radio plugged in ──');
  const evSeq0 = (await session()).eventSeq;
  plug('RADIOA', rtlA);
  ok(await running('RADIOA'), 'radio A adopted and running — no setup step');
  const vsA = await getJ('/vibeserver.json');
  ok(vsA.handheld === true && vsA.radio !== null && typeof vsA.proto === 'number', `its own /vibeserver.json arrives through the hand-off ("handheld":true, proto ${vsA.proto})`);
  const ev = await getJ(`/handheld/events?since=${evSeq0}`);
  ok(ev.events.some((e) => e.event === 'radio' && e.state === 'running'), 'the events stream announced it running');
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=hh000001&bins=256&proto=1`);
  ws.binaryType = 'arraybuffer';
  let frames = 0, cfg = null;
  ws.onmessage = (e) => { if (typeof e.data === 'string') { const j = JSON.parse(e.data); if (j.type === 'config') cfg = j; } else frames++; };
  ok(await until(() => frames > 3 && cfg, 15000), `a spectrum socket through the hand-off: config + ${frames} frames`);
  ws.close();
  ok(/handheld profile/.test(L()), 'the radio process runs the handheld profile');
  ok(!/advertising as/.test(L()) && !/tunnel/i.test(L().split('\n').filter((l) => /starting|started/.test(l)).join('\n')), 'no mDNS advert, no tunnel');

  // ── 3 ──
  console.log('── per-radio memory ──');
  const prefsA = { tuned: { x: { hz: 7074000, mode: 'usb' } }, palette: 'gqrx', zoom: 3, 'quote"key': 'é' };
  let r = await postJ('/handheld/memory', { serial: 'RADIOA', prefs: prefsA });
  ok(r.status === 200 && r.j?.ok, 'the page\'s prefs for radio A accepted');
  r = await postJ('/handheld/memory', { serial: 'SOMEONEELSE', prefs: { a: 1 } });
  ok(r.status === 409, `a post for a radio that is not plugged in is refused (${r.status})`);
  ok(JSON.stringify((await session()).memory) === JSON.stringify(prefsA), 'the session hands the memory back as posted');

  // ── 4 ──
  console.log('── hot swap ──');
  plug('RADIOB', rtlB);
  ok(await running('RADIOB'), 'radio A out, radio B in — B running');
  ok(fs.existsSync(memFile('RADIOA')), 'radio A\'s memory was written on unplug');
  const onDisk = JSON.parse(fs.readFileSync(memFile('RADIOA'), 'utf8'));
  ok(JSON.stringify(onDisk.prefs) === JSON.stringify(prefsA) && onDisk.serial === 'RADIOA', 'what is on disk is exactly what the page sent');
  ok(!walk(path.join(data, 'handheld')).some((f) => f.endsWith('.part')), 'no temp file left beside it');
  s = await session();
  ok(s.memory === null, 'radio B starts with no memory (defaults)');
  await postJ('/handheld/memory', { serial: 'RADIOB', prefs: { tuned: { x: { hz: 100700000, mode: 'wfm' } } } });
  plug('', 0);
  ok(await until(async () => (await session()).state === 'none', 15000), 'B unplugged — "Please connect an SDR" (state none)');
  ok(fs.existsSync(memFile('RADIOB')), 'B\'s memory written on unplug');
  plug('RADIOA', rtlA);
  ok(await running('RADIOA'), 'radio A back');
  ok(JSON.stringify((await session()).memory) === JSON.stringify(prefsA), 'radio A\'s own memory restored');

  // ── 7 (before the reset restart) ──
  console.log('── the hardware service ──');
  const dev = await getJ('/handheld/device');
  ok(dev.state?.ok === true && dev.state.brightness === 120 && dev.ssh === undefined, 'state relayed (brightness 120), nothing about SSH');
  r = await postJ('/handheld/device', { set: 'brightness', value: 0 });
  ok(r.j?.ok && hwd.state.brightness === 1, `brightness floored at 1 (asked 0, service got ${hwd.state.brightness})`);
  r = await postJ('/handheld/device', { set: 'volume', value: 140 });
  ok(r.j?.ok && hwd.state.volume === 100, 'volume clamped to 100');
  for (const q of [{ set: 'ssh', on: true }, { set: 'ssh-password', password: 'correct-horse-battery' }]) {
    r = await postJ('/handheld/device', q);
    ok(r.status === 400 && !hwd.requests.some((x) => x.set === q.set), `no SSH on the handheld: "${q.set}" refused, never relayed`);
  }
  const PW = 'correct-horse-battery';
  r = await postJ('/handheld/device', { set: 'something-else', value: 1 });
  ok(r.status === 400, 'a request outside the list is refused, not relayed');
  const before = Date.now();
  r = await postJ('/handheld/clock', { deltaMs: 1234, source: 'test' });
  ok(r.j?.ok && Math.abs(hwd.state.timeSetTo - (before + 1234)) < 1500, `a clock nudge reaches the service as now + delta (${hwd.state.timeSetTo - before} ms)`);
  r = await postJ('/handheld/clock', { utcMs: 1000 });
  ok(r.status === 400, 'an implausible time is refused');
  const seq1 = (await session()).eventSeq;
  const longPoll = getJ(`/handheld/events?since=${seq1}`);
  await sleep(300);
  hwd.emit({ event: 'volume', volume: 42, muted: false });
  const lp = await longPoll;
  ok(lp.events.some((e) => e.event === 'volume' && e.volume === 42), 'a volume event reaches a waiting long-poll');
  const perf = await getJ('/handheld/perf');
  ok(typeof perf.cpu === 'number' && perf.battery && perf.battery.pct === 83, `perf figures (battery ${perf.battery?.pct} %)`);

  // ── 6 ──
  console.log('── the card ──');
  const wav = Buffer.alloc(44 + 48000 * 2);
  wav.write('RIFF', 0); wav.write('WAVE', 8);
  r = await postJ(`/handheld/recordings?hz=7074000&mode=USB&start=${Date.UTC(2026, 10, 5, 14, 23) / 1000}`, wav);
  ok(r.j?.ok && r.j.name === '2026-11-05 1423 - 7074.000 kHz USB.wav', `recording saved as "${r.j?.name}"`);
  r = await postJ(`/handheld/recordings?hz=7074000&mode=USB&start=${Date.UTC(2026, 10, 5, 14, 23) / 1000}`, wav);
  ok(r.j?.name === '2026-11-05 1423 - 7074.000 kHz USB (2).wav', `a second in the same minute does not overwrite it ("${r.j?.name}")`);
  const list = await getJ('/handheld/recordings');
  ok(list.recordings.length === 2 && list.recordings[0].bytes === wav.length, 'both listed with their size');
  const one = await fetch(U('/handheld/recordings/' + encodeURIComponent('2026-11-05 1423 - 7074.000 kHz USB.wav')), { headers: { Range: 'bytes=0-3' } });
  ok(one.status === 206 && (await one.text()) === 'RIFF', 'played back with a Range (206, "RIFF")');
  const trav = await fetch(U('/handheld/recordings/' + encodeURIComponent('../data/handheld/radio-RADIOA.json')));
  ok(trav.status === 404, 'a path outside Recordings/ is refused');
  r = await postJ('/handheld/recordings/' + encodeURIComponent('2026-11-05 1423 - 7074.000 kHz USB (2).wav'), '', 'DELETE');
  ok(r.j?.ok && (await getJ('/handheld/recordings')).recordings.length === 1, 'deleted');
  ok(!walk(card).some((f) => path.basename(f).endsWith('.part')), 'no ".part" left on the card');
  fs.mkdirSync(path.join(card, 'import'), { recursive: true });
  fs.writeFileSync(path.join(card, 'import', 'my bookmarks.json'), '[{"name":"x","frequency":7074000}]');
  const imp = await getJ('/handheld/import');
  ok(imp.files.length === 1 && imp.files[0].name === 'my bookmarks.json' && imp.files[0].text.includes('7074000'), 'import/ lists the waiting file with its text');
  r = await postJ('/handheld/import/result', { name: 'my bookmarks.json', ok: true, imported: 1, message: 'Imported 1 bookmark' });
  const done = path.join(card, 'import', 'done');
  ok(r.j?.ok && fs.existsSync(path.join(done, 'my bookmarks.json')) && !fs.existsSync(path.join(card, 'import', 'my bookmarks.json')), 'moved to import/done/');
  ok(/Bookmarks imported: 1/.test(fs.readFileSync(path.join(done, 'my bookmarks.json.result.txt'), 'utf8')), 'with a result .txt beside it');
  r = await postJ('/handheld/export?kind=bookmarks', '[{"name":"x"}]');
  ok(r.j?.ok && /^export\/\d{4}-\d\d-\d\d \d{6} - bookmarks\.json$/.test(r.j.name) && fs.existsSync(path.join(card, r.j.name)), `export → ${r.j?.name}`);
  r = await postJ('/handheld/diagnostics', '');
  ok(r.j?.ok && /^Diagnostic logs\/\d{4}-\d\d-\d\d \d{6} UTC - VibeSDR diagnostics\.txt$/.test(r.j.name), `diagnostics → ${r.j?.name}`);
  const dtext = r.j?.ok ? fs.readFileSync(path.join(card, r.j.name), 'utf8') : '';
  ok(dtext.includes('fake diagnostics') && dtext.includes('handheld supervisor') && !dtext.includes(PW), 'the script\'s text and the supervisor\'s state — and no SSH password');
  ok(walk(card).every((f) => !/[:*?"<>|\\]/.test(path.relative(card, f))), 'every name on the card is FAT-safe');

  // ── 7b: shutdown ──
  console.log('── shutdown event ──');
  await postJ('/handheld/memory', { serial: 'RADIOA', prefs: { ...prefsA, zoom: 9 } });
  const ackMs = await hwd.shutdown('battery');
  ok(ackMs !== null && ackMs < 5000, `the shutdown event is ACKed (${ackMs} ms)`);
  ok(JSON.parse(fs.readFileSync(memFile('RADIOA'), 'utf8')).prefs.zoom === 9, 'and the latest memory was on disk before the ack');
  await stopSupervisor();

  // ── 5 ──
  console.log('── Start+Select reset ──');
  fs.writeFileSync(resetFlag, '1');
  startSupervisor();
  ok(await running('RADIOA'), 'restarted with radio A plugged in');
  s = await session();
  ok(s.memory === null && s.resetSeq > 0 && s.resetSerial === 'RADIOA', `A's memory gone, a reset for the toast (seq ${s.resetSeq})`);
  ok(!fs.existsSync(resetFlag), 'the flag was spent and removed');
  ok(fs.existsSync(memFile('RADIOB')), 'radio B\'s memory untouched by A\'s reset');
} catch (e) {
  fail++; console.log('   FAIL threw:', e && e.stack || e);
} finally {
  await stopSupervisor();
  for (const p of procs) p.kill('SIGTERM');
  await hwd.close();
  if (fail) console.log(`   (log: ${logPath})`);
}
console.log(`test-handheld: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
