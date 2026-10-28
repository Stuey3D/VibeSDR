// test-bg-decode.mjs — BACKGROUND DECODING + THE GALLERY, end to end through a real vibeserver (V12, 2026-10-22).
//
// ★★★ WHAT THE UNIT TEST (vibeserver/test-bgdecode.cpp) CANNOT SHOW: that the SERVER moves the radio when the last
//     listener has gone for the wait, runs the profile's channels and decoders, switches profile at a boundary, and —
//     the shared-dial contract — STOPS AT ONCE when anybody arrives and hands the newcomer the dial the radio was on
//     BEFORE background decoding moved it (never the NAVTEX centre). Plus the gallery over HTTP (listed, served,
//     path-traversal refused) and the config API (profiles round-trip; overlaps and an out-of-window VFO refused at the
//     save, in words).
// ★ TEST-ONLY knobs (read from the environment, so the build is the one that ships): VIBESERVER_BG_WAIT_SEC shortens
//   the owner's wait; VIBESERVER_BG_TEST_CLOCK names a file holding "HH:MM" that stands in for the UTC clock.
// ★ SILENT: no browser, no audio device — the fake dongle's samples go to sockets nobody plays.
// usage: VIBESERVER_BIN=… node scripts/test-bg-decode.mjs   (~60 s)
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)');
  process.exit(3);
}

/** A minimal WebSocket client (test-ask-before-tuning.mjs's), text frames only. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false, closed: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!hs) { const j = buf.indexOf('\r\n\r\n'); if (j < 0) return; hs = true; st.open = true; buf = buf.subarray(j + 4); }
    for (;;) {
      if (buf.length < 2) break;
      const op = buf[0] & 0x0f; let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) break;
      const p = buf.subarray(off, off + len); buf = buf.subarray(off + len);
      if (op === 0x9) { const q = Buffer.alloc(6 + p.length); q[0] = 0x8a; q[1] = 0x80 | p.length; q.writeUInt32BE(0, 2); p.copy(q, 6); s.write(q); continue; }
      if (op === 0x1) st.txt.push(p.toString());
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  st.last = (type) => { for (let i = st.txt.length - 1; i >= 0; i--) if (st.txt[i].includes(`"type":"${type}"`)) return JSON.parse(st.txt[i]); return null; };
  return st;
}

const NICK = { centre: 500000, from: '00:00', to: '12:00', vfos: [{ hz: 489500, mode: 'usb', dec: 'navtex' }, { hz: 517500, mode: 'usb', dec: 'navtex' }] };
const AFTERNOON = { centre: 4600000, from: '12:00', to: '00:00', vfos: [{ hz: 4583000, mode: 'lsb', dec: 'rtty-ham' }] };
const BG = { on: true, waitMin: 5, profiles: [NICK, AFTERNOON] };
const ADMIN = 'bg-admin-pass';

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-bgdec-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
  configured: true, name: 'Background Decoding Test', locator: 'IO92nh', adminPass: ADMIN, galleryMb: 50, decoderMax: 4,
  radios: [{ serial: 'bgdec-test', label: 'Test radio', enabled: true, configured: true, users: 4, rate: 2048000,
             bgDecode: JSON.stringify(BG) }],
}));
// ★ An item already in the gallery (as a previous night left it), and a file beside it that is NOT in the index.
const gdir = path.join(dir, 'data', 'gallery');
fs.mkdirSync(gdir);
fs.writeFileSync(path.join(gdir, '20261022-230000-navtex-518k0-abcd.txt'), 'NAVTEX · Station G · Met warning · #98\n\nZCZC GB98\nGALE WARNING\nNNNN\n');
// ★ beta 4: an FT8 summary (OLDER, so it lists second) — the FT8 tab sends kind=ft8, which the server once cut to "ft"
fs.writeFileSync(path.join(gdir, 'index.jsonl'), JSON.stringify({ f: '20261022-220000-ft8-1840k0-ef01.txt', k: 'ft8', o: 'background', t: 1761170400, hz: 1840000, m: 'usb', d: 'ft8',
                     p: 'Profile 1', r: 'Test radio', title: 'FT8 summary · 142 spots · 23 countries' }) + '\n'
  + JSON.stringify({ f: '20261022-230000-navtex-518k0-abcd.txt', k: 'navtex', o: 'background', t: 1761174000,
  hz: 517500, m: 'usb', d: 'navtex', p: 'Profile 1', r: 'Test radio bgdec-test', title: 'Station G · Met warning · #98' }) + '\n');   // ★ an older build wrote the serial
fs.writeFileSync(path.join(gdir, '20261022-220000-ft8-1840k0-ef01.txt'), 'FT8 / FT4 summary\n\nSpots heard:       142\n');
fs.writeFileSync(path.join(gdir, 'secret.txt'), 'not in the index');
fs.writeFileSync(path.join(dir, 'data', 'config-copy.json'), 'outside the gallery');
const clock = path.join(dir, 'clock');
fs.writeFileSync(clock, '06:00\n');

const rtlPort = await freePort(), port = await freePort();
const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', '2048000'], { stdio: 'ignore' });
await sleep(300);
const WAIT_S = 3;
const env = { ...process.env, VIBESERVER_BG_WAIT_SEC: String(WAIT_S), VIBESERVER_BG_TEST_CLOCK: clock,
              VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'), VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const log = fs.openSync(logPath, 'w');
const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '2048000', '--freq', '7074000',
                        '--mode', 'usb', '--users', '4'], { env, stdio: ['ignore', log, log] });
const stop = () => { try { srv.kill('SIGTERM'); } catch {} try { rtl.kill('SIGTERM'); } catch {} };
process.on('exit', stop);
for (let i = 0; i < 100; i++) {
  const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
  if (up) break;
  await sleep(100);
}
const logText = () => fs.readFileSync(logPath, 'utf8');
const logCount = (re) => (logText().match(new RegExp(re.source, 'g')) || []).length;
const until = async (f, ms = 8000) => { for (let t = 0; t < ms; t += 100) { if (f()) return true; await sleep(100); } return f(); };
const vfo = (st) => { const c = st.last('config'); return c ? Number(c.vfo) : NaN; };
const H = `http://127.0.0.1:${port}`;
const adminQ = async () => {
  const nonce = (await fetch(`${H}/vibeserver/auth`).then((r) => r.json())).nonce;
  return `vs_admin_nonce=${nonce}&vs_admin_auth=${crypto.createHmac('sha256', ADMIN).update(nonce).digest('hex')}`;
};

try {
  console.log('── a listener tunes, and leaves ──');
  const a = ws(port, '/ws/user-spectrum?user_session_id=bgaa0001&bins=256');
  await sleep(1200);
  ok(logCount(/background decoding: ON, wait 300 s, 2 profile\(s\)/) >= 1, 'the radio read its two profiles from config.json (ON, 5 minutes)');
  a.send({ type: 'tune', frequency: 7100000, mode: 'usb' });
  ok(await until(() => vfo(a) === 7100000), `the listener is on 7100.000 kHz (${vfo(a)})`);
  await sleep(2500);
  ok(logCount(/background decoding stopped|RF centre 500\.0 kHz/) === 0, 'nothing moved while somebody was listening');
  a.send({ type: 'leave' });   // ★ a real goodbye — a dropped socket is HELD (session hold, beta24) and still counts
  a.close();
  const leftAt = Date.now();

  console.log(`── nobody here: after the wait (${WAIT_S} s, test-only) the radio goes to profile 1 ──`);
  ok(await until(() => logCount(/profile 1, RF centre 500\.0 kHz, 2 VFO\(s\), 2 decoder\(s\) running/) === 1, 15000),
     '★ profile 1 runs: RF centre 500.0 kHz, 2 VFOs, 2 NAVTEX decoders');
  const waited = (Date.now() - leftAt) / 1000;
  ok(waited >= WAIT_S - 0.5, `...not before the wait (${waited.toFixed(1)} s after the listener left)`);
  ok(/moving the radio from [\d.]+ kHz \(dial 7100\.000\) to the profile 1 centre 500\.000 kHz/.test(logText()),
     '...and it remembered the dial it moved the radio FROM (7100.000)');
  ok(logCount(/decoder attached: fsk cf=500 shift=170 baud=100\.00 enc=CCIR476/) >= 2, '...both VFOs run NAVTEX (CCIR476, 100 Bd, tone 500 Hz)');

  console.log('── a profile boundary: the test clock passes 12:00 UTC ──');
  fs.writeFileSync(clock, '13:00\n');
  ok(await until(() => logCount(/a profile boundary — switching to profile 2/) === 1, 6000), '★ the boundary is seen: switching to profile 2');
  ok(await until(() => logCount(/profile 2, RF centre 4600\.0 kHz, 1 VFO\(s\), 1 decoder\(s\) running/) === 1, 6000),
     '★ profile 2 runs: RF centre 4600.0 kHz, its RTTY on LSB');
  ok(/decoder attached: fsk cf=1000 shift=170 baud=45\.45 enc=ITA2/.test(logText()), '...RTTY 45.45 Bd 170 Hz');
  ok(logCount(/back on 7100\.000/) === 0, '...and the switch did NOT give the dial back in between (nobody is here to give it to)');

  console.log('── somebody arrives: everything stops at once, and they land where the radio was LEFT ──');
  const stopsAtArrival = logCount(/background decoding stopped — /);
  const b = ws(port, '/ws/user-spectrum?user_session_id=bgbb0002&bins=256');
  // ★ ("Keep decoding stations still in range" is on by default: profile 2's VFO is not in the window of the dial the radio goes back to, so it stops.)
  ok(await until(() => logCount(/background decoding stopped — /) > stopsAtArrival, 3000), '★ background decoding stopped the moment a listener connected');
  ok(/the radio is back on 7100\.000 kHz/.test(logText()), '★ ...and the radio is back on 7100.000 kHz');
  ok(await until(() => vfo(b) === 7100000, 3000), `★ the newcomer is on 7100.000 kHz — not the NAVTEX or RTTY centre (${vfo(b)})`);
  await sleep(WAIT_S * 1000 + 2500);
  ok(logCount(/profile 2, RF centre 4600\.0 kHz/) === 1, 'while they are here, it does not come back');
  b.send({ type: 'leave' });   // ★ a real goodbye — a dropped socket is HELD (session hold, beta24) and still counts
  b.close();

  console.log('── they leave; it comes back after the wait; a DECODER socket on its own stops it too ──');
  ok(await until(() => logCount(/profile 2, RF centre 4600\.0 kHz/) === 2, 15000), 'nobody again: profile 2 is back after the wait');
  const stopsBefore = logCount(/background decoding stopped — /);
  const dx = ws(port, '/ws/dxcluster?user_session_id=bgcc0003');
  ok(await until(() => logCount(/background decoding stopped — /) > stopsBefore, 3000), '★ a decoder socket alone counts as somebody');
  ok(logCount(/background decoding stopped — /) === stopsBefore + 1, '...stopped once');
  dx.close();

  console.log('── the gallery, over HTTP ──');
  const g = await fetch(`${H}/vibeserver/gallery.json`).then((r) => r.json());
  ok(g.enabled === true && g.items?.length === 2 && g.items[0].title === 'Station G · Met warning · #98' && g.items[0].o === 'background',
     `listed: ${g.items?.length} items (newest the NAVTEX), enabled, labelled background (${JSON.stringify(g.items?.[0] || {}).slice(0, 120)}…)`);
  ok(!JSON.stringify(g).includes('bgdec-test') && g.items[0].r === 'Test radio', `★ the radio's SERIAL is nowhere in gallery.json, even on an item stored with it (r: "${g.items[0]?.r}")`);
  ok(g.limitBytes === 50 * 1024 * 1024, `the owner's size reached the server (${g.limitBytes} bytes = 50 MB)`);
  const one = await fetch(`${H}/vibeserver/gallery/file/20261022-230000-navtex-518k0-abcd.txt`);
  const body = await one.text();
  ok(one.status === 200 && /text\/plain/.test(one.headers.get('content-type') || '') && body.includes('GALE WARNING'), 'an item is served, as text');
  const dl = await fetch(`${H}/vibeserver/gallery/file/20261022-230000-navtex-518k0-abcd.txt?download=1`);
  ok(/attachment/.test(dl.headers.get('content-disposition') || ''), '...and ?download=1 saves it');
  for (const bad of ['secret.txt', 'index.jsonl', '.lock', '..%2Fconfig-copy.json', '..%2F..%2Fconfig.json', '%2Fetc%2Fpasswd']) {
    const r = await fetch(`${H}/vibeserver/gallery/file/${bad}`);
    ok(r.status === 404, `★ refused: ${decodeURIComponent(bad)} (${r.status})`);
  }
  const raw = await new Promise((res) => {
    const c = net.connect(port, '127.0.0.1'); let out = '';
    c.on('connect', () => c.write('GET /vibeserver/gallery/file/../config-copy.json HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'));
    c.on('data', (d) => { out += d; }); c.on('end', () => res(out)); c.on('error', () => res(out));
  });
  ok(/^HTTP\/1\.1 404/.test(raw) && !raw.includes('outside the gallery'), '★ refused: a raw ../ that no browser would normalise away');
  const k = await fetch(`${H}/vibeserver/gallery.json?kind=rtty`).then((r) => r.json());
  ok(Array.isArray(k.items) && k.items.length === 0, 'filtered by decoder: no RTTY yet');
  const k8 = await fetch(`${H}/vibeserver/gallery.json?kind=ft8`).then((r) => r.json());
  ok(Array.isArray(k8.items) && k8.items.length === 1 && k8.items[0].k === 'ft8' && /142 spots/.test(k8.items[0].title),
     `★ kind=ft8 (the FT8 tab) returns the FT8 summary — the digit is kept (${k8.items && k8.items.length})`);

  console.log('── the config API: the profiles round-trip; the save refuses what cannot work, in words ──');
  const got = await fetch(`${H}/vibeserver/config?${await adminQ()}`).then((r) => r.json());
  ok(got.radios?.[0]?.bgDecode && JSON.parse(got.radios[0].bgDecode).profiles?.length === 2, 'GET: the radio carries its profiles');
  ok(got.galleryMb === 50 && got.galleryPublic === true && got.galleryFreeBytes > 0, `GET: the gallery's size, and ${(got.galleryFreeBytes / 1e9).toFixed(1)} GB free on the disk`);
  ok(typeof got.serverUtcOffsetMin === 'number', `GET: the server's UTC offset for the local-time hint (${got.serverUtcOffsetMin} min)`);
  const post = async (cfg) => {
    const r = await fetch(`${H}/vibeserver/config?${await adminQ()}`, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(cfg) });
    return { status: r.status, j: await r.json().catch(() => ({})) };
  };
  const withBg = (bg) => ({ ...got, radios: [{ ...got.radios[0], bgDecode: JSON.stringify(bg) }] });
  const overlap = await post(withBg({ on: true, waitMin: 5, profiles: [{ ...NICK, from: '00:00', to: '00:00' }, AFTERNOON] }));
  ok(overlap.status === 400 && /Profile 1 \(all day\) and Profile 2 \(12:00–00:00\) overlap/.test(overlap.j.error || ''),
     `★ overlapping profiles refused, named: "${overlap.j.error}"`);
  const far = await post(withBg({ on: true, waitMin: 5, profiles: [{ ...NICK, vfos: [...NICK.vfos, { hz: 2500000, mode: 'usb', dec: 'navtex' }] }] }));
  ok(far.status === 400 && /VFO 3 \(2500\.0 kHz\).*2048 kS\/s/.test(far.j.error || ''), `★ a VFO the radio cannot see is refused, saying why: "${far.j.error}"`);
  // ★★ V12 2026-10-27: a Band digital-spots VFO is checked against its band's REAL span at the save (the registry is
  //    linked into the daemon's save path — vibe_bgdecode_band.h): a frequency on no band, and a band outside the window.
  const noBand = await post(withBg({ on: true, waitMin: 5, profiles: [{ centre: 6500000, from: '00:00', to: '00:00', vfos: [{ hz: 6500000, mode: 'usb', dec: 'spots-band' }] }] }));
  ok(noBand.status === 400 && /not in a band the digital modes use/.test(noBand.j.error || ''), `★ a Band VFO on no band: refused, in words: "${noBand.j.error}"`);
  const bandFar = await post(withBg({ on: true, waitMin: 5, profiles: [{ centre: 6000000, from: '00:00', to: '00:00', vfos: [{ hz: 7074000, mode: 'usb', dec: 'spots-band' }] }] }));
  ok(bandFar.status === 400 && /the 40 m band's digital modes .* are not all inside this radio's window/.test(bandFar.j.error || ''),
     `★ the band's modes outside the window: refused, saying why: "${bandFar.j.error}"`);
  // ★★ A single-radio server: a profile may have no more VFOs than the box has decoder slots (decoderMax 4 here).
  const five = await post(withBg({ on: true, waitMin: 5, keepInRange: false, profiles: [{ ...NICK, to: '00:00',
    vfos: [0, 1, 2, 3, 4].map((i) => ({ hz: 489500 + i * 2000, mode: 'usb', dec: 'navtex' })) }] }));
  ok(five.status === 400 && /Profile 1 has 5 VFOs — this server can run 4 decoders at once/.test(five.j.error || ''),
     `★ 5 VFOs on a one-radio server with 4 decoder slots: refused, in words: "${five.j.error}"`);
  const nickOnly = { on: true, waitMin: 10, profiles: [{ ...NICK, to: '00:00' }] };
  const good = await post(withBg(nickOnly));
  ok(good.status === 200, `Nick's profile (all day) saves (${good.status} ${JSON.stringify(good.j)})`);
  const back = await fetch(`${H}/vibeserver/config?${await adminQ()}`).then((r) => r.json());
  ok(back.radios?.[0]?.bgDecode === JSON.stringify(nickOnly), `...and reads back exactly: ${back.radios?.[0]?.bgDecode}`);
  ok(await until(() => logCount(/background decoding: ON, wait 600 s, 1 profile\(s\)/) >= 1, 3000), '★ ...and the running radio took it LIVE (wait 10 min, 1 profile)');
} catch (e) {
  fail++; console.log(`   FAIL threw: ${e && e.stack || e}`);
} finally {
  stop();
  await sleep(300);
}
console.log(`\n${pass} passed, ${fail} failed`);
if (fail) console.log(`   (server log: ${logPath})`);
process.exit(fail ? 1 : 0);
