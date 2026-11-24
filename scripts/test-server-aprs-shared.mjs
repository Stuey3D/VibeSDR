// test-server-aprs-shared.mjs — APRS on the ONE-PIPELINE receiver through the REAL server (2026-11-23, the beta91 on-air
// fault: "no packets yet" on the XCover's Airspy, the Lenovo's RSP1A and the Pi 500's V4L, all one-pipeline radios).
//
// test-server-ais-aprs.mjs and test-server-aprs-capture.mjs feed a clean, always-keyed clip (0.4 of full scale, almost no
// noise) to one listener with the old {"extension_name":"aprs"} attach or a per-listener VFO. This one feeds the off-air
// shape (`test-aprs-onair --write-clip`: a tracker KEYS UP for each packet — full-scale FM hiss between them — Bell 202,
// pre-emphasised, at a CNR in 12.5 kHz) and attaches as the clients do today ({baud_rate:1200, packet:'auto'}: 1200 on the
// audio + 1200 narrow and 9600 on the channel IQ, FX.25, IL2P), on every kind of receiver:
//   A. one listener on a one-pipeline radio (users 1 — the Pi 500 V4L's shape);
//   B. a SHARED DIAL (users 4, unlocked — the XCover's and the Lenovo's), with a second listener on it;
//   C. a listener's own VFO (users 4, range-locked) — the per-listener path, for comparison;
//   D. B with a STRONG NEIGHBOUR 60 kHz up, 30 dB over the packets (the S9 burst at 144.860 in the failing screenshots);
//   E. THE SILENT MUTE AS THE iOS APP DOES IT on a remote server (setMuted → disconnectForPause: the audio socket closed, the
//      spectrum socket paused) with the DX scanner on and its wait shortened: the attached decoder IS somebody — the
//      scanner never sweeps, the capture never parks, the packets keep coming;
//   F. the XCover sequence: a SATELLITE pick, then leaving it, the dial put on 144.800 NFM, APRS attached by hand.
// Every row: N packets in, ≥ N−1 decoded, no wrong frames.
// Silent: an audio socket is opened only so a listener has audio, and its frames are dropped unread.
//   VIBESERVER_BIN=…/vibeserver VIBE_APRS_ONAIR_BIN=…/test-aprs-onair node scripts/test-server-aprs-shared.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
const GEN = process.env.VIBE_APRS_ONAIR_BIN || '';
if (!BIN || !fs.existsSync(BIN) || !GEN || !fs.existsSync(GEN)) {
  console.log('   not run — set VIBESERVER_BIN and VIBE_APRS_ONAIR_BIN (the test-aprs-onair binary, which writes the clips)');
  process.exit(3);
}
// ★ VIBE_APRS_RATE: the capture rate (1 200 000 default; 2 400 000, and 960 000 — an Airspy HF+'s class of rate — 2026-11-24)
const RATE = Number(process.env.VIBE_APRS_RATE || 1200000), DIAL = 144800000, N = 10;
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const until = async (f, ms) => { for (let i = 0; i < ms / 200; i++) { if (await f()) return true; await sleep(200); } return !!(await f()); };

function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { txt: [], open: false };
  let hs = false, buf = Buffer.alloc(0);
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('close', () => { st.open = false; });
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
      if (op === 0x1 && st.txt.length < 20000) st.txt.push(p.toString());
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
  return st;
}
const jsons = (st, type) => st.txt.filter((t) => t.includes(`"type":"${type}"`)).map((t) => { try { return JSON.parse(t); } catch { return null; } }).filter(Boolean);
/** The packets heard, by number ("on-air test #NNN"), and frames that are not ours. */
function heard(dx) {
  const got = new Set(); let wrong = 0;
  for (const p of jsons(dx, 'aprs_packet')) { const m = /on-air test #(\d+)/.exec(p.data.raw || ''); if (m) got.add(Number(m[1])); else wrong++; }
  return { got, wrong };
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-aprs-shared-'));
const clip = (name, cnr, nbOff = 0, nbDb = 0, dev = 3000, err = 0, tilt = 5) => {
  const f = path.join(tmp, `${name}.cu8`);
  const r = spawnSync(GEN, ['--write-clip', f, String(RATE), String(cnr), String(N), String(nbOff), String(nbDb), String(dev), String(err), String(tilt)], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`test-aprs-onair --write-clip failed: ${r.stderr}`);
  return f;
};

/** vibesdr.net's sats.json fixture with every epoch moved to an hour ago (test-server-sat.mjs freshSats) — ISS, its APRS. */
function freshSats() {
  const iso = new Date(Date.now() - 3600e3).toISOString().replace('Z', '');
  const d = JSON.parse(fs.readFileSync(path.join(root, 'vibeserver/testdata-sat/sats-6digit-2026-11-19.json'), 'utf8'));
  for (const s of d.satellites) if (s.omm) s.omm.EPOCH = iso;
  for (const g of Object.keys(d.orbits)) d.orbits[g].fetched = new Date(Date.now() - 2 * 3600e3).toISOString();
  return JSON.stringify(d);
}
async function server(clipFile, tag, { users = 1, locked = false, dxWait = 0, sats = false } = {}) {
  const dir = fs.mkdtempSync(path.join(tmp, `${tag}-`));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  if (sats) {   // ★ the orbits, and the sync told it has just checked — so it asks vibesdr.net nothing
    fs.mkdirSync(path.join(dir, 'data/sat'));
    fs.writeFileSync(path.join(dir, 'data/sat', 'sats.json'), freshSats());
    const t = Math.floor(Date.now() / 1000);
    fs.writeFileSync(path.join(dir, 'data/sat', 'sync-state'), `checked ${t}\nattempt ${t}\nseed 0\n`);
  }
  const radio = { serial: `${tag}-test`, label: 'Test radio', enabled: true, configured: true, users };
  if (dxWait) radio.dxScan = JSON.stringify({ on: true, bands: 'fm', waitMin: 5 });
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
    configured: true, name: 'APRS shared test', locator: 'IO92nh', adminPass: 'aprs-shared-test-admin', radios: [radio],
  }));
  const rtlPort = await freePort(), port = await freePort();
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort), '--rate', String(RATE),
                                       '--hf', clipFile, '--hf-centre', String(DIAL)], { stdio: 'ignore' });
  await sleep(400);
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...(dxWait ? { VIBESERVER_DX_WAIT_SEC: String(dxWait), VIBESERVER_DX_DWELL_SEC: '2' } : {}) };
  const logPath = path.join(dir, 'server.log');
  const log = fs.openSync(logPath, 'w');
  const args = ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(DIAL), '--mode', 'nfm', '--users', String(users)];
  if (locked) args.push('--lock-freq', String(DIAL));
  const srv = spawn(BIN, args, { env, stdio: ['ignore', log, log] });
  const status = async () => { try { const r = await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) }); return await r.json(); } catch { return null; } };
  await until(async () => !!(await status()), 15000);
  return { port, dir, logText: () => fs.readFileSync(logPath, 'utf8'), stop: () => { srv.kill('SIGTERM'); rtl.kill('SIGTERM'); } };
}

/** A listener as the page / the app is one: spectrum, audio, decoder sockets; tuned to 144.800 NFM. */
async function listener(S, sid) {
  const sp = ws(S.port, `/ws/user-spectrum?user_session_id=${sid}&mode=binary8&bins=256&proto=1`);
  await sleep(300);
  const au = ws(S.port, `/ws/audio?user_session_id=${sid}&proto=1`);
  const dx = ws(S.port, `/ws/dxcluster?user_session_id=${sid}`);
  await sleep(600);
  return { sp, au, dx, close: () => { sp.close(); au.close(); dx.close(); } };
}
const ATTACH = { type: 'audio_extension_attach', extension_name: 'aprs', baud_rate: 1200, packet: 'auto' };   // src/services/packet.ts packetParams('auto')

/** Wait for one whole pass of the clip (it loops) and judge it. */
async function judge(dx, what, ms = 30000) {
  await until(() => heard(dx).got.size >= N, ms);
  const h = heard(dx);
  ok(h.got.size >= N - 1 && h.wrong === 0, `${what}: ${h.got.size} of ${N} packets${h.wrong ? `, ${h.wrong} WRONG` : ''}`);
  return h;
}

const clean = clip('clean', 15);
const neighbour = clip('neighbour', 15, 60000, 30);

{
  console.log('── A. one listener, one-pipeline radio (users 1), the clients\' AUTO attach ──');
  const S = await server(clean, 'one', { users: 1 });
  try {
    const L = await listener(S, 'aprsone01');
    L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L.dx.send(ATTACH);
    await judge(L.dx, 'users 1');
    ok(/decoder attached: aprs AUTO \(1200 \+ 9600\)/.test(S.logText()), 'the server ran AUTO (1200 + 9600)');
    L.close();
  } finally { S.stop(); }
}
{
  console.log('── B. a SHARED DIAL (users 4), two listeners ──');
  const S = await server(clean, 'shared', { users: 4 });
  try {
    const L = await listener(S, 'aprsshr01'), M = await listener(S, 'aprsshr02');
    L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L.dx.send(ATTACH);
    await judge(L.dx, 'shared dial, the listener who attached');
    M.dx.send(ATTACH);   // ★ a second window on the same run (a mirror): it joins, nothing restarts
    await sleep(500);
    await judge(M.dx, 'shared dial, the second listener (mirror)');
    L.close(); M.close();
  } finally { S.stop(); }
}
{
  console.log('── C. a listener\'s own VFO (users 4, range-locked) ──');
  const S = await server(clean, 'own', { users: 4, locked: true });
  try {
    const L = await listener(S, 'aprsown01');
    L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L.dx.send(ATTACH);
    await judge(L.dx, 'per-listener VFO');
    L.close();
  } finally { S.stop(); }
}
{
  console.log('── D. shared dial, a strong neighbour 60 kHz up (30 dB over the packets) ──');
  const S = await server(neighbour, 'nb', { users: 4 });
  try {
    const L = await listener(S, 'aprsnb01');
    L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L.dx.send(ATTACH);
    await judge(L.dx, 'shared dial + neighbour');
    L.close();
  } finally { S.stop(); }
}
{
  console.log('── E. the iOS app\'s silent mute (audio closed, spectrum paused), the DX scanner on (wait 3 s) ──');
  const S = await server(clean, 'mute', { users: 1, dxWait: 3 });
  try {
    const L = await listener(S, 'aprsmute01');
    L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L.dx.send(ATTACH);
    await sleep(1500);
    L.au.close(); L.sp.close();   // ★ VibePowerModule.setMuted(true) → disconnectForPause; SDRScreen → pauseSpectrum
    const t0 = L.dx.txt.length;
    await sleep(12000);           // ★ four times the scanner's wait
    const lg = S.logText();
    ok(!/DX scanner: nobody here for/.test(lg), 'the DX scanner never swept (the attached decoder is somebody)');
    ok(!/dongle capture paused \(idle\)/.test(lg), 'the capture never parked');
    const after = { txt: L.dx.txt.slice(t0) };
    const h = heard(after);
    ok(h.got.size >= 4, `packets kept coming with only the decoder socket open: ${h.got.size} in 12 s`);
    L.dx.close();
  } finally { S.stop(); }
}

{
  console.log('── F. the XCover sequence: SATELLITE (ISS APRS, then ISS SSTV), dropped, 144.800 NFM, APRS by hand (shared dial) ──');
  const S = await server(clean, 'satseq', { users: 4, sats: true });
  try {
    const L = await listener(S, 'aprssat01');
    const picked = () => jsons(L.sp, 'sat').filter((m) => m.state === 'picked').length;
    let n0 = picked();
    L.sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 2, doppler: true });   // ★ ISS APRS 145.825 (test-server-sat.mjs)
    ok(await until(() => picked() > n0, 5000), 'ISS APRS picked');
    await sleep(1500);
    n0 = picked();
    L.sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 1, doppler: true });   // ★ ISS voice / SSTV 145.800
    ok(await until(() => picked() > n0, 5000), 'ISS SSTV picked');
    await sleep(1500);
    L.sp.send({ type: 'sat', op: 'drop' });                                       // ★ out of SATELLITE: the lock ends
    await sleep(1000);
    L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(600);
    L.dx.send(ATTACH);
    await judge(L.dx, 'after the satellite picks and the drop');
    const lg = S.logText();
    ok(/\[sat\] decoder (aprs|sstv) attached for the shared dial/.test(lg), 'the picks attached their decoders (the sequence really ran)');
    L.close();
  } finally { S.stop(); }
}

/* ★★★ beta92 APRS FROM IQ (Stuart 11-23: "surely aprs needs to be iq based and bypass the nfm processing completely").
 *  An OVER-DEVIATED tracker (±6 kHz, flat) 1.5 kHz OFF 144.800 at CNR 10 dB. Before beta92 the decoder read the listener's
 *  NFM audio and their channel cut to ±6.25 kHz: test-aprs-onair's same row gives it next to nothing (its "before beta92"
 *  column); now its own tap, AFC and filters take it whatever the listener has set. */
const overdev = clip('overdev', 10, 0, 0, 6000, 1500, 0);
for (const [tag, users, locked, what] of [['odshared', 4, false, 'shared dial'], ['odown', 4, true, "a listener's own VFO"], ['odone', 1, false, 'one listener (users 1)']]) {
  console.log(`── G. over-deviated (±6 kHz), 1.5 kHz off, CNR 10 dB — ${what}, the listener on NFM 12.5 kHz ──`);
  const S = await server(overdev, tag, { users, locked });
  try {
    const L = await listener(S, `aprs${tag}01`);
    L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L.dx.send(ATTACH);
    await judge(L.dx, `${what}, over-deviated + off tune`);
    L.close();
  } finally { S.stop(); }
}
{
  console.log("── H. the same signal, the listener's passband cut to ±3 kHz (their choice must not reach the decoder) ──");
  const S = await server(overdev, 'odnarrow', { users: 4 });
  try {
    const L = await listener(S, 'aprsodn01');
    L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(400);
    L.sp.send({ type: 'bandwidth', bandwidthLow: -3000, bandwidthHigh: 3000 }); await sleep(400);
    L.dx.send(ATTACH);
    await judge(L.dx, 'passband ±3 kHz');
    L.close();
  } finally { S.stop(); }
}
{
  /* ★★ The XCover, 11-23 (relayed by the owner): after a satellite APRS pick, an SSTV pick and leaving SATELLITE, APRS by
   *  hand "decoded poorly" until a fresh AUTO attach. At the MARGIN (over-deviated ±5 kHz, 1 kHz off, CNR 9 dB) the
   *  satellite sequence then a hand attach must decode what a fresh AUTO attach does — nothing of the pick (passband,
   *  Doppler, satellite name, baud) carried into the decoder. Both hand attaches are tried: AUTO (the fixed clients), and
   *  the "1200" the old clients sent after a pick (it reads the same IQ receiver). */
  const marginal = clip('marginal', 9, 0, 0, 5000, 1000, 0);
  const counts = {};
  for (const [tag, seq, attach] of [['mfresh', false, ATTACH], ['msat', true, ATTACH], ['msat1200', true, { ...ATTACH, packet: '1200', fx25: 1, il2p: 1 }]]) {
    console.log(`── I. marginal (±5 kHz, 1 kHz off, 9 dB): ${seq ? 'the satellite sequence, then' : 'a fresh'} ${attach.packet.toUpperCase()} attach ──`);
    const S = await server(marginal, tag, { users: 4, sats: true });
    try {
      const L = await listener(S, `aprs${tag}01`);
      if (seq) {
        const picked = () => jsons(L.sp, 'sat').filter((m) => m.state === 'picked').length;
        let n0 = picked();
        L.sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 2, doppler: true });
        ok(await until(() => picked() > n0, 5000), 'ISS APRS picked');
        await sleep(1500);
        n0 = picked();
        L.sp.send({ type: 'sat', op: 'pick', norad: 25544, tx: 1, doppler: true });
        ok(await until(() => picked() > n0, 5000), 'ISS SSTV picked');
        await sleep(1500);
        L.sp.send({ type: 'sat', op: 'drop' });
        await sleep(1000);
      }
      L.sp.send({ type: 'tune', frequency: DIAL, mode: 'nfm' }); await sleep(600);
      L.dx.send(attach);
      // one whole pass of the clip (10 packets, ~22 s)
      await until(() => heard(L.dx).got.size >= N, 32000);
      const h = heard(L.dx);
      counts[tag] = h.got.size;
      ok(h.wrong === 0, `${h.got.size} of ${N} packets, no wrong frames`);
      L.close();
    } finally { S.stop(); }
  }
  ok(counts.mfresh >= N - 2, `a fresh AUTO attach at the margin: ${counts.mfresh} of ${N}`);
  ok(counts.msat >= counts.mfresh - 1, `after the satellite sequence, AUTO by hand: ${counts.msat} (fresh ${counts.mfresh})`);
  ok(counts.msat1200 >= counts.mfresh - 1, `after the satellite sequence, the old clients' "1200": ${counts.msat1200} (fresh ${counts.mfresh})`);
}

fs.rmSync(tmp, { recursive: true, force: true });
console.log(`\ntest-server-aprs-shared: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
