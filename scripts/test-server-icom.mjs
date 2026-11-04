// test-server-icom.mjs — the REAL server against a pretend IC-7300, end to end (docs/ICOM-SUPPORT-PLAN.md).
//
// tools/icom/fake-icom serves CI-V on a pty with a synthetic 475-point scope; vibeserver opens it as an Icom
// (`--icom <pty> --icom-tty-ok`, test audio `tone`). Then, over the same sockets a client uses:
//   • hwinfo says driver "icom" with the radio's own lists, and nothing IQ-shaped;
//   • SPEC frames arrive at the radio's rate over the radio's edges, and config's span matches them;
//   • the OWNER LOCK refuses a listener's tune (default ON), and a signed-in admin tunes the radio;
//   • the radio's own knob reaches every listener; zoom is the span ladder; a mode change reaches the radio;
//   • the owner keying the radio raises radio.tx;
//   • ★★★ and at the end the fake is asked how many frames it was sent that would transmit or are off the
//     allow-list. The answer must be 0.
// ★ Needs VIBESERVER_BIN and the fake built beside it (FAKE_ICOM_BIN, default: next to VIBESERVER_BIN).
// ★ SILENT: the audio is a test tone that goes to a socket, never to a speaker.
//
//   usage: VIBESERVER_BIN=vibeserver/build/vibeserver node scripts/test-server-icom.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { spawn } from 'node:child_process';

const BIN = process.env.VIBESERVER_BIN || '';
const FAKE = process.env.FAKE_ICOM_BIN || (BIN ? path.join(path.dirname(BIN), 'fake-icom') : '');
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(FAKE)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree (with fake-icom beside it)');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const until = async (f, ms = 3000) => { for (let i = 0; i < ms / 50; i++) { if (await f()) return true; await sleep(50); } return !!(await f()); };
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const getJson = (port, p) => new Promise((res) => {
  http.get({ host: '127.0.0.1', port, path: p }, (r) => { let b = ''; r.on('data', (d) => { b += d; }); r.on('end', () => { try { res(JSON.parse(b)); } catch { res(null); } }); })
    .on('error', () => res(null));
});

/** A minimal WebSocket client — text kept, binary SPEC frames summarised. */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], bin: 0, binBytes: 0, lastLen: 0, lastCentre: 0, open: false, closed: false };
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
      if (op === 0x1) st.txt.push({ at: Date.now(), t: p.toString() });
      if (op === 0x2) {
        st.bin++; st.binBytes += p.length;
        if (p.length >= 22 && p.toString('latin1', 0, 4) === 'SPEC') { st.lastLen = p.length; st.lastCentre = Number(p.readBigUInt64LE(14)); st.spec = (st.spec || 0) + 1; }
        else if (p[0] === 0x01 && p.length >= 13) st.decoded = (st.decoded || '') + p.subarray(13).toString();   // a decoder's text: 0x01, ts u64, len u32, text
      }
    }
  });
  st.send = (o) => {
    const b = Buffer.from(JSON.stringify(o));
    const h = Buffer.alloc(b.length < 126 ? 6 : 8);
    h[0] = 0x81;
    if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
    s.write(Buffer.concat([h, b]));
  };
  st.last = (type) => { for (let i = st.txt.length - 1; i >= 0; i--) { try { const j = JSON.parse(st.txt[i].t); if (j.type === type) return j; } catch {} } return null; };
  st.close = () => { try { s.write(Buffer.from([0x88, 0x80, 0, 0, 0, 0])); } catch {} s.end(); };
  return st;
}

const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-icom-'));
for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
const fake = spawn(FAKE, ['--echo'], { stdio: ['pipe', 'pipe', 'pipe'] });
let fakeOut = '';
fake.stdout.on('data', (d) => { fakeOut += d.toString(); });
fake.stderr.on('data', (d) => { fakeOut += d.toString(); });
await until(() => /FAKE_ICOM_PTY (\S+)/.test(fakeOut), 3000);
const pty = (fakeOut.match(/FAKE_ICOM_PTY (\S+)/) || [])[1];
const stats = async () => {
  const n0 = fakeOut.length; fake.stdin.write('stats\n');
  await until(() => fakeOut.slice(n0).includes('FAKE_ICOM_STATS'), 2000);
  const m = fakeOut.slice(n0).match(/FAKE_ICOM_STATS (.*)/);
  const o = {}; if (m) for (const kv of m[1].trim().split(' ')) { const [k, v] = kv.split('='); o[k] = Number(v); }
  return o;
};

const port = await freePort();
const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
              VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
const logPath = path.join(dir, 'server.log');
const log = fs.openSync(logPath, 'w');
const srv = spawn(BIN, ['--icom', pty, '--icom-tty-ok', '--icom-audio', 'tone', '--port', String(port), '--users', '4', '--idle-grace', '0'],
                  { env, stdio: ['ignore', log, log] });
const readLog = () => fs.readFileSync(logPath, 'utf8');
try {
  console.log('── vibeserver + fake IC-7300 ──');
  ok(!!pty, `the fake radio is on ${pty}`);
  ok(await until(() => getJson(port, '/vibeserver.json').then((j) => !!j), 8000), 'the server is up');
  const vj = await getJson(port, '/vibeserver.json');
  ok(JSON.stringify(vj.tunable) === '[[30000,74800000]]', `/vibeserver.json: tunable is the IC-7300's receive range (${JSON.stringify(vj.tunable)})`);
  ok(vj.dab === false, 'and DAB is not offered');
  // ★★ V12 what one demodulated audio channel cannot do is not offered (vibe_icom_decode.h)
  ok(vj.skim === false, 'the CW / PSK SKIMMER is not offered (skim:false — ONE SIGNAL only)');
  ok(Array.isArray(vj.modes) && !vj.modes.includes('drm') && vj.modes.includes('ft8') && vj.modes.includes('rtty'),
     `DRM is not offered (the 7300's widest SSB filter is 3.6 kHz); FT8 and RTTY are (${(vj.modes || []).join(',')})`);
  ok(!('spotsBand' in vj), 'nor Band spots (an IQ slice)');

  const sid = 'icom0001-aaaa', sid2 = 'icom0002-bbbb';
  const a = ws(port, `/ws/user-spectrum?user_session_id=${sid}&bins=512`);
  const au = ws(port, `/ws/audio?user_session_id=${sid}`);
  ok(await until(() => a.last('hwinfo') && a.last('config'), 4000), 'hwinfo and config arrive on the spectrum socket');
  const hw = a.last('hwinfo') || {}; const r = hw.radio || {};
  ok(r.driver === 'icom' && r.model === 'IC-7300' && r.civ === '94', `hwinfo radio: driver icom, IC-7300, CI-V 94 (${r.driver} ${r.model} ${r.civ})`);
  ok(r.noIq === true && r.sharedOnly === true && r.canTransmit === false, 'noIq, sharedOnly, canTransmit:false');
  ok(Array.isArray(r.modes) && r.modes.includes('usbd') && !r.modes.includes('wfm'), `modes are the radio's (${(r.modes || []).join(',')})`);
  ok(r.scope && r.scope.points === 475 && JSON.stringify(r.scope.spans) === '[5000,10000,20000,50000,100000,200000,500000,1000000]',
     `scope: 475 points, the 8-rung span ladder (${r.scope && JSON.stringify(r.scope.spans)})`);
  ok(r.preflight && r.preflight.passed === true && r.preflight.items.length === 5, `preflight passed, ${r.preflight && r.preflight.items.length} items shown`);
  ok(r.ownerLock === true, 'owner lock ON by default — listeners follow the radio');
  ok(!('rates' in r) && !('attSteps' in r) && !('hfLna' in r), 'no IQ-radio fields (rates, attSteps, hfLna) in the radio object');

  const cfg0 = a.last('config') || {};
  ok(cfg0.vfo === 14074000 && cfg0.radioMode === 'usbd' && cfg0.mode === 'usb', `config: vfo 14.074, radioMode usbd, mode usb (${cfg0.vfo} ${cfg0.radioMode} ${cfg0.mode})`);
  ok(Math.abs(cfg0.binBandwidth * 512 - 50000) < 1, `config span = the scope's 50 kHz (${(cfg0.binBandwidth * 512).toFixed(0)})`);
  const s0 = a.spec || 0;
  await sleep(2000);
  const fps = ((a.spec || 0) - s0) / 2;
  ok(fps >= 5, `SPEC frames arrive at the radio's rate: ${fps.toFixed(1)} /s`);
  ok(a.lastLen === 22 + 512 && a.lastCentre === 14074000, `each is 512 bins centred on the radio (${a.lastLen - 22} bins at ${a.lastCentre})`);
  ok(au.binBytes > 1000, `audio flows on the audio socket (${au.binBytes} bytes)`);
  ok(a.last('icom') && typeof a.last('icom').sRaw === 'number', `the icom status line arrives (S ${a.last('icom') && a.last('icom').sRaw})`);

  console.log('   the owner lock');
  a.send({ type: 'tune', frequency: 7074000, mode: 'usb' });
  await sleep(800);
  let st = await stats();
  ok(st.freq === 14074000, `★ a listener's tune is REFUSED with the lock on — the radio stayed on ${st.freq}`);
  ok(a.txt.some((m) => m.t.includes('"notice"') && m.t.includes('follow')), 'and they are told why, in words');

  a.send({ type: 'admin_unlock', nonce: '', token: '' });
  ok(await until(() => a.txt.some((m) => m.t.includes('"type":"admin","ok":true')), 2000), 'admin signs in (no password set: granted)');
  a.send({ type: 'tune', frequency: 7074000, mode: 'usb' });
  ok(await until(async () => (await stats()).freq === 7074000, 3000), '★ the admin tunes: the RADIO moved to 7.074');
  ok(await until(() => a.last('config')?.vfo === 7074000, 3000), 'config follows');
  ok(await until(() => a.lastCentre === 7074000, 3000), 'and the waterfall is centred there');

  const b = ws(port, `/ws/user-spectrum?user_session_id=${sid2}&bins=256`);
  ok(await until(() => b.last('config')?.vfo === 7074000, 3000), 'a second listener joins and ADOPTS the radio (does not impose its own)');
  fake.stdin.write('knob 7100000\n');
  ok(await until(() => a.last('config')?.vfo === 7100000 && b.last('config')?.vfo === 7100000, 3000), "★ the radio's own knob reaches EVERY listener");

  console.log('   zoom = the span ladder; mode; filter');
  a.send({ type: 'zoom', frequency: 7100000, binBandwidth: 100000 / 512 });
  ok(await until(async () => (await stats()).span === 100000, 3000), 'a zoom out to ~100 kHz sets the radio\'s span to the 100 kHz rung');
  ok(await until(() => Math.abs((a.last('config')?.binBandwidth || 0) * 512 - 100000) < 1, 3000), 'config span follows the sweep edges');
  a.send({ type: 'zoom', frequency: 7100000, binBandwidth: 33000 / 512 });
  ok(await until(async () => (await stats()).span === 50000, 3000), 'a zoom to 33 kHz picks the nearest rung (50 kHz), never a span the radio lacks');
  a.send({ type: 'mode', mode: 'cwu' });
  ok(await until(async () => (await stats()).mode === 3, 3000), 'mode cwu → the radio is in CW');
  ok(await until(() => a.last('config')?.radioMode === 'cw' && a.last('config')?.mode === 'cwu', 3000), 'config: radioMode cw, mode cwu');
  a.send({ type: 'icom_control', mode: 'usbd' });
  ok(await until(async () => { const s = await stats(); return s.mode === 1 && s.data === 1; }, 3000), 'icom_control mode usbd → USB + DATA');
  a.send({ type: 'gain', value: 300 });
  a.send({ type: 'sampleRate', value: 2400000 });
  a.send({ type: 'dab', on: 1, channel: 5 });
  await sleep(400);
  ok(/icom: 'gain' ignored/.test(readLog()) && /icom: 'dab' ignored/.test(readLog()), 'IQ controls (gain, sampleRate, DAB) are swallowed, never reach dongle code');

  console.log('   the owner transmits');
  fake.stdin.write('tx 1\n');
  ok(await until(() => a.last('icom')?.tx === true, 3000), '★ the owner keys the radio: the icom status says tx');
  ok(await until(() => a.last('hwinfo')?.radio?.tx === true, 3000), 'and hwinfo radio.tx is true (the TX banner)');
  const specAtTx = a.spec || 0; await sleep(800);
  ok((a.spec || 0) - specAtTx <= 2, `the waterfall is frozen while he transmits (${(a.spec || 0) - specAtTx} frames)`);
  fake.stdin.write('tx 0\n');
  ok(await until(() => a.last('icom')?.tx === false, 3000), 'unkey: receive again');

  st = await stats();
  ok(st.violations === 0, `★★★ the fake radio saw ZERO disallowed frames (${st.frames} frames in total)`);
  ok(!/REFUSED a CI-V frame/.test(readLog()), 'and the server refused none of its own');
  a.close(); au.close(); b.close();
} finally {
  srv.kill('SIGTERM');
  await sleep(600);
  try { fake.stdin.write('quit\n'); } catch {}
  await sleep(200);
  fake.kill('SIGTERM');
}
// ── ★★ PHASE 2: THE RADIO'S AUDIO REACHES THE DECODERS. Real off-air RTTY (Stuart's library, JARTS 2026-10-18)
//    played as the "radio's USB audio" through the same capture path a real codec uses (paced at 48 kHz),
//    and the server's own FSK decoder must read it. Skipped — and said so — where the library is absent.
{
  const wav = path.join(os.homedir(), 'VibeSDR-test-audio', 'rtty', 'jarts_DK_599_usb.wav');
  if (!fs.existsSync(wav)) {
    console.log(`   not run — the RTTY decode phase needs ${wav}`);
  } else {
    console.log('── the radio\'s audio feeds the decoders (real RTTY through --icom-audio wav:) ──');
    const fake2 = spawn(FAKE, [], { stdio: ['pipe', 'pipe', 'pipe'] });
    let out2 = ''; fake2.stdout.on('data', (d) => { out2 += d.toString(); });
    await until(() => /FAKE_ICOM_PTY (\S+)/.test(out2), 3000);
    const pty2 = (out2.match(/FAKE_ICOM_PTY (\S+)/) || [])[1];
    const port2 = await freePort();
    const log2Path = path.join(dir, 'server2.log');
    const srv2 = spawn(BIN, ['--icom', pty2, '--icom-tty-ok', '--icom-audio', `wav:${wav}`, '--port', String(port2), '--users', '2', '--idle-grace', '0'],
                       { env, stdio: ['ignore', fs.openSync(log2Path, 'w'), fs.openSync(log2Path, 'a')] });
    try {
      ok(await until(() => getJson(port2, '/vibeserver.json').then((j) => !!j), 8000), 'second server up, audio from the WAV');
      const sid3 = 'icom0003-cccc';
      const sp = ws(port2, `/ws/user-spectrum?user_session_id=${sid3}&bins=256`);
      const au2 = ws(port2, `/ws/audio?user_session_id=${sid3}`);
      const dx = ws(port2, `/ws/dxcluster?user_session_id=${sid3}`);
      await sleep(800);
      // tones 1624 / 1801 Hz (README.txt in the library): centre 1712, shift 177, 45.45 Bd ITA2
      dx.send({ type: 'audio_extension_attach', extension_name: 'fsk', center_frequency: 1712, shift: 177, baud_rate: 45.45 });
      ok(await until(() => /599|DKWVM|D2VM/.test(dx.decoded || ''), 25000), `★ the server's RTTY decoder reads the radio's audio: "${(dx.decoded || '').replace(/\s+/g, ' ').slice(0, 60)}"`);
      sp.close(); au2.close(); dx.close();
    } finally {
      srv2.kill('SIGTERM'); await sleep(400);
      try { fake2.stdin.write('quit\n'); } catch {}
      fake2.kill('SIGTERM');
    }
  }
}
// ── ★★★ V12 PHASE 3: A DECODER ON AN ICOM (vibe_icom_decode.h). The owner's radio is in USB FIL2 on 14.074; FT8 off the
//    air (Stuart's library, 2026-10-25 20:44, 20 m) is the "radio's USB audio", cut to start on a slot boundary and
//    started on one. The listener tuned the dial and presses DIGITAL SPOTS, as on an SDR: the radio goes to FIL1 (the
//    widest), the DIAL STAYS, the audio stays audible, the known calls are decoded, and stopping puts FIL2 back. Then
//    RTTY pressed with the radio in CW: USB, the radio moved 1000 Hz below what the listener sees, the readout unchanged
//    — and CW and the dial come back on stop. A second listener without tune rights (owner lock) cannot start a decoder
//    that changes the mode; the SKIMMER is refused. Skipped — and said so — without ffmpeg or the library.
{
  const src = path.join(os.homedir(), 'VibeSDR-test-audio', 'ft8', 'ft8_20m_14074000_20261025T2044.wav');
  const wav = path.join(dir, 'ft8_48k.wav');
  let haveWav = false;
  if (fs.existsSync(src)) {
    // start_utc 1792961117.188 (its .log): 2.188 s into a slot — cut 12.812 s so the file starts ON a boundary
    try { (await import('node:child_process')).execFileSync('ffmpeg', ['-loglevel', 'error', '-y', '-ss', '12.812', '-t', '75', '-i', src,
      '-ar', '48000', '-ac', '1', '-c:a', 'pcm_s16le', wav]); haveWav = fs.existsSync(wav); } catch { haveWav = false; }
  }
  if (!haveWav) {
    console.log(`   not run — the Icom decoder phase needs ffmpeg and ${src}`);
  } else {
    console.log('── a decoder on the Icom: the radio set for it, the dial kept, put back after ──');
    const fake3 = spawn(FAKE, [], { stdio: ['pipe', 'pipe', 'pipe'] });
    let out3 = ''; fake3.stdout.on('data', (d) => { out3 += d.toString(); });
    await until(() => /FAKE_ICOM_PTY (\S+)/.test(out3), 3000);
    const pty3 = (out3.match(/FAKE_ICOM_PTY (\S+)/) || [])[1];
    const stats3 = async () => {
      const n0 = out3.length; fake3.stdin.write('stats\n');
      await until(() => out3.slice(n0).includes('FAKE_ICOM_STATS'), 2000);
      const m = out3.slice(n0).match(/FAKE_ICOM_STATS (.*)/);
      const o = {}; if (m) for (const kv of m[1].trim().split(' ')) { const [k, v] = kv.split('='); o[k] = Number(v); }
      return o;
    };
    fake3.stdin.write('mode 1 0 2\n');   // ★ the owner's front panel: USB, DATA off, FIL2
    ok(await until(async () => { const s = await stats3(); return s.mode === 1 && s.data === 0 && s.fil === 2; }, 2000), 'the radio starts in USB FIL2 on 14.074');
    const port3 = await freePort();
    const log3Path = path.join(dir, 'server3.log');
    // ★ The wav source starts at open (~0.3 s after spawn): spawn just before a 15 s boundary so the file's slot is the clock's.
    const lead = 400, wait = (15000 - (Date.now() % 15000) - lead + 15000) % 15000;
    await sleep(wait);
    const srv3 = spawn(BIN, ['--icom', pty3, '--icom-tty-ok', '--icom-audio', `wav:${wav}`, '--port', String(port3), '--users', '4', '--idle-grace', '0'],
                       { env, stdio: ['ignore', fs.openSync(log3Path, 'w'), fs.openSync(log3Path, 'a')] });
    const read3 = () => fs.readFileSync(log3Path, 'utf8');
    try {
      ok(await until(() => getJson(port3, '/vibeserver.json').then((j) => !!j), 8000), 'server up, the radio\'s audio from the FT8 recording');
      const sidA = 'icom0004-dddd', sidB = 'icom0005-eeee';
      const spA = ws(port3, `/ws/user-spectrum?user_session_id=${sidA}&bins=256`);
      const auA = ws(port3, `/ws/audio?user_session_id=${sidA}`);
      const dxA = ws(port3, `/ws/dxcluster?user_session_id=${sidA}`);
      const spB = ws(port3, `/ws/user-spectrum?user_session_id=${sidB}&bins=256`);
      const dxB = ws(port3, `/ws/dxcluster?user_session_id=${sidB}`);
      await until(() => spA.last('config') && spB.last('config'), 4000);
      // ── a listener without tune rights (the owner lock, on by default) ──
      dxB.send({ type: 'subscribe_digital_spots' });
      ok(await until(() => dxB.last('decoder_refused')?.reason === 'icom', 3000),
         `★ a listener without tune rights cannot start spots that change the radio's filter: "${dxB.last('decoder_refused')?.message || ''}"`);
      ok((await stats3()).fil === 2, 'and the radio is untouched (FIL2)');
      dxB.send({ type: 'audio_extension_attach', extension_name: 'cwskim', width: 48000 });
      ok(await until(() => dxB.last('decoder_refused')?.ext === 'cwskim', 3000), `the CW SKIMMER is refused on an Icom: "${dxB.last('decoder_refused')?.message || ''}"`);
      // ── the owner (signed in) presses DIGITAL SPOTS ──
      spA.send({ type: 'admin_unlock', nonce: '', token: '' });
      ok(await until(() => spA.txt.some((m) => m.t.includes('"type":"admin","ok":true')), 2000), 'the owner signs in');
      const au0 = auA.binBytes;
      dxA.send({ type: 'subscribe_digital_spots' });
      ok(await until(async () => { const s = await stats3(); return s.fil === 1 && s.mode === 1; }, 3000), '★ spots started: the radio goes to FIL1 (the widest), still USB');
      let s3 = await stats3();
      ok(s3.freq === 14074000 && s3.data === 0, `★ the dial is NOT moved (${s3.freq}), and DATA is not switched on`);
      const spots = () => dxA.txt.map((m) => { try { const j = JSON.parse(m.t); return j.type === 'digital_spot' ? j.data : null; } catch { return null; } })
        .filter((d) => d && d.mode === 'FT8');
      ok(await until(() => new Set(spots().map((d) => d.callsign)).size >= 2, 50000),
         `★ FT8 decoded from the radio's audio: ${[...new Set(spots().map((d) => d.callsign))].slice(0, 6).join(' ')}`);
      ok(spots().length > 0 && spots().every((d) => d.frequency >= 14074000 && d.frequency <= 14077200 && d.band === '20m'),
         `each spot filed on the radio's dial + its audio offset (${spots().slice(0, 3).map((d) => d.frequency).join(', ')})`);
      ok(auA.binBytes - au0 > 20000, `and the listener still HEARS the radio while it decodes (${auA.binBytes - au0} audio bytes), as on an SDR`);
      dxA.send({ type: 'unsubscribe_digital_spots' });
      ok(await until(async () => (await stats3()).fil === 2, 3000), '★ spots stopped: FIL2 is back');
      s3 = await stats3();
      ok(s3.mode === 1 && s3.data === 0 && s3.freq === 14074000, 'USB, DATA off, 14.074 — as the owner left it');
      // ── RTTY pressed with the radio in CW: the dial means the SIGNAL there ──
      fake3.stdin.write('mode 3 0 2\n');   // CW FIL2, on 14.074
      ok(await until(() => spA.last('config')?.radioMode === 'cw', 3000), 'the owner puts the radio in CW FIL2');
      dxA.send({ type: 'audio_extension_attach', extension_name: 'fsk', center_frequency: 1000, shift: 170, baud_rate: 45.45 });
      ok(await until(async () => { const s = await stats3(); return s.mode === 1 && s.fil === 1 && s.freq === 14073000; }, 3000),
         '★ RTTY from CW: USB FIL1, the radio 1000 Hz below the dial so the signal tuned comes out at the decoder\'s 1000 Hz');
      ok(await until(() => spA.last('config')?.vfo === 14074000 && spA.last('icom')?.freq === 14074000, 3000),
         `★ and the readout is what the listener tuned: config vfo ${spA.last('config')?.vfo}, status ${spA.last('icom')?.freq}`);
      spA.send({ type: 'tune', frequency: 14075000 });
      ok(await until(async () => (await stats3()).freq === 14074000, 3000), 'a tune to 14.075 while it runs moves the radio to 14.074 (the pitch kept)');
      ok(await until(() => spA.last('config')?.vfo === 14075000, 3000), 'and the readout says 14.075');
      dxA.send({ type: 'audio_extension_detach' });
      ok(await until(async () => { const s = await stats3(); return s.mode === 3 && s.fil === 2 && s.freq === 14075000; }, 3000),
         '★ RTTY stopped: CW FIL2 back, the radio on the 14.075 the listener sees');
      ok(await until(() => spA.last('config')?.vfo === 14075000 && spA.last('config')?.radioMode === 'cw', 3000), 'and the readout did not jump');
      ok(/icom decoder: spots — the radio to usb FIL1/.test(read3()) && /icom decoder: fsk stopped — the radio back to cw FIL2/.test(read3()), 'the server log says what it set and what it put back');
      const st3 = await stats3();
      ok(st3.violations === 0, `★★★ the fake radio saw ZERO disallowed frames (${st3.frames} frames)`);
      for (const s of [spA, auA, dxA, spB, dxB]) s.close();
    } finally {
      srv3.kill('SIGTERM'); await sleep(400);
      try { fake3.stdin.write('quit\n'); } catch {}
      fake3.kill('SIGTERM');
    }
  }
}
if (fail) console.log(`   server log: ${logPath}\n   fake log tail: ${fakeOut.slice(-600)}`);
console.log(`icom end-to-end: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
