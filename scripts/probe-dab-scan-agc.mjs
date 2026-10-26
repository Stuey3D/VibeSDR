#!/usr/bin/env node
// probe-dab-scan-agc.mjs — MEASURE the DAB band scan against the RTL front-end model (fake-rtl-tcp --dab-mux): how long
// the gain takes to let each ensemble be found, per level, and whether it is found at all. A measuring tool, not a test:
// prints one line per ensemble block and the scan's total. Silent, loopback only.
//
// usage: VIBESERVER_BIN=… node scripts/probe-dab-scan-agc.mjs --ens 222064000:8[,…] [--adj 218640000:45[,…]]
//        [--start 12B] [--direct 11D] [--gainmem 11D:3,…] [--noise-max 6] [--json]
//   --direct BLK: instead of a scan, tune straight from --start to BLK and time lock / first FIB / list (a cold start).
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = process.env.VIBESERVER_BIN;
const ROOT = path.resolve(new URL('..', import.meta.url).pathname);
const MUX = process.env.DAB_MUX || path.join(ROOT, 'vibeserver/bench-clip/mux.cf32');   // ★ make-clip.sh's clean multiplex (gitignored)
if (!BIN || !fs.existsSync(BIN) || !fs.existsSync(MUX)) { console.error('need VIBESERVER_BIN and the bench mux.cf32 — not run'); process.exit(3); }
const opt = (k, d = '') => { const i = process.argv.indexOf(`--${k}`); return i >= 0 ? process.argv[i + 1] : d; };
const BLOCKS = ['5A','5B','5C','5D','6A','6B','6C','6D','7A','7B','7C','7D','8A','8B','8C','8D','9A','9B','9C','9D',
  '10A','10N','10B','10C','10D','11A','11N','11B','11C','11D','12A','12N','12B','12C','12D','13A','13B','13C','13D','13E','13F'];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const freePort = () => new Promise((res) => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

export async function runProbe({ ens, adj = '', start = '12B', direct = '', gainmem = '', noiseMax = 6, quiet = false, env = {} }) {
  const T = fs.mkdtempSync(path.join(os.tmpdir(), 'vs-dabagc-'));
  for (const d of ['data', 'run']) fs.mkdirSync(path.join(T, d));
  if (gainmem) {   // ★ a remembered gain per block, as dab-gains.txt holds it ("BLOCK steps")
    fs.writeFileSync(path.join(T, 'data', 'dab-gains.txt'), gainmem.split(',').map((p) => p.replace(':', ' ')).join('\n') + '\n');
  }
  fs.writeFileSync(path.join(T, 'config.json'), JSON.stringify({ configured: true, name: 'agc probe', locator: 'IO92nh', adminPass: 'agc-probe-pass',
    radios: [{ serial: 'agcp', label: 'T', enabled: true, configured: true, users: 1 }] }));
  const rp = await freePort(), port = await freePort();
  const rtlLog = [];
  const t0 = Date.now();
  const rtl = spawn(process.execPath, [path.join(ROOT, 'vibeserver/fake-rtl-tcp.mjs'), '--port', String(rp), '--rate', '2048000', '--dab-mux', process.env.DAB_MUX || MUX,
    '--dab-ens', ens, ...(adj ? ['--dab-adj', adj] : []), '--noise-max', String(noiseMax),
    ...(process.env.DAB_MUX_FROM ? ['--dab-mux-from', process.env.DAB_MUX_FROM] : [])], { stdio: ['ignore', 'pipe', 'ignore'] });
  rtl.stdout.on('data', (b) => { for (const l of b.toString().split('\n')) if (l) rtlLog.push([Date.now() - t0, l]); });
  await sleep(1500);
  const log = path.join(T, 'server.log');
  const lf = fs.openSync(log, 'a');
  const srv = spawn(BIN, ['--tcp', `127.0.0.1:${rp}`, '--port', String(port), '--rate', '2048000', '--freq', '96100000', '--mode', 'wfm', '--users', '1'],
    { env: { ...process.env, ...env, VIBESERVER_CONFIG: path.join(T, 'config.json'), VIBESERVER_DATA_DIR: path.join(T, 'data'), VIBESERVER_RUNTIME_DIR: path.join(T, 'run'), HOME: T },
      stdio: ['ignore', lf, lf] });
  for (let i = 0; i < 80; i++) { try { await fetch(`http://127.0.0.1:${port}/vibeserver.json`); break; } catch { await sleep(250); } }
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws/user-spectrum?user_session_id=agc${Date.now()}&bins=256&proto=1`);
  let dab = null, scan = null;
  ws.onmessage = (e) => { if (typeof e.data !== 'string') return; try { const m = JSON.parse(e.data); if (m.type === 'dab' && m.channel) dab = m; if (m.type === 'dab_scan') scan = m; } catch {} };
  await new Promise((r) => (ws.onopen = r));
  const hb = setInterval(() => ws.readyState === 1 && ws.send('{"type":"dab_resync"}'), 8000);
  ws.send(JSON.stringify({ type: 'dab', on: 1, channel: BLOCKS.indexOf(start) }));
  await sleep(10000);   // ★ the AGC settles on the starting block, as a listener's radio would be before they press SCAN
  const gainAt = (ms) => { let g = null; for (const [t, l] of rtlLog) { if (t > ms) break; const m = /gain ([\d.]+) dB/.exec(l); if (m) g = Number(m[1]); } return g; };
  const out = { startGain: gainAt(Date.now() - t0) };
  if (direct) {
    const tTune = Date.now() - t0;
    ws.send(JSON.stringify({ type: 'dab', on: 1, channel: BLOCKS.indexOf(direct) }));
    let lockAt = -1, fibAt = -1, listAt = -1;
    const end = Date.now() + 30000;
    while (Date.now() < end && listAt < 0) {
      await sleep(100);
      const t = (Date.now() - t0 - tTune) / 1000;
      if (dab && dab.channel === direct) {
        if (dab.locked && lockAt < 0) lockAt = t;
        if (dab.fibRate > 0 && fibAt < 0) fibAt = t;
        if (Array.isArray(dab.services) && dab.services.length >= 12 && dab.services.every((s) => s.label) && listAt < 0) listAt = t;
      }
    }
    Object.assign(out, { lockAt, fibAt, listAt, gainAfter: gainAt(Date.now() - t0) });
  } else {
    const tScan = Date.now() - t0;
    ws.send(JSON.stringify({ type: 'dab_scan', on: 1 }));
    const end = Date.now() + 600000;
    while (Date.now() < end && !(scan && scan.finished)) await sleep(250);
    const lines = fs.readFileSync(log, 'utf8').split('\n').filter((l) => /band scan: \S+ (".*|nothing) in [\d.]+ s \(lock/.test(l));
    out.blocks = lines.map((l) => {
      const m = /band scan: (\S+) (.*?) in ([\d.]+) s \(lock ([-\d.]+), FIB ([-\d.]+), list ([-\d.]+); -1 = never; null depth ([-\d.]+) dB/.exec(l);
      return m && { blk: m[1], what: m[2], secs: +m[3], lock: +m[4], fib: +m[5], list: +m[6], null1: +m[7] };
    }).filter(Boolean);
    out.found = (scan?.found || []).map((h) => `${h.block}:${h.services.length}`);
    out.total = scan?.secs;
    out.scanMs = Date.now() - t0 - tScan;
    await sleep(3000);
    out.gainAfter = gainAt(Date.now() - t0);
  }
  out.agcLines = fs.readFileSync(log, 'utf8').split('\n').filter((l) => /\[DAB\] (entering|band scan: (started|finished|CANCELLED))|scan gain/.test(l)).slice(-12);
  clearInterval(hb); ws.close(); srv.kill(); rtl.kill();
  await sleep(600);
  if (!quiet) out.logDir = T; else fs.rmSync(T, { recursive: true, force: true });
  return out;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const r = await runProbe({ ens: opt('ens'), adj: opt('adj'), start: opt('start', '12B'), direct: opt('direct'), gainmem: opt('gainmem'), noiseMax: Number(opt('noise-max', '6')) });
  if (process.argv.includes('--json')) console.log(JSON.stringify(r));
  else {
    console.log(`start gain ${r.startGain} dB → after ${r.gainAfter} dB`);
    if (r.blocks) {
      for (const b of r.blocks) if (b.what !== 'nothing' || b.lock >= 0 || b.null1 > 4) console.log(`  ${b.blk.padEnd(4)} ${b.what.padEnd(36)} ${b.secs.toFixed(1)} s lock ${b.lock} FIB ${b.fib} list ${b.list} null@1s ${b.null1}`);
      const empty = r.blocks.filter((b) => b.what === 'nothing');
      console.log(`  empty blocks ${empty.length}, mean ${(empty.reduce((a, b) => a + b.secs, 0) / Math.max(1, empty.length)).toFixed(2)} s, max ${Math.max(...empty.map((b) => b.secs)).toFixed(1)} s`);
      console.log(`found ${r.found.join(' ') || 'NOTHING'} — scan ${r.total} s`);
    } else console.log(`direct: lock ${r.lockAt} s, FIB ${r.fibAt} s, list ${r.listAt} s`);
    console.log(r.agcLines.join('\n'));
    console.log(`(log ${r.logDir})`);
  }
}
