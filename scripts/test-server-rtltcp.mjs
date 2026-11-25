// test-server-rtltcp.mjs — THE RTL-TCP FRONT DOOR (LocalSdrShim::startRtlTcpDoor), through a real vibeserver.
//
// ★★★ THE FAULT (Sloper, 2026-11-25, V12 Android): Server mode → RTL-TCP → SDR# / SDR++ said "connected", the app said
//     "Good", no waterfall, no audio. The old server opened every radio through librtlsdr, which "opens" an Airspy or an
//     RSP and then sends nothing. Now the engine opens the radio with its own driver and the door serves RAW IQ OUT's
//     full-rate stream — so this drives the door exactly as an rtl_tcp client does and checks what comes out:
//   · the 12-byte header (RTL0, R820T, 29 gains) arrives only WITH IQ, never on its own;
//   · real IQ flows at the rate the client SETS — 1.024 MS/s, then 250 kS/s — within 2 %;
//   · SET_FREQUENCY is honoured: a carrier at a known frequency lands at the right offset in the stream (≥ 20 dB up);
//   · every command an rtl_tcp client sends (SDR#, SDR++, SDR Console: 0x01–0x0e) is taken without dropping the stream;
//   · gain AUTO is VibeAGC; a manual gain stops it and holds (vibeserver.json `rtltcp`);
//   · one client at a time; a second is turned away;
//   · NON-RTL: the same with the source given a fixed rate list (VIBE_TEST_TCP_RATES "2000000,3000000"), so a 1.024 MS/s
//     ask is served by the next rate up (2 MS/s) and RESAMPLED to exactly 1.024 MS/s;
//   · LAN ONLY: a client from a public address (VIBE_TEST_RTLTCP_PEER=8.8.8.8) is refused — no header, no bytes;
//     ★★ beta94 one from 169.254/16 link-local (169.254.10.20) is served (vibe_ipclass.h isPrivateV4);
//   · the server's CPU while streaming 2.4 MS/s (printed: the figure to weigh against the old librtlsdr pass-through).
// ★ SILENT: no audio anywhere — bytes are counted and one tone measured. Loopback only.
// usage: VIBESERVER_BIN=… node scripts/test-server-rtltcp.mjs   (~60 s)
import net from 'node:net';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) { console.log('   (not run — set VIBESERVER_BIN to a vibeserver built from this tree)'); process.exit(3); }
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

function cpuSecs(pid) {
  try {
    const t = execFileSync('ps', ['-o', 'time=', '-p', String(pid)], { encoding: 'utf8' }).trim();
    const p = t.split(/[:.-]/).map(Number);
    const [ss, cc] = [p[p.length - 2], p[p.length - 1]];
    const mm = p[p.length - 3] || 0, hh = p.length > 3 ? p[p.length - 4] : 0;
    return hh * 3600 + mm * 60 + ss + cc / 100;
  } catch { return NaN; }
}
/** Amplitude of the complex tone at `hz` in u8 IQ `b` (pairs from `from`, `n` of them, rate `fs`), Blackman-Harris. */
function toneAmp(b, from, n, fs, hz) {
  let sr = 0, si = 0, sw = 0;
  const w = -2 * Math.PI * hz / fs;
  let cr = 1, ci = 0; const rr = Math.cos(w), ri = Math.sin(w);
  for (let k = 0; k < n; k++) {
    const t = 2 * Math.PI * k / (n - 1);
    const win = 0.35875 - 0.48829 * Math.cos(t) + 0.14128 * Math.cos(2 * t) - 0.01168 * Math.cos(3 * t);
    const a = b[2 * (from + k)] - 127.5, q = b[2 * (from + k) + 1] - 127.5;
    sr += win * (a * cr - q * ci); si += win * (a * ci + q * cr); sw += win;
    const nr = cr * rr - ci * ri; ci = cr * ri + ci * rr; cr = nr;
    if ((k & 1023) === 1023) { const m = 1 / Math.hypot(cr, ci); cr *= m; ci *= m; }
  }
  return Math.hypot(sr, si) / sw;
}
const dB = (a) => 20 * Math.log10(Math.max(a, 1e-12));

/** A minimal rtl_tcp client: header (when it comes), byte count, a capture window, commands. */
function client(port) {
  const s = net.connect(port, '127.0.0.1');
  const st = { hdr: null, hdrAt: 0, firstIqAt: 0, bytes: 0, closed: false, cap: null, capWant: 0, t0: Date.now(), hdrAlone: false };
  let pend = Buffer.alloc(0);
  s.on('error', () => {});
  s.on('close', () => { st.closed = true; });
  s.on('data', (d) => {
    if (!st.hdr) {
      pend = Buffer.concat([pend, d]);
      if (pend.length < 12) return;
      st.hdr = pend.subarray(0, 12); st.hdrAt = Date.now();
      d = pend.subarray(12); pend = null;
      if (d.length === 0) st.hdrAlone = true;   // ★ a header with no IQ behind it in the same read — noted, judged below
    }
    if (d.length && !st.firstIqAt) st.firstIqAt = Date.now();
    st.bytes += d.length;
    if (st.cap && st.cap.length < st.capWant) st.cap = Buffer.concat([st.cap, d.subarray(0, st.capWant - st.cap.length)]);
  });
  st.cmd = (code, val) => { const b = Buffer.alloc(5); b[0] = code; b.writeUInt32BE(val >>> 0, 1); s.write(b); };
  st.capture = async (bytes) => { st.cap = Buffer.alloc(0); st.capWant = bytes; for (let i = 0; i < 100 && st.cap.length < bytes; i++) await sleep(50); const c = st.cap; st.cap = null; return c; };
  st.rate = async (ms) => { const a = st.bytes; await sleep(ms); return (st.bytes - a) / (ms / 1000) / 2; };
  st.end = () => s.destroy();
  return st;
}

const FREQ = 145500000, CARRIER = 145600000, RATE = 2400000;

async function server({ env = {}, label }) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-rtltcp-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(dir, d));
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({ configured: true, name: 'RTL-TCP door test', adminPass: 'rtltcp-door-admin',
    radios: [{ serial: 'rtltcpdoor', label: 'Test radio', enabled: true, configured: true, users: 1 }] }));
  const fakePort = await freePort(), port = await freePort(), door = await freePort();
  const fake = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(fakePort), '--rate', String(RATE),
                                        '--tones', '0', '--carrier', String(CARRIER), '--carrier-amp', '12'], { stdio: 'ignore' });
  await sleep(300);
  const logPath = path.join(dir, 'server.log');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${fakePort}`, '--port', String(port), '--rate', String(RATE), '--freq', String(FREQ),
                          '--mode', 'usb', '--users', '1', '--rtltcp-port', String(door)],
                    { env: { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                             VIBESERVER_RUNTIME_DIR: path.join(dir, 'run'), ...env },
                      stdio: ['ignore', fs.openSync(logPath, 'w'), fs.openSync(logPath, 'a')] });
  const status = async () => { try { return await (await fetch(`http://127.0.0.1:${port}/vibeserver.json`, { signal: AbortSignal.timeout(3000) })).json(); } catch { return null; } };
  for (let i = 0; i < 100 && !(await status()); i++) await sleep(100);
  for (let i = 0; i < 50 && !((await status())?.rtltcp); i++) await sleep(100);
  console.log(`\n── ${label}`);
  return { srv, fake, door, status, L: () => { try { return fs.readFileSync(logPath, 'utf8'); } catch { return ''; } },
           stop: () => { try { srv.kill('SIGTERM'); } catch {} try { fake.kill(); } catch {} } };
}

async function toneCheck(c, fs_, offHz, what) {
  const n = 65536;
  await sleep(1500);   // ★ the retune and the stream's own settling
  const b = await c.capture(2 * n);
  if (b.length < 2 * n) { ok(false, `${what}: captured ${b.length / 2} of ${n} pairs`); return; }
  const sig = dB(toneAmp(b, 0, n, fs_, offHz));
  const flr = Math.max(dB(toneAmp(b, 0, n, fs_, offHz + fs_ * 0.11)), dB(toneAmp(b, 0, n, fs_, offHz - fs_ * 0.13)));
  ok(sig - flr >= 20, `${what}: carrier at ${(offHz / 1e3).toFixed(0)} kHz in the stream, ${(sig - flr).toFixed(1)} dB over the floor beside it`);
}
const within = (got, want, pct) => Math.abs(got - want) <= want * pct / 100;

// ── 1. An RTL (the fake rtl_tcp source): header, rates, frequency, commands, gain, one client ───────────────────
{
  const S = await server({ label: 'RTL (rtl_tcp source)' });
  try {
    const st0 = await S.status();
    ok(st0?.rtltcp?.running === true && st0.rtltcp.port === S.door, `the door is listening (vibeserver.json rtltcp: ${JSON.stringify(st0?.rtltcp ?? null).slice(0, 120)}…)`);
    const c = client(S.door);
    for (let i = 0; i < 60 && !c.hdr; i++) await sleep(100);
    ok(!!c.hdr && c.hdr.subarray(0, 4).toString() === 'RTL0', 'header: RTL0');
    ok(!!c.hdr && c.hdr.readUInt32BE(4) === 5 && c.hdr.readUInt32BE(8) === 29, `header: R820T (5) with 29 gains (got ${c.hdr?.readUInt32BE(4)}/${c.hdr?.readUInt32BE(8)})`);
    await sleep(300);
    ok(c.firstIqAt > 0 && c.firstIqAt - c.hdrAt < 300, `IQ follows the header at once (${c.firstIqAt ? c.firstIqAt - c.hdrAt : '∞'} ms) — the header is sent with the first block`);
    // What SDR++ / SDR# send first: rate, frequency, gain mode, AGC
    c.cmd(0x02, 1024000); c.cmd(0x01, FREQ); c.cmd(0x03, 0); c.cmd(0x08, 0);
    await sleep(2000);
    const r1 = await c.rate(2000);
    ok(within(r1, 1024000, 2), `SET_SAMPLE_RATE 1.024 MS/s honoured: ${(r1 / 1e6).toFixed(4)} MS/s`);
    await toneCheck(c, 1024000, CARRIER - FREQ, 'SET_FREQUENCY 145.500 MHz');
    c.cmd(0x01, CARRIER + 50000);
    await toneCheck(c, 1024000, -50000, 'SET_FREQUENCY 145.650 MHz (moved)');
    c.cmd(0x02, 250000);
    await sleep(1500);
    const r2 = await c.rate(2000);
    ok(within(r2, 250000, 2), `SET_SAMPLE_RATE 250 kS/s honoured: ${(r2 / 1e3).toFixed(1)} kS/s`);
    await toneCheck(c, 250000, -50000, 'at 250 kS/s');
    // gain: AUTO = VibeAGC, then manual holds
    let s = (await S.status())?.rtltcp;
    ok(s?.agc === true && s?.agcName === 'VibeAGC', `gain mode AUTO runs VibeAGC (agc ${s?.agc}, ${s?.agcName})`);
    c.cmd(0x03, 1); c.cmd(0x04, 297);
    await sleep(1500);
    s = (await S.status())?.rtltcp;
    ok(s?.agc === false && Math.abs((s?.gainTenthDb ?? -9) - 297) <= 10, `a manual gain stops VibeAGC and holds (agc ${s?.agc}, ${((s?.gainTenthDb ?? 0) / 10).toFixed(1)} dB)`);
    // every command the common clients send — the stream must survive them all
    const before = c.bytes;
    for (const [code, v] of [[0x05, 0], [0x06, 0], [0x08, 1], [0x08, 0], [0x09, 0], [0x0a, 0], [0x0d, 8], [0x0e, 0], [0x0f, 0]]) c.cmd(code, v);
    await sleep(1500);
    ok(!c.closed && c.bytes > before + 100000, `the stream survives 0x05–0x0f (ppm, IF gain, AGC, direct sampling, offset tuning, gain index, bias-T, unknown)`);
    s = (await S.status())?.rtltcp;
    ok(s?.agc === false && Math.abs((s?.gainTenthDb ?? -9) - 144) <= 10, `0x0d gain index 8 = 14.4 dB (${((s?.gainTenthDb ?? 0) / 10).toFixed(1)} dB)`);
    // one client at a time
    const c2 = client(S.door);
    await sleep(1200);
    ok(c2.closed && !c2.hdr, 'a second client is turned away (one at a time)');
    // CPU at 2.4 MS/s straight through
    c.cmd(0x02, 2400000); c.cmd(0x03, 0);
    await sleep(2500);
    const t0 = cpuSecs(S.srv.pid), w0 = Date.now(), b0 = c.bytes;
    await sleep(8000);
    const cpu = (cpuSecs(S.srv.pid) - t0) / ((Date.now() - w0) / 1000) * 100;
    const rr = (c.bytes - b0) / ((Date.now() - w0) / 1000) / 2;
    ok(within(rr, 2400000, 2), `2.4 MS/s straight through: ${(rr / 1e6).toFixed(3)} MS/s`);
    console.log(`   (server CPU streaming 2.4 MS/s through the door: ${cpu.toFixed(1)} % of one core on this machine)`);
    c.end();
    await sleep(800);
    s = (await S.status())?.rtltcp;
    ok(s?.client === false, 'the client leaving is noticed');
  } finally { S.stop(); }
}

// ── 2. A NON-RTL radio's shape: a fixed rate list — next rate up, resampled to exactly what was asked ──────────
{
  const S = await server({ label: 'radio with a fixed rate list (VIBE_TEST_TCP_RATES 2000000,3000000)', env: { VIBE_TEST_TCP_RATES: '2000000,3000000' } });
  try {
    const c = client(S.door);
    for (let i = 0; i < 60 && !c.hdr; i++) await sleep(100);
    ok(!!c.hdr && c.hdr.subarray(0, 4).toString() === 'RTL0', 'header: RTL0');
    c.cmd(0x02, 1024000); c.cmd(0x01, FREQ);
    await sleep(2500);
    const st = (await S.status())?.rtltcp;
    ok(st && Math.abs(st.sampleRate - 2000000) < 2, `the radio runs at the next rate up from its list (${st?.sampleRate})`);
    const r1 = await c.rate(2000);
    ok(within(r1, 1024000, 2), `…and the stream is resampled to exactly 1.024 MS/s: ${(r1 / 1e6).toFixed(4)} MS/s`);
    await toneCheck(c, 1024000, CARRIER - FREQ, 'resampled stream, SET_FREQUENCY 145.500 MHz');
    c.end();
  } finally { S.stop(); }
}

// ── 3. LOCAL NETWORK ONLY: a client from a public address is refused ────────────────────────────────────────────
{
  const S = await server({ label: 'a client from a public address (VIBE_TEST_RTLTCP_PEER=8.8.8.8)', env: { VIBE_TEST_RTLTCP_PEER: '8.8.8.8' } });
  try {
    const c = client(S.door);
    await sleep(1500);
    ok(c.closed && !c.hdr && c.bytes === 0, 'refused: no header, no IQ, connection closed');
    const st = (await S.status())?.rtltcp;
    ok((st?.refused ?? 0) >= 1 && /local network/.test(st?.lastRefusal ?? ''), `…and said why (${st?.lastRefusal})`);
    ok(/RTL-TCP: refused 8\.8\.8\.8/.test(S.L()), 'the refusal is in the log');
  } finally { S.stop(); }
}

// ── 4. ★★ beta94 (Stuart 11-25) 169.254/16 LINK-LOCAL IS THE LOCAL NETWORK — a laptop cabled straight to the box, no DHCP ──
{
  const S = await server({ label: 'a client from a link-local address (VIBE_TEST_RTLTCP_PEER=169.254.10.20)', env: { VIBE_TEST_RTLTCP_PEER: '169.254.10.20' } });
  try {
    const c = client(S.door);
    await sleep(2500);
    ok(!!c.hdr && c.bytes > 100000, `served: the header and IQ (${c.bytes} bytes)`);
    const st = (await S.status())?.rtltcp;
    ok((st?.refused ?? 0) === 0 && !/RTL-TCP: refused 169\.254/.test(S.L()), 'never refused');
    c.end();
  } finally { S.stop(); }
}

console.log(`\n${fail ? 'FAILED' : 'PASS'}: ${pass} ok, ${fail} failed`);
process.exit(fail ? 1 : 0);
