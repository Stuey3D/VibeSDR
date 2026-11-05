// test-server-icom.mjs — the REAL server against a pretend IC-7300, end to end (docs/ICOM-SUPPORT-PLAN.md).
//
// tools/icom/fake-icom serves CI-V on a pty with a synthetic 475-point scope; vibeserver opens it as an Icom
// (`--icom <pty> --icom-tty-ok`, test audio `tone`). Then, over the same sockets a client uses:
//   • hwinfo says driver "icom" with the radio's own lists, and nothing IQ-shaped;
//   • SPEC frames arrive at the radio's rate over the radio's edges, and config's span matches them;
//   • ★ an Icom tunes like any radio (2026-11-04): a listener's tune moves it (the tune lock is OFF by default);
//     no RADIO ON / RADIO OFF in the clients (the owner's POWER ON / OFF are HTTP, for the front door); with --icom-tune-lock only the admin tunes; with ask before
//     tuning a second listener is held back like on any shared dial; a radio started SWITCHED OFF waits;
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
  ok(r.ownerLock === false, '★ the tune lock is OFF by default — the ordinary dial rules');
  ok(r.power && r.power.autoOn === false && r.power.autoOff === false && r.power.offDelayMin === 5 && r.power.offInS === -1,
     `hwinfo carries the power facts (auto power-on off, so no auto power-off; delay 5; none pending) ${JSON.stringify(r.power)}`);
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

  console.log('   RF settings follow the SDRs\' gain rules');
  ok(r.rfOpen === true, 'hwinfo rfOpen: a personal (unlocked) receiver — listeners may change the RF settings, like SDR gain');
  a.send({ type: 'icom_control', att: 20 });
  ok(await until(async () => (await stats()).att === 0x20, 3000), '★ a listener (not admin) switches the ATT on — like SDR gain on a personal receiver');
  a.send({ type: 'icom_control', preamp: 2, rfGain: 200, agc: 'slow' });
  ok(await until(async () => { const s = await stats(); return s.preamp === 2 && s.rf === 200 && s.agc === 3; }, 3000), '  … and the preamp, RF gain and AGC');
  a.send({ type: 'icom_control', att: 0, preamp: 1, rfGain: 255, agc: 'mid' });
  await until(async () => { const s = await stats(); return s.att === 0 && s.preamp === 1; }, 3000);
  console.log('   tuning follows the ordinary dial rules; the power keys are the owner\'s');
  a.send({ type: 'tune', frequency: 7050000, mode: 'usb' });
  ok(await until(async () => (await stats()).freq === 7050000, 3000), "★ a listener (not admin) tunes the radio — like any other radio");
  // ★★★ 2026-11-04 (Saber's IC-7300: "AM sounds like LSB"): a tune's mode reaches the radio only when it is a CHOICE.
  {
    const m0 = (await stats()).mode;
    a.send({ type: 'tune', frequency: 7051000, mode: m0 === 2 ? 'usb' : 'am' });     // a STEP's echo of a (stale) mode
    ok(await until(async () => (await stats()).freq === 7051000, 3000), '  a step without modeSet still tunes the radio');
    await sleep(800);
    ok((await stats()).mode === m0, `★ …and does NOT switch its mode (stayed ${m0}) — an old page's stale mode cannot flip it`);
    a.send({ type: 'tune', frequency: 7051000, mode: 'am', modeSet: 1 });           // the mode menu / a bookmark (beta64 client)
    ok(await until(async () => (await stats()).mode === 2, 3000), '★ a tune marked modeSet changes the mode (AM)');
    a.send({ type: 'mode', mode: 'usb' });                                           // an explicit mode message is always a choice
    ok(await until(async () => (await stats()).mode === 1, 3000), '★ a {type:"mode"} message changes the mode (USB)');
    a.send({ type: 'tune', frequency: 7050000 });
    await until(async () => (await stats()).freq === 7050000, 3000);
  }
  // ★★ 2026-11-05: NO RADIO ON / RADIO OFF IN THE CLIENTS. An old page's icom_power is refused in words for everybody,
  //    admin included, and nothing reaches the radio; the owner's keys are POST /vibeserver/icom/power (the front door).
  ok(a.last('hwinfo')?.radio?.power?.canSwitch === false, '★ hwinfo power.canSwitch is false — no client draws RADIO ON / OFF');
  a.send({ type: 'icom_power', action: 'off' });
  await sleep(800);
  let st = await stats();
  ok(st.powered === 1 && st.offs === 0, '★ an icom_power message (an older page) is ignored — the radio is still on');
  ok(a.txt.some((m) => m.t.includes('"notice"') && m.t.includes('settings on the Setup page')), 'and the sender is told why, in words');
  a.send({ type: 'admin_unlock', nonce: '', token: '' });
  ok(await until(() => a.txt.some((m) => m.t.includes('"type":"admin","ok":true')), 2000), 'admin signs in (no password set: granted)');
  a.send({ type: 'icom_power', action: 'off' });
  await sleep(800);
  ok((await stats()).offs === 0, '★ …the admin\'s too: the socket path is retired');
  console.log('   POWER OFF / POWER ON (the owner, front door HTTP)');
  const post = (q) => new Promise((res) => {
    const rq = http.request({ host: '127.0.0.1', port, path: `/vibeserver/icom/power?action=${q}`, method: 'POST' }, (r) => {
      let b = ''; r.on('data', (d) => { b += d; }); r.on('end', () => { try { res({ code: r.statusCode, j: JSON.parse(b) }); } catch { res({ code: r.statusCode, j: null }); } });
    });
    rq.on('error', () => res({ code: 0, j: null })); rq.end();
  });
  let pr = await post('off');
  ok(pr.code === 200 && pr.j?.ok === true, `POST action=off → 200 ok (${pr.code})`);
  ok(await until(async () => { const s = await stats(); return s.powered === 0 && s.offs === 1; }, 3000), '★ POWER OFF: 18 00 sent once, the radio is off');
  ok(await until(() => a.last('hwinfo')?.radio?.linkWhy === 'Switched off by the owner', 5000), `every listener is told: "${a.last('hwinfo')?.radio?.linkWhy}"`);
  ok(/the owner pressed POWER OFF/.test(readLog()), 'the press is logged plainly');
  await sleep(4000);
  st = await stats();
  ok(st.wakes === 0 && st.powered === 0, 'it stays off (no automatic power-on without the option)');
  ok(srv.exitCode === null, 'and the server is still running (no exit, no restart loop)');
  pr = await post('on');
  ok(pr.code === 200 && pr.j?.ok === true, 'POST action=on → 200 ok');
  ok(await until(async () => (await stats()).wakes === 1, 4000), 'POWER ON: the FE preamble + 18 01 is sent once');
  ok(await until(() => a.last('hwinfo')?.radio?.linkUp === true, 15000), '★ the radio boots, the PREFLIGHT runs again, and it is back');
  ok(await until(() => a.last('hwinfo')?.radio?.preflight?.passed === true, 3000), '  … with the preflight passed');
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
  ok(st.offs === 1, `★★ exactly one 18 00 in the whole run — the owner's POWER OFF (${st.offs})`);
  ok(!/REFUSED a CI-V frame/.test(readLog()), 'and the server refused none of its own');
  a.close(); au.close(); b.close();
} finally {
  srv.kill('SIGTERM');
  await sleep(600);
  try { fake.stdin.write('quit\n'); } catch {}
  await sleep(200);
  fake.kill('SIGTERM');
}
// ── ★★ PHASE 1b: THE TUNE LOCK ON, ASK BEFORE TUNING, AND A RADIO STARTED SWITCHED OFF ──
async function phase(fakeArgs, srvArgs, body, cfgObj = null) {
  const f = spawn(FAKE, fakeArgs, { stdio: ['pipe', 'pipe', 'pipe'] });
  let out = ''; f.stdout.on('data', (d) => { out += d.toString(); }); f.stderr.on('data', (d) => { out += d.toString(); });
  await until(() => /FAKE_ICOM_PTY (\S+)/.test(out), 3000);
  const p = (out.match(/FAKE_ICOM_PTY (\S+)/) || [])[1];
  const st = async () => {
    const n0 = out.length; f.stdin.write('stats\n');
    await until(() => out.slice(n0).includes('FAKE_ICOM_STATS'), 2000);
    const m = out.slice(n0).match(/FAKE_ICOM_STATS (.*)/);
    const o = {}; if (m) for (const kv of m[1].trim().split(' ')) { const [k, v] = kv.split('='); o[k] = Number(v); }
    return o;
  };
  const pt = await freePort();
  const lp = path.join(dir, `server-${pt}.log`);
  let penv = env;
  if (cfgObj) {   // ★ a phase's own config file (the owner's AGC lock lives there, as on a real server)
    const cp = path.join(dir, `config-${pt}.json`);
    fs.writeFileSync(cp, JSON.stringify(cfgObj));
    penv = { ...env, VIBESERVER_CONFIG: cp };
  }
  const sv = spawn(BIN, ['--icom', p, '--icom-tty-ok', '--icom-audio', 'tone', '--port', String(pt), '--idle-grace', '0', ...srvArgs],
                   { env: penv, stdio: ['ignore', fs.openSync(lp, 'w'), fs.openSync(lp, 'a')] });
  try {
    ok(await until(() => getJson(pt, '/vibeserver.json').then((j) => !!j), 8000), `server up (${srvArgs.join(' ')}${fakeArgs.length ? ', fake ' + fakeArgs.join(' ') : ''})`);
    await body(pt, st, f, sv, () => fs.readFileSync(lp, 'utf8'));
  } finally {
    sv.kill('SIGTERM'); await sleep(400);
    try { f.stdin.write('quit\n'); } catch {}
    f.kill('SIGTERM');
  }
}
console.log('── "Only I may tune this radio" ON ──');
await phase([], ['--users', '4', '--icom-tune-lock'], async (pt, st) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=lock0001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.ownerLock === true, 4000), 'hwinfo says the tune lock is on');
  x.send({ type: 'tune', frequency: 7074000, mode: 'usb' });
  await sleep(800);
  ok((await st()).freq === 14074000, '★ a listener\'s tune is REFUSED with the lock on');
  ok(x.txt.some((m) => m.t.includes('"notice"') && m.t.includes('only they tune it')), 'and they are told why, in words');
  x.send({ type: 'admin_unlock', nonce: '', token: '' });
  await until(() => x.txt.some((m) => m.t.includes('"type":"admin","ok":true')), 2000);
  x.send({ type: 'tune', frequency: 7074000, mode: 'usb' });
  ok(await until(async () => (await st()).freq === 7074000, 3000), 'the admin tunes it');
  x.close();
});
console.log('── a LOCKED (shared) Icom: RF settings are the owner\'s, as SDR gain is ──');
await phase([], ['--users', '4', '--lock-freq', '14074000', '--admin-pass', 'icomtest'], async (pt, st) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=rfl00001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.rfOpen === false, 4000), 'hwinfo rfOpen false on a locked receiver');
  x.send({ type: 'icom_control', att: 20 });
  await sleep(800);
  ok((await st()).att === 0, '★ a listener\'s ATT is refused on a locked receiver (sharedGate, as for SDR gain)');
  const nonce = (await getJson(pt, '/vibeserver/auth'))?.nonce || '';
  x.send({ type: 'admin_unlock', nonce, token: crypto.createHmac('sha256', 'icomtest').update(nonce).digest('hex') });
  ok(await until(() => x.txt.some((m) => m.t.includes('"type":"admin","ok":true')), 3000), 'the owner signs in');
  x.send({ type: 'icom_control', att: 20 });
  ok(await until(async () => (await st()).att === 0x20, 3000), 'the admin changes it');
  x.close();
});
console.log('── the owner\'s AGC lock ──');
await phase([], ['--users', '1', '--agc-lock'], async (pt, st) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=agl00001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.rfOpen === false, 4000), 'hwinfo rfOpen false under the AGC lock');
  x.send({ type: 'icom_control', att: 20, rfGain: 100 });
  await sleep(800);
  let s = await st();
  ok(s.att === 0 && s.rf === 255, '★ with the AGC locked, a listener\'s ATT and RF gain are refused (the HF+ rule)');
  ok(x.txt.some((m) => m.t.includes('fixed this radio') && m.t.includes('gain')), '  … and told why');
  x.send({ type: 'icom_control', agc: 'slow' });
  ok(await until(async () => (await st()).agc === 3, 3000), '  … the AGC speed is still theirs (the AGC\'s own setting, not a way past it)');
  x.send({ type: 'admin_unlock', nonce: '', token: '' });
  await until(() => x.txt.some((m) => m.t.includes('"type":"admin","ok":true')), 2000);
  x.send({ type: 'icom_control', att: 20 });
  ok(await until(async () => (await st()).att === 0x20, 3000), 'the signed-in admin is exempt, as on every radio');
  x.close();
});
console.log('── a shared Icom with ask before tuning ──');
await phase([], ['--users', '4', '--ask-before-tuning'], async (pt, st) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=ask00001-aaaa&bins=256');
  await until(() => x.last('config'), 4000);
  x.send({ type: 'tune', frequency: 7040000, mode: 'usb' });
  ok(await until(async () => (await st()).freq === 7040000, 3000), 'alone on the shared dial: the listener tunes');
  await sleep(300);
  const y = ws(pt, '/ws/user-spectrum?user_session_id=ask00002-bbbb&bins=256');
  ok(await until(() => y.last('dial')?.askFirst === true, 5000), 'a second listener arrives: ask before tuning is in force');
  y.send({ type: 'tune', frequency: 7074000, mode: 'usb' });
  y.send({ type: 'icom_control', fil: 3 });
  y.send({ type: 'icom_control', att: 20 });
  await sleep(1000);
  const s = await st();
  ok(s.freq === 7040000, `★ the newcomer\'s tune is held back like on any shared dial (radio on ${s.freq})`);
  ok(s.att === 0, '★ and their ATT too — RF settings are gated by ask before tuning like SDR gain');
  ok(y.txt.some((m) => m.t.includes('Ask in chat before tuning')), '  … and told to ask, in words (icom_control is gated too)');
  x.close(); y.close();
});
console.log('── started with the radio SWITCHED OFF ──');
await phase(['--off'], ['--users', '2'], async (pt, st, f, sv, rl) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=off00001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.linkUp === false, 5000), '★ the server runs with the radio off (no exit)');
  const why = x.last('hwinfo')?.radio?.linkWhy || '';
  ok(/switched off/.test(why) && /when it is turned on/.test(why), `listeners are told: "${why}"`);
  ok(/MENU > SET > Connectors > CI-V/.test(x.last('hwinfo')?.radio?.linkHint || ''), 'and the CI-V hint comes second');
  await sleep(5000);
  ok((await st()).wakes === 0 && sv.exitCode === null, 'option off: no power command, still running');
  ok((rl().match(/waiting for it/g) || []).length <= 2, 'one quiet line for the wait, not one every retry');
  f.stdin.write('power 1\n');
  ok(await until(() => x.last('hwinfo')?.radio?.linkUp === true, 10000), '★ switched on by hand: VibeServer starts it (preflight, scope)');
  ok(await until(() => (x.spec || 0) > 3, 4000), '  … and the waterfall streams');
  x.close();
});
console.log('── radio off, "Power on the radio over CI-V" ticked ──');
await phase(['--off'], ['--users', '2', '--icom-power-on'], async (pt, st) => {
  await sleep(5000);
  ok((await st()).wakes === 0, '★ nobody connected: nothing is sent (a radio that is off stays off until it is wanted)');
  const x = ws(pt, '/ws/user-spectrum?user_session_id=pwr00001-aaaa&bins=256');
  ok(await until(async () => (await st()).wakes === 1, 5000), '★ a listener connects: the server sends the power-on (FE preamble + 18 01) once');
  ok(await until(() => /Switching the radio on/.test(x.last('hwinfo')?.radio?.linkWhy || '') || x.last('hwinfo')?.radio?.linkUp === true, 5000),
     '  … and they are told "Switching the radio on"');
  ok(await until(() => x.last('hwinfo')?.radio?.linkUp === true, 15000), '  … the radio boots and runs, preflight passed');
  ok((await st()).offs === 0, '★ no 18 00 was ever sent');
  x.close();
});

// ── ★★ AUTOMATIC POWER-OFF (Stuart, 2026-11-05): "How long after the last user leaves". --icom-off-delay-scale 4 makes
//    one minute last 4 s, so "1 min" here is 4 s. ──
console.log('── automatic power-off: 1 min (4 s in this test) after the last listener leaves ──');
await phase([], ['--users', '4', '--icom-power-on', '--icom-off-delay', '1', '--icom-off-delay-scale', '4'], async (pt, st, f, sv, rl) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=aof00001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.linkUp === true, 8000), 'a listener is on the radio');
  const pw = x.last('hwinfo')?.radio?.power || {};
  ok(pw.autoOn === true && pw.autoOff === true && pw.offDelayMin === 1, `hwinfo: power-on, automatic power-off, 1 min (${JSON.stringify(pw)})`);
  await sleep(7000);
  ok((await st()).offs === 0, '★ never switched off while somebody is connected (7 s > the 4 s delay)');
  x.close();
  const t0 = Date.now();
  await sleep(2000);
  ok((await st()).offs === 0, '★ not before the delay (2 s after the last listener left)');
  const hw = await getJson(pt, '/vibeserver/hardware');
  const left = hw?.radio?.power?.offInS;
  ok(typeof left === 'number' && left >= 0 && left <= 4, `the Setup page sees the countdown (offInS ${left})`);
  ok(await until(async () => (await st()).offs === 1, 6000), `★ switched off after the delay (${((Date.now() - t0) / 1000).toFixed(1)} s)`);
  ok(await until(async () => (await st()).powered === 0, 2000), '  … the radio is off');
  ok(/automatic power-off/.test(rl()), '  … logged plainly');
  const y = ws(pt, '/ws/user-spectrum?user_session_id=aof00002-bbbb&bins=256');
  ok(await until(async () => (await st()).wakes === 1, 5000), '★ the next listener switches it back on (18 01)');
  ok(await until(() => y.last('hwinfo')?.radio?.linkUp === true, 15000), '  … and it runs');
  y.close();
  await sleep(2000);
  const z = ws(pt, '/ws/user-spectrum?user_session_id=aof00003-cccc&bins=256');   // ★ back inside the wait
  ok(await until(() => z.last('hwinfo')?.radio?.linkUp === true, 3000), 'somebody connects again inside the wait');
  await sleep(6000);
  ok((await st()).offs === 1, '★ the re-join cancelled the power-off (still one 18 00 in the run)');
  z.close();
  ok(await until(async () => (await st()).offs === 2, 8000), 'and with them gone too, it switches off after the delay');
});
console.log('── automatic power-off: Instant ──');
await phase([], ['--users', '4', '--icom-power-on', '--icom-off-delay', '0'], async (pt, st) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=ins00001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.linkUp === true, 8000), 'a listener is on the radio');
  await sleep(2500);
  ok((await st()).offs === 0, 'never while they are connected');
  x.close();
  ok(await until(async () => (await st()).offs === 1, 3000), '★ Instant: switched off within a second or two of the last listener leaving');
});
console.log('── power-on ticked, automatic power-off unticked ──');
await phase([], ['--users', '4', '--icom-power-on', '--icom-no-auto-off', '--icom-off-delay', '0'], async (pt, st) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=nao00001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.linkUp === true, 8000), 'a listener is on the radio');
  ok(x.last('hwinfo')?.radio?.power?.autoOff === false, 'hwinfo: automatic power-off is not in effect');
  x.close();
  await sleep(5000);
  ok((await st()).offs === 0, '★ never an 18 00 without "Automatically power off the radio when not in use"');
});
console.log('── ★★ auto power-on wins over the owner\'s POWER OFF (Stuart 2026-11-05: "auto power on should power on the radio") ──');
await phase([], ['--users', '4', '--icom-power-on', '--icom-no-auto-off'], async (pt, st) => {
  const post = (q) => new Promise((res) => {
    const rq = http.request({ host: '127.0.0.1', port: pt, path: `/vibeserver/icom/power?action=${q}`, method: 'POST' }, (r) => {
      let b = ''; r.on('data', (d) => { b += d; }); r.on('end', () => res(r.statusCode)); });
    rq.on('error', () => res(0)); rq.end();
  });
  const x = ws(pt, '/ws/user-spectrum?user_session_id=own00001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.linkUp === true, 8000), 'a listener is on the radio');
  ok(await post('off') === 200, 'the owner presses POWER OFF');
  ok(await until(async () => (await st()).offs === 1, 3000), 'the radio is switched off (one 18 00)');
  await sleep(3000);
  ok((await st()).wakes === 0, '★ no instant re-wake under the owner\'s finger while that listener stays');
  const radios = await getJson(pt, '/vibeserver/radios');
  ok(!JSON.stringify(radios || {}).includes('"switchedOff":true'), '★ the radio list does NOT grey it — auto power-on means available');
  x.close();
  await sleep(1500);
  const y = ws(pt, '/ws/user-spectrum?user_session_id=own00002-bbbb&bins=256');
  ok(await until(async () => (await st()).wakes >= 1, 6000), '★★ the next listener to arrive WAKES it (FE preamble + 18 01)');
  ok(await until(() => y.last('hwinfo')?.radio?.linkUp === true, 15000), 'and the radio is back for them');
  y.close();
});
console.log('── automatic power-off ticked, power-on NOT ticked ──');
await phase([], ['--users', '4', '--icom-off-delay', '0'], async (pt, st) => {
  const x = ws(pt, '/ws/user-spectrum?user_session_id=npo00001-aaaa&bins=256');
  ok(await until(() => x.last('hwinfo')?.radio?.linkUp === true, 8000), 'a listener is on the radio');
  x.close();
  await sleep(5000);
  const s2 = await st();
  ok(s2.offs === 0 && s2.wakes === 0, '★ no power command at all without "Power on the radio over CI-V" (nothing could switch it back on)');
});

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
    const srv3 = spawn(BIN, ['--icom', pty3, '--icom-tty-ok', '--icom-audio', `wav:${wav}`, '--port', String(port3), '--users', '4', '--idle-grace', '0', '--icom-tune-lock'],
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
      // ── a listener without tune rights (Only I may tune this radio ticked; not the default since 2026-11-04) ──
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
