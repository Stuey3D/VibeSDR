// test-rate-guard.mjs — A RADIO THAT FALLS OVER AT A RATE A MODE BOOSTED IT TO (vibe_rate_guard.h, 2026-10-24).
//
// The real server against a fake rtl_tcp radio (vibeserver/fake-rtl-tcp.mjs), base rate 1.024 MS/s, the owner's
// "change the sample rate when a mode needs it" switch ON:
//   1. STALL (in-process): the fake stops sending at 2.4 MS/s (--stall-at). A listener picks ADS-B, ADS-B boosts the
//      radio to 2.4, the samples stop — within ~3 s the watchdog must leave ADS-B, put the radio back at 1.024, record
//      "stopped responding at 2.4 MS/s during ADS-B", switch the rate switch off, and IQ must flow again. The marker
//      written before the boost must be gone (a clean recovery is not a crash).
//   2. CRASH (whole process): a marker planted beside the config, as a run that died at a boost leaves it. The next
//      start must record "SDR crashed at 2.4 MS/s during ADS-B", remove the marker, persist dabRateBoost off, and
//      refuse to boost; Clear (POST ?clear=1) forgets it.
// ★ SILENT: nothing is played. ~30 s.
//   usage: VIBESERVER_BIN=… node scripts/test-rate-guard.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const BIN = process.env.VIBESERVER_BIN || '';
if (!BIN || !fs.existsSync(BIN)) {
  console.log('   not run — set VIBESERVER_BIN to a vibeserver built from this tree');
  process.exit(3);
}
let pass = 0, fail = 0;
const ok = (c, what) => { c ? pass++ : fail++; console.log(`   ${c ? 'ok  ' : 'FAIL'} ${what}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
const getJson = async (port, p, opts) => { try { return await (await fetch(`http://127.0.0.1:${port}${p}`, opts)).json(); } catch { return null; } };

/** A WebSocket that sends JSON and counts the binary frames it is sent (spectrum = IQ flowing). */
function ws(port, pathq) {
  const key = crypto.randomBytes(16).toString('base64');
  const s = net.connect(port, '127.0.0.1');
  const st = { frames: 0, texts: [] };
  let buf = Buffer.alloc(0), upgraded = false;
  s.on('connect', () => s.write(`GET ${pathq} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`));
  s.on('error', () => {});
  s.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    if (!upgraded) { const e = buf.indexOf('\r\n\r\n'); if (e < 0) return; buf = buf.subarray(e + 4); upgraded = true; }
    for (;;) {
      if (buf.length < 2) return;
      let len = buf[1] & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      if (buf.length < off + len) return;
      const op = buf[0] & 0x0f, body = buf.subarray(off, off + len);
      if (op === 2) st.frames++; else if (op === 1) st.texts.push(body.toString());
      else if (op === 9) {   // ★ a ping: pong it back (masked, zero key), or the server counts us gone
        const h = Buffer.from([0x8a, 0x80 | body.length, 0, 0, 0, 0]);
        try { s.write(Buffer.concat([h, body])); } catch {}
      }
      buf = buf.subarray(off + len);
    }
  });
  return {
    st,
    send(o) {
      const b = Buffer.from(JSON.stringify(o));
      const h = Buffer.alloc(b.length < 126 ? 6 : 8);
      h[0] = 0x81;
      if (b.length < 126) { h[1] = 0x80 | b.length; h.writeUInt32BE(0, 2); } else { h[1] = 0x80 | 126; h.writeUInt16BE(b.length, 2); h.writeUInt32BE(0, 4); }
      try { s.write(Buffer.concat([h, b])); } catch {}
    },
    close() { try { s.destroy(); } catch {} },
  };
}

async function start(dir, extraRtl = []) {
  const rtlPort = await freePort(), port = await freePort(), front = await freePort();
  if (!fs.existsSync(path.join(dir, 'config.json')))
    fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({
      configured: true, fullMode: true, sharing: 'public', port: front,
      radios: [{ serial: 'fake', driver: 'rtlsdr', enabled: true, configured: true, freq: 100000000, rate: 1024000,
                 demodMode: 'wfm', landingFreq: 100000000, users: 10, dabRateBoost: true }],
    }));
  const rtl = spawn(process.execPath, [path.join(root, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rtlPort),
                                        '--rate', '1024000', ...extraRtl], { stdio: 'ignore' });
  await sleep(300);
  const logPath = path.join(dir, `server-${Date.now()}.log`);
  const logFd = fs.openSync(logPath, 'w');
  const env = { ...process.env, VIBESERVER_CONFIG: path.join(dir, 'config.json'), VIBESERVER_DATA_DIR: path.join(dir, 'data'),
                VIBESERVER_RUNTIME_DIR: path.join(dir, 'run') };
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rtlPort}`, '--port', String(port), '--rate', '1024000', '--idle-grace', '0'],
                    { env, stdio: ['ignore', logFd, logFd] });
  for (let i = 0; i < 100; i++) {
    const up = await new Promise((r) => { const c = net.connect(port, '127.0.0.1'); c.on('connect', () => { c.end(); r(true); }); c.on('error', () => r(false)); });
    if (up) break;
    await sleep(100);
  }
  await sleep(500);
  return {
    port, log: () => fs.readFileSync(logPath, 'utf8'),
    async stop() { srv.kill('SIGTERM'); for (let i = 0; i < 100 && srv.exitCode === null && srv.signalCode === null; i++) await sleep(50);
                   if (srv.exitCode === null && srv.signalCode === null) srv.kill('SIGKILL'); rtl.kill('SIGTERM'); await sleep(200); },
  };
}

const mkdir = () => { const d = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-rateguard-')); for (const x of ['data', 'run']) fs.mkdirSync(path.join(d, x)); return d; };

// ── 1. a stall at the boosted rate ──
console.log('── STALL: the radio stops delivering at the rate ADS-B boosted it to ──');
{
  const dir = mkdir();
  const s = await start(dir, ['--stall-at', '2000000']);
  const SID = crypto.randomUUID();
  const audio = ws(s.port, `/ws/audio?user_session_id=${SID}&codec=opus`);
  await sleep(600);
  const spec = ws(s.port, `/ws/user-spectrum?user_session_id=${SID}&mode=binary8&bins=256`);
  await sleep(1500);
  ok(spec.st.frames > 0, `spectrum flows at the base rate (${spec.st.frames} frames)`);
  spec.send({ type: 'adsb', on: 1 });
  await sleep(1500);
  ok(fs.existsSync(path.join(dir, 'rate-guard-fake.marker')), 'the marker is down while ADS-B has the radio at 2.4 MS/s');
  let hw = null;
  for (let i = 0; i < 40; i++) { await sleep(500); hw = await getJson(s.port, '/vibeserver/hardware'); if (hw?.rateGuard?.fault) break; }
  ok(hw?.rateGuard?.fault === 'stalled' && hw.rateGuard.rate === 2400000 && hw.rateGuard.mode === 'ADS-B',
     `recorded: ${JSON.stringify(hw?.rateGuard)}`);
  ok(/stopped responding at 2\.4 MS\/s during ADS-B/.test(hw?.rateGuard?.say || ''), 'the sentence names the rate and the mode');
  await sleep(2500);
  const before = spec.st.frames;
  await sleep(2000);
  ok(spec.st.frames > before, `IQ flows again after the recovery (${spec.st.frames - before} frames in 2 s)`);
  ok(spec.st.texts.some((t) => t.includes('"adsb_off"')), 'listeners were told ADS-B ended');
  ok(!fs.existsSync(path.join(dir, 'rate-guard-fake.marker')), 'a recovered stall is not a crash: the marker is gone');
  const log = s.log();
  if (process.env.SHOW_LOG) console.log(log.split('\n').filter((l) => /ADS-B|rate|IQ|watchdog|stall|tcp|silent/i.test(l)).slice(-40).join('\n'));
  ok(/\[rate\] .* at 2\.400 MS\/s during ADS-B/.test(log), 'the log says what happened');
  ok(/sample rate[^\n]*1024000|rate 2\.400 -> 1\.024|1\.024/.test(log), 'back at the base rate');
  const cfg = JSON.parse(fs.readFileSync(path.join(dir, 'config.json'), 'utf8'));
  ok(cfg.radios?.[0]?.dabRateBoost === false, `the rate switch was persisted OFF (${cfg.radios?.[0]?.dabRateBoost})`);
  // asking for ADS-B again must not boost
  spec.send({ type: 'adsb', on: 1 });
  await sleep(1500);
  ok(!fs.existsSync(path.join(dir, 'rate-guard-fake.marker')), 'ADS-B asked again: no boost while the fault stands');
  audio.close(); spec.close();
  await s.stop();
  fs.rmSync(dir, { recursive: true, force: true });
}

// ── 2. a whole-process crash at the boosted rate ──
console.log('── CRASH: a marker left by a run that died at 2.4 MS/s during ADS-B ──');
{
  const dir = mkdir();
  fs.writeFileSync(path.join(dir, 'rate-guard-fake.marker'), 'entering 2400000 Hz for ADS-B at 1761300200\n');
  const s = await start(dir);
  const hw = await getJson(s.port, '/vibeserver/hardware');
  ok(hw?.rateGuard?.fault === 'crashed' && hw.rateGuard.rate === 2400000 && hw.rateGuard.mode === 'ADS-B' && hw.rateGuard.at === 1761300200,
     `recorded at the marker's time: ${JSON.stringify(hw?.rateGuard)}`);
  ok(/^SDR crashed at 2\.4 MS\/s during ADS-B/.test(hw?.rateGuard?.say || ''), 'the red sentence');
  ok(!fs.existsSync(path.join(dir, 'rate-guard-fake.marker')), 'the marker is removed');
  ok(/died while the radio was at 2\.4 MS\/s during ADS-B/.test(s.log()), 'the log says so');
  const cfg = JSON.parse(fs.readFileSync(path.join(dir, 'config.json'), 'utf8'));
  ok(cfg.radios?.[0]?.dabRateBoost === false, `the rate switch persisted OFF (${cfg.radios?.[0]?.dabRateBoost})`);
  if (process.env.SHOW_LOG) console.log(s.log().split('\n').filter((l) => /setting|save|rate switch|died/i.test(l)).join('\n'), JSON.stringify(cfg));
  const cleared = await getJson(s.port, '/vibeserver/rate-guard?clear=1', { method: 'POST' });
  ok(cleared && cleared.fault === '' && cleared.blocked === false, `Clear forgets it: ${JSON.stringify(cleared)}`);
  await s.stop();
  ok(!fs.existsSync(path.join(dir, 'rate-guard-fake.marker')), 'a clean stop leaves no marker');
  const s2 = await start(dir);
  const hw2 = await getJson(s2.port, '/vibeserver/hardware');
  ok(hw2?.rateGuard?.fault === '', 'the next start finds nothing — a clean stop is not a crash');
  await s2.stop();
  fs.rmSync(dir, { recursive: true, force: true });
}

console.log(`\n${pass}/${pass + fail} checks passed`);
process.exit(fail ? 1 : 0);
